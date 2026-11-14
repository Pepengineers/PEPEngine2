#include "MetaApplication.h"
#include <Windows.h>
#include <string_view>
#include <charconv>
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR arguments, int)
{
    const std::string_view args(arguments);
    const bool focused=args.find("--gpu-fixtures")!=std::string_view::npos;
    const bool sceneOnly=args.find("--scene-smoke")!=std::string_view::npos;
    const auto matrix=args.find("--particle-interaction")!=std::string_view::npos?MetaValidationMatrix::ParticleInteraction:
        args.find("--ao-ssr-matrix")!=std::string_view::npos?MetaValidationMatrix::AO_SSR:
        args.find("--fsr-check")!=std::string_view::npos?MetaValidationMatrix::FSR:MetaValidationMatrix::None;
    unsigned firstCase=0;
    const auto resume=args.find("--matrix-start=");
    if(resume!=std::string_view::npos) {
        const auto parsed=std::from_chars(args.data()+resume+15,args.data()+args.size(),firstCase);
        if(parsed.ec!=std::errc{} || firstCase>20 || (parsed.ptr!=args.data()+args.size() && *parsed.ptr!=' ')) return 1;
    }
    MetaBenchmark benchmark;
    if(args.find("--benchmark")!=std::string_view::npos) {
        benchmark.Proof=args.find("--benchmark-proof")!=std::string_view::npos;
        benchmark.Frames=benchmark.Proof || args.find("--benchmark-long")!=std::string_view::npos?1800:
            args.find("--benchmark-preflight")!=std::string_view::npos || args.find("--benchmark-trace")!=std::string_view::npos?32:300;
        if(benchmark.Frames==32) benchmark.Warmup=8;
        benchmark.Direct=args.find("--direct")!=std::string_view::npos;
        benchmark.Serial=args.find("--serial")!=std::string_view::npos;
        benchmark.FSR=args.find("--fsr")!=std::string_view::npos;
        benchmark.Trace=args.find("--benchmark-trace")!=std::string_view::npos;
        if(focused || sceneOnly || matrix!=MetaValidationMatrix::None || args.find("--smoke")!=std::string_view::npos || args.find("--gbv")!=std::string_view::npos) return 1;
    }
    return RunMetaApplication(focused || sceneOnly || matrix!=MetaValidationMatrix::None || args.find("--smoke")!=std::string_view::npos,
        focused, args.find("--gbv")!=std::string_view::npos,sceneOnly,matrix,firstCase,benchmark);
}
