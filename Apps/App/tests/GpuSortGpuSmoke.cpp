#include "Engine.RendererDX12/GDX12GpuRadixSort.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

namespace
{
    void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
    void Upload(GDX12Device& device,GDX12UploadBuffer<UINT>& destination,const std::vector<UINT>& values)
    {
        GDX12UploadBuffer<UINT> upload(&device,static_cast<UINT>(values.size()),EBufferType::Upload,false);
        upload.CopyData(0,values.data(),static_cast<UINT>(values.size()));
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList();
        commands->ResourceBarrier({destination.GetResource().GetCopyDestBarrier()});
        commands->GetCommandList()->CopyBufferRegion(destination.GetResource().D3DResource.Get(),0,upload.GetResource().D3DResource.Get(),0,values.size()*sizeof(UINT));
        commands->ResourceBarrier({destination.GetResource().GetSRVBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
    }
    std::vector<UINT> Read(GDX12Device& device,GDX12UploadBuffer<UINT>& source)
    {
        const auto bytes=UINT64(source.GetElementCount())*sizeof(UINT);
        const auto heap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto description=CD3DX12_RESOURCE_DESC::Buffer(bytes); ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); commands->ResourceBarrier({source.GetResource().GetCopySourceBarrier()});
        commands->GetCommandList()->CopyBufferRegion(readback.Get(),0,source.GetResource().D3DResource.Get(),0,bytes);
        commands->ResourceBarrier({source.GetResource().GetSRVBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped=nullptr; const D3D12_RANGE read={0,static_cast<SIZE_T>(bytes)}; ThrowIfFailed(readback->Map(0,&read,&mapped));
        std::vector<UINT> result(source.GetElementCount()); std::memcpy(result.data(),mapped,static_cast<size_t>(bytes));
        const D3D12_RANGE noWrite={}; readback->Unmap(0,&noWrite); return result;
    }
}
int main()
{
    std::cout<<std::unitbuf; SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);
    try
    {
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        auto adapter=GDX12DeviceFactory::GetMostPerformantAdapter(); GDX12Device device; ThrowIfFailed(device.Initialize(adapter));
        GDX12DeviceResources resources; resources.Initialize(&device);
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info)); info->ClearStoredMessages();
        constexpr UINT capacity=409617;
        GDX12GpuRadixSort sorter(resources,capacity);
        GDX12UploadBuffer<UINT> count(&device,3,EBufferType::Default,false);
        count.CreateSRV(resources.SRV_UAV_Heap.get(),resources.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        std::mt19937 random(91372);
        std::vector<UINT> keys(capacity),payloads(capacity);
        for(bool duplicates:{false,true})
            for(UINT requested:{0u,1u,511u,512u,513u,409600u,409601u,0u,capacity,0u,UINT_MAX})
            {
                for(auto& key:keys) key=duplicates?random()%19:random();
                std::iota(payloads.begin(),payloads.end(),0u);
                Upload(device,sorter.Keys(),keys); Upload(device,sorter.Payloads(),payloads); Upload(device,count,{117,991,requested});
                auto* queue=device.GetCommandQueue(); auto* commands=queue->GetCommandList(); commands->SetDescriptorHeaps({resources.SRV_UAV_Heap.get()});
                sorter.Execute(commands,count,2); queue->ExecuteCommandList(commands); queue->Flush();
                const auto resultKeys=Read(device,sorter.Keys()),resultPayloads=Read(device,sorter.Payloads());
                const UINT active=(std::min)(requested,capacity);
                Require(std::is_sorted(resultKeys.begin(),resultKeys.begin()+active),"GPU radix keys must be ascending across block and capped-group boundaries");
                std::vector<bool> seen(active);
                for(UINT i=0;i<active;++i)
                {
                    const UINT id=resultPayloads[i]; Require(id<active,"Radix payload must remain an input ID");
                    Require(!seen[id],"GPU radix sort must preserve every payload exactly once"); seen[id]=true;
                    Require(resultKeys[i]==keys[id],"GPU radix sort must retain the key/payload association");
                }
                for(UINT i=active;i<capacity;++i)
                    Require(resultKeys[i]==keys[i]&&resultPayloads[i]==payloads[i],"GPU count must preserve all suffix canaries");
                Require(Read(device,count)==std::vector<UINT>({117,991,requested}),"GPU count input must remain unchanged");
                std::cout<<"Radix "<<(duplicates?"duplicates ":"random ")<<requested<<" passed\n";
            }
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
        {
            SIZE_T bytes; ThrowIfFailed(info->GetMessage(i,nullptr,&bytes)); std::vector<unsigned char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i,message,&bytes));
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        GDX12ShaderCompiler::Shutdown();
        std::cout<<"GPU-count radix sort: indirect stages, empty/full transitions, duplicates, payloads and canaries passed.\n"; return 0;
    }
    catch(const DxException& error) { std::wcerr<<error.ToString()<<'\n'; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; }
    return 1;
}
