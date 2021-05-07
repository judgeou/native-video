// PixelShader.hlsl
Texture2D<float4> starTexture : t0;

SamplerState splr;

float4 main_PS(float2 tc : TEXCOORD) : SV_TARGET
{
    float4 color = starTexture.Sample(splr, tc);
    return color;
}