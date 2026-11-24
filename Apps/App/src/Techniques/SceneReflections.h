#pragma once
#include "NativeWork.h"
#include "SceneEnvironment.h"
#include "SceneGeometry.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include <chrono>
#include <map>
namespace BistroRender
{
struct PublishedShadows;
class SceneReflections
{
  public:
    SceneReflections(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneReflections();
    void Freeze(const std::shared_ptr<QueueSubmission>& submissions)
    {
        _submissions = submissions;
        _settings = GetReflectionSettings();
        _historyBase = _common.HistoryRevision;
    }
    void Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&, UINT slot,
                const PublishedShadows& staticShadows, const PublishedShadows& finalShadows);
    bool Record(Id, ID3D12GraphicsCommandList*) const;
    void Submitted(uint64_t fence);
    void AddExchanges(const Schedule&, std::vector<PacketWait>&, std::vector<Executor::PacketExchange>&);
    void RemoteSubmitted(const std::vector<CompletionToken>&);
    void Reset();
    void RetireLocalShadows();
    const auto& Owners() const { return _owners; }
    void AppendPublishedOwners(std::vector<std::pair<std::string, Id>>& owners) const;
    std::pair<size_t, size_t> Readiness() const;
    GDX12Texture* Published()
    {
        return &_passes->_published;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Radiance() const
    {
        return _radiance.GPUHandle;
    }
    uint64_t History() const
    {
        return _history;
    }

  private:
    struct CaptureInputs;
    struct Remote;
    void AppendCapture(Graph&, std::vector<ID3D12Resource*>&, NativeWork&, GDX12ReflectionProbePass&,
                       GDX12DeviceResources&, const CaptureInputs&, std::vector<Id>&, std::vector<UINT>&);
    void AppendRemote(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&, UINT,
                      std::vector<Id>&, bool arrayReplaced, const PublishedShadows&, const PublishedShadows&);
    struct Publication
    {
        uint64_t Fence = 0;
        bool Ready = false;
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::unique_ptr<GDX12ReflectionProbePass> _passes;
    NativeWork _work;
    std::vector<UINT> _publishing;
    std::vector<Publication> _publications;
    GDX12Resource _black;
    GDX12Descriptor _radiance;
    uint64_t _history = 0;
    GDX12ReflectionSettings _settings;
    uint64_t _historyBase = 0;
    std::shared_ptr<QueueSubmission> _submissions;
    std::map<Id, std::unique_ptr<Remote>> _remotes;
    std::vector<Id> _owners;
    struct ShadowFallback
    {
        bool Static, Final;
        std::chrono::steady_clock::time_point NextCheck;
    };
    std::map<Id, ShadowFallback> _shadowFallbackOwners;
    UINT _transferProbe = 0;
};
} // namespace BistroRender
