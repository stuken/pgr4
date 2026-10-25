// Occlusion query count draws (gpu/occlusion.cpp): swapped in for the guest
// pixel shader of a counted draw's copy. [earlydepthstencil] runs it only for
// pixels that pass the depth and stencil tests, so its invocations are the
// passing pixels whatever the guest shader would discard. The pipeline keeps
// the target (ZERO/ONE), so the output is never seen.
[earlydepthstencil]
float4 main(float4 pos : SV_Position) : SV_Target
{
    return float4(0.0, 0.0, 0.0, 0.0);
}
