Texture2D<float4> DlaaOutput : register(t0);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> SharpenedOutput : register(u0);

cbuffer SharpenConstants : register(b0)
{
    float2 InvOutputSize;
    float Strength;
    float SoftThreshold;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    SharpenedOutput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const int2 pixel = int2(dispatchThreadId.xy);
    const float2 uv = (float2(pixel) + 0.5) * InvOutputSize;
    const float2 halfTexel = 0.5 * InvOutputSize;

    // Four bilinear taps at half-texel diagonals exactly reproduce the
    // normalized 3x3 Gaussian kernel [1 2 1; 2 4 2; 1 2 1] / 16.
    const float4 gaussian = 0.25 * (
        DlaaOutput.SampleLevel(LinearClamp, uv + float2(-halfTexel.x, -halfTexel.y), 0.0) +
        DlaaOutput.SampleLevel(LinearClamp, uv + float2( halfTexel.x, -halfTexel.y), 0.0) +
        DlaaOutput.SampleLevel(LinearClamp, uv + float2(-halfTexel.x,  halfTexel.y), 0.0) +
        DlaaOutput.SampleLevel(LinearClamp, uv + float2( halfTexel.x,  halfTexel.y), 0.0));
    const float4 center = DlaaOutput.Load(int3(pixel, 0));

    const float centerLuma = dot(saturate(center.rgb), LumaWeights);
    const float gaussianLuma = dot(saturate(gaussian.rgb), LumaWeights);
    const float detail = centerLuma - gaussianLuma;
    const float detailMagnitude = abs(detail);
    const float gate = smoothstep(SoftThreshold, SoftThreshold * 4.0, detailMagnitude);
    const float limitedDetail = clamp(detail * gate, -0.25, 0.25);
    const float targetLuma = saturate(centerLuma + Strength * limitedDetail);

    // Scale RGB through luminance to preserve hue. The ratio clamp limits
    // dark-edge ringing in Odyssey's R8G8B8A8 TAA color domain.
    float lumaRatio = centerLuma > 1e-4 ? targetLuma / centerLuma : 1.0;
    lumaRatio = clamp(lumaRatio, 0.75, 1.25);
    SharpenedOutput[pixel] = float4(saturate(center.rgb * lumaRatio), center.a);
}
