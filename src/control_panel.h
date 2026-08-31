#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

struct DlssNrControlSnapshot
{
    bool enabled{};
    int preset{};
    int style{};
    float intensity{1.0f};
    float localTone{1.0f};
    float localStructure{1.0f};
    float skinStructure{-1.0f};
    bool autoMask{true};
    bool uiCorrection{true};
    int depthConvention{};
    float motionScaleX{1.0f};
    float motionScaleY{1.0f};
    bool controlCompatibleColor{};
    float scenePaperWhiteScale{1.0f};
    float hdrTransferStrength{1.0f};
    float colorStrength{1.0f};
    uint64_t resetGeneration{};
    uint64_t retryGeneration{};
};

void ControlPanelInitialize(HMODULE module, const std::wstring& configPath);
void ControlPanelShutdown();
void ControlPanelToggle();
bool ControlPanelIsVisible();
DlssNrControlSnapshot ControlPanelSnapshot();
void ControlPanelUpdateStatus(const std::wstring& status);
