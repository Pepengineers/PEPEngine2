#pragma once
#include "NativeWork.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include <array>

namespace BistroRender
{
class SceneGeometry;
class SceneParticles
{
public:
    SceneParticles(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneParticles();
    enum class Inputs { Frame, Interaction, Depth };
    void BeginSimulation(GDX12ParticleInteractionField);
    void AppendSimulation(Graph&, std::vector<ID3D12Resource*>&, Inputs, GDX12Texture* depth);
    void AppendOpaque(Graph&, std::vector<ID3D12Resource*>&, const SceneGeometry&, UINT slot);
    void Prepare(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* rasterDepth, GDX12Texture* accum,
        GDX12Texture* reveal);
    void AppendWeighted(Graph&, std::vector<ID3D12Resource*>&);
    void AppendComposite(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr);
    GDX12Texture* MotionOutput() const { return _pass.MotionOutput(); }
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd); }
private:
    std::vector<Access> DrawReads(Graph&, std::vector<ID3D12Resource*>&, bool opaque);
    void AppendPreparation(Graph&, std::vector<ID3D12Resource*>&, const std::vector<uint64_t>&, bool opaque);
    void Packet(Graph&, const std::vector<ID3D12Resource*>&, const char*, std::vector<Access>,
        std::function<void(GDX12CommandList*)>, Queue target = Queue::Direct);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12ParticlesPass _pass;
    NativeWork _work;
    GDX12ParticleInteractionField _interaction{};
    std::array<std::vector<uint64_t>, 3> _groups;
    std::vector<uint64_t> _opaque, _transparent;
    std::vector<Access> _drawReads;
    GDX12Texture *_accum = nullptr, *_reveal = nullptr;
    UINT _width = 0, _height = 0;
};
}
