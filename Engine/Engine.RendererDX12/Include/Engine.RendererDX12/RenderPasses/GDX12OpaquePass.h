#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <array>

class GDX12OpaquePass : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(_stage == Stage::DepthPrepass ? "Depth prepass" : "GBuffer");
        graph.Read(SharedResource::Scene);
        graph.Read(SharedResource::Visibility);
        graph.ReadInputs(_inputs);
        if (_stage != Stage::GBuffer)
            graph.Write(_outputs[2], "Opaque depth", Lifetime::Persistent);
        graph.Write(_outputs[3], _stage == Stage::DepthPrepass ? "Prepass normals" : "GBuffer normals");
        if (_stage != Stage::DepthPrepass)
        {
            graph.Write(_outputs[0], "GBuffer albedo");
            graph.Write(_outputs[1], "Opaque motion");
            graph.Write(_outputs[4], "GBuffer material");
            graph.Write(_outputs[5], "GBuffer emissive");
        }
    }

    enum class Stage : std::uint8_t { Full, DepthPrepass, GBuffer };

	GDX12OpaquePass(Stage stage = Stage::Full) : _stage(stage)
	{ 
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS
		| RENDER_PASS_FLAG_USE_INSTANCES; 
		_numInputs = stage == Stage::GBuffer ? 1 : 0;
		_numOutputs = 6;
		OUT_Accumulation = std::make_unique<GDX12Texture>();
		OUT_VelocityBuffer = std::make_unique<GDX12Texture>();
		OUT_DepthStencil = std::make_unique<GDX12Texture>();

		_outputs.push_back(OUT_Accumulation.get());
		_outputs.push_back(OUT_VelocityBuffer.get());
		_outputs.push_back(OUT_DepthStencil.get());
		for (auto& texture : _surfaceBuffers)
		{
			texture = std::make_unique<GDX12Texture>();
			_outputs.push_back(texture.get());
		}
	}

    void SetInputs(std::vector<IRenderPassLink*> inputs) override
    {
        GDX12RenderPass::SetInputs(std::move(inputs));
        if (_stage == Stage::GBuffer) _outputs[2] = _inputs.at(0);
    }

	// Output 0 - Albedo/metallic
	// Output 1 - MotionVectors
	// Output 2 - DepthStencil
	// Outputs 3, 4, 5 - Normal/roughness, F0/baked, emissive
	void Initialize() override
	{
        const UINT width = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
        const UINT height = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
        if (_stage != Stage::DepthPrepass)
        {
            GDX12RenderHelpers::InitializeRenderTarget(*OUT_Accumulation, *_resources, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
            GDX12RenderHelpers::InitializeRenderTarget(*OUT_VelocityBuffer, *_resources, width, height, DXGI_FORMAT_R16G16_FLOAT);
        }
        for (auto& texture : _surfaceBuffers)
            if (_stage != Stage::DepthPrepass || texture == _surfaceBuffers[0])
                GDX12RenderHelpers::InitializeRenderTarget(*texture, *_resources, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);

        if (_stage != Stage::GBuffer)
        {
            auto& desc = _depthDesc;
            desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
            desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            desc.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.SRVDesc.Texture2D.MipLevels = 1;
            desc.CreateDSV = true;
            desc.DSVHeap = _resources->DSVHeap.get();
            desc.DSVHeapIndex = desc.DSVHeap->GetAvailableIndex();
            desc.Format = desc.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
            desc.ClearValue = { 0, 0, 0, 0 };
            desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            CreateDepth(width, height);
        }

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_opaqueVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "VS", "vs");
		_opaquePS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, _stage == Stage::DepthPrepass ? "PrepassPS" : "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
        RSDesc1.InstancedGeometry = true;
		RSDesc1.NumSingleCBVSlots = 2;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		RSDesc1.Constants.push_back(1);
		_opaqueRS = std::make_unique<GDX12RootSignature>(_resources->Device, RSDesc1);

        _opaqueCS = GDX12RenderHelpers::CreateIndexedDrawSignature(_resources->Device, _opaqueRS.get());

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};

		PSODesc1.InputLayout = { _resources->InputLayouts["Default"].data(), (UINT)_resources->InputLayouts["Default"].size() };
		PSODesc1.pRootSignature = _opaqueRS->GetRootSignature().Get();
		PSODesc1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
		PSODesc1.SampleMask = UINT_MAX;
		PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PSODesc1.NumRenderTargets = 5;
		PSODesc1.RTVFormats[0] = OUT_Accumulation->GetFormat();
		PSODesc1.RTVFormats[1] = OUT_VelocityBuffer->GetFormat();
		for (UINT i = 0; i < _surfaceBuffers.size(); ++i)
			PSODesc1.RTVFormats[i + 2] = _surfaceBuffers[i]->GetFormat();
		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DSVFormat = _outputs[2]->GetTexture()->GetFormat();
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_opaqueVS->GetBufferPointer()), _opaqueVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_opaquePS->GetBufferPointer()), _opaquePS->GetBufferSize() };
        if (_stage == Stage::DepthPrepass)
        {
            PSODesc1.NumRenderTargets = 1;
            std::fill(std::begin(PSODesc1.RTVFormats), std::end(PSODesc1.RTVFormats), DXGI_FORMAT_UNKNOWN);
            PSODesc1.RTVFormats[0] = _surfaceBuffers[0]->GetFormat();
        }
        else if (_stage == Stage::GBuffer)
        {
            PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
            PSODesc1.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_opaquePSO)));
        if (_stage == Stage::DepthPrepass)
        {
            auto depthVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "DepthVS", "vs");
            auto depthPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "DepthPS", "ps");
            PSODesc1.NumRenderTargets = 0;
            PSODesc1.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
            PSODesc1.VS = { depthVS->GetBufferPointer(), depthVS->GetBufferSize() };
            PSODesc1.PS = { depthPS->GetBufferPointer(), depthPS->GetBufferSize() };
            ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_depthOnlyPSO)));
        }
	}

	// VisibilityBuffers will automatically be selected from CameraCBIndex
	// It requires GPUCullingPass to be executed beforehand
	void Execute(GDX12CommandList* cmdList) override
	{
		auto& currentFrameConstants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
		auto& currentCameraVisBuffers = currentFrameConstants->CameraVisibilityCommands[_commonData->ActiveCameraCBufferIndex];
		GDX12Texture* depthStencil = _outputs[2]->GetTexture();

		GDX12_SCOPED_EVENT(cmdList, _stage == Stage::DepthPrepass ? "Depth prepass" : "Opaque G-buffer", Colors::ForestGreen);
		cmdList->SetViewport(depthStencil->GetViewport());
		cmdList->SetScissorRect(depthStencil->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_opaqueRS.get());
        const auto geometryAddress = _resources->GeometryBuffer->GetVertexAddress();
        cmdList->GetCommandList()->SetGraphicsRootShaderResourceView(_opaqueRS->GetGeometryRootParamIndex(), geometryAddress);
        cmdList->GetCommandList()->SetGraphicsRootShaderResourceView(_opaqueRS->GetGeometryRootParamIndex() + 1,
            currentCameraVisBuffers.OpaqueBatches ? currentCameraVisBuffers.OpaqueBatches->VisibleInstancesAddress() : geometryAddress);

        const bool writeNormals = _stage != Stage::DepthPrepass || _commonData->AOAfterDepthPrepass;
        cmdList->SetPipelineState(writeNormals ? _opaquePSO.Get() : _depthOnlyPSO.Get());
		cmdList->SetGraphicsRootConstantBufferView(1, currentFrameConstants->MainCB->GetElementAddress(0));
		cmdList->SetGraphicsRootConstantBufferView(2, currentFrameConstants->CameraCB->
			GetElementAddress(_commonData->ActiveCameraCBufferIndex));
		cmdList->SetGeometryBuffer(_resources->GeometryBuffer.get());
		cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->SetGraphicsSRV(0, currentFrameConstants->ShadowMaterials->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, currentFrameConstants->ShadowTransforms->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, currentFrameConstants->ShadowInstances->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(3, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
        cmdList->ResourceBarrier({
            currentCameraVisBuffers.VisibleOpaqueCommandsCache->GetResource().GetIndirectArgsBarrier(),
            currentCameraVisBuffers.OpaqueDrawCounter->GetResource().GetIndirectArgsBarrier(),
            depthStencil->GetResource()->GetDepthWriteBarrier() });
        if (_stage != Stage::GBuffer)
            cmdList->GetCommandList()->ClearDepthStencilView(depthStencil->GetDSV()->CPUHandle,
                D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        if (_stage == Stage::DepthPrepass)
        {
            if (writeNormals)
            {
                cmdList->ResourceBarrier({ _surfaceBuffers[0]->GetResource()->GetRenderTargetBarrier() });
                cmdList->SetRenderTargets({ _surfaceBuffers[0].get() }, depthStencil);
                cmdList->ClearRenderTargetView(_surfaceBuffers[0].get());
            }
            else cmdList->SetRenderTargets({}, depthStencil);
        }
        else
        {
            cmdList->ResourceBarrier({ OUT_Accumulation->GetResource()->GetRenderTargetBarrier(),
                OUT_VelocityBuffer->GetResource()->GetRenderTargetBarrier(),
                _surfaceBuffers[0]->GetResource()->GetRenderTargetBarrier(),
                _surfaceBuffers[1]->GetResource()->GetRenderTargetBarrier(),
                _surfaceBuffers[2]->GetResource()->GetRenderTargetBarrier() });
            cmdList->SetRenderTargets({ OUT_Accumulation.get(), OUT_VelocityBuffer.get(),
                _surfaceBuffers[0].get(), _surfaceBuffers[1].get(), _surfaceBuffers[2].get() }, depthStencil);
            cmdList->ClearRenderTargetView(OUT_Accumulation.get());
            cmdList->ClearRenderTargetView(OUT_VelocityBuffer.get());
            for (auto& texture : _surfaceBuffers) cmdList->ClearRenderTargetView(texture.get());
        }
		cmdList->ExecuteIndirect(_opaqueCS.Get(), _resources->IndirectCommandsCache->GetElementCount(),
			(currentCameraVisBuffers.OpaqueBatches ? currentCameraVisBuffers.OpaqueBatches->Commands() : *currentCameraVisBuffers.VisibleOpaqueCommandsCache).GetResource().D3DResource.Get(), 0,
			currentCameraVisBuffers.OpaqueDrawCounter->GetResource().D3DResource.Get(), 0);
        if (_resources->Grass)
        {
            const auto stage = _stage == Stage::DepthPrepass ?
                (writeNormals ? GDX12GrassRenderer::Stage::PrepassNormals : GDX12GrassRenderer::Stage::Depth) :
                (_stage == Stage::GBuffer ? GDX12GrassRenderer::Stage::GBuffer : GDX12GrassRenderer::Stage::Full);
            _resources->Grass->Draw(cmdList, stage, _commonData->CameraView * _commonData->CameraProjection,
                _commonData->CameraView.Invert().Translation(),
                currentFrameConstants->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
        }
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
        if (_stage != Stage::DepthPrepass)
        {
            OUT_Accumulation->Resize(newWidth, newHeight);
            OUT_VelocityBuffer->Resize(newWidth, newHeight);
        }
        for (auto& texture : _surfaceBuffers)
            if (_stage != Stage::DepthPrepass || texture == _surfaceBuffers[0]) texture->Resize(newWidth, newHeight);
        if (_stage != Stage::GBuffer) CreateDepth(newWidth, newHeight);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();

		OUT_Accumulation.reset();
		OUT_VelocityBuffer.reset();
		OUT_DepthStencil.reset();
		_depthDesc.ExternalResource.Reset();
		for (auto& texture : _surfaceBuffers) texture.reset();
		_opaqueVS.Reset();
		_opaquePS.Reset();
		_opaqueRS.reset();
		_opaqueCS.Reset();
		_opaquePSO.Reset();
        _depthOnlyPSO.Reset();
	}

private:
    Stage _stage;
	void CreateDepth(UINT width, UINT height)
	{
		_depthDesc.Width = width;
		_depthDesc.Height = height;
		_depthDesc.ExternalResource.Reset();
		const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, width, height,
			1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		D3D12_CLEAR_VALUE clear = {};
		clear.Format = DXGI_FORMAT_D32_FLOAT;
		clear.DepthStencil.Depth = 0.0f;
		_depthDesc.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource,
			D3D12_RESOURCE_STATE_COMMON, &clear);
		OUT_DepthStencil->Initialize(_depthDesc);
	}

	ComPtr<ID3DBlob> _opaqueVS;
	ComPtr<ID3DBlob> _opaquePS;
	std::unique_ptr<GDX12RootSignature> _opaqueRS;
	ComPtr<ID3D12CommandSignature> _opaqueCS;
	ComPtr<ID3D12PipelineState> _opaquePSO;
    ComPtr<ID3D12PipelineState> _depthOnlyPSO;

	std::unique_ptr<GDX12Texture> OUT_Accumulation;
	std::unique_ptr<GDX12Texture> OUT_VelocityBuffer;
	std::unique_ptr<GDX12Texture> OUT_DepthStencil;
	std::array<std::unique_ptr<GDX12Texture>, 3> _surfaceBuffers;
	GDX12TextureDesc _depthDesc;
};
