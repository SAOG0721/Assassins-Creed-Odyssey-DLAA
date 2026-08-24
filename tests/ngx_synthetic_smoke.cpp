#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <iostream>
#include <vector>

#include "bridge_api.h"

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    HMODULE bridge = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bridge)
    {
        std::wcerr << L"bridge load failed win32=" << GetLastError() << L'\n';
        return 3;
    }
    const auto evaluate = reinterpret_cast<AcoDlaaEvaluateFn>(
        GetProcAddress(bridge, "ACO_DLAA_Evaluate"));
    if (!evaluate) return 4;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, &level, &context);
    if (result == E_INVALIDARG)
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 1,
            D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) return 5;

    constexpr UINT width = 1280;
    constexpr UINT height = 720;
    std::vector<uint8_t> colorData(static_cast<size_t>(width) * height * 4);
    std::vector<uint8_t> motionData(static_cast<size_t>(width) * height * 4);
    std::vector<float> depthData(static_cast<size_t>(width) * height);
    std::vector<uint8_t> outputData(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            colorData[pixel * 4 + 0] = static_cast<uint8_t>((x * 255u) / (width - 1));
            colorData[pixel * 4 + 1] = static_cast<uint8_t>((y * 255u) / (height - 1));
            colorData[pixel * 4 + 2] = ((x / 32 + y / 32) & 1) ? 224 : 32;
            colorData[pixel * 4 + 3] = 255;
            // 12-bit x/y values 2048 encode near-zero normalized motion.
            motionData[pixel * 4 + 0] = 128;
            motionData[pixel * 4 + 1] = 0;
            motionData[pixel * 4 + 2] = 8;
            motionData[pixel * 4 + 3] = 255;
            depthData[pixel] = 0.05f + 0.9f * static_cast<float>(y) / static_cast<float>(height - 1);
            outputData[pixel * 4 + 0] = 17;
            outputData[pixel * 4 + 1] = 33;
            outputData[pixel * 4 + 2] = 65;
            outputData[pixel * 4 + 3] = 255;
        }
    }

    auto createTexture = [&](DXGI_FORMAT format, UINT bindFlags, const void* data, UINT rowPitch,
                             ComPtr<ID3D11Texture2D>& output)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        D3D11_SUBRESOURCE_DATA initial{data, rowPitch, 0};
        return device->CreateTexture2D(&desc, data ? &initial : nullptr, &output);
    };

    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11Texture2D> gameOutput;
    if (FAILED(createTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, colorData.data(), width * 4, color)) ||
        FAILED(createTexture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
            motionData.data(), width * 4, motion)) ||
        FAILED(createTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
            depthData.data(), width * sizeof(float), depth)) ||
        FAILED(createTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
            outputData.data(), width * 4, gameOutput)))
        return 6;

    ACO_DLAA_Frame frame{};
    frame.structSize = sizeof(frame);
    frame.context = context.Get();
    frame.color = color.Get();
    frame.packedMotion = motion.Get();
    frame.depth = depth.Get();
    frame.gameOutput = gameOutput.Get();
    frame.width = width;
    frame.height = height;
    frame.presentationMode = ACO_DLAA_PRESENT_GAME_TAA;
    frame.jitterScale = 1.0f;
    frame.jitterSignX = 1;
    frame.jitterSignY = 1;
    frame.projection22 = -1.0000833f;
    frame.projection23 = -1.0f;
    frame.projection32 = -0.10000834f;
    frame.zFrontBackValueY = 1.0f;
    frame.maxViewDepthParams[0] = 0.0f;
    frame.maxViewDepthParams[1] = -0.10000834f;
    frame.maxViewDepthParams[2] = -1.0f;
    frame.maxViewDepthParams[3] = -1.0000833f;
    for (int i = 0; i < 4; ++i)
    {
        frame.clipXYZToViewPos[i * 4 + i] = 1.0f;
        frame.viewToWorld[i * 4 + i] = 1.0f;
        frame.worldViewProjPrevFrame[i * 4 + i] = 1.0f;
    }
    D3D11_TEXTURE2D_DESC stagingDesc{};
    gameOutput->GetDesc(&stagingDesc);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return 8;

    const int32_t background = evaluate(&frame);
    if (background != ACO_DLAA_OK) return 7;
    context->CopyResource(staging.Get(), gameOutput.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 9;
    bool gameTaaPreserved = true;
    for (UINT y = 0; y < height && gameTaaPreserved; y += 16)
    {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = 0; x < width; x += 16)
        {
            const auto* pixel = row + x * 4;
            if (pixel[0] != 17 || pixel[1] != 33 || pixel[2] != 65 || pixel[3] != 255)
            {
                gameTaaPreserved = false;
                break;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    if (!gameTaaPreserved) return 10;

    frame.presentationMode = ACO_DLAA_PRESENT_DLAA;
    const int32_t first = evaluate(&frame);
    const int32_t second = evaluate(&frame);
    if (first != ACO_DLAA_OK || second != ACO_DLAA_OK) return 11;
    context->CopyResource(staging.Get(), gameOutput.Get());
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 12;
    uint8_t minimum = 255;
    uint8_t maximum = 0;
    for (UINT y = 0; y < height; y += 16)
    {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = 0; x < width; x += 16)
        {
            for (UINT channel = 0; channel < 3; ++channel)
            {
                const uint8_t value = row[x * 4 + channel];
                minimum = value < minimum ? value : minimum;
                maximum = value > maximum ? value : maximum;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    std::cout << "synthetic-background=" << background
              << " game-taa-preserved=" << (gameTaaPreserved ? 1 : 0)
              << " synthetic-evaluate-1=" << first << " synthetic-evaluate-2=" << second
              << " presented-min=" << static_cast<unsigned int>(minimum)
              << " presented-max=" << static_cast<unsigned int>(maximum) << '\n';
    return maximum > minimum ? 0 : 13;
}
