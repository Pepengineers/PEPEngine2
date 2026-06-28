struct VSInput
{
    uint VertexID : SV_VertexID;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

VSOutput VS(VSInput input)
{
    VSOutput output;
    
    float2 texCoord = float2((input.VertexID << 1) & 2, input.VertexID & 2);
    output.Position = float4(texCoord * 2.0f - 1.0f, 0.0f, 1.0f);
    output.TexCoord = float2(texCoord.x, 1.0f - texCoord.y);
    return output;
}