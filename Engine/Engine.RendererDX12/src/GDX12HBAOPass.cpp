#include "Engine.RendererDX12/RenderPasses/GDX12HBAOPass.h"
#include <cmath>

GDX12HBAOPass::GDX12HBAOPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12HBAOPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 1.5f);
    description.AddParameter("Bias", 0.1f);
    description.AddParameter("Directions", 8);
    description.AddParameter("Steps", 4);
    description.AddParameter("Sharpness", 0.4f);
    description.AddParameter("FadeStart", 5000.0f);
    description.AddParameter("FadeEnd", 15000.0f);

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::NonPixelShaderRead;

    auto& normals = description.AddInputTexture("InNormalRoughness");
    normals.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    normals.Usages = ResourceUsage::NonPixelShaderRead;
    normals.AllowShared = true;

    auto& occlusion = description.AddOutputTexture("OutAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Size = Extent::SameAs("InDepthBuffer");
    // The horizontal blur reads the raw AO through an SRV inside this pass.
    occlusion.Usages = ResourceUsage::UnorderedAccess | ResourceUsage::NonPixelShaderRead;
    occlusion.AllowShared = true;
    description.RequireSameTextureSize({"InDepthBuffer", "InNormalRoughness"});
}

void GDX12HBAOPass::Initialize(PassInitContext& context)
{
    _radius = context.GetParameter<float>("Radius");
    _intensity = context.GetParameter<float>("Intensity");
    _bias = context.GetParameter<float>("Bias");
    _sharpness = context.GetParameter<float>("Sharpness");
    _fadeStart = context.GetParameter<float>("FadeStart");
    _fadeEnd = context.GetParameter<float>("FadeEnd");
    const int directions = context.GetParameter<int>("Directions");
    const int steps = context.GetParameter<int>("Steps");
    if (!std::isfinite(_radius) || _radius <= 0 || !std::isfinite(_intensity) || _intensity < 0 ||
        !std::isfinite(_bias) || _bias < 0 || _bias >= 1 || !std::isfinite(_sharpness) || _sharpness < 0 ||
        !std::isfinite(_fadeStart) || _fadeStart < 0 || !std::isfinite(_fadeEnd) || _fadeEnd <= _fadeStart ||
        directions < 2 || directions > 16 || steps < 2 || steps > 16)
    { throw std::invalid_argument("ERROR: HBAO requires positive Radius, nonnegative Intensity/Sharpness, Bias in [0,1), Directions/Steps in [2,16], and 0 <= FadeStart < FadeEnd. Distances are in scene centimetres."); }
    _directions = static_cast<UINT>(directions);
    _steps = static_cast<UINT>(steps);

    auto* device = context.GetDevice();
    GDX12RootSignatureDesc root;
    root.Constants = {9};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 3;
    root.NumSingleUAVSlots = 1;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(device, root);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto file = SHADERS_FOLDER "HBAOPass.hlsl";
    auto occlusion = compiler.CompileShader(device, file, nullptr, "HBAOCS", "cs");
    auto blur = compiler.CompileShader(device, file, nullptr, "BlurCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.CS = {occlusion->GetBufferPointer(), occlusion->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_occlusionPipeline)));
    pipeline.CS = {blur->GetBufferPointer(), blur->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_blurPipeline)));
    CreateBlurTexture(context);
}

void GDX12HBAOPass::CreateBlurTexture(PassInitContext& context)
{
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    if (!_blurred)
    {
        auto* resources = context.GetDeviceResources();
        GDX12TextureDesc desc;
        desc.Device = context.GetDevice();
        desc.Width = size.x;
        desc.Height = size.y;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.CreateUAV = true;
        desc.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex);
        desc.UAVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex);
        _blurred = std::make_unique<GDX12Texture>();
        _blurred->Initialize(desc);
    }
    else { _blurred->Resize(size.x, size.y); }
}

void GDX12HBAOPass::OnResourcesRecreated(PassInitContext& context)
{ CreateBlurTexture(context); }

std::vector<ExternalResourceUse> GDX12HBAOPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_blurred->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return uses;
}

void GDX12HBAOPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    auto* output = context.GetOutputTexture("OutAmbientOcclusion").GetTexture();
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    const struct { float Radius, Intensity, Bias, Sharpness; UINT Directions, Steps, BlurAxis; float FadeStart, FadeEnd; }
        parameters = {_radius, _intensity, _bias, _sharpness, _directions, _steps, 0, _fadeStart, _fadeEnd};

    cmd->BeginPixEvent("HBAO", Colors::DarkSlateBlue);
    cmd->SetComputeRootSignature(_root.get());
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->GetCommandList()->SetComputeRoot32BitConstants(0, 9, &parameters, 0);
    cmd->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    cmd->SetComputeSRV(0, context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness").GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, output->GetUAV()->GPUHandle);
    cmd->SetPipelineState(_occlusionPipeline);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);

    cmd->ResourceBarrier({output->GetResource()->GetNonPixelShaderResourceBarrier()});
    cmd->SetPipelineState(_blurPipeline);
    cmd->SetComputeSRV(2, output->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, _blurred->GetUAV()->GPUHandle);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);

    cmd->ResourceBarrier({_blurred->GetResource()->GetNonPixelShaderResourceBarrier(), output->GetResource()->GetUnorderedAccessBarrier()});
    cmd->GetCommandList()->SetComputeRoot32BitConstant(0, 1, 6);
    cmd->SetComputeSRV(2, _blurred->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, output->GetUAV()->GPUHandle);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    cmd->ResourceBarrier({_blurred->GetResource()->GetUnorderedAccessBarrier()});
    cmd->EndPixEvent();
}
