#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;

enum ACO_DLAA_PresentationMode : uint32_t
{
    ACO_DLAA_PRESENT_GAME_TAA = 0,
    ACO_DLAA_PRESENT_DLAA = 1,
    ACO_DLAA_PRESENT_RAW_COLOR = 2,
};

// C ABI kept deliberately small so the proxy can remain loadable when NGX is absent.
struct ACO_DLAA_Frame
{
    uint32_t structSize;
    ID3D11DeviceContext* context;
    ID3D11Resource* color;
    ID3D11Resource* packedMotion;
    ID3D11Resource* depth;
    ID3D11Resource* gameOutput;
    uint32_t width;
    uint32_t height;
    float jitterX;
    float jitterY;
    // Odyssey cb6 projection terms. For positive linear view distance d:
    // deviceDepth = saturate((projection22 * d - projection32) / (projection23 * d)).
    float projection22;
    float projection23;
    float projection32;
    // Constants used by Odyssey's active TAA permutation to reconstruct
    // rotational/far-field motion instead of sampling the packed motion map.
    float zFrontBackValueY;
    float maxViewDepthParams[4];
    float clipXYZToViewPos[16];
    float viewToWorld[16];
    float worldViewProjPrevFrame[16];
    // Selects which already-available full-resolution surface is copied into
    // Odyssey's TAA output: the untouched game TAA result, DLAA, or the
    // pre-TAA color input. DLAA continues evaluating in every mode so its
    // temporal history remains warm during A/B comparisons.
    uint32_t presentationMode;
    uint32_t depthInverted;
    // Runtime calibration applied to the raw per-frame Odyssey jitter before
    // submission to NGX. Scale is [0, 1]; signs must each be -1 or +1.
    float jitterScale;
    int32_t jitterSignX;
    int32_t jitterSignY;
    // Phase-selected sample from Odyssey's verified repeating jitter sequence.
    float phaseJitterX;
    float phaseJitterY;
    int32_t jitterPhaseOffset;
    uint32_t jitterCycleVerified;
    // One-frame reset request after a runtime jitter calibration change.
    uint32_t resetHistory;

    // DLSS Neural Rendering is an optional one-to-one post-pass after the
    // bridge's raw DLAA output.  It owns an independent reset/failure state so
    // disabling it or changing its controls never invalidates DLAA history.
    uint32_t dlssNrEnabled;
    int32_t dlssNrPreset;       // 0 Default, 1 Preset #1, 2 Preset #2, 3 Preset #3.
    int32_t dlssNrStyle;        // 0 Default, 1 Natural, 2 Cinematic.
    float dlssNrIntensity;      // [0, 2]
    float dlssNrLocalTone;      // [0, 2]
    float dlssNrLocalStructure; // [0, 2]
    float dlssNrSkinStructure;  // [-1, 2]
    uint32_t dlssNrAutoMask;
    uint32_t dlssNrUiCorrection;

    // Advanced diagnostics. Depth convention: 0 game flag, 1 normal,
    // 2 inverted. Motion multipliers are applied on top of Odyssey's audited
    // normalized-UV to current->previous pixel conversion.
    int32_t dlssNrDepthConvention;
    float dlssNrMotionScaleX;
    float dlssNrMotionScaleY;

    // Addon-compatible color bridge controls. The default disabled path is a
    // normalized UNORM <-> FP16 identity transfer suitable for Odyssey's LDR
    // pre-tonemap TAA surface.
    uint32_t dlssNrControlCompatibleColor;
    float dlssNrScenePaperWhiteScale;
    float dlssNrHdrTransferStrength;
    float dlssNrColorStrength;

    // Monotonic UI generations. resetGeneration requests one history reset;
    // retryGeneration also clears the NR-only failure latch and recreates its
    // feature without disturbing DLAA.
    uint64_t dlssNrResetGeneration;
    uint64_t dlssNrRetryGeneration;
};

enum ACO_DLSSNR_State : uint32_t
{
    ACO_DLSSNR_DISABLED = 0,
    ACO_DLSSNR_READY = 1,
    ACO_DLSSNR_WARMING_UP = 2,
    ACO_DLSSNR_ACTIVE = 3,
    ACO_DLSSNR_BUSY_FALLBACK = 4,
    ACO_DLSSNR_FAILURE_LATCHED = 5,
};

struct ACO_DLSSNR_Status
{
    uint32_t structSize;
    uint32_t state;
    uint32_t failureLatched;
    uint32_t warmupFrames;
    uint64_t evaluatedFrames;
    uint64_t lastSubmittedFence;
    int32_t lastNgxResult;
    char message[256];
};

enum ACO_DLAA_Result : int32_t
{
    ACO_DLAA_OK = 0,
    ACO_DLAA_BAD_ARGUMENT = -1,
    ACO_DLAA_STATE_ISOLATION_FAILED = -2,
    ACO_DLAA_SHADER_FAILED = -3,
    ACO_DLAA_RESOURCE_FAILED = -4,
    ACO_DLAA_NGX_INIT_FAILED = -5,
    ACO_DLAA_NGX_UNAVAILABLE = -6,
    ACO_DLAA_NGX_CREATE_FAILED = -7,
    ACO_DLAA_NGX_EVALUATE_FAILED = -8,
};

using AcoDlaaEvaluateFn = int32_t(WINAPI*)(const ACO_DLAA_Frame* frame);
using AcoDlssNrGetStatusFn = int32_t(WINAPI*)(ACO_DLSSNR_Status* status);
using AcoDlssNrShutdownFn = int32_t(WINAPI*)();
