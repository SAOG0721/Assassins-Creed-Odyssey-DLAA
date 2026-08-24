#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <iostream>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"usage: bridge-smoke <absolute-bridge-path>\n";
        return 2;
    }
    HMODULE bridge = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bridge)
    {
        std::wcerr << L"LoadLibraryExW failed: " << GetLastError() << L'\n';
        return 3;
    }
    if (!GetProcAddress(bridge, "ACO_DLAA_Evaluate"))
    {
        std::cerr << "ACO_DLAA_Evaluate export missing\n";
        return 4;
    }

    D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested,
        static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION, &device, &selected, &context);
    if (result == E_INVALIDARG)
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested + 1, 1,
            D3D11_SDK_VERSION, &device, &selected, &context);
    }
    if (FAILED(result))
    {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex << result << '\n';
        return 5;
    }
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(device.As(&device1)) || FAILED(context.As(&context1)))
    {
        std::cerr << "D3D11.1 interfaces unavailable\n";
        return 6;
    }
    ComPtr<ID3DDeviceContextState> isolated;
    D3D_FEATURE_LEVEL chosen{};
    result = device1->CreateDeviceContextState(0, &selected, 1, D3D11_SDK_VERSION,
        __uuidof(ID3D11Device), &chosen, &isolated);
    if (FAILED(result))
    {
        std::cerr << "CreateDeviceContextState failed: 0x" << std::hex << result << '\n';
        return 7;
    }
    const D3D11_VIEWPORT gameViewport{7.0f, 11.0f, 123.0f, 77.0f, 0.125f, 0.875f};
    context1->RSSetViewports(1, &gameViewport);
    ID3DDeviceContextState* previous{};
    context1->SwapDeviceContextState(isolated.Get(), &previous);
    if (!previous)
    {
        std::cerr << "SwapDeviceContextState returned no previous state\n";
        return 8;
    }
    const D3D11_VIEWPORT bridgeViewport{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f};
    context1->RSSetViewports(1, &bridgeViewport);
    ID3DDeviceContextState* discarded{};
    context1->SwapDeviceContextState(previous, &discarded);
    previous->Release();
    if (discarded) discarded->Release();
    UINT viewportCount = 1;
    D3D11_VIEWPORT restored{};
    context1->RSGetViewports(&viewportCount, &restored);
    if (viewportCount != 1 || restored.TopLeftX != gameViewport.TopLeftX ||
        restored.TopLeftY != gameViewport.TopLeftY || restored.Width != gameViewport.Width ||
        restored.Height != gameViewport.Height || restored.MinDepth != gameViewport.MinDepth ||
        restored.MaxDepth != gameViewport.MaxDepth)
    {
        std::cerr << "context state did not restore the original viewport\n";
        return 9;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        adapter->GetDesc(&adapterDesc);
    std::wcout << L"bridge-load=ok state-swap=ok state-restore=ok feature-level=0x" << std::hex << selected
               << L" adapter=" << adapterDesc.Description << L'\n';
    FreeLibrary(bridge);
    return 0;
}
