#pragma once
#include "NativeWork.h"
#include "../Graph/RenderGraphExecutor.h"
#include "Engine.RendererDX12/GDX12Particles.h"
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
    static int SecondaryAdapter();
    void BeginSimulation(GDX12ParticleInteractionField, GDX12Texture* depth, Executor&, Id device, float worldRate, bool ownerDraw);
    Id ExecutionDevice() const;
    void PauseIndependentClock();
    void AddExchanges(const Schedule&, std::vector<PacketWait>&, std::vector<Executor::PacketExchange>&);
    void Submitted(const std::vector<CompletionToken>&);
    void Presented(uint64_t frame);
    void AppendSimulation(Graph&, std::vector<ID3D12Resource*>&, Inputs, GDX12Texture* depth);
    void PrepareOpaque(const SceneGeometry&, UINT slot);
    void AppendEarlyOpaquePreparation(Graph&, std::vector<ID3D12Resource*>&);
    void AppendOpaquePrepass(Graph&, std::vector<ID3D12Resource*>&, const SceneGeometry&, UINT slot);
    void AppendOpaque(Graph&, std::vector<ID3D12Resource*>&, const SceneGeometry&, UINT slot);
    void Prepare(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* rasterDepth, GDX12Texture* accum,
        GDX12Texture* reveal);
    void AppendWeighted(Graph&, std::vector<ID3D12Resource*>&);
    struct RemoteRaster
    {
        RenderPipelineCommonData Common;
        GDX12ParticlesPass Pass;
        GDX12Descriptor SimulationDepth;
        std::vector<std::unique_ptr<std::array<GDX12Descriptor, 3>>> SourceViews;
        GDX12ParticleRenderFrame Frame;
        std::vector<Access> Reads;
    };
    void AppendRemoteWeighted(Graph&, std::vector<ID3D12Resource*>&, NativeWork&, GDX12DeviceResources& owner,
        GDX12DeviceResources& simulationResources, GDX12ParticleSimulation&, GDX12Texture* accum, GDX12Texture* reveal,
        GDX12Texture* depth, Id cameraId, D3D12_GPU_VIRTUAL_ADDRESS camera, UINT slot, RemoteRaster&,
        const std::function<ID3D12Resource*(GDX12Texture&)>& asset);
    void AppendRemoteComposite(Graph&, std::vector<ID3D12Resource*>&, NativeWork&, GDX12Texture* hdr, RemoteRaster&);
    void AppendComposite(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr);
    GDX12Texture* MotionOutput() const { return _pass.MotionOutput(); }
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd); }
private:
    struct EmitterLayout
    {
        uint64_t Id;
        UINT Capacity;
        bool operator==(const EmitterLayout& rhs) const { return Id == rhs.Id && Capacity == rhs.Capacity; }
    };
    struct Remote;
    struct Lane
    {
        std::unique_ptr<Remote> Solver;
        std::vector<EmitterLayout> Layout;
        Id Device = 0;
        uint64_t Generation = 0;
        Id Export = Invalid, Import = Invalid, ImportPacket = Invalid;
        bool Attempted = false;
    };
    bool HasDrawSources(bool opaque) const;
    std::vector<Access> DrawReads(Graph&, std::vector<ID3D12Resource*>&, bool opaque);
    void AppendOpaqueDraw(Graph&, std::vector<ID3D12Resource*>&, const SceneGeometry&, UINT,
        GDX12ParticlesPass::OpaqueStage);
    void AppendPreparation(Graph&, std::vector<ID3D12Resource*>&, bool opaque);
    void AppendSimulationImports(Graph&, std::vector<ID3D12Resource*>&, bool opaque);
    GDX12Resource* VisibilityReadback(UINT slot, bool opaque, GDX12Device&, Id owner);
    void Packet(Graph&, const std::vector<ID3D12Resource*>&, const char*, std::vector<Access>,
        std::function<void(GDX12CommandList*)>, Queue target = Queue::Direct, NativeWork* work = nullptr);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12ParticlesPass _pass;
    NativeWork _work;
    GDX12ParticleInteractionField _interaction{};
    std::array<std::vector<uint64_t>, 3> _groups;
    GDX12ParticleRenderFrame _renderFrame;
    std::shared_ptr<GDX12DescriptorHeap> _secondaryHeap;
    Id _heapDevice = 0;
    std::array<Lane, 3> _remote;
    struct VisibilitySample
    {
        GDX12Resource Buffer;
        Id Owner = 0;
        uint64_t Frame = 0;
        std::vector<std::pair<uint64_t, uint64_t>> Sources;
        bool Written = false;
    };
    std::vector<std::array<VisibilitySample, 2>> _visibilitySamples;
    std::vector<uint64_t> _remoteVersions;
    UINT _firstRemote = 0;
    std::vector<uint64_t> _visibleSources;
    std::vector<Access> _drawReads;
    GDX12Texture *_accum = nullptr, *_reveal = nullptr;
    UINT _width = 0, _height = 0;
    bool _opaqueReady = false, _splitOpaque = false, _opaquePrepared = false;
};
}
