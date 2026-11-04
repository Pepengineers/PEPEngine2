#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITTransparencyPass;

enum class GDX12GeometryStage : std::uint8_t
{
    Depth, DepthNormals, GBuffer, Forward, Shadow, ReflectionCapture
};

struct GDX12CaptureLighting
{
    GDX12Texture* SunShadow = nullptr;
    GDX12Texture* Irradiance = nullptr;
    GDX12Texture* Prefiltered = nullptr;
    GDX12Texture* BRDF = nullptr;
    GDX12Texture* LocalShadows = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants = 0;
};

struct GDX12GeometryContext
{
    GDX12GeometryStage Stage = GDX12GeometryStage::Forward;
    Matrix ViewProjection = Matrix::Identity;
    Vector3 Eye = Vector3::Zero;
    D3D12_GPU_VIRTUAL_ADDRESS CameraConstants = 0;
    const GDX12CaptureLighting* CaptureLighting = nullptr;
    bool StaticOnly = false;
    // Targets, viewport and scissor are already bound by the containing pass.
};

struct GDX12SceneContributions
{
    std::vector<GDX12LocalShadowCaster> Casters;
    uint64_t Revision = 14695981039346656037ull;
    bool HasBounds = false;
    BoundingBox Bounds;

    void Append(const BoundingBox& bounds, uint64_t revision, bool dynamic, bool castsShadows)
    {
        Revision = (Revision ^ revision) * 1099511628211ull;
        if (HasBounds) BoundingBox::CreateMerged(Bounds, Bounds, bounds);
        else Bounds = bounds;
        HasBounds = true;
        if (castsShadows) Casters.push_back({ bounds, revision, dynamic });
    }
};

// Geometry-only source; pipeline composition belongs to IRenderPipeline.
class IRenderGeometrySource
{
public:
    virtual ~IRenderGeometrySource() = default;
    virtual void AppendSceneContributions(GDX12SceneContributions&) const {}
    virtual void DeclareGeometryResources(GDX12RenderGraphBuilder&) const {}
    virtual void DrawGeometry(GDX12CommandList*, const GDX12GeometryContext&) = 0;
};
