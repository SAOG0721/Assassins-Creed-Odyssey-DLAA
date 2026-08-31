#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#include "bridge_api.h"
#include "dlssnr_d3d12_bridge.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr char kProjectId[] = "144e1375-1890-4de7-9258-9f14262d79a7";
constexpr char kEngineVersion[] = "Anvil-ACOdyssey-Steam-17083392";
constexpr wchar_t kMotionShaderName[] = L"ACOdysseyDLAA.motion_decode.cso";
constexpr wchar_t kSharpenShaderName[] = L"ACOdysseyDLAA.sharpen.cso";
constexpr wchar_t kDlssNrEncodeShaderName[] = L"ACOdysseyDLSSNR.encode.cso";
constexpr wchar_t kDlssNrDecodeShaderName[] = L"ACOdysseyDLSSNR.decode.cso";
constexpr wchar_t kLogName[] = L"ACOdysseyDLSSBridge.log";
// Runtime phase diagnosis baseline: Odyssey's static packed motion is much
// smaller than its jitter deltas, so let NGX consume those vectors directly.
constexpr bool kExperimentalMvJittered = false;
constexpr float kSharpenStrength = 0.2f;
constexpr float kSharpenSoftThreshold = 1.0f / 255.0f;
constexpr uint32_t kDlssNrWarmupFrames = 8;

HMODULE g_module{};
HANDLE g_log = INVALID_HANDLE_VALUE;
std::mutex g_logMutex;
std::mutex g_evaluateMutex;

std::wstring ModuleDirectory()
{
    std::array<wchar_t, 32768> path{};
    const DWORD count = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return L".";
    std::wstring result(path.data(), count);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

std::wstring SiblingPath(const wchar_t* name)
{
    return ModuleDirectory() + L"\\" + name;
}

void Log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log == INVALID_HANDLE_VALUE)
    {
        g_log = CreateFileW(SiblingPath(kLogName).c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_log == INVALID_HANDLE_VALUE) return;
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

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature source)
{
    std::ostringstream line;
    line << "NGX level=" << static_cast<unsigned int>(level)
         << " source=" << static_cast<unsigned int>(source) << ' ' << (message ? message : "");
    Log(line.str());
}

std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
        nullptr, nullptr);
    return result;
}

std::string FileVersion(const wchar_t* path)
{
    DWORD ignored{};
    const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    if (!bytes) return "unavailable";
    std::vector<uint8_t> data(bytes);
    if (!GetFileVersionInfoW(path, 0, bytes, data.data())) return "unavailable";
    VS_FIXEDFILEINFO* info{};
    UINT infoSize{};
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        !info || infoSize < sizeof(VS_FIXEDFILEINFO))
        return "unavailable";
    std::ostringstream version;
    version << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS) << '.'
            << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    return version.str();
}

void LogLoadedNgxModules()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            std::wstring path = entry.szExePath;
            std::wstring lowered = path;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
            if (lowered.find(L"nvngx") != std::wstring::npos ||
                lowered.find(L"\\nvidia\\ngx\\") != std::wstring::npos ||
                lowered.find(L"dlss") != std::wstring::npos)
            {
                Log("NGX_LOADED_MODULE name=" + Utf8(entry.szModule) + " version=" +
                    FileVersion(entry.szExePath) + " path=" + Utf8(path));
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
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

bool TextureMatches(ID3D11Resource* resource, UINT width, UINT height, DXGI_FORMAT expectedFormat)
{
    if (!resource) return false;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Width == width && desc.Height == height && desc.Format == expectedFormat &&
        desc.ArraySize == 1 && desc.MipLevels == 1;
}

class ContextStateGuard
{
public:
    ContextStateGuard(ID3D11DeviceContext1* context, ID3DDeviceContextState* isolated)
        : context_(context)
    {
        if (context_ && isolated)
        {
            context_->SwapDeviceContextState(isolated, &previous_);
            active_ = previous_ != nullptr;
        }
    }

    ContextStateGuard(const ContextStateGuard&) = delete;
    ContextStateGuard& operator=(const ContextStateGuard&) = delete;

    ~ContextStateGuard()
    {
        if (!active_) return;
        ID3DDeviceContextState* discarded{};
        context_->SwapDeviceContextState(previous_, &discarded);
        if (discarded) discarded->Release();
        previous_->Release();
    }

    explicit operator bool() const { return active_; }

private:
    ID3D11DeviceContext1* context_{};
    ID3DDeviceContextState* previous_{};
    bool active_{};
};

struct alignas(16) MotionConstants
{
    float fullResSize[2];
    float fullResSizeInv[2];
    float zFrontBackValueY;
    float padding[3];
    float maxViewDepthParams[4];
    float clipXYZToViewPos[16];
    float viewToWorld[16];
    float worldViewProjPrevFrame[16];
    float projection22;
    float projection23;
    float projection32;
    float projectionPadding;
};

static_assert(sizeof(MotionConstants) == 256);

class Bridge
{
public:
    void ShutdownDlssNr()
    {
        dlssNr_.Shutdown();
    }

    void MarkProcessDetaching()
    {
        dlssNr_.MarkProcessDetaching();
    }

    int32_t Evaluate(const ACO_DLAA_Frame& frame)
    {
        const auto finiteRange = [](const float* values, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
                if (!std::isfinite(values[i])) return false;
            return true;
        };
        const float farThresholdDenominator =
            frame.maxViewDepthParams[2] - frame.maxViewDepthParams[3];
        if (!std::isfinite(frame.projection22) || !std::isfinite(frame.projection23) ||
            !std::isfinite(frame.projection32) || std::abs(frame.projection23) < 1e-8f ||
            !std::isfinite(frame.zFrontBackValueY) ||
            !finiteRange(frame.maxViewDepthParams, std::size(frame.maxViewDepthParams)) ||
            !finiteRange(frame.clipXYZToViewPos, std::size(frame.clipXYZToViewPos)) ||
            !finiteRange(frame.viewToWorld, std::size(frame.viewToWorld)) ||
            !finiteRange(frame.worldViewProjPrevFrame, std::size(frame.worldViewProjPrevFrame)) ||
            std::abs(farThresholdDenominator) < 1e-8f ||
            frame.depthInverted != 0 || frame.presentationMode > ACO_DLAA_PRESENT_RAW_COLOR ||
            !std::isfinite(frame.jitterScale) || frame.jitterScale < 0.0f ||
            frame.jitterScale > 1.0f ||
            !std::isfinite(frame.phaseJitterX) || !std::isfinite(frame.phaseJitterY) ||
            frame.jitterPhaseOffset < -2 || frame.jitterPhaseOffset > 2 ||
            (frame.jitterSignX != -1 && frame.jitterSignX != 1) ||
            (frame.jitterSignY != -1 && frame.jitterSignY != 1) ||
            (frame.dlssNrEnabled != 0 && (frame.dlssNrPreset < 0 || frame.dlssNrPreset > 3 ||
            frame.dlssNrStyle < 0 || frame.dlssNrStyle > 2 ||
            !std::isfinite(frame.dlssNrIntensity) || frame.dlssNrIntensity < 0.0f ||
            frame.dlssNrIntensity > 2.0f ||
            !std::isfinite(frame.dlssNrLocalTone) || frame.dlssNrLocalTone < 0.0f ||
            frame.dlssNrLocalTone > 2.0f ||
            !std::isfinite(frame.dlssNrLocalStructure) ||
            frame.dlssNrLocalStructure < 0.0f || frame.dlssNrLocalStructure > 2.0f ||
            !std::isfinite(frame.dlssNrSkinStructure) ||
            frame.dlssNrSkinStructure < -1.0f || frame.dlssNrSkinStructure > 2.0f ||
            frame.dlssNrDepthConvention < 0 || frame.dlssNrDepthConvention > 2 ||
            !std::isfinite(frame.dlssNrMotionScaleX) || frame.dlssNrMotionScaleX < -2.0f ||
            frame.dlssNrMotionScaleX > 2.0f ||
            !std::isfinite(frame.dlssNrMotionScaleY) || frame.dlssNrMotionScaleY < -2.0f ||
            frame.dlssNrMotionScaleY > 2.0f ||
            !std::isfinite(frame.dlssNrScenePaperWhiteScale) ||
            frame.dlssNrScenePaperWhiteScale < 0.05f ||
            frame.dlssNrScenePaperWhiteScale > 16.0f ||
            !std::isfinite(frame.dlssNrHdrTransferStrength) ||
            frame.dlssNrHdrTransferStrength < 0.0f ||
            frame.dlssNrHdrTransferStrength > 1.0f ||
            !std::isfinite(frame.dlssNrColorStrength) ||
            frame.dlssNrColorStrength < 0.0f || frame.dlssNrColorStrength > 1.0f)))
        {
            Log("ERROR invalid Odyssey depth/motion reconstruction constants; evaluation refused");
            return ACO_DLAA_BAD_ARGUMENT;
        }
        ComPtr<ID3D11Device> frameDevice;
        frame.context->GetDevice(&frameDevice);
        if (!frameDevice) return ACO_DLAA_BAD_ARGUMENT;
        if (device_ && device_.Get() != frameDevice.Get())
        {
            Log("ERROR D3D11 device changed; refusing cross-device evaluation");
            return ACO_DLAA_BAD_ARGUMENT;
        }
        if (!device_)
        {
            const int32_t result = Initialize(frameDevice.Get(), frame.context);
            if (result != ACO_DLAA_OK) return result;
        }
        if (frame.width != width_ || frame.height != height_ || frame.depthInverted != depthInverted_)
        {
            const int32_t result = RecreateFeature(frame.context, frame.width, frame.height,
                frame.depthInverted != 0);
            if (result != ACO_DLAA_OK) return result;
        }
        if (!TextureMatches(frame.color, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM) ||
            !TextureMatches(frame.packedMotion, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM) ||
            !TextureMatches(frame.depth, width_, height_, DXGI_FORMAT_R32_FLOAT) ||
            !TextureMatches(frame.gameOutput, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM))
        {
            Log("ERROR frame resource format or dimensions changed");
            return ACO_DLAA_BAD_ARGUMENT;
        }

        ContextStateGuard state(context1_.Get(), isolatedState_.Get());
        if (!state)
        {
            Log("ERROR SwapDeviceContextState did not return a previous state");
            return ACO_DLAA_STATE_ISOLATION_FAILED;
        }

        ComPtr<ID3D11ShaderResourceView> motionInput;
        HRESULT srvResult = device_->CreateShaderResourceView(frame.packedMotion, nullptr, &motionInput);
        if (FAILED(srvResult) || !motionInput)
        {
            Log("ERROR CreateShaderResourceView(packed motion) failed hr=0x" + Hex(srvResult));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        ComPtr<ID3D11ShaderResourceView> depthInput;
        srvResult = device_->CreateShaderResourceView(frame.depth, nullptr, &depthInput);
        if (FAILED(srvResult) || !depthInput)
        {
            Log("ERROR CreateShaderResourceView(linear depth) failed hr=0x" + Hex(srvResult));
            return ACO_DLAA_RESOURCE_FAILED;
        }

        MotionConstants motionConstants{};
        motionConstants.fullResSize[0] = static_cast<float>(width_);
        motionConstants.fullResSize[1] = static_cast<float>(height_);
        motionConstants.fullResSizeInv[0] = 1.0f / static_cast<float>(width_);
        motionConstants.fullResSizeInv[1] = 1.0f / static_cast<float>(height_);
        motionConstants.zFrontBackValueY = frame.zFrontBackValueY;
        std::copy_n(frame.maxViewDepthParams, std::size(frame.maxViewDepthParams),
            motionConstants.maxViewDepthParams);
        std::copy_n(frame.clipXYZToViewPos, std::size(frame.clipXYZToViewPos),
            motionConstants.clipXYZToViewPos);
        std::copy_n(frame.viewToWorld, std::size(frame.viewToWorld), motionConstants.viewToWorld);
        std::copy_n(frame.worldViewProjPrevFrame, std::size(frame.worldViewProjPrevFrame),
            motionConstants.worldViewProjPrevFrame);
        motionConstants.projection22 = frame.projection22;
        motionConstants.projection23 = frame.projection23;
        motionConstants.projection32 = frame.projection32;
        context1_->UpdateSubresource(motionConstants_.Get(), 0, nullptr, &motionConstants, 0, 0);

        ID3D11ShaderResourceView* srvs[] = {motionInput.Get(), depthInput.Get()};
        ID3D11UnorderedAccessView* uavs[] = {decodedMotionUav_.Get(), deviceDepthUav_.Get()};
        ID3D11Buffer* motionBuffers[] = {motionConstants_.Get()};
        context1_->CSSetShader(motionDecode_.Get(), nullptr, 0);
        context1_->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
        context1_->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(uavs)), uavs, nullptr);
        context1_->CSSetConstantBuffers(0, 1, motionBuffers);
        context1_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrvs[] = {nullptr, nullptr};
        ID3D11UnorderedAccessView* nullUavs[] = {nullptr, nullptr};
        ID3D11Buffer* nullBuffer[] = {nullptr};
        context1_->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSrvs)), nullSrvs);
        context1_->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(nullUavs)), nullUavs, nullptr);
        context1_->CSSetConstantBuffers(0, 1, nullBuffer);

        NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
        eval.Feature.pInColor = frame.color;
        eval.Feature.pInOutput = output_.Get();
        eval.Feature.InSharpness = 0.0f;
        eval.pInDepth = deviceDepth_.Get();
        eval.pInMotionVectors = decodedMotion_.Get();
        // Odyssey's resolve kernel samples current color at this signed pixel
        // phase; it matches NGX's render-pixel jitter convention directly.
        const float submittedJitterX =
            frame.phaseJitterX * frame.jitterScale * static_cast<float>(frame.jitterSignX);
        const float submittedJitterY =
            frame.phaseJitterY * frame.jitterScale * static_cast<float>(frame.jitterSignY);
        eval.InJitterOffsetX = submittedJitterX;
        eval.InJitterOffsetY = submittedJitterY;
        eval.InRenderSubrectDimensions = {width_, height_};
        const int resetThisFrame = (reset_ || frame.resetHistory != 0) ? 1 : 0;
        eval.InReset = resetThisFrame;
        // Odyssey's TAA resolves history at previousUV = currentUV - decodedMotion,
        // so the decoded value is currentUV - previousUV (previous position ->
        // current position). Direct NGX consumes current -> previous pixel motion;
        // the negative scale performs that direction and unit conversion.
        eval.InMVScaleX = -static_cast<float>(width_);
        eval.InMVScaleY = -static_cast<float>(height_);
        eval.InPreExposure = 1.0f;
        eval.InExposureScale = 1.0f;

        const NVSDK_NGX_Result ngxResult =
            NGX_D3D11_EVALUATE_DLSS_EXT(context1_.Get(), feature_, parameters_, &eval);
        if (NVSDK_NGX_FAILED(ngxResult))
        {
            Log("ERROR NGX evaluate failed result=0x" + Hex(ngxResult));
            return ACO_DLAA_NGX_EVALUATE_FAILED;
        }
        reset_ = false;
        ++evaluatedFrames_;

        bool publishDlssNr = false;
        const bool dlssNrEnabled = frame.dlssNrEnabled != 0;
        if (dlssNrEnabled != dlssNrWasEnabled_)
        {
            dlssNrWasEnabled_ = dlssNrEnabled;
            dlssNrWarmupFrames_ = 0;
            dlssNrResetPending_ = true;
        }
        if (frame.dlssNrResetGeneration != lastDlssNrResetGeneration_)
        {
            lastDlssNrResetGeneration_ = frame.dlssNrResetGeneration;
            dlssNrWarmupFrames_ = 0;
            dlssNrResetPending_ = true;
        }
        if (frame.dlssNrRetryGeneration != lastDlssNrRetryGeneration_)
        {
            lastDlssNrRetryGeneration_ = frame.dlssNrRetryGeneration;
            dlssNr_.Retry(frame.dlssNrRetryGeneration);
            dlssNrWarmupFrames_ = 0;
            dlssNrResetPending_ = true;
        }
        if (dlssNrEnabled)
        {
            if (dlssNr_.Prepare(device_.Get(), frame.context, width_, height_, ModuleDirectory(),
                    &Log))
            {
                EncodeDlssNrInputs(frame);
                DlssNrD3D12Settings settings{};
                settings.preset = frame.dlssNrPreset;
                settings.style = frame.dlssNrStyle;
                settings.intensity = frame.dlssNrIntensity;
                settings.localTone = frame.dlssNrLocalTone;
                settings.localStructure = frame.dlssNrLocalStructure;
                settings.skinStructure = frame.dlssNrSkinStructure;
                settings.autoMask = frame.dlssNrAutoMask != 0;
                settings.uiCorrection = frame.dlssNrUiCorrection != 0;
                settings.depthInverted = frame.dlssNrDepthConvention == 2 ||
                    (frame.dlssNrDepthConvention == 0 && frame.depthInverted != 0);
                settings.motionScaleX = -static_cast<float>(width_) * frame.dlssNrMotionScaleX;
                settings.motionScaleY = -static_cast<float>(height_) * frame.dlssNrMotionScaleY;
                const DlssNrSubmitResult nrResult =
                    dlssNr_.Submit(frame.context, settings, dlssNrResetPending_);
                if (nrResult.state == DlssNrSubmitState::Success)
                {
                    DecodeDlssNrOutput(frame);
                    dlssNrResetPending_ = false;
                    const bool warmupCompleteBeforeFrame =
                        dlssNrWarmupFrames_ >= kDlssNrWarmupFrames;
                    if (dlssNrWarmupFrames_ < kDlssNrWarmupFrames)
                        ++dlssNrWarmupFrames_;
                    publishDlssNr = warmupCompleteBeforeFrame;
                    dlssNrState_ = publishDlssNr ? ACO_DLSSNR_ACTIVE : ACO_DLSSNR_WARMING_UP;
                    dlssNrMessage_ = publishDlssNr ? nrResult.message :
                        "DLSSNR private warmup " + std::to_string(dlssNrWarmupFrames_) + "/" +
                        std::to_string(kDlssNrWarmupFrames);
                    const DlssNrD3D12Status status = dlssNr_.Status();
                    if (status.evaluatedFrames <= 8 || status.evaluatedFrames % 300 == 0)
                    {
                        std::ostringstream nrLine;
                        nrLine << "DLSSNR_EVALUATED frame=" << status.evaluatedFrames
                               << " preset=" << settings.preset << " style=" << settings.style
                               << " intensity=" << settings.intensity << " localTone="
                               << settings.localTone << " localStructure="
                               << settings.localStructure << " skinStructure="
                               << settings.skinStructure << " autoMask="
                               << (settings.autoMask ? 1 : 0) << " uiCorrection="
                               << (settings.uiCorrection ? 1 : 0) << " depthInverted="
                               << (settings.depthInverted ? 1 : 0) << " mvScale=("
                               << settings.motionScaleX << ',' << settings.motionScaleY
                               << ") warmup=" << dlssNrWarmupFrames_ << '/'
                               << kDlssNrWarmupFrames << " publish="
                               << (publishDlssNr ? 1 : 0) << " fence="
                               << status.lastSubmittedFence;
                        Log(nrLine.str());
                    }
                }
                else
                {
                    dlssNrResetPending_ = true;
                    dlssNrWarmupFrames_ = 0;
                    publishDlssNr = false;
                    const DlssNrD3D12Status status = dlssNr_.Status();
                    const uint32_t newState = nrResult.state == DlssNrSubmitState::Busy ?
                        ACO_DLSSNR_BUSY_FALLBACK : ACO_DLSSNR_FAILURE_LATCHED;
                    if (newState != dlssNrState_ || nrResult.message != dlssNrMessage_)
                        Log("DLSSNR_FALLBACK " + nrResult.message);
                    dlssNrState_ = newState;
                    dlssNrMessage_ = status.message.empty() ? nrResult.message : status.message;
                }
            }
            else
            {
                const DlssNrD3D12Status status = dlssNr_.Status();
                dlssNrResetPending_ = true;
                dlssNrWarmupFrames_ = 0;
                publishDlssNr = false;
                const uint32_t newState = status.failureLatched ?
                    ACO_DLSSNR_FAILURE_LATCHED : ACO_DLSSNR_BUSY_FALLBACK;
                if (newState != dlssNrState_ || status.message != dlssNrMessage_)
                    Log("DLSSNR_PREP_FALLBACK " + status.message);
                dlssNrState_ = newState;
                dlssNrMessage_ = status.message;
            }
        }
        else
        {
            dlssNrState_ = ACO_DLSSNR_DISABLED;
            dlssNrMessage_ = "DLSSNR disabled; DLAA remains active";
        }

        if (frame.presentationMode == ACO_DLAA_PRESENT_DLAA)
        {
            ApplySharpen(publishDlssNr ? dlssNrDecodedSrv_.Get() : outputSrv_.Get());
            context1_->CopyResource(frame.gameOutput, sharpened_.Get());
        }
        else if (frame.presentationMode == ACO_DLAA_PRESENT_RAW_COLOR &&
            frame.gameOutput != frame.color)
            context1_->CopyResource(frame.gameOutput, frame.color);

        if (evaluatedFrames_ <= 8 || evaluatedFrames_ % 300 == 0 || resetThisFrame)
        {
            std::ostringstream line;
            line << "DLAA_EVALUATED frame=" << evaluatedFrames_ << " size=" << width_ << 'x' << height_
                 << " jitterRaw=(" << frame.jitterX << ',' << frame.jitterY << ") jitterPhase=("
                 << frame.phaseJitterX << ',' << frame.phaseJitterY << ") jitterSubmitted=("
                 << submittedJitterX << ',' << submittedJitterY << ") phaseOffset="
                 << frame.jitterPhaseOffset << " cycle8=" << frame.jitterCycleVerified << " reset="
                 << resetThisFrame << " mvScale=(" << eval.InMVScaleX << ',' << eval.InMVScaleY
                 << ") farThreshold=" <<
                    (frame.maxViewDepthParams[0] - frame.maxViewDepthParams[1]) /
                    farThresholdDenominator
                 << ") mode=" << frame.presentationMode << " jitterScale=" << frame.jitterScale
                 << " sharpness=" << kSharpenStrength << " sharpnessMethod=gaussian-usm-luma"
                 << " jitterSigns=(" << frame.jitterSignX << ',' << frame.jitterSignY << ')'
                 << " dlssNrEnabled=" << (dlssNrEnabled ? 1 : 0)
                 << " dlssNrPublish=" << (publishDlssNr ? 1 : 0)
                 << " ngx=0x" << std::hex << static_cast<uint32_t>(ngxResult);
            Log(line.str());
        }
        return ACO_DLAA_OK;
    }

    void FillDlssNrStatus(ACO_DLSSNR_Status& output) const
    {
        const DlssNrD3D12Status status = dlssNr_.Status();
        output = {};
        output.structSize = sizeof(output);
        output.state = dlssNrState_;
        output.failureLatched = status.failureLatched ? 1u : 0u;
        output.warmupFrames = dlssNrWarmupFrames_;
        output.evaluatedFrames = status.evaluatedFrames;
        output.lastSubmittedFence = status.lastSubmittedFence;
        output.lastNgxResult = status.lastNgxResult;
        const std::string& message = dlssNrMessage_.empty() ? status.message : dlssNrMessage_;
        strncpy_s(output.message, message.c_str(), _TRUNCATE);
    }

private:
    template <typename T>
    static std::string Hex(T value)
    {
        std::ostringstream text;
        text << std::hex << std::uppercase << static_cast<uint64_t>(value);
        return text.str();
    }

    int32_t Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        device_ = device;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1_))))
        {
            Log("ERROR ID3D11DeviceContext1 is unavailable");
            return ACO_DLAA_STATE_ISOLATION_FAILED;
        }
        ComPtr<ID3D11Device1> device1;
        if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&device1))))
        {
            Log("ERROR ID3D11Device1 is unavailable");
            return ACO_DLAA_STATE_ISOLATION_FAILED;
        }
        const D3D_FEATURE_LEVEL featureLevel = device_->GetFeatureLevel();
        const UINT stateFlags = (device_->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
            ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        D3D_FEATURE_LEVEL chosen{};
        const HRESULT stateResult = device1->CreateDeviceContextState(stateFlags, &featureLevel, 1,
            D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &isolatedState_);
        if (FAILED(stateResult) || !isolatedState_)
        {
            Log("ERROR CreateDeviceContextState failed hr=0x" + Hex(stateResult));
            return ACO_DLAA_STATE_ISOLATION_FAILED;
        }

        const std::vector<uint8_t> motionShader = ReadFileBytes(SiblingPath(kMotionShaderName));
        if (motionShader.empty() || FAILED(device_->CreateComputeShader(motionShader.data(),
                motionShader.size(), nullptr,
                &motionDecode_)))
        {
            Log("ERROR motion decode shader could not be loaded");
            return ACO_DLAA_SHADER_FAILED;
        }
        const std::vector<uint8_t> sharpenShader = ReadFileBytes(SiblingPath(kSharpenShaderName));
        if (sharpenShader.empty() || FAILED(device_->CreateComputeShader(sharpenShader.data(),
                sharpenShader.size(), nullptr, &sharpen_)))
        {
            Log("ERROR sharpen shader could not be loaded");
            return ACO_DLAA_SHADER_FAILED;
        }
        const std::vector<uint8_t> nrEncodeShader =
            ReadFileBytes(SiblingPath(kDlssNrEncodeShaderName));
        if (nrEncodeShader.empty() || FAILED(device_->CreateComputeShader(nrEncodeShader.data(),
                nrEncodeShader.size(), nullptr, &dlssNrEncode_)))
        {
            Log("ERROR DLSSNR encode shader could not be loaded");
            return ACO_DLAA_SHADER_FAILED;
        }
        const std::vector<uint8_t> nrDecodeShader =
            ReadFileBytes(SiblingPath(kDlssNrDecodeShaderName));
        if (nrDecodeShader.empty() || FAILED(device_->CreateComputeShader(nrDecodeShader.data(),
                nrDecodeShader.size(), nullptr, &dlssNrDecode_)))
        {
            Log("ERROR DLSSNR decode shader could not be loaded");
            return ACO_DLAA_SHADER_FAILED;
        }
        D3D11_BUFFER_DESC constantsDesc{};
        constantsDesc.ByteWidth = sizeof(MotionConstants);
        constantsDesc.Usage = D3D11_USAGE_DEFAULT;
        constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT constantsResult = device_->CreateBuffer(&constantsDesc, nullptr, &motionConstants_);
        if (FAILED(constantsResult))
        {
            Log("ERROR motion constants buffer creation failed hr=0x" + Hex(constantsResult));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        constantsDesc.ByteWidth = 16;
        constantsResult = device_->CreateBuffer(&constantsDesc, nullptr, &sharpenConstants_);
        if (FAILED(constantsResult))
        {
            Log("ERROR sharpen constants buffer creation failed hr=0x" + Hex(constantsResult));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        constantsDesc.ByteWidth = 32;
        constantsResult = device_->CreateBuffer(&constantsDesc, nullptr, &dlssNrCodecConstants_);
        if (FAILED(constantsResult))
        {
            Log("ERROR DLSSNR codec constants buffer creation failed hr=0x" + Hex(constantsResult));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        const HRESULT samplerResult = device_->CreateSamplerState(&samplerDesc, &sharpenSampler_);
        if (FAILED(samplerResult))
        {
            Log("ERROR sharpen sampler creation failed hr=0x" + Hex(samplerResult));
            return ACO_DLAA_RESOURCE_FAILED;
        }

        std::wstring modulePath = ModuleDirectory();
        wchar_t* paths[] = {modulePath.data()};
        NVSDK_NGX_FeatureCommonInfo info{};
        info.PathListInfo.Length = 1;
        info.PathListInfo.Path = paths;
        info.LoggingInfo.LoggingCallback = NgxLog;
        info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        info.LoggingInfo.DisableOtherLoggingSinks = true;

        std::wstring appData = ModuleDirectory();
        std::array<wchar_t, 32768> localAppData{};
        const DWORD localCount = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        if (localCount && localCount < localAppData.size())
        {
            appData.assign(localAppData.data(), localCount);
            appData += L"\\ACOdysseyDLAA";
            CreateDirectoryW(appData.c_str(), nullptr);
        }

        const NVSDK_NGX_Result init = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId,
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, appData.c_str(), device_.Get(), &info,
            NVSDK_NGX_Version_API);
        Log("NGX_INIT projectId=" + std::string(kProjectId) + " result=0x" + Hex(init));
        if (NVSDK_NGX_FAILED(init)) return ACO_DLAA_NGX_INIT_FAILED;
        ngxInitialized_ = true;

        const NVSDK_NGX_Result capabilities =
            NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters_);
        if (NVSDK_NGX_FAILED(capabilities) || !parameters_)
        {
            Log("ERROR GetCapabilityParameters failed result=0x" + Hex(capabilities));
            return ACO_DLAA_NGX_INIT_FAILED;
        }
        int available{};
        int initResult{};
        const NVSDK_NGX_Result availableResult =
            parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        const NVSDK_NGX_Result featureResult =
            parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        std::ostringstream line;
        line << "NGX_CAPABILITY available=" << available << " getAvailable=0x" << std::hex
             << static_cast<uint32_t>(availableResult) << " featureInit=0x" << initResult
             << " getFeatureInit=0x" << static_cast<uint32_t>(featureResult);
        Log(line.str());
        if (NVSDK_NGX_FAILED(availableResult) || available == 0)
            return ACO_DLAA_NGX_UNAVAILABLE;
        return ACO_DLAA_OK;
    }

    void ReleaseFeatureResources()
    {
        if (feature_)
        {
            const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_ReleaseFeature(feature_);
            Log("NGX_RELEASE_FEATURE result=0x" + Hex(result));
            feature_ = nullptr;
        }
        decodedMotionUav_.Reset();
        decodedMotionSrv_.Reset();
        decodedMotion_.Reset();
        deviceDepthUav_.Reset();
        deviceDepthSrv_.Reset();
        deviceDepth_.Reset();
        outputSrv_.Reset();
        dlssNrDecodedSrv_.Reset();
        dlssNrDecodedUav_.Reset();
        dlssNrDecoded_.Reset();
        sharpenedUav_.Reset();
        sharpened_.Reset();
        output_.Reset();
        width_ = 0;
        height_ = 0;
    }

    int32_t RecreateFeature(ID3D11DeviceContext* context, UINT width, UINT height, bool depthInverted)
    {
        ReleaseFeatureResources();
        D3D11_TEXTURE2D_DESC motionDesc{};
        motionDesc.Width = width;
        motionDesc.Height = height;
        motionDesc.MipLevels = 1;
        motionDesc.ArraySize = 1;
        motionDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        motionDesc.SampleDesc.Count = 1;
        motionDesc.Usage = D3D11_USAGE_DEFAULT;
        motionDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT result = device_->CreateTexture2D(&motionDesc, nullptr, &decodedMotion_);
        if (FAILED(result))
        {
            Log("ERROR decoded motion texture creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateUnorderedAccessView(decodedMotion_.Get(), nullptr, &decodedMotionUav_);
        if (FAILED(result))
        {
            Log("ERROR decoded motion UAV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateShaderResourceView(decodedMotion_.Get(), nullptr, &decodedMotionSrv_);
        if (FAILED(result))
        {
            Log("ERROR decoded motion SRV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }

        D3D11_TEXTURE2D_DESC depthDesc = motionDesc;
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        result = device_->CreateTexture2D(&depthDesc, nullptr, &deviceDepth_);
        if (FAILED(result))
        {
            Log("ERROR device depth texture creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateUnorderedAccessView(deviceDepth_.Get(), nullptr, &deviceDepthUav_);
        if (FAILED(result))
        {
            Log("ERROR device depth UAV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateShaderResourceView(deviceDepth_.Get(), nullptr, &deviceDepthSrv_);
        if (FAILED(result))
        {
            Log("ERROR device depth SRV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }

        D3D11_TEXTURE2D_DESC outputDesc = motionDesc;
        outputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        result = device_->CreateTexture2D(&outputDesc, nullptr, &output_);
        if (FAILED(result))
        {
            Log("ERROR DLAA output texture creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateShaderResourceView(output_.Get(), nullptr, &outputSrv_);
        if (FAILED(result))
        {
            Log("ERROR DLAA output SRV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateTexture2D(&outputDesc, nullptr, &dlssNrDecoded_);
        if (FAILED(result))
        {
            Log("ERROR DLSSNR decoded texture creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateShaderResourceView(dlssNrDecoded_.Get(), nullptr,
            &dlssNrDecodedSrv_);
        if (FAILED(result))
        {
            Log("ERROR DLSSNR decoded SRV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateUnorderedAccessView(dlssNrDecoded_.Get(), nullptr,
            &dlssNrDecodedUav_);
        if (FAILED(result))
        {
            Log("ERROR DLSSNR decoded UAV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateTexture2D(&outputDesc, nullptr, &sharpened_);
        if (FAILED(result))
        {
            Log("ERROR sharpen scratch texture creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }
        result = device_->CreateUnorderedAccessView(sharpened_.Get(), nullptr, &sharpenedUav_);
        if (FAILED(result))
        {
            Log("ERROR sharpen scratch UAV creation failed hr=0x" + Hex(result));
            return ACO_DLAA_RESOURCE_FAILED;
        }

        ContextStateGuard state(context1_.Get(), isolatedState_.Get());
        if (!state) return ACO_DLAA_STATE_ISOLATION_FAILED;
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = width;
        create.Feature.InHeight = height;
        create.Feature.InTargetWidth = width;
        create.Feature.InTargetHeight = height;
        create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            (kExperimentalMvJittered ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0) |
            (depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
        create.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result createResult =
            NGX_D3D11_CREATE_DLSS_EXT(context, &feature_, parameters_, &create);
        std::ostringstream line;
        line << "NGX_CREATE_DLAA size=" << width << 'x' << height << " flags=0x" << std::hex
             << create.InFeatureCreateFlags << " result=0x" << static_cast<uint32_t>(createResult);
        Log(line.str());
        if (NVSDK_NGX_FAILED(createResult) || !feature_)
        {
            ReleaseFeatureResources();
            return ACO_DLAA_NGX_CREATE_FAILED;
        }
        LogLoadedNgxModules();
        Log("DLAA_PREP combined motion/depth dispatch=1 depthTile=groupshared-10x10");
        Log("DLAA_SHARPEN configured strength=0.2 method=gaussian-usm-4tap-luma");
        width_ = width;
        height_ = height;
        depthInverted_ = depthInverted ? 1u : 0u;
        reset_ = true;
        return ACO_DLAA_OK;
    }

    struct alignas(16) DlssNrCodecConstants
    {
        uint32_t outputSize[2];
        uint32_t controlCompatibleColor;
        uint32_t padding;
        float scenePaperWhiteScale;
        float hdrTransferStrength;
        float colorStrength;
        float padding2;
    };

    static_assert(sizeof(DlssNrCodecConstants) == 32);

    DlssNrCodecConstants MakeDlssNrCodecConstants(const ACO_DLAA_Frame& frame) const
    {
        DlssNrCodecConstants constants{};
        constants.outputSize[0] = width_;
        constants.outputSize[1] = height_;
        constants.controlCompatibleColor = frame.dlssNrControlCompatibleColor != 0 ? 1u : 0u;
        constants.scenePaperWhiteScale = frame.dlssNrScenePaperWhiteScale;
        constants.hdrTransferStrength = frame.dlssNrHdrTransferStrength;
        constants.colorStrength = frame.dlssNrColorStrength;
        return constants;
    }

    void ClearComputeBindings()
    {
        std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> srvs{};
        std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs{};
        context1_->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context1_->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
    }

    void EncodeDlssNrInputs(const ACO_DLAA_Frame& frame)
    {
        const DlssNrCodecConstants constants = MakeDlssNrCodecConstants(frame);
        context1_->UpdateSubresource(dlssNrCodecConstants_.Get(), 0, nullptr, &constants, 0, 0);
        ClearComputeBindings();
        ID3D11ShaderResourceView* srvs[] = {
            outputSrv_.Get(), decodedMotionSrv_.Get(), deviceDepthSrv_.Get()};
        ID3D11UnorderedAccessView* uavs[] = {
            dlssNr_.ColorProxyUav(), dlssNr_.GuidePackUav()};
        ID3D11Buffer* buffers[] = {dlssNrCodecConstants_.Get()};
        context1_->CSSetShader(dlssNrEncode_.Get(), nullptr, 0);
        context1_->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
        context1_->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(uavs)), uavs, nullptr);
        context1_->CSSetConstantBuffers(0, 1, buffers);
        context1_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
        ClearComputeBindings();
        ID3D11Buffer* nullBuffer[] = {nullptr};
        context1_->CSSetConstantBuffers(0, 1, nullBuffer);
    }

    void DecodeDlssNrOutput(const ACO_DLAA_Frame& frame)
    {
        const DlssNrCodecConstants constants = MakeDlssNrCodecConstants(frame);
        context1_->UpdateSubresource(dlssNrCodecConstants_.Get(), 0, nullptr, &constants, 0, 0);
        ClearComputeBindings();
        ID3D11ShaderResourceView* srvs[] = {
            outputSrv_.Get(), dlssNr_.ColorProxySrv(), dlssNr_.RawOutputSrv()};
        ID3D11UnorderedAccessView* uavs[] = {dlssNrDecodedUav_.Get()};
        ID3D11Buffer* buffers[] = {dlssNrCodecConstants_.Get()};
        context1_->CSSetShader(dlssNrDecode_.Get(), nullptr, 0);
        context1_->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
        context1_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context1_->CSSetConstantBuffers(0, 1, buffers);
        context1_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
        ClearComputeBindings();
        ID3D11Buffer* nullBuffer[] = {nullptr};
        context1_->CSSetConstantBuffers(0, 1, nullBuffer);
    }

    void ApplySharpen(ID3D11ShaderResourceView* input)
    {
        struct alignas(16) SharpenConstants
        {
            float invOutputSize[2];
            float strength;
            float softThreshold;
        } constants{{1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_)},
            kSharpenStrength, kSharpenSoftThreshold};
        context1_->UpdateSubresource(sharpenConstants_.Get(), 0, nullptr, &constants, 0, 0);

        // NGX may leave its output in a compute UAV slot. Clear all compute
        // UAVs before binding that texture as our sharpening SRV.
        std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> nullUavs{};
        context1_->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUavs.size()),
            nullUavs.data(), nullptr);
        ID3D11ShaderResourceView* source[] = {input};
        ID3D11UnorderedAccessView* destination[] = {sharpenedUav_.Get()};
        ID3D11Buffer* constantsBuffer[] = {sharpenConstants_.Get()};
        ID3D11SamplerState* sampler[] = {sharpenSampler_.Get()};
        context1_->CSSetShader(sharpen_.Get(), nullptr, 0);
        context1_->CSSetShaderResources(0, 1, source);
        context1_->CSSetUnorderedAccessViews(0, 1, destination, nullptr);
        context1_->CSSetConstantBuffers(0, 1, constantsBuffer);
        context1_->CSSetSamplers(0, 1, sampler);
        context1_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

        ID3D11ShaderResourceView* nullSrv[] = {nullptr};
        ID3D11UnorderedAccessView* nullUav[] = {nullptr};
        ID3D11Buffer* nullBuffer[] = {nullptr};
        ID3D11SamplerState* nullSampler[] = {nullptr};
        context1_->CSSetShaderResources(0, 1, nullSrv);
        context1_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context1_->CSSetConstantBuffers(0, 1, nullBuffer);
        context1_->CSSetSamplers(0, 1, nullSampler);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3DDeviceContextState> isolatedState_;
    ComPtr<ID3D11ComputeShader> motionDecode_;
    ComPtr<ID3D11ComputeShader> sharpen_;
    ComPtr<ID3D11ComputeShader> dlssNrEncode_;
    ComPtr<ID3D11ComputeShader> dlssNrDecode_;
    ComPtr<ID3D11Buffer> motionConstants_;
    ComPtr<ID3D11Buffer> sharpenConstants_;
    ComPtr<ID3D11Buffer> dlssNrCodecConstants_;
    ComPtr<ID3D11SamplerState> sharpenSampler_;
    ComPtr<ID3D11Texture2D> decodedMotion_;
    ComPtr<ID3D11UnorderedAccessView> decodedMotionUav_;
    ComPtr<ID3D11ShaderResourceView> decodedMotionSrv_;
    ComPtr<ID3D11Texture2D> deviceDepth_;
    ComPtr<ID3D11UnorderedAccessView> deviceDepthUav_;
    ComPtr<ID3D11ShaderResourceView> deviceDepthSrv_;
    ComPtr<ID3D11Texture2D> output_;
    ComPtr<ID3D11ShaderResourceView> outputSrv_;
    ComPtr<ID3D11Texture2D> dlssNrDecoded_;
    ComPtr<ID3D11ShaderResourceView> dlssNrDecodedSrv_;
    ComPtr<ID3D11UnorderedAccessView> dlssNrDecodedUav_;
    ComPtr<ID3D11Texture2D> sharpened_;
    ComPtr<ID3D11UnorderedAccessView> sharpenedUav_;
    NVSDK_NGX_Parameter* parameters_{};
    NVSDK_NGX_Handle* feature_{};
    UINT width_{};
    UINT height_{};
    uint32_t depthInverted_{};
    uint64_t evaluatedFrames_{};
    bool ngxInitialized_{};
    bool reset_{true};
    DlssNrD3D12Bridge dlssNr_;
    uint32_t dlssNrState_{ACO_DLSSNR_DISABLED};
    uint32_t dlssNrWarmupFrames_{};
    uint64_t lastDlssNrResetGeneration_{};
    uint64_t lastDlssNrRetryGeneration_{};
    bool dlssNrWasEnabled_{};
    bool dlssNrResetPending_{true};
    std::string dlssNrMessage_{"DLSSNR disabled; DLAA remains active"};
};

Bridge g_bridge;
}

extern "C" __declspec(dllexport) int32_t WINAPI
ACO_DLAA_Evaluate(const ACO_DLAA_Frame* frame)
{
    if (!frame || frame->structSize != sizeof(ACO_DLAA_Frame) || !frame->context || !frame->color ||
        !frame->packedMotion || !frame->depth || !frame->gameOutput || !frame->width || !frame->height)
        return ACO_DLAA_BAD_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_evaluateMutex);
    return g_bridge.Evaluate(*frame);
}

extern "C" __declspec(dllexport) int32_t WINAPI
ACO_DLSSNR_GetStatus(ACO_DLSSNR_Status* status)
{
    if (!status || status->structSize != sizeof(ACO_DLSSNR_Status))
        return ACO_DLAA_BAD_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_evaluateMutex);
    g_bridge.FillDlssNrStatus(*status);
    return ACO_DLAA_OK;
}

extern "C" __declspec(dllexport) int32_t WINAPI ACO_DLSSNR_Shutdown()
{
    std::lock_guard<std::mutex> lock(g_evaluateMutex);
    g_bridge.ShutdownDlssNr();
    return ACO_DLAA_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_bridge.MarkProcessDetaching();
        if (g_log != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
