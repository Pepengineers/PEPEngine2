#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class SyntheticBufferComputePass : public GDX12RenderPass
{
protected:
    void InitializeShader(PassInitContext& context, const char* entryPoint, bool readsBuffer);
    GDX12CommandList* Bind(PassExecutionContext& context);

private:
    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipeline;
};

class SyntheticBufferWritePass final : public SyntheticBufferComputePass
{
public:
    explicit SyntheticBufferWritePass(UINT sizeMiB);
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    UINT _sizeMiB;
};

class SyntheticBufferReadPass final : public SyntheticBufferComputePass
{
public:
    SyntheticBufferReadPass(UINT sizeMiB, UINT readCount, bool* verified = nullptr);
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    void VerifyChecksums(GDX12CommandQueue& queue);
    UINT ExpectedChecksum(UINT group) const;

    UINT _sizeMiB;
    UINT _readCount;
    bool* _verified;
    ComPtr<ID3D12Resource> _readback;
    UINT _verificationSeed = 0;
    bool _verificationRecorded = false;
};
