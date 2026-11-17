#pragma once
#include "NativeWork.h"
#include "SceneGeometry.h"
#include "SceneReflections.h"
#include <unordered_set>

namespace Meta
{
class SceneProbePreview
{
public:
    SceneProbePreview(GDX12DeviceResources&, RenderPipelineCommonData&);
    void Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneReflections&,
        UINT slot, GDX12Texture* hdr, GDX12Texture* inputDepth, GDX12Texture* inputMotion, SceneEnvironment&);
    bool Record(Id id, ID3D12GraphicsCommandList* commands) const { return _work.Record(id, commands); }
    D3D12_CPU_DESCRIPTOR_HANDLE ReadDepth() const { return _readDepth; }
    GDX12Texture* HDR() const { return _hdr; }
    GDX12Texture* Depth() const { return _depth; }
    GDX12Texture* Motion() const { return _motion; }
private:
    struct Frame
    {
        GDX12ResourceHandle DepthResource;
        std::unique_ptr<GDX12Texture> HDR, Depth, Motion;
        GDX12Descriptor Dsv, ReadDsv;
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::vector<Frame> _frames;
    std::unique_ptr<GDX12RootSignature> _root, _backgroundRoot;
    ComPtr<ID3D12PipelineState> _pipeline, _backgroundPipeline;
    NativeWork _work;
    std::unordered_set<UINT64> _previous;
    std::vector<GDX12IndirectDrawArgs> _previewCommands;
    GDX12ResourceHandle _commandSource;
    uint64_t _commandRevision = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE _readDepth{};
    GDX12Texture* _hdr = nullptr;
    GDX12Texture* _depth = nullptr;
    GDX12Texture* _motion = nullptr;
};
}
