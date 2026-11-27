#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12Device;

struct GDX12RootSignatureRange
{
	GDX12RootSignatureRange(UINT numDescs, UINT descOffset = 0) 
		: NumDescriptors(numDescs), DescriptorOffset(descOffset) {}
	UINT NumDescriptors = 0;
	UINT DescriptorOffset = 0;
};

struct GDX12RootSignatureDesc
{
	UINT NumSingleCBVSlots = 0;
	UINT NumSingleSRVSlots = 0;
	D3D12_SHADER_VISIBILITY SingleSRVVisibility = D3D12_SHADER_VISIBILITY_ALL;
	UINT NumSingleUAVSlots = 0;
	std::vector<CD3DX12_STATIC_SAMPLER_DESC> StaticSamplers;
	std::vector<GDX12RootSignatureRange> SRVRanges;
	std::vector<GDX12RootSignatureRange> UAVRanges;
	std::vector<GDX12RootSignatureRange> CBVRanges;
	std::vector<UINT> Constants;
	// Registers advance within each resource type, spaces are independent between types.
	UINT CBVBaseRegister = 0;
	UINT CBVRegisterSpace = 0;
	UINT SRVBaseRegister = 0;
	UINT SRVRegisterSpace = 0;
	UINT UAVBaseRegister = 0;
	UINT UAVRegisterSpace = 0;
	D3D12_DESCRIPTOR_RANGE_FLAGS SingleSRVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
	D3D12_DESCRIPTOR_RANGE_FLAGS SingleUAVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
	D3D12_DESCRIPTOR_RANGE_FLAGS CBVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
	D3D12_DESCRIPTOR_RANGE_FLAGS SRVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
	D3D12_DESCRIPTOR_RANGE_FLAGS UAVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
	D3D12_ROOT_SIGNATURE_FLAGS Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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
