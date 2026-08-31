Texture2D<float4> DlaaColor : register(t0);
Texture2D<float2> DecodedMotion : register(t1);
Texture2D<float> DeviceDepth : register(t2);
RWTexture2D<float4> ColorProxy : register(u0);
RWTexture2D<float4> GuidePack : register(u1);

cbuffer CodecConstants : register(b0)
{
    uint2 OutputSize;
    uint ControlCompatibleColor;
    uint CodecPadding;
    float ScenePaperWhiteScale;
    float HdrTransferStrength;
    float ColorStrength;
    float CodecPadding2;
};

float3 SrgbEncode(float3 color)
{
    color = saturate(color);
    const float3 low = color * 12.92;
    const float3 high = 1.055 * pow(color, 1.0 / 2.4) - 0.055;
    return float3(color.r <= 0.0031308 ? low.r : high.r,
                  color.g <= 0.0031308 ? low.g : high.g,
                  color.b <= 0.0031308 ? low.b : high.b);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    const int3 pixel = int3(id.xy, 0);
    const float4 source = DlaaColor.Load(pixel);
    float3 proxy = saturate(source.rgb);
    if (ControlCompatibleColor != 0)
    {
        const float paperWhite = max(ScenePaperWhiteScale, 0.05);
        const float3 transferred = SrgbEncode(max(source.rgb / paperWhite, 0.0));
        proxy = lerp(proxy, transferred, saturate(HdrTransferStrength));
    }
    ColorProxy[id.xy] = float4(proxy, source.a);
    GuidePack[id.xy] = float4(DecodedMotion.Load(pixel), DeviceDepth.Load(pixel), 0.0);
}
