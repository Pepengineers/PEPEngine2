#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

#include <memory>

inline constexpr DXGI_FORMAT SyntheticTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

class SyntheticTexturePass : public GDX12RenderPass
{
protected:
    explicit SyntheticTexturePass(UINT extent);
    void InitializeShader(PassInitContext& context, const char* entry, bool readsTexture);
    GDX12CommandList* Bind(PassExecutionContext& context);
    void Dispatch(GDX12CommandList& commands, UINT seed = 0) const;

    const UINT _extent;
    const UINT _groupsPerRow;

private:
    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipeline;
};

class SyntheticTextureWritePass final : public SyntheticTexturePass
{
public:
    explicit SyntheticTextureWritePass(UINT extent) : SyntheticTexturePass(extent) {}

    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
};

class SyntheticTextureReadPass final : public SyntheticTexturePass
{
public:
    SyntheticTextureReadPass(UINT extent, bool* verified = nullptr) : SyntheticTexturePass(extent), _verified(verified) {}

    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    void ValidateChecksum(PassExecutionContext& context, GDX12CommandList& commands, GDX12Resource& checksum);
    UINT ExpectedChecksum(UINT group) const;

    bool* _verified;
    bool _validationRecorded = false;
    UINT _validationSeed = 0;
    ComPtr<ID3D12Resource> _readback;
};
