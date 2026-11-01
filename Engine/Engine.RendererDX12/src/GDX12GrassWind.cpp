#include "Engine.RendererDX12/GDX12GrassWind.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <stdexcept>

namespace
{
    using RenderSettings::Quality;
    bool QualityValue(Quality v) { return v>=Quality::Off&&v<=Quality::High; }
    bool Scale(float v) { return std::isfinite(v)&&v>=0&&v<=4; }
    bool Lod(float v) { return std::isfinite(v)&&v>0&&v<=4; }
    bool Direction(float v) { return std::isfinite(v)&&v>=-360&&v<=360; }
    bool Grid(int v) { return v==-1||(v>=9&&v<=512); }
    bool Iterations(int v) { return v==-1||(v>=1&&v<=128); }
    constexpr auto Runtime=EConsoleVariableFlags::Default;
    AutoConsoleVariable<bool> grassEnabled(L"Render.Grass.Enabled",true,L"Enable grass rendering; does not create components");
    AutoConsoleVariable<Quality> grassQuality(L"Render.Grass.Quality",Quality::Mid,L"0 Off, 1 Low, 2 Mid, 3 High",Runtime,QualityValue);
    AutoConsoleVariable<float> density(L"Render.Grass.DensityScale",1,L"Scale component tuft density, 0..4",Runtime,Scale);
    AutoConsoleVariable<float> height(L"Render.Grass.HeightScale",1,L"Scale component blade height, 0..4",Runtime,Scale);
    AutoConsoleVariable<float> bend(L"Render.Grass.BendScale",1,L"Scale component wind response, 0..4",Runtime,Scale);
    AutoConsoleVariable<float> lod(L"Render.Grass.LodScale",1,L"Scale all component LOD distances, (0,4]",Runtime,Lod);
    AutoConsoleVariable<bool> windEnabled(L"Render.Wind.Enabled",true,L"Enable solver; Off publishes a neutral field");
    AutoConsoleVariable<Quality> windQuality(L"Render.Wind.Quality",Quality::Mid,L"0 Off, 1 Low, 2 Mid, 3 High",Runtime,QualityValue);
    AutoConsoleVariable<float> force(L"Render.Wind.ForceScale",1,L"Scale component acceleration, 0..4",Runtime,Scale);
    AutoConsoleVariable<float> direction(L"Render.Wind.DirectionOffset",0,L"Add degrees to component wind direction, -360..360",Runtime,Direction);
    AutoConsoleVariable<int> grid(L"Render.Wind.GridResolution",-1,L"-1 component/preset, otherwise 9..512 cells per axis",Runtime,Grid);
    AutoConsoleVariable<int> pressure(L"Render.Wind.PressureIterations",-1,L"-1 component/preset, otherwise 1..128 Jacobi iterations",Runtime,Iterations);
}
namespace RenderSettings
{
    Grass GetGrass()
    {
        auto q=grassQuality.GetValue(); constexpr unsigned blades[]={0,1,2,4},segments[]={0,2,4,6}; auto i=static_cast<int>(q);
        return {grassEnabled.GetValue()&&q!=Quality::Off,q,density.GetValue(),height.GetValue(),bend.GetValue(),lod.GetValue(),blades[i],segments[i]};
    }
    Wind GetWind() { auto q=windQuality.GetValue(); return {windEnabled.GetValue()&&q!=Quality::Off,q,force.GetValue(),direction.GetValue(),grid.GetValue(),pressure.GetValue()}; }
}
unsigned GrassChunkCount(const GDX12GrassSettings& s)
{
    const double x=std::ceil(2.0*s.Extents.x/s.ChunkSize),z=std::ceil(2.0*s.Extents.y/s.ChunkSize);
    if(!std::isfinite(x*z)||x*z>4096||x*z<1) throw std::invalid_argument("Grass patch needs 1..4096 bounded chunks");
    return static_cast<unsigned>(x*z);
}
void ValidateGrass(const GDX12GrassSettings& s)
{
    auto positive=[](float v){return std::isfinite(v)&&v>0&&v<=100000;};
    if(!positive(s.Extents.x)||!positive(s.Extents.y)||!positive(s.ChunkSize)||s.TuftsPerChunk>1024||!positive(s.Height)||!positive(s.Width)||!std::isfinite(s.Bend)||s.Bend<0||s.Bend>4||!positive(s.LodNear)||!positive(s.LodFar)||!positive(s.MaxDistance)||s.LodNear>=s.LodFar||s.LodFar>=s.MaxDistance)
        throw std::invalid_argument("Invalid grass patch dimensions, density, shape or ordered LOD distances");
    GrassChunkCount(s);
}
void ValidateWind(const GDX12WindSettings& s)
{
    if(!std::isfinite(s.HalfExtent)||s.HalfExtent<.001f||s.HalfExtent>1000000||!std::isfinite(s.Force)||s.Force<0||s.Force>100000||!Direction(s.DirectionDegrees)||!std::isfinite(s.Dissipation)||s.Dissipation<0||s.Dissipation>100||(s.GridResolution&&(s.GridResolution<9||s.GridResolution>512))||s.PressureIterations>128)
        throw std::invalid_argument("Invalid wind domain, force, direction, dissipation or solver dimensions");
}
