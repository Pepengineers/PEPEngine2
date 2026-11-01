#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <stdexcept>
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(_front ? "Front composition" : "Behind-water composition");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Read(SharedResource::Environment);
        if (_particles) graph.Read(SharedResource::Particles);
        graph.Write(_outputs[0], "Composited HDR", Lifetime::Transient, GDX12RenderGraph::UnmanagedState, OUT_Result.get());
        if (_front) graph.Fallback(_outputs[0], _inputs[0]);
    }

    void SetParticles(GDX12ParticlesPass* particles) { _particles = particles; }
	GDX12WBOITCompositionPass(GDX12WBOITCompositionPass* reuse = nullptr,
        GDX12WBOITTransparencyPass* front = nullptr) : IN_OpaqueScene(nullptr), IN_TransparencyAccum(nullptr),
		IN_Revealage(nullptr), _reuse(reuse), _front(front)
	{ 
		_flags = RENDER_PASS_FLAG_NONE; 
		_numInputs = 3;
		_numOutputs = 1;
		OUT_Result = reuse ? reuse->OUT_Result : std::make_shared<GDX12Texture>();
        _link.Target = OUT_Result.get();
		_outputs.push_back(&_link);
	}

	// Input 0 - OpaqueScene
	// Input 1 - TransparencyAccumulation
	// Input 2 - RevealageTexture
	// Output 0 - CompositionResult
	void Initialize() override
	{
		IN_OpaqueScene = _inputs[0];
		IN_TransparencyAccum = _inputs[1];
		IN_Revealage = _inputs[2];

        if (!_reuse)
        {
            const UINT width = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
            const UINT height = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
            GDX12RenderHelpers::InitializeRenderTarget(*OUT_Result, *_resources, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        }

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_compositionVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
		const D3D_SHADER_MACRO frontDefines[] = { { "SKIP_OPAQUE_AERIAL", "1" }, { nullptr, nullptr } };
		_compositionPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "CompositionPass.hlsl", _front ? frontDefines : nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 2;
        RSDesc1.NumSingleSRVSlots = 7;
		RSDesc1.StaticSamplers = GetStaticSamplers();
        RSDesc1.StaticSamplers.emplace_back(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
		_compositionRS = std::make_unique<GDX12RootSignature>(_resources->Device, RSDesc1);

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};
		PSODesc1.InputLayout = { nullptr, 0 };
		PSODesc1.pRootSignature = _compositionRS->GetRootSignature().Get();
		PSODesc1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
		PSODesc1.SampleMask = UINT_MAX;
		PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PSODesc1.NumRenderTargets = 1;
		PSODesc1.RTVFormats[0] = OUT_Result->GetFormat();

		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DepthStencilState.DepthEnable = false;
		PSODesc1.DepthStencilState.StencilEnable = false;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_compositionVS->GetBufferPointer()), _compositionVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_compositionPS->GetBufferPointer()), _compositionPS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_compositionPSO)));
	}

	void Execute(GDX12CommandList* cmdList) override
	{
        _link.Target = IN_OpaqueScene->GetTexture();
        if (_front && !_front->Rendered()) return;
        _link.Target = OUT_Result.get();
		GDX12Texture* opaqueAccum = IN_OpaqueScene->GetTexture();
		GDX12Texture* transparencyAccum = IN_TransparencyAccum->GetTexture();
		GDX12Texture* transparencyRevealage = IN_Revealage->GetTexture();

        {
            GDX12_SCOPED_EVENT(cmdList, "Composition Render Pass", Colors::Bisque);
            cmdList->SetViewport(OUT_Result->GetViewport());
            cmdList->SetScissorRect(OUT_Result->GetScissorRect());
            cmdList->SetGraphicsRootSignature(_compositionRS.get());
            cmdList->SetPipelineState(_compositionPSO.Get());
            cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
            cmdList->ResourceBarrier({ opaqueAccum->GetResource()->GetSRVBarrier(),
            transparencyAccum->GetResource()->GetSRVBarrier(),
            transparencyRevealage->GetResource()->GetSRVBarrier(),
            OUT_Result->GetResource()->GetRenderTargetBarrier() });
            cmdList->SetRenderTargets({ OUT_Result.get() }, nullptr);
            cmdList->ClearRenderTargetView(OUT_Result.get());
            auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
            const bool hasCamera = _commonData->ActiveCameraCBufferIndex < frame->CameraCB->GetElementCount();
            if (_resources->Sky.Source == SkySource::SkyAtmosphere && (!hasCamera || _inputs.size() < 4))
                throw std::invalid_argument("Aerial perspective composition requires active camera and opaque depth");
            cmdList->SetGraphicsRootConstantBufferView(0, hasCamera ?
                frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex) : frame->SkyCB->GetElementAddress(0));
            cmdList->SetGraphicsRootConstantBufferView(1, frame->SkyCB->GetElementAddress(0));
            if (_inputs.size() > 3)
            {
                auto* depth = _inputs[3]->GetTexture();
                cmdList->ResourceBarrier({ depth->GetResource()->GetSRVBarrier() });
                cmdList->SetGraphicsSRV(3, depth->GetSRV()->GPUHandle);
            }
            else cmdList->SetGraphicsSRV(3, _resources->SkyTransmittanceSRV.GPUHandle);
            cmdList->SetGraphicsSRV(4, _resources->SkyTransmittanceSRV.GPUHandle);
            cmdList->SetGraphicsSRV(5, _resources->SkyAerialRadianceSRV.GPUHandle);
            cmdList->SetGraphicsSRV(6, _resources->SkyAerialTransmittanceSRV.GPUHandle);
            cmdList->SetGraphicsSRV(0, opaqueAccum->GetSRV()->GPUHandle);
            cmdList->SetGraphicsSRV(1, transparencyAccum->GetSRV()->GPUHandle);
            cmdList->SetGraphicsSRV(2, transparencyRevealage->GetSRV()->GPUHandle);
            cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmdList->GetCommandList()->DrawInstanced(3, 1, 0, 0);
        }
        if (_particles) _particles->Composite(cmdList, OUT_Result.get());
	}

	void Resize() override
	{
        if (_reuse) return;
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		OUT_Result->Resize(newWidth, newHeight);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		IN_OpaqueScene = nullptr;
		IN_TransparencyAccum = nullptr;
		IN_Revealage = nullptr;
		OUT_Result.reset();
		_compositionVS.Reset();
		_compositionPS.Reset();
		_compositionRS.reset();
		_compositionPSO.Reset();
        _link.Target = nullptr;
	}

private:
    GDX12ParticlesPass* _particles = nullptr;
    struct Link final : IRenderPassLink
    {
        GDX12Texture* Target = nullptr;
        GDX12Texture* GetTexture() override { return Target; }
        bool IsInitialized() override { return Target && Target->IsInitialized(); }
    } _link;
    GDX12WBOITCompositionPass* _reuse;
    GDX12WBOITTransparencyPass* _front;
	ComPtr<ID3DBlob> _compositionVS;
	ComPtr<ID3DBlob> _compositionPS;
	std::unique_ptr<GDX12RootSignature> _compositionRS;
	ComPtr<ID3D12PipelineState> _compositionPSO;

	IRenderPassLink* IN_OpaqueScene;
	IRenderPassLink* IN_TransparencyAccum;
	IRenderPassLink* IN_Revealage;
	std::shared_ptr<GDX12Texture> OUT_Result;
};
