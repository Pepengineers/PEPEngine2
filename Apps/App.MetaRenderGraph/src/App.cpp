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
    return RunMetaApplication(focused || sceneOnly || matrix!=MetaValidationMatrix::None || args.find("--smoke")!=std::string_view::npos,
        focused, args.find("--gbv")!=std::string_view::npos,sceneOnly,matrix,firstCase);
}
