#pragma once
#include "Graph/RenderGraph.h"
#include "Graph/RenderGraphExecutor.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include <functional>
#include <optional>

namespace BistroRender
{
// Owner prepares every allocation, descriptor and constant before recording.
// Dispatch closures contain only immutable native bindings; the graph owns all
// transitions. Persistent outputs return to COMMON between frame submissions.
class SceneEnvironment
{
  public:
    SceneEnvironment(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneEnvironment();
    void Freeze(double simulationDelta, std::optional<double> routeTime, Executor&, uint64_t frame);
    void ResetClouds();
    Id BenchmarkCloudsOwner() const;
    D3D12_GPU_DESCRIPTOR_HANDLE AddCloudReads(Graph&, std::vector<ID3D12Resource*>&, std::vector<Access>&);
    void Resize()
    {
        _environment.Resize();
    }
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool AsyncComputeEnabled() const;
    std::array<Id, 2> AtmosphereDevices() const;
    std::array<Id, 2> BenchmarkAtmosphereConsumers() const;
    bool BenchmarkReady() const;
    bool Record(Id, ID3D12GraphicsCommandList*) const;
    void Submitted(uint64_t fence);
    void AddAtmosphereWaits(const Schedule&, std::vector<PacketWait>&);
    void AtmosphereSubmitted(uint64_t frame, const std::vector<CompletionToken>&);
    void SetRetirement(std::function<void()> retire) { _environment.SetResourceRetirement(std::move(retire)); }
    IRenderPassLink* Output(UINT index)
    {
        return _environment.GetOutput(index);
    }

    std::vector<Access> Reads(Graph&, std::vector<ID3D12Resource*>&) const;
    GDX12Texture* CaptureTransmittance() const
    {
        return _environment._luts[0]->IsInitialized() ? _environment._luts[0].get() : nullptr;
    }
    void AddAerialReads(Graph&, std::vector<ID3D12Resource*>&, std::vector<Access>&);
    GDX12Texture* VisibleSkyView() const
    {
        auto* texture = _environment._luts[2].get();
        return texture->IsInitialized() ? texture : nullptr;
    }
    struct SkyViewConstants { XMFLOAT4 ObserverRadius; UINT CloudsEnabled; };
    GDX12UploadBuffer<SkyViewConstants>& SkyViewSampling() const
    {
        return *_skyViewSampling.at(_resources.CurrFrameConstantsIndex);
    }

  private:
    enum class WorkDomain { Cached, Camera };
    struct Work
    {
        std::string Name;
        std::vector<std::pair<GDX12Resource*, Access>> Accesses;
        std::function<void(ID3D12GraphicsCommandList*)> Record;
        bool Compute = false;
        WorkDomain Domain = WorkDomain::Cached;
    };
    struct RemoteAtmosphere;
    struct VolumetricClouds;
    void Build(Executor&, uint64_t frame, double cloudTime);
    bool PrepareAtmosphere(Executor&, uint64_t frame, bool atmosphere, uint64_t skyRevision, bool& localChanged,
                           const GDX12CameraConstants* camera = nullptr, bool invalidated = false);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12EnvironmentIBLPass _environment;
    std::unique_ptr<GDX12Texture> _captureSkyView;
    std::unique_ptr<VolumetricClouds> _cloud;
    std::vector<Work> _work;
    std::vector<Id> _operations;
    std::vector<std::pair<uint64_t, std::vector<Work>>> _retained;
    GDX12SkyConstants _mediumConstants{}, _multipleConstants{}, _captureConstants{}, _viewConstants{}, _aerialConstants{};
    Matrix _aerialViewProjection = Matrix::Identity;
    bool _atmosphereReady = false;
    std::unique_ptr<RemoteAtmosphere> _remoteSkyView, _remoteAerial;
    std::vector<std::unique_ptr<GDX12UploadBuffer<SkyViewConstants>>> _skyViewSampling;
    int _failedSkyViewAdapter = -1, _failedAerialAdapter = -1;
};
} // namespace BistroRender
