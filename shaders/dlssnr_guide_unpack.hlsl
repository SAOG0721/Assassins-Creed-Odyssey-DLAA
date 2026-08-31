Texture2D<float4> GuidePack : register(t0);
RWTexture2D<float2> Motion : register(u0);
RWTexture2D<float> Depth : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    GuidePack.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float4 guide = GuidePack.Load(int3(id.xy, 0));
    Motion[id.xy] = guide.xy;
    Depth[id.xy] = guide.z;
}
