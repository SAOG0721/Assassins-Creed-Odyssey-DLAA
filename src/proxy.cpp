#include <Windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dinput.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "bridge_api.h"

namespace
{
constexpr char kExpectedExeSha256[] =
    "AC327DAD2CBBDD72A3FDA8E99CBEAB9D12AF328363E4F09BC5674BDD36B8C483";
constexpr uint32_t kShaderInfoMagic = 0x41415441; // "ATAA"
constexpr wchar_t kConfigName[] = L"ACOdysseyDLAA.ini";
constexpr wchar_t kLogName[] = L"ACOdysseyDLAA.log";
constexpr wchar_t kBridgeName[] = L"ACOdysseyDLSSBridge.dll";

constexpr GUID kShaderInfoGuid{
    0x9144a0c4, 0xdfcc, 0x4797, {0xa1, 0x4a, 0x31, 0x51, 0xba, 0xa1, 0xf9, 0x20}};
constexpr GUID kConstantBufferCaptureGuid{
    0x207a35ad, 0x24f9, 0x45ca, {0xa7, 0xbd, 0x06, 0x48, 0x1c, 0x6a, 0x42, 0x31}};
constexpr GUID kCommandCaptureGuid{
    0x18beb40e, 0x680a, 0x430b, {0x9d, 0x35, 0xe8, 0xc1, 0x20, 0x7e, 0xf2, 0x76}};

enum ShaderFlags : uint32_t
{
    ShaderTemporalAny = 1u << 0,
    ShaderColor = 1u << 1,
    ShaderMotion = 1u << 2,
    ShaderDepth = 1u << 3,
    ShaderResponsive = 1u << 4,
    ShaderAccumulation = 1u << 5,
    ShaderHistoryColor = 1u << 6,
    ShaderHistoryMisc = 1u << 7,
    ShaderDitherBlend = 1u << 8,
};

struct ShaderInfo
{
    uint32_t magic{};
    uint32_t flags{};
    uint64_t bytecodeSize{};
    std::array<uint8_t, 32> sha256{};
};

HMODULE g_module{};
HMODULE g_realDinput8{};
HMODULE g_bridgeModule{};
AcoDlaaEvaluateFn g_bridgeEvaluate{};
HANDLE g_log = INVALID_HANDLE_VALUE;
std::mutex g_logMutex;
std::atomic<bool> g_workerStarted{};
std::atomic<bool> g_versionVerified{};
std::atomic<uint32_t> g_temporalShaderCount{};
std::atomic<uint32_t> g_taaDrawLogCount{};
std::atomic<uint64_t> g_captureSequence{};
std::atomic<uint64_t> g_executedCaptureCount{};
std::atomic<uint64_t> g_bridgeFrameCount{};
std::atomic<int32_t> g_lastBridgeResult{INT32_MIN};
std::atomic<uint64_t> g_contextHookRepairCount{};
std::atomic<uint64_t> g_contextHookUnknownCount{};
uint32_t g_maxTaaDrawLogs = 0;
bool g_dlaaEnabled{};
bool g_evaluateOnly{true};
bool g_depthInverted{};
bool g_allowPresentationToggle{};
UINT g_presentationToggleKey = VK_F8;
std::atomic<uint32_t> g_presentationMode{ACO_DLAA_PRESENT_GAME_TAA};
std::atomic<bool> g_presentationToggleKeyDown{};
std::atomic<bool> g_initialHistoryResetSubmitted{};

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT(WINAPI*)();
using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT(WINAPI*)();
using GetdfDIJoystickFn = LPCDIDATAFORMAT(WINAPI*)();

using D3D11CreateDeviceFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

D3D11CreateDeviceFn g_realD3D11CreateDevice{};
D3D11CreateDeviceAndSwapChainFn g_realD3D11CreateDeviceAndSwapChain{};

std::wstring ModuleDirectory()
{
    std::array<wchar_t, 32768> path{};
    const DWORD count = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size())
        return L".";
    std::wstring result(path.data(), count);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

std::wstring SiblingPath(const wchar_t* name)
{
    return ModuleDirectory() + L"\\" + name;
}

void WriteLog(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log == INVALID_HANDLE_VALUE)
    {
        const std::wstring path = SiblingPath(kLogName);
        g_log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_log == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::ostringstream line;
    line << std::setfill('0') << '[' << std::setw(2) << time.wHour << ':' << std::setw(2)
         << time.wMinute << ':' << std::setw(2) << time.wSecond << '.' << std::setw(3)
         << time.wMilliseconds << "][tid=" << GetCurrentThreadId() << "] " << message << "\r\n";
    const std::string text = line.str();
    DWORD written{};
    WriteFile(g_log, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(g_log);
}

const char* PresentationModeName(uint32_t mode)
{
    switch (mode)
    {
    case ACO_DLAA_PRESENT_GAME_TAA: return "game-taa";
    case ACO_DLAA_PRESENT_DLAA: return "dlaa";
    default: return "invalid";
    }
}

void SetPresentationMode(uint32_t mode, const char* reason)
{
    if (mode != ACO_DLAA_PRESENT_GAME_TAA && mode != ACO_DLAA_PRESENT_DLAA) return;
    const uint32_t previous = g_presentationMode.exchange(mode, std::memory_order_relaxed);
    if (previous == mode) return;
    WriteLog(std::string("PRESENTATION_MODE_CHANGED reason=") + reason +
        " mode=" + PresentationModeName(mode));
}

std::string Hex(const uint8_t* bytes, size_t size)
{
    std::ostringstream text;
    text << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < size; ++i)
        text << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    return text.str();
}

bool Sha256Bytes(const void* data, size_t size, std::array<uint8_t, 32>& digest)
{
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength{};
    DWORD resultLength{};
    std::vector<uint8_t> object;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok)
        ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                 reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) >= 0;
    if (ok)
    {
        object.resize(objectLength);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0;
    }
    if (ok && size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (ok && remaining)
        {
            const ULONG chunk = static_cast<ULONG>(std::min<size_t>(remaining, 64u * 1024u * 1024u));
            ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes), chunk, 0) >= 0;
            bytes += chunk;
            remaining -= chunk;
        }
    }
    if (ok)
        ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string Sha256File(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength{};
    DWORD resultLength{};
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok)
        ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                 reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0) >= 0;
    if (ok)
    {
        object.resize(objectLength);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0;
    }

    // CreateThread inherits the host executable's default stack reservation. Odyssey's
    // reservation is smaller than a 1 MiB local buffer, so keep the streaming buffer on the heap.
    std::vector<uint8_t> buffer(1024 * 1024);
    while (ok)
    {
        DWORD read{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            ok = false;
            break;
        }
        if (!read)
            break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (ok)
        ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;

    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok ? Hex(digest.data(), digest.size()) : std::string{};
}

std::string CurrentExecutableSha256()
{
    std::array<wchar_t, 32768> path{};
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return count && count < path.size() ? Sha256File(std::wstring(path.data(), count)) : std::string{};
}

bool ContainsString(const void* data, size_t size, const char* text)
{
    const auto* begin = static_cast<const uint8_t*>(data);
    const auto* end = begin + size;
    const auto* stringBegin = reinterpret_cast<const uint8_t*>(text);
    const auto* stringEnd = stringBegin + std::strlen(text);
    return std::search(begin, end, stringBegin, stringEnd) != end;
}

uint32_t ClassifyTemporalShader(const void* bytecode, size_t size)
{
    if (!ContainsString(bytecode, size, "TemporalAA_"))
        return 0;
    uint32_t flags = ShaderTemporalAny;
    if (ContainsString(bytecode, size, "TemporalAA_Color")) flags |= ShaderColor;
    if (ContainsString(bytecode, size, "TemporalAA_Motion")) flags |= ShaderMotion;
    if (ContainsString(bytecode, size, "TemporalAA_Depth")) flags |= ShaderDepth;
    if (ContainsString(bytecode, size, "TemporalAA_ResponsiveAA")) flags |= ShaderResponsive;
    if (ContainsString(bytecode, size, "TemporalAA_ColorAccumulation")) flags |= ShaderAccumulation;
    if (ContainsString(bytecode, size, "TemporalAA_ReprojectedHistoryColor")) flags |= ShaderHistoryColor;
    if (ContainsString(bytecode, size, "TemporalAA_ReprojectedHistoryMisc")) flags |= ShaderHistoryMisc;
    if (ContainsString(bytecode, size, "TemporalAA_DitherBlendColor")) flags |= ShaderDitherBlend;
    return flags;
}

std::string ShaderFlagText(uint32_t flags)
{
    std::ostringstream out;
    auto add = [&out](const char* name)
    {
        if (out.tellp() > 0) out << ',';
        out << name;
    };
    if (flags & ShaderColor) add("color");
    if (flags & ShaderMotion) add("motion");
    if (flags & ShaderDepth) add("depth");
    if (flags & ShaderResponsive) add("responsive");
    if (flags & ShaderAccumulation) add("accum");
    if (flags & ShaderHistoryColor) add("historyColor");
    if (flags & ShaderHistoryMisc) add("historyMisc");
    if (flags & ShaderDitherBlend) add("ditherBlend");
    return out.str();
}

const char* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
    case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    default: return nullptr;
    }
}

std::string FormatText(DXGI_FORMAT format)
{
    const char* name = FormatName(format);
    if (name) return name;
    return std::string("DXGI_FORMAT(") + std::to_string(static_cast<unsigned int>(format)) + ')';
}

std::string DescribeResource(ID3D11Resource* resource)
{
    if (!resource) return "null";
    std::ostringstream out;
    out << "res=" << resource;
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        ID3D11Texture2D* texture{};
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))) && texture)
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            out << " tex2d=" << desc.Width << 'x' << desc.Height << " mips=" << desc.MipLevels
                << " array=" << desc.ArraySize << " samples=" << desc.SampleDesc.Count
                << " format=" << FormatText(desc.Format) << " bind=0x" << std::hex << desc.BindFlags
                << std::dec;
            texture->Release();
        }
    }
    else if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        ID3D11Buffer* buffer{};
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer))) && buffer)
        {
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            out << " buffer=" << desc.ByteWidth << " usage=" << desc.Usage << " bind=0x"
                << std::hex << desc.BindFlags << " cpu=0x" << desc.CPUAccessFlags << std::dec;
            buffer->Release();
        }
    }
    else
    {
        out << " dimension=" << static_cast<unsigned int>(dimension);
    }
    return out.str();
}

std::string DescribeSrv(ID3D11ShaderResourceView* view)
{
    if (!view) return "null";
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    view->GetDesc(&desc);
    ID3D11Resource* resource{};
    view->GetResource(&resource);
    std::ostringstream out;
    out << "srv=" << view << " viewFormat=" << FormatText(desc.Format) << ' '
        << DescribeResource(resource);
    if (resource) resource->Release();
    return out.str();
}

std::string DescribeRtv(ID3D11RenderTargetView* view)
{
    if (!view) return "null";
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    view->GetDesc(&desc);
    ID3D11Resource* resource{};
    view->GetResource(&resource);
    std::ostringstream out;
    out << "rtv=" << view << " viewFormat=" << FormatText(desc.Format) << ' '
        << DescribeResource(resource);
    if (resource) resource->Release();
    return out.str();
}

template <typename T>
bool PatchSlot(void** table, size_t index, T hook, T& original)
{
    DWORD oldProtect{};
    if (!VirtualProtect(&table[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    original = reinterpret_cast<T>(table[index]);
    table[index] = reinterpret_cast<void*>(hook);
    DWORD ignored{};
    VirtualProtect(&table[index], sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &table[index], sizeof(void*));
    return original != nullptr;
}

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*,
    ID3D11ClassInstance* const*, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndexedInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
    const D3D11_BOX*, const void*, UINT, UINT);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);

struct DeviceHooks
{
    void** table{};
    CreateBufferFn createBuffer{};
    CreatePixelShaderFn createPixelShader{};
    CreateDeferredContextFn createDeferredContext{};
};

struct ContextHooks
{
    void** table{};
    PSSetShaderFn psSetShader{};
    DrawIndexedFn drawIndexed{};
    DrawFn draw{};
    MapFn map{};
    UnmapFn unmap{};
    DrawIndexedInstancedFn drawIndexedInstanced{};
    DrawInstancedFn drawInstanced{};
    DrawAutoFn drawAuto{};
    DrawIndexedInstancedIndirectFn drawIndexedInstancedIndirect{};
    DrawInstancedIndirectFn drawInstancedIndirect{};
    UpdateSubresourceFn updateSubresource{};
    ExecuteCommandListFn executeCommandList{};
    FinishCommandListFn finishCommandList{};
};

std::mutex g_hookTablesMutex;
std::vector<std::unique_ptr<DeviceHooks>> g_deviceHookTables;
std::vector<std::unique_ptr<ContextHooks>> g_contextHookTables;
std::array<std::atomic<DeviceHooks*>, 16> g_deviceHookRegistry{};
std::array<std::atomic<ContextHooks*>, 256> g_contextHookRegistry{};
std::atomic<bool> g_hookRegistryOverflowLogged{};
std::mutex g_executeDetourMutex;
void* g_executeDetourTarget{};
ExecuteCommandListFn g_executeDetourOriginal{};

DeviceHooks* FindDeviceHooks(ID3D11Device* device)
{
    void** table = *reinterpret_cast<void***>(device);
    static thread_local void** cachedTable{};
    static thread_local DeviceHooks* cachedHooks{};
    if (table == cachedTable && cachedHooks) return cachedHooks;
    for (const auto& entry : g_deviceHookRegistry)
    {
        DeviceHooks* hooks = entry.load(std::memory_order_acquire);
        if (!hooks || hooks->table != table) continue;
        cachedTable = table;
        cachedHooks = hooks;
        return cachedHooks;
    }
    std::lock_guard<std::mutex> lock(g_hookTablesMutex);
    for (const auto& hooks : g_deviceHookTables)
    {
        if (hooks->table != table) continue;
        cachedTable = table;
        cachedHooks = hooks.get();
        return cachedHooks;
    }
    return nullptr;
}

ContextHooks* FindContextHooks(ID3D11DeviceContext* context)
{
    void** table = *reinterpret_cast<void***>(context);
    static thread_local void** cachedTable{};
    static thread_local ContextHooks* cachedHooks{};
    if (table == cachedTable && cachedHooks) return cachedHooks;
    for (const auto& entry : g_contextHookRegistry)
    {
        ContextHooks* hooks = entry.load(std::memory_order_acquire);
        if (!hooks || hooks->table != table) continue;
        cachedTable = table;
        cachedHooks = hooks;
        return cachedHooks;
    }
    std::lock_guard<std::mutex> lock(g_hookTablesMutex);
    for (const auto& hooks : g_contextHookTables)
    {
        if (hooks->table != table) continue;
        cachedTable = table;
        cachedHooks = hooks.get();
        return cachedHooks;
    }
    return nullptr;
}

template <typename T, size_t N>
void PublishHookTable(std::array<std::atomic<T*>, N>& registry, T* hooks)
{
    for (auto& entry : registry)
    {
        T* expected{};
        if (entry.compare_exchange_strong(expected, hooks, std::memory_order_release,
                std::memory_order_relaxed) || expected == hooks)
            return;
    }
    if (!g_hookRegistryOverflowLogged.exchange(true, std::memory_order_relaxed))
        WriteLog("ERROR lock-free hook registry capacity was exhausted; locked fallback remains active");
}

struct MappedConstantBuffer
{
    ID3D11Buffer* buffer{};
    void* data{};
    UINT size{};
};

thread_local std::unordered_map<ID3D11DeviceContext*, ShaderInfo> g_currentTemporalShaders;
thread_local std::unordered_map<ID3D11Resource*, MappedConstantBuffer> g_mappedConstantBuffers;

class FrameCapture final : public IUnknown
{
public:
    static constexpr uint32_t Magic = 0x50414341; // "ACAP"

    FrameCapture() = default;
    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (riid == IID_IUnknown)
        {
            *output = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG value = --references_;
        if (!value) delete this;
        return value;
    }

    uint32_t magic{Magic};
    uint64_t sequence{};
    ID3D11Resource* color{};
    ID3D11Resource* motion{};
    ID3D11Resource* depth{};
    ID3D11Resource* gameOutput{};
    UINT width{};
    UINT height{};
    float fullWidth{};
    float fullHeight{};
    float lowWidth{};
    float lowHeight{};
    float jitterX{};
    float jitterY{};
    float projection22{};
    float projection23{};
    float projection32{};
    float zFrontBackValueY{};
    std::array<float, 4> maxViewDepthParams{};
    std::array<float, 16> clipXYZToViewPos{};
    std::array<float, 16> viewToWorld{};
    std::array<float, 16> worldViewProjPrevFrame{};

private:
    ~FrameCapture()
    {
        if (color) color->Release();
        if (motion) motion->Release();
        if (depth) depth->Release();
        if (gameOutput) gameOutput->Release();
    }

    std::atomic<ULONG> references_{1};
};

thread_local std::unordered_map<ID3D11DeviceContext*, FrameCapture*> g_pendingFrameCaptures;

bool ConstantBufferInfo(ID3D11Resource* resource, ID3D11Buffer** bufferOut, D3D11_BUFFER_DESC* descOut)
{
    if (bufferOut) *bufferOut = nullptr;
    if (!resource) return false;
    ID3D11Buffer* buffer{};
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))) || !buffer)
        return false;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
    {
        buffer->Release();
        return false;
    }
    if (descOut) *descOut = desc;
    if (bufferOut) *bufferOut = buffer;
    else buffer->Release();
    return true;
}

void StoreConstantBufferCapture(ID3D11Buffer* buffer, const void* data, UINT size)
{
    if (buffer && data && size)
        buffer->SetPrivateData(kConstantBufferCaptureGuid, size, data);
}

bool Texture2DSize(ID3D11Resource* resource, UINT& width, UINT& height, DXGI_FORMAT* format = nullptr)
{
    if (!resource) return false;
    ID3D11Texture2D* texture{};
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    width = desc.Width;
    height = desc.Height;
    if (format) *format = desc.Format;
    return width != 0 && height != 0;
}

FrameCapture* CaptureCurrentFrame(ID3D11DeviceContext* context)
{
    std::array<ID3D11ShaderResourceView*, 3> views{};
    context->PSGetShaderResources(85, static_cast<UINT>(views.size()), views.data());
    std::array<ID3D11Resource*, 3> resources{};
    for (size_t i = 0; i < views.size(); ++i)
    {
        if (views[i])
        {
            views[i]->GetResource(&resources[i]);
            views[i]->Release();
        }
    }

    ID3D11RenderTargetView* outputView{};
    context->OMGetRenderTargets(1, &outputView, nullptr);
    ID3D11Resource* output{};
    if (outputView)
    {
        outputView->GetResource(&output);
        outputView->Release();
    }

    UINT width{}, height{}, motionWidth{}, motionHeight{}, depthWidth{}, depthHeight{}, outputWidth{}, outputHeight{};
    const bool dimensionsMatch =
        Texture2DSize(resources[0], width, height) &&
        Texture2DSize(resources[1], motionWidth, motionHeight) &&
        Texture2DSize(resources[2], depthWidth, depthHeight) &&
        Texture2DSize(output, outputWidth, outputHeight) &&
        width == motionWidth && width == depthWidth && width == outputWidth &&
        height == motionHeight && height == depthHeight && height == outputHeight;
    if (!dimensionsMatch)
    {
        for (auto* resource : resources) if (resource) resource->Release();
        if (output) output->Release();
        return nullptr;
    }

    constexpr UINT firstConstantBufferSlot = 2;
    std::array<ID3D11Buffer*, 5> boundConstantBuffers{};
    context->PSGetConstantBuffers(firstConstantBufferSlot,
        static_cast<UINT>(boundConstantBuffers.size()), boundConstantBuffers.data());
    thread_local std::vector<uint8_t> passConstants;
    thread_local std::vector<uint8_t> instanceConstants;
    thread_local std::vector<uint8_t> temporalConstants;
    auto captureConstants = [](ID3D11Buffer* buffer, std::vector<uint8_t>& destination,
                                UINT requiredSize)
    {
        UINT capturedSize{};
        HRESULT sizeResult = buffer
            ? buffer->GetPrivateData(kConstantBufferCaptureGuid, &capturedSize, nullptr)
            : E_POINTER;
        bool captured = false;
        if ((SUCCEEDED(sizeResult) || sizeResult == DXGI_ERROR_MORE_DATA) &&
            capturedSize >= requiredSize && capturedSize <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u)
        {
            destination.resize(capturedSize);
            UINT readSize = capturedSize;
            captured = SUCCEEDED(buffer->GetPrivateData(
                kConstantBufferCaptureGuid, &readSize, destination.data())) && readSize >= requiredSize;
        }
        return captured;
    };
    const bool haveConstants =
        captureConstants(boundConstantBuffers[0], passConstants, 1232) &&
        captureConstants(boundConstantBuffers[2], instanceConstants, 256) &&
        captureConstants(boundConstantBuffers[4], temporalConstants, 128);
    for (ID3D11Buffer* buffer : boundConstantBuffers)
        if (buffer) buffer->Release();
    if (!haveConstants)
    {
        for (auto* resource : resources) if (resource) resource->Release();
        if (output) output->Release();
        return nullptr;
    }

    auto readFloat = [](const auto& constants, size_t offset)
    {
        float value{};
        std::memcpy(&value, constants.data() + offset, sizeof(value));
        return value;
    };
    auto* capture = new (std::nothrow) FrameCapture();
    if (!capture)
    {
        for (auto* resource : resources) if (resource) resource->Release();
        if (output) output->Release();
        return nullptr;
    }
    capture->sequence = g_captureSequence.fetch_add(1) + 1;
    capture->color = resources[0];
    capture->motion = resources[1];
    capture->depth = resources[2];
    capture->gameOutput = output;
    capture->width = width;
    capture->height = height;
    capture->fullWidth = readFloat(temporalConstants, 16);
    capture->fullHeight = readFloat(temporalConstants, 20);
    capture->lowWidth = readFloat(temporalConstants, 32);
    capture->lowHeight = readFloat(temporalConstants, 36);
    capture->jitterX = readFloat(temporalConstants, 48);
    capture->jitterY = readFloat(temporalConstants, 52);
    capture->projection32 = readFloat(temporalConstants, 116);
    capture->projection23 = readFloat(temporalConstants, 120);
    capture->projection22 = readFloat(temporalConstants, 124);
    capture->zFrontBackValueY = readFloat(passConstants, 852);
    std::memcpy(capture->maxViewDepthParams.data(), temporalConstants.data() + 112,
        sizeof(capture->maxViewDepthParams));
    std::memcpy(capture->clipXYZToViewPos.data(), passConstants.data() + 1088,
        sizeof(capture->clipXYZToViewPos));
    std::memcpy(capture->viewToWorld.data(), passConstants.data() + 80,
        sizeof(capture->viewToWorld));
    std::memcpy(capture->worldViewProjPrevFrame.data(), instanceConstants.data() + 192,
        sizeof(capture->worldViewProjPrevFrame));
    return capture;
}

void MaybeCaptureTemporalCommand(ID3D11DeviceContext* context)
{
    if (!context) return;
    const auto shader = g_currentTemporalShaders.find(context);
    if (shader == g_currentTemporalShaders.end()) return;
    constexpr uint32_t required = ShaderColor | ShaderMotion | ShaderDepth | ShaderAccumulation;
    if ((shader->second.flags & required) != required) return;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED ||
        g_pendingFrameCaptures.contains(context))
        return;
    if (FrameCapture* capture = CaptureCurrentFrame(context))
    {
        g_pendingFrameCaptures.emplace(context, capture);
        if (capture->sequence <= 8 || capture->sequence % 300 == 0)
        {
            std::ostringstream line;
            line << "TAA_COMMAND_CAPTURED sequence=" << capture->sequence << " context=" << context
                 << " size=" << capture->width << 'x' << capture->height << " full="
                 << capture->fullWidth << 'x' << capture->fullHeight << " low="
                 << capture->lowWidth << 'x' << capture->lowHeight << " jitter=("
                 << capture->jitterX << ',' << capture->jitterY << ')';
            WriteLog(line.str());
        }
    }
}


void HookContext(ID3D11DeviceContext* context);

HRESULT STDMETHODCALLTYPE HookCreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initialData, ID3D11Buffer** output)
{
    DeviceHooks* hooks = FindDeviceHooks(self);
    const HRESULT result = hooks && hooks->createBuffer
        ? hooks->createBuffer(self, desc, initialData, output) : E_FAIL;
    if (SUCCEEDED(result) && desc && output && *output && initialData && initialData->pSysMem &&
        (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER))
    {
        StoreConstantBufferCapture(*output, initialData->pSysMem, desc->ByteWidth);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(ID3D11Device* self, const void* bytecode,
    SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** output)
{
    DeviceHooks* hooks = FindDeviceHooks(self);
    const HRESULT result = hooks && hooks->createPixelShader
        ? hooks->createPixelShader(self, bytecode, bytecodeLength, linkage, output) : E_FAIL;
    if (SUCCEEDED(result) && output && *output && bytecode && bytecodeLength)
    {
        const uint32_t flags = ClassifyTemporalShader(bytecode, bytecodeLength);
        if (flags)
        {
            ShaderInfo info{};
            info.magic = kShaderInfoMagic;
            info.flags = flags;
            info.bytecodeSize = bytecodeLength;
            if (Sha256Bytes(bytecode, bytecodeLength, info.sha256))
            {
                (*output)->SetPrivateData(kShaderInfoGuid, sizeof(info), &info);
                const uint32_t index = g_temporalShaderCount.fetch_add(1) + 1;
                std::ostringstream line;
                line << "TEMPORAL_SHADER_CREATED index=" << index << " ps=" << *output
                     << " bytes=" << bytecodeLength << " sha256="
                     << Hex(info.sha256.data(), info.sha256.size()) << " flags="
                     << ShaderFlagText(flags);
                WriteLog(line.str());
            }
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(ID3D11Device* self, UINT flags,
    ID3D11DeviceContext** output)
{
    DeviceHooks* hooks = FindDeviceHooks(self);
    const HRESULT result = hooks && hooks->createDeferredContext
        ? hooks->createDeferredContext(self, flags, output) : E_FAIL;
    if (SUCCEEDED(result) && output && *output)
        HookContext(*output);
    return result;
}

void ProbeTaaDraw(ID3D11DeviceContext* context, const char* method, const std::string& arguments)
{
    const auto shaderIt = g_currentTemporalShaders.find(context);
    if (shaderIt == g_currentTemporalShaders.end() || shaderIt->second.magic != kShaderInfoMagic)
        return;
    const uint32_t sequence = g_taaDrawLogCount.fetch_add(1);
    if (sequence >= g_maxTaaDrawLogs)
        return;

    const ShaderInfo& shader = shaderIt->second;
    std::ostringstream report;
    report << "TAA_DRAW sequence=" << sequence + 1 << " context=" << context
           << " type=" << (context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred")
           << " method=" << method << ' ' << arguments << " shader="
           << Hex(shader.sha256.data(), shader.sha256.size()) << " flags="
           << ShaderFlagText(shader.flags);

    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    context->RSGetViewports(&viewportCount, viewports.data());
    for (UINT i = 0; i < viewportCount; ++i)
    {
        const auto& viewport = viewports[i];
        report << "\n  viewport[" << i << "]=" << viewport.TopLeftX << ',' << viewport.TopLeftY
               << ' ' << viewport.Width << 'x' << viewport.Height << " depth="
               << viewport.MinDepth << ".." << viewport.MaxDepth;
    }

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ID3D11DepthStencilView* dsv{};
    context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
    for (size_t i = 0; i < rtvs.size(); ++i)
    {
        if (rtvs[i])
        {
            report << "\n  rtv[" << i << "] " << DescribeRtv(rtvs[i]);
            rtvs[i]->Release();
        }
    }
    if (dsv)
    {
        ID3D11Resource* resource{};
        dsv->GetResource(&resource);
        report << "\n  dsv=" << dsv << ' ' << DescribeResource(resource);
        if (resource) resource->Release();
        dsv->Release();
    }

    constexpr UINT firstSrv = 80;
    constexpr UINT srvCount = 21;
    std::array<ID3D11ShaderResourceView*, srvCount> srvs{};
    context->PSGetShaderResources(firstSrv, srvCount, srvs.data());
    for (UINT i = 0; i < srvCount; ++i)
    {
        if (srvs[i])
        {
            report << "\n  srv[t" << firstSrv + i << "] " << DescribeSrv(srvs[i]);
            srvs[i]->Release();
        }
    }

    constexpr UINT constantBufferCount = 8;
    std::array<ID3D11Buffer*, constantBufferCount> buffers{};
    context->PSGetConstantBuffers(0, constantBufferCount, buffers.data());
    for (UINT i = 0; i < constantBufferCount; ++i)
    {
        ID3D11Buffer* buffer = buffers[i];
        if (!buffer) continue;
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        report << "\n  cb[" << i << "] buffer=" << buffer << " bytes=" << desc.ByteWidth
               << " usage=" << desc.Usage << " cpu=0x" << std::hex << desc.CPUAccessFlags << std::dec;
        if (i == 6 && desc.ByteWidth)
        {
            std::vector<uint8_t> captured(desc.ByteWidth);
            UINT capturedSize = desc.ByteWidth;
            if (SUCCEEDED(buffer->GetPrivateData(kConstantBufferCaptureGuid, &capturedSize, captured.data())) &&
                capturedSize >= sizeof(float))
            {
                const size_t floatCount = std::min<size_t>(capturedSize / sizeof(float), 48);
                report << " capturedFloats=";
                for (size_t f = 0; f < floatCount; ++f)
                {
                    float value{};
                    std::memcpy(&value, captured.data() + f * sizeof(float), sizeof(float));
                    if (f) report << ',';
                    if (std::isfinite(value)) report << std::setprecision(8) << value;
                    else report << "nonfinite";
                }
            }
            else
            {
                report << " capturedFloats=unavailable";
            }
        }
        buffer->Release();
    }
    WriteLog(report.str());
}

bool ShouldLogTaaDraw(ID3D11DeviceContext* context)
{
    return g_taaDrawLogCount.load(std::memory_order_relaxed) < g_maxTaaDrawLogs &&
        g_currentTemporalShaders.contains(context);
}

void STDMETHODCALLTYPE HookPSSetShader(ID3D11DeviceContext* self, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classInstances, UINT classInstanceCount)
{
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->psSetShader)
        hooks->psSetShader(self, shader, classInstances, classInstanceCount);

    ShaderInfo info{};
    UINT size = sizeof(info);
    if (shader && SUCCEEDED(shader->GetPrivateData(kShaderInfoGuid, &size, &info)) &&
        size == sizeof(info) && info.magic == kShaderInfoMagic)
        g_currentTemporalShaders[self] = info;
    else
        g_currentTemporalShaders.erase(self);
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT startIndex, INT baseVertex)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self))
        ProbeTaaDraw(self, "DrawIndexed", "indexCount=" + std::to_string(indexCount));
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->drawIndexed) hooks->drawIndexed(self, indexCount, startIndex, baseVertex);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* self, UINT vertexCount, UINT startVertex)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self))
        ProbeTaaDraw(self, "Draw", "vertexCount=" + std::to_string(vertexCount));
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->draw) hooks->draw(self, vertexCount, startVertex);
}

HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource,
    D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    ContextHooks* hooks = FindContextHooks(self);
    const HRESULT result = hooks && hooks->map
        ? hooks->map(self, resource, subresource, mapType, mapFlags, mapped) : E_FAIL;
    if (SUCCEEDED(result) && resource && subresource == 0 && mapped && mapped->pData)
    {
        ID3D11Buffer* buffer{};
        D3D11_BUFFER_DESC desc{};
        if (ConstantBufferInfo(resource, &buffer, &desc))
        {
            auto existing = g_mappedConstantBuffers.find(resource);
            if (existing != g_mappedConstantBuffers.end() && existing->second.buffer)
                existing->second.buffer->Release();
            g_mappedConstantBuffers[resource] = {buffer, mapped->pData, desc.ByteWidth};
        }
    }
    return result;
}

void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource)
{
    thread_local std::vector<uint8_t> copy;
    copy.clear();
    ID3D11Buffer* buffer{};
    if (resource && subresource == 0)
    {
        const auto it = g_mappedConstantBuffers.find(resource);
        if (it != g_mappedConstantBuffers.end())
        {
            buffer = it->second.buffer;
            if (it->second.data && it->second.size)
            {
                const auto* bytes = static_cast<const uint8_t*>(it->second.data);
                copy.assign(bytes, bytes + it->second.size);
            }
            g_mappedConstantBuffers.erase(it);
        }
    }

    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->unmap) hooks->unmap(self, resource, subresource);
    if (buffer)
    {
        StoreConstantBufferCapture(buffer, copy.data(), static_cast<UINT>(copy.size()));
        buffer->Release();
    }
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* self, UINT indexCountPerInstance,
    UINT instanceCount, UINT startIndex, INT baseVertex, UINT startInstance)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self))
        ProbeTaaDraw(self, "DrawIndexedInstanced", "indexCount=" +
            std::to_string(indexCountPerInstance) + " instances=" + std::to_string(instanceCount));
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->drawIndexedInstanced)
        hooks->drawIndexedInstanced(self, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* self, UINT vertexCountPerInstance,
    UINT instanceCount, UINT startVertex, UINT startInstance)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self))
        ProbeTaaDraw(self, "DrawInstanced", "vertexCount=" +
            std::to_string(vertexCountPerInstance) + " instances=" + std::to_string(instanceCount));
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->drawInstanced)
        hooks->drawInstanced(self, vertexCountPerInstance, instanceCount, startVertex, startInstance);
}

void STDMETHODCALLTYPE HookDrawAuto(ID3D11DeviceContext* self)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self)) ProbeTaaDraw(self, "DrawAuto", "");
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->drawAuto) hooks->drawAuto(self);
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args,
    UINT alignedByteOffset)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self))
        ProbeTaaDraw(self, "DrawIndexedInstancedIndirect",
            "argsOffset=" + std::to_string(alignedByteOffset));
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->drawIndexedInstancedIndirect)
        hooks->drawIndexedInstancedIndirect(self, args, alignedByteOffset);
}

void STDMETHODCALLTYPE HookDrawInstancedIndirect(ID3D11DeviceContext* self, ID3D11Buffer* args,
    UINT alignedByteOffset)
{
    MaybeCaptureTemporalCommand(self);
    if (ShouldLogTaaDraw(self))
        ProbeTaaDraw(self, "DrawInstancedIndirect", "argsOffset=" +
            std::to_string(alignedByteOffset));
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->drawInstancedIndirect)
        hooks->drawInstancedIndirect(self, args, alignedByteOffset);
}

void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* self, ID3D11CommandList* commandList,
    BOOL restoreContextState)
{
    IUnknown* unknown{};
    UINT size = sizeof(unknown);
    if (commandList)
        commandList->GetPrivateData(kCommandCaptureGuid, &size, &unknown);
    ContextHooks* hooks = FindContextHooks(self);
    if (g_executeDetourOriginal &&
        (!hooks || reinterpret_cast<void*>(hooks->executeCommandList) == g_executeDetourTarget))
        g_executeDetourOriginal(self, commandList, restoreContextState);
    else if (hooks && hooks->executeCommandList)
        hooks->executeCommandList(self, commandList, restoreContextState);
    if (unknown)
    {
        auto* capture = reinterpret_cast<FrameCapture*>(unknown);
        if (capture->magic == FrameCapture::Magic)
        {
            if (g_allowPresentationToggle)
            {
                DWORD foregroundProcess{};
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
                const bool keyDown = foregroundProcess == GetCurrentProcessId() &&
                    (GetAsyncKeyState(static_cast<int>(g_presentationToggleKey)) & 0x8000) != 0;
                const bool keyWasDown = g_presentationToggleKeyDown.exchange(keyDown);
                if (keyDown && !keyWasDown)
                {
                    const uint32_t current =
                        g_presentationMode.load(std::memory_order_relaxed);
                    const uint32_t next = current == ACO_DLAA_PRESENT_DLAA
                        ? ACO_DLAA_PRESENT_GAME_TAA : ACO_DLAA_PRESENT_DLAA;
                    SetPresentationMode(next, "F8");
                }
            }
            const uint64_t executedFrame = g_executedCaptureCount.fetch_add(1) + 1;
            if (executedFrame <= 8 || executedFrame % 300 == 0)
            {
                std::ostringstream line;
                line << "TAA_COMMAND_EXECUTED frame=" << executedFrame << " sequence="
                     << capture->sequence << " immediateContext=" << self << " size="
                     << capture->width << 'x' << capture->height << " restoreContextState="
                     << (restoreContextState ? 1 : 0);
                WriteLog(line.str());
            }
            if (g_bridgeEvaluate)
            {
                const uint32_t presentationMode =
                    g_presentationMode.load(std::memory_order_relaxed);
                constexpr float jitterScale = 1.0f;
                constexpr int32_t jitterSignX = 1;
                constexpr int32_t jitterSignY = 1;
                constexpr int32_t jitterPhaseOffset = 0;
                constexpr bool jitterCycleVerified = false;
                const bool resetHistory =
                    !g_initialHistoryResetSubmitted.exchange(true, std::memory_order_relaxed);
                ACO_DLAA_Frame frame{};
                frame.structSize = sizeof(frame);
                frame.context = self;
                frame.color = capture->color;
                frame.packedMotion = capture->motion;
                frame.depth = capture->depth;
                frame.gameOutput = capture->gameOutput;
                frame.width = capture->width;
                frame.height = capture->height;
                frame.jitterX = capture->jitterX;
                frame.jitterY = capture->jitterY;
                frame.projection22 = capture->projection22;
                frame.projection23 = capture->projection23;
                frame.projection32 = capture->projection32;
                frame.zFrontBackValueY = capture->zFrontBackValueY;
                std::copy(capture->maxViewDepthParams.begin(), capture->maxViewDepthParams.end(),
                    frame.maxViewDepthParams);
                std::copy(capture->clipXYZToViewPos.begin(), capture->clipXYZToViewPos.end(),
                    frame.clipXYZToViewPos);
                std::copy(capture->viewToWorld.begin(), capture->viewToWorld.end(), frame.viewToWorld);
                std::copy(capture->worldViewProjPrevFrame.begin(),
                    capture->worldViewProjPrevFrame.end(), frame.worldViewProjPrevFrame);
                frame.presentationMode = presentationMode;
                frame.depthInverted = g_depthInverted ? 1u : 0u;
                frame.jitterScale = jitterScale;
                frame.jitterSignX = jitterSignX;
                frame.jitterSignY = jitterSignY;
                frame.phaseJitterX = capture->jitterX;
                frame.phaseJitterY = capture->jitterY;
                frame.jitterPhaseOffset = jitterPhaseOffset;
                frame.jitterCycleVerified = jitterCycleVerified ? 1u : 0u;
                frame.resetHistory = resetHistory ? 1u : 0u;
                const int32_t bridgeResult = g_bridgeEvaluate(&frame);
                const uint64_t bridgeFrame = g_bridgeFrameCount.fetch_add(1) + 1;
                const int32_t previous = g_lastBridgeResult.exchange(bridgeResult);
                if (bridgeFrame <= 8 || bridgeFrame % 300 == 0 || bridgeResult != previous ||
                    resetHistory)
                {
                    std::ostringstream bridgeLine;
                    bridgeLine << "DLAA_BRIDGE_RESULT frame=" << bridgeFrame << " sequence="
                               << capture->sequence << " result=" << bridgeResult
                               << " mode=" << PresentationModeName(presentationMode)
                               << " jitterScale=" << jitterScale << " jitterSigns=("
                               << jitterSignX << ',' << jitterSignY << ") phaseOffset="
                               << jitterPhaseOffset << " cycle8=" << (jitterCycleVerified ? 1 : 0)
                               << " reset=" << (resetHistory ? 1 : 0);
                    WriteLog(bridgeLine.str());
                }
                if (bridgeResult != ACO_DLAA_OK &&
                    g_presentationMode.exchange(ACO_DLAA_PRESENT_GAME_TAA) !=
                        ACO_DLAA_PRESENT_GAME_TAA)
                    WriteLog("DLAA_PRESENTATION_DISABLED bridge failure; game TAA retained");
            }
        }
        unknown->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookFinishCommandList(ID3D11DeviceContext* self, BOOL restoreDeferredContextState,
    ID3D11CommandList** commandList)
{
    ContextHooks* hooks = FindContextHooks(self);
    const HRESULT result = hooks && hooks->finishCommandList
        ? hooks->finishCommandList(self, restoreDeferredContextState, commandList) : E_FAIL;
    const auto pending = g_pendingFrameCaptures.find(self);
    if (pending != g_pendingFrameCaptures.end())
    {
        FrameCapture* capture = pending->second;
        if (SUCCEEDED(result) && commandList && *commandList)
        {
            const HRESULT attach = (*commandList)->SetPrivateDataInterface(kCommandCaptureGuid, capture);
            if (capture->sequence <= 8 || capture->sequence % 300 == 0 || FAILED(attach))
            {
                std::ostringstream line;
                line << "TAA_COMMAND_FINISHED sequence=" << capture->sequence << " commandList="
                     << *commandList << " attach=0x" << std::hex << static_cast<unsigned long>(attach);
                WriteLog(line.str());
            }
        }
        capture->Release();
        g_pendingFrameCaptures.erase(pending);
    }
    return result;
}

void STDMETHODCALLTYPE HookUpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* resource,
    UINT destinationSubresource, const D3D11_BOX* destinationBox, const void* sourceData,
    UINT sourceRowPitch, UINT sourceDepthPitch)
{
    ID3D11Buffer* buffer{};
    D3D11_BUFFER_DESC desc{};
    if (resource && destinationSubresource == 0 && !destinationBox && sourceData)
        ConstantBufferInfo(resource, &buffer, &desc);
    ContextHooks* hooks = FindContextHooks(self);
    if (hooks && hooks->updateSubresource)
        hooks->updateSubresource(self, resource, destinationSubresource, destinationBox, sourceData,
            sourceRowPitch, sourceDepthPitch);
    if (buffer)
    {
        StoreConstantBufferCapture(buffer, sourceData, desc.ByteWidth);
        buffer->Release();
    }
}

bool InstallExecuteCommandListDetour(void* target)
{
    if (!target || target == reinterpret_cast<void*>(HookExecuteCommandList))
        return false;
    std::lock_guard<std::mutex> lock(g_executeDetourMutex);
    if (g_executeDetourTarget)
    {
        if (g_executeDetourTarget == target) return true;
        WriteLog("ERROR a different ExecuteCommandList implementation was observed; detour unchanged");
        return false;
    }

    MEMORY_BASIC_INFORMATION memory{};
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11 || !VirtualQuery(target, &memory, sizeof(memory)) || memory.AllocationBase != d3d11)
    {
        WriteLog("ERROR ExecuteCommandList target is not owned by d3d11.dll; detour refused");
        return false;
    }

    ExecuteCommandListFn original{};
    const MH_STATUS create = MH_CreateHook(target, reinterpret_cast<void*>(HookExecuteCommandList),
        reinterpret_cast<void**>(&original));
    if (create != MH_OK)
    {
        WriteLog("ERROR failed to create ExecuteCommandList detour status=" +
            std::to_string(static_cast<int>(create)));
        return false;
    }
    // Publish the trampoline before enabling the hook so another rendering
    // thread can never enter the detour without a callable original.
    g_executeDetourTarget = target;
    g_executeDetourOriginal = original;
    const MH_STATUS enable = MH_EnableHook(target);
    if (enable != MH_OK)
    {
        g_executeDetourTarget = nullptr;
        g_executeDetourOriginal = nullptr;
        MH_RemoveHook(target);
        WriteLog("ERROR failed to enable ExecuteCommandList detour status=" +
            std::to_string(static_cast<int>(enable)));
        return false;
    }

    std::ostringstream line;
    line << "D3D11_EXECUTE_DETOUR_ENABLED target=" << target << " trampoline="
         << reinterpret_cast<void*>(original);
    WriteLog(line.str());
    return true;
}

void HookContext(ID3D11DeviceContext* context)
{
    if (!context) return;
    void** table = *reinterpret_cast<void***>(context);
    {
        std::lock_guard<std::mutex> lock(g_hookTablesMutex);
        for (const auto& hooks : g_contextHookTables)
        {
            if (hooks->table != table) continue;

            const bool knownState =
                (table[9] == reinterpret_cast<void*>(HookPSSetShader) ||
                    table[9] == reinterpret_cast<void*>(hooks->psSetShader)) &&
                (table[12] == reinterpret_cast<void*>(HookDrawIndexed) ||
                    table[12] == reinterpret_cast<void*>(hooks->drawIndexed)) &&
                (table[13] == reinterpret_cast<void*>(HookDraw) ||
                    table[13] == reinterpret_cast<void*>(hooks->draw)) &&
                (table[14] == reinterpret_cast<void*>(HookMap) ||
                    table[14] == reinterpret_cast<void*>(hooks->map)) &&
                (table[15] == reinterpret_cast<void*>(HookUnmap) ||
                    table[15] == reinterpret_cast<void*>(hooks->unmap)) &&
                (table[20] == reinterpret_cast<void*>(HookDrawIndexedInstanced) ||
                    table[20] == reinterpret_cast<void*>(hooks->drawIndexedInstanced)) &&
                (table[21] == reinterpret_cast<void*>(HookDrawInstanced) ||
                    table[21] == reinterpret_cast<void*>(hooks->drawInstanced)) &&
                (table[38] == reinterpret_cast<void*>(HookDrawAuto) ||
                    table[38] == reinterpret_cast<void*>(hooks->drawAuto)) &&
                (table[39] == reinterpret_cast<void*>(HookDrawIndexedInstancedIndirect) ||
                    table[39] == reinterpret_cast<void*>(hooks->drawIndexedInstancedIndirect)) &&
                (table[40] == reinterpret_cast<void*>(HookDrawInstancedIndirect) ||
                    table[40] == reinterpret_cast<void*>(hooks->drawInstancedIndirect)) &&
                (table[48] == reinterpret_cast<void*>(HookUpdateSubresource) ||
                    table[48] == reinterpret_cast<void*>(hooks->updateSubresource)) &&
                (table[58] == reinterpret_cast<void*>(HookExecuteCommandList) ||
                    table[58] == reinterpret_cast<void*>(hooks->executeCommandList)) &&
                (table[114] == reinterpret_cast<void*>(HookFinishCommandList) ||
                    table[114] == reinterpret_cast<void*>(hooks->finishCommandList));
            if (!knownState)
            {
                const uint64_t unknown = g_contextHookUnknownCount.fetch_add(1) + 1;
                if (unknown <= 8 || unknown % 300 == 0)
                {
                    std::ostringstream line;
                    line << "ERROR D3D11 context vtable changed by an unknown hook; refusing to overwrite it"
                         << " count=" << unknown << " table=" << table;
                    WriteLog(line.str());
                }
                return;
            }

            bool repaired = false;
            auto restore = [&repaired](void** slot, void* hook) -> bool
            {
                if (*slot == hook) return true;
                DWORD oldProtect{};
                if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
                    return false;
                *slot = hook;
                DWORD ignored{};
                VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
                repaired = true;
                return true;
            };
            const bool repairedAll =
                restore(&table[9], reinterpret_cast<void*>(HookPSSetShader)) &&
                restore(&table[12], reinterpret_cast<void*>(HookDrawIndexed)) &&
                restore(&table[13], reinterpret_cast<void*>(HookDraw)) &&
                restore(&table[14], reinterpret_cast<void*>(HookMap)) &&
                restore(&table[15], reinterpret_cast<void*>(HookUnmap)) &&
                restore(&table[20], reinterpret_cast<void*>(HookDrawIndexedInstanced)) &&
                restore(&table[21], reinterpret_cast<void*>(HookDrawInstanced)) &&
                restore(&table[38], reinterpret_cast<void*>(HookDrawAuto)) &&
                restore(&table[39], reinterpret_cast<void*>(HookDrawIndexedInstancedIndirect)) &&
                restore(&table[40], reinterpret_cast<void*>(HookDrawInstancedIndirect)) &&
                restore(&table[48], reinterpret_cast<void*>(HookUpdateSubresource)) &&
                restore(&table[58], reinterpret_cast<void*>(HookExecuteCommandList)) &&
                restore(&table[114], reinterpret_cast<void*>(HookFinishCommandList));
            if (!repairedAll)
            {
                WriteLog("ERROR failed to repair a restored D3D11 context vtable slot");
            }
            else if (repaired)
            {
                const uint64_t repair = g_contextHookRepairCount.fetch_add(1) + 1;
                if (repair <= 8 || repair % 300 == 0)
                {
                    std::ostringstream line;
                    line << "D3D11_CONTEXT_HOOKS_REPAIRED count=" << repair
                         << " type=" << (context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
                                ? "immediate" : "deferred");
                    WriteLog(line.str());
                }
            }
            return;
        }
    }

    auto ownedHooks = std::make_unique<ContextHooks>();
    ownedHooks->table = table;
    const D3D11_DEVICE_CONTEXT_TYPE type = context->GetType();
    if (type == D3D11_DEVICE_CONTEXT_IMMEDIATE)
        InstallExecuteCommandListDetour(table[58]);
    ContextHooks* hooks = ownedHooks.get();
    {
        std::lock_guard<std::mutex> lock(g_hookTablesMutex);
        g_contextHookTables.push_back(std::move(ownedHooks));
    }
    const bool ok =
        PatchSlot(table, 9, HookPSSetShader, hooks->psSetShader) &&
        PatchSlot(table, 12, HookDrawIndexed, hooks->drawIndexed) &&
        PatchSlot(table, 13, HookDraw, hooks->draw) &&
        PatchSlot(table, 14, HookMap, hooks->map) &&
        PatchSlot(table, 15, HookUnmap, hooks->unmap) &&
        PatchSlot(table, 20, HookDrawIndexedInstanced, hooks->drawIndexedInstanced) &&
        PatchSlot(table, 21, HookDrawInstanced, hooks->drawInstanced) &&
        PatchSlot(table, 38, HookDrawAuto, hooks->drawAuto) &&
        PatchSlot(table, 39, HookDrawIndexedInstancedIndirect, hooks->drawIndexedInstancedIndirect) &&
        PatchSlot(table, 40, HookDrawInstancedIndirect, hooks->drawInstancedIndirect) &&
        PatchSlot(table, 48, HookUpdateSubresource, hooks->updateSubresource) &&
        PatchSlot(table, 58, HookExecuteCommandList, hooks->executeCommandList) &&
        PatchSlot(table, 114, HookFinishCommandList, hooks->finishCommandList);
    if (!ok)
    {
        WriteLog("ERROR failed to patch a D3D11 context vtable slot");
        return;
    }
    PublishHookTable(g_contextHookRegistry, hooks);
    WriteLog(std::string("D3D11_CONTEXT_HOOKED type=") +
        (type == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred"));
}

void HookDevice(ID3D11Device* device, ID3D11DeviceContext* suppliedContext)
{
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    bool alreadyHooked = false;
    {
        std::lock_guard<std::mutex> lock(g_hookTablesMutex);
        for (const auto& hooks : g_deviceHookTables)
            if (hooks->table == table) alreadyHooked = true;
    }
    if (!alreadyHooked)
    {
        auto ownedHooks = std::make_unique<DeviceHooks>();
        ownedHooks->table = table;
        DeviceHooks* hooks = ownedHooks.get();
        {
            std::lock_guard<std::mutex> lock(g_hookTablesMutex);
            g_deviceHookTables.push_back(std::move(ownedHooks));
        }
        const bool ok =
            PatchSlot(table, 3, HookCreateBuffer, hooks->createBuffer) &&
            PatchSlot(table, 15, HookCreatePixelShader, hooks->createPixelShader) &&
            PatchSlot(table, 27, HookCreateDeferredContext, hooks->createDeferredContext);
        if (!ok)
        {
            WriteLog("ERROR failed to patch a D3D11 device vtable slot");
            return;
        }
        PublishHookTable(g_deviceHookRegistry, hooks);
        std::ostringstream line;
        line << "D3D11_DEVICE_HOOKED device=" << device << " featureLevel=0x" << std::hex
             << static_cast<unsigned int>(device->GetFeatureLevel());
        WriteLog(line.str());
    }

    ID3D11DeviceContext* context = suppliedContext;
    if (context) context->AddRef();
    else device->GetImmediateContext(&context);
    if (context)
    {
        HookContext(context);
        context->Release();
    }
}

HRESULT WINAPI HookD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** immediateContext)
{
    if (!g_realD3D11CreateDevice) return E_FAIL;
    const HRESULT result = g_realD3D11CreateDevice(adapter, driverType, software, flags, featureLevels,
        featureLevelCount, sdkVersion, device, featureLevel, immediateContext);
    if (SUCCEEDED(result) && device && *device)
    {
        WriteLog("D3D11CreateDevice intercepted");
        HookDevice(*device, immediateContext ? *immediateContext : nullptr);
    }
    return result;
}

HRESULT WINAPI HookD3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext)
{
    if (!g_realD3D11CreateDeviceAndSwapChain) return E_FAIL;
    const HRESULT result = g_realD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags,
        featureLevels, featureLevelCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel,
        immediateContext);
    if (SUCCEEDED(result) && device && *device)
    {
        WriteLog("D3D11CreateDeviceAndSwapChain intercepted");
        HookDevice(*device, immediateContext ? *immediateContext : nullptr);
    }
    return result;
}

HMODULE RealDinput8()
{
    if (g_realDinput8) return g_realDinput8;
    std::array<wchar_t, MAX_PATH> system{};
    const UINT count = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!count || count >= system.size()) return nullptr;
    std::wstring path(system.data(), count);
    path += L"\\dinput8.dll";
    g_realDinput8 = LoadLibraryW(path.c_str());
    return g_realDinput8;
}

template <typename T>
T RealExport(const char* name)
{
    HMODULE module = RealDinput8();
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

DWORD WINAPI HookWorker(void*)
{
    g_maxTaaDrawLogs = static_cast<uint32_t>(std::clamp(
        GetPrivateProfileIntW(L"Probe", L"MaxTaaDrawLogs", 256, SiblingPath(kConfigName).c_str()),
        0u, 4096u));
    const std::wstring configPath = SiblingPath(kConfigName);
    g_dlaaEnabled = GetPrivateProfileIntW(L"DLAA", L"Enable", 0, configPath.c_str()) != 0;
    g_evaluateOnly = GetPrivateProfileIntW(L"DLAA", L"EvaluateOnly", 1, configPath.c_str()) != 0;
    g_depthInverted = GetPrivateProfileIntW(L"DLAA", L"DepthInverted", 0, configPath.c_str()) != 0;
    g_allowPresentationToggle =
        GetPrivateProfileIntW(L"DLAA", L"AllowPresentationToggle", 0, configPath.c_str()) != 0;
    // The successful Odyssey baseline is final: raw jitter, amplitude 1.0,
    // signs ++, phase 0, and no runtime calibration UI.
    const UINT configuredToggleKey = static_cast<UINT>(
        GetPrivateProfileIntW(L"DLAA", L"PresentationToggleKey", VK_F8, configPath.c_str()));
    if (configuredToggleKey >= 1 && configuredToggleKey <= 255)
        g_presentationToggleKey = configuredToggleKey;
    g_presentationMode.store(g_evaluateOnly
        ? ACO_DLAA_PRESENT_GAME_TAA : ACO_DLAA_PRESENT_DLAA);
    WriteLog(std::string("ACOdysseyDLAA probe starting expectedExeSha256=") + kExpectedExeSha256);
    const std::string actualHash = CurrentExecutableSha256();
    if (actualHash != kExpectedExeSha256)
    {
        WriteLog(std::string("RUNTIME_VERSION_CHECK failed actual=") +
            (actualHash.empty() ? "unavailable" : actualHash) + " hooks=disabled");
        return 0;
    }
    g_versionVerified.store(true);
    WriteLog(std::string("RUNTIME_VERSION_CHECK passed actual=") + actualHash);

    if (g_dlaaEnabled)
    {
        const std::wstring bridgePath = SiblingPath(kBridgeName);
        g_bridgeModule = LoadLibraryExW(bridgePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (g_bridgeModule)
        {
            g_bridgeEvaluate = reinterpret_cast<AcoDlaaEvaluateFn>(
                GetProcAddress(g_bridgeModule, "ACO_DLAA_Evaluate"));
        }
        if (!g_bridgeEvaluate)
        {
            std::ostringstream line;
            line << "DLAA_BRIDGE_LOAD failed win32=" << GetLastError()
                 << " originalTaaRemainsActive=1";
            WriteLog(line.str());
            g_bridgeModule = nullptr;
        }
        else
        {
            WriteLog(std::string("DLAA_BRIDGE_LOAD passed evaluateOnly=") +
                (g_evaluateOnly ? "1" : "0") + " depthInverted=" +
                 (g_depthInverted ? "1" : "0") + " allowPresentationToggle=" +
                 (g_allowPresentationToggle ? "1" : "0") + " toggleKey=" +
                 std::to_string(g_presentationToggleKey));
            WriteLog("RUNTIME_CONTROLS gui=0 hotkeys=F8-only fixedJitter=raw amplitude=1 signs=(1,1) phase=0");
        }
    }
    else
    {
        WriteLog("DLAA_BRIDGE_LOAD skipped Enable=0");
    }

    if (MH_Initialize() != MH_OK)
    {
        WriteLog("ERROR MH_Initialize failed");
        return 0;
    }
    HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d11)
    {
        WriteLog("ERROR could not load system d3d11.dll");
        return 0;
    }

    void* createDevice = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    void* createDeviceAndSwapChain =
        reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!createDevice || MH_CreateHook(createDevice, HookD3D11CreateDevice,
            reinterpret_cast<void**>(&g_realD3D11CreateDevice)) != MH_OK)
    {
        WriteLog("ERROR failed to create D3D11CreateDevice hook");
        return 0;
    }
    if (createDeviceAndSwapChain && MH_CreateHook(createDeviceAndSwapChain,
            HookD3D11CreateDeviceAndSwapChain,
            reinterpret_cast<void**>(&g_realD3D11CreateDeviceAndSwapChain)) != MH_OK)
    {
        WriteLog("ERROR failed to create D3D11CreateDeviceAndSwapChain hook");
        return 0;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        WriteLog("ERROR MH_EnableHook failed");
        return 0;
    }
    WriteLog("D3D11 creation hooks enabled; probe is passive and original draws remain active");
    return 0;
}

void StartWorker()
{
    bool expected = false;
    if (!g_workerStarted.compare_exchange_strong(expected, true))
        return;
    HANDLE thread = CreateThread(nullptr, 0, HookWorker, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid,
    LPVOID* output, LPUNKNOWN outer)
{
    StartWorker();
    auto function = RealExport<DirectInput8CreateFn>("DirectInput8Create");
    return function ? function(instance, version, riid, output, outer) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
    auto function = RealExport<DllCanUnloadNowFn>("DllCanUnloadNow");
    return function ? function() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* output)
{
    auto function = RealExport<DllGetClassObjectFn>("DllGetClassObject");
    return function ? function(clsid, riid, output) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer()
{
    auto function = RealExport<DllRegisterServerFn>("DllRegisterServer");
    return function ? function() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DllUnregisterServer()
{
    auto function = RealExport<DllRegisterServerFn>("DllUnregisterServer");
    return function ? function() : E_NOTIMPL;
}

extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystick()
{
    auto function = RealExport<GetdfDIJoystickFn>("GetdfDIJoystick");
    return function ? function() : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        StartWorker();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_log != INVALID_HANDLE_VALUE)
            CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        g_realDinput8 = nullptr;
    }
    return TRUE;
}
