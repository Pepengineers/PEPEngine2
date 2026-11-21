#include "Techniques/Water/GDX12FluidPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

struct GDX12FluidParticlePass::State
{
	GDX12FluidSolver::Buffers Solver;
	GDX12FluidSolver::Head SolverHead;
	std::unique_ptr<GDX12UploadBuffer<GDX12FluidParameters>> Constants;
	std::vector<std::unique_ptr<GDX12UploadBuffer<XMFLOAT4>>> DisplaySlots;
	std::shared_ptr<const std::vector<GDX12FluidParticle>> Source;
	uint64_t Revision = 0;
	uint64_t SettingsRevision = 0;
	UINT SeedFrameSlot = UINT_MAX;
	bool WasRendered = false;
};
namespace
{
constexpr UINT ParameterSlotsPerFrame = 2; // Simulation and draw can read different radii in the same frame.
constexpr UINT BillboardVertexCount = 4;
constexpr UINT PackThreads = 128;
static_assert(sizeof(XMFLOAT4) == 16, "Must match the water display payload");
static_assert(sizeof(GDX12FluidParameters) == 336, "Must match FluidParameters.hlsli");
} // namespace
GDX12FluidParticlePass::GDX12FluidParticlePass(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _commonData(common)
{
}
GDX12FluidParticlePass::~GDX12FluidParticlePass() = default;
void GDX12FluidParticlePass::CreatePipelines()
{
	if (_role != Role::Solver && !_surfaceRoot)
	{
		GDX12RootSignatureDesc surface;
		surface.VolatileSRVData = true;
		surface.Constants = {1}; // b0 precedes the fluid and camera constant buffers.
		surface.NumSingleCBVSlots = 2;
		surface.NumSingleSRVSlots = 1;
		_surfaceRoot = std::make_unique<GDX12RootSignature>(_resources.Device, surface);
		auto& compiler = GDX12ShaderCompiler::GetInstance();
		auto vs =
		    compiler.CompileShader(_resources.Device, APP_SHADERS_FOLDER "FluidParticleDraw.hlsl", nullptr, "VS", "vs");
		auto ps =
		    compiler.CompileShader(_resources.Device, APP_SHADERS_FOLDER "FluidParticleDraw.hlsl", nullptr, "PS", "ps");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = _surfaceRoot->GetRootSignature().Get();
		desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		desc.SampleMask = UINT_MAX;
		desc.SampleDesc.Count = 1;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		ThrowIfFailed(_resources.Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_particleDraw)));
	}
	if (_role != Role::Display && !_packRoot)
	{
		GDX12RootSignatureDesc pack;
		pack.Constants = {1};
		pack.NumSingleSRVSlots = 1;
		pack.NumSingleUAVSlots = 1;
		_packRoot = std::make_unique<GDX12RootSignature>(_resources.Device, pack);
		const D3D_SHADER_MACRO packDefines[] = {{"FLUID_PACK_DISPLAY", "1"}, {nullptr, nullptr}};
		_packDisplay = GDX12RenderHelpers::CreateComputePipeline(
		    _resources.Device, _packRoot.get(), APP_SHADERS_FOLDER "FluidParticleDraw.hlsl", "PackCS", packDefines);
	}
}
void GDX12FluidParticlePass::PrepareStates()
{
	std::set<uint64_t> alive;
	bool retire = false;
	for (const auto& instance : _instances)
	{
		ValidateFluidInstance(instance);
		const auto& source = instance.Component.GetParticles();
		if (!source || source->empty())
		{
			continue;
		}
		if (!alive.insert(instance.Id).second)
		{
			throw std::invalid_argument("Duplicate fluid instance id");
		}
		auto found = _states.find(instance.Id);
		if (found == _states.end())
		{
			continue;
		}
		const auto& author = instance.Component.GetSettings();
		auto& state = *found->second;
		const bool active = _controls.Enabled && (author.SimulationEnabled || author.RenderEnabled);
		if (active && (state.Source != source || state.Revision != instance.Component.GetResetRevision()))
		{
			retire = true;
		}
		if (state.WasRendered && (!_controls.Enabled || !author.RenderEnabled ||
		                          state.SettingsRevision != instance.Component.GetSettingsRevision()))
		{
			_commonData.ResetHistory = true;
		}
	}
	for (const auto& [id, state] : _states)
	{
		if (!alive.count(id))
		{
			retire = true;
			_commonData.ResetHistory |= state->WasRendered;
		}
	}
	// Old buffers, seed uploads and descriptor slots may still be referenced by queued frames.
	if (retire)
	{
		_retire();
	}
	for (auto it = _states.begin(); it != _states.end();)
	{
		if (!alive.count(it->first))
		{
			it = _states.erase(it);
		}
		else
		{
			++it;
		}
	}
	for (const auto& instance : _instances)
	{
		const auto& source = instance.Component.GetParticles();
		const auto& author = instance.Component.GetSettings();
		if (!source || source->empty() || !_controls.Enabled || (!author.SimulationEnabled && !author.RenderEnabled))
		{
			continue;
		}
		auto& state = _states[instance.Id];
		if (state && state->Source == source && state->Revision == instance.Component.GetResetRevision())
		{
			continue;
		}
		auto replacement = std::make_unique<State>();
		replacement->Source = source;
		replacement->Revision = instance.Component.GetResetRevision();
		if (_role != Role::Display)
			replacement->Solver = GDX12FluidSolver::CreateBuffers(_resources.Device, _resources.SRV_UAV_Heap.get(), instance);
		replacement->Constants = std::make_unique<GDX12UploadBuffer<GDX12FluidParameters>>(
		    _resources.Device, ParameterSlotsPerFrame * static_cast<UINT>(_resources.FrameConstants.size()));
		state = std::move(replacement);
	}
}

std::vector<GDX12ResourceAccess> GDX12FluidParticlePass::GetSimulationBufferAccesses() const
{
	std::vector<GDX12ResourceAccess> accesses;
	for (const auto& [id, state] : _states)
	{
		if (!state->Solver.Count)
		{
			continue;
		}
		accesses.push_back({&state->Constants->GetResource(), GDX12AccessMode::Read});
		for (const auto& particles : state->Solver.Particles)
		{
			accesses.push_back({&particles->GetResource(), GDX12AccessMode::ReadWrite});
		}
		accesses.push_back({&state->Solver.Density->GetResource(), GDX12AccessMode::ReadWrite});
		accesses.push_back({&state->Solver.Keys->GetResource(), GDX12AccessMode::ReadWrite});
		accesses.push_back({&state->Solver.Offsets->GetResource(), GDX12AccessMode::ReadWrite});
		accesses.push_back({&state->Solver.Predictions->GetResource(), GDX12AccessMode::ReadWrite});
		if (state->SolverHead.SeedPending && state->Solver.SeedUpload)
		{
			accesses.push_back({&state->Solver.SeedUpload->GetResource(), GDX12AccessMode::Read});
		}
	}
	return accesses;
}

void GDX12FluidParticlePass::PrepareFrame(std::vector<GDX12FluidInstance> instances,
                                         GDX12FluidControls controls, Role role)
{
	_drawSources.clear();
	_substeps = 0;
	if (role != _role)
	{
		if (!_states.empty()) _retire();
		_states.clear();
		_role = role;
		_commonData.ResetHistory = true;
	}
	_instances = std::move(instances);
	const auto previousControls = _controls;
	_controls = controls;
	PrepareStates();
	// The frame-slot wait covers the final DIRECT fence and its COMPUTE join.
	for (auto& [id, state] : _states)
	{
		if (!state->SolverHead.SeedPending && state->SeedFrameSlot == _resources.CurrFrameConstantsIndex)
		{
			state->Solver.SeedUpload.reset();
		}
	}
	bool rendering = false;
	_simulating = false;
	for (const auto& instance : _instances)
	{
		const auto& seed = instance.Component.GetParticles();
		if (!seed || seed->empty())
		{
			continue;
		}
		const auto& settings = instance.Component.GetSettings();
		rendering |= _controls.Enabled && settings.RenderEnabled;
		_simulating |= _controls.Enabled && settings.SimulationEnabled;
	}
	const bool changed = rendering != _rendering || previousControls.Enabled != _controls.Enabled ||
	                     previousControls.Quality != _controls.Quality;
	_commonData.ResetHistory |= changed;
	_rendering = rendering;
	if (!rendering)
	{
		for (auto& [id, state] : _states)
		{
			state->WasRendered = false;
		}
	}

	if (!_rendering && !_simulating)
	{
		return;
	}
	if (_rendering)
	{
		CreatePipelines();
		PrepareDrawSources();
	}
	if (_role != Role::Display) _solver.Initialize(_resources.Device);
}

void GDX12FluidParticlePass::PrepareDrawSources()
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto* heap = _resources.SRV_UAV_Heap.get();
	for (const auto& instance : _instances)
	{
		const auto found = _states.find(instance.Id);
		const auto settings = ResolveFluidSettings(instance.Component.GetSettings(), _controls.Quality);
		if (found == _states.end() || !settings.RenderEnabled)
		{
			continue;
		}
		auto& state = *found->second;
		if (state.DisplaySlots.empty())
		{
			state.DisplaySlots.resize(_resources.FrameConstants.size());
		}
		auto& display = state.DisplaySlots.at(slot);
		if (!display)
		{
			display = std::make_unique<GDX12UploadBuffer<XMFLOAT4>>(
			    _resources.Device, UINT(state.Source->size()), EBufferType::Default, false);
			display->CreateSRV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
			if (_role != Role::Display)
				display->CreateUAV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
		}
		auto parameters = MakeFluidParameters(instance.ContainerToWorld, settings);
		if (settings.ReferenceModel)
		{
			parameters.Radius *= std::cbrt(std::abs(instance.ContainerToWorld.Determinant()));
		}
		const UINT element = ParameterSlotsPerFrame * slot + 1;
		if (_role != Role::Solver) state.Constants->CopyData(element, parameters);
		_drawSources.push_back({instance.Id, display.get(), &state.Constants->GetResource(),
		                       state.Constants->GetElementAddress(element)});
		state.WasRendered = true;
		state.SettingsRevision = instance.Component.GetSettingsRevision();
	}
}
void GDX12FluidParticlePass::ExecuteSimulation(GDX12CommandList* commands)
{
	if (!_rendering && !_simulating)
	{
		return;
	}
	GDX12_SCOPED_EVENT(commands, "Fluid SPH simulation", Colors::Aqua);
	commands->SetDescriptorHeaps({_resources.SRV_UAV_Heap.get()});
	for (const auto& instance : _instances)
	{
		auto found = _states.find(instance.Id);
		if (found == _states.end())
		{
			continue;
		}
		auto& state = *found->second;
		if (!state.Solver.Count)
		{
			continue;
		}
		const auto settings = ResolveFluidSettings(instance.Component.GetSettings(), _controls.Quality);
		if (!settings.RenderEnabled && !settings.SimulationEnabled)
		{
			continue;
		}
		const auto parameters = MakeFluidParameters(instance.ContainerToWorld, settings);
		const bool seedPending = state.SolverHead.SeedPending;
		const UINT constants = ParameterSlotsPerFrame * _resources.CurrFrameConstantsIndex;

		state.Constants->CopyData(constants, parameters);
		const auto steps = GDX12FluidSolver::PlanSteps(settings, instance.DeltaSeconds * settings.TimeScale);
		_substeps += steps.Count;
		state.SolverHead = _solver.Record(*commands, state.Solver, state.SolverHead, settings, steps,
		                                  state.Constants->GetElementAddress(constants));
		if (seedPending)
		{
			state.SeedFrameSlot = _resources.CurrFrameConstantsIndex;
		}
	}
}

std::vector<GDX12ResourceAccess> GDX12FluidParticlePass::GetPackAccesses() const
{
	std::vector<GDX12ResourceAccess> result;
	for (const auto& source : _drawSources)
	{
		for (const auto& particles : _states.at(source.Instance)->Solver.Particles)
		{
			result.push_back({&particles->GetResource(), GDX12AccessMode::Read,
			                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
		}
		result.push_back({&source.Particles->GetResource(), GDX12AccessMode::Write,
		                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
	}
	return result;
}

void GDX12FluidParticlePass::PackDrawSources(GDX12CommandList* commands)
{
	GDX12_SCOPED_EVENT(commands, "Pack water display", Colors::Aqua);
	commands->SetDescriptorHeaps({_resources.SRV_UAV_Heap.get()});
	commands->SetComputeRootSignature(_packRoot.get());
	commands->SetPipelineState(_packDisplay);
	for (const auto& source : _drawSources)
	{
		const auto& state = *_states.at(source.Instance);
		const auto& particleBuffer = state.Solver.Particles[state.SolverHead.Current];
		const UINT count = source.Particles->GetElementCount();
		commands->GetCommandList()->SetComputeRoot32BitConstant(0, count, 0);
		commands->SetComputeSRV(0, particleBuffer->GetSRV()->GPUHandle);
		commands->SetComputeUAV(0, source.Particles->GetUAV()->GPUHandle);
		commands->Dispatch((count + PackThreads - 1) / PackThreads, 1, 1);
	}
}

void GDX12FluidParticlePass::DrawParticles(GDX12CommandList* commands, GDX12Texture* hdr,
                                          D3D12_CPU_DESCRIPTOR_HANDLE depth, const std::vector<DrawSource>& sources)
{
	if (hdr->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		throw std::logic_error("Fluid particle draw requires RGBA16F HDR");
	}
	commands->SetDescriptorHeaps({_resources.SRV_UAV_Heap.get()});
	commands->SetGraphicsRootSignature(_surfaceRoot.get());
	commands->SetPipelineState(_particleDraw);
	commands->SetViewport(hdr->GetViewport());
	commands->SetScissorRect(hdr->GetScissorRect());
	commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	const auto color = hdr->GetRTV()->CPUHandle;
	commands->GetCommandList()->OMSetRenderTargets(1, &color, false, &depth);
	auto& frame = _resources.FrameConstants[_resources.CurrFrameConstantsIndex];
	commands->SetGraphicsRootConstantBufferView(
	    2, frame->CameraCB->GetElementAddress(_commonData.ActiveCameraCBufferIndex));
	for (const auto& source : sources)
	{
		commands->SetGraphicsRootConstantBufferView(1, source.Parameters);
		commands->SetGraphicsSRV(0, source.Particles->GetSRV()->GPUHandle);
		commands->DrawInstanced(BillboardVertexCount, source.Particles->GetElementCount(), 0, 0);
	}
}

std::vector<GDX12ResourceAccess>
GDX12FluidParticlePass::NativeSurfaceAccesses(const std::vector<DrawSource>& sources) const
{
	using Mode = GDX12AccessMode;
	std::vector<GDX12ResourceAccess> result;
	result.push_back({&_resources.FrameConstants.at(_resources.CurrFrameConstantsIndex)->CameraCB->GetResource(),
	                  Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	for (const auto& source : sources)
	{
		result.push_back({source.ParameterBuffer, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
		result.push_back({&source.Particles->GetResource(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
	}
	return result;
}
