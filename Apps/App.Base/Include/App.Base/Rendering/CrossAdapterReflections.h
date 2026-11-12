#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "App.Base/Rendering/CrossAdapterWork.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include <map>

class CrossAdapterReflections
{
public:
    using MaterialTextures = std::function<std::vector<GDX12Texture*>()>;
    CrossAdapterReflections(GDX12DeviceResources& primary, GDX12Device& secondary, RenderMGPURuntime& runtime,
        MaterialTextures materials, RenderPipelineCommonData& common, GDX12ReflectionProbePass& pass, GDX12EnvironmentIBLPass& environment);
    ~CrossAdapterReflections();
    void Configure(GDX12RenderGraph& graph, GDX12RenderPass& node, RenderPipelineBindings& bindings,
        std::vector<IRenderPassLink*> materialInputs = {});
    void ResetWorld();
    bool HasSecondaryWork() const;
    using PrepareApplication = std::function<std::vector<CrossAdapterWork::Use>(GDX12DeviceResources&, GDX12ReflectionProbePass&, CrossAdapterWork&, UINT64&)>;
    using DrawApplication = std::function<void(GDX12CommandList&, GDX12ReflectionProbePass&, UINT)>;
    using ApplicationInputs = std::function<std::vector<std::shared_ptr<GDX12Resource>>() >;
    void SetApplication(ApplicationInputs inputs, PrepareApplication prepare, DrawApplication draw)
    { _applicationInputs = std::move(inputs); _prepareApplication = std::move(prepare); _drawApplication = std::move(draw); }
    struct Diagnostics { UINT64 SceneBytes = 0, ResultBytes = 0; double WaitMs = 0; };
    Diagnostics GetDiagnostics() const { return {_sceneBytes, _resultBytes, _work.WaitMilliseconds}; }
private:
    using View = CrossAdapterWork::View;
    struct TextureMirror
    {
        std::shared_ptr<GDX12TextureResource> Source;
        std::unique_ptr<GDX12Texture> Texture;
    };
    GDX12Texture* MirrorTexture(GDX12Texture* source, bool material, bool refresh);
    void PrepareScene();
    std::vector<CrossAdapterWork::Use> PrimaryUses();
    std::optional<std::vector<GDX12RenderGraph::LegacyExternalView>> Boundary();
    GDX12Submission::CompletionPoint Submit(GDX12RenderPass& node, GDX12Submission::CompletionPoint prefix);
    GDX12DeviceResources& _primary;
    GDX12Device& _secondaryDevice;
    RenderMGPURuntime& _runtime;
    MaterialTextures _materials;
    RenderPipelineCommonData& _common;
    GDX12ReflectionProbePass& _pass;
    GDX12EnvironmentIBLPass& _environment;
    std::unique_ptr<GDX12DeviceResources> _secondary;
    std::unique_ptr<GDX12ReflectionProbePass> _capture;
    CrossAdapterWork _work;
    CrossAdapterWork::Channel _sceneChannel, _resultChannel;
    std::map<GDX12Texture*, TextureMirror> _textures;
    View _vertices, _indices;
    UINT64 _sceneBytes = 0, _resultBytes = 0;
    PrepareApplication _prepareApplication;
    DrawApplication _drawApplication;
    ApplicationInputs _applicationInputs;
    std::vector<IRenderPassLink*> _materialInputs;
    std::vector<CrossAdapterWork::Use> _applicationReads;
};
