#include "Engine.RendererDX12/GDX12GpuRadixSort.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <algorithm>
#include <stdexcept>

namespace
{
    std::unique_ptr<GDX12UploadBuffer<UINT>> Buffer(GDX12DeviceResources& resources,UINT size,bool readable=false)
    {
        auto buffer=std::make_unique<GDX12UploadBuffer<UINT>>(resources.Device,size,EBufferType::Default,false);
        auto* heap=resources.SRV_UAV_Heap.get();
        buffer->CreateUAV(heap,heap->GetAvailableIndex(ConstantsResources));
        if(readable) buffer->CreateSRV(heap,heap->GetAvailableIndex(ConstantsResources));
        return buffer;
    }
}
GDX12GpuRadixSort::GDX12GpuRadixSort(GDX12DeviceResources& resources,UINT capacity):_resources(resources),_capacity(capacity)
{
    if(capacity==0||capacity>UINT_MAX/sizeof(UINT)) throw std::invalid_argument("GPU radix capacity must fit a nonempty uint buffer");
    for(auto& buffer:_keys) buffer=Buffer(resources,capacity,true);
    for(auto& buffer:_payloads) buffer=Buffer(resources,capacity,true);
    const UINT groups=(std::min)((capacity+511)/512,800u);
    _sum=Buffer(resources,16*groups); _reduced=Buffer(resources,16*((groups+511)/512));
    _constants=Buffer(resources,8*64); _arguments=Buffer(resources,9);
    GDX12RootSignatureDesc setup; setup.Constants.push_back(2); setup.NumSingleSRVSlots=1; setup.NumSingleUAVSlots=2;
    _setupRoot=std::make_unique<GDX12RootSignature>(resources.Device,setup);
    GDX12RootSignatureDesc sort; sort.NumSingleCBVSlots=1; sort.NumSingleUAVSlots=6;
    _sortRoot=std::make_unique<GDX12RootSignature>(resources.Device,sort);
    const char* entries[]={"SetupCS","CountCS","ReduceCS","ScanCS","ScanAddCS","ScatterCS"};
    for(UINT i=0;i<_pipelines.size();++i)
    {
        const D3D_SHADER_MACRO setupDefine[]={{"SORT_SETUP","1"},{nullptr,nullptr}},scanAddDefine[]={{"SORT_SCAN_ADD","1"},{nullptr,nullptr}};
        const auto* defines=i==0?setupDefine:i==4?scanAddDefine:nullptr;
        auto shader=GDX12ShaderCompiler::GetInstance().CompileShader(resources.Device,SHADERS_FOLDER "GpuRadixSort.hlsl",defines,entries[i],"cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso={}; pso.pRootSignature=(i==0?_setupRoot:_sortRoot)->GetRootSignature().Get();
        pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
        ThrowIfFailed(resources.Device->GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&_pipelines[i])));
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument={}; argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature={}; signature.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS); signature.NumArgumentDescs=1; signature.pArgumentDescs=&argument;
    ThrowIfFailed(resources.Device->GetDevice()->CreateCommandSignature(&signature,nullptr,IID_PPV_ARGS(&_dispatch)));
}
GDX12GpuRadixSort::~GDX12GpuRadixSort() { _resources.Device->GetCommandQueue()->Flush(); }

void GDX12GpuRadixSort::Execute(GDX12CommandList* commands,GDX12UploadBuffer<UINT>& count,UINT countIndex)
{
    if(countIndex>=count.GetElementCount()||!count.GetSRV()) throw std::invalid_argument("GPU radix count needs an in-range SRV element");
    commands->SetDescriptorHeaps({_resources.SRV_UAV_Heap.get()});
    commands->ResourceBarrier({count.GetResource().GetSRVBarrier(),_constants->GetResource().GetUnorderedAccessBarrier(),_arguments->GetResource().GetUnorderedAccessBarrier()});
    commands->SetComputeRootSignature(_setupRoot.get()); commands->SetPipelineState(_pipelines[0]);
    const UINT setup[]={_capacity,countIndex}; commands->GetCommandList()->SetComputeRoot32BitConstants(0,2,setup,0);
    commands->SetComputeSRV(0,count.GetSRV()->GPUHandle); commands->SetComputeUAV(0,_constants->GetUAV()->GPUHandle); commands->SetComputeUAV(1,_arguments->GetUAV()->GPUHandle);
    commands->Dispatch(1,1,1);
    commands->ResourceBarrier({_constants->GetResource().GetUAVBarrier(),_arguments->GetResource().GetUAVBarrier(),
        _constants->GetResource().GetGenericReadBarrier(),_arguments->GetResource().GetIndirectArgsBarrier()});
    for(UINT i=0;i<2;++i) commands->ResourceBarrier({_keys[i]->GetResource().GetUnorderedAccessBarrier(),_payloads[i]->GetResource().GetUnorderedAccessBarrier()});
    commands->ResourceBarrier({_sum->GetResource().GetUnorderedAccessBarrier(),_reduced->GetResource().GetUnorderedAccessBarrier()});
    commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::UAV(nullptr)});
    commands->SetComputeRootSignature(_sortRoot.get());
    commands->SetComputeUAV(4,_sum->GetUAV()->GPUHandle); commands->SetComputeUAV(5,_reduced->GetUAV()->GPUHandle);
    const auto dispatch=[&](UINT pipeline,UINT argumentIndex)
    {
        commands->SetPipelineState(_pipelines[pipeline]);
        commands->ExecuteIndirect(_dispatch.Get(),1,_arguments->GetResource().D3DResource.Get(),argumentIndex*sizeof(D3D12_DISPATCH_ARGUMENTS),nullptr);
    };
    for(UINT pass=0;pass<8;++pass)
    {
        const UINT source=pass&1u,destination=1-source;
        // Default buffers do not apply the 256-byte constant-buffer alignment.
        commands->SetComputeRootConstantBufferView(0,_constants->GetResource().D3DResource->GetGPUVirtualAddress()+pass*256);
        commands->SetComputeUAV(0,_keys[source]->GetUAV()->GPUHandle); commands->SetComputeUAV(1,_keys[destination]->GetUAV()->GPUHandle);
        commands->SetComputeUAV(2,_payloads[source]->GetUAV()->GPUHandle); commands->SetComputeUAV(3,_payloads[destination]->GetUAV()->GPUHandle);
        dispatch(1,0); commands->ResourceBarrier({_sum->GetResource().GetUAVBarrier()});
        dispatch(2,1); commands->ResourceBarrier({_reduced->GetResource().GetUAVBarrier()});
        dispatch(3,2); commands->ResourceBarrier({_reduced->GetResource().GetUAVBarrier()});
        dispatch(4,1); commands->ResourceBarrier({_sum->GetResource().GetUAVBarrier()});
        dispatch(5,0); commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::UAV(nullptr)});
    }
    commands->ResourceBarrier({_keys[0]->GetResource().GetSRVBarrier(),_payloads[0]->GetResource().GetSRVBarrier()});
}
