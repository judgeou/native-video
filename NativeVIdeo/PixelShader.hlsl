// PixelShader.hlsl

float4 main_PS(float2 tc : TEXCOORD) : SV_TARGET
{
    float4 color = float4(1, tc.x, tc.y, 1);
    return color;
}