#include "MetaApplication.h"
#include <Windows.h>
#include <string_view>
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR arguments, int)
{
    return RunMetaApplication(std::string_view(arguments).find("--smoke") != std::string_view::npos);
}
