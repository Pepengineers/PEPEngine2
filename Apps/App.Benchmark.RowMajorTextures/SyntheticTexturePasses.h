#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

#include <memory>
#include <vector>

inline constexpr DXGI_FORMAT SyntheticTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

class SyntheticTextureWritePass final : public GDX12RenderPass
{
public:
    explicit SyntheticTextureWritePass(UINT extent);

    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void OnResourcesRecreated(PassInitContext&) override { _initializedSlots.clear(); }
    void Execute(PassExecutionContext& context) override;

private:
    const UINT _extent;
    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipeline;
    std::vector<bool> _initializedSlots;
};

class SyntheticTextureReadPass final : public GDX12RenderPass
{
public:
    SyntheticTextureReadPass(UINT extent, bool* verified = nullptr);

    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    void ValidateChecksum(PassExecutionContext& context, GDX12CommandList& commands, GDX12Resource& checksum);
    UINT ExpectedChecksum(UINT group) const;

    const UINT _extent;
    const UINT _groupsPerRow;
    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipeline;
    bool* _verified;
    bool _validationRecorded = false;
    UINT _validationSeed = 0;
    ComPtr<ID3D12Resource> _readback;
};
