#include "Engine.RendererDX12/GDX12RootSignature.h"

#include "Engine.RendererDX12/GDX12Device.h"


GDX12RootSignature::GDX12RootSignature(std::shared_ptr<GDX12Device> device, const GDX12RootSignatureDesc& desc)
    : _device(device)
    , _desc(desc)
    , _cbv0ParamIndex(-1)
    , _srv0ParamIndex(-1)
    , _uav0ParamIndex(-1)
{
    std::vector<CD3DX12_ROOT_PARAMETER1> rootParameters;
    std::vector<CD3DX12_DESCRIPTOR_RANGE1> ranges;
    ranges.reserve(desc.NumSRVSlots + desc.NumUAVSlots);

    UINT currentParamIndex = 0;

    if (desc.NumCBVSlots > 0)
    {
        _cbv0ParamIndex = currentParamIndex;
        for (UINT i = 0; i < desc.NumCBVSlots; i++)
        {
            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsConstantBufferView(i);
            rootParameters.push_back(param);
            currentParamIndex++;
        }
    }

    if (desc.NumSRVSlots > 0)
    {
        _srv0ParamIndex = currentParamIndex;
        for (UINT i = 0; i < desc.NumSRVSlots; i++)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
        }
    }

    if (desc.NumUAVSlots > 0)
    {
        _uav0ParamIndex = currentParamIndex;
        for (UINT i = 0; i < desc.NumUAVSlots; i++)
        {
            CD3DX12_DESCRIPTOR_RANGE1 range;
            range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, i);
            ranges.push_back(range);

            CD3DX12_ROOT_PARAMETER1 param;
            param.InitAsDescriptorTable(1, &ranges.back());
            rootParameters.push_back(param);
            currentParamIndex++;
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
