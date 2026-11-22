// dear imgui: single-file wrapper include
// We use this to validate compiling all *.cpp files in a same compilation unit.
// Users of that technique (also called "Unity builds") can generally provide this themselves easily.

// Do this:
//    #define IMGUI_IMPLEMENTATION
//    #include "imgui_single_file.h"
// In *one* C++ file to create the implementation.
// Using this in your project will leak the contents of imgui_internal.h and ImVec2 operators in this compilation unit.

#if defined(IMGUI_IMPLEMENTATION) && !defined(IMGUI_DEFINE_MATH_OPERATORS)
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui/imgui.h>
#ifdef IMGUI_ENABLE_FREETYPE
#include <imgui/misc/imgui_freetype.h>
#endif

#ifdef IMGUI_IMPLEMENTATION
#include "../../../src/imgui.cpp"
#include "../../../src/imgui_demo.cpp"
#include "../../../src/imgui_draw.cpp"
#include "../../../src/imgui_tables.cpp"
#include "../../../src/imgui_widgets.cpp"
#ifdef IMGUI_ENABLE_FREETYPE
#include "../../../src/misc/imgui_freetype.cpp"
#endif
#endif
