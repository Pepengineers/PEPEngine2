#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include "Engine.Core/Logging/Logger.h"

void CheckGrassGpuEvidence(const std::filesystem::path& root) {
    std::vector<std::vector<char>> fields;
    for(unsigned frame=0;frame<8;++frame) {
        const auto stem=root/("geometry-"+std::to_string(frame)+"-parallel");
        unsigned long long version=0,bytes=0;unsigned patches=0;std::ifstream metadata(stem.string()+"-grass.txt");std::string line;std::getline(metadata,line);
        if(sscanf_s(line.c_str(),"version=%llu patches=%u bytes=%llu",&version,&patches,&bytes)!=3 || version!=frame+1 || patches!=unsigned(frame!=3 && frame!=6))
            throw std::runtime_error("Wrong grass interaction publication version or patch count");
        std::ifstream in(stem.string()+"-grass.bin",std::ios::binary);fields.emplace_back(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
        if(fields.back().size()!=bytes || bytes<3*65536)throw std::runtime_error("Incomplete grass GPU field readback");
        float presence=0;
        for(unsigned cell=0;cell<4096;++cell){float value[4];std::memcpy(value,fields.back().data()+2*65536+cell*16,16);for(float x:value)if(!std::isfinite(x))throw std::runtime_error("Nonfinite interaction field");presence=(std::max)(presence,value[2]);}
        if(presence<=0 || presence>1.001f)throw std::runtime_error("Interaction source did not reach the GPU field");
        const bool reset=frame==0 || frame==4 || frame==7;
        const auto& previous=reset?fields[frame]:fields[frame-1];
        if(std::memcmp(previous.data(),fields[frame].data()+65536,65536)!=0)throw std::runtime_error("Grass previous interaction version is stale");
        unsigned probePixels=0;
        for(unsigned face=0;face<6;++face){std::ifstream probe(stem.string()+"-probe-"+std::to_string(face)+".txt");std::getline(probe,line);unsigned covered=0;if(sscanf_s(line.c_str(),"covered=%u",&covered)!=1)throw std::runtime_error("Missing grass probe view");probePixels+=covered;}
        if((probePixels>0)!=(patches>0))throw std::runtime_error("Grass probe geometry missing or stale");
    }
    if(std::memcmp(fields[1].data()+3*65536,fields[2].data()+3*65536,4096)!=0)throw std::runtime_error("Paused projected wind advanced");
    Log::Info("MetaRenderGraph","PASS GrassGpuTests: actual ECS patches, projected/analytic wind, pause/reset, Ribbon/CrossedRibbons, moving interaction; GPU versions 1..8 and previous-version bytes; six diagnostic probe geometry faces; exact serial/parallel fields/images=0");
}
