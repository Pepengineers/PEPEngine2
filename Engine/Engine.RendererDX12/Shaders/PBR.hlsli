#ifndef PBR_HLSLI
#define PBR_HLSLI
#include "PBRSurface.hlsli"
#include "PBRLighting.hlsli"
float3 ShadePBR(Material material, float3 albedo, float3 position, float3 normal, float2 uv)
{
    PBRSurface s = SamplePBRSurface(material, albedo, position, normal, uv);
    float3 v = SafeNormalize(CBCamera.CameraLocation - position);
    float3 result = ShadeSunAndIBL(s, position, v);
    for (uint i = 0; i < CBMain.LocalLightCount; ++i)
        result += ShadeLocalLight(s, position, v, LocalLights[i]);
    return result;
}
#endif
