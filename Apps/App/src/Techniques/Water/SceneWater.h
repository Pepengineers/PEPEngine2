#pragma once
#include "Techniques/Water/GDX12FluidPass.h"
#include "Techniques/NativeWork.h"
#include "Techniques/Water/BistroWaterRendering.h"

namespace BistroRender
{
class SceneWater
{
  public:
	SceneWater(GDX12DeviceResources&, RenderPipelineCommonData&);
	void SetRetirement(std::function<void()> retire);
	void Prepare(float delta);
	void AppendSimulation(Graph&, std::vector<ID3D12Resource*>&);
	void SetSceneInputs(GDX12Texture* opaqueDepth, GDX12Texture* opaqueMotion);
	void AppendDraw(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr, D3D12_CPU_DESCRIPTOR_HANDLE depth);
	GDX12Texture* Depth() const { return _depth; }
	GDX12Texture* Motion() const { return _motion; }
	bool Record(Id id, ID3D12GraphicsCommandList* commands) const { return _work.Record(id, commands); }

  private:
	void Packet(Graph&, std::vector<ID3D12Resource*>&, const char*, const std::vector<GDX12ResourceAccess>&,
	            std::function<void(GDX12CommandList*)>, Queue target = Queue::Direct);
	BistroWaterRendering _collector;
	GDX12FluidParticlePass _surface;
	NativeWork _work;
	GDX12Texture* _depth = nullptr;
	GDX12Texture* _motion = nullptr;
};
} // namespace BistroRender
