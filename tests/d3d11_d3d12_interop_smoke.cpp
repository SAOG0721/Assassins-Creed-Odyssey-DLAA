#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

using Microsoft::WRL::ComPtr;

int main()
{
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device11, &selected, &context11);
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels + 1, 1, D3D11_SDK_VERSION, &device11, &selected, &context11);
    if (FAILED(hr)) return 2;

    ComPtr<ID3D11Device5> device11_5;
    ComPtr<ID3D11DeviceContext4> context11_4;
    if (FAILED(device11.As(&device11_5)) || FAILED(context11.As(&context11_4))) return 3;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device11.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))) return 4;
    ComPtr<ID3D12Device> device12;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device12)))) return 5;

    D3D12_HEAP_PROPERTIES sharedHeap{};
    sharedHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    sharedHeap.CreationNodeMask = 1;
    sharedHeap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC sharedDesc{};
    sharedDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sharedDesc.Width = 64;
    sharedDesc.Height = 64;
    sharedDesc.DepthOrArraySize = 1;
    sharedDesc.MipLevels = 1;
    sharedDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    sharedDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    ComPtr<ID3D12Resource> texture12;
    hr = device12->CreateCommittedResource(&sharedHeap, D3D12_HEAP_FLAG_SHARED,
        &sharedDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture12));
    if (FAILED(hr)) return 6;
    HANDLE textureHandle{};
    hr = device12->CreateSharedHandle(texture12.Get(), nullptr, GENERIC_ALL, nullptr,
        &textureHandle);
    if (FAILED(hr)) return 8;
    ComPtr<ID3D11Texture2D> texture11;
    hr = device11_5->OpenSharedResource1(textureHandle, IID_PPV_ARGS(&texture11));
    CloseHandle(textureHandle);
    if (FAILED(hr))
    {
        std::cerr << "OpenSharedResource1 failed hr=0x" << std::hex << hr << '\n';
        return 9;
    }
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture11->GetDesc(&textureDesc);
    ComPtr<ID3D11ShaderResourceView> sharedSrv;
    ComPtr<ID3D11UnorderedAccessView> sharedUav;
    if (FAILED(device11->CreateShaderResourceView(texture11.Get(), nullptr, &sharedSrv)) ||
        FAILED(device11->CreateUnorderedAccessView(texture11.Get(), nullptr, &sharedUav))) return 28;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return 10;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 rowBytes{};
    UINT64 uploadBytes{};
    device12->GetCopyableFootprints(&sharedDesc, 0, 1, 0, &footprint, &rows,
        &rowBytes, &uploadBytes);
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device12->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload)))) return 20;
    void* mapped{};
    if (FAILED(upload->Map(0, nullptr, &mapped))) return 21;
    constexpr std::array<uint16_t, 4> expected{0x3400, 0x3800, 0x3A00, 0x3C00};
    for (UINT y = 0; y < 64; ++y)
    {
        auto* row = static_cast<uint8_t*>(mapped) + footprint.Offset +
            static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < 64; ++x)
            std::memcpy(row + x * sizeof(expected), expected.data(), sizeof(expected));
    }
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator)))) return 22;
    if (FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)))) return 23;
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = texture12.Get();
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture12.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toCopy);
    if (FAILED(commandList->Close())) return 24;

    ComPtr<ID3D12Fence> fence12;
    if (FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12)))) return 11;
    HANDLE fenceHandle{};
    if (FAILED(device12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr,
            &fenceHandle))) return 12;
    ComPtr<ID3D11Fence> fence11;
    hr = device11_5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&fence11));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) return 13;
    if (FAILED(context11_4->Signal(fence11.Get(), 1))) return 14;
    if (FAILED(queue->Wait(fence12.Get(), 1))) return 15;
    ID3D12CommandList* lists[] = {commandList.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence12.Get(), 2))) return 16;
    if (FAILED(context11_4->Wait(fence11.Get(), 2))) return 17;
    D3D11_TEXTURE2D_DESC stagingDesc = textureDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device11->CreateTexture2D(&stagingDesc, nullptr, &staging))) return 25;
    context11_4->CopyResource(staging.Get(), texture11.Get());
    D3D11_MAPPED_SUBRESOURCE stagingMap{};
    if (FAILED(context11_4->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &stagingMap))) return 26;
    const bool pixelMatches = std::memcmp(stagingMap.pData, expected.data(), sizeof(expected)) == 0;
    if (!pixelMatches)
    {
        const auto* actual = static_cast<const uint16_t*>(stagingMap.pData);
        std::cerr << "pixel mismatch actual=" << std::hex << actual[0] << ',' << actual[1]
                  << ',' << actual[2] << ',' << actual[3] << " expected=" << expected[0]
                  << ',' << expected[1] << ',' << expected[2] << ',' << expected[3] << '\n';
    }
    context11_4->Unmap(staging.Get(), 0);
    if (!pixelMatches) return 27;

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device12->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
            &uploadDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback)))) return 29;
    if (FAILED(allocator->Reset()) || FAILED(commandList->Reset(allocator.Get(), nullptr))) return 30;
    D3D12_RESOURCE_BARRIER toSource{};
    toSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSource.Transition.pResource = texture12.Get();
    toSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSource.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toSource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &toSource);
    D3D12_TEXTURE_COPY_LOCATION readbackDestination{};
    readbackDestination.pResource = readback.Get();
    readbackDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readbackDestination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION sharedSource{};
    sharedSource.pResource = texture12.Get();
    sharedSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(&readbackDestination, 0, 0, 0, &sharedSource, nullptr);
    std::swap(toSource.Transition.StateBefore, toSource.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toSource);
    if (FAILED(commandList->Close())) return 31;
    constexpr float clearColor[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    context11_4->ClearUnorderedAccessViewFloat(sharedUav.Get(), clearColor);
    if (FAILED(context11_4->Signal(fence11.Get(), 3))) return 32;
    if (FAILED(queue->Wait(fence12.Get(), 3))) return 33;
    lists[0] = commandList.Get();
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence12.Get(), 4))) return 34;
    if (FAILED(context11_4->Wait(fence11.Get(), 4))) return 35;
    context11_4->Flush();
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) return 18;
    hr = fence11->SetEventOnCompletion(4, eventHandle);
    const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(eventHandle, 5000) : WAIT_FAILED;
    CloseHandle(eventHandle);
    if (wait != WAIT_OBJECT_0) return 19;
    void* readbackMap{};
    if (FAILED(readback->Map(0, nullptr, &readbackMap))) return 36;
    constexpr std::array<uint16_t, 4> clearExpected{0x3C00, 0x3800, 0x3400, 0x3C00};
    const auto* clearPixel = static_cast<const uint8_t*>(readbackMap) + footprint.Offset;
    const bool reverseMatches =
        std::memcmp(clearPixel, clearExpected.data(), sizeof(clearExpected)) == 0;
    readback->Unmap(0, nullptr);
    if (!reverseMatches) return 37;
    std::cout << "d3d11-d3d12-resource=open-ok srv-uav=ok d3d12-write-d3d11-read=ok d3d11-write-d3d12-read=ok shared-fence=roundtrip-ok completed="
              << fence12->GetCompletedValue() << '\n';
    return 0;
}
