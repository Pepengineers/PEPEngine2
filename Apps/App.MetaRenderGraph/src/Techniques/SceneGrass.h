#pragma once
#include "GeometryTarget.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include <functional>

namespace Meta {
class SceneGrass {
public:
    explicit SceneGrass(RenderPipelineContext&);
    void Prepare(RenderPipelineContext&,float dt);
    void Select(bool serial) { _mode=serial?0:1; }
    std::vector<ID3D12Resource*> Append(Graph&,UINT slot,const std::vector<ID3D12Resource*>& existing,GDX12Descriptor& materials,
        const std::vector<GeometryTarget>& targets,bool viewsOnly=false,
        const std::function<void(const GeometryTarget&,Graph&,std::vector<ID3D12Resource*>&)>& beforeView={});
    const std::vector<ID3D12Resource*>& Resources(UINT slot) const;
    bool Record(UINT slot,Id,ID3D12GraphicsCommandList*) const;
    std::vector<uint8_t> Readback(UINT slot,const std::string& stem) const;
private:
    struct RecordedFrame {
        struct Probe {
            GDX12ResourceHandle Depth;
            GDX12Descriptor Dsv, ReadDepth;
            std::array<std::unique_ptr<GDX12Texture>,5> Color;
        };
        std::vector<ID3D12Resource*> Bindings;
        std::vector<std::pair<Id,std::function<void(ID3D12GraphicsCommandList*)>>> Commands;
        Id First=Invalid;
        GDX12ResourceHandle Readback;
        UINT64 Bytes=0;
        uint64_t Version=0;
        UINT Patches=0;
        std::array<Probe,6> Probes;
        std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> ProbeCameras;
        GDX12ResourceHandle ProbeReadback;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> ProbeFootprints;
        UINT64 ProbeBytes=0;
    };
    GDX12DeviceResources& _resources;
    BistroGrassGeometry _scene;
    std::array<std::unique_ptr<GDX12GrassRenderer>,2> _gpu;
    std::array<std::vector<RecordedFrame>,2> _frames;
    UINT _mode=1;
};
}
