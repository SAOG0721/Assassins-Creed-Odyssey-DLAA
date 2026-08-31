Texture2D<float4> OriginalDlaa : register(t0);
Texture2D<float4> ColorProxy : register(t1);
Texture2D<float4> NeuralColor : register(t2);
RWTexture2D<float4> DecodedColor : register(u0);

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

float Luminance(float3 color)
{
    return dot(color, float3(0.212639, 0.715169, 0.072192));
}
float3 SrgbDecode(float3 color)
{
    color = saturate(color);
    const float3 low = color / 12.92;
    const float3 high = pow((color + 0.055) / 1.055, 2.4);
    return float3(color.r <= 0.04045 ? low.r : high.r,
                  color.g <= 0.04045 ? low.g : high.g,
                  color.b <= 0.04045 ? low.b : high.b);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    const int3 pixel = int3(id.xy, 0);
    const float4 original = OriginalDlaa.Load(pixel);
    const float3 neuralRaw = saturate(NeuralColor.Load(pixel).rgb);
    float3 result = neuralRaw;
    if (ControlCompatibleColor != 0)
    {
        const float transfer = saturate(HdrTransferStrength);
        const float paperWhite = max(ScenePaperWhiteScale, 0.05);
        const float3 proxyRaw = saturate(ColorProxy.Load(pixel).rgb);
        const float3 proxy = lerp(proxyRaw, SrgbDecode(proxyRaw), transfer);
        const float3 neural = lerp(neuralRaw, SrgbDecode(neuralRaw), transfer);
        const float3 scaledOriginal = max(original.rgb / paperWhite, 0.0);
        const float3 channelRatio = clamp((neural + 0.02) / (proxy + 0.02), 0.25, 4.0);
        const float luminanceRatio = clamp(
            (Luminance(neural) + 0.02) / (Luminance(proxy) + 0.02), 0.25, 4.0);
        const float3 luminanceOnly = scaledOriginal * luminanceRatio;
        const float3 colorTransfer = scaledOriginal * channelRatio;
        result = lerp(luminanceOnly, colorTransfer, saturate(ColorStrength)) * paperWhite;
    }
    DecodedColor[id.xy] = float4(saturate(result), original.a);
}
