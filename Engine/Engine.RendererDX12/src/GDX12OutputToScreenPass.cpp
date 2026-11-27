#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"

void GDX12OutputToScreenPass::Describe(PassDescription& s) const
{
	auto& color = s.AddInputTexture(InColorTexture);
	color.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	color.Usages = ResourceUsage::PixelShaderRead;
	color.AllowShared = true;
	auto& target = s.AddInputTexture(InBackBuffer);
	target.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
	target.Access = DataAccess::WriteOnly;
	target.Usages = ResourceUsage::RenderTarget;
	auto& presented = s.AddOutputTexture(OutPresentedBackBuffer);
	presented.AliasInput = InBackBuffer;
	presented.Usages = ResourceUsage::Present;
	s.RequireSameTextureSize({InColorTexture, InBackBuffer});
}

void GDX12OutputToScreenPass::Initialize(PassInitContext& context)
{
	auto* deviceResources = context.GetDeviceResources();
	GDX12RootSignatureDesc root;
	root.NumSingleSRVSlots = 1;
	root.SingleSRVVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	_rootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, root);

	auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
	const auto shaderPath = SHADERS_FOLDER "OutputToScreenPass.hlsl";
	auto vertexShader = shaderCompiler.CompileShader(deviceResources->Device, shaderPath, nullptr, "VS", "vs");
	auto pixelShader = shaderCompiler.CompileShader(deviceResources->Device, shaderPath, nullptr, "PS", "ps");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
	pipeline.pRootSignature = _rootSignature->GetRootSignature().Get();
	pipeline.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
	pipeline.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
	pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pipeline.RasterizerState.FrontCounterClockwise = TRUE;
	pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	pipeline.DepthStencilState.DepthEnable = FALSE;
	pipeline.DepthStencilState.StencilEnable = FALSE;
	pipeline.SampleMask = UINT_MAX;
	pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipeline.NumRenderTargets = 1;
	pipeline.RTVFormats[0] = DirectX::MakeSRGB(context.GetInputResourceDescription(InBackBuffer).Format);
	pipeline.SampleDesc.Count = 1;
	ThrowIfFailed(deviceResources->Device->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_pipeline)));
}

void GDX12OutputToScreenPass::Execute(PassExecutionContext& context)
{
	auto* cmdList = context.GetCommandList();
	auto* deviceResources = context.GetDeviceResources();
	auto* target = context.GetInputTexture(InBackBuffer)->GetTexture();
	cmdList->BeginPixEvent("Output to Screen", Colors::MediumPurple);
	cmdList->SetGraphicsRootSignature(_rootSignature.get());
	cmdList->SetPipelineState(_pipeline);
	cmdList->SetDescriptorHeaps({deviceResources->SRV_UAV_Heap.get()});
	cmdList->SetGraphicsSRV(0, context.GetInputTexture(InColorTexture)->GetSRV()->GPUHandle);
	cmdList->SetViewport(target->GetViewport());
	cmdList->SetScissorRect(target->GetScissorRect());
	cmdList->SetRenderTargets({target}, nullptr);
	cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->DrawInstanced(3, 1, 0, 0);
	cmdList->EndPixEvent();
}
