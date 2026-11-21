#pragma once
#include "NativeWork.h"
#include "../Graph/RenderGraphExecutor.h"
#include <optional>
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
namespace BistroRender
{
class SceneAO
{
    struct Remote;

  public:
    struct History
    {
        std::shared_ptr<Remote> Last;
        uint64_t CameraRevision = 0;
    };
    SceneAO(GDX12DeviceResources&, RenderPipelineCommonData&, GDX12Texture* depth, GDX12Texture* normals);
    ~SceneAO();
    static bool SupportsRemote(const GDX12AOSettings&, bool depthNormals);
    void Freeze(GDX12Texture* depth, GDX12Texture* normals, GDX12Texture* motion, GDX12Texture* earlyNormals, UINT& temporalIndex,
                const std::shared_ptr<QueueSubmission>& remote, History&);
    void AppendXeInputs(Graph&, std::vector<ID3D12Resource*>&, Id ready);
    void AppendRemoteDepth(Graph&, std::vector<ID3D12Resource*>&);
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    std::optional<Executor::PacketExchange> Exchange(const Schedule&, History&);
    void Submitted(const std::vector<CompletionToken>&);
    void Presented(uint64_t frame, const std::vector<CompletionToken>&);
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const
    {
        return _work.Record(id, cmd);
    }
    bool UsesHistory() const { return _offload && _import != _remote; }
    GDX12Texture* Output()
    {
        return &_pass._output;
    }

  private:
    struct Plan;
    struct PreparedInputs
    {
        GDX12Texture* Depth;
        GDX12Texture* Normals;
    };
    PreparedInputs AppendPreparation(Plan&, GDX12Texture* depth, GDX12Texture* normals);
    void AppendXeImport(Plan&, GDX12Texture& terms, GDX12Texture& edges);
    void AppendRawVisibility(Plan&, const PreparedInputs&, GDX12Texture&);
    GDX12Texture* AppendVisibility(Plan&, const PreparedInputs&);

    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12AmbientOcclusionPass _pass;
    GDX12AOSettings _settings{};
    Matrix _projection;
    NativeWork _work;
    std::shared_ptr<Remote> _remote, _spare, _import;
    GDX12Texture* _motion = nullptr;
    GDX12Texture* _earlyNormals = nullptr;
    uint64_t _version = 0, _attemptedDomain = 0;
    bool _attemptedXe = false, _attemptedDelayed = false, _attemptedSecondary = false;
    int _attemptedBlur = 0;
    UINT _attemptedWidth = 0, _attemptedHeight = 0;
    Id _depthExport = Invalid, _visibilityImport = Invalid, _reproject = Invalid;
    Id _importPacket = Invalid, _reprojectPacket = Invalid;
    bool _offload = false;
    UINT _width = 0, _height = 0;
    bool _half = false;
};
} // namespace BistroRender
