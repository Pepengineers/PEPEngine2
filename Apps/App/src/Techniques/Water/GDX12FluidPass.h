#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "GDX12FluidSolver.h"
#include <functional>
#include <map>

class GDX12FluidParticlePass final
{
  public:
	GDX12FluidParticlePass(GDX12DeviceResources&, RenderPipelineCommonData&);
	~GDX12FluidParticlePass();
	void PrepareFrame(std::vector<GDX12FluidInstance>);
	void SetResourceRetirement(std::function<void()> retire) { _retire = std::move(retire); }
	bool HasRetirement() const { return bool(_retire); }
	bool IsRendering() const { return _rendering; }
	bool NeedsSimulation() const { return _rendering || _simulating; }
	void DrawParticles(GDX12CommandList*, GDX12Texture*, D3D12_CPU_DESCRIPTOR_HANDLE);
	void ExecuteSimulation(GDX12CommandList*);
	std::vector<GDX12ResourceAccess> NativeSurfaceAccesses() const;
	std::vector<GDX12ResourceAccess> GetSimulationBufferAccesses() const;

  private:
	struct State;
	void PrepareStates();
	void CreatePipelines();
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _commonData;
	std::vector<GDX12FluidInstance> _instances;
	std::map<uint64_t, std::unique_ptr<State>> _states;
	GDX12FluidSolver _solver;
	std::unique_ptr<GDX12RootSignature> _surfaceRoot;
	ComPtr<ID3D12PipelineState> _particleDraw;
	GDX12FluidControls _controls = {false, 0};
	std::function<void()> _retire;
	bool _rendering = false, _simulating = false;
};
