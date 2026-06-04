#include "Engine.RendererDX12/GDX12RootSignature.h"

#include "Engine.RendererDX12/GDX12Device.h"


GDX12RootSignature::GDX12RootSignature(GDX12Device* device, const GDX12RootSignatureDesc& desc)
    : _device(device)
    , _desc(desc)
    , _cbv0ParamIndex(-1)
    , _srv0ParamIndex(-1)
    , _uav0ParamIndex(-1)
{
    std::vector<CD3DX12_ROOT_PARAMETER1> rootParameters;
    std::vector<CD3DX12_DESCRIPTOR_RANGE1> ranges;
    ranges.reserve(desc.NumSingleSRVSlots + desc.NumSingleUAVSlots + desc.SRVRanges.size() + desc.UAVRanges.size());

    UINT currentParamIndex = 0;

    if (desc.NumSingleCBVSlots > 0 || !desc.CBVRanges.empty())
    {
        _cbv0ParamIndex = currentParamIndex;
        UINT currentRegister = 0;

        for (UINT i = 0; i < desc.NumSingleCBVSlots; i++)
        {
            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsConstantBufferView(currentRegister);
            rootParameters.push_back(param);
            currentParamIndex++;
            currentRegister++;
        }

        for (const auto& cbvRange : desc.CBVRanges)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, cbvRange.NumDescriptors, currentRegister,
                0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
            currentRegister += cbvRange.NumDescriptors;
        }
    }

    if (desc.NumSingleSRVSlots > 0 || !desc.SRVRanges.empty())
    {
        _srv0ParamIndex = currentParamIndex;
        UINT currentRegister = 0;

        for (UINT i = 0; i < desc.NumSingleSRVSlots; i++)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, currentRegister);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
            currentRegister++;
        }

        for (const auto& srvRange : desc.SRVRanges)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvRange.NumDescriptors, currentRegister,
                0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
            currentRegister += srvRange.NumDescriptors;
        }
    }

    if (desc.NumSingleUAVSlots > 0 || !desc.UAVRanges.empty())
    {
        _uav0ParamIndex = currentParamIndex;
        UINT currentRegister = 0;

        for (UINT i = 0; i < desc.NumSingleUAVSlots; i++)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, currentRegister,
                0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
            currentRegister++;
        }

        for (const auto& uavRange : desc.UAVRanges)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavRange.NumDescriptors, currentRegister);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
            currentRegister += uavRange.NumDescriptors;
        }
    }

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(
        static_cast<UINT>(rootParameters.size()),
        rootParameters.data(),
        static_cast<UINT>(desc.StaticSamplers.size()),
        desc.StaticSamplers.empty() ? nullptr : desc.StaticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signatureBlob, errorBlob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSignatureDesc, &signatureBlob, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob) { OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer())); }
        ThrowIfFailed(hr);
    }

    ThrowIfFailed(_device->GetDevice()->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&_rootSignature)));
}

GDX12RootSignature::~GDX12RootSignature()
{
    _rootSignature.Reset();
}

int GDX12RootSignature::GetTRootParamIndex(int registerIndex)
{
    return registerIndex + _srv0ParamIndex;
}

int GDX12RootSignature::GetBRootParamIndex(int registerIndex)
{
    return registerIndex + _cbv0ParamIndex;
}

int GDX12RootSignature::GetURootParamIndex(int registerIndex)
{
    return registerIndex + _uav0ParamIndex;
}
