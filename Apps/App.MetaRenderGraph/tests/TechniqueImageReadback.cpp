#include "Graph/MetaGraphExecutor.h"
#include "Techniques/NativeWork.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <array>
#include <cmath>
#include <cstring>

namespace
{
std::vector<uint8_t> Bytes(const std::string& path)
{
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input) throw std::runtime_error("Missing technique evidence: "+path);
    const auto size=input.tellg();
    if(size<0) throw std::runtime_error("Invalid technique evidence size");
    std::vector<uint8_t> result(static_cast<size_t>(size)); input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()),result.size());
    if(!input) throw std::runtime_error("Incomplete technique evidence");
    return result;
}
uint32_t Word(const std::vector<uint8_t>& bytes,size_t index)
{
    if(index>=bytes.size()/4) throw std::runtime_error("Particle evidence index out of range");
    uint32_t value; std::memcpy(&value,bytes.data()+index*4,4); return value;
}
using DrawRecord=std::array<uint8_t,96>;
DrawRecord Draw(const std::vector<uint8_t>& bytes,uint32_t index)
{
    if(index>=bytes.size()/96) throw std::runtime_error("Particle draw index out of range");
    DrawRecord record; std::memcpy(record.data(),bytes.data()+index*96,96); return record;
}
float Field(const DrawRecord& record,size_t index)
{
    float value;std::memcpy(&value,record.data()+index*4,4);
    if(!std::isfinite(value)) throw std::runtime_error("Non-finite particle draw field");
    return value;
}
double Half(const uint8_t* bytes)
{
    uint16_t value; std::memcpy(&value,bytes,2);
    const unsigned exponent=(value>>10)&31, fraction=value&1023;
    if(exponent==31) throw std::runtime_error("Non-finite technique pixel");
    const double result=exponent?std::ldexp(1.+fraction/1024.,int(exponent)-15):std::ldexp(double(fraction),-24);
    return value&32768?-result:result;
}
void SumImages(const std::string& reference,const std::string& candidate,uint32_t count,bool half,
    std::ofstream& statistics)
{
    const auto a=Bytes(reference),b=Bytes(candidate);
    if(a.size()!=b.size() || a.size()%(half?2:16)) throw std::runtime_error("Incompatible image evidence");
    // A full ULP per blend is conservative for both FP16 and FP32. For nonnegative
    // sums, gamma_n bounds each ordering; account for both errors and underflow.
    const double unit=std::ldexp(1.,half?-10:-23),nu=count*unit;
    if(nu>=.5) throw std::runtime_error("Accumulation oracle needs a higher precision reference");
    const double gamma=nu/(1-nu),relative=2*gamma/(1-gamma);
    const double absolute=2*count*std::ldexp(1.,half?-14:-126);
    double maximum=0,maximumFraction=0; size_t changed=0;
    for(size_t offset=0;offset<a.size();offset+=half?2:4) {
        double x,y;
        if(half) {x=Half(a.data()+offset);y=Half(b.data()+offset);}
        else {float p,q;std::memcpy(&p,a.data()+offset,4);std::memcpy(&q,b.data()+offset,4);x=p;y=q;}
        if(!std::isfinite(x)||!std::isfinite(y)) throw std::runtime_error("Non-finite accumulated pixel");
        // In this static-camera fixture both projected sprite positions are in
        // [-2,2]. Thus sum(abs(contributions)) <= 4*n even with cancellation.
        if(!half && offset%16<8) {
            const double delta=std::abs(x-y),bound=8*gamma*count+absolute;
            if(delta>bound) throw std::runtime_error("Signed particle motion exceeds absolute-sum error bound");
            maximum=(std::max)(maximum,delta);
            if(bound>0) maximumFraction=(std::max)(maximumFraction,delta/bound);
            changed+=x!=y;
            continue;
        }
        if(x<0 || y<0) throw std::runtime_error("Signed input outside nonnegative sum oracle");
        const double delta=std::abs(x-y),bound=relative*(std::max)(x,y)+absolute;
        if(delta>bound) throw std::runtime_error("Particle accumulation exceeds format-derived error bound");
        maximum=(std::max)(maximum,delta);
        if(bound>0) maximumFraction=(std::max)(maximumFraction,delta/bound);
        changed+=x!=y;
    }
    statistics<<candidate<<','<<count<<','<<changed<<','<<maximum<<','<<maximumFraction<<'\n';
}
}

void CheckWaterMotionEvidence(const std::string&,const Matrix&,const Matrix&,unsigned,bool);
void CheckWaterParticleEvidence(const std::string& stem,const Matrix& projection,const Matrix& view,unsigned frame)
{
    const auto a=stem+"-serial-particles-",b=stem+"-parallel-particles-";
    const auto counts=Bytes(a+"render-buffer-0.bin");
    if(counts!=Bytes(b+"render-buffer-0.bin")) throw std::runtime_error("Particle counts differ");
    const auto drawsA=Bytes(a+"render-buffer-10.bin"),drawsB=Bytes(b+"render-buffer-10.bin");
    if(drawsA.size()!=drawsB.size() || drawsA.size()%96) throw std::runtime_error("Invalid particle draw grid");
    const auto capacity=drawsA.size()/96;
    std::vector<DrawRecord> allA,allB;
    for(uint32_t i=0;i<Word(counts,0);++i) {allA.push_back(Draw(drawsA,i));allB.push_back(Draw(drawsB,i));}
    std::sort(allA.begin(),allA.end());std::sort(allB.begin(),allB.end());
    if(allA!=allB) throw std::runtime_error("Canonical particle draws differ");
    for(const auto& record:allA) {
        uint32_t geometry;std::memcpy(&geometry,record.data()+92,4);
        if(geometry!=0) throw std::runtime_error("Signed-motion oracle requires billboard fixture geometry");
        for(size_t previous=0;previous<2;++previous) {
            const size_t position=previous?4:0;
            const auto center=Vector4::Transform(Vector4(Field(record,position),Field(record,position+1),Field(record,position+2),1),view*projection);
            const double radius=std::abs(Field(record,previous?19:3))*.707107;
            if(center.w<=0 || std::abs(center.x)+radius*projection._11>1.99*center.w ||
                std::abs(center.y)+radius*projection._22>1.99*center.w)
                throw std::runtime_error("Sprite outside signed-motion oracle projection bounds");
        }
    }
    const auto indicesA=Bytes(a+"render-buffer-1.bin"),indicesB=Bytes(b+"render-buffer-1.bin");
    const auto sortedA=Bytes(a+"render-buffer-6.bin"),sortedB=Bytes(b+"render-buffer-6.bin");
    for(size_t mode=0;mode<5;++mode) {
        std::vector<DrawRecord> left,right;
        for(uint32_t i=0;i<Word(counts,mode+1);++i) {
            left.push_back(Draw(drawsA,Word(mode==2?sortedA:indicesA,mode==2?i:(mode<3?mode:mode-1)*capacity+i)));
            right.push_back(Draw(drawsB,Word(mode==2?sortedB:indicesB,mode==2?i:(mode<3?mode:mode-1)*capacity+i)));
        }
        if(mode==1 || mode==4) {std::sort(left.begin(),left.end());std::sort(right.begin(),right.end());}
        if(left!=right) throw std::runtime_error("Particle blend membership or ordered alpha draws differ");
    }
    std::ifstream manifest(stem+"-serial-particles-buffers.txt");
    if(!manifest) throw std::runtime_error("Missing particle buffer manifest");
    std::string name;
    while(std::getline(manifest,name)) {
        if(name.rfind("emitter-",0)!=0) continue;
        auto left=Bytes(a+name+".bin"),right=Bytes(b+name+".bin");
        name+=".bin";
        const auto suffix=name.substr(name.size()-11);
        if(suffix=="-alive0.bin" || suffix=="-alive1.bin") {
            std::vector<uint32_t> idsA,idsB;
            for(size_t i=0;i<left.size()/4;++i) idsA.push_back(Word(left,i));
            for(size_t i=0;i<right.size()/4;++i) idsB.push_back(Word(right,i));
            std::sort(idsA.begin(),idsA.end());std::sort(idsB.begin(),idsB.end());
            if(idsA!=idsB) throw std::runtime_error("Particle live-index membership differs");
        } else if(left!=right) throw std::runtime_error("Exact particle simulation state differs: "+name);
    }
    std::ofstream statistics(stem+"-accumulation-error.csv");
    statistics<<"image,contributions,changed_values,max_absolute_error,max_fraction_of_bound\n";
    for(unsigned i=0;i<5;++i) {
        const auto left=stem+"-serial-water-image-"+std::to_string(i)+".bin";
        const auto right=stem+"-parallel-water-image-"+std::to_string(i)+".bin";
        if(i==2 || i==4) SumImages(left,right,Word(counts,2)+Word(counts,5),true,statistics);
        else if(Bytes(left)!=Bytes(right)) throw std::runtime_error("Exact water depth/motion/opaque depth differs");
        const auto stageA=stem+"-serial-water-stages-image-"+std::to_string(i)+".bin";
        const auto stageB=stem+"-parallel-water-stages-image-"+std::to_string(i)+".bin";
        if(i==4) SumImages(stageA,stageB,Word(counts,0),false,statistics);
        else if(Bytes(stageA)!=Bytes(stageB)) throw std::runtime_error("Exact far HDR/water optics/near WBOIT stage differs");
    }
    if(!statistics) throw std::runtime_error("Cannot save accumulation oracle evidence");
    CheckWaterMotionEvidence(stem,projection,view,frame,false);
}
void CheckWaterMotionEvidence(const std::string& stem,const Matrix& projection,const Matrix& view,unsigned frame,bool translation)
{
    const auto depth=Bytes(stem+"-parallel-water-image-0.bin"),opaque=Bytes(stem+"-parallel-water-image-3.bin");
    const auto motion=Bytes(stem+"-parallel-water-image-1.bin");
    if(depth.size()!=opaque.size() || depth.size()!=motion.size()) throw std::runtime_error("Water image grids differ");
    size_t covered=0,moving=0; double maximumError=0;
    for(size_t offset=0;offset<depth.size();offset+=4) {
        float z,sceneZ;std::memcpy(&z,depth.data()+offset,4);std::memcpy(&sceneZ,opaque.data()+offset,4);
        if(!std::isfinite(z) || !std::isfinite(sceneZ)) throw std::runtime_error("Non-finite water depth");
        if(z<=sceneZ) continue;
        ++covered;
        const double x=Half(motion.data()+offset),y=Half(motion.data()+offset+2);
        moving+=x!=0 || y!=0;
        if(frame==0 && (x!=0 || y!=0)) throw std::runtime_error("Initial water motion did not reset");
        if(translation) {
            const double distance=projection._43/(z+projection._33);
            const double expectedX=-8*view._11*projection._11/distance,expectedY=-8*view._12*projection._22/distance;
            const double error=(std::max)(std::abs(x-expectedX),std::abs(y-expectedY));
            maximumError=(std::max)(maximumError,error);
            // World reconstruction is FP32 around 1400 cm; motion is stored in FP16.
            if(error>2e-5+.002*(std::max)(std::abs(expectedX),std::abs(expectedY)))
                throw std::runtime_error("Presented water translation motion differs from camera projection oracle");
        }
    }
    if((frame==0 || frame==1 || translation) && covered<100) throw std::runtime_error("Water fixture has no meaningful visible surface");
    if(translation && moving!=covered) throw std::runtime_error("Moving water did not retain previous presented transform");
    std::ofstream waterStatistics(stem+"-water-motion.csv");
    waterStatistics<<"frame,water_pixels,moving_water_pixels,max_projection_error\n"<<frame<<','<<covered<<','<<moving<<','<<maximumError<<'\n';
    if(!waterStatistics) throw std::runtime_error("Cannot save water motion evidence");
}

void CheckFSRGpuEvidence(const std::string& stem,unsigned width,unsigned height)
{
    const auto source=Bytes(stem+"-parallel-water-image-4.bin");
    const auto focus=Bytes(stem+"-post-image-0.bin"),color=Bytes(stem+"-post-image-1.bin");
    const auto motion=Bytes(stem+"-post-image-2.bin"),reconstructed=Bytes(stem+"-post-image-3.bin");
    if(source.size()!=focus.size() || focus.size()!=color.size() || color.size()!=2*motion.size())
        throw std::runtime_error("FSR input color/motion grids disagree");
    if(source!=focus || focus!=color) throw std::runtime_error("Disabled post effects changed FSR input HDR");
    if(reconstructed.size()!=uint64_t(width)*height*8) throw std::runtime_error("FSR output is not the requested display grid");
    double inputPeak=0,outputPeak=0,outputMinimum=0;
    for(size_t i=0;i<source.size();i+=2) {
        const auto value=Half(source.data()+i);
        if(i%8<6) inputPeak=(std::max)(inputPeak,value);
    }
    for(size_t i=0;i<motion.size();i+=2) Half(motion.data()+i);
    for(size_t i=0;i<reconstructed.size();i+=2) {
        const auto value=Half(reconstructed.data()+i);
        if(i%8<6) {outputPeak=(std::max)(outputPeak,value);outputMinimum=(std::min)(outputMinimum,value);}
    }
    if(inputPeak<=1 || outputPeak<=1) throw std::runtime_error("FSR fixture lost its HDR signal or clipped to LDR");
    std::ofstream statistics(stem+"-fsr-numeric.csv");
    statistics<<"input_peak_rgb,output_peak_rgb,output_minimum_rgb,output_width,output_height,finite_output_values\n";
    statistics<<inputPeak<<','<<outputPeak<<','<<outputMinimum<<','<<width<<','<<height<<','<<reconstructed.size()/2<<'\n';
    if(!statistics) throw std::runtime_error("Cannot save FSR image metrics");
}

void CaptureTechniqueBuffers(GDX12Device& device,const Meta::Schedule& sourcePlan,
    const std::vector<ID3D12Resource*>& sourceBindings,
    const std::vector<std::pair<std::string,GDX12Resource*>>& buffers,const std::string& stem)
{
    Meta::Graph graph;
    std::vector<ID3D12Resource*> bindings;
    std::vector<GDX12ResourceHandle> readbacks;
    for(const auto& [name,wrapper]:buffers) {
        auto* native=wrapper->D3DResource.Get();
        const auto found=std::find(sourceBindings.begin(),sourceBindings.end(),native);
        if(found==sourceBindings.end()) throw std::logic_error("Buffer has no declared producer state");
        const auto id=Meta::NativeWork::ResourceFor(graph,bindings,native,name);
        graph.Resources[id].Initial=graph.Resources[id].Final=sourcePlan.FinalStates.at(found-sourceBindings.begin());
        readbacks.push_back(device.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(native->GetDesc().Width),D3D12_RESOURCE_STATE_COPY_DEST));
        const auto dst=Meta::NativeWork::ResourceFor(graph,bindings,readbacks.back().Get(),"buffer readback");
        const auto state=graph.Resources[id].Memory==Meta::Heap::Upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_SOURCE;
        graph.Operations.push_back({"capture particle state",{{id,Meta::Mode::Read,Meta::State(state)},
            {dst,Meta::Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}});
    }
    Meta::Executor executor(device,1,UINT((buffers.size()+1)*3)); executor.Begin(0);
    executor.Execute(Meta::Compile(graph),bindings,[&](Meta::Id op,uint32_t,ID3D12GraphicsCommandList* cmd) {
        cmd->CopyResource(readbacks.at(op).Get(),buffers.at(op).second->D3DResource.Get());
    },nullptr,nullptr,true);
    executor.Drain();
    std::ofstream manifest(stem+"-buffers.txt");
    for(size_t i=0;i<readbacks.size();++i) {
        void* mapped=nullptr; ThrowIfFailed(readbacks[i]->Map(0,nullptr,&mapped));
        std::ofstream raw(stem+"-"+buffers[i].first+".bin",std::ios::binary);
        raw.write(static_cast<const char*>(mapped),readbacks[i]->GetDesc().Width);
        readbacks[i]->Unmap(0,nullptr);
        if(!raw) throw std::runtime_error("Cannot save particle state");
        manifest<<buffers[i].first<<'\n';
    }
    if(!manifest) throw std::runtime_error("Cannot save particle buffer manifest");
}

std::vector<uint8_t> CaptureTechniqueImages(GDX12Device& device, const Meta::Schedule& sourcePlan,
    const std::vector<ID3D12Resource*>& sourceBindings, const std::vector<GDX12Texture*>& images,
    const std::string& stem)
{
    struct Copy { ID3D12Resource* Source; GDX12ResourceHandle Readback; D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint;
        UINT Rows; UINT64 RowBytes, Bytes; };
    std::vector<Copy> copies;
    Meta::Graph graph;
    std::vector<ID3D12Resource*> bindings;
    for(auto* image:images) {
        auto* native=image->GetResource()->D3DResource.Get();
        const auto found=std::find(sourceBindings.begin(),sourceBindings.end(),native);
        if(found==sourceBindings.end()) throw std::logic_error("Image has no declared producer state");
        const auto state=sourcePlan.FinalStates.at(found-sourceBindings.begin());
        const auto id=Meta::NativeWork::ResourceFor(graph,bindings,native,"technique image");
        graph.Resources[id].Initial=graph.Resources[id].Final=state;
        Copy copy{}; copy.Source=native;
        const auto desc=native->GetDesc();
        device.GetDevice()->GetCopyableFootprints(&desc,0,1,0,&copy.Footprint,&copy.Rows,&copy.RowBytes,&copy.Bytes);
        copy.Readback=device.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(copy.Bytes),D3D12_RESOURCE_STATE_COPY_DEST);
        const auto dst=Meta::NativeWork::ResourceFor(graph,bindings,copy.Readback.Get(),"image readback");
        graph.Operations.push_back({"capture produced technique image",{{id,Meta::Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},
            {dst,Meta::Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}});
        copies.push_back(std::move(copy));
    }
    Meta::Executor executor(device,1,UINT((copies.size()+1)*3));
    executor.Begin(0);
    executor.Execute(Meta::Compile(graph),bindings,[&](Meta::Id op,uint32_t,ID3D12GraphicsCommandList* cmd) {
        const auto& copy=copies.at(op);
        const CD3DX12_TEXTURE_COPY_LOCATION dst(copy.Readback.Get(),copy.Footprint),src(copy.Source,0);
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    },nullptr,nullptr,true);
    executor.Drain();
    std::vector<uint8_t> result;
    std::ofstream metadata(stem+"-images.txt");
    for(size_t i=0;i<copies.size();++i) {
        const auto& copy=copies[i]; const auto desc=copy.Source->GetDesc();
        void* mapped=nullptr; ThrowIfFailed(copy.Readback->Map(0,nullptr,&mapped));
        std::ofstream raw(stem+"-image-"+std::to_string(i)+".bin",std::ios::binary);
        for(UINT y=0;y<copy.Rows;++y) {
            const auto* row=static_cast<const uint8_t*>(mapped)+copy.Footprint.Offset+y*copy.Footprint.Footprint.RowPitch;
            raw.write(reinterpret_cast<const char*>(row),copy.RowBytes);
            result.insert(result.end(),row,row+copy.RowBytes);
        }
        copy.Readback->Unmap(0,nullptr);
        if(!raw) throw std::runtime_error("Cannot save technique image");
        metadata<<i<<" width="<<desc.Width<<" height="<<desc.Height<<" format="<<desc.Format<<" row_bytes="<<copy.RowBytes<<'\n';
    }
    if(!metadata) throw std::runtime_error("Cannot save image metadata");
    return result;
}
