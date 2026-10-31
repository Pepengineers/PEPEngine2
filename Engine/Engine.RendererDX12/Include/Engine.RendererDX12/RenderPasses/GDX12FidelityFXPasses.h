#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: HDR color, reverse-Z depth, NDC motion, world normal, material parameters (roughness in A), cubemap, BRDF LUT.
// Output is reflection radiance; the material BRDF must be applied once by the lighting resolve.
class GDX12SssrPass final : public GDX12RenderPass
{
public:
    GDX12SssrPass();
    ~GDX12SssrPass() override;
    void Initialize() override;
    void PrepareFrame() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
    bool IsEnabled() const;
    bool HasValidHistory() const;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};

// Inputs: linear HDR RGBA16F color and matching reverse-Z depth. Off is an exact color copy.
class GDX12DepthOfFieldPass final : public GDX12RenderPass
{
public:
    GDX12DepthOfFieldPass();
    ~GDX12DepthOfFieldPass() override;
    void Initialize() override;
    void PrepareFrame() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
    bool IsEnabled() const;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
