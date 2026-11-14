#pragma once
#include "NativeWork.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
namespace Meta
{
class SceneAO
{
  public:
    SceneAO(GDX12DeviceResources &, RenderPipelineCommonData &, GDX12Texture *depth, GDX12Texture *normals);
    ~SceneAO();
    void Freeze(GDX12Texture *depth, GDX12Texture *normals, UINT &temporalIndex);
    void Append(Graph &, std::vector<ID3D12Resource *> &);
    bool Record(Id id, ID3D12GraphicsCommandList *cmd) const
    {
        return _work.Record(id, cmd);
    }
    GDX12Texture *Output()
    {
        return &_pass._output;
    }

  private:
    GDX12DeviceResources &_resources;
    RenderPipelineCommonData &_common;
    GDX12AmbientOcclusionPass _pass;
    GDX12AOSettings _settings{};
    Matrix _projection;
    NativeWork _work;
    UINT _width = 0, _height = 0;
    bool _half = false;
};
} // namespace Meta
