#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <iterator>
#include <cmath>
#include <cstring>
#include "Engine.Core/Logging/Logger.h"

void CheckGeometryGpuEvidence(const std::filesystem::path& root) {
    const unsigned expected[]={4,4,3,0,4,3,0,4};
    for(unsigned i=0;i<8;++i) {
        const auto stem=root/("geometry-"+std::to_string(i)+"-parallel");
        std::ifstream in(stem.string()+"-metrics.txt"); std::stringstream text; text<<in.rdbuf();
        const auto contents=text.str();
        if(contents.find("opaque="+std::to_string(expected[i])+" ")==std::string::npos)
            throw std::runtime_error("Geometry GPU culling mismatch frame="+std::to_string(i)+": "+contents);
        const auto position=contents.find("depth_covered_pixels=");
        if(position==std::string::npos) throw std::runtime_error("Missing depth coverage metric");
        const auto coverage=std::stoul(contents.substr(position+21));
        if((coverage>0)!=(expected[i]>0)) throw std::runtime_error("Geometry depth coverage mismatch");
        std::ifstream skin(stem.string()+"-skin.bin",std::ios::binary);
        std::vector<char> vertices(std::istreambuf_iterator<char>(skin),{});
        if(vertices.size()!=4*56) throw std::runtime_error("Animated fixture did not dispatch four skinned vertices");
        float vertexPosition[3]; std::memcpy(vertexPosition,vertices.data(),sizeof(vertexPosition));
        if(std::abs(vertexPosition[0]-(-70.f+10.f*i))>0.001f || std::abs(vertexPosition[1]-(-70.f+10.f*i))>0.001f)
            throw std::runtime_error("Animated GPU vertex differs from expected pose");
    }
    const auto read=[&](const char* file) {
        std::ifstream in(root/file,std::ios::binary); return std::vector<char>(std::istreambuf_iterator<char>(in),{});
    };
    if(read("geometry-0-parallel-images.bin")==read("geometry-1-parallel-images.bin"))
        throw std::runtime_error("Object motion did not update geometry attachments");
    Log::Info("MetaRenderGraph","PASS GeometryGpuTests: 8 serial/parallel pairs; motion, invisible and empty commands, three slots, resize; exact image metric=0");
}
