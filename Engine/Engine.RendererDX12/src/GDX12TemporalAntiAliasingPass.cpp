#include "Engine.RendererDX12/RenderPasses/GDX12TemporalAntiAliasingPass.h"

GDX12TemporalAntiAliasingPass::GDX12TemporalAntiAliasingPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_JITTER; }

void GDX12TemporalAntiAliasingPass::Describe(PassDescription& description) const
{
    auto& color = description.AddInputTexture("InColorTexture");
    color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
    color.Usages = ResourceUsage::PixelShaderRead;

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::PixelShaderRead;

    auto& velocity = description.AddInputTexture("InVelocityBuffer");
    velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
    velocity.Usages = ResourceUsage::PixelShaderRead;

    auto& mask = description.AddInputTexture("InTransparencyMask");
    mask.Formats = {DXGI_FORMAT_R8_UNORM};
    mask.Usages = ResourceUsage::PixelShaderRead;

    auto& output = description.AddOutputTexture("OutColorTexture");
    output.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
    output.Size = Extent::SameAs("InColorTexture");
    output.Usages = ResourceUsage::RenderTarget;
    output.AllowShared = true;

    description.RequireSameTextureSize({"InColorTexture", "InDepthBuffer", "InVelocityBuffer", "InTransparencyMask"});
}

void GDX12TemporalAntiAliasingPass::Initialize(PassInitContext& context)
{
    auto* device = context.GetDevice();
    GDX12RootSignatureDesc root;
    root.Constants = {4};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 5;
    root.SingleSRVVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(device, root);

    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto file = SHADERS_FOLDER "TemporalAntiAliasingPass.hlsl";
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
    pipeline.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pipeline.SampleDesc.Count = 1;
    ThrowIfFailed(device->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_pipeline)));
    CreateHistory(context);
}

void GDX12TemporalAntiAliasingPass::CreateHistory(PassInitContext& context)
{
    const auto& output = context.GetOutputResourceDescription("OutColorTexture");
    auto* resources = context.GetDeviceResources();
    if (!_history)
    {
        GDX12TextureDesc desc;
        desc.Device = context.GetDevice();
        desc.Width = output.Width;
        desc.Height = output.Height;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
        desc.RTVHeap = resources->RTVHeap.get();
        desc.CreateRTV = true;
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        desc.RTVHeapIndex = resources->RTVHeap->GetAvailableIndex();
        _history = std::make_unique<GDX12Texture>();
        _history->Initialize(desc);
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        desc.RTVHeapIndex = resources->RTVHeap->GetAvailableIndex();
        _nextHistory = std::make_unique<GDX12Texture>();
        _nextHistory->Initialize(desc);
    }
    else if (_history->GetWidth() != output.Width || _history->GetHeight() != output.Height)
    {
        _history->Resize(output.Width, output.Height);
        _nextHistory->Resize(output.Width, output.Height);
    }
    ResetHistory();
}

void GDX12TemporalAntiAliasingPass::OnResourcesRecreated(PassInitContext& context)
{ CreateHistory(context); }

std::vector<ExternalResourceUse> GDX12TemporalAntiAliasingPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_history->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, {}});
    uses.push_back({_nextHistory->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, {}});
    return uses;
}

void GDX12TemporalAntiAliasingPass::ResetHistory()
{ _historyValid = false; }

void GDX12TemporalAntiAliasingPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    const auto* common = context.GetFrameData();
    if (_historyCameraIndex != common->ActiveCameraCBufferIndex || _historyProjection != common->CameraViewToClip)
    { ResetHistory(); }
    const struct { float JitterX, JitterY; UINT HistoryValid; float HistoryWeight; } parameters = {
        common->ActiveCameraJitterOffsetX, common->ActiveCameraJitterOffsetY, _historyValid ? 1u : 0u, 0.9f};

    auto* output = context.GetOutputTexture("OutColorTexture").GetTexture();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    cmd->BeginPixEvent("Temporal Anti-Aliasing", Colors::MediumPurple);
    cmd->SetGraphicsRootSignature(_root.get());
    cmd->SetPipelineState(_pipeline);
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->GetCommandList()->SetGraphicsRoot32BitConstants(0, 4, &parameters, 0);
    cmd->SetGraphicsRootConstantBufferView(1, frame.CameraCB->GetElementAddress(common->ActiveCameraCBufferIndex));
    cmd->SetGraphicsSRV(0, context.GetInputTexture("InColorTexture").GetSRV()->GPUHandle);
    cmd->SetGraphicsSRV(1, context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle);
    cmd->SetGraphicsSRV(2, context.GetInputTexture("InVelocityBuffer").GetSRV()->GPUHandle);
    cmd->SetGraphicsSRV(3, context.GetInputTexture("InTransparencyMask").GetSRV()->GPUHandle);
    cmd->SetGraphicsSRV(4, _history->GetSRV()->GPUHandle);
    cmd->SetViewport(output->GetViewport());
    cmd->SetScissorRect(output->GetScissorRect());
    cmd->SetRenderTargets({output, _nextHistory.get()}, nullptr);
    cmd->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->GetCommandList()->DrawInstanced(3, 1, 0, 0);
    cmd->EndPixEvent();

    // The pass always runs on this device's DIRECT queue, so frame N finishes writing
    // before frame N+1 reads the same history, including when their frame slots differ.
    std::swap(_history, _nextHistory);
    _historyValid = true;
    _historyCameraIndex = common->ActiveCameraCBufferIndex;
    _historyProjection = common->CameraViewToClip;
}
