struct Vertex
{
    float3 Position;
    float3 Normal;
    float2 UV;
    float3 Tangent;
    float3 PreviousPosition;
};
struct Influence { uint Bone; float Weight; };
struct Palette { row_major float4x4 Current; row_major float4x4 Previous; };

cbuffer Parameters : register(b0) { uint VertexCount; uint OutputVertex; uint BoneCount; };
StructuredBuffer<Vertex> Vertices : register(t0);
StructuredBuffer<uint2> Ranges : register(t1);
StructuredBuffer<Influence> Influences : register(t2);
StructuredBuffer<Palette> Bones : register(t3);
RWByteAddressBuffer Output : register(u0);

float3 SafeNormal(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1e-20 ? value * rsqrt(lengthSquared) : fallback;
}

[numthreads(64, 1, 1)]
void CS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint vertexIndex = (group.y * 65535 + group.x) * 64 + lane;
    if (vertexIndex >= VertexCount) return;
    Vertex source = Vertices[vertexIndex];
    uint2 range = Ranges[vertexIndex];
    float4x4 current = 0;
    float4x4 previous = 0;
    if (range.y == 0)
    {
        current = Bones[BoneCount - 1].Current;
        previous = Bones[BoneCount - 1].Previous;
    }
    else
    {
        for (uint i = 0; i < range.y; ++i)
        {
            Influence influence = Influences[range.x + i];
            current += Bones[influence.Bone].Current * influence.Weight;
            previous += Bones[influence.Bone].Previous * influence.Weight;
        }
    }
    float3 position = mul(float4(source.Position, 1), current).xyz;
    float3 previousPosition = mul(float4(source.Position, 1), previous).xyz;
    float3x3 deformation = (float3x3)current;
    float3x3 cofactor = float3x3(cross(deformation[1], deformation[2]),
        cross(deformation[2], deformation[0]), cross(deformation[0], deformation[1]));
    float determinant = dot(deformation[0], cofactor[0]);
    float3 bindNormal = SafeNormal(source.Normal, float3(0, 1, 0));
    // Dividing by determinant preserves normal orientation under reflected scales.
    float3 normal = abs(determinant) > 1e-12
        ? SafeNormal(mul(bindNormal, cofactor) / determinant, bindNormal)
        : SafeNormal(mul(bindNormal, deformation), bindNormal);
    float3 tangent = mul(source.Tangent, deformation);
    float3 tangentAxis = abs(normal.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 fallbackTangent = normalize(cross(tangentAxis, normal));
    tangent = SafeNormal(tangent - normal * dot(normal, tangent), fallbackTangent);
    uint address = (OutputVertex + vertexIndex) * 56;
    Output.Store3(address, asuint(position));
    Output.Store3(address + 12, asuint(normal));
    Output.Store2(address + 24, asuint(source.UV));
    Output.Store3(address + 32, asuint(tangent));
    Output.Store3(address + 44, asuint(previousPosition));
}
