#pragma once
#include "Techniques/Water/GDX12FluidPass.h"
#include "Techniques/NativeWork.h"
#include "Techniques/Water/BistroWaterRendering.h"
#include "Graph/RenderGraphExecutor.h"
#include "Techniques/SimulationPublication.h"

namespace BistroRender
{
class SceneWater
{
  public:
	SceneWater(GDX12DeviceResources&, RenderPipelineCommonData&);
	~SceneWater();
	static int SecondaryAdapter();
	void SetRetirement(std::function<void()> retire);
	void Prepare(float delta, Executor&, Id device);
	Id ExecutionDevice() const;
	bool BenchmarkReady() const;
	void AppendBenchmarkPublications(std::vector<SimulationPublication>&) const;
	void PauseIndependentClock();
	void AddWaits(const Schedule&, std::vector<PacketWait>&);
	void Submitted(const std::vector<CompletionToken>&);
	void Presented(uint64_t frame, const std::vector<CompletionToken>&);
	void AppendSimulation(Graph&, std::vector<ID3D12Resource*>&);
	void SetSceneInputs(GDX12Texture* opaqueDepth, GDX12Texture* opaqueMotion);
	bool DrawsDiagnosticParticles() const { return !_surface.DrawSources().empty(); }
	void AppendDraw(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr, D3D12_CPU_DESCRIPTOR_HANDLE depth);
	GDX12Texture* Depth() const { return _depth; }
	GDX12Texture* Motion() const { return _motion; }
	bool Record(Id id, ID3D12GraphicsCommandList* commands) const { return _work.Record(id, commands); }

  private:
	static Id Packet(NativeWork&, Graph&, std::vector<ID3D12Resource*>&, const char*, const std::vector<GDX12ResourceAccess>&,
	            std::function<void(GDX12CommandList*)>, Queue target = Queue::Direct);
	struct Remote;
	void RetireRemote(Executor&);
	GDX12DeviceResources& _resources;
	BistroWaterRendering _collector;
	GDX12FluidParticlePass _surface;
	NativeWork _work;
	std::unique_ptr<Remote> _remote;
	Id _attemptedDevice = 0, _failedDevice = 0;
	Id _import = Invalid, _draw = Invalid, _importPacket = Invalid, _drawPacket = Invalid;
	GDX12Texture* _depth = nullptr;
	GDX12Texture* _motion = nullptr;
};
} // namespace BistroRender
