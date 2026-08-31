#include "control_panel.h"

#include <CommCtrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>

namespace
{
constexpr wchar_t kWindowClass[] = L"ACOdysseyDLSSNRControlPanel";
constexpr UINT kToggleMessage = WM_APP + 0x41;
constexpr UINT kStatusMessage = WM_APP + 0x42;
constexpr UINT kQuitMessage = WM_APP + 0x43;

enum ControlId : int
{
    IdEnable = 100,
    IdPreset,
    IdStyle,
    IdIntensity,
    IdIntensityValue,
    IdLocalTone,
    IdLocalToneValue,
    IdLocalStructure,
    IdLocalStructureValue,
    IdSkinStructure,
    IdSkinStructureValue,
    IdAutoMask,
    IdUiCorrection,
    IdAdvanced,
    IdAdvancedGroup,
    IdDepth,
    IdMotionX,
    IdMotionXValue,
    IdMotionY,
    IdMotionYValue,
    IdColorTransfer,
    IdPaperWhite,
    IdPaperWhiteValue,
    IdHdrTransfer,
    IdHdrTransferValue,
    IdColorStrength,
    IdColorStrengthValue,
    IdReset,
    IdStatus,
    IdDepthLabel,
    IdMotionXLabel,
    IdMotionYLabel,
    IdPaperWhiteLabel,
    IdHdrTransferLabel,
    IdColorStrengthLabel,
};

HMODULE g_module{};
std::wstring g_configPath;
std::atomic<HWND> g_window{};
HANDLE g_thread{};
std::atomic<bool> g_initialized{};
std::atomic<bool> g_visible{};
std::atomic<bool> g_enabled{};
std::atomic<int> g_preset{};
std::atomic<int> g_style{};
std::atomic<float> g_intensity{1.0f};
std::atomic<float> g_localTone{1.0f};
std::atomic<float> g_localStructure{1.0f};
std::atomic<float> g_skinStructure{-1.0f};
std::atomic<bool> g_autoMask{true};
std::atomic<bool> g_uiCorrection{true};
std::atomic<bool> g_advanced{};
std::atomic<int> g_depthConvention{};
std::atomic<float> g_motionScaleX{1.0f};
std::atomic<float> g_motionScaleY{1.0f};
std::atomic<bool> g_colorTransfer{};
std::atomic<float> g_paperWhite{1.0f};
std::atomic<float> g_hdrTransfer{1.0f};
std::atomic<float> g_colorStrength{1.0f};
std::atomic<uint64_t> g_resetGeneration{1};
std::atomic<uint64_t> g_retryGeneration{1};
std::mutex g_statusMutex;
std::wstring g_status = L"等待首个 TAA 帧。DLSSNR 默认关闭。";

HWND Item(HWND parent, int id)
{
    return GetDlgItem(parent, id);
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback,
    float minimum, float maximum)
{
    wchar_t fallbackText[64]{};
    swprintf_s(fallbackText, L"%.6g", fallback);
    std::array<wchar_t, 128> value{};
    GetPrivateProfileStringW(section, key, fallbackText, value.data(),
        static_cast<DWORD>(value.size()), g_configPath.c_str());
    wchar_t* end{};
    const float parsed = std::wcstof(value.data(), &end);
    if (end == value.data() || !std::isfinite(parsed)) return fallback;
    return std::clamp(parsed, minimum, maximum);
}

void WriteInt(const wchar_t* section, const wchar_t* key, int value)
{
    const std::wstring text = std::to_wstring(value);
    WritePrivateProfileStringW(section, key, text.c_str(), g_configPath.c_str());
}

void WriteFloat(const wchar_t* section, const wchar_t* key, float value)
{
    wchar_t text[64]{};
    swprintf_s(text, L"%.4f", value);
    WritePrivateProfileStringW(section, key, text, g_configPath.c_str());
}

void LoadSettings()
{
    g_enabled = GetPrivateProfileIntW(L"DLSSNR", L"Enable", 0, g_configPath.c_str()) != 0;
    g_preset = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"DLSSNR", L"Preset", 0,
        g_configPath.c_str())), 0, 3);
    g_style = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"DLSSNR", L"Style", 0,
        g_configPath.c_str())), 0, 2);
    g_intensity = ReadFloat(L"DLSSNR", L"Intensity", 1.0f, 0.0f, 2.0f);
    g_localTone = ReadFloat(L"DLSSNR", L"LocalToneStrength", 1.0f, 0.0f, 2.0f);
    g_localStructure = ReadFloat(L"DLSSNR", L"LocalStructureStrength", 1.0f, 0.0f, 2.0f);
    g_skinStructure = ReadFloat(L"DLSSNR", L"SkinStructureStrength", -1.0f, -1.0f, 2.0f);
    g_autoMask = GetPrivateProfileIntW(L"DLSSNR", L"UseAutoMask", 1,
        g_configPath.c_str()) != 0;
    g_uiCorrection = GetPrivateProfileIntW(L"DLSSNR", L"UICorrection", 1,
        g_configPath.c_str()) != 0;
    g_advanced = GetPrivateProfileIntW(L"DLSSNR.UI", L"ShowAdvanced", 0,
        g_configPath.c_str()) != 0;
    g_depthConvention = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"DLSSNR.Advanced",
        L"DepthConvention", 0, g_configPath.c_str())), 0, 2);
    g_motionScaleX = ReadFloat(L"DLSSNR.Advanced", L"MotionScaleX", 1.0f, -2.0f, 2.0f);
    g_motionScaleY = ReadFloat(L"DLSSNR.Advanced", L"MotionScaleY", 1.0f, -2.0f, 2.0f);
    g_colorTransfer = GetPrivateProfileIntW(L"DLSSNR.Advanced",
        L"ControlCompatibleColorTransfer", 0, g_configPath.c_str()) != 0;
    g_paperWhite = ReadFloat(L"DLSSNR.Advanced", L"ScenePaperWhiteScale",
        1.0f, 0.05f, 16.0f);
    g_hdrTransfer = ReadFloat(L"DLSSNR.Advanced", L"HDRTransferStrength",
        1.0f, 0.0f, 1.0f);
    g_colorStrength = ReadFloat(L"DLSSNR.Advanced", L"ColorStrength",
        1.0f, 0.0f, 1.0f);
}

void BumpReset()
{
    g_resetGeneration.fetch_add(1, std::memory_order_relaxed);
}

HWND CreateChild(HWND parent, const wchar_t* className, const wchar_t* text,
    DWORD style, int id, int x, int y, int width, int height)
{
    return CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_module, nullptr);
}

void SetTrackbar(HWND trackbar, int minimum, int maximum, int position)
{
    SendMessageW(trackbar, TBM_SETRANGEMIN, FALSE, minimum);
    SendMessageW(trackbar, TBM_SETRANGEMAX, TRUE, maximum);
    SendMessageW(trackbar, TBM_SETPOS, TRUE, position);
}

void SetValueLabel(HWND window, int id, float value)
{
    wchar_t text[32]{};
    swprintf_s(text, L"%.2f", value);
    SetWindowTextW(Item(window, id), text);
}

void ApplyFont(HWND window)
{
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(window, [](HWND child, LPARAM parameter)
    {
        SendMessageW(child, WM_SETFONT, parameter, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

void SyncStatus(HWND window)
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    SetWindowTextW(Item(window, IdStatus), g_status.c_str());
}

constexpr std::array<int, 19> kAdvancedControls{
    IdAdvancedGroup, IdDepth, IdMotionX, IdMotionXValue, IdMotionY, IdMotionYValue,
    IdColorTransfer, IdPaperWhite, IdPaperWhiteValue, IdHdrTransfer, IdHdrTransferValue,
    IdColorStrength, IdColorStrengthValue, IdDepthLabel, IdMotionXLabel, IdMotionYLabel,
    IdPaperWhiteLabel, IdHdrTransferLabel, IdColorStrengthLabel};

void LayoutAdvanced(HWND window)
{
    const bool shown = g_advanced.load(std::memory_order_relaxed);
    for (int id : kAdvancedControls)
        ShowWindow(Item(window, id), shown ? SW_SHOW : SW_HIDE);
    const int clientHeight = shown ? 735 : 475;
    RECT client{0, 0, 530, clientHeight};
    AdjustWindowRectEx(&client, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    SetWindowPos(window, nullptr, 0, 0, client.right - client.left,
        client.bottom - client.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const int resetY = shown ? 650 : 390;
    const int statusY = shown ? 685 : 425;
    SetWindowPos(Item(window, IdReset), nullptr, 16, resetY, 160, 28,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(Item(window, IdStatus), nullptr, 16, statusY, 495, 42,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

void BuildControls(HWND window)
{
    CreateChild(window, WC_BUTTONW, L"启用 DLSS Neural Rendering", BS_AUTOCHECKBOX,
        IdEnable, 16, 14, 250, 24);
    CreateChild(window, WC_STATICW, L"NR Preset", 0, 0, 16, 49, 120, 22);
    HWND preset = CreateChild(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        IdPreset, 145, 46, 210, 160);
    for (const wchar_t* item : {L"Default", L"Preset #1", L"Preset #2", L"Preset #3"})
        SendMessageW(preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    CreateChild(window, WC_STATICW, L"NR Style", 0, 0, 16, 82, 120, 22);
    HWND style = CreateChild(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        IdStyle, 145, 79, 210, 140);
    for (const wchar_t* item : {L"Default", L"Natural", L"Cinematic"})
        SendMessageW(style, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));

    CreateChild(window, WC_STATICW, L"NR Intensity", 0, 0, 16, 119, 120, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdIntensity, 145, 111, 300, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdIntensityValue, 455, 119, 50, 22);
    CreateChild(window, WC_STATICW, L"Local Tone", 0, 0, 16, 159, 120, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdLocalTone, 145, 151, 300, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdLocalToneValue, 455, 159, 50, 22);
    CreateChild(window, WC_STATICW, L"Local Structure", 0, 0, 16, 199, 120, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdLocalStructure, 145, 191, 300, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdLocalStructureValue, 455, 199, 50, 22);
    CreateChild(window, WC_STATICW, L"Skin Structure", 0, 0, 16, 239, 120, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdSkinStructure, 145, 231, 300, 30);
    CreateChild(window, WC_STATICW, L"-1.00", SS_RIGHT, IdSkinStructureValue, 455, 239, 50, 22);
    CreateChild(window, WC_BUTTONW, L"Automatic Mask", BS_AUTOCHECKBOX,
        IdAutoMask, 16, 278, 210, 24);
    CreateChild(window, WC_BUTTONW, L"NR UI Correction", BS_AUTOCHECKBOX,
        IdUiCorrection, 250, 278, 210, 24);
    CreateChild(window, WC_BUTTONW, L"显示高级诊断", BS_AUTOCHECKBOX,
        IdAdvanced, 16, 319, 210, 24);

    CreateChild(window, WC_BUTTONW, L"高级：导引与色彩桥", BS_GROUPBOX,
        IdAdvancedGroup, 10, 350, 505, 285);
    CreateChild(window, WC_STATICW, L"Depth Convention", 0, IdDepthLabel, 24, 378, 140, 22);
    HWND depth = CreateChild(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        IdDepth, 170, 375, 245, 120);
    for (const wchar_t* item : {L"Use game NGX flag", L"Force normal depth", L"Force inverted depth"})
        SendMessageW(depth, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    CreateChild(window, WC_STATICW, L"Motion Scale X", 0, IdMotionXLabel, 24, 418, 120, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdMotionX, 170, 410, 275, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdMotionXValue, 450, 418, 50, 22);
    CreateChild(window, WC_STATICW, L"Motion Scale Y", 0, IdMotionYLabel, 24, 458, 120, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdMotionY, 170, 450, 275, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdMotionYValue, 450, 458, 50, 22);
    CreateChild(window, WC_BUTTONW, L"Control-compatible color transfer", BS_AUTOCHECKBOX,
        IdColorTransfer, 24, 492, 300, 24);
    CreateChild(window, WC_STATICW, L"Scene Paper-White Scale", 0, IdPaperWhiteLabel,
        24, 530, 145, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdPaperWhite, 170, 522, 275, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdPaperWhiteValue, 450, 530, 50, 22);
    CreateChild(window, WC_STATICW, L"HDR Transfer Strength", 0, IdHdrTransferLabel,
        24, 570, 145, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdHdrTransfer, 170, 562, 275, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdHdrTransferValue, 450, 570, 50, 22);
    CreateChild(window, WC_STATICW, L"Color Strength", 0, IdColorStrengthLabel,
        24, 610, 145, 22);
    CreateChild(window, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        IdColorStrength, 170, 602, 275, 30);
    CreateChild(window, WC_STATICW, L"1.00", SS_RIGHT, IdColorStrengthValue, 450, 610, 50, 22);

    CreateChild(window, WC_BUTTONW, L"重建 NR / 清除失败", BS_PUSHBUTTON | WS_TABSTOP,
        IdReset, 16, 650, 160, 28);
    CreateChild(window, WC_STATICW, L"", SS_LEFT, IdStatus, 16, 685, 495, 42);

    SendMessageW(Item(window, IdEnable), BM_SETCHECK, g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(preset, CB_SETCURSEL, g_preset, 0);
    SendMessageW(style, CB_SETCURSEL, g_style, 0);
    SetTrackbar(Item(window, IdIntensity), 0, 200, static_cast<int>(g_intensity * 100.0f));
    SetTrackbar(Item(window, IdLocalTone), 0, 200, static_cast<int>(g_localTone * 100.0f));
    SetTrackbar(Item(window, IdLocalStructure), 0, 200,
        static_cast<int>(g_localStructure * 100.0f));
    SetTrackbar(Item(window, IdSkinStructure), -100, 200,
        static_cast<int>(g_skinStructure * 100.0f));
    SendMessageW(Item(window, IdAutoMask), BM_SETCHECK, g_autoMask ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(window, IdUiCorrection), BM_SETCHECK,
        g_uiCorrection ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Item(window, IdAdvanced), BM_SETCHECK, g_advanced ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(depth, CB_SETCURSEL, g_depthConvention, 0);
    SetTrackbar(Item(window, IdMotionX), -200, 200, static_cast<int>(g_motionScaleX * 100.0f));
    SetTrackbar(Item(window, IdMotionY), -200, 200, static_cast<int>(g_motionScaleY * 100.0f));
    SendMessageW(Item(window, IdColorTransfer), BM_SETCHECK,
        g_colorTransfer ? BST_CHECKED : BST_UNCHECKED, 0);
    SetTrackbar(Item(window, IdPaperWhite), 5, 1600, static_cast<int>(g_paperWhite * 100.0f));
    SetTrackbar(Item(window, IdHdrTransfer), 0, 100, static_cast<int>(g_hdrTransfer * 100.0f));
    SetTrackbar(Item(window, IdColorStrength), 0, 100, static_cast<int>(g_colorStrength * 100.0f));
    SetValueLabel(window, IdIntensityValue, g_intensity);
    SetValueLabel(window, IdLocalToneValue, g_localTone);
    SetValueLabel(window, IdLocalStructureValue, g_localStructure);
    SetValueLabel(window, IdSkinStructureValue, g_skinStructure);
    SetValueLabel(window, IdMotionXValue, g_motionScaleX);
    SetValueLabel(window, IdMotionYValue, g_motionScaleY);
    SetValueLabel(window, IdPaperWhiteValue, g_paperWhite);
    SetValueLabel(window, IdHdrTransferValue, g_hdrTransfer);
    SetValueLabel(window, IdColorStrengthValue, g_colorStrength);
    ApplyFont(window);
    SyncStatus(window);
    LayoutAdvanced(window);
}

void CommitCheckbox(HWND window, int id)
{
    const bool checked = SendMessageW(Item(window, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    switch (id)
    {
    case IdEnable:
        g_enabled = checked;
        WriteInt(L"DLSSNR", L"Enable", checked ? 1 : 0);
        BumpReset();
        break;
    case IdAutoMask:
        g_autoMask = checked;
        WriteInt(L"DLSSNR", L"UseAutoMask", checked ? 1 : 0);
        BumpReset();
        break;
    case IdUiCorrection:
        g_uiCorrection = checked;
        WriteInt(L"DLSSNR", L"UICorrection", checked ? 1 : 0);
        BumpReset();
        break;
    case IdAdvanced:
        g_advanced = checked;
        WriteInt(L"DLSSNR.UI", L"ShowAdvanced", checked ? 1 : 0);
        LayoutAdvanced(window);
        break;
    case IdColorTransfer:
        g_colorTransfer = checked;
        WriteInt(L"DLSSNR.Advanced", L"ControlCompatibleColorTransfer", checked ? 1 : 0);
        BumpReset();
        break;
    }
}

void CommitTrackbar(HWND window, HWND trackbar)
{
    const int id = GetDlgCtrlID(trackbar);
    const int position = static_cast<int>(SendMessageW(trackbar, TBM_GETPOS, 0, 0));
    const float value = static_cast<float>(position) / 100.0f;
    switch (id)
    {
    case IdIntensity:
        g_intensity = value; WriteFloat(L"DLSSNR", L"Intensity", value);
        SetValueLabel(window, IdIntensityValue, value); break;
    case IdLocalTone:
        g_localTone = value; WriteFloat(L"DLSSNR", L"LocalToneStrength", value);
        SetValueLabel(window, IdLocalToneValue, value); break;
    case IdLocalStructure:
        g_localStructure = value; WriteFloat(L"DLSSNR", L"LocalStructureStrength", value);
        SetValueLabel(window, IdLocalStructureValue, value); break;
    case IdSkinStructure:
        g_skinStructure = value; WriteFloat(L"DLSSNR", L"SkinStructureStrength", value);
        SetValueLabel(window, IdSkinStructureValue, value); break;
    case IdMotionX:
        g_motionScaleX = value; WriteFloat(L"DLSSNR.Advanced", L"MotionScaleX", value);
        SetValueLabel(window, IdMotionXValue, value); break;
    case IdMotionY:
        g_motionScaleY = value; WriteFloat(L"DLSSNR.Advanced", L"MotionScaleY", value);
        SetValueLabel(window, IdMotionYValue, value); break;
    case IdPaperWhite:
        g_paperWhite = value; WriteFloat(L"DLSSNR.Advanced", L"ScenePaperWhiteScale", value);
        SetValueLabel(window, IdPaperWhiteValue, value); break;
    case IdHdrTransfer:
        g_hdrTransfer = value; WriteFloat(L"DLSSNR.Advanced", L"HDRTransferStrength", value);
        SetValueLabel(window, IdHdrTransferValue, value); break;
    case IdColorStrength:
        g_colorStrength = value; WriteFloat(L"DLSSNR.Advanced", L"ColorStrength", value);
        SetValueLabel(window, IdColorStrengthValue, value); break;
    default: return;
    }
    BumpReset();
}

void ToggleWindow(HWND window)
{
    const bool show = !g_visible.load(std::memory_order_relaxed);
    g_visible = show;
    ShowWindow(window, show ? SW_SHOWNORMAL : SW_HIDE);
    if (show)
    {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        BuildControls(window);
        return 0;
    case kToggleMessage:
        ToggleWindow(window);
        return 0;
    case kStatusMessage:
        SyncStatus(window);
        return 0;
    case kQuitMessage:
        DestroyWindow(window);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F9)
        {
            ToggleWindow(window);
            return 0;
        }
        break;
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (notification == BN_CLICKED &&
            (id == IdEnable || id == IdAutoMask || id == IdUiCorrection ||
             id == IdAdvanced || id == IdColorTransfer))
        {
            CommitCheckbox(window, id);
            return 0;
        }
        if (notification == CBN_SELCHANGE && id == IdPreset)
        {
            g_preset = static_cast<int>(SendMessageW(Item(window, id), CB_GETCURSEL, 0, 0));
            WriteInt(L"DLSSNR", L"Preset", g_preset);
            BumpReset();
            return 0;
        }
        if (notification == CBN_SELCHANGE && id == IdStyle)
        {
            g_style = static_cast<int>(SendMessageW(Item(window, id), CB_GETCURSEL, 0, 0));
            WriteInt(L"DLSSNR", L"Style", g_style);
            BumpReset();
            return 0;
        }
        if (notification == CBN_SELCHANGE && id == IdDepth)
        {
            g_depthConvention = static_cast<int>(
                SendMessageW(Item(window, id), CB_GETCURSEL, 0, 0));
            WriteInt(L"DLSSNR.Advanced", L"DepthConvention", g_depthConvention);
            BumpReset();
            return 0;
        }
        if (notification == BN_CLICKED && id == IdReset)
        {
            g_retryGeneration.fetch_add(1, std::memory_order_relaxed);
            BumpReset();
            return 0;
        }
        break;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam))
        {
            CommitTrackbar(window, reinterpret_cast<HWND>(lParam));
            return 0;
        }
        break;
    case WM_CLOSE:
        g_visible = false;
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_window = nullptr;
        g_visible = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI PanelThread(void*)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_module;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass,
        L"AC Odyssey DLSSNR — F9", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 550, 500, nullptr, nullptr, g_module, nullptr);
    if (!window) return 0;
    g_window = window;
    ShowWindow(window, SW_HIDE);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
}

void ControlPanelInitialize(HMODULE module, const std::wstring& configPath)
{
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) return;
    g_module = module;
    g_configPath = configPath;
    LoadSettings();
    g_thread = CreateThread(nullptr, 0, PanelThread, nullptr, 0, nullptr);
}

void ControlPanelShutdown()
{
    if (HWND window = g_window.load()) PostMessageW(window, kQuitMessage, 0, 0);
    if (g_thread) CloseHandle(g_thread);
    g_thread = nullptr;
}

void ControlPanelToggle()
{
    if (HWND window = g_window.load()) PostMessageW(window, kToggleMessage, 0, 0);
}

bool ControlPanelIsVisible()
{
    return g_visible.load(std::memory_order_relaxed);
}

DlssNrControlSnapshot ControlPanelSnapshot()
{
    DlssNrControlSnapshot result{};
    result.enabled = g_enabled.load(std::memory_order_relaxed);
    result.preset = g_preset.load(std::memory_order_relaxed);
    result.style = g_style.load(std::memory_order_relaxed);
    result.intensity = g_intensity.load(std::memory_order_relaxed);
    result.localTone = g_localTone.load(std::memory_order_relaxed);
    result.localStructure = g_localStructure.load(std::memory_order_relaxed);
    result.skinStructure = g_skinStructure.load(std::memory_order_relaxed);
    result.autoMask = g_autoMask.load(std::memory_order_relaxed);
    result.uiCorrection = g_uiCorrection.load(std::memory_order_relaxed);
    result.depthConvention = g_depthConvention.load(std::memory_order_relaxed);
    result.motionScaleX = g_motionScaleX.load(std::memory_order_relaxed);
    result.motionScaleY = g_motionScaleY.load(std::memory_order_relaxed);
    result.controlCompatibleColor = g_colorTransfer.load(std::memory_order_relaxed);
    result.scenePaperWhiteScale = g_paperWhite.load(std::memory_order_relaxed);
    result.hdrTransferStrength = g_hdrTransfer.load(std::memory_order_relaxed);
    result.colorStrength = g_colorStrength.load(std::memory_order_relaxed);
    result.resetGeneration = g_resetGeneration.load(std::memory_order_relaxed);
    result.retryGeneration = g_retryGeneration.load(std::memory_order_relaxed);
    return result;
}

void ControlPanelUpdateStatus(const std::wstring& status)
{
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status = status;
    }
    if (HWND window = g_window.load()) PostMessageW(window, kStatusMessage, 0, 0);
}
