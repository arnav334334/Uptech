
#define A_GPU 1
#define A_HLSL 1

#include "../external/FidelityFX-FSR/ffx-fsr/ffx_a.h"

Texture2D<float4> InputTexture : register(t0);
SamplerState samLinearClamp : register(s0);

#define FSR_EASU_F 1

AF4 FsrEasuRF(AF2 p)
{
    return InputTexture.GatherRed(
        samLinearClamp, p, int2(0, 0)
    );
}

AF4 FsrEasuGF(AF2 p)
{
    return InputTexture.GatherGreen(
        samLinearClamp, p, int2(0, 0)
    );
}

AF4 FsrEasuBF(AF2 p)
{
    return InputTexture.GatherBlue(
        samLinearClamp, p, int2(0, 0)
    );
}

#include "../external/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h"

cbuffer EASUConstants : register(b0)
{
    uint4 Const0;
    uint4 Const1;
    uint4 Const2;
    uint4 Const3;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 color;

    uint2 pixelPosition = uint2(input.position.xy);

    FsrEasuF(
        color,
        pixelPosition,
        Const0,
        Const1,
        Const2,
        Const3
    );

    return float4(color, 1.0);
}
