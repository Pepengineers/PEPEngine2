#include "Engine.RendererDX12/RenderPasses/GDX12SkyBoxPass.h"

GDX12SkyBoxPass::GDX12SkyBoxPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12SkyBoxPass::Describe(PassDescription& description) const
{
    description.AddInputTexture("InSkyBoxMap", ResourceType::TextureCube).Usages = ResourceUsage::PixelShaderRead;

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::PixelShaderRead;

    auto& color = description.AddInputTexture("InColorTexture");
    color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
    color.Access = DataAccess::ReadWrite;
    color.Usages = ResourceUsage::RenderTarget;

    auto& velocity = description.AddInputTexture("InVelocityBuffer");
    velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
    velocity.Access = DataAccess::ReadWrite;
    velocity.Usages = ResourceUsage::RenderTarget;

    auto& outputColor = description.AddOutputTexture("OutColorTexture");
    outputColor.AliasInput = "InColorTexture";
    outputColor.Usages = ResourceUsage::RenderTarget;
    outputColor.AllowShared = true;

    auto& outputVelocity = description.AddOutputTexture("OutVelocityBuffer");
    outputVelocity.AliasInput = "InVelocityBuffer";
    outputVelocity.Usages = ResourceUsage::RenderTarget;
    outputVelocity.AllowShared = true;

    description.RequireSameTextureSize({"InColorTexture", "InDepthBuffer", "InVelocityBuffer"});
}

void GDX12SkyBoxPass::Initialize(PassInitContext& context)
{
    auto* device = context.GetDevice();
    GDX12RootSignatureDesc desc;
    desc.NumSingleCBVSlots = 1;
    desc.NumSingleSRVSlots = 2;
    desc.SingleSRVVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    desc.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(device, desc);

    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto file = SHADERS_FOLDER "SkyBoxPass.hlsl";
    auto vs = compiler.CompileShader(device, file, nullptr, "VS", "vs");
    auto ps = compiler.CompileShader(device, file, nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pipeline.RasterizerState.FrontCounterClockwise = TRUE;
    pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.SampleMask = UINT_MAX;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 2;
    pipeline.RTVFormats[0] = context.GetOutputResourceDescription("OutColorTexture").Format;
    pipeline.RTVFormats[1] = context.GetOutputResourceDescription("OutVelocityBuffer").Format;
    pipeline.SampleDesc.Count = 1;
    ThrowIfFailed(device->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_pipeline)));
}

void GDX12SkyBoxPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    auto* color = context.GetOutputTexture("OutColorTexture").GetTexture();
    auto* velocity = context.GetOutputTexture("OutVelocityBuffer").GetTexture();
    cmd->BeginPixEvent("SkyBox", Colors::CornflowerBlue);
    cmd->SetGraphicsRootSignature(_root.get());
    cmd->SetPipelineState(_pipeline);
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->SetGraphicsRootConstantBufferView(0, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    cmd->SetGraphicsSRV(0, context.GetInputTexture("InSkyBoxMap").GetSRV()->GPUHandle);
    cmd->SetGraphicsSRV(1, context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle);
    cmd->SetViewport(color->GetViewport());
    cmd->SetScissorRect(color->GetScissorRect());
    cmd->SetRenderTargets({color, velocity}, nullptr);
    cmd->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->GetCommandList()->DrawInstanced(3, 1, 0, 0);
    cmd->EndPixEvent();
}
