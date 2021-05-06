float4 main_VS(float3 pos : POSITION) : SV_POSITION
{
    return float4(pos, 1);
}