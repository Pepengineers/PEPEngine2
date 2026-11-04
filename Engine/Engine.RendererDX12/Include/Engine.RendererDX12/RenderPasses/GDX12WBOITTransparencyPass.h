#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"

class GDX12WBOITTransparencyPass : public GDX12RenderPass
{
public:
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _resources ? _resources->GetGeometryBufferAccesses(GDX12GeometryStage::Transparency) : std::vector<GDX12RenderGraph::BufferAccess>{}; }

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(_partition == WaterPartition::Front ? "Front transparency" :
            _partition == WaterPartition::Behind ? "Behind-water transparency" : "Transparency");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Read(SharedResource::Visibility);
        graph.Read(SharedResource::Environment);
        graph.Read(SharedResource::ReflectionProbes);
        graph.Write(_outputs[0], "Transparency accumulation");
        graph.Write(_outputs[1], "Transparency revealage");
    }

	enum class WaterPartition : std::uint8_t { All, Behind, Front };

	GDX12WBOITTransparencyPass(WaterPartition partition = WaterPartition::All,
        GDX12WBOITTransparencyPass* reuse = nullptr) : IN_DepthStencil(nullptr), _partition(partition), _reuse(reuse)
	{ 
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | 
		RENDER_PASS_FLAG_USE_INSTANCES; 
		_numInputs = partition == WaterPartition::All ? 6 : 7;
		_numOutputs = 2;
		OUT_Accumulation = reuse ? reuse->OUT_Accumulation : std::make_shared<GDX12Texture>();
		OUT_Revealage = reuse ? reuse->OUT_Revealage : std::make_shared<GDX12Texture>();
		_outputs.push_back(OUT_Accumulation.get());
		_outputs.push_back(OUT_Revealage.get());
	}

	// Input 0 - DepthStencil; input 6 - combined nearest water/opaque depth for water partitions
	// Output 0 - TransparencyAccumulation
	// Output 1 - RevealageTexture
	void Initialize() override
	{
		IN_DepthStencil = _inputs[0];
        UpdateReadOnlyDepth();

        if (!_reuse)
        {
            const UINT width = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
            const UINT height = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
            GDX12RenderHelpers::InitializeRenderTarget(*OUT_Accumulation, *_resources, width, height,
                DXGI_FORMAT_R16G16B16A16_FLOAT, { 0, 0, 0, 0 });
            GDX12RenderHelpers::InitializeRenderTarget(*OUT_Revealage, *_resources, width, height,
                DXGI_FORMAT_R16_FLOAT, { 1, 1, 1, 1 });
        }

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_transparencyVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "VS", "vs");
		_transparencyPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, _partition == WaterPartition::Behind ? "PSBehindWater" :
            _partition == WaterPartition::Front ? "PSFrontWater" : "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 4;
		RSDesc1.NumSingleSRVSlots = 16;
		RSDesc1.StaticSamplers = GetStaticSamplers();
        RSDesc1.StaticSamplers.emplace_back(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
		CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
		shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		RSDesc1.StaticSamplers.push_back(shadowSampler);
		RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		RSDesc1.Constants.push_back(1);
		_transparencyRS = std::make_unique<GDX12RootSignature>(_resources->Device, RSDesc1);

        _transparencyCS = GDX12RenderHelpers::CreateIndexedDrawSignature(_resources->Device, _transparencyRS.get());

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};
		PSODesc1.InputLayout = { _resources->InputLayouts["Default"].data(), (UINT)_resources->InputLayouts["Default"].size() };
		PSODesc1.pRootSignature = _transparencyRS->GetRootSignature().Get();
		PSODesc1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState.IndependentBlendEnable = true;
		PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
		PSODesc1.SampleMask = UINT_MAX;
		PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		// Accumulation
		PSODesc1.BlendState.RenderTarget[0].BlendEnable = true;
		PSODesc1.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		// Revealage
		PSODesc1.BlendState.RenderTarget[1].BlendEnable = true;
		PSODesc1.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
		PSODesc1.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
		PSODesc1.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		// Disable depth write
		PSODesc1.DepthStencilState.DepthEnable = true;
		PSODesc1.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.DepthStencilState.StencilEnable = false;

		PSODesc1.NumRenderTargets = 2;
		PSODesc1.RTVFormats[0] = OUT_Accumulation->GetFormat();
		PSODesc1.RTVFormats[1] = OUT_Revealage->GetFormat();
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_transparencyVS->GetBufferPointer()), _transparencyVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_transparencyPS->GetBufferPointer()), _transparencyPS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_transparencyPSO)));
	}

	// VisibilityBuffers will automatically be selected from CameraCBIndex
	// It requires GPUCullingPass to be executed beforehand
	bool Rendered() const { return _rendered; }

	void Execute(GDX12CommandList* cmdList) override
	{
        _rendered = _partition != WaterPartition::Front ||
            (_inputs.size() > 6 && _inputs[6]->GetTexture() != _inputs[0]->GetTexture()) ||
            !_resources->ParticleEmitters.empty();
        if (!_rendered) return;
		auto& currentFrameConstants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
		auto& currentCameraVisBuffers = currentFrameConstants->CameraVisibilityCommands[_commonData->ActiveCameraCBufferIndex];
		GDX12Texture* depthStencil = IN_DepthStencil->GetTexture();

		GDX12_SCOPED_EVENT(cmdList, "Transparent Render Pass", Colors::Aqua);
		cmdList->SetViewport(OUT_Accumulation->GetViewport());
		cmdList->SetScissorRect(OUT_Accumulation->GetScissorRect());
        GDX12GeometryContext view{ GDX12GeometryStage::Transparency,
            _commonData->CameraView * _commonData->CameraProjection, _commonData->CameraView.Invert().Translation(),
            currentFrameConstants->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex) };
        view.Key = 1;
        view.FrameSlot = _resources->CurrFrameConstantsIndex;
        view.ColorTargetCount = 2;
        view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
        view.ColorFormats[0] = OUT_Accumulation->GetFormat();
        view.ColorFormats[1] = OUT_Revealage->GetFormat();
        GDX12CaptureLighting lighting;
        lighting.SunShadow = _inputs[1]->GetTexture();
        lighting.Irradiance = _inputs[2]->GetTexture();
        lighting.Prefiltered = _inputs[3]->GetTexture();
        lighting.BRDF = _inputs[4]->GetTexture();
        lighting.LocalShadows = _inputs[5]->GetTexture();
        lighting.ShadowConstants = currentFrameConstants->ShadowCB->GetElementAddress(0);
        view.CaptureLighting = &lighting;
        GDX12GraphicsBindings binding;
        binding.Stage = view.Stage;
        binding.ColorTargetCount = view.ColorTargetCount;
        binding.ColorFormats = view.ColorFormats;
        binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
        binding.IndirectSignature = _transparencyCS.Get();
        binding.IndirectStride = sizeof(GDX12IndirectDrawArgs);
		binding.Root = _transparencyRS.get();
		binding.Pipeline = _transparencyPSO;
		binding.CBVs.push_back({ 1, currentFrameConstants->MainCB->GetElementAddress(0) });
		binding.CBVs.push_back({ 3, currentFrameConstants->ShadowCB->GetElementAddress(0) });
		binding.CBVs.push_back({ 2, currentFrameConstants->CameraCB->
			GetElementAddress(_commonData->ActiveCameraCBufferIndex) });
		binding.Heap = _resources->SRV_UAV_Heap.get();
		binding.SRVs.push_back({ 0, currentFrameConstants->ShadowMaterials->GetSRV()->GPUHandle });
		binding.SRVs.push_back({ 1, currentFrameConstants->ShadowTransforms->GetSRV()->GPUHandle });
		binding.SRVs.push_back({ 2, currentFrameConstants->ShadowInstances->GetSRV()->GPUHandle });
		for (UINT input = 1; input < 5; ++input)
			binding.SRVs.push_back({ 2 + input, _inputs[input]->GetTexture()->GetSRV()->GPUHandle });
		binding.SRVs.push_back({ 7, currentFrameConstants->ShadowLights->GetSRV()->GPUHandle });
		binding.SRVs.push_back({ 8, _inputs[5]->GetTexture()->GetSRV()->GPUHandle });
		binding.SRVs.push_back({ 9, _resources->ReflectionRadianceSRV.GPUHandle });
		binding.SRVs.push_back({ 10, currentFrameConstants->ReflectionProbes->GetSRV()->GPUHandle });
        binding.CBVs.push_back({ 4, currentFrameConstants->SkyCB->GetElementAddress(0) });
        binding.SRVs.push_back({ 11, _resources->SkyTransmittanceSRV.GPUHandle });
        binding.SRVs.push_back({ 12, _resources->SkyAerialRadianceSRV.GPUHandle });
        binding.SRVs.push_back({ 13, _resources->SkyAerialTransmittanceSRV.GPUHandle });
        binding.SRVs.push_back({ 16, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex) });
        auto* waterDepth = (_inputs.size() > 6 ? _inputs[6] : _inputs[0])->GetTexture();
        view.SceneDepth = depthStencil;
        view.PartitionDepth = waterDepth;
        view.DepthPartition = _partition == WaterPartition::All ? GDX12GeometryDepthPartition::All :
            (_partition == WaterPartition::Behind ? GDX12GeometryDepthPartition::Behind : GDX12GeometryDepthPartition::Front);
        constexpr auto depthReadState = D3D12_RESOURCE_STATE_DEPTH_READ |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        // Fluid resolve can leave opaque depth compute-only; graphics binding also validates its pixel state.
        cmdList->ResourceBarrier({ waterDepth->GetResource()->GetSRVBarrier(),
            CD3DX12_RESOURCE_BARRIER::Transition(depthStencil->GetResource()->D3DResource.Get(),
                depthStencil->GetResource()->GetCurrentState(), depthReadState) });
        depthStencil->GetResource()->GDX12Resource::SetCurrentState(depthReadState);
		binding.SRVs.push_back({ 14, waterDepth->GetSRV()->GPUHandle });
        binding.SRVs.push_back({ 15, depthStencil->GetSRV()->GPUHandle });

        const UINT meshCount = _resources->IndirectCommandsCache->GetElementCount();
        if (meshCount)
            cmdList->ResourceBarrier({ currentCameraVisBuffers.VisibleTransparentCommandsCache->GetResource().GetIndirectArgsBarrier(),
                currentCameraVisBuffers.TransparentDrawCounter->GetResource().GetIndirectArgsBarrier() });
        cmdList->ResourceBarrier({ OUT_Accumulation->GetResource()->GetRenderTargetBarrier(),
            OUT_Revealage->GetResource()->GetRenderTargetBarrier() });
		const D3D12_CPU_DESCRIPTOR_HANDLE renderTargets[] = { OUT_Accumulation->GetRTV()->CPUHandle, OUT_Revealage->GetRTV()->CPUHandle };
        cmdList->GetCommandList()->OMSetRenderTargets(2, renderTargets, FALSE, &_readOnlyDepth->CPUHandle);
		cmdList->ClearRenderTargetView(OUT_Accumulation.get());
		cmdList->ClearRenderTargetView(OUT_Revealage.get());
        GDX12RenderBatch mesh;
        mesh.Binding = &binding;
        mesh.Geometry = _resources->GeometryBuffer.get();
        mesh.InstanceCount = meshCount;
        mesh.Draw = { GDX12DrawKind::Indirect, {}, {}, _transparencyCS.Get(),
            meshCount ? currentCameraVisBuffers.VisibleTransparentCommandsCache->GetResource().D3DResource.Get() : nullptr, 0,
            meshCount ? currentCameraVisBuffers.TransparentDrawCounter->GetResource().D3DResource.Get() : nullptr, 0,
            meshCount, sizeof(GDX12IndirectDrawArgs) };
        _resources->EmitGeometry(cmdList, view, mesh);
		cmdList->ResourceBarrier({ OUT_Accumulation->GetResource()->GetSRVBarrier(),
		OUT_Revealage->GetResource()->GetSRVBarrier() });
	}

	void Resize() override
	{
        UpdateReadOnlyDepth();
        if (_reuse) return;
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		OUT_Accumulation->Resize(newWidth, newHeight);
		OUT_Revealage->Resize(newWidth, newHeight);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		OUT_Accumulation.reset();
		OUT_Revealage.reset();
		IN_DepthStencil = nullptr;
		_transparencyVS.Reset();
		_transparencyPS.Reset();
		_transparencyRS.reset();
		_transparencyCS.Reset();
		_transparencyPSO.Reset();
        _readOnlyDepth.reset();
	}

private:
    void UpdateReadOnlyDepth()
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        auto* heap = _resources->DSVHeap.get();
        if (!_readOnlyDepth) _readOnlyDepth = std::make_unique<GDX12Descriptor>();
        const UINT index = _readOnlyDepth->HeapIndex == UINT_MAX ? heap->GetAvailableIndex() : _readOnlyDepth->HeapIndex;
        _readOnlyDepth->InitAsDSV(IN_DepthStencil->GetTexture()->GetResource()->D3DResource.Get(), &desc, heap, index);
    }
    std::unique_ptr<GDX12Descriptor> _readOnlyDepth;
	ComPtr<ID3DBlob> _transparencyVS;
	ComPtr<ID3DBlob> _transparencyPS;
	std::unique_ptr<GDX12RootSignature> _transparencyRS;
	ComPtr<ID3D12CommandSignature> _transparencyCS;
	ComPtr<ID3D12PipelineState> _transparencyPSO;

	std::shared_ptr<GDX12Texture> OUT_Accumulation;
	std::shared_ptr<GDX12Texture> OUT_Revealage;

	IRenderPassLink* IN_DepthStencil;
    WaterPartition _partition;
    GDX12WBOITTransparencyPass* _reuse;
    bool _rendered = false;
};
