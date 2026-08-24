#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <d3d11.h>

#include <cstddef>

template <typename Vtable>
constexpr std::size_t Slot(std::size_t offset)
{
    return offset / sizeof(void*);
}

static_assert(Slot<ID3D11DeviceVtbl>(offsetof(ID3D11DeviceVtbl, CreateBuffer)) == 3);
static_assert(Slot<ID3D11DeviceVtbl>(offsetof(ID3D11DeviceVtbl, CreatePixelShader)) == 15);
static_assert(Slot<ID3D11DeviceVtbl>(offsetof(ID3D11DeviceVtbl, CreateDeferredContext)) == 27);

static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, PSSetShader)) == 9);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, DrawIndexed)) == 12);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, Draw)) == 13);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, Map)) == 14);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, Unmap)) == 15);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced)) == 20);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, DrawInstanced)) == 21);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, DrawAuto)) == 38);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstancedIndirect)) == 39);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, DrawInstancedIndirect)) == 40);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, UpdateSubresource)) == 48);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, ExecuteCommandList)) == 58);
static_assert(Slot<ID3D11DeviceContextVtbl>(offsetof(ID3D11DeviceContextVtbl, FinishCommandList)) == 114);

int main()
{
    return 0;
}
