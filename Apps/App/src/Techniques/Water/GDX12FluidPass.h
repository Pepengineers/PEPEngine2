#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "GDX12FluidSolver.h"
#include <functional>
#include <map>

class GDX12FluidParticlePass final
{
  public:
	struct DrawSource
	{
		uint64_t Instance;
		GDX12UploadBuffer<XMFLOAT4>* Particles;
		GDX12Resource* ParameterBuffer;
		D3D12_GPU_VIRTUAL_ADDRESS Parameters;
	};

	GDX12FluidParticlePass(GDX12DeviceResources&, RenderPipelineCommonData&);
	~GDX12FluidParticlePass();
	void PrepareFrame(std::vector<GDX12FluidInstance>);
	void SetResourceRetirement(std::function<void()> retire) { _retire = std::move(retire); }
	bool HasRetirement() const { return bool(_retire); }
	bool NeedsSimulation() const { return _rendering || _simulating; }
	const std::vector<DrawSource>& DrawSources() const { return _drawSources; }
	void DrawParticles(GDX12CommandList*, GDX12Texture*, D3D12_CPU_DESCRIPTOR_HANDLE, const std::vector<DrawSource>&);
	void ExecuteSimulation(GDX12CommandList*);
	void PackDrawSources(GDX12CommandList*);
	std::vector<GDX12ResourceAccess> NativeSurfaceAccesses(const std::vector<DrawSource>&) const;
	std::vector<GDX12ResourceAccess> GetSimulationBufferAccesses() const;
	std::vector<GDX12ResourceAccess> GetPackAccesses() const;

  private:
	struct State;
	void PrepareStates();
	void PrepareDrawSources();
	void CreatePipelines();
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _commonData;
	std::vector<GDX12FluidInstance> _instances;
	std::vector<DrawSource> _drawSources;
	std::map<uint64_t, std::unique_ptr<State>> _states;
	GDX12FluidSolver _solver;
	std::unique_ptr<GDX12RootSignature> _surfaceRoot, _packRoot;
	ComPtr<ID3D12PipelineState> _particleDraw, _packDisplay;
	GDX12FluidControls _controls = {false, 0};
	std::function<void()> _retire;
	bool _rendering = false, _simulating = false;
};
