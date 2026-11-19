#include "SceneParticles.h"
#include "SceneGeometry.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncSimulation(L"App.Async.Particles", false,
                                          L"Run particle pool initialization and simulation on the local COMPUTE queue.",
                                          EConsoleVariableFlags::Default);

State BoundaryState(ID3D12Resource* resource)
{
	const auto desc = resource->GetDesc();
	if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
	{
		return D3D12_RESOURCE_STATE_GENERIC_READ;
	}
	return desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE ? State(D3D12_RESOURCE_STATE_COMMON)
	                                                             : State(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}
Access ResourceAccess(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Resource* resource, Mode mode,
                      State state = State(-1))
{
	auto* native = resource->D3DResource.Get();
	D3D12_HEAP_PROPERTIES heap{};
	ThrowIfFailed(native->GetHeapProperties(&heap, nullptr));
	return {NativeWork::ResourceFor(graph, bindings, native, "particle resource"),
	        heap.Type == D3D12_HEAP_TYPE_UPLOAD ? Mode::Read : mode,
	        heap.Type == D3D12_HEAP_TYPE_UPLOAD || state == State(-1) ? BoundaryState(native) : state};
}
void AddResources(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses,
                  const std::vector<GDX12Resource*>& resources, Mode mode)
{
	for (auto* resource : resources)
	{
		accesses.push_back(ResourceAccess(graph, bindings, resource, mode));
	}
}
void AddAccesses(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses,
                 const std::vector<GDX12ResourceAccess>& resources)
{
	for (const auto& resource : resources)
	{
		accesses.push_back(ResourceAccess(graph, bindings, resource.Resource,
		                                  resource.Usage == GDX12AccessMode::Read    ? Mode::Read
		                                  : resource.Usage == GDX12AccessMode::Write ? Mode::Write
		                                                                             : Mode::ReadWrite,
		                                  resource.State));
	}
}
} // namespace
SceneParticles::SceneParticles(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _common(common)
{
	if (!_resources.ParticleSimulation)
	{
		_resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
	}
	// Merged fluid depth is an SRV/UAV, so the raster PSO always uses shader depth rejection.
	_pass.SetReadOnlyDepthEnabled(false);
	_pass.SetMotionOutputEnabled(false);
	_pass.Setup(&_resources, &_common);
}
SceneParticles::~SceneParticles()
{
	_pass.ClearDenendencies();
	if (_resources.ParticleSimulation)
	{
		_resources.ParticleSimulation->SetInteractionField({});
	}
}
void SceneParticles::Packet(Graph& graph, const std::vector<ID3D12Resource*>& bindings, const char* name,
                            std::vector<Access> accesses, std::function<void(GDX12CommandList*)> record,
                            Queue target)
{
	std::vector<Access> unique;
	for (auto access : accesses)
	{
		const auto found =
		    std::find_if(unique.begin(), unique.end(), [&](auto other) { return other.Resource == access.Resource; });
		if (found == unique.end())
		{
			unique.push_back(access);
		}
		else
		{
			if (found->Required != access.Required)
			{
				throw std::logic_error("Conflicting particle resource states");
			}
			if (found->Usage != access.Usage)
			{
				found->Usage = Mode::ReadWrite;
			}
		}
	}
	_work.AddOwned(graph, name, std::move(unique), bindings, std::move(record),
	               "Particle packets cannot submit private signals", {}, target);
}
void SceneParticles::BeginSimulation(GDX12ParticleInteractionField interaction)
{
	_work.Commands.clear();
	_interaction = interaction;
	auto* simulation = _resources.ParticleSimulation.get();
	simulation->SetInteractionField(interaction);
	simulation->BeginFrame(&_resources, _common);
	_renderFrame = simulation->RenderFrame();
	for (auto& group : _groups)
	{
		group.clear();
	}
	for (const auto& emitter : _resources.ParticleEmitters)
	{
		const auto inputs = emitter.DepthCollision ? Inputs::Depth
		                  : emitter.InteractionEnabled && interaction.Resource ? Inputs::Interaction : Inputs::Frame;
		_groups[size_t(inputs)].push_back(emitter.Id);
	}
}
void SceneParticles::AppendSimulation(Graph& graph, std::vector<ID3D12Resource*>& bindings, Inputs inputs,
                                      GDX12Texture* depth)
{
	const auto& ids = _groups[size_t(inputs)];
	auto* simulation = _resources.ParticleSimulation.get();
	std::vector<Access> pools;
	AddResources(graph, bindings, pools, simulation->GetPoolResources(&ids), Mode::ReadWrite);
	if (pools.empty())
	{
		return;
	}
	const bool async = AsyncSimulation.GetValue();
	const Queue target = async ? Queue::Compute : Queue::Direct;
	const State simulationRead = async ? State(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) : State(-1);
	if (async)
	{
		for (auto& access : pools)
		{
			access.Required = UnorderedAccess;
		}
	}
	const char* initialize[] = {"particle frame pool initialization/reset", "particle interaction pool initialization/reset",
	                            "particle depth pool initialization/reset"};
	const char* simulate[] = {"particle simulation from frame inputs", "particle simulation from interaction field",
	                          "particle simulation from depth prepass"};
	Packet(graph, bindings, initialize[size_t(inputs)], pools,
	       [this, simulation, ids](auto* cmd) { simulation->InitializePools(cmd, &_resources, &ids); }, target);
	const UINT slot = _resources.CurrFrameConstantsIndex;
	AddResources(graph, bindings, pools, simulation->GetConstantResources(slot, &ids), Mode::Read);
	pools.push_back(ResourceAccess(graph, bindings, &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read));
	if (inputs == Inputs::Depth)
	{
		pools.push_back(ResourceAccess(graph, bindings, depth->GetResource(), Mode::Read, simulationRead));
	}
	if (inputs != Inputs::Frame && _interaction.Resource)
	{
		pools.push_back(ResourceAccess(graph, bindings, _interaction.Resource, Mode::Read, simulationRead));
	}
	Packet(graph, bindings, simulate[size_t(inputs)], std::move(pools), [this, simulation, depth, ids](auto* cmd)
	       { simulation->Execute(cmd, &_resources, _common, depth, &ids); }, target);
}
void SceneParticles::AppendOpaque(Graph& graph, std::vector<ID3D12Resource*>& bindings, const SceneGeometry& geometry,
                                  UINT slot)
{
	auto* depth = geometry.Depth(slot);
	if (_width && (_width != depth->GetWidth() || _height != depth->GetHeight()))
	{
		_pass.Resize();
	}
	_width = depth->GetWidth();
	_height = depth->GetHeight();
	_pass.SetMotionOutputEnabled(HasDrawSources(false) &&
	    (GetMotionBlurSettings().Enabled || GetFrameGenerationProvider() != GDX12FrameGenerationProvider::Off ||
	     (_common.Upscaler && (_common.Upscaler->GetFlags() & RENDER_PASS_FLAG_USE_JITTER))));
	_pass.BeginFrame(depth, _renderFrame);
	if (!HasDrawSources(true))
	{
		return;
	}
	std::array<GDX12Texture*, 5> gbuffer;
	for (UINT i = 0; i < gbuffer.size(); ++i)
	{
		gbuffer[i] = geometry.GBuffer(slot, i);
	}
	if (!_pass.InitializeOpaque(gbuffer))
	{
		return;
	}
	AppendPreparation(graph, bindings, true);
	auto accesses = DrawReads(graph, bindings, true);
	AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(false));
	for (auto* target : gbuffer)
	{
		accesses.push_back(ResourceAccess(graph, bindings, target->GetResource(), Mode::ReadWrite,
		                                  D3D12_RESOURCE_STATE_RENDER_TARGET));
	}
	accesses.push_back(ResourceAccess(graph, bindings, depth->GetResource(), Mode::ReadWrite,
	                                  D3D12_RESOURCE_STATE_DEPTH_WRITE));
	const auto dsv = geometry.DepthDSV(slot);
	Packet(graph, bindings, "opaque particles G-buffer and depth", std::move(accesses),
	       [this, depth, dsv, gbuffer](auto* cmd) { _pass.DrawOpaque(cmd, depth, dsv, gbuffer); });
}
bool SceneParticles::HasDrawSources(bool opaque) const
{
	return std::any_of(_renderFrame.Sources.begin(), _renderFrame.Sources.end(), [opaque](const auto& source)
	                   { return (source.Emitter.Blend == ParticleBlend::Opaque) == opaque; });
}
void SceneParticles::AppendPreparation(Graph& graph, std::vector<ID3D12Resource*>& bindings, bool opaque)
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	const auto camera =
	    ResourceAccess(graph, bindings, &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read);
	std::vector<Access> preparation{camera};
	AddAccesses(graph, bindings, preparation, _pass.NativePreparationAccesses(_renderFrame, opaque));
	Packet(graph, bindings, opaque ? "opaque particles frustum cull" : "transparent particles cull and global sort",
	       std::move(preparation), [this, opaque](auto* cmd) { _pass.PrepareRenderData(cmd, _renderFrame, opaque); });
}
std::vector<Access> SceneParticles::DrawReads(Graph& graph, std::vector<ID3D12Resource*>& bindings, bool opaque)
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	std::vector<Access> reads{
	    ResourceAccess(graph, bindings, &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read)};
	for (const auto& source : _renderFrame.Sources)
	{
		const auto& emitter = source.Emitter;
		if ((emitter.Blend == ParticleBlend::Opaque) == opaque && emitter.Texture)
		{
			auto* atlas = emitter.Texture->PrimaryDeviceTexture;
			if (!atlas)
			{
				throw std::logic_error("Particle atlas has no primary texture");
			}
			constexpr State published = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
			const Id id = NativeWork::ImportReadOnlyTexture(graph, bindings, *atlas->GetResource(), published,
			                                                "renderer particle atlas");
			reads.push_back({id, Mode::Read, published});
		}
	}
	return reads;
}
void SceneParticles::Prepare(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* rasterDepth,
                             GDX12Texture* accum, GDX12Texture* reveal)
{
	_drawReads.clear();
	if (!HasDrawSources(false) || _pass.NativeRasterAccesses(false).empty())
	{
		return;
	}
	_accum = accum;
	_reveal = reveal;
	_pass.SetInputs({rasterDepth, accum, reveal});
	_pass.Initialize();
	AppendPreparation(graph, bindings, false);
	_drawReads = DrawReads(graph, bindings, false);
	_drawReads.push_back(ResourceAccess(graph, bindings, rasterDepth->GetResource(), Mode::Read));
}
void SceneParticles::AppendWeighted(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	if (_drawReads.empty()) return;
	auto accesses = _drawReads;
	AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(true));
	accesses.push_back(
	    ResourceAccess(graph, bindings, _accum->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
	accesses.push_back(
	    ResourceAccess(graph, bindings, _reveal->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
	Packet(graph, bindings, "particle weighted-alpha append", std::move(accesses),
	       [this](auto* cmd) { _pass.DrawWeightedAndMotion(cmd); });
}
void SceneParticles::AppendComposite(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr)
{
	if (_drawReads.empty()) return;
	auto accesses = _drawReads;
	AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(false));
	accesses.push_back(
	    ResourceAccess(graph, bindings, hdr->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
	Packet(graph, bindings, "global SortedAlpha/additive HDR particles", std::move(accesses),
	       [this, hdr](auto* cmd) { _pass.Composite(cmd, hdr); });
}
} // namespace BistroRender
