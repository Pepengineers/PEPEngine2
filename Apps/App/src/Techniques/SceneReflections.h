#pragma once
#include "NativeWork.h"
#include "ReflectionFrame.h"
#include "SceneEnvironment.h"
#include "SceneGeometry.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include <chrono>
#include <map>
namespace BistroRender
{
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
    void Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&, UINT slot);
    bool Record(Id, ID3D12GraphicsCommandList*) const;
    void Submitted(uint64_t fence);
    void AddExchanges(const Schedule&, std::vector<PacketWait>&, std::vector<Executor::PacketExchange>&);
    void RemoteSubmitted(const std::vector<CompletionToken>&);
    void Reset();
    const auto& Owners() const { return _owners; }
    void AppendPublishedOwners(std::vector<std::pair<std::string, Id>>& owners) const;
    std::pair<size_t, size_t> Readiness() const;
    const ReflectionFrame& Frame() const { return _frame; }
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
    struct FilteredFaces { UINT Probe, Mask; };
    void AppendCapture(Graph&, std::vector<ID3D12Resource*>&, NativeWork&, GDX12ReflectionProbePass&,
                       GDX12DeviceResources&, const CaptureInputs&, std::vector<Id>&, std::vector<FilteredFaces>&);
    void AppendRemote(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&, UINT,
                      std::vector<Id>&, bool arrayReplaced);
    void PrepareCaptureBudget();
    bool CanCapture(size_t probe) const;
    void StampCapturedFaces(UINT probe, UINT mask, std::array<ReflectionStamp, 6>&);
    struct Publication
    {
        uint64_t Fence = 0;
        bool Ready = false;
        std::array<ReflectionStamp, 6> Faces{};
    };
    struct MetadataSlot
    {
        ID3D12Resource* Target = nullptr;
        UINT Count = 0;
        std::array<bool, MaxReflectionProbes> Initialized{};
        std::array<GDX12ReflectionProbeData, MaxReflectionProbes> Data{};
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::unique_ptr<GDX12ReflectionProbePass> _passes;
    NativeWork _work;
    std::vector<FilteredFaces> _publishing;
    std::vector<Publication> _publications;
    GDX12ReflectionSchedule::SceneBudget _sceneBudget;
    ReflectionFrame _frame;
    ReflectionStamp _stamp;
    GDX12ReflectionSchedule::Frequency _budgetFrequency = GDX12ReflectionSchedule::Frequency::Off;
    std::vector<std::array<uint64_t, 6>> _due;
    std::vector<UINT> _captureMasks;
    bool _bootstrapping = false;
    bool _useSceneBudget = false;
    std::vector<MetadataSlot> _metadata;
    UINT _metadataSlot = UINT_MAX;
    std::vector<std::pair<UINT, UINT>> _metadataChanges;
    GDX12Resource _black;
    GDX12Descriptor _radiance;
    uint64_t _history = 0;
    GDX12ReflectionSettings _settings;
    uint64_t _historyBase = 0;
    std::shared_ptr<QueueSubmission> _submissions;
    std::map<Id, std::unique_ptr<Remote>> _remotes;
    std::vector<Id> _owners;
    UINT _transferProbe = 0;
};
} // namespace BistroRender
