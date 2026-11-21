#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include <memory>
#include <string>

class SceneManagerModule;
class World;
class RenderModule;
namespace Engine::Core { class Mesh; }

namespace BistroRender
{
struct LindenVertex
{
    Vector3 Position;
    UINT Fragment;
    Vector3 Normal;
    float Weight;
    Vector2 UV;
};

struct LindenFragment
{
    Vector3 Pivot;
    float Phase;
};

struct LindenBatch
{
    UINT FirstIndex, IndexCount, MaterialIndex;
    bool Leaves;
};

struct LindenMesh
{
    std::string Label;
    std::vector<LindenVertex> Vertices;
    std::vector<UINT> Indices;
    std::vector<LindenFragment> Fragments;
    std::vector<LindenBatch> Batches;
    DirectX::BoundingBox Bounds, WoodBounds, CrownBounds;
    DirectX::BoundingBox WindBounds(DirectX::BoundingBox bounds) const
    {
        // LindenWind.hlsl caps the offset from each rest position at 25 cm.
        bounds.Extents = Vector3(bounds.Extents) + Vector3(25.f);
        return bounds;
    }
};

static_assert(sizeof(LindenVertex) == 40 && offsetof(LindenVertex, UV) == 32);
static_assert(sizeof(LindenFragment) == 16);

void ExtractLinden(World&, const Engine::Core::Mesh&, RenderModule&, std::vector<UINT>& staticParts);
std::shared_ptr<const LindenMesh> FindLindenMesh(SceneManagerModule&);
}
