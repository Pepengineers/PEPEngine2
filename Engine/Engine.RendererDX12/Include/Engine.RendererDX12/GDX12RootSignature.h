#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12Device;

struct GDX12RootSignatureDesc
{
	int NumCBVSlots = 0;
	int NumSRVSlots = 0;
	int NumUAVSlots = 0;
	std::vector<CD3DX12_STATIC_SAMPLER_DESC> StaticSamplers;
};

class GDX12RootSignature
{
public:
	GDX12RootSignature(GDX12Device* device, const GDX12RootSignatureDesc& desc);
	~GDX12RootSignature();

	ComPtr<ID3D12RootSignature> GetRootSignature() const { return _rootSignature; }
	const GDX12RootSignatureDesc& GetDesc() const { return _desc; }

	int GetTRootParamIndex(int registerIndex);
	int GetBRootParamIndex(int registerIndex);
	int GetURootParamIndex(int registerIndex);


private:
	GDX12Device* _device;
	ComPtr<ID3D12RootSignature> _rootSignature;
	GDX12RootSignatureDesc _desc;

	int _cbv0ParamIndex;
	int _srv0ParamIndex;
	int _uav0ParamIndex;
};