#include "copy_common.hlsli"

// The shader resolve of a multisampled surface, for the cases the hardware
// resolve cannot take: a format conversion, a scale, or a depth source (PGR3
// resolves its 2x scene depth into an R32_FLOAT texture). plume makes a
// Texture2DMS SRV for a multisampled texture, so the heap is redeclared as
// one at the same slot.
Texture2DMS<float4, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

// Param0 scales RGB (the resolve's exponent bias), as in copy_color_ps.
// ResourceDescriptorIndex2 bit 1: a depth source, which takes sample 0 rather
// than an average.
float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2DMS<float4, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    int2 coord = min(int2(texCoord * float2(w, h)), int2(w - 1, h - 1));
    if ((g_PushConstants.ResourceDescriptorIndex2 & 2u) != 0u)
        return tex.Load(coord, 0);
    float4 result = tex.Load(coord, 0);
    [unroll] for (int i = 1; i < SAMPLE_COUNT; i++)
        result += tex.Load(coord, i);
    result /= SAMPLE_COUNT;
    return float4(result.rgb * g_PushConstants.Param0, result.a);
}
