Texture2D tex : register(t0);

SamplerState splr;

float4 main_PS_Sub(float2 tc : TEXCOORD) : SV_TARGET
{
    float4 pixel = tex.Sample(splr, tc);
    return pixel;
}