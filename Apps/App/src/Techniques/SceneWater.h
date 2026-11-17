#pragma once
#include "Rendering/Water/GDX12FluidPass.h"
#include "NativeWork.h"
#include "Rendering/Water/BistroWaterRendering.h"

namespace Meta
{
class SceneWater
{
public:
    SceneWater(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneWater();
    void SetRetirement(std::function<void()> retire);
    void Prepare(float delta);
    void AppendSurface(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* opaqueDepth, GDX12Texture* opaqueMotion);
    void AppendDraw(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr, D3D12_CPU_DESCRIPTOR_HANDLE depth);
    GDX12Texture* Depth() const { return _depth; }
    GDX12Texture* Motion() const { return _motion; }
    bool Record(Id id, ID3D12GraphicsCommandList* commands) const { return _work.Record(id, commands); }
private:
    void Packet(Graph&, std::vector<ID3D12Resource*>&, const char*,
        const std::vector<GDX12ResourceAccess>&, std::function<void(GDX12CommandList*)>);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::unique_ptr<BistroWaterRendering> _collector;
    GDX12FluidSurfacePass _surface;
    NativeWork _work;
    std::function<void()> _retire;
    UINT _width = 0, _height = 0;
    GDX12Texture *_depth = nullptr, *_motion = nullptr;
};
}
