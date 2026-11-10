Texture2D<float4> Surface : register(t0);
SamplerState LinearClamp : register(s0);

struct Vertex
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

Vertex VSMain(uint id : SV_VertexID)
{
    Vertex vertex;
    vertex.UV = float2((id << 1) & 2, id & 2);
    vertex.Position = float4(vertex.UV * float2(2, -2) + float2(-1, 1), 0, 1);
    return vertex;
}

float4 PSMain(Vertex vertex) : SV_Target
{
    return Surface.Sample(LinearClamp, vertex.UV);
}
