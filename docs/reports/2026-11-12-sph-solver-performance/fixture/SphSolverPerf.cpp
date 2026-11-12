// Isolated production-kernel validation and GPU timestamps; no scene assets.
#include "Rendering/Water/GDX12FluidSolver.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <d3d12sdklayers.h>
#include "BistroWater.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
using Buffers = GDX12FluidSolver::Buffers;
using Particle = GDX12FluidSolver::Particle;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Dispatch { UINT Count, Capacity, K=0, J=0; float Dt=0; UINT Reference=0; float Wave=0; UINT Padding=0; };
struct Variant { UINT Tile; ComPtr<ID3D12PipelineState> Presort, Tail; };

class Fixture {
public:
    GDX12Device Device;
    GDX12CommandQueue* Queue = nullptr;
    std::unique_ptr<GDX12DescriptorHeap> Heap;
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Save, Hash, Sort, Offsets, Density, Integrate;
    std::array<Variant,4> Variants{{{0},{512},{1024},{2048}}};
    ComPtr<ID3D12PipelineState> BaselineDensity, BaselineIntegrate, SpatialDensity, SpatialIntegrate;
    void Spatial(bool enabled) { Density=enabled ? SpatialDensity : BaselineDensity; Integrate=enabled ? SpatialIntegrate : BaselineIntegrate; }
    ComPtr<ID3D12QueryHeap> Queries;
    GDX12ResourceHandle TimeReadback;
    UINT64 Frequency = 0;

    Fixture(UINT adapter, bool debug, bool direct=false) {
        if (debug) {
            ComPtr<ID3D12Debug> layer;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
            layer->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(layer.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter4> selected;
        ThrowIfFailed(factory->EnumAdapterByGpuPreference(adapter, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&selected)));
        ThrowIfFailed(Device.Initialize(selected));
        Queue = direct ? Device.GetCommandQueue() : Device.TryGetComputeQueue();
        Require(Queue != nullptr, "Requested queue unavailable");
        Heap = std::make_unique<GDX12DescriptorHeap>(&Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        GDX12RootSignatureDesc desc;
        desc.VolatileSRVData=true; desc.Constants={8}; desc.NumSingleCBVSlots=1; desc.NumSingleSRVSlots=4; desc.NumSingleUAVSlots=4;
        Root = std::make_unique<GDX12RootSignature>(&Device, desc);
        const auto compile = [&](const char* entry, const D3D_SHADER_MACRO* defines=nullptr) {
            return GDX12RenderHelpers::CreateComputePipeline(&Device, Root.get(), LR"(I:/PEPEngine2/Build/SphSolverPerf/FluidSolver.hlsl)", entry, defines);
        };
        Save=compile("SavePrevious"); Hash=compile("Hash"); Sort=compile("Sort"); Offsets=compile("BuildOffsets"); Density=compile("Densities"); Integrate=compile("Integrate");
        BaselineDensity=Density; BaselineIntegrate=Integrate;
        SpatialDensity=GDX12RenderHelpers::CreateComputePipeline(&Device,Root.get(),LR"(I:/PEPEngine2/Build/SphSolverPerf/FluidSolverSpatial.hlsl)","Densities");
        SpatialIntegrate=GDX12RenderHelpers::CreateComputePipeline(&Device,Root.get(),LR"(I:/PEPEngine2/Build/SphSolverPerf/FluidSolverSpatial.hlsl)","Integrate");
        for (auto& v : Variants) if (v.Tile) {
            const auto size=std::to_string(v.Tile);
            const D3D_SHADER_MACRO defines[]={{"WATER_SORT_TILE_SIZE",size.c_str()},{nullptr,nullptr}};
            v.Presort=compile("PresortCS",defines); v.Tail=compile("MergeTailCS",defines);
        }
        D3D12_QUERY_HEAP_DESC q{}; q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count=64;
        ThrowIfFailed(Device.GetDevice()->CreateQueryHeap(&q,IID_PPV_ARGS(&Queries)));
        TimeReadback=Device.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(512),D3D12_RESOURCE_STATE_COPY_DEST);
        ThrowIfFailed(Queue->GetCommandQueue()->GetTimestampFrequency(&Frequency));
        std::cout << "Adapter: " << Device.GetDeviceFeatures().Name << '\n';
    }
    ~Fixture() { if (Queue) Queue->Flush(); }
    GDX12CommandList* Begin() {
        auto* c=Queue->GetCommandList(); c->SetDescriptorHeaps({Heap.get()}); c->SetComputeRootSignature(Root.get()); return c;
    }
    void Submit(GDX12CommandList* c) { Queue->ExecuteCommandList(c); Queue->Flush(); }
    static void Constants(GDX12CommandList& c, const Dispatch& d) { c.GetCommandList()->SetComputeRoot32BitConstants(0,8,&d,0); }
    template<class T> void Upload(GDX12UploadBuffer<T>& dst, const std::vector<T>& values) {
        GDX12UploadBuffer<T> source(&Device,static_cast<UINT>(values.size()),EBufferType::Upload,false);
        source.CopyData(0,values.data(),static_cast<UINT>(values.size()));
        auto* c=Begin(); c->ResourceBarrier({dst.GetResource().GetCopyDestBarrier()});
        c->GetCommandList()->CopyResource(dst.GetResource().D3DResource.Get(),source.GetResource().D3DResource.Get()); Submit(c);
    }
    template<class T> std::vector<T> Read(GDX12UploadBuffer<T>& src) {
        auto& resource=src.GetResource(); const auto bytes=resource.D3DResource->GetDesc().Width;
        auto readback=Device.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(bytes),D3D12_RESOURCE_STATE_COPY_DEST);
        auto* c=Begin(); const auto prior=resource.GetCurrentState(); c->ResourceBarrier({resource.GetCopySourceBarrier()});
        c->GetCommandList()->CopyResource(readback.Get(),resource.D3DResource.Get()); c->ResourceBarrier({resource.GetTransitionBarrier(prior)}); Submit(c);
        void* data=nullptr; ThrowIfFailed(readback->Map(0,nullptr,&data));
        std::vector<T> result(static_cast<size_t>(bytes)/sizeof(T)); std::memcpy(result.data(),data,static_cast<size_t>(bytes)); readback->Unmap(0,nullptr); return result;
    }
    void RecordSort(GDX12CommandList& c, Buffers& b, Dispatch d, const Variant& v) {
        const auto dispatch=[&](ComPtr<ID3D12PipelineState> pso, UINT groups) {
            Constants(c,d); c.SetPipelineState(pso); c.Dispatch(groups,1,1); c.ResourceBarrier({b.Keys->GetResource().GetUAVBarrier()});
        };
        if (!v.Tile) {
            for (UINT k=2;k<=b.Capacity;k<<=1) for (UINT j=k/2;j;j>>=1) { d.K=k;d.J=j;dispatch(Sort,(b.Capacity+127)/128); }
        } else {
            dispatch(v.Presort,(b.Capacity+v.Tile-1)/v.Tile);
            for (UINT k=v.Tile*2;k<=b.Capacity;k<<=1) {
                d.K=k;
                for (UINT j=k/2;j>=v.Tile;j>>=1) { d.J=j;dispatch(Sort,(b.Capacity+127)/128); }
                dispatch(v.Tail,(b.Capacity+v.Tile-1)/v.Tile);
            }
        }
    }
    double SortKeys(Buffers& b, const Variant& v, bool buildOffsets) {
        auto* c=Begin(); Dispatch d{b.Count,b.Capacity}; Constants(*c,d);
        c->ResourceBarrier({b.Keys->GetResource().GetUnorderedAccessBarrier(),b.Offsets->GetResource().GetUnorderedAccessBarrier()});
        c->SetComputeUAV(2,b.Keys->GetUAV()->GPUHandle); c->SetComputeUAV(3,b.Offsets->GetUAV()->GPUHandle);
        c->GetCommandList()->EndQuery(Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        RecordSort(*c,b,d,v);
        c->GetCommandList()->EndQuery(Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        c->GetCommandList()->ResolveQueryData(Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,TimeReadback.Get(),0);
        if (buildOffsets) {
            c->ResourceBarrier({b.Keys->GetResource().GetNonPixelShaderResourceBarrier()});
            c->SetComputeSRV(2,b.Keys->GetSRV()->GPUHandle); c->SetPipelineState(Offsets); Constants(*c,d); c->Dispatch((b.Count+127)/128,1,1);
            c->ResourceBarrier({b.Offsets->GetResource().GetUAVBarrier()});
        }
        Submit(c); UINT64* ticks=nullptr; ThrowIfFailed(TimeReadback->Map(0,nullptr,reinterpret_cast<void**>(&ticks)));
        const double ms=static_cast<double>(ticks[1]-ticks[0])*1000.0/static_cast<double>(Frequency); TimeReadback->Unmap(0,nullptr); return ms;
    }
    void DebugErrors() {
        ComPtr<ID3D12InfoQueue> info; if (FAILED(Device.GetDevice().As(&info))) return;
        for (UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T bytes=0; ThrowIfFailed(info->GetMessage(i,nullptr,&bytes)); std::vector<char> storage(bytes);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); ThrowIfFailed(info->GetMessage(i,m,&bytes));
            if (m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(m->pDescription);
        }
        info->ClearStoredMessages();
    }
};

GDX12FluidInstance Instance(UINT count, bool reference=false) {
    GDX12FluidInstance result; GDX12FluidSettings s;
    s.MGPUReference=reference; s.MaxSubsteps=3; s.SmoothingRadiusCm=.2f; s.ParticleRadiusCm=.01f;
    s.ParticleMassKg=.001f; s.RestDensityKgPerCm3=1; s.PressureStiffnessCm2PerSecond2=10;
    s.NearPressureStiffnessCm2PerSecond2=reference ? 2.15f : 0; s.PredictionSeconds=1.f/120;
    s.AccelerationCmPerSecond2=Vector3(0,-1,0); s.ViscosityCm2PerSecond=.01f;
    result.Component.SetSettings(s);
    std::vector<GDX12FluidParticle> seed(count);
    for (UINT i=0;i<count;++i) {
        seed[i].Position=Vector3((static_cast<int>(i%41)-20)*.11f,(static_cast<int>((i/41)%37)-18)*.11f,(static_cast<int>(i/(41*37))-17)*.11f);
        seed[i].Velocity=Vector3(static_cast<float>(i%3)*.01f,0,static_cast<float>(i%7)*-.01f);
    }
    result.Component.SetParticles(std::move(seed)); return result;
}

template<class T> void Exact(const std::vector<T>& a,const std::vector<T>& b,const char* what) {
    Require(a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(T))==0,what);
}

void ValidateSort(Fixture& f) {
    std::mt19937 rng(51975);
    for (UINT count : {1u,2u,3u,511u,512u,513u,1023u,1024u,1025u,2047u,2048u,2049u,4095u,4096u,4097u,51975u,1048576u}) {
        auto b=GDX12FluidSolver::CreateBuffers(&f.Device,f.Heap.get(),Instance(count));
        for (UINT mode=0;mode<(count>51975 ? 1u : 4u);++mode) {
            std::vector<XMUINT2> input(b.Capacity,XMUINT2(UINT_MAX,UINT_MAX));
            for (UINT i=0;i<count;++i) input[i]=XMUINT2(mode==0 ? rng()%b.Capacity : mode==1 ? 0 : mode==2 ? b.Capacity-1 : count-i-1,i);
            auto expected=input; std::sort(expected.begin(),expected.end(),[](auto a,auto c){return std::tie(a.x,a.y)<std::tie(c.x,c.y);});
            std::vector<UINT> offsets(b.Capacity,count);
            for (UINT i=0;i<count;++i) if (i==0 || expected[i-1].x!=expected[i].x) offsets[expected[i].x]=i;
            for (const auto& v:f.Variants) {
                f.Upload(*b.Keys,input); f.Upload(*b.Offsets,std::vector<UINT>(b.Capacity,count)); f.SortKeys(b,v,true);
                Exact(f.Read(*b.Keys),expected,"GPU keys differ from lexicographic oracle");
                Exact(f.Read(*b.Offsets),offsets,"GPU bucket offsets differ from oracle"); f.DebugErrors();
            }
        }
        std::cout << "Sort oracle passed Count=" << count << '\n';
    }
}

GDX12FluidSolver::Head RecordManual(Fixture& f, GDX12CommandList& c, Buffers& b,
    GDX12FluidSolver::Head head, const GDX12FluidSettings& settings, GDX12FluidSolver::Steps steps,
    D3D12_GPU_VIRTUAL_ADDRESS constants, const Variant& variant, bool stages=false) {
    const auto stamp=[&](UINT n) { c.GetCommandList()->EndQuery(f.Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,n); };
    stamp(0);
    auto& current=b.Particles[head.Current]->GetResource();
    if (head.SeedPending) {
        c.ResourceBarrier({current.GetCopyDestBarrier()});
        c.GetCommandList()->CopyResource(current.D3DResource.Get(),b.SeedUpload->GetResource().D3DResource.Get()); head.SeedPending=false;
    }
    c.SetComputeRootSignature(f.Root.get()); c.SetComputeRootConstantBufferView(1,constants);
    Dispatch d{b.Count,b.Capacity}; d.Reference=settings.MGPUReference ? 1u : 0u;
    Fixture::Constants(c,d); c.ResourceBarrier({current.GetUnorderedAccessBarrier()});
    c.SetComputeUAV(0,b.Particles[head.Current]->GetUAV()->GPUHandle); c.SetPipelineState(f.Save); c.Dispatch((b.Count+127)/128,1,1); c.ResourceBarrier({current.GetUAVBarrier()});
    if (stages) stamp(1);
    d.Dt=steps.Dt;
    for (UINT step=0;step<steps.Count;++step) {
        if (settings.WaveAcceleration>0) {
            d.Wave=settings.WaveAcceleration*std::cos(XM_2PI*(head.WaveTime+d.Dt*.5f)/settings.WavePeriodSeconds);
            head.WaveTime=std::fmod(head.WaveTime+d.Dt,settings.WavePeriodSeconds);
        }
        auto& input=b.Particles[head.Current]->GetResource(); auto& output=b.Particles[1-head.Current]->GetResource();
        c.ResourceBarrier({input.GetNonPixelShaderResourceBarrier(),b.Keys->GetResource().GetUnorderedAccessBarrier(),b.Offsets->GetResource().GetUnorderedAccessBarrier()});
        c.SetComputeSRV(0,b.Particles[head.Current]->GetSRV()->GPUHandle); c.SetComputeUAV(2,b.Keys->GetUAV()->GPUHandle); c.SetComputeUAV(3,b.Offsets->GetUAV()->GPUHandle);
        d.K=d.J=0; Fixture::Constants(c,d); c.SetPipelineState(f.Hash); c.Dispatch((b.Capacity+127)/128,1,1);
        c.ResourceBarrier({b.Keys->GetResource().GetUAVBarrier(),b.Offsets->GetResource().GetUAVBarrier()});
        if (stages) stamp(2+step*5);
        f.RecordSort(c,b,d,variant);
        if (stages) stamp(3+step*5);
        c.ResourceBarrier({b.Keys->GetResource().GetNonPixelShaderResourceBarrier()}); c.SetComputeSRV(2,b.Keys->GetSRV()->GPUHandle);
        Fixture::Constants(c,d); c.SetPipelineState(f.Offsets); c.Dispatch((b.Count+127)/128,1,1);
        if (stages) stamp(4+step*5);
        c.ResourceBarrier({b.Offsets->GetResource().GetNonPixelShaderResourceBarrier(),b.Density->GetResource().GetUnorderedAccessBarrier()});
        c.SetComputeSRV(3,b.Offsets->GetSRV()->GPUHandle); c.SetComputeUAV(1,b.Density->GetUAV()->GPUHandle); c.SetPipelineState(f.Density); c.Dispatch((b.Count+127)/128,1,1);
        if (stages) stamp(5+step*5);
        c.ResourceBarrier({b.Density->GetResource().GetNonPixelShaderResourceBarrier(),output.GetUnorderedAccessBarrier()});
        c.SetComputeSRV(1,b.Density->GetSRV()->GPUHandle); c.SetComputeUAV(0,b.Particles[1-head.Current]->GetUAV()->GPUHandle);
        c.SetPipelineState(f.Integrate); c.Dispatch((b.Count+127)/128,1,1); c.ResourceBarrier({output.GetUAVBarrier()}); head.Current=1-head.Current;
        if (stages) stamp(6+step*5);
    }
    if (!stages) stamp(1);
    c.GetCommandList()->ResolveQueryData(f.Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,stages ? 2+steps.Count*5 : 2,f.TimeReadback.Get(),0);
    return head;
}

struct Snapshot {
    std::vector<Particle> A,B;
    std::vector<XMFLOAT2> Density;
    std::vector<XMUINT2> Keys;
    std::vector<UINT> Offsets;
};
void ValidateSimulation(Fixture& f) {
    auto* compute=f.Queue;
    struct RestoreQueue {
        Fixture& Target;
        GDX12CommandQueue* Original;
        ~RestoreQueue() { Target.Queue=Original; }
    } restore{f,compute};
    GDX12FluidSolver production; production.Initialize(&f.Device);
    for (UINT count : {3u,1023u,1025u,2049u,51975u}) for (bool reference : {false,true}) {
        auto instance=Instance(count,reference);
        auto seed=*instance.Component.GetParticles();
        seed[1].Position=seed[0].Position; seed[2].Position=seed[0].Position;
        instance.Component.SetParticles(std::move(seed));
        const auto& settings=instance.Component.GetSettings();
        GDX12UploadBuffer<GDX12FluidParameters> constants(&f.Device,1);
        constants.CopyData(0,MakeFluidParameters(instance.ContainerToWorld,settings));
        std::vector<Snapshot> expected;
        // Baseline, all tiles, production tracked/placed, then a completed DIRECT/COMPUTE/DIRECT sequence.
        for (UINT route=0;route<(count==2049 ? 7u : 6u);++route) {
            f.Spatial(route==1);
            auto b=GDX12FluidSolver::CreateBuffers(&f.Device,f.Heap.get(),instance); GDX12FluidSolver::Head head;
            UINT totalSteps=0;
            for (UINT frame=0;frame<3;++frame) {
                f.Queue->Flush();
                f.Queue=route==6 && frame!=1 ? f.Device.GetCommandQueue() : compute;
                const GDX12FluidSolver::Steps steps{frame==0 ? 1u : 3u,.0001f};
                totalSteps+=steps.Count;
                auto* c=f.Begin();
                if (route<4) head=RecordManual(f,*c,b,head,settings,steps,constants.GetElementAddress(0),f.Variants[2]);
                else {
                    const bool placed=route==5;
                    GDX12Resource* resources[]={&b.Particles[0]->GetResource(),&b.Particles[1]->GetResource(),&b.Density->GetResource(),&b.Keys->GetResource(),&b.Offsets->GetResource()};
                    if (placed) for (auto* resource:resources) c->ResourceBarrier({resource->GetUnorderedAccessBarrier()});
                    head=production.Record(*c,b,head,settings,steps,constants.GetElementAddress(0),placed ? GDX12FluidSolver::Boundary::PlacedUav : GDX12FluidSolver::Boundary::LegacyTracked);
                    // Placed recording intentionally does not mutate the legacy state tracker.
                    if (placed) for (auto* resource:resources) resource->SetCurrentState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                f.Submit(c);
                Snapshot sample{f.Read(*b.Particles[0]),f.Read(*b.Particles[1]),f.Read(*b.Density),f.Read(*b.Keys),f.Read(*b.Offsets)};
                for (const auto* particles : {&sample.A,&sample.B})
                    for (const auto& p:*particles) Require(std::isfinite(p.Position.x)&&std::isfinite(p.Position.y)&&std::isfinite(p.Position.z)&&std::isfinite(p.Velocity.x)&&std::isfinite(p.Velocity.y)&&std::isfinite(p.Velocity.z),"Nonfinite simulation");
                for (const auto& density:sample.Density) Require(std::isfinite(density.x)&&std::isfinite(density.y)&&density.x>0&&density.y>0,"Invalid simulation density");
                Require((head.Current ? sample.B : sample.A).front().Velocity.y!=instance.Component.GetParticles()->front().Velocity.y,"Simulation failed to advance");
                if (route==0) expected.push_back(sample);
                else {
                    Exact(sample.A,expected[frame].A,"Particle A differs from baseline"); Exact(sample.B,expected[frame].B,"Particle B differs from baseline");
                    Exact(sample.Density,expected[frame].Density,"Density differs from baseline");
                    Exact(sample.Keys,expected[frame].Keys,"Simulation keys differ from baseline");
                    Exact(sample.Offsets,expected[frame].Offsets,"Simulation offsets differ from baseline");
                }
                Require(head.Current==(totalSteps%2),"Wrong ping-pong head"); f.DebugErrors();
                f.Queue->Flush();
            }
        }
        std::cout << "Exact simulation passed Count=" << count << " Reference=" << reference
                  << " (1+3+3 substeps, all tiles, production tracked/placed"
                  << (count==2049 ? ", DIRECT/COMPUTE/DIRECT)\n" : ")\n");
    }
}

void Benchmark(Fixture& f, const std::string& output, UINT rounds) {
    std::ofstream json(output); Require(bool(json),"Cannot open benchmark output");
    json << std::setprecision(10) << "{\"adapter\":" << std::quoted(f.Device.GetDeviceFeatures().Name)
         << ",\"queue\":" << std::quoted(f.Queue->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : "compute")
         << ",\"debug_layer\":false,\"timestamp_frequency\":" << f.Frequency
         << ",\"scope\":\"sort dispatches and UAV barriers; upload and offsets excluded\",\"samples\":[\n";
    bool first=true; std::mt19937 rng(87213);
    for (UINT series=0;series<4;++series) {
        const UINT count=std::array<UINT,4>{4097u,51975u,1048576u,51975u}[series];
        const char* dataset=series==3 ? "fluid_hash" : "random";
        const auto instance=Instance(count);
        auto b=GDX12FluidSolver::CreateBuffers(&f.Device,f.Heap.get(),instance);
        std::vector<XMUINT2> input(b.Capacity,XMUINT2(UINT_MAX,UINT_MAX));
        if (series==3) {
            GDX12UploadBuffer<GDX12FluidParameters> constants(&f.Device,1);
            constants.CopyData(0,MakeFluidParameters(instance.ContainerToWorld,instance.Component.GetSettings()));
            auto* c=f.Begin(); auto& particles=b.Particles[0]->GetResource();
            c->ResourceBarrier({particles.GetCopyDestBarrier()});
            c->GetCommandList()->CopyResource(particles.D3DResource.Get(),b.SeedUpload->GetResource().D3DResource.Get());
            c->ResourceBarrier({particles.GetNonPixelShaderResourceBarrier(),b.Keys->GetResource().GetUnorderedAccessBarrier(),b.Offsets->GetResource().GetUnorderedAccessBarrier()});
            c->SetComputeRootConstantBufferView(1,constants.GetElementAddress(0));
            c->SetComputeSRV(0,b.Particles[0]->GetSRV()->GPUHandle);
            c->SetComputeUAV(2,b.Keys->GetUAV()->GPUHandle); c->SetComputeUAV(3,b.Offsets->GetUAV()->GPUHandle);
            Dispatch d{b.Count,b.Capacity}; d.Dt=.0001f; Fixture::Constants(*c,d);
            c->SetPipelineState(f.Hash); c->Dispatch((b.Capacity+127)/128,1,1);
            c->ResourceBarrier({b.Keys->GetResource().GetUAVBarrier(),b.Offsets->GetResource().GetUAVBarrier()}); f.Submit(c);
            input=f.Read(*b.Keys);
        } else for (UINT i=0;i<count;++i) input[i]=XMUINT2(rng()%b.Capacity,i);
        // At least one second of measured GPU sorting, shared across all variants.
        double warmupMs=0;
        while (warmupMs<1000) for (const auto& v:f.Variants) { f.Upload(*b.Keys,input); warmupMs+=f.SortKeys(b,v,false); }
        for (UINT round=0;round<rounds;++round) {
            std::array<UINT,4> order{0,1,2,3}; std::shuffle(order.begin(),order.end(),rng);
            for (UINT position=0;position<4;++position) {
                const auto& v=f.Variants[order[position]]; f.Upload(*b.Keys,input); const double ms=f.SortKeys(b,v,false);
                if (!first) json << ",\n"; first=false;
                json << "{\"dataset\":" << std::quoted(dataset) << ",\"count\":" << count << ",\"capacity\":" << b.Capacity << ",\"round\":" << round
                     << ",\"order\":" << position << ",\"tile\":" << v.Tile << ",\"ms\":" << ms << '}';
            }
            json.flush();
        }
        std::cout << "Benchmark completed dataset=" << dataset << " Count=" << count << " rounds=" << rounds << '\n';
    }
    json << "\n]}\n"; Require(bool(json),"Benchmark JSON write failed");
}
}

int SortFixtureMain(int argc,char** argv) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);
    try {
        bool validate=false,benchmark=false,direct=false; UINT adapter=0,rounds=31; std::string output="WaterSortBenchmark.json";
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--validate") validate=true;
            else if (arg=="--benchmark") benchmark=true;
            else if (arg=="--direct") direct=true;
            else if (arg=="--adapter" && i+1<argc) adapter=static_cast<UINT>(std::stoul(argv[++i]));
            else if (arg=="--rounds" && i+1<argc) rounds=static_cast<UINT>(std::stoul(argv[++i]));
            else if (arg=="--output" && i+1<argc) output=argv[++i];
            else throw std::invalid_argument("Usage: --validate | --benchmark [--direct] [--adapter N] [--rounds N>=31] [--output file.json]");
        }
        Require(validate!=benchmark,"Choose exactly one of --validate and --benchmark"); Require(rounds>=31 && rounds<=1000,"Rounds must be 31..1000");
        Require(!direct || benchmark,"--direct is available only with --benchmark");
        Fixture fixture(adapter,validate,direct);
        if (validate) { ValidateSort(fixture); ValidateSimulation(fixture); fixture.DebugErrors(); std::cout << "All GPU checks passed\n"; }
        else Benchmark(fixture,output,rounds);
        return 0;
    } catch (const DxException& e) { std::wcerr << e.ToString() << '\n'; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 1;
}

namespace {
Snapshot ReadSnapshot(Fixture& f, Buffers& b) {
    return {f.Read(*b.Particles[0]),f.Read(*b.Particles[1]),f.Read(*b.Density),f.Read(*b.Keys),f.Read(*b.Offsets)};
}
void Compare(const Snapshot& a,const Snapshot& b) {
    Exact(a.A,b.A,"Particle A differs"); Exact(a.B,b.B,"Particle B differs");
    Exact(a.Density,b.Density,"Density differs"); Exact(a.Keys,b.Keys,"Keys differ"); Exact(a.Offsets,b.Offsets,"Offsets differ");
}
void SolverBenchmark(Fixture& f,const std::string& output, bool stages, UINT rounds, UINT warmup) {
    GDX12FluidInstance instance; instance.Component=BistroWater::Scene::CreateFluidSource();
    const auto settings=instance.Component.GetSettings();
    Require(instance.Component.GetParticles()->size()==100980,"Unexpected Bistro workload");
    const auto steps=GDX12FluidSolver::PlanSteps(settings,1.f/60);
    Require(steps.Count==3,"Bistro substep count changed");
    GDX12UploadBuffer<GDX12FluidParameters> constants(&f.Device,1);
    constants.CopyData(0,MakeFluidParameters(instance.ContainerToWorld,settings));
    auto a=GDX12FluidSolver::CreateBuffers(&f.Device,f.Heap.get(),instance);
    auto b=GDX12FluidSolver::CreateBuffers(&f.Device,f.Heap.get(),instance);
    Buffers* buffers[]={&a,&b}; GDX12FluidSolver::Head heads[2];
    std::ofstream json(output); Require(bool(json),"Cannot open output");
    json << std::setprecision(10) << "{\"adapter\":" << std::quoted(f.Device.GetDeviceFeatures().Name)
         << ",\"queue\":" << std::quoted(f.Queue->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : "compute")
         << ",\"stages\":" << (stages ? "true" : "false") << ",\"count\":" << a.Count << ",\"capacity\":" << a.Capacity
         << ",\"substeps\":" << steps.Count << ",\"dt\":" << steps.Dt << ",\"warmup\":" << warmup
         << ",\"stage_names\":[\"save\",\"hash\",\"sort\",\"offsets\",\"densities\",\"integrate\"],\"samples\":[\n";
    bool first=true;
    for (UINT frame=0;frame<warmup+rounds;++frame) {
        for (UINT order=0;order<2;++order) {
            const UINT variant=(frame+order)%2; f.Spatial(variant!=0); auto* c=f.Begin();
            heads[variant]=RecordManual(f,*c,*buffers[variant],heads[variant],settings,steps,constants.GetElementAddress(0),f.Variants[2],stages);
            f.Submit(c);
            if (frame<warmup) continue;
            UINT64* ticks=nullptr; ThrowIfFailed(f.TimeReadback->Map(0,nullptr,reinterpret_cast<void**>(&ticks)));
            const auto delta=[&](UINT l,UINT r) { return double(ticks[r]-ticks[l])*1000/double(f.Frequency); };
            if (!first) json << ",\n"; first=false;
            json << "{\"frame\":" << frame << ",\"order\":" << order << ",\"variant\":" << variant
                 << ",\"ms\":" << delta(0,stages ? 1+steps.Count*5 : 1);
            if (stages) {
                double sums[6]={delta(0,1)};
                for (UINT step=0;step<steps.Count;++step) for (UINT stage=0;stage<5;++stage) sums[stage+1]+=delta(1+step*5+stage,2+step*5+stage);
                json << ",\"stage_ms\":[";
                for (UINT s=0;s<6;++s) { if(s) json << ','; json << sums[s]; } json << ']';
            }
            json << '}'; f.TimeReadback->Unmap(0,nullptr); json.flush();
        }
        if (frame==warmup-1 || frame==warmup+rounds-1) {
            Compare(ReadSnapshot(f,a),ReadSnapshot(f,b));
            Require(heads[0].Current==heads[1].Current && heads[0].WaveTime==heads[1].WaveTime,"Head mismatch");
            std::cout << "Exact Bistro state after " << frame+1 << " frames\n";
        }
    }
    json << "\n]}\n"; Require(bool(json),"Write failed");
}
}
int main(int argc,char** argv) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);
    try {
        bool validate=false,direct=false,stages=false; UINT rounds=31,warmup=120; std::string output="solver.json";
        for (int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--validate") validate=true;
            else if(arg=="--direct") direct=true;
            else if(arg=="--stages") stages=true;
            else if(arg=="--rounds" && i+1<argc) rounds=std::stoul(argv[++i]);
            else if(arg=="--warmup" && i+1<argc) warmup=std::stoul(argv[++i]);
            else if(arg=="--output" && i+1<argc) output=argv[++i];
            else throw std::invalid_argument("Unknown option");
        }
        Require(rounds>=3 && rounds<=1000 && warmup>=1 && warmup<=1000,"Invalid run length");
        Fixture f(0,validate,direct);
        if (validate) { ValidateSort(f); ValidateSimulation(f); f.DebugErrors(); }
        else SolverBenchmark(f,output,stages,rounds,warmup);
        return 0;
    } catch(const DxException& e) { std::wcerr << e.ToString() << '\n'; }
      catch(const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 1;
}
