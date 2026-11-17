#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"

namespace BistroRender { class SceneTransparency; }

class GDX12WBOITTransparencyPass : public GDX12RenderPass
{
public:

	using DepthPartition = GDX12GeometryDepthPartition;

	GDX12WBOITTransparencyPass(DepthPartition partition = DepthPartition::All,
        GDX12WBOITTransparencyPass* reuse = nullptr) : IN_DepthStencil(nullptr), _partition(partition), _reuse(reuse)
	{ 
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | 
		RENDER_PASS_FLAG_USE_INSTANCES; 
		_numInputs = partition == DepthPartition::All ? 6 : 7;
		_numOutputs = 2;
		OUT_Accumulation = reuse ? reuse->OUT_Accumulation : std::make_shared<GDX12Texture>();
		OUT_Revealage = reuse ? reuse->OUT_Revealage : std::make_shared<GDX12Texture>();
		_outputs.push_back(OUT_Accumulation.get());
		_outputs.push_back(OUT_Revealage.get());
	}

	// Input 0 - DepthStencil; input 6 - optional nearest separator/scene depth for depth partitions
	// Output 0 - TransparencyAccumulation
	// Output 1 - RevealageTexture
    // Select before Initialize. Per-instance indirect commands carry the actual
    // instance ID and must not use the opaque compact-batch remap.
    void SetUnbatchedDraws(bool enabled) { _unbatchedDraws = enabled; }
    void SetStaticIBLOnly(bool enabled) { _staticIBLOnly = enabled; }
    void SetAtmosphereEnabled(bool enabled) { _atmosphereEnabled = enabled; }
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
        const D3D_SHADER_MACRO defines[] = {{"TRANSPARENT_UNBATCHED", _unbatchedDraws ? "1" : "0"},
            {"TRANSPARENT_STATIC_IBL_ONLY", _staticIBLOnly ? "1" : "0"},
            {"TRANSPARENT_ATMOSPHERE", !_staticIBLOnly || _atmosphereEnabled ? "1" : "0"}, {nullptr, nullptr}};
		_transparencyVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", defines, "VS", "vs");
		_transparencyPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", defines, _partition == DepthPartition::Far ? "PSFar" :
            _partition == DepthPartition::Near ? "PSNear" : "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
        RSDesc1.InstancedGeometry = true;
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
    friend class BistroRender::SceneTransparency;
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
    DepthPartition _partition;
    GDX12WBOITTransparencyPass* _reuse;
    bool _unbatchedDraws = false;
    bool _staticIBLOnly = false;
    bool _atmosphereEnabled = false;
};
