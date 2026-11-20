#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12DeferredGeometryPass : public GDX12RenderPass
{
public:
	static constexpr const char* InDrawStream = "InDrawStream";
	static constexpr const char* OutAlbedoMetallic = "OutAlbedoMetallic";
	static constexpr const char* OutVelocityBuffer = "OutVelocityBuffer";
	static constexpr const char* OutNormalRoughness = "OutNormalRoughness";
	static constexpr const char* OutF0Baked = "OutF0Baked";
	static constexpr const char* OutEmissive = "OutEmissive";
	static constexpr const char* OutDepthBuffer = "OutDepthBuffer";

	GDX12DeferredGeometryPass();
	void Describe(PassDescription& s) const override;
	void Initialize(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	// MRT order matches PS_OUTPUT in DeferredGeometryPass.hlsl.
	struct ColorTarget { const char* Name; DXGI_FORMAT Format; };
	static constexpr ColorTarget ColorTargets[] = {
		{OutAlbedoMetallic, DXGI_FORMAT_R8G8B8A8_UNORM},
		{OutVelocityBuffer, DXGI_FORMAT_R16G16_FLOAT},
		{OutNormalRoughness, DXGI_FORMAT_R16G16B16A16_FLOAT},
		{OutF0Baked, DXGI_FORMAT_R16G16B16A16_FLOAT},
		{OutEmissive, DXGI_FORMAT_R16G16B16A16_FLOAT}
	};
	ComPtr<ID3D12PipelineState> _depthPSO;
	std::unique_ptr<GDX12RootSignature> _opaqueRS;
	ComPtr<ID3D12CommandSignature> _opaqueCS;
	ComPtr<ID3D12PipelineState> _opaquePSO;

};
