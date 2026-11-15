#pragma once
#include "NativeWork.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"

namespace Meta
{
class SceneParticles
{
public:
    SceneParticles(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneParticles();
    void Prepare(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* depth,
        GDX12Texture* accum, GDX12Texture* reveal, GDX12ParticleInteractionField);
    void AppendWeighted(Graph&, std::vector<ID3D12Resource*>&);
    void AppendComposite(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr);
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd); }
private:
    void Packet(Graph&, const std::vector<ID3D12Resource*>&, const char*, std::vector<Access>,
        std::function<void(GDX12CommandList*)>);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12ParticlesPass _pass;
    NativeWork _work;
    std::vector<Access> _drawReads;
    GDX12Texture *_accum = nullptr, *_reveal = nullptr;
    UINT _width = 0, _height = 0;
};
}
