#pragma once
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include <array>
#include <filesystem>
#include <climits>
#include <functional>
namespace BistroRender { class SceneEnvironment; }

// Outputs: diffuse irradiance, GGX radiance (8 mips when enabled), DFG, source cube.
class GDX12EnvironmentIBLPass final : public GDX12RenderPass
{
public:

    GDX12EnvironmentIBLPass();
    void Initialize() override;
    void Resize() override;
    void Invalidate();
    void ClearDenendencies() override;
    // Drain graph queues before replacing resources or descriptors.
    void SetResourceRetirement(std::function<void()> retire) { _retire = std::move(retire); }
private:
    friend class BistroRender::SceneEnvironment;
    void InitializeKernels();
    void CreateTexture(GDX12Texture& texture, UINT width, UINT height, UINT depth, UINT mips, bool cube);
    void EnsureOutputs(bool enabled, UINT cubeSize);
    void AllocateOutputs(bool enabled, UINT cubeSize);
    void EnsureAtmosphere(const SkyAtmosphereSettings& settings);
    void PublishAtmosphereViews(bool enabled);
    void ResetEnvironmentHistory();
    void ReleaseAtmosphere();
    void RetireResources();
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
    SkyIBLSettings _resolvedIBL;
    std::function<void()> _retire;
};
