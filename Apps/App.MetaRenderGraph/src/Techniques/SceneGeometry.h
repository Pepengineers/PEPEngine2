#pragma once
#include "Graph/MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include "SceneShadows.h"
#include "SceneGrass.h"
#include <array>

namespace Meta {
class SceneLighting;
class SceneReflections;
// Owner-only Freeze/Resize. Record uses immutable slot data and native commands;
// no shared resource tracker, heap allocator, ECS or queue access on workers.
class SceneGeometry {
public:
    explicit SceneGeometry(RenderPipelineContext&);
    void PrepareGrass(RenderPipelineContext& c,float dt) { _grass->Prepare(c,dt); }
    GDX12ParticleInteractionField InteractionField() const { return _grass->InteractionField(); }
    void Select(bool serial) { _grass->Select(serial); }
    UINT SlotCount() const { return UINT(_slots.size()); }
    void WriteOperations(UINT slot,std::ostream& out) const { const auto& names=_slots.at(slot).Names;for(UINT i=0;i<names.size();++i)out<<i<<' '<<names[i]<<'\n'; }
    void Resize(UINT width, UINT height); // caller has drained all slots
    void Freeze(UINT slot, UINT camera, std::vector<GDX12SkinningWork> skin);
    using AppendTechnique = std::function<void(Graph&,std::vector<ID3D12Resource*>&)>;
    Schedule Plan(UINT slot,ID3D12Resource* target,const AppendTechnique& append = {},const AppendTechnique& earlyAO = {},const AppendTechnique& prelude = {});
    const std::vector<ID3D12Resource*>& PlannedResources(UINT slot) const { return _slots.at(slot).PlannedBindings; }
    // Frame-slot inputs for future graph consumers; access declarations and the
    // frame completion fence still govern their lifetime and readiness.
    GDX12Texture* Depth(UINT slot) const { return _slots.at(slot).DepthView.get(); }
    GDX12Texture* GBuffer(UINT slot,UINT attachment) const { return _slots.at(slot).Color.at(attachment).get(); }
    std::vector<ID3D12Resource*> Resources(UINT slot, ID3D12Resource* target,bool grass=true) const;
    void Record(UINT slot, Id operation, UINT bucket, ID3D12GraphicsCommandList*, ID3D12Resource* target) const;
    std::vector<uint8_t> Readback(UINT slot, const std::string& stem) const; // after GPU completion
private:
    friend class SceneWaterParticles;
    friend class SceneLighting;
    friend class SceneReflections;
    struct Buffer {
        GDX12ResourceHandle Source, Data;
        UINT64 Bytes=0;
        GDX12Descriptor Srv, Uav;
    };
    struct Slot {
        std::vector<ID3D12Resource*> PlannedBindings;
        std::array<Buffer,8> Buffers; // instances/templates/materials/transforms/opaque/count/transparent/count
        std::array<Buffer,2> FrameBuffers; // immutable main/camera constants
        std::array<Id,4> FrameIds{};
        std::array<UINT,2> FrameStrides{};
        UINT64 FrameOffset=0;
        std::array<std::unique_ptr<GDX12Texture>,5> Color;
        std::unique_ptr<GDX12Texture> DepthView,PrepassNormals;
        Id PrepassId=Invalid;
        bool EarlyAO=false;
        GDX12ResourceHandle Depth;
        GDX12Descriptor Dsv, ReadDepth;
        GDX12GeometryBuffer::RecordingBinding Geometry{};
        D3D12_GPU_VIRTUAL_ADDRESS Camera=0, Main=0;
        UINT Draws=0;
        GDX12ResourceHandle ImageReadback, SceneReadback;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,6> Footprints{};
        UINT64 ImageBytes=0;
        std::array<UINT64,8> Offsets{};
        UINT64 SceneBytes=0;
        UINT64 SkinOffset=0;
        std::vector<GDX12SkinningWork> Skin;
        std::vector<ID3D12Resource*> SkinBindings;
        std::vector<Resource> SkinDeclarations;
        std::vector<std::array<Id,5>> SkinIds;
        Id FinalCopy=Invalid,FinalReadback=Invalid;
        std::unique_ptr<GDX12InstanceBatcher> Batcher;
        GDX12ResourceHandle BatchCounter,BatchReadback;
        UINT Batches=0,InstanceCount=0;
        std::array<Id,6> BatchIds{};
        Id BatchReadOperation=Invalid;
        std::vector<GDX12InstanceData> InstanceSnapshot;
        std::vector<std::string> Names;
    };
    void BufferFor(Buffer&, UINT64 bytes, UINT stride, bool upload=false);
    GDX12DeviceResources& _resources;
    std::vector<Slot> _slots;
    std::unique_ptr<GDX12RootSignature> _cullRoot, _clearRoot, _drawRoot;
    std::unique_ptr<SceneShadows> _shadows;
    std::unique_ptr<SceneGrass> _grass;
    RenderPipelineCommonData& _common;
    GDX12ResourceHandle _emptyVertices,_emptyIndices;
    ComPtr<ID3D12PipelineState> _cull, _clear, _depth, _depthNormals, _gbuffer;
    ComPtr<ID3D12CommandSignature> _signature;
    UINT _width=0,_height=0;
};
}
