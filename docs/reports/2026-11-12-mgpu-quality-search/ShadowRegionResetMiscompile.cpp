#include <array>
#include <vector>
#include <iostream>
#include <cstdint>
struct Region { unsigned Slice, X, Y, Width, Height; };
struct View { Region Bounds; float Matrix[16]; uint64_t Revision; };
struct Output { std::array<View,6> Views = {}; };
__declspec(noinline) bool HasLastFace(std::vector<Output> groups)
{
    return groups.front().Views[5].Bounds.Width != 0;
}
int main()
{
    Output point;
    for (unsigned face = 0; face < 6; ++face) point.Views[face].Bounds = {face,0,0,256,256};
    auto partial = point;
    partial.Views[5].Bounds = {};
    if (HasLastFace({partial}))
    {
        std::cout << "Compiler dropped a region reset\n";
        return 1;
    }
    std::cout << "Region reset survived\n";
    return 0;
}