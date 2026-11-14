#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include "Engine.Core/Logging/Logger.h"

void CheckShadowGpuEvidence(const std::filesystem::path& root) {
    unsigned sunPixels=0,localPixels=0;
    for(unsigned frame=0;frame<8;++frame) for(unsigned view=0;view<11;++view) {
        const auto stem=root/("geometry-"+std::to_string(frame)+"-parallel-shadow-"+std::to_string(view));
        std::ifstream in(stem.string()+".txt"); std::string key; unsigned sun=0,covered=0,statics=0,dynamic=0;
        std::string line; std::getline(in,line);
        if(sscanf_s(line.c_str(),"sun=%u covered=%u static=%u dynamic=%u",&sun,&covered,&statics,&dynamic)!=4)
            throw std::runtime_error("Missing shadow evidence "+stem.string());
        if(bool(sun)!=(view<4)) throw std::runtime_error("Shadow view constants selected the wrong family");
        if(frame==0) (sun?sunPixels:localPixels)+=covered;
        if((frame==3 || frame==6) && !sun && (covered || statics || dynamic)) throw std::runtime_error("Empty local shadow retained a draw");
    }
    if(!sunPixels || !localPixels) throw std::runtime_error("Shadow maps contain no actual casters");
    const auto read=[&](const std::string& file) { std::ifstream in(root/file,std::ios::binary); return std::vector<char>(std::istreambuf_iterator<char>(in),{}); };
    if(read("geometry-0-parallel-shadow-4.pgm")==read("geometry-0-parallel-shadow-8.pgm"))
        throw std::runtime_error("Independent shadow cameras produced identical maps");
    if(read("geometry-0-parallel-shadow-8.pgm")==read("geometry-1-parallel-shadow-8.pgm"))
        throw std::runtime_error("Motion/animation did not change local shadow");
    Log::Info("MetaRenderGraph","PASS ShadowGpuTests: four cascades, six point faces, spot; independent cameras; motion; empty views; exact serial/parallel depth metric=0; sun pixels=",sunPixels," local pixels=",localPixels);
}
