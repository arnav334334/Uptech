#define A_GPU 1
#define A_HLSL 1

#include "../external/FidelityFX-FSR/ffx-fsr/ffx_a.h"

Texture2D<float4> InputTexture : register(t0);

#define FSR_RCAS_F 1

// Load the neighbouring pixels required by RCAS.
AF4 FsrRcasLoadF(ASU2 p)
{
    return InputTexture.Load(int3(p, 0));
}

// No additional input processing is required.
void FsrRcasInputF(
    inout AF1 r,
    inout AF1 g,
    inout AF1 b)
{
}

#include "../external/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h"

// RCAS constants supplied by the C++ application.
cbuffer RCASConstants : register(b0)
{
    uint4 Const0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

float4 PSMain(VSOutput input) : SV_TARGET
{
    float r;
    float g;
    float b;

    uint2 pixelPosition = uint2(input.position.xy);

    FsrRcasF(
        r,
        g,
        b,
        pixelPosition,
        Const0
    );

    return float4(r, g, b, 1.0);
}