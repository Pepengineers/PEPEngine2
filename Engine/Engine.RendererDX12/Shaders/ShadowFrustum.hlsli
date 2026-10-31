#ifndef SHADOW_FRUSTUM_HLSLI
#define SHADOW_FRUSTUM_HLSLI
bool ShadowBoxVisible(float3 center, float3 extents, float4x4 m)
{
    float4 planes[6] = {
        float4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41),
        float4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41),
        float4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42),
        float4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42),
        float4(m._13, m._23, m._33, m._43),
        float4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43) };
    for (uint p = 0; p < 6; ++p)
        if (dot(float4(center, 1), planes[p]) + dot(extents, abs(planes[p].xyz)) < 0) return false;
    return true;
}
#endif
