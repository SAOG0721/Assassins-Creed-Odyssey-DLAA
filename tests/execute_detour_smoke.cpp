#include <Windows.h>
#include <d3d11.h>

#include <atomic>
#include <iostream>

#include "MinHook.h"

namespace
{
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);

ExecuteCommandListFn g_original{};
std::atomic<unsigned int> g_calls{};

void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* context,
    ID3D11CommandList* commandList, BOOL restoreContextState)
{
    g_calls.fetch_add(1);
    g_original(context, commandList, restoreContextState);
}
}

int main()
{
    ID3D11Device* device{};
    ID3D11DeviceContext* immediate{};
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &immediate);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &immediate);
    if (FAILED(result) || !device || !immediate) return 2;

    ID3D11DeviceContext* deferred{};
    ID3D11CommandList* commandList{};
    result = device->CreateDeferredContext(0, &deferred);
    if (SUCCEEDED(result) && deferred)
        result = deferred->FinishCommandList(FALSE, &commandList);
    if (FAILED(result) || !commandList) return 3;

    void** table = *reinterpret_cast<void***>(immediate);
    void* const target = table[58];
    if (MH_Initialize() != MH_OK) return 4;
    if (MH_CreateHook(target, reinterpret_cast<void*>(HookExecuteCommandList),
            reinterpret_cast<void**>(&g_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
        return 5;

    void* const slotBefore = table[58];
    immediate->ExecuteCommandList(commandList, FALSE);
    void* const slotAfter = table[58];
    const unsigned int calls = g_calls.load();

    MH_DisableHook(target);
    MH_RemoveHook(target);
    MH_Uninitialize();
    commandList->Release();
    deferred->Release();
    immediate->Release();
    device->Release();

    std::cout << "execute-detour calls=" << calls
              << " vtable-unchanged=" << (slotBefore == target && slotAfter == target ? 1 : 0)
              << " feature-level=0x" << std::hex << static_cast<unsigned int>(featureLevel) << '\n';
    return calls == 1 && slotBefore == target && slotAfter == target ? 0 : 6;
}
