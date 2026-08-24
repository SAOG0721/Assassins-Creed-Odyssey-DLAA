Texture2D<float4> PackedMotion : register(t0);
Texture2D<float> LinearDepth : register(t1);
RWTexture2D<float2> DecodedMotion : register(u0);
RWTexture2D<float> DeviceDepth : register(u1);

cbuffer MotionConstants : register(b0)
{
    float2 FullResSize;
    float2 FullResSizeInv;
    float ZFrontBackValueY;
    float3 MotionPadding;
    float4 MaxViewDepthParams;
    row_major float4x4 ClipXYZToViewPos;
    row_major float4x4 ViewToWorld;
    row_major float4x4 WorldViewProjPrevFrame;
    float Projection22;
    float Projection23;
    float Projection32;
    float ProjectionPadding;
};

groupshared float DepthTile[10][10];

int2 ClampPixel(int2 pixel, uint width, uint height)
{
    return clamp(pixel, int2(0, 0), int2((int)width - 1, (int)height - 1));
}

float2 DecodePackedMotion(int2 pixel)
{
    const uint3 bytes = (uint3)round(saturate(PackedMotion.Load(int3(pixel, 0)).rgb) * 255.0);
    // Odyssey stores X in R plus the high nibble of G, and Y in the low
    // nibble of G plus B. The active TAA DXBC reconstructs the same values as
    // R * 4080 + floor(G * 255 / 16), then frac(G * 255 / 16) * 4096 + B * 255.
    const uint packedX = (bytes.r << 4) | (bytes.g >> 4);
    const uint packedY = ((bytes.g & 15) << 8) | bytes.b;
    const float2 encoded = clamp(float2(packedX, packedY) * (2.0 / 4095.0) - 1.0, -1.0, 1.0);
    const float2 magnitude2 = abs(encoded) * abs(encoded);
    // The retail 745C... TAA permutation preserves the encoded sign here and
    // later resolves history at currentUv - decodedMotion.  The packed value
    // is therefore currentUv - previousUv, matching FarMotion's direction.
    return sign(encoded) * 0.2 * magnitude2 * magnitude2;
}

float2 FarMotion(float2 currentUv)
{
    // Exact high-level form of the active 745C... permutation's far branch:
    // inverse-project a point on the camera ray, transform it to world space,
    // then project it by the previous-frame view-projection matrix.
    const float4 currentClip = float4(
        currentUv.x * 2.0 - 1.0,
        currentUv.y * -2.0 + 1.0,
        ZFrontBackValueY,
        1.0);
    const float4 viewH = mul(ClipXYZToViewPos, currentClip);
    if (abs(viewH.w) < 1e-8)
        return float2(0.0, 0.0);
    const float4 viewPosition = float4(viewH.xyz / viewH.w, 1.0);
    const float3 worldPosition = mul(ViewToWorld, viewPosition).xyz;
    const float4 previousClip = mul(WorldViewProjPrevFrame, float4(worldPosition, 1.0));
    if (abs(previousClip.w) < 1e-8)
        return float2(0.0, 0.0);
    const float2 previousUv = previousClip.xy / previousClip.w * float2(0.5, -0.5) +
        float2(0.5, 0.5);
    return float2(currentUv.x - previousUv.x, currentUv.y - previousUv.y);
}

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint groupIndex : SV_GroupIndex)
{
    uint width;
    uint height;
    DecodedMotion.GetDimensions(width, height);

    // A 10x10 tile covers an 8x8 output group plus its one-pixel halo. All
    // threads participate before the bounds return so edge groups cannot
    // deadlock at the group barrier.
    [unroll]
    for (uint tileIndex = groupIndex; tileIndex < 100; tileIndex += 64)
    {
        const uint tileX = tileIndex % 10;
        const uint tileY = tileIndex / 10;
        const int2 sourcePixel = ClampPixel(
            int2(groupId.xy * 8) + int2(tileX, tileY) - int2(1, 1), width, height);
        DepthTile[tileY][tileX] = LinearDepth.Load(int3(sourcePixel, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const int2 center = int2(dispatchThreadId.xy);
    const int2 tileCenter = int2(groupThreadId.xy) + int2(1, 1);
    const float centerDepth = DepthTile[tileCenter.y][tileCenter.x];
    float minDepth = centerDepth;
    float maxDepth = minDepth;
    int2 minPixel = center;
    int2 maxPixel = center;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            const int2 samplePixel = ClampPixel(center + int2(x, y), width, height);
            const float sampleDepth = DepthTile[tileCenter.y + y][tileCenter.x + x];
            if (sampleDepth < minDepth)
            {
                minDepth = sampleDepth;
                minPixel = samplePixel;
            }
            if (sampleDepth > maxDepth)
            {
                maxDepth = sampleDepth;
                maxPixel = samplePixel;
            }
        }
    }

    // Matches (MaxViewDepthParams.x - .y) / (.z - .w) in the original DXBC.
    const float thresholdDenominator = MaxViewDepthParams.z - MaxViewDepthParams.w;
    const float farThreshold = abs(thresholdDenominator) > 1e-8
        ? (MaxViewDepthParams.x - MaxViewDepthParams.y) / thresholdDenominator
        : 3.402823466e+38;
    const float2 currentUv = (float2(center) + 0.5) * FullResSizeInv;
    float2 resolvedMotion;
    if (minDepth >= farThreshold)
    {
        resolvedMotion = FarMotion(currentUv);
    }
    else
    {
        // The retail TAA permutation selects motion from its 3x3 depth
        // neighborhood. When the near and center candidates disagree by more
        // than two pixels it uses the farthest-depth candidate to avoid pulling
        // foreground motion across a disocclusion edge.
        const float2 minMotion = DecodePackedMotion(minPixel);
        const float2 centerMotion = DecodePackedMotion(center);
        const float2 disagreementPixels = (centerMotion - minMotion) * FullResSize;
        resolvedMotion = dot(disagreementPixels, disagreementPixels) > 4.0
            ? DecodePackedMotion(maxPixel)
            : minMotion;
    }

    // Result is currentUv - previousUv (previous position -> current position)
    // in normalized UVs. The bridge's negative pixel-space scale converts it
    // to the current -> previous convention consumed by Direct NGX.
    DecodedMotion[center] = resolvedMotion;

    // This is the exact projection conversion previously performed by the
    // separate depth compute pass, now sharing the center depth already
    // present in the tile.
    float deviceDepth = 1.0;
    if (centerDepth > 0.0)
    {
        const float denominator = Projection23 * centerDepth;
        if (abs(denominator) > 1e-8)
            deviceDepth = (Projection22 * centerDepth - Projection32) / denominator;
    }
    DeviceDepth[center] = saturate(deviceDepth);
}
