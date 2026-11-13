#include "DirectFixture.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.Core/Logging/Logger.h"
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include "Common/RuntimePaths.h"
namespace {
void Check(bool v,const char* text) { if(!v) throw std::runtime_error(text); }
}
void RunDirectExecutionGpuTests(GDX12Device& device) {
    // Order guarantees executor drains before fixture and target are destroyed.
    auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM,32,32,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto target=device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowIfFailed(device.GetDevice()->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)));
    const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();
    device.GetDevice()->CreateRenderTargetView(target.Get(),nullptr,rtv);
    auto seamUpload=device.CreateResource(D3D12_HEAP_TYPE_UPLOAD,CD3DX12_RESOURCE_DESC::Buffer(16),D3D12_RESOURCE_STATE_GENERIC_READ);
    auto seamData=device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,CD3DX12_RESOURCE_DESC::Buffer(16),D3D12_RESOURCE_STATE_COMMON);
    auto seamReadback=device.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(16),D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped=nullptr; D3D12_RANGE noRead{0,0};
    ThrowIfFailed(seamUpload->Map(0,&noRead,&mapped));
    static_cast<uint32_t*>(mapped)[0]=0x12345678; seamUpload->Unmap(0,nullptr);
    Meta::DirectFixture fixture(device,2);
    Meta::Executor executor(device,2,16);
    const auto schedule=fixture.Plan(D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON);
    std::ofstream trace(Engine::Core::GetExecutableDirectory()/"Logs/direct-schedule.txt"); schedule.Write(trace);
    for(uint32_t frame=0;frame<8;++frame) {
        auto slot=frame%2;
        executor.Begin(slot); fixture.Validate(slot); fixture.Prepare(slot,frame+1);
        auto* prefix=device.GetCommandQueue()->GetCommandList();
        prefix->GetCommandList()->CopyBufferRegion(seamData.Get(),0,seamUpload.Get(),0,4);
        auto* suffix=device.GetCommandQueue()->GetCommandList();
        suffix->GetCommandList()->CopyBufferRegion(seamReadback.Get(),0,seamData.Get(),0,4);
        executor.Execute(schedule,fixture.Resources(slot,target.Get()),[&](Meta::Id op,uint32_t list,ID3D12GraphicsCommandList* cmd) {
            fixture.Record(slot,op,list,cmd,target.Get(),rtv,32,32);
        },prefix,suffix);
        Check(executor.Tokens().back()>0,"missing submitted token");
    }
    executor.Drain(); fixture.Validate(0); fixture.Validate(1);
    D3D12_RANGE seamRange{0,4}; ThrowIfFailed(seamReadback->Map(0,&seamRange,&mapped));
    const bool seamValid=static_cast<const uint32_t*>(mapped)[0]==0x12345678;
    seamReadback->Unmap(0,&noRead);
    Check(seamValid,"prefix/suffix commands lost at execution seam");
    const auto prior=device.GetCommandQueue()->FenceValue;
    const auto retirement=executor.Retirement(0);
    executor.Begin(0);
    bool draw=false, failed=false;
    try {
        executor.Execute(schedule,fixture.Resources(0,target.Get()),[&](Meta::Id op,uint32_t list,ID3D12GraphicsCommandList* cmd) {
            if(op==2) throw std::runtime_error("injected recording failure");
            if(op==6) draw=true;
            fixture.Record(0,op,list,cmd,target.Get(),rtv,32,32);
        });
    } catch(const std::runtime_error&) {failed=true;}
    Check(failed && !draw,"recording failure executed dependent draw");
    Check(device.GetCommandQueue()->FenceValue==prior && executor.Retirement(0)==retirement,"failed recording submitted or changed retirement");
    for(auto token:executor.Tokens()) Check(token==0,"failed recording published success token");
    fixture.Validate(0); // readbacks remain unchanged after failed recording
    // Block the real DIRECT queue, release it from a CPU fence after 150ms.
    // No owner-thread queue API is called from that releasing thread.
    ComPtr<ID3D12Fence> gate;
    ThrowIfFailed(device.GetDevice()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    struct ReleaseGateOnError {
        ID3D12Fence* Fence;
        ~ReleaseGateOnError() { Fence->Signal(1); }
    } releaseOnError{gate.Get()};
    executor.Begin(0); fixture.Prepare(0,11);
    ThrowIfFailed(device.GetCommandQueue()->GetCommandQueue()->Wait(gate.Get(),1));
    executor.Execute(schedule,fixture.Resources(0,target.Get()),[&](Meta::Id op,uint32_t list,ID3D12GraphicsCommandList* cmd) {
        fixture.Record(0,op,list,cmd,target.Get(),rtv,32,32);
    });
    Check(device.GetCommandQueue()->GetFence()->GetCompletedValue()<executor.Retirement(0),"delayed slot retired prematurely");
    const auto start=std::chrono::steady_clock::now();
    std::thread release([gate] {std::this_thread::sleep_for(std::chrono::milliseconds(150)); gate->Signal(1);});
    try { executor.Begin(0); } catch(...) {release.join();throw;}
    release.join();
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    Check(elapsed>=100,"slot reuse did not wait for delayed GPU work");
    fixture.Validate(0);
    // A fresh successful submission after the failed recording reuses only
    // CPU-only abandoned lists, never an in-flight allocator.
    executor.Drain();
    Log::Info("MetaRenderGraph","PASS DirectExecutionGpuTests: 8 frames/2 slots; prefix/suffix sentinel; upload multi-list; UAV dispatch; diamond WAR; draw pixel+buffer readbacks; recording failure tokens=0; delayed slot wait ms=",elapsed);
}
