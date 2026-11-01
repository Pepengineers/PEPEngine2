// Compact two-sided triangle-list grass in the shared PBR vertex arena.
cbuffer Parameters : register(b0)
{
    float4x4 World;
    float4x4 InverseWorld;
    float4x4 PreviousWorld;
    float4x4 PreviousInverseWorld;
    float4 Patch; // local XZ center and half size
    float4 Shape; // height, width, bend response, time
    float4 PreviousShape;
    float4 Lod; // near, far, maximum distance
    float4 Eye;
    float4 WindDomain; // world XZ center, half extent
    float4 PreviousWindDomain;
    uint VertexBase; uint PreviousBase; uint InstanceID; uint TuftCount;
    uint Seed; uint BladeCount; uint SegmentCount; uint ResetMotion;
    uint WindGrid; uint PreviousWindGrid; uint WindEnabled; uint Reserved;
};
StructuredBuffer<float2> Wind : register(t0);
StructuredBuffer<float2> PreviousWind : register(t1);
struct Vertex { float3 Position; float3 Normal; float2 UV; float3 Tangent; };
RWStructuredBuffer<Vertex> Vertices : register(u0);
RWStructuredBuffer<float3> PreviousPositions : register(u1);
RWStructuredBuffer<uint> Counts : register(u2);
uint Hash(uint v) { v^=v>>16; v*=0x7feb352d; v^=v>>15; v*=0x846ca68b; return v^(v>>16); }
float Random(uint v) { return (Hash(v)>>8)*(1.0/16777216.0); }
float2 Field(float3 root, bool previous)
{
    if(WindEnabled==0) return 0;
    float4 domain=previous?PreviousWindDomain:WindDomain;
    uint grid=previous?PreviousWindGrid:WindGrid;
    float2 uv=((previous?mul(float4(root,1),PreviousWorld):mul(float4(root,1),World)).xz-domain.xy)/(2*domain.z)+.5;
    if(any(uv<0)||any(uv>1)) return 0;
    float2 p=clamp(uv*grid-.5,0,float(grid)-1); uint2 a=uint2(floor(p)),b=min(a+1,grid-1); float2 f=frac(p);
    float2 v00=previous?PreviousWind[a.y*grid+a.x]:Wind[a.y*grid+a.x];
    float2 v10=previous?PreviousWind[a.y*grid+b.x]:Wind[a.y*grid+b.x];
    float2 v01=previous?PreviousWind[b.y*grid+a.x]:Wind[b.y*grid+a.x];
    float2 v11=previous?PreviousWind[b.y*grid+b.x]:Wind[b.y*grid+b.x];
    float2 v=lerp(lerp(v00,v10,f.x),lerp(v01,v11,f.x),f.y);
    return (previous?mul(float4(v.x,0,v.y,0),PreviousInverseWorld):mul(float4(v.x,0,v.y,0),InverseWorld)).xz;
}
float3 Point(float3 root,float3 side,float t,float edge,float4 shape,float2 wind,float phase,bool card)
{
    float speed=length(wind);
    float2 bend=speed>1e-6?wind/speed:0;
    float displacement=shape.x*.75*(1-exp(-speed*.01*shape.z));
    displacement*=1+.15*sin(shape.w*1.7+phase);
    float horizontal=displacement*t*t;
    float vertical=sqrt(max(shape.x*shape.x*t*t-horizontal*horizontal,0));
    float width=shape.y*(card?1.0:lerp(1,.02,t));
    return root+side*width*edge+float3(bend.x*horizontal,vertical,bend.y*horizontal);
}
[numthreads(1,1,1)]
void ClearCount() { Counts[InstanceID]=0; }
[numthreads(64,1,1)]
void Generate(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=TuftCount) return;
    uint seed=Hash(Seed^id.x);
    float3 root=float3(Patch.x+(Random(seed)-.5)*2*Patch.z,0,Patch.y+(Random(seed+1)-.5)*2*Patch.w);
    float distanceToEye=length(mul(float4(root,1),World).xyz-Eye.xyz);
    if(distanceToEye>Lod.z) return;
    bool card=distanceToEye>Lod.y;
    uint blades=card?1:(distanceToEye>Lod.x?max(1,BladeCount/2):BladeCount);
    uint segments=card?1:(distanceToEye>Lod.x?2:SegmentCount);
    uint count=blades*segments*12,base;
    InterlockedAdd(Counts[InstanceID],count,base);
    float2 wind=Field(root,false),previousWind=ResetMotion!=0?wind:Field(root,true);
    float4 previousShape=ResetMotion!=0?Shape:PreviousShape;
    for(uint blade=0;blade<blades;++blade)
    {
        float angle=Random(seed+blade+2)*6.2831853;
        float3 side=float3(cos(angle),0,sin(angle));
        float phase=Random(seed+blade+11)*6.2831853;
        for(uint segment=0;segment<segments;++segment)
        {
            float t0=float(segment)/segments,t1=float(segment+1)/segments;
            float3 p[4],old[4]; float2 uv[4];
            for(uint corner=0;corner<4;++corner)
            {
                float t=corner>=2?t1:t0,edge=(corner&1)!=0?1:-1;
                p[corner]=Point(root,side,t,edge,Shape,wind,phase,card);
                old[corner]=ResetMotion!=0?p[corner]:Point(root,side,t,edge,previousShape,previousWind,phase,card);
                uv[corner]=float2((edge+1)*.5,1-t);
            }
            uint corners[6]={0,1,2,1,3,2};
            float3 normal=cross(p[1]-p[0],p[2]-p[0]); normal*=rsqrt(max(dot(normal,normal),1e-20));
            for(uint back=0;back<2;++back) for(uint v=0;v<6;++v)
            {
                uint corner=corners[back!=0?(v/3)*3+(2-v%3):v];
                uint dst=base+(blade*segments+segment)*12+back*6+v;
                Vertex output; output.Position=p[corner]; output.Normal=back!=0?-normal:normal; output.UV=uv[corner]; output.Tangent=side;
                Vertices[VertexBase+dst]=output; PreviousPositions[PreviousBase+dst]=old[corner];
            }
        }
    }
}
