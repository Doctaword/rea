// framegen compute shaders. Compiled at runtime with D3DCompile (cs_5_0).
// Entry points: CSLuma, CSDownsample, CSMotion, CSSynthesize, CSOutput.
// The host defines PASS_<ENTRY> for each compile so every pass can bind its
// own resources to the same registers.

cbuffer Params : register(b0)
{
    float2 srcSize;      // size of the texture being read (pixels)
    float2 dstSize;      // size of the texture being written (pixels)
    float2 parentSize;   // CSMotion: parent flow grid size, 0 when coarsest
    float  t;            // CSSynthesize: time of the generated frame in (0,1)
    int    searchRadius; // CSMotion: exhaustive radius at the coarsest level
    float4 dstRect;      // CSOutput: xy = image origin, zw = image size in dst pixels
    float  sharpness;    // CSOutput: 0 = off, 1 = strong
    float  occlusionThreshold;
    float2 flowUvScale;  // image UV -> flow grid UV for the grid being read
};

SamplerState linearClamp : register(s0);
SamplerState pointClamp  : register(s1);

// ---------------------------------------------------------------------------
// Luma pyramid

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

#if defined(PASS_CSLUMA)
Texture2D<float4> colorIn : register(t0);
RWTexture2D<float> lumaOut : register(u0);

// Full-resolution color -> half-resolution luma (2x2 box).
[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / dstSize;
    lumaOut[id.xy] = Luma(colorIn.SampleLevel(linearClamp, uv, 0).rgb);
}

#endif

#if defined(PASS_CSDOWNSAMPLE)
Texture2D<float>  lumaIn  : register(t0);
RWTexture2D<float> lumaOut : register(u0);

// Luma level -> next coarser level (2x2 box through bilinear fetch).
[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / dstSize;
    lumaOut[id.xy] = lumaIn.SampleLevel(linearClamp, uv, 0);
}

#endif

// ---------------------------------------------------------------------------
// Motion estimation: one vector per 8x8 block of the current pyramid level.
// Vectors are in UV units and map a position in "from" to its match in "to":
// from(x) ~= to(x + v).

#if defined(PASS_CSMOTION)
#define BLOCK 8

Texture2D<float>  lumaFrom   : register(t0);
Texture2D<float>  lumaTo     : register(t1);
Texture2D<float2> parentFlow : register(t2);
RWTexture2D<float2> flowOut  : register(u0);

float BlockCost(float2 blockOriginPx, float2 v)
{
    float2 texel = 1.0 / srcSize;
    float sad = 0;
    [unroll] for (int y = 0; y < BLOCK; ++y)
    [unroll] for (int x = 0; x < BLOCK; ++x)
    {
        float2 uv = (blockOriginPx + float2(x, y) + 0.5) * texel;
        sad += abs(lumaFrom.SampleLevel(pointClamp, uv, 0) -
                   lumaTo.SampleLevel(linearClamp, uv + v, 0));
    }
    return sad / (BLOCK * BLOCK);
}

[numthreads(8, 8, 1)]
void CSMotion(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 texel = 1.0 / srcSize;
    float2 origin = float2(id.xy) * BLOCK;
    float2 centerUv = (origin + BLOCK * 0.5) * texel;

    const float smoothWeight = 0.02;  // per pixel of deviation from the parent
    const float zeroBias = 0.004;     // favours exact zero motion for static HUDs

    float2 best = 0;
    float bestCost = BlockCost(origin, 0) - zeroBias;

    if (parentSize.x == 0)
    {
        // Coarsest level: exhaustive search.
        for (int dy = -searchRadius; dy <= searchRadius; ++dy)
        for (int dx = -searchRadius; dx <= searchRadius; ++dx)
        {
            float2 v = float2(dx, dy) * texel;
            float c = BlockCost(origin, v);
            if (c < bestCost) { bestCost = c; best = v; }
        }
    }
    else
    {
        float2 parentTexel = 1.0 / parentSize;
        float2 parentUv = centerUv * flowUvScale;
        float2 parent = parentFlow.SampleLevel(linearClamp, parentUv, 0);
        float2 candidates[5] = {
            parent,
            parentFlow.SampleLevel(pointClamp, parentUv + float2(-parentTexel.x, 0), 0),
            parentFlow.SampleLevel(pointClamp, parentUv + float2( parentTexel.x, 0), 0),
            parentFlow.SampleLevel(pointClamp, parentUv + float2(0, -parentTexel.y), 0),
            parentFlow.SampleLevel(pointClamp, parentUv + float2(0,  parentTexel.y), 0),
        };
        [unroll] for (int i = 0; i < 5; ++i)
        {
            float2 v = candidates[i];
            float c = BlockCost(origin, v) + smoothWeight * length((v - parent) * srcSize);
            if (c < bestCost) { bestCost = c; best = v; }
        }

        // Refine around the winner with a +-1 pixel search, twice.
        [unroll] for (int pass = 0; pass < 2; ++pass)
        {
            float2 center = best;
            [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0) continue;
                float2 v = center + float2(dx, dy) * texel;
                float c = BlockCost(origin, v) + smoothWeight * length((v - parent) * srcSize);
                if (c < bestCost) { bestCost = c; best = v; }
            }
        }
    }
    flowOut[id.xy] = best;
}

#endif

// ---------------------------------------------------------------------------
// Frame synthesis at time t between prev (t=0) and cur (t=1).

#if defined(PASS_CSSYNTHESIZE)
Texture2D<float4> prevColor : register(t0);
Texture2D<float4> curColor  : register(t1);
Texture2D<float2> fwdFlow   : register(t2); // prev -> cur
Texture2D<float2> bwdFlow   : register(t3); // cur -> prev
RWTexture2D<float4> synthOut : register(u0);

[numthreads(8, 8, 1)]
void CSSynthesize(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / dstSize;

    // Forward field: motion w from prev to cur. The in-between pixel came from
    // prev at uv - t*w; one fixed-point step estimates w at that position.
    float2 w = fwdFlow.SampleLevel(linearClamp, uv * flowUvScale, 0);
    w = fwdFlow.SampleLevel(linearClamp, (uv - t * w) * flowUvScale, 0);
    float4 a0 = prevColor.SampleLevel(linearClamp, uv - t * w, 0);
    float4 a1 = curColor.SampleLevel(linearClamp, uv + (1 - t) * w, 0);

    // Backward field: motion b from cur to prev, i.e. forward motion -b.
    float2 b = bwdFlow.SampleLevel(linearClamp, uv * flowUvScale, 0);
    b = bwdFlow.SampleLevel(linearClamp, (uv - (1 - t) * b) * flowUvScale, 0);
    float4 b0 = prevColor.SampleLevel(linearClamp, uv + t * b, 0);
    float4 b1 = curColor.SampleLevel(linearClamp, uv - (1 - t) * b, 0);

    float eA = abs(Luma(a0.rgb) - Luma(a1.rgb));
    float eB = abs(Luma(b0.rgb) - Luma(b1.rgb));

    float4 result;
    if (min(eA, eB) > occlusionThreshold)
    {
        // Neither pairing agrees: occlusion or a failed match. Blending would
        // ghost, so take one sample from the nearer real frame.
        bool useA = eA <= eB;
        result = t < 0.5 ? (useA ? a0 : b0) : (useA ? a1 : b1);
    }
    else
    {
        float wA = 1.0 / (eA + 1e-3);
        float wB = 1.0 / (eB + 1e-3);
        float4 colA = lerp(a0, a1, t);
        float4 colB = lerp(b0, b1, t);
        result = (colA * wA + colB * wB) / (wA + wB);
    }
    synthOut[id.xy] = float4(result.rgb, 1);
}

#endif

// ---------------------------------------------------------------------------
// Output: aspect-preserving Lanczos-2 resample plus clamped sharpening.

#if defined(PASS_CSOUTPUT)
Texture2D<float4> frameIn : register(t0);
RWTexture2D<float4> outputOut : register(u0);

float Lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1;
    if (x >= 2) return 0;
    const float PI = 3.14159265;
    float px = PI * x;
    return 2 * sin(px) * sin(px * 0.5) / (px * px);
}

[numthreads(8, 8, 1)]
void CSOutput(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)dstSize)) return;
    float2 p = float2(id.xy) + 0.5 - dstRect.xy;
    if (any(p < 0) || any(p >= dstRect.zw))
    {
        outputOut[id.xy] = float4(0, 0, 0, 1);
        return;
    }

    float2 srcPos = p / dstRect.zw * srcSize;  // in source pixels
    float2 texel = 1.0 / srcSize;
    float2 base = floor(srcPos - 0.5) + 0.5;
    float2 f = srcPos - base;

    float3 sum = 0;
    float wsum = 0;
    float3 mn = 1e9, mx = -1e9;     // nearest 2x2: clamps Lanczos ringing
    float3 mn4 = 1e9, mx4 = -1e9;   // full 4x4: bounds sharpening
    [unroll] for (int y = -1; y <= 2; ++y)
    [unroll] for (int x = -1; x <= 2; ++x)
    {
        float3 c = frameIn.SampleLevel(pointClamp, (base + float2(x, y)) * texel, 0).rgb;
        float wgt = Lanczos2(x - f.x) * Lanczos2(y - f.y);
        sum += c * wgt;
        wsum += wgt;
        mn4 = min(mn4, c); mx4 = max(mx4, c);
        if (x >= 0 && x <= 1 && y >= 0 && y <= 1) { mn = min(mn, c); mx = max(mx, c); }
    }
    // Clamp to the nearest 2x2 neighbourhood so Lanczos lobes cannot ring.
    float3 color = clamp(sum / wsum, mn, mx);

    if (sharpness > 0)
    {
        float3 blur = 0.25 * (frameIn.SampleLevel(linearClamp, (srcPos + float2(-1, 0)) * texel, 0).rgb +
                       frameIn.SampleLevel(linearClamp, (srcPos + float2( 1, 0)) * texel, 0).rgb +
                       frameIn.SampleLevel(linearClamp, (srcPos + float2(0, -1)) * texel, 0).rgb +
                       frameIn.SampleLevel(linearClamp, (srcPos + float2(0,  1)) * texel, 0).rgb);
        float3 sharpened = color + (color - blur) * (sharpness * 1.5);
        color = clamp(sharpened, mn4, mx4);
    }
    outputOut[id.xy] = float4(saturate(color), 1);
}
#endif
