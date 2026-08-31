#include "dlssnr_d3d12_bridge.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

#include "nvsdk_ngx.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr unsigned long long kDlssNrApplicationId = 0x0876232Cull;
constexpr char kProjectId[] = "144e1375-1890-4de7-9258-9f14262d79a7";
constexpr char kEngineVersion[] = "Anvil-ACOdyssey-Steam-17083392";
constexpr wchar_t kRuntimeName[] = L"nvngx_dlssnr.dll";
constexpr wchar_t kGuideShaderName[] = L"ACOdysseyDLSSNR.guide_unpack.cso";
constexpr size_t kCommandSlotCount = 4;
constexpr size_t kMaxRetiredFeatures = 8;

using DlssNrInitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long,
    const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using DlssNrCreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
    NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using DlssNrEvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using DlssNrReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using DlssNrShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

void** g_getModuleFileNameSlot{};
GetModuleFileNameWFn g_originalGetModuleFileNameW{};
HMODULE g_authorizedCaller{};

DWORD WINAPI DlssNrGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size)
{
    if (module == g_authorizedCaller)
    {
        constexpr wchar_t kCallerName[] = L"nvngx.dll";
        constexpr DWORD kLength = static_cast<DWORD>(std::size(kCallerName) - 1);
        if (!filename || size == 0)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (size <= kLength)
        {
            std::memcpy(filename, kCallerName, size * sizeof(wchar_t));
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, kCallerName, sizeof(kCallerName));
        return kLength;
    }
    if (g_originalGetModuleFileNameW)
        return g_originalGetModuleFileNameW(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

template <typename Function>
bool ResolveExport(HMODULE module, const char* name, Function& function)
{
    const FARPROC address = GetProcAddress(module, name);
    if (!address) return false;
    static_assert(sizeof(address) == sizeof(function));
    std::memcpy(&function, &address, sizeof(address));
    return true;
}

void** FindImportedFunctionSlot(HMODULE module, const char* importedFunction)
{
    if (!module || !importedFunction) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    const auto* end = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress + directory.Size);
    for (; descriptor < end && descriptor->Name; ++descriptor)
    {
        const char* library = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(library, "KERNEL32.dll") != 0 &&
            _stricmp(library, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0)
            continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), importedFunction) == 0)
                return reinterpret_cast<void**>(&addresses->u1.Function);
        }
    }
    return nullptr;
}

void* FunctionAddress(GetModuleFileNameWFn function)
{
    void* address{};
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&address, &function, sizeof(address));
    return address;
}

bool InstallCallerCompatibility(HMODULE runtime, std::string& error)
{
    if (g_getModuleFileNameSlot && g_originalGetModuleFileNameW) return true;
    g_getModuleFileNameSlot = FindImportedFunctionSlot(runtime, "GetModuleFileNameW");
    if (!g_getModuleFileNameSlot)
    {
        error = "DLSSNR caller compatibility: GetModuleFileNameW IAT slot missing";
        return false;
    }
    auto hook = &DlssNrGetModuleFileNameW;
    void* hookAddress{};
    static_assert(sizeof(hook) == sizeof(hookAddress));
    std::memcpy(&hookAddress, &hook, sizeof(hookAddress));
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(hookAddress), &g_authorizedCaller))
    {
        error = "DLSSNR caller compatibility: bridge module lookup failed";
        return false;
    }
    DWORD oldProtection{};
    if (!VirtualProtect(g_getModuleFileNameSlot, sizeof(void*), PAGE_READWRITE, &oldProtection))
    {
        error = "DLSSNR caller compatibility: IAT slot is not writable";
        return false;
    }
    g_originalGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFn>(
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(g_getModuleFileNameSlot),
            hookAddress));
    DWORD ignored{};
    VirtualProtect(g_getModuleFileNameSlot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_getModuleFileNameSlot, sizeof(void*));
    if (!g_originalGetModuleFileNameW)
    {
        error = "DLSSNR caller compatibility: original import was null";
        return false;
    }
    return true;
}

void RestoreCallerCompatibility()
{
    if (g_getModuleFileNameSlot && g_originalGetModuleFileNameW)
    {
        DWORD oldProtection{};
        if (VirtualProtect(g_getModuleFileNameSlot, sizeof(void*), PAGE_READWRITE, &oldProtection))
        {
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(g_getModuleFileNameSlot),
                FunctionAddress(g_originalGetModuleFileNameW));
            DWORD ignored{};
            VirtualProtect(g_getModuleFileNameSlot, sizeof(void*), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), g_getModuleFileNameSlot, sizeof(void*));
        }
    }
    g_getModuleFileNameSlot = nullptr;
    g_originalGetModuleFileNameW = nullptr;
    g_authorizedCaller = nullptr;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(file);
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read{};
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
        read == bytes.size();
    CloseHandle(file);
    if (!ok) bytes.clear();
    return bytes;
}

std::string ResultText(const char* stage, NVSDK_NGX_Result result)
{
    char text[256]{};
    std::snprintf(text, sizeof(text), "%s failed: NGX result 0x%08x", stage,
        static_cast<unsigned int>(result));
    return text;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!list || !resource || before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

NVSDK_NGX_Result NVSDK_CONV ScalingRatioCallback(NVSDK_NGX_Parameter* parameters)
{
    if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    parameters->Set("DLSSNR.ScalingRatio", 1.0f);
    return NVSDK_NGX_Result_Success;
}

void SetOneToOne(NVSDK_NGX_Parameter* parameters, uint32_t width, uint32_t height)
{
    parameters->Set("DLSSNR.Width", width);
    parameters->Set("DLSSNR.Height", height);
    parameters->Set("DLSSNR.InputWidth", width);
    parameters->Set("DLSSNR.InputHeight", height);
    parameters->Set("DLSSNR.OutputWidth", width);
    parameters->Set("DLSSNR.OutputHeight", height);
    parameters->Set("DLSSNR.Output.Width", width);
    parameters->Set("DLSSNR.Output.Height", height);
    parameters->Set("DLSSNR.Upscaling", 0u);
    parameters->Set("DLSSNR.Scale", 1.0f);
    parameters->Set("DLSSNR.ScalingRatio", 1.0f);
    auto callback = &ScalingRatioCallback;
    void* callbackAddress{};
    static_assert(sizeof(callback) == sizeof(callbackAddress));
    std::memcpy(&callbackAddress, &callback, sizeof(callbackAddress));
    parameters->Set("DLSSNRComputeScalingRatioCallback", callbackAddress);
}

void SetSubrect(NVSDK_NGX_Parameter* parameters, const char* prefix,
    uint32_t width, uint32_t height)
{
    const std::string base(prefix);
    parameters->Set((base + "SubrectBaseX").c_str(), 0u);
    parameters->Set((base + "SubrectBaseY").c_str(), 0u);
    parameters->Set((base + "SubrectWidth").c_str(), width);
    parameters->Set((base + "SubrectHeight").c_str(), height);
}
}

struct DlssNrD3D12Bridge::Impl
{
    struct SharedTexture
    {
        ComPtr<ID3D11Texture2D> texture11;
        ComPtr<ID3D11ShaderResourceView> srv11;
        ComPtr<ID3D11UnorderedAccessView> uav11;
        ComPtr<ID3D12Resource> resource12;
    };
    struct CommandSlot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t doneFence{};
    };
    struct RetiredFeature
    {
        NVSDK_NGX_Parameter* parameters{};
        NVSDK_NGX_Handle* feature{};
        uint64_t fence{};
    };

    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device1> device11_1;
    ComPtr<ID3D11Device5> device11_5;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    std::array<CommandSlot, kCommandSlotCount> commands;
    ComPtr<ID3D12RootSignature> unpackRootSignature;
    ComPtr<ID3D12PipelineState> unpackPipeline;
    ComPtr<ID3D12DescriptorHeap> unpackDescriptors;
    SharedTexture colorProxy;
    SharedTexture guidePack;
    SharedTexture rawOutput;
    ComPtr<ID3D12Resource> motion12;
    ComPtr<ID3D12Resource> depth12;
    D3D12_RESOURCE_STATES motionState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES depthState{D3D12_RESOURCE_STATE_COMMON};
    uint32_t width{};
    uint32_t height{};
    std::wstring directory;
    uint64_t nextFence{};
    uint64_t lastSubmittedFence{};
    uint64_t evaluatedFrames{};
    uint64_t lastRetryGeneration{};
    bool resetPending{true};
    bool failureLatched{};
    bool unrecoverableResourceHazard{};
    int32_t lastNgxResult{};
    std::string message{"DLSSNR D3D12 bridge not initialized"};

    HMODULE runtimeModule{};
    DlssNrLogFn logCallback{};
    DlssNrInitExtFn initExt{};
    DlssNrCreateFeatureFn createFeature{};
    DlssNrEvaluateFeatureFn evaluateFeature{};
    DlssNrReleaseFeatureFn releaseFeature{};
    DlssNrShutdownFn shutdown{};
    bool coreInitialized{};
    bool snippetInitialized{};
    bool processDetaching{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    int featurePreset{-1};
    uint64_t featureLastFence{};
    std::vector<RetiredFeature> retired;

    void LogResult(const char* stage, NVSDK_NGX_Result result,
        const std::string& detail = {}) const
    {
        if (!logCallback) return;
        char prefix[128]{};
        std::snprintf(prefix, sizeof(prefix), "%s result=0x%08x", stage,
            static_cast<unsigned int>(result));
        logCallback(detail.empty() ? std::string(prefix) : std::string(prefix) + " " + detail);
    }

    void Fail(const std::string& text, int32_t result = 0)
    {
        failureLatched = true;
        lastNgxResult = result;
        message = text;
        resetPending = true;
    }

    uint64_t CompletedFence() const
    {
        return fence12 ? fence12->GetCompletedValue() : 0;
    }

    void ReleaseFeaturePair(NVSDK_NGX_Handle*& handle, NVSDK_NGX_Parameter*& params)
    {
        if (handle && releaseFeature)
        {
            const NVSDK_NGX_Result result = releaseFeature(handle);
            LogResult("SNIPPET_RELEASE_FEATURE", result);
        }
        handle = nullptr;
        if (params)
        {
            const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_DestroyParameters(params);
            LogResult("DESTROY_PARAMETERS", result);
        }
        params = nullptr;
    }

    bool WaitForOutstandingWork()
    {
        if (!fence12 || !lastSubmittedFence || CompletedFence() >= lastSubmittedFence)
            return true;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
        {
            if (logCallback) logCallback("DLSSNR_SHUTDOWN_WAIT CreateEvent failed");
            return false;
        }
        const HRESULT hr = fence12->SetEventOnCompletion(lastSubmittedFence, event);
        const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        if (FAILED(hr) || wait != WAIT_OBJECT_0)
        {
            if (logCallback)
            {
                std::ostringstream text;
                text << "DLSSNR_SHUTDOWN_WAIT failed hr=0x" << std::hex << hr
                     << " wait=" << std::dec << wait << " fence=" << lastSubmittedFence;
                logCallback(text.str());
            }
            return false;
        }
        return true;
    }

    void ShutdownNgx()
    {
        if (!WaitForOutstandingWork())
        {
            if (logCallback)
                logCallback("DLSSNR_SHUTDOWN_DEFERRED outstanding private D3D12 work");
            return;
        }
        for (RetiredFeature& item : retired)
            ReleaseFeaturePair(item.feature, item.parameters);
        retired.clear();
        ReleaseFeaturePair(feature, parameters);
        featurePreset = -1;
        featureLastFence = 0;

        if (snippetInitialized && shutdown)
        {
            const NVSDK_NGX_Result result = shutdown(device12.Get());
            LogResult("SNIPPET_SHUTDOWN", result);
            snippetInitialized = false;
        }
        RestoreCallerCompatibility();
        if (runtimeModule)
        {
            FreeLibrary(runtimeModule);
            runtimeModule = nullptr;
        }
        initExt = nullptr;
        createFeature = nullptr;
        evaluateFeature = nullptr;
        releaseFeature = nullptr;
        shutdown = nullptr;

        if (coreInitialized)
        {
            const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Shutdown1(device12.Get());
            LogResult("CORE_D3D12_SHUTDOWN", result);
            coreInitialized = false;
        }
    }

    void CollectRetired()
    {
        const uint64_t completed = CompletedFence();
        retired.erase(std::remove_if(retired.begin(), retired.end(), [&](RetiredFeature& item)
        {
            if (item.fence && item.fence > completed) return false;
            ReleaseFeaturePair(item.feature, item.parameters);
            return true;
        }), retired.end());
    }

    void RetireCurrentFeature()
    {
        if (!feature && !parameters) return;
        if (!featureLastFence || featureLastFence <= CompletedFence())
            ReleaseFeaturePair(feature, parameters);
        else
        {
            retired.push_back({parameters, feature, featureLastFence});
            parameters = nullptr;
            feature = nullptr;
        }
        featurePreset = -1;
        featureLastFence = 0;
    }

    bool InitializeDevices(ID3D11Device* device, std::string& error)
    {
        if (device12) return device11.Get() == device;
        device11 = device;
        if (FAILED(device11.As(&device11_1)) || FAILED(device11.As(&device11_5)))
        {
            error = "D3D11.4 fence interfaces are unavailable";
            return false;
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device11.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)))
        {
            error = "Could not resolve Odyssey's DXGI adapter";
            return false;
        }
        HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device12));
        if (FAILED(hr))
        {
            std::ostringstream text;
            text << "D3D12CreateDevice on Odyssey adapter failed hr=0x" << std::hex << hr;
            error = text.str();
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12));
        if (FAILED(hr))
        {
            error = "D3D12 direct command queue creation failed";
            return false;
        }
        hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12));
        if (FAILED(hr))
        {
            error = "D3D12 shared fence creation failed";
            return false;
        }
        HANDLE fenceHandle{};
        hr = device12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr,
            &fenceHandle);
        if (FAILED(hr))
        {
            error = "D3D12 shared fence handle creation failed";
            return false;
        }
        hr = device11_5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&fence11));
        CloseHandle(fenceHandle);
        if (FAILED(hr))
        {
            error = "D3D11 could not open the D3D12 shared fence";
            return false;
        }
        for (CommandSlot& slot : commands)
        {
            hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&slot.allocator));
            if (FAILED(hr))
            {
                error = "D3D12 command allocator creation failed";
                return false;
            }
            hr = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list));
            if (FAILED(hr) || FAILED(slot.list->Close()))
            {
                error = "D3D12 command list creation failed";
                return false;
            }
        }
        return true;
    }

    bool CreateUnpackPipeline(std::string& error)
    {
        if (unpackPipeline) return true;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 2;
        parameter.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &parameter;
        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &signature, &errors);
        if (FAILED(hr))
        {
            error = "D3D12 guide-unpack root signature serialization failed";
            return false;
        }
        hr = device12->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&unpackRootSignature));
        if (FAILED(hr))
        {
            error = "D3D12 guide-unpack root signature creation failed";
            return false;
        }
        const std::vector<uint8_t> shader = ReadFileBytes(directory + L"\\" + kGuideShaderName);
        if (shader.empty())
        {
            error = "D3D12 guide-unpack shader is missing";
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = unpackRootSignature.Get();
        pipeline.CS = {shader.data(), shader.size()};
        hr = device12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&unpackPipeline));
        if (FAILED(hr))
        {
            error = "D3D12 guide-unpack pipeline creation failed";
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 3;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&unpackDescriptors));
        if (FAILED(hr))
        {
            error = "D3D12 guide-unpack descriptor heap creation failed";
            return false;
        }
        return true;
    }

    bool CreateSharedTexture(SharedTexture& output, uint32_t textureWidth,
        uint32_t textureHeight, bool createSrv, bool createUav, std::string& error)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = textureWidth;
        desc.Height = textureHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // ALLOW_RENDER_TARGET satisfies the cross-DDI shared-heap texture
        // contract; ALLOW_SIMULTANEOUS_ACCESS makes COMMON the explicit
        // D3D11/D3D12 handoff state synchronized by the shared fence.
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED,
            &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output.resource12));
        if (FAILED(hr))
        {
            std::ostringstream text;
            text << "D3D12 FP16 shared texture creation failed hr=0x" << std::hex << hr;
            error = text.str();
            return false;
        }
        HANDLE sharedHandle{};
        hr = device12->CreateSharedHandle(output.resource12.Get(), nullptr, GENERIC_ALL,
            nullptr, &sharedHandle);
        if (FAILED(hr))
        {
            error = "D3D12 FP16 shared handle creation failed";
            return false;
        }
        hr = device11_1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&output.texture11));
        CloseHandle(sharedHandle);
        if (FAILED(hr))
        {
            std::ostringstream text;
            text << "D3D11 OpenSharedResource1(FP16) failed hr=0x" << std::hex << hr;
            error = text.str();
            return false;
        }
        if (createSrv && FAILED(device11->CreateShaderResourceView(output.texture11.Get(),
                nullptr, &output.srv11)))
        {
            error = "D3D11 shared FP16 SRV creation failed";
            return false;
        }
        if (createUav && FAILED(device11->CreateUnorderedAccessView(output.texture11.Get(),
                nullptr, &output.uav11)))
        {
            error = "D3D11 shared FP16 UAV creation failed";
            return false;
        }
        return true;
    }

    bool CreatePrivateTexture(DXGI_FORMAT format, ComPtr<ID3D12Resource>& output,
        std::string& error)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output));
        if (FAILED(hr))
        {
            error = "D3D12 private guide texture creation failed";
            return false;
        }
        return true;
    }

    bool RecreateResources(uint32_t newWidth, uint32_t newHeight, std::string& error)
    {
        if (width == newWidth && height == newHeight && colorProxy.resource12 &&
            guidePack.resource12 && rawOutput.resource12)
            return true;
        if (lastSubmittedFence && CompletedFence() < lastSubmittedFence)
        {
            message = "D3D12 bridge resize deferred until the previous submission completes";
            return false;
        }
        CollectRetired();
        RetireCurrentFeature();
        colorProxy = {};
        guidePack = {};
        rawOutput = {};
        motion12.Reset();
        depth12.Reset();
        width = newWidth;
        height = newHeight;
        if (!CreateSharedTexture(colorProxy, width, height, true, true, error) ||
            !CreateSharedTexture(guidePack, width, height, false, true, error) ||
            !CreateSharedTexture(rawOutput, width, height, true, false, error) ||
            !CreatePrivateTexture(DXGI_FORMAT_R16G16_FLOAT, motion12, error) ||
            !CreatePrivateTexture(DXGI_FORMAT_R32_FLOAT, depth12, error))
            return false;
        const UINT increment = device12->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            unpackDescriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device12->CreateShaderResourceView(guidePack.resource12.Get(), &srv, cpu);
        cpu.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        device12->CreateUnorderedAccessView(motion12.Get(), nullptr, &uav, cpu);
        cpu.ptr += increment;
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        device12->CreateUnorderedAccessView(depth12.Get(), nullptr, &uav, cpu);
        motionState = D3D12_RESOURCE_STATE_COMMON;
        depthState = D3D12_RESOURCE_STATE_COMMON;
        resetPending = true;
        message = "D3D11/D3D12 FP16 shared working set ready";
        return true;
    }

    bool InitializeCore(std::string& error)
    {
        if (coreInitialized) return true;
        std::wstring modulePath = directory;
        wchar_t* paths[] = {modulePath.data()};
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Length = 1;
        info.PathListInfo.Path = paths;

        std::wstring appData = directory;
        std::array<wchar_t, 32768> localAppData{};
        const DWORD localCount = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        if (localCount && localCount < localAppData.size())
        {
            appData.assign(localAppData.data(), localCount);
            appData += L"\\ACOdysseyDLAA";
            CreateDirectoryW(appData.c_str(), nullptr);
        }

        const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId,
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, appData.c_str(), device12.Get(), &info,
            NVSDK_NGX_Version_API);
        LogResult("CORE_D3D12_INIT", result, "projectId=" + std::string(kProjectId));
        if (NVSDK_NGX_FAILED(result))
        {
            error = ResultText("CORE_D3D12_INIT", result);
            lastNgxResult = static_cast<int32_t>(result);
            return false;
        }
        coreInitialized = true;
        return true;
    }

    bool InitializeSnippet(std::string& error)
    {
        if (snippetInitialized) return true;
        const std::wstring runtimePath = directory + L"\\" + kRuntimeName;
        runtimeModule = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!runtimeModule)
        {
            error = "nvngx_dlssnr.dll could not be loaded from the mod directory";
            return false;
        }
        const bool resolved =
            ResolveExport(runtimeModule, "NVSDK_NGX_D3D12_Init_Ext", initExt) &&
            ResolveExport(runtimeModule, "NVSDK_NGX_D3D12_CreateFeature", createFeature) &&
            ResolveExport(runtimeModule, "NVSDK_NGX_D3D12_EvaluateFeature", evaluateFeature) &&
            ResolveExport(runtimeModule, "NVSDK_NGX_D3D12_ReleaseFeature", releaseFeature) &&
            ResolveExport(runtimeModule, "NVSDK_NGX_D3D12_Shutdown1", shutdown);
        if (!resolved)
        {
            error = "nvngx_dlssnr.dll is missing a required D3D12 snippet export";
            FreeLibrary(runtimeModule);
            runtimeModule = nullptr;
            return false;
        }
        if (!InstallCallerCompatibility(runtimeModule, error))
        {
            RestoreCallerCompatibility();
            FreeLibrary(runtimeModule);
            runtimeModule = nullptr;
            initExt = nullptr;
            createFeature = nullptr;
            evaluateFeature = nullptr;
            releaseFeature = nullptr;
            shutdown = nullptr;
            return false;
        }
        const NVSDK_NGX_Result result = initExt(kDlssNrApplicationId, directory.c_str(),
            device12.Get(), NVSDK_NGX_Version_API, nullptr);
        LogResult("SNIPPET_INIT", result);
        if (NVSDK_NGX_FAILED(result))
        {
            error = ResultText("DLSSNR D3D12 Init_Ext", result);
            lastNgxResult = static_cast<int32_t>(result);
            RestoreCallerCompatibility();
            FreeLibrary(runtimeModule);
            runtimeModule = nullptr;
            return false;
        }
        snippetInitialized = true;
        return true;
    }

    enum class FeatureResult { Ready, Busy, Failed };

    FeatureResult EnsureFeature(ID3D12GraphicsCommandList* list,
        const DlssNrD3D12Settings& settings, bool& rebuilt)
    {
        rebuilt = false;
        if (feature && featurePreset == settings.preset) return FeatureResult::Ready;
        CollectRetired();
        if (retired.size() >= kMaxRetiredFeatures)
        {
            message = "DLSSNR preset rebuild deferred: retired feature queue is busy";
            resetPending = true;
            return FeatureResult::Busy;
        }
        rebuilt = feature != nullptr;
        RetireCurrentFeature();
        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_AllocateParameters(&parameters);
        LogResult("ALLOCATE", result);
        if (NVSDK_NGX_FAILED(result) || !parameters)
        {
            Fail(ResultText("DLSSNR AllocateParameters", result), static_cast<int32_t>(result));
            return FeatureResult::Failed;
        }
        SetOneToOne(parameters, width, height);
        parameters->Set("DLSSNR.Hint.Render.Preset", settings.preset);
        parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,
            static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
        parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        constexpr NVSDK_NGX_Feature kFeature = static_cast<NVSDK_NGX_Feature>(18);
        result = createFeature(list, kFeature, parameters, &feature);
        if (NVSDK_NGX_FAILED(result) || !feature)
        {
            Fail(ResultText("DLSSNR CreateFeature(18)", result), static_cast<int32_t>(result));
            ReleaseFeaturePair(feature, parameters);
            return FeatureResult::Failed;
        }
        featurePreset = settings.preset;
        featureLastFence = 0;
        return FeatureResult::Ready;
    }

    ~Impl()
    {
        // DLL detach runs under the loader lock and later-loaded dependencies
        // may already have torn down their globals. Normal ExitProcess is
        // explicitly intercepted by the proxy and calls ShutdownNgx earlier;
        // explicit unloaders must call the exported shutdown before FreeLibrary.
        if (!processDetaching) ShutdownNgx();
    }
};

DlssNrD3D12Bridge::DlssNrD3D12Bridge() : impl_(std::make_unique<Impl>()) {}
DlssNrD3D12Bridge::~DlssNrD3D12Bridge() = default;

bool DlssNrD3D12Bridge::Prepare(ID3D11Device* device, ID3D11DeviceContext*,
    uint32_t width, uint32_t height, const std::wstring& moduleDirectory,
    DlssNrLogFn logCallback)
{
    if (!device || !width || !height) return false;
    if (impl_->failureLatched || impl_->unrecoverableResourceHazard) return false;
    impl_->directory = moduleDirectory;
    impl_->logCallback = logCallback;
    std::string error;
    if (!impl_->InitializeDevices(device, error) || !impl_->CreateUnpackPipeline(error))
    {
        impl_->Fail(error);
        return false;
    }
    if (!impl_->RecreateResources(width, height, error))
    {
        if (!error.empty()) impl_->Fail(error);
        return false;
    }
    return true;
}

void DlssNrD3D12Bridge::Retry(uint64_t generation)
{
    if (generation == impl_->lastRetryGeneration) return;
    impl_->lastRetryGeneration = generation;
    impl_->CollectRetired();
    impl_->RetireCurrentFeature();
    if (impl_->unrecoverableResourceHazard)
    {
        impl_->failureLatched = true;
        impl_->message = "DLSSNR shared-fence hazard requires a game restart";
        return;
    }
    impl_->failureLatched = false;
    impl_->lastNgxResult = 0;
    impl_->resetPending = true;
    impl_->message = "DLSSNR retry requested; failure latch cleared";
}

DlssNrSubmitResult DlssNrD3D12Bridge::Submit(ID3D11DeviceContext* context,
    const DlssNrD3D12Settings& settings, bool resetHistory)
{
    DlssNrSubmitResult output{};
    if (!context || impl_->failureLatched || impl_->unrecoverableResourceHazard)
    {
        output.state = DlssNrSubmitState::Failure;
        output.ngxResult = impl_->lastNgxResult;
        output.message = impl_->message;
        return output;
    }
    std::string error;
    if (!impl_->InitializeCore(error) || !impl_->InitializeSnippet(error))
    {
        impl_->Fail(error, impl_->lastNgxResult);
        output.state = DlssNrSubmitState::Failure;
        output.ngxResult = impl_->lastNgxResult;
        output.message = error;
        return output;
    }
    impl_->CollectRetired();
    const uint64_t completed = impl_->CompletedFence();
    Impl::CommandSlot* slot{};
    for (Impl::CommandSlot& candidate : impl_->commands)
    {
        if (!candidate.doneFence || candidate.doneFence <= completed)
        {
            slot = &candidate;
            break;
        }
    }
    if (!slot)
    {
        impl_->resetPending = true;
        impl_->message = "DLSSNR command ring busy; current frame uses DLAA";
        output.state = DlssNrSubmitState::Busy;
        output.message = impl_->message;
        return output;
    }
    HRESULT hr = slot->allocator->Reset();
    if (SUCCEEDED(hr)) hr = slot->list->Reset(slot->allocator.Get(), nullptr);
    if (FAILED(hr))
    {
        impl_->Fail("D3D12 command-list reset failed");
        output.state = DlssNrSubmitState::Failure;
        output.message = impl_->message;
        return output;
    }

    bool featureRebuilt{};
    const Impl::FeatureResult featureResult =
        impl_->EnsureFeature(slot->list.Get(), settings, featureRebuilt);
    if (featureResult != Impl::FeatureResult::Ready)
    {
        slot->list->Close();
        output.state = featureResult == Impl::FeatureResult::Busy ?
            DlssNrSubmitState::Busy : DlssNrSubmitState::Failure;
        output.ngxResult = impl_->lastNgxResult;
        output.message = impl_->message;
        return output;
    }

    Transition(slot->list.Get(), impl_->guidePack.resource12.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(slot->list.Get(), impl_->motion12.Get(), impl_->motionState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(slot->list.Get(), impl_->depth12.Get(), impl_->depthState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {impl_->unpackDescriptors.Get()};
    slot->list->SetDescriptorHeaps(1, heaps);
    slot->list->SetComputeRootSignature(impl_->unpackRootSignature.Get());
    slot->list->SetPipelineState(impl_->unpackPipeline.Get());
    slot->list->SetComputeRootDescriptorTable(0,
        impl_->unpackDescriptors->GetGPUDescriptorHandleForHeapStart());
    slot->list->Dispatch((impl_->width + 7) / 8, (impl_->height + 7) / 8, 1);
    UavBarrier(slot->list.Get(), impl_->motion12.Get());
    UavBarrier(slot->list.Get(), impl_->depth12.Get());
    Transition(slot->list.Get(), impl_->motion12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(slot->list.Get(), impl_->depth12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(slot->list.Get(), impl_->colorProxy.resource12.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(slot->list.Get(), impl_->rawOutput.resource12.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    NVSDK_NGX_Parameter* parameters = impl_->parameters;
    SetOneToOne(parameters, impl_->width, impl_->height);
    parameters->Set("DLSSNR.Color", impl_->colorProxy.resource12.Get());
    parameters->Set("DLSSNR.Output", impl_->rawOutput.resource12.Get());
    parameters->Set("DLSSNR.MVec", impl_->motion12.Get());
    parameters->Set("DLSSNR.Depth", impl_->depth12.Get());
    SetSubrect(parameters, "DLSSNR.Color", impl_->width, impl_->height);
    SetSubrect(parameters, "DLSSNR.Output", impl_->width, impl_->height);
    SetSubrect(parameters, "DLSSNR.MVec", impl_->width, impl_->height);
    SetSubrect(parameters, "DLSSNR.Depth", impl_->width, impl_->height);
    parameters->Set("DLSSNR.MVecScaleX", settings.motionScaleX);
    parameters->Set("DLSSNR.MVecScaleY", settings.motionScaleY);
    parameters->Set("DLSSNR.DepthInverted", settings.depthInverted ? 1 : 0);
    parameters->Set("DLSSNR.Enabled", 1);
    const bool reset = resetHistory || featureRebuilt || impl_->resetPending;
    parameters->Set("DLSSNR.Reset", reset ? 1 : 0);
    parameters->Set("DLSSNR.Style", settings.style);
    parameters->Set("DLSSNR.Intensity", settings.intensity);
    parameters->Set("DLSSNR.LocalToneStrength", settings.localTone);
    parameters->Set("DLSSNR.LocalStructureStrength", settings.localStructure);
    parameters->Set("DLSSNR.SkinStructureStrength", settings.skinStructure);
    parameters->Set("DLSSNR.UseAutoMask", settings.autoMask ? 1 : 0);
    parameters->Set("DLSSNR.UICorrection", settings.uiCorrection ? 1 : 0);
    const NVSDK_NGX_Result ngx = impl_->evaluateFeature(slot->list.Get(), impl_->feature,
        parameters, nullptr);
    impl_->lastNgxResult = static_cast<int32_t>(ngx);
    if (NVSDK_NGX_FAILED(ngx))
    {
        slot->list->Close();
        impl_->Fail(ResultText("DLSSNR EvaluateFeature(18)", ngx),
            static_cast<int32_t>(ngx));
        impl_->RetireCurrentFeature();
        output.state = DlssNrSubmitState::Failure;
        output.ngxResult = static_cast<int32_t>(ngx);
        output.message = impl_->message;
        return output;
    }
    UavBarrier(slot->list.Get(), impl_->rawOutput.resource12.Get());
    Transition(slot->list.Get(), impl_->rawOutput.resource12.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Transition(slot->list.Get(), impl_->colorProxy.resource12.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(slot->list.Get(), impl_->guidePack.resource12.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    hr = slot->list->Close();
    if (FAILED(hr))
    {
        impl_->Fail("D3D12 command-list close failed");
        output.state = DlssNrSubmitState::Failure;
        output.message = impl_->message;
        return output;
    }

    ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))))
    {
        impl_->Fail("Odyssey immediate context has no ID3D11DeviceContext4 fence support");
        output.state = DlssNrSubmitState::Failure;
        output.message = impl_->message;
        return output;
    }
    const uint64_t readyFence = ++impl_->nextFence;
    const uint64_t doneFence = ++impl_->nextFence;
    hr = context4->Signal(impl_->fence11.Get(), readyFence);
    if (SUCCEEDED(hr)) hr = impl_->queue12->Wait(impl_->fence12.Get(), readyFence);
    if (FAILED(hr))
    {
        impl_->Fail("D3D11->D3D12 shared-fence handoff failed");
        output.state = DlssNrSubmitState::Failure;
        output.message = impl_->message;
        return output;
    }
    ID3D12CommandList* lists[] = {slot->list.Get()};
    impl_->queue12->ExecuteCommandLists(1, lists);
    hr = impl_->queue12->Signal(impl_->fence12.Get(), doneFence);
    if (FAILED(hr))
    {
        impl_->unrecoverableResourceHazard = true;
        impl_->Fail("D3D12 completion fence signal failed; shared working set quarantined");
        output.state = DlssNrSubmitState::Failure;
        output.message = impl_->message;
        return output;
    }
    hr = context4->Wait(impl_->fence11.Get(), doneFence);
    if (FAILED(hr))
    {
        impl_->unrecoverableResourceHazard = true;
        impl_->Fail("D3D12->D3D11 shared-fence handoff failed; shared working set quarantined");
        output.state = DlssNrSubmitState::Failure;
        output.message = impl_->message;
        return output;
    }
    slot->doneFence = doneFence;
    impl_->lastSubmittedFence = doneFence;
    impl_->featureLastFence = doneFence;
    impl_->motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->resetPending = false;
    ++impl_->evaluatedFrames;
    std::ostringstream success;
    success << "DLSSNR Feature 18 submitted on D3D12 fence=" << doneFence
            << " reset=" << (reset ? 1 : 0);
    impl_->message = success.str();
    output.state = DlssNrSubmitState::Success;
    output.ngxResult = static_cast<int32_t>(ngx);
    output.message = impl_->message;
    return output;
}

void DlssNrD3D12Bridge::Shutdown()
{
    impl_->ShutdownNgx();
}

void DlssNrD3D12Bridge::MarkProcessDetaching()
{
    impl_->processDetaching = true;
}

ID3D11UnorderedAccessView* DlssNrD3D12Bridge::ColorProxyUav() const
{
    return impl_->colorProxy.uav11.Get();
}

ID3D11UnorderedAccessView* DlssNrD3D12Bridge::GuidePackUav() const
{
    return impl_->guidePack.uav11.Get();
}

ID3D11ShaderResourceView* DlssNrD3D12Bridge::ColorProxySrv() const
{
    return impl_->colorProxy.srv11.Get();
}

ID3D11ShaderResourceView* DlssNrD3D12Bridge::RawOutputSrv() const
{
    return impl_->rawOutput.srv11.Get();
}

DlssNrD3D12Status DlssNrD3D12Bridge::Status() const
{
    DlssNrD3D12Status result{};
    result.failureLatched = impl_->failureLatched || impl_->unrecoverableResourceHazard;
    result.evaluatedFrames = impl_->evaluatedFrames;
    result.lastSubmittedFence = impl_->lastSubmittedFence;
    result.lastNgxResult = impl_->lastNgxResult;
    result.message = impl_->message;
    return result;
}
