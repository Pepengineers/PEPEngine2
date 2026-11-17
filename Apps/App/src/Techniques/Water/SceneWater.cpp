#include "SceneWater.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncWater(L"App.Async.Water", false,
                                     L"Run the SPH water simulation on the local COMPUTE queue.",
                                     EConsoleVariableFlags::Default);
}

SceneWater::SceneWater(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _collector(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()), _surface(resources, common)
{
}
void SceneWater::SetRetirement(std::function<void()> retire)
{
	_surface.SetResourceRetirement(std::move(retire));
}
void SceneWater::Prepare(float delta)
{
	_instances = _collector.CollectScene(delta);
}
void SceneWater::Packet(Graph& graph, std::vector<ID3D12Resource*>& bindings, const char* name,
                        const std::vector<GDX12ResourceAccess>& resources,
                        std::function<void(GDX12CommandList*)> record, Queue target)
{
	std::vector<Access> accesses;
	for (const auto& resource : resources)
	{
		auto* native = resource.Resource->D3DResource.Get();
		const Id id = NativeWork::ResourceFor(graph, bindings, native, name);
		const bool upload = graph.Resources.at(id).Memory == Heap::Upload;
		const auto mode = upload || resource.Usage == GDX12AccessMode::Read ? Mode::Read
		                  : resource.Usage == GDX12AccessMode::Write        ? Mode::Write
		                                                                    : Mode::ReadWrite;
		const State state =
		    upload                                        ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
		    : resource.State != D3D12_RESOURCE_STATES(-1) ? State(resource.State)
		    : native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
		        ? State(mode == Mode::Read ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		        : State(D3D12_RESOURCE_STATE_COMMON);
		const auto found =
		    std::find_if(accesses.begin(), accesses.end(), [id](auto access) { return access.Resource == id; });
		if (found == accesses.end())
		{
			accesses.push_back({id, mode, state});
		}
		else
		{
			if (found->Required != state)
			{
				throw std::logic_error("Conflicting water packet states");
			}
			if (found->Usage != mode)
			{
				found->Usage = Mode::ReadWrite;
			}
		}
	}
	_work.AddOwned(graph, name, std::move(accesses), bindings, std::move(record),
	               "Water recording cannot submit private queue signals", {}, target);
}
void SceneWater::AppendSurface(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* opaqueDepth,
                               GDX12Texture* opaqueMotion)
{
	if (!_surface.HasRetirement())
	{
		throw std::logic_error("Water requires executor-owned resource retirement");
	}
	_work.Commands.clear();
	if (!opaqueDepth->GetWidth() || !opaqueDepth->GetHeight() || opaqueDepth->GetWidth() != opaqueMotion->GetWidth() ||
	    opaqueDepth->GetHeight() != opaqueMotion->GetHeight() || opaqueMotion->GetFormat() != DXGI_FORMAT_R16G16_FLOAT)
	{
		throw std::invalid_argument("Fluid surface requires matching reverse-Z depth and RG16F motion grids");
	}
	_depth = opaqueDepth;
	_motion = opaqueMotion;
	_surface.PrepareFrame(std::move(_instances));
	if (_surface.NeedsSimulation())
	{
		Packet(graph, bindings, "water seed / SPH hash-sort-density-integrate", _surface.GetSimulationBufferAccesses(),
		       [this](auto* commands) { _surface.ExecuteSimulation(commands); },
		       AsyncWater.GetValue() ? Queue::Compute : Queue::Direct);
	}
}
void SceneWater::AppendDraw(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr,
                            D3D12_CPU_DESCRIPTOR_HANDLE depth)
{
	if (!_surface.IsRendering())
	{
		return;
	}
	auto accesses = _surface.NativeSurfaceAccesses();
	accesses.push_back({hdr->GetResource(), GDX12AccessMode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET});
	accesses.push_back({_depth->GetResource(), GDX12AccessMode::ReadWrite, D3D12_RESOURCE_STATE_DEPTH_WRITE});
	Packet(graph, bindings, "water draw / MGPU unlit opaque particles", accesses,
	       [this, hdr, depth](auto* commands) { _surface.DrawParticles(commands, hdr, depth); });
}
} // namespace BistroRender
