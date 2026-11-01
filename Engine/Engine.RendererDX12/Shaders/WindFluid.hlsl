cbuffer Parameters : register(b0)
{
    uint Grid; uint Reserved0; uint Reserved1; uint Reserved2;
    float Dt; float CellSize; float Decay; float Force;
    float2 Direction; float2 Reserved3;
};
StructuredBuffer<float2> Input : register(t0);
StructuredBuffer<float2> Auxiliary : register(t1);
RWStructuredBuffer<float2> Output : register(u0);
uint Address(int2 p) { p=clamp(p,0,int(Grid)-1); return p.y*Grid+p.x; }
float2 Velocity(int2 p) { return Input[Address(p)]; }
float Pressure(int2 p) { return Auxiliary[Address(p)].x; }
float2 Bilinear(float2 p)
{
    p=clamp(p,0,float(Grid)-1); int2 a=int2(floor(p)); float2 f=frac(p);
    return lerp(lerp(Velocity(a),Velocity(a+int2(1,0)),f.x),lerp(Velocity(a+int2(0,1)),Velocity(a+1),f.x),f.y);
}
[numthreads(8,8,1)]
void Clear(uint3 id : SV_DispatchThreadID) { if(any(id.xy>=Grid)) return; Output[Address(id.xy)]=0; }
[numthreads(8,8,1)]
void Inject(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Grid)) return; int2 p=id.xy;
    float2 uv=(float2(p)+.5)/Grid;
    float2 v=Bilinear(float2(p)-Dt*Velocity(p)/CellSize)*exp(-Decay*Dt);
    float envelope=sin(3.14159265*uv.x)*sin(3.14159265*uv.y);
    v+=Direction*Force*Dt*envelope;
    if(p.x==int(Grid)-1) v.x=0; if(p.y==int(Grid)-1) v.y=0;
    Output[Address(p)]=v;
}
[numthreads(8,8,1)]
void Divergence(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Grid)) return; int2 p=id.xy;
    float left=p.x>0?Velocity(p-int2(1,0)).x:0;
    float down=p.y>0?Velocity(p-int2(0,1)).y:0;
    float2 v=Velocity(p); Output[Address(p)]=float2((v.x-left+v.y-down)/CellSize,0);
}
[numthreads(8,8,1)]
void Jacobi(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Grid)) return; int2 p=id.xy;
    float sum=Pressure(p-int2(1,0))+Pressure(p+int2(1,0))+Pressure(p-int2(0,1))+Pressure(p+int2(0,1));
    Output[Address(p)]=float2((sum-Input[Address(p)].x*CellSize*CellSize)*.25,0);
}
[numthreads(8,8,1)]
void Project(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=Grid)) return; int2 p=id.xy; float center=Pressure(p);
    float2 v=Velocity(p)-float2(Pressure(p+int2(1,0))-center,Pressure(p+int2(0,1))-center)/CellSize;
    if(p.x==int(Grid)-1) v.x=0; if(p.y==int(Grid)-1) v.y=0;
    Output[Address(p)]=v;
}
