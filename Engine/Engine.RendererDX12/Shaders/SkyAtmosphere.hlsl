// Hillaire 2020 LUT model; adapted from sebh/UnrealEngineSkyAtmosphere (MIT, see SkyAtmosphere/LICENSE.txt).
static const float PI=3.14159265358979323846;
cbuffer SkyParameters : register(b1)
{
    row_major float4x4 InverseViewProjection;
    float4 CameraKm;
    float4 SunDirectionRadius;
    float4 SunRadiance;
    float4 RayleighHeight;
    float4 MieHeight;
    float4 MieExtinctionG;
    float4 OzoneMulti;
    float4 GroundRadius;
    float4 Options;
    uint4 Resolutions;
    uint4 Resolutions2;
    float4 CameraWorld;
};
cbuffer DispatchParameters : register(b0) { uint OutputSize; uint SourceMip; uint Flags; uint Unused; };
TextureCube<float4> BoxTexture : register(t0);
Texture2D<float4> TransmittanceTexture : register(t1);
Texture2D<float4> MultiTexture : register(t2);
Texture2D<float4> SkyTexture : register(t3);
Texture3D<float4> AerialLuminance : register(t4);
Texture3D<float4> AerialTransmittance : register(t5);
Texture2D<float4> InputColor : register(t6);
Texture2D<float> InputDepth : register(t7);
RWTexture2DArray<float4> CubeOutput : register(u0);
RWTexture2D<float4> TextureOutput : register(u1);
RWTexture3D<float4> VolumeLuminanceOutput : register(u2);
RWTexture3D<float4> VolumeTransmittanceOutput : register(u3);
SamplerState LinearClamp : register(s0);

float3 ToAtmosphere(float3 v) { return float3(v.x,-v.z,v.y); }
float BoundaryOffset() { return min(max(.01,CameraKm.w),(Options.x-GroundRadius.w)*.25); }
float Sphere(float3 p,float3 d,float radius)
{
    float b=dot(p,d),c=dot(p,p)-radius*radius,disc=b*b-c;
    if(disc<0) return -1;
    float root=sqrt(disc),near=-b-root,far=-b+root;
    return near>=0?near:(far>=0?far:-1);
}
bool MoveToTop(inout float3 p,float3 d,inout float distance)
{
    if(length(p)<Options.x) return true;
    float entry=Sphere(p,d,Options.x);
    if(entry<0||entry+CameraKm.w>=distance) return false;
    p+=d*(entry+CameraKm.w); distance-=entry+CameraKm.w;
    return true;
}
float2 TransmittanceUV(float height,float mu)
{
    height=clamp(height,GroundRadius.w,Options.x);
    float H=sqrt(max(0,Options.x*Options.x-GroundRadius.w*GroundRadius.w));
    float rho=sqrt(max(0,height*height-GroundRadius.w*GroundRadius.w));
    float disc=height*height*(mu*mu-1)+Options.x*Options.x;
    float d=max(0,-height*mu+sqrt(max(0,disc)));
    return saturate(float2((d-(Options.x-height))/max(1e-5,rho+H-(Options.x-height)),rho/H));
}
void Medium(float3 p,out float3 ray,out float3 mie,out float3 extinction)
{
    float altitude=max(0,length(p)-GroundRadius.w);
    ray=RayleighHeight.rgb*exp(-altitude/RayleighHeight.w);
    float densityMie=exp(-altitude/MieHeight.w);
    mie=MieHeight.rgb*densityMie;
    float ozone=saturate(altitude<25?altitude/15-2.0/3.0:8.0/3.0-altitude/15);
    extinction=ray+MieExtinctionG.rgb*densityMie+OzoneMulti.rgb*ozone;
}
float3 SunTransmittance(float3 p,float3 sun)
{
    float3 up=normalize(p);
    if(Sphere(p+up*BoundaryOffset(),sun,GroundRadius.w)>=0) return 0;
    return TransmittanceTexture.SampleLevel(LinearClamp,TransmittanceUV(length(p),dot(up,sun)),0).rgb;
}
struct Scattering { float3 L; float3 T; float3 F; };
Scattering Integrate(float3 p,float3 d,float3 sun,float distance,uint steps,bool isotropic,bool useMulti,bool ground)
{
    Scattering result; result.L=0; result.T=1; result.F=0;
    if(!MoveToTop(p,d,distance)) return result;
    float bottom=Sphere(p,d,GroundRadius.w),top=Sphere(p,d,Options.x);
    float end=top;
    if(bottom>=0) end=top<0?bottom:min(bottom,top);
    if(end<=0) return result;
    end=min(end,distance);
    float cosTheta=dot(d,sun),g=MieExtinctionG.w;
    float rayPhase=3*(1+cosTheta*cosTheta)/(16*PI);
    float miePhase=3*(1-g*g)*(1+cosTheta*cosTheta)/(8*PI*(2+g*g)*pow(max(.001,1+g*g-2*g*cosTheta),1.5));
    for(uint i=0;i<steps;++i)
    {
        float a=float(i)/steps,b=float(i+1)/steps;
        float dt=end*(b*b-a*a);
        float3 samplePos=p+d*end*lerp(a*a,b*b,.3);
        float3 ray,mie,extinction; Medium(samplePos,ray,mie,extinction);
        float3 T=exp(-extinction*dt);
        float3 integral=(1-T)/max(extinction,1e-8);
        float3 scattering=ray+mie;
        float3 phaseScattering=isotropic?scattering/(4*PI):ray*rayPhase+mie*miePhase;
        float3 multi=0;
        if(useMulti)
        {
            float height=length(samplePos);
            float2 uv=float2(dot(samplePos/height,sun)*.5+.5,(height-GroundRadius.w)/(Options.x-GroundRadius.w));
            uv=(saturate(uv)*(Resolutions.z-1)+.5)/Resolutions.z;
            multi=MultiTexture.SampleLevel(LinearClamp,uv,0).rgb*scattering;
        }
        result.L+=result.T*(SunTransmittance(samplePos,sun)*phaseScattering+multi)*integral;
        result.F+=result.T*scattering*integral;
        result.T*=T;
    }
    if(ground&&bottom>=0&&abs(end-bottom)<BoundaryOffset())
    {
        float3 pos=p+d*bottom,normal=normalize(pos);
        result.L+=result.T*SunTransmittance(pos+normal*BoundaryOffset(),sun)*saturate(dot(normal,sun))*GroundRadius.rgb/PI;
    }
    return result;
}
[numthreads(8,8,1)]
void TransmittanceCS(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=Resolutions.xy)) return;
    float2 uv=(id.xy+.5)/Resolutions.xy;
    float H=sqrt(Options.x*Options.x-GroundRadius.w*GroundRadius.w),rho=H*uv.y;
    float height=sqrt(rho*rho+GroundRadius.w*GroundRadius.w);
    float dMin=Options.x-height,dMax=rho+H,d=lerp(dMin,dMax,uv.x);
    float mu=d==0?1:(H*H-rho*rho-d*d)/(2*height*d);
    float3 p=float3(0,0,height),direction=float3(0,sqrt(saturate(1-mu*mu)),mu),opticalDepth=0;
    float lengthToTop=max(0,Sphere(p,direction,Options.x));
    uint steps=Resolutions2.z*2;
    for(uint i=0;i<steps;++i) { float3 ray,mie,extinction; Medium(p+direction*lengthToTop*(i+.5)/steps,ray,mie,extinction); opticalDepth+=extinction*(lengthToTop/steps); }
    TextureOutput[id.xy]=float4(saturate(exp(-opticalDepth)),1);
}
groupshared float3 MultiL[64];
groupshared float3 MultiF[64];
[numthreads(1,1,64)]
void MultiScatteringCS(uint3 id:SV_DispatchThreadID)
{
    float2 uv=float2(id.xy)/max(1.0,Resolutions.z-1.0);
    float mu=uv.x*2-1;
    float3 sun=float3(0,sqrt(saturate(1-mu*mu)),mu);
    float offset=BoundaryOffset();
    float height=GroundRadius.w+offset+uv.y*(Options.x-GroundRadius.w-2*offset);
    float z=1-2*(float(id.z%8)+.5)/8,phi=2*PI*(float(id.z/8)+.5)/8;
    float3 direction=float3(sqrt(saturate(1-z*z))*cos(phi),sqrt(saturate(1-z*z))*sin(phi),z);
    Scattering value=Integrate(float3(0,0,height),direction,sun,1e6,20,true,false,true);
    MultiL[id.z]=value.L/64; MultiF[id.z]=value.F/64;
    GroupMemoryBarrierWithGroupSync();
    for(uint count=32;count>0;count>>=1) { if(id.z<count) { MultiL[id.z]+=MultiL[id.z+count]; MultiF[id.z]+=MultiF[id.z+count]; } GroupMemoryBarrierWithGroupSync(); }
    if(id.z==0) TextureOutput[id.xy]=float4(min(65504,MultiL[0]/max(.001,1-MultiF[0])*OzoneMulti.w),1);
}
float ToSub(float u,float resolution) { return (u*(resolution-1)+.5)/resolution; }
float FromSub(float u,float resolution) { return saturate((u*resolution-.5)/(resolution-1)); }
void SkyAngles(float2 uv,float height,out float zenith,out float lightView)
{
    uv=float2(FromSub(uv.x,Resolutions.w),FromSub(uv.y,Resolutions2.x));
    float beta=acos(sqrt(max(0,height*height-GroundRadius.w*GroundRadius.w))/height),horizon=PI-beta;
    if(uv.y<.5) { float c=1-2*uv.y; zenith=cos(horizon*(1-c*c)); }
    else { float c=2*uv.y-1; zenith=cos(horizon+beta*c*c); }
    lightView=1-2*uv.x*uv.x;
}
float2 SkyUV(float3 p,float3 d,float3 sun)
{
    float height=length(p); float3 up=p/height;
    float zenith=clamp(dot(d,up),-1,1),beta=acos(sqrt(max(0,height*height-GroundRadius.w*GroundRadius.w))/height),horizon=PI-beta;
    float3 projectedView=d-up*zenith,projectedSun=sun-up*dot(sun,up);
    float projectedAzimuthMagnitudeProduct=length(projectedView)*length(projectedSun);
    const float alignedAzimuthCosine=1;
    float lightView=projectedAzimuthMagnitudeProduct>1e-6?clamp(dot(projectedView,projectedSun)/projectedAzimuthMagnitudeProduct,-1,1):alignedAzimuthCosine;
    float y;
    if(Sphere(p,d,GroundRadius.w)<0) y=(1-sqrt(saturate(1-acos(zenith)/horizon)))*.5;
    else y=(1+sqrt(saturate((acos(zenith)-horizon)/beta)))*.5;
    return float2(ToSub(sqrt(saturate((1-lightView)*.5)),Resolutions.w),ToSub(y,Resolutions2.x));
}
[numthreads(8,8,1)]
void SkyViewCS(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=Resolutions.w||id.y>=Resolutions2.x) return;
    float height=clamp(length(CameraKm.xyz),GroundRadius.w+CameraKm.w,Options.x-CameraKm.w),zenith,lightView;
    SkyAngles((id.xy+.5)/float2(Resolutions.w,Resolutions2.x),height,zenith,lightView);
    float sunMu=clamp(dot(normalize(CameraKm.xyz),SunDirectionRadius.xyz),-1,1);
    float3 sun=float3(sqrt(saturate(1-sunMu*sunMu)),0,sunMu);
    float sinZenith=sqrt(saturate(1-zenith*zenith));
    float3 d=float3(sinZenith*lightView,sinZenith*sqrt(saturate(1-lightView*lightView)),zenith);
    Scattering value=Integrate(float3(0,0,height),d,sun,1e6,Resolutions2.z,false,true,false);
    TextureOutput[id.xy]=float4(min(65504,value.L*SunRadiance.rgb),1);
}
float3 CubeDirection(uint3 id)
{
    float2 uv=(id.xy+.5)/OutputSize*2-1;
    if(id.z==0) return normalize(float3(1,-uv.y,-uv.x));
    if(id.z==1) return normalize(float3(-1,-uv.y,uv.x));
    if(id.z==2) return normalize(float3(uv.x,1,uv.y));
    if(id.z==3) return normalize(float3(uv.x,-1,-uv.y));
    if(id.z==4) return normalize(float3(uv.x,-uv.y,1));
    return normalize(float3(-uv.x,-uv.y,-1));
}
[numthreads(8,8,1)]
void CubeCS(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=OutputSize)||id.z>=6) return;
    float3 direction=CubeDirection(id),L=0;
    if(Resolutions2.w==1)
    {
        float s=sin(Options.w),c=cos(Options.w);
        direction=float3(c*direction.x+s*direction.z,direction.y,-s*direction.x+c*direction.z);
        L=BoxTexture.SampleLevel(LinearClamp,direction,SourceMip).rgb*Options.z;
    }
    else if(Resolutions2.w==2)
    {
        direction=ToAtmosphere(direction);
        if(length(CameraKm.xyz)<Options.x) L=SkyTexture.SampleLevel(LinearClamp,SkyUV(CameraKm.xyz,direction,SunDirectionRadius.xyz),0).rgb;
        else L=Integrate(CameraKm.xyz,direction,SunDirectionRadius.xyz,1e6,Resolutions2.z,false,true,false).L*SunRadiance.rgb;
        // The direct light already carries the solar disk energy; only the visible sky gets its disk.
        if(Flags!=0&&dot(direction,SunDirectionRadius.xyz)>=cos(SunDirectionRadius.w)&&Sphere(CameraKm.xyz,direction,GroundRadius.w)<0)
            L+=SunTransmittance(CameraKm.xyz,SunDirectionRadius.xyz)*SunRadiance.rgb/(PI*SunDirectionRadius.w*SunDirectionRadius.w);
    }
    CubeOutput[id]=float4(clamp(L,0,65504),1);
}
float3 WorldRay(float2 uv)
{
    float4 world=mul(float4(uv*float2(2,-2)+float2(-1,1),.5,1),InverseViewProjection);
    return normalize(world.xyz/world.w-CameraWorld.xyz);
}
[numthreads(4,4,4)]
void AerialVolumeCS(uint3 id:SV_DispatchThreadID)
{
    if(any(id>=Resolutions2.y)) return;
    float slice=(float(id.z)+.5)/Resolutions2.y;
    float distance=slice*slice*Options.y;
    float3 d=ToAtmosphere(WorldRay((id.xy+.5)/Resolutions2.y));
    Scattering value=Integrate(CameraKm.xyz,d,SunDirectionRadius.xyz,distance,Resolutions2.z,false,true,false);
    VolumeLuminanceOutput[id]=float4(min(65504,value.L*SunRadiance.rgb),1);
    VolumeTransmittanceOutput[id]=float4(saturate(value.T),1);
}
[numthreads(8,8,1)]
void ApplyAerialCS(uint3 id:SV_DispatchThreadID)
{
    uint width,height; InputColor.GetDimensions(width,height); if(id.x>=width||id.y>=height) return;
    float4 color=InputColor[id.xy]; float depth=InputDepth[id.xy];
    if(depth>0)
    {
        float2 uv=(id.xy+.5)/float2(width,height);
        float4 world=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),InverseViewProjection);
        float distance=length(world.xyz/world.w-CameraWorld.xyz)*1e-5;
        float coord=sqrt(saturate(distance/Options.y));
        float firstSlice=.5/Resolutions2.y;
        float weight=saturate(coord*coord/(firstSlice*firstSlice));
        float3 sampleUV=float3(uv,max(coord,firstSlice));
        float3 L=AerialLuminance.SampleLevel(LinearClamp,sampleUV,0).rgb*weight;
        float3 T=lerp(1,AerialTransmittance.SampleLevel(LinearClamp,sampleUV,0).rgb,weight);
        color.rgb=color.rgb*T+L;
    }
    TextureOutput[id.xy]=float4(clamp(color.rgb,0,65504),color.a);
}
