// pgr4_debug_solid_vs: the draws of one vertex shader in solid magenta, to see
// where their geometry lands whatever the guest pixel shader would do.
float4 main(float4 pos : SV_Position) : SV_Target
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
