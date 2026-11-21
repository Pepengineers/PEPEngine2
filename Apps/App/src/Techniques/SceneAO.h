#pragma once
#include "NativeWork.h"
#include "../Graph/RenderGraphExecutor.h"
#include <optional>
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
namespace BistroRender
{
class SceneAO
{
  public:
    SceneAO(GDX12DeviceResources&, RenderPipelineCommonData&, GDX12Texture* depth, GDX12Texture* normals);
    ~SceneAO();
    void Freeze(GDX12Texture* depth, GDX12Texture* normals, UINT& temporalIndex,
                const std::shared_ptr<QueueSubmission>& remote = {});
    void AppendRemoteDepth(Graph&, std::vector<ID3D12Resource*>&);
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    std::optional<Executor::PacketExchange> Exchange(const Schedule&);
    void Presented(uint64_t frame, const std::vector<CompletionToken>&);
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const
    {
        return _work.Record(id, cmd);
    }
    GDX12Texture* Output()
    {
        return &_pass._output;
    }

  private:
    struct Plan;
    struct Remote;
    void RetireRemote();
    struct PreparedInputs
    {
        GDX12Texture* Depth;
        GDX12Texture* Normals;
    };
    PreparedInputs AppendPreparation(Plan&, GDX12Texture* depth, GDX12Texture* normals);
    void AppendRawVisibility(Plan&, const PreparedInputs&, GDX12Texture&);
    GDX12Texture* AppendVisibility(Plan&, const PreparedInputs&);

    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12AmbientOcclusionPass _pass;
    GDX12AOSettings _settings{};
    Matrix _projection;
    NativeWork _work;
    std::unique_ptr<Remote> _remote;
    uint64_t _attemptedDomain = 0;
    UINT _attemptedWidth = 0, _attemptedHeight = 0;
    Id _depthExport = Invalid, _visibilityImport = Invalid, _importPacket = Invalid;
    bool _offload = false;
    UINT _width = 0, _height = 0;
    bool _half = false;
};
} // namespace BistroRender
