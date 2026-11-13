#pragma once
#include "Graph/MetaGraph.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"
namespace Meta {
struct GeometryTarget {
    GDX12GeometryContext View;
    Id Depth=Invalid;
    Id Camera=Invalid;
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv{};
    std::vector<Id> Colors;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> Rtvs;
    UINT Width=0,Height=0;
    bool Clear=false;
};
}
