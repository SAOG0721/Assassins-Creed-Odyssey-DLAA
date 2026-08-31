#pragma once

#include <d3d11.h>
#include <d3d11_4.h>

#include <cstdint>
#include <memory>
#include <string>

using DlssNrLogFn = void (*)(const std::string& message);

struct DlssNrD3D12Settings
{
    int preset{};
    int style{};
    float intensity{1.0f};
    float localTone{1.0f};
    float localStructure{1.0f};
    float skinStructure{-1.0f};
    bool autoMask{true};
    bool uiCorrection{true};
    bool depthInverted{};
    float motionScaleX{-1.0f};
    float motionScaleY{-1.0f};
};

enum class DlssNrSubmitState : uint32_t
{
    Success,
    Busy,
    Failure,
};

struct DlssNrSubmitResult
{
    DlssNrSubmitState state{DlssNrSubmitState::Failure};
    int32_t ngxResult{};
    std::string message;
};

struct DlssNrD3D12Status
{
    bool failureLatched{};
    uint64_t evaluatedFrames{};
    uint64_t lastSubmittedFence{};
    int32_t lastNgxResult{};
    std::string message;
};

class DlssNrD3D12Bridge
{
public:
    DlssNrD3D12Bridge();
    ~DlssNrD3D12Bridge();
    DlssNrD3D12Bridge(const DlssNrD3D12Bridge&) = delete;
    DlssNrD3D12Bridge& operator=(const DlssNrD3D12Bridge&) = delete;

    bool Prepare(ID3D11Device* device, ID3D11DeviceContext* context,
        uint32_t width, uint32_t height, const std::wstring& moduleDirectory,
        DlssNrLogFn logCallback);
    void Retry(uint64_t generation);
    DlssNrSubmitResult Submit(ID3D11DeviceContext* context,
        const DlssNrD3D12Settings& settings, bool resetHistory);
    void Shutdown();
    void MarkProcessDetaching();

    ID3D11UnorderedAccessView* ColorProxyUav() const;
    ID3D11UnorderedAccessView* GuidePackUav() const;
    ID3D11ShaderResourceView* ColorProxySrv() const;
    ID3D11ShaderResourceView* RawOutputSrv() const;
    DlssNrD3D12Status Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
