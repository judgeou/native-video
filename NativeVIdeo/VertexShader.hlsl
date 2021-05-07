// VertexShader.hlsl
struct VSOut
{
    float2 tex : TEXCOORD;
    float4 pos : SV_POSITION;
};

VSOut main_VS(float3 pos : POSITION, float2 tex : TEXCOORD)
{
    VSOut vsout;
    vsout.pos = float4(pos.x, pos.y, pos.z, 1);
    vsout.tex = tex;
    return vsout;
}