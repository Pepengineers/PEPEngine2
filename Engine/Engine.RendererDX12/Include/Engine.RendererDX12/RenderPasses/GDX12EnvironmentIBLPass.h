#pragma once
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include <array>
#include <filesystem>
#include <climits>
namespace Meta { class SceneEnvironment; }

// Outputs: diffuse irradiance, GGX radiance (8 mips when enabled), DFG, source cube.
class GDX12EnvironmentIBLPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Environment");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        for (UINT i = 0; i < _outputs.size(); ++i)
            graph.Write(_outputs[i], "Environment " + std::to_string(i), Lifetime::Dynamic);
        graph.Write(SharedResource::Environment, "Atmosphere cache");
        graph.HasSideEffects();
    }

    GDX12EnvironmentIBLPass();
    void Initialize() override;
    void Resize() override;
    void Execute(GDX12CommandList* commands) override;
    void Invalidate();
    void ClearDenendencies() override;
    void SetCloudSource(GDX12CloudComponent* source);
private:
    friend class Meta::SceneEnvironment;
    void InitializeKernels();
    void LoadEnvironment(const std::filesystem::path& asset);
    void CreateTexture(GDX12Texture& texture, UINT width, UINT height, UINT depth, UINT mips, bool cube);
    void EnsureOutputs(bool enabled, UINT cubeSize);
    void AllocateOutputs(bool enabled, UINT cubeSize);
    void EnsureAtmosphere(const SkyAtmosphereSettings& settings);
    void PublishAtmosphereViews(bool enabled);
    void BindSky(GDX12CommandList* commands, UINT constantIndex);
    void BindSkyConstants(GDX12CommandList* commands, D3D12_GPU_VIRTUAL_ADDRESS address);
    void DispatchLUT(GDX12CommandList* commands, UINT pipeline, UINT index);
    void RecordSourceCube(GDX12CommandList* commands);
    void RecordDisabledOutputs(GDX12CommandList* commands);
    void Filter(GDX12CommandList* commands, UINT samples, float intensity);
    void EnsureBRDF(GDX12CommandList* commands);
    void ResetEnvironmentHistory();
    void ReleaseAtmosphere();
    std::array<std::unique_ptr<GDX12Texture>, 4> _textures;
    std::array<std::unique_ptr<GDX12Texture>, 5> _luts;
    std::unique_ptr<GDX12Texture> _boxTexture;
    std::unique_ptr<GDX12UploadBuffer<std::array<uint16_t, 4>>> _disabledPixel;
    std::array<std::unique_ptr<GDX12Descriptor>, 8> _prefilterUAVs;
    std::array<ComPtr<ID3D12PipelineState>, 3> _filters;
    std::array<ComPtr<ID3D12PipelineState>, 5> _skyPipelines;
    std::unique_ptr<GDX12RootSignature> _filterRoot, _skyRoot;
    std::filesystem::path _boxAsset;
    GDX12Descriptor _nullCube, _null2D;
    std::vector<std::unique_ptr<GDX12Descriptor>> _sourceUAVs;
    UINT _prefilterMips = 0;
    uint64_t _revision = UINT64_MAX;
    UINT _quality = UINT_MAX;
    SkySource _source = SkySource::Off;
    bool _invalidated = true;
    bool _brdfReady = false;
    GDX12CloudComponent* _cloudSource = nullptr;
    uint64_t _cloudSourceRevision = 0, _cloudLightingRevision = 0;
    bool _cloudActive = false;
    SkyIBLSettings _resolvedIBL;
};
