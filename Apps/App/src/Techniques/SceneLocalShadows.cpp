#include "SceneLocalShadows.h"
#include "ShadowTexture.h"
#include "ShadowGeometry.h"
#include "Graph/QueueSubmission.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cassert>

namespace BistroRender
{
namespace
{
float Distance(const XMFLOAT4& plane, const XMFLOAT3& point)
{
	return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
}

std::optional<BoundingBox> VisibleBounds(const BoundingBox& box, const std::array<XMFLOAT4, 6>& planes,
                                         const std::array<XMFLOAT3, 8>& cameraCorners)
{
	bool contained = true;
	for (const auto& p : planes)
	{
		const float distance = Distance(p, box.Center);
		const float radius =
		    std::abs(p.x) * box.Extents.x + std::abs(p.y) * box.Extents.y + std::abs(p.z) * box.Extents.z;
		if (distance + radius < 0)
		{
			return std::nullopt;
		}
		contained &= distance >= radius;
	}
	if (contained)
	{
		return box;
	}
	const Vector3 lo = Vector3(box.Center) - Vector3(box.Extents), hi = Vector3(box.Center) + Vector3(box.Extents);
	const std::array<XMFLOAT4, 6> boxPlanes{
	    {{1, 0, 0, -lo.x}, {-1, 0, 0, hi.x}, {0, 1, 0, -lo.y}, {0, -1, 0, hi.y}, {0, 0, 1, -lo.z}, {0, 0, -1, hi.z}}};
	std::array<XMFLOAT3, 8> corners;
	for (UINT i = 0; i < 8; ++i)
	{
		corners[i] = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
	}
	Vector3 minimum(FLT_MAX, FLT_MAX, FLT_MAX), maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	bool found = false;
	const auto clip = [&](const XMFLOAT3& a, const XMFLOAT3& b, const auto& clippingPlanes)
	{
		float first = 0, last = 1;
		for (const auto& plane : clippingPlanes)
		{
			const float da = Distance(plane, a), db = Distance(plane, b);
			if (da < 0 && db < 0)
			{
				return;
			}
			if (da < 0)
			{
				first = (std::max)(first, da / (da - db));
			}
			if (db < 0)
			{
				last = (std::min)(last, da / (da - db));
			}
			if (first > last)
			{
				return;
			}
		}
		for (float t : {first, last})
		{
			const Vector3 point = Vector3(a) + (Vector3(b) - Vector3(a)) * t;
			minimum = Vector3::Min(minimum, point);
			maximum = Vector3::Max(maximum, point);
			found = true;
		}
	};
	// Intersection vertices lie on box edges, camera edges, or their endpoints.
	for (UINT i = 0; i < 8; ++i)
	{
		for (UINT axis = 1; axis <= 4; axis <<= 1)
		{
			if (!(i & axis))
			{
				clip(corners[i], corners[i | axis], planes);
				clip(cameraCorners[i], cameraCorners[i | axis], boxPlanes);
			}
		}
	}
	if (!found)
	{
		return box; // Keep uncertain numerical intersections conservative.
	}
	BoundingBox result;
	BoundingBox::CreateFromPoints(result, minimum, maximum);
	result.Extents = Vector3(result.Extents) + Vector3(0.01f, 0.01f, 0.01f);
	return result;
}
} // namespace

void SceneLocalShadows::ScreenCoverage::Add(const BoundingBox& receiver, const BoundingBox& influence,
                                             const XMFLOAT4X4& camera)
{
	if (Full) return;
	const auto lo = Vector3::Max(Vector3(receiver.Center) - Vector3(receiver.Extents),
	                             Vector3(influence.Center) - Vector3(influence.Extents));
	const auto hi = Vector3::Min(Vector3(receiver.Center) + Vector3(receiver.Extents),
	                             Vector3(influence.Center) + Vector3(influence.Extents));
	if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z) return;
	const auto matrix = XMMatrixTranspose(XMLoadFloat4x4(&camera));
	float left = 1, right = -1, bottom = 1, top = -1;
	for (UINT i = 0; i < 8; ++i)
	{
		XMFLOAT4 clip;
		XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y,
		                                                   i & 4 ? hi.z : lo.z, 1), matrix));
		if (clip.w <= 0 || clip.z <= 0)
		{
			Full = true;
			Rectangles.clear();
			return;
		}
		const float x = std::clamp(clip.x / clip.w, -1.f, 1.f);
		const float y = std::clamp(clip.y / clip.w, -1.f, 1.f);
		left = (std::min)(left, x);
		right = (std::max)(right, x);
		bottom = (std::min)(bottom, y);
		top = (std::max)(top, y);
	}
	if (left < right && bottom < top) Rectangles.push_back({left, right, bottom, top});
}

double SceneLocalShadows::ScreenCoverage::Area() const
{
	if (Full) return 4;
	if (Rectangles.empty()) return 0;
	if (Rectangles.size() == 1)
	{
		const auto& rect = Rectangles.front();
		return (double(rect[1]) - rect[0]) * (double(rect[3]) - rect[2]);
	}
	struct Edge
	{
		float X;
		UINT First, Last;
		int Delta;
	};
	std::vector<float> y;
	y.reserve(2 * Rectangles.size());
	for (const auto& rect : Rectangles)
	{
		y.push_back(rect[2]);
		y.push_back(rect[3]);
	}
	std::sort(y.begin(), y.end());
	y.erase(std::unique(y.begin(), y.end()), y.end());
	std::vector<Edge> events;
	events.reserve(2 * Rectangles.size());
	for (const auto& rect : Rectangles)
	{
		const auto low = UINT(std::lower_bound(y.begin(), y.end(), rect[2]) - y.begin());
		const auto high = UINT(std::lower_bound(y.begin(), y.end(), rect[3]) - y.begin());
		events.push_back({rect[0], low, high, 1});
		events.push_back({rect[1], low, high, -1});
	}
	std::sort(events.begin(), events.end(), [](const Edge& a, const Edge& b) { return a.X < b.X; });
	struct Interval
	{
		int Covers = 0;
		double Length = 0;
	};
	std::vector<Interval> tree(4 * y.size());
	// Sweep in X, counting the union of covered Y intervals between consecutive edges.
	const auto update = [&](auto&& self, UINT node, UINT first, UINT last, const Edge& edge) -> void
	{
		if (edge.First <= first && last <= edge.Last)
			tree[node].Covers += edge.Delta;
		else
		{
			const UINT mid = first + (last - first) / 2;
			if (edge.First < mid) self(self, 2 * node, first, mid, edge);
			if (edge.Last > mid) self(self, 2 * node + 1, mid, last, edge);
		}
		tree[node].Length = tree[node].Covers ? double(y[last]) - y[first] :
		    last == first + 1 ? 0 : tree[2 * node].Length + tree[2 * node + 1].Length;
	};
	double area = 0;
	float previous = events.front().X;
	for (const auto& edge : events)
	{
		area += (double(edge.X) - previous) * tree[1].Length;
		update(update, 1, 0, UINT(y.size() - 1), edge);
		previous = edge.X;
	}
	return area;
}

SceneLocalShadows::SceneLocalShadows(GDX12DeviceResources& r, RenderPipelineCommonData& common, bool staticOnly, bool remote)
    : _resources(r), _common(common), _slots(remote ? 1 : r.FrameConstants.size()), _staticOnly(staticOnly)
{
	_dsvHeap = std::make_unique<GDX12DescriptorHeap>(
	    r.Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
	    UINT(_slots.size() + remote) * MaxLocalShadowAtlasCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	GDX12RootSignatureDesc root;
	root.Constants = {2};
	root.NumSingleSRVSlots = 4;
	root.InstancedGeometry = true;
	root.StaticSamplers = GetStaticSamplers();
	root.SRVRanges.emplace_back(Texture2D_RangeLength);
	_root = std::make_unique<GDX12RootSignature>(r.Device, root);
	_signature = GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device, _root.get());
	auto& compiler = GDX12ShaderCompiler::GetInstance();
	const D3D_SHADER_MACRO defines[] = {{"SHADOW_UNBATCHED", "1"}, {nullptr, nullptr}};
	auto vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", defines, "VS", "vs");
	auto ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", nullptr, "PS", "ps");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
	p.pRootSignature = _root->GetRootSignature().Get();
	p.InputLayout = {r.InputLayouts.at("Default").data(), UINT(r.InputLayouts.at("Default").size())};
	p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	p.RasterizerState.FrontCounterClockwise = TRUE;
	p.RasterizerState.DepthBias = 250;
	p.RasterizerState.SlopeScaledDepthBias = 1.5f;
	p.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	p.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	p.SampleMask = UINT_MAX;
	p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	p.SampleDesc.Count = 1;
	ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_maskedDraw)));
	const D3D_SHADER_MACRO depthDefines[] = {{"SHADOW_UNBATCHED", "1"}, {"SHADOW_DEPTH_ONLY", "1"}, {nullptr, nullptr}};
	auto depthVS = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", depthDefines, "VS", "vs");
	p.VS = {depthVS->GetBufferPointer(), depthVS->GetBufferSize()};
	p.PS = {};
	ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_opaqueDraw)));
	for (auto& frame : _slots)
	{
		frame.Cull.Owner = this;
		frame.Cull.Frame = &frame;
	}
	if (remote) return;
	r.IndirectCommandsCache->TrackElementChanges();
	for (auto& frame : r.FrameConstants)
	{
		frame->InstanceCache->TrackElementChanges();
		frame->TransformCache->TrackElementChanges();
		frame->MaterialCache->TrackElementChanges();
	}
}

SceneLocalShadows::~SceneLocalShadows()
{
	for (auto& frame : _slots)
	{
		try
		{
			frame.Ready.Complete();
		}
		catch (...)
		{
		}
	}
}

UINT SceneLocalShadows::BuildNode(UINT first, UINT count, UINT parent)
{
	const UINT index = UINT(_nodes.size());
	_nodes.emplace_back();
	auto bounds = _casters[first].Bounds;
	for (UINT i = first + 1; i < first + count; ++i)
	{
		BoundingBox::CreateMerged(bounds, bounds, _casters[i].Bounds);
	}
	if (count <= 32)
	{
		_nodes[index] = {bounds, first, count, 0, 0, parent};
		for (UINT i = first; i < first + count; ++i)
		{
			_leaves[i] = index;
		}
	}
	else
	{
		const auto& e = bounds.Extents;
		const UINT axis = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
		const UINT half = count / 2;
		const auto coordinate = [axis](const Caster& value)
		{
			return axis == 0 ? value.Bounds.Center.x : axis == 1 ? value.Bounds.Center.y : value.Bounds.Center.z;
		};
		std::nth_element(_casters.begin() + first, _casters.begin() + first + half, _casters.begin() + first + count,
		                 [&](const auto& a, const auto& b) { return coordinate(a) < coordinate(b); });
		const UINT left = BuildNode(first, half, index), right = BuildNode(first + half, count - half, index);
		_nodes[index] = {bounds, 0, 0, left, right, parent};
	}
	return index;
}

SceneLocalShadows::Input SceneLocalShadows::ReadInput(UINT slot, UINT commandIndex) const
{
	const auto& frame = *_resources.FrameConstants.at(slot);
	Input result;
	result.Command = _resources.IndirectCommandsCache->GetCPUData()[commandIndex];
	if (!result.Command.InstanceCount)
	{
		return result;
	}
	if (result.Command.InstanceID >= frame.InstanceCache->GetElementCount())
	{
		throw std::out_of_range("Local shadow command references a missing instance");
	}
	result.Instance = frame.InstanceCache->GetCPUData()[result.Command.InstanceID];
	if (result.Instance.MaterialIndex >= frame.MaterialCache->GetElementCount() ||
	    result.Instance.TransformIndex >= frame.TransformCache->GetElementCount())
	{
		throw std::out_of_range("Local shadow instance references missing scene data");
	}
	result.Material = frame.MaterialCache->GetCPUData()[result.Instance.MaterialIndex];
	result.World = frame.TransformCache->GetCPUData()[result.Instance.TransformIndex].WorldMatrix;
	result.Dynamic = result.Instance.ReflectionDynamic != 0 || result.Instance.SkinOutputVertex != UINT_MAX;
	if (const auto* primitive = _resources.GetMeshPrimitive(result.Command.InstanceID))
	{
		result.DeformationRevision = primitive->DeformationRevision;
		result.Dynamic |= primitive->Dynamic;
	}
	return result;
}

void SceneLocalShadows::Refit(UINT caster)
{
	for (UINT index = _leaves[caster]; index != UINT_MAX; index = _nodes[index].Parent)
	{
		auto& node = _nodes[index];
		if (node.Count)
		{
			node.Bounds = _casters[node.First].Bounds;
			for (UINT i = node.First + 1; i < node.First + node.Count; ++i)
			{
				BoundingBox::CreateMerged(node.Bounds, node.Bounds, _casters[i].Bounds);
			}
		}
		else
		{
			BoundingBox::CreateMerged(node.Bounds, _nodes[node.Left].Bounds, _nodes[node.Right].Bounds);
		}
	}
}

void SceneLocalShadows::Invalidate(const Caster& caster, bool listsChanged)
{
	if (!caster.CastsShadow)
	{
		return;
	}
	_changedCasterBounds.push_back(caster.Bounds);
	InvalidateBounds(caster.Bounds, listsChanged && caster.Dynamic != _staticOnly);
}

void SceneLocalShadows::InvalidateBounds(const BoundingBox& bounds, bool listsChanged)
{
	for (auto& slot : _slots)
	{
		for (auto& cache : slot.Cache)
		{
			const UINT valid = cache.MapsValid | (listsChanged ? cache.ListsValid : 0);
			if (!valid || !LocalShadowRangeIntersects(cache.Light, bounds)) continue;
			for (UINT face = 0; face < LocalShadowFaceCount(cache.Light); ++face)
			{
				const UINT bit = 1u << face;
				if ((valid & bit) && cache.Frusta.value()[face].Intersects(bounds))
				{
					if (listsChanged) cache.ListsValid &= ~bit;
					cache.MapsValid &= ~bit;
				}
			}
		}
	}
}

bool SceneLocalShadows::InvalidateGrass(const GDX12RenderPrimitive& grass)
{
	if (!grass.Visible || !grass.CastsShadows || !grass.InstanceCount)
	{
		return false;
	}
	InvalidateBounds(grass.Bounds, false);
	return true;
}

void SceneLocalShadows::UpdateGrass(const std::vector<GDX12RenderPrimitive>& grass)
{
	bool receiversChanged = _grass.size() != grass.size();
	bool castersChanged = false;
	for (size_t i = 0; i < (std::max)(_grass.size(), grass.size()); ++i)
	{
		const auto* before = i < _grass.size() ? &_grass[i] : nullptr;
		const auto* after = i < grass.size() ? &grass[i] : nullptr;
		if (before && after && before->Revision() == after->Revision())
		{
			continue;
		}
		if (before)
		{
			castersChanged |= InvalidateGrass(*before);
		}
		if (after)
		{
			castersChanged |= InvalidateGrass(*after);
		}
		receiversChanged |= !before || !after || before->Visible != after->Visible ||
		                    before->InstanceCount != after->InstanceCount ||
		                    std::memcmp(&before->Bounds, &after->Bounds, sizeof(BoundingBox)) != 0;
	}
	_grass = grass;
	if (receiversChanged)
	{
		++_grassReceiverRevision;
	}
	if (castersChanged)
	{
		++_grassCasterRevision;
	}
}

void SceneLocalShadows::UpdateCaster(UINT position, const Input& current)
{
	auto& caster = _casters[position];
	const BoundingBox bounds(current.Instance.BoundingBoxCenter, current.Instance.BoundingBoxExtents);
	const bool boundsChanged = std::memcmp(&caster.Bounds, &bounds, sizeof(bounds)) != 0;
	const bool castsShadow = current.Instance.CastsShadows != 0 && current.Material.RenderLayer != 2;
	const bool masked = current.Material.RenderLayer == 1;
	const bool listsChanged = boundsChanged || caster.CastsShadow != castsShadow || caster.Masked != masked ||
	                          std::memcmp(&caster.Command, &current.Command, sizeof(current.Command)) != 0;
	if (boundsChanged || caster.CastsShadow != castsShadow)
	{
		Invalidate(caster, listsChanged);
	}
	caster.Bounds = bounds;
	caster.Command = current.Command;
	caster.CastsShadow = castsShadow;
	caster.Masked = masked;
	Invalidate(caster, listsChanged);
	if (boundsChanged)
	{
		Refit(position);
	}
}

void SceneLocalShadows::UpdateCasters(UINT slot)
{
	_changedCasterBounds.clear();
	_castersRebuilt = false;
	auto& frame = *_resources.FrameConstants.at(slot);
	const auto commands = (_staticOnly ? _resources.IndirectCommandsCache->ChangedElements()
	                            : _resources.IndirectCommandsCache->ConsumeChangedElements());
	const auto instances =
	    (_staticOnly ? frame.InstanceCache->ChangedElements() : frame.InstanceCache->ConsumeChangedElements());
	const auto transforms =
	    (_staticOnly ? frame.TransformCache->ChangedElements() : frame.TransformCache->ConsumeChangedElements());
	const auto materials =
	    (_staticOnly ? frame.MaterialCache->ChangedElements() : frame.MaterialCache->ConsumeChangedElements());
	if (!_treeReady || _inputs.size() != _resources.IndirectCommandsCache->GetElementCount() ||
	    _geometryRevision != _resources.GeometryRevision ||
	    _meshRevision != _resources.GeometryBuffer->GetMeshCacheRevision())
	{
		RebuildCasters(slot);
		return;
	}
	// Resource rebinding is rare; no texture-registry scan on ordinary frames.
	if (_textureRevision != _resources.TextureRevision)
	{
		for (auto& cachedSlot : _slots)
		{
			for (auto& cache : cachedSlot.Cache)
			{
				cache.MapsValid = 0;
			}
		}
		_textureRevision = _resources.TextureRevision;
		++_sceneRevision;
		_castersRebuilt = true;
	}
	if (++_changeGeneration == 0)
	{
		_commandGeneration.clear();
		_materialGeneration.clear();
		++_changeGeneration;
	}
	_commandGeneration.resize(_inputs.size());
	_materialGeneration.resize(frame.MaterialCache->GetElementCount());
	_changedCommands.clear();
	const auto markCommand = [&](UINT index)
	{
		auto& generation = _commandGeneration.at(index);
		if (generation == _changeGeneration) return;
		generation = _changeGeneration;
		_changedCommands.push_back(index);
	};
	const auto append = [&](const auto& indices, const auto& references)
	{
		for (UINT index : indices)
		{
			if (const auto found = references.find(index); found != references.end())
				for (UINT command : found->second) markCommand(command);
		}
	};
	for (UINT index : commands) markCommand(index);
	for (UINT material : materials) _materialGeneration.at(material) = _changeGeneration;
	append(instances, _instances);
	append(transforms, _transforms);
	append(materials, _materials);
	for (UINT index : _changedCommands)
	{
		const auto current = ReadInput(slot, index);
		auto& old = _inputs.at(index);
		const auto included = [](const Input& input)
		{ return input.Command.InstanceCount && !input.Material.ReflectionProbePreview; };
		// Membership or mobility changes rebuild the trees; ordinary bounds edits only refit ancestors.
		if (included(current) != included(old) || current.Dynamic != old.Dynamic)
		{
			RebuildCasters(slot);
			return;
		}
		if (_staticOnly && current.Dynamic)
		{
			old = current;
			continue;
		}
		const bool materialDirty =
		    current.Instance.MaterialIndex < _materialGeneration.size() &&
		    _materialGeneration[current.Instance.MaterialIndex] == _changeGeneration;
		if (!materialDirty && current.DeformationRevision == old.DeformationRevision &&
		    std::memcmp(&current.Command, &old.Command, sizeof(current.Command)) == 0 &&
		    std::memcmp(&current.Instance, &old.Instance, sizeof(current.Instance)) == 0 &&
		    std::memcmp(&current.Material, &old.Material, sizeof(current.Material)) == 0 &&
		    std::memcmp(&current.World, &old.World, sizeof(current.World)) == 0)
		{
			continue;
		}
		const auto rebind = [index](auto& references, UINT previous, UINT next)
		{
			if (previous == next)
			{
				return;
			}
			if (auto found = references.find(previous); found != references.end())
			{
				auto& commands = found->second;
				commands.erase(std::remove(commands.begin(), commands.end(), index), commands.end());
				if (commands.empty())
				{
					references.erase(found);
				}
			}
			references[next].push_back(index);
		};
		rebind(_instances, old.Command.InstanceID, current.Command.InstanceID);
		if (current.Command.InstanceCount && old.Command.InstanceCount)
		{
			rebind(_transforms, old.Instance.TransformIndex, current.Instance.TransformIndex);
			rebind(_materials, old.Instance.MaterialIndex, current.Instance.MaterialIndex);
		}
		else if (current.Command.InstanceCount != old.Command.InstanceCount)
		{
			RebuildCasters(slot);
			return;
		}
		old = current;
		const UINT position = _positions[index];
		if (position == UINT_MAX)
		{
			continue;
		}
		UpdateCaster(position, current);
		++_sceneRevision;
	}
}

void SceneLocalShadows::RebuildCasters(UINT slot)
{
	_castersRebuilt = true;
	_casters.clear();
	_nodes.clear();
	_instances.clear();
	_transforms.clear();
	_materials.clear();
	_inputs.resize(_resources.IndirectCommandsCache->GetElementCount());
	_positions.assign(_inputs.size(), UINT_MAX);
	for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
	{
		const auto& input = _inputs[i] = ReadInput(slot, i);
		const auto& command = input.Command;
		_instances[command.InstanceID].push_back(i);
		if (!command.InstanceCount)
		{
			continue;
		}
		const auto& instance = input.Instance;
		_transforms[instance.TransformIndex].push_back(i);
		_materials[instance.MaterialIndex].push_back(i);
		const auto& material = input.Material;
		if (material.ReflectionProbePreview)
		{
			continue;
		}
		// Blended bounds receive shadows, but never enter opaque/masked caster lists.
		_casters.push_back({BoundingBox(instance.BoundingBoxCenter, instance.BoundingBoxExtents), command,
		                    instance.CastsShadows != 0 && material.RenderLayer != 2, material.RenderLayer == 1, i,
		                    input.Dynamic});
	}
	_leaves.resize(_casters.size());
	const auto split =
	    std::partition(_casters.begin(), _casters.end(), [](const Caster& caster) { return !caster.Dynamic; });
	const UINT staticCount = UINT(split - _casters.begin());
	_staticRoot = staticCount ? BuildNode(0, staticCount) : UINT_MAX;
	_dynamicRoot =
	    staticCount < _casters.size() ? BuildNode(staticCount, UINT(_casters.size()) - staticCount) : UINT_MAX;
	for (UINT i = 0; i < _casters.size(); ++i)
	{
		_positions[_casters[i].InputIndex] = i;
	}
	for (auto& frame : _slots)
	{
		for (auto& cache : frame.Cache)
		{
			cache.ListsValid = cache.MapsValid = cache.LocalFallback = 0;
		}
	}
	_treeReady = true;
	++_sceneRevision;
	_geometryRevision = _resources.GeometryRevision;
	_meshRevision = _resources.GeometryBuffer->GetMeshCacheRevision();
	_textureRevision = _resources.TextureRevision;
}

void SceneLocalShadows::Gather(UINT index, const GDX12LocalLightConstants& light,
                               const std::array<GDX12LocalShadowFrustum, 6>& faces, UINT faceMask,
                               std::array<FaceCommands, 6>& output) const
{
	const auto& node = _nodes[index];
	if (!LocalShadowRangeIntersects(light, node.Bounds))
	{
		return;
	}
	for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
	{
		if ((faceMask & (1u << face)) && !faces[face].Intersects(node.Bounds))
		{
			faceMask &= ~(1u << face);
		}
	}
	if (!faceMask)
	{
		return;
	}
	if (!node.Count)
	{
		Gather(node.Left, light, faces, faceMask, output);
		Gather(node.Right, light, faces, faceMask, output);
		return;
	}
	for (UINT i = node.First; i < node.First + node.Count; ++i)
	{
		const auto& caster = _casters[i];
		if (!caster.CastsShadow || !LocalShadowRangeIntersects(light, caster.Bounds))
		{
			continue;
		}
		for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
		{
			if ((faceMask & (1u << face)) && faces[face].Intersects(caster.Bounds))
			{
				(caster.Masked ? output[face].Masked : output[face].Opaque).push_back(caster.Command);
			}
		}
	}
}

UINT SceneLocalShadows::NeededFaces(UINT index, const GDX12LocalLightConstants& light,
                                    const std::array<GDX12LocalShadowFrustum, 6>& faces, UINT faceMask,
                                    const Slot& frame, const BoundingBox& influence, ScreenCoverage& coverage) const
{
	const auto& node = _nodes[index];
	if (!frame.Camera->Intersects(node.Bounds) || !LocalShadowRangeIntersects(light, node.Bounds))
		return 0;
	if (!faceMask && coverage.Full)
		return 0;
	if (!node.Count)
	{
		const UINT left = NeededFaces(node.Left, light, faces, faceMask, frame, influence, coverage);
		return left | NeededFaces(node.Right, light, faces, faceMask & ~left, frame, influence, coverage);
	}
	UINT needed = 0;
	const float margin = light.Range / 16 * 0.3f + 0.01f;
	for (UINT i = node.First; i < node.First + node.Count; ++i)
	{
		if (const auto visible = VisibleBounds(_casters[i].Bounds, frame.CameraPlanes, frame.CameraCorners);
		    visible && LocalShadowRangeIntersects(light, *visible))
		{
			auto bounds = *visible;
			bounds.Extents = Vector3(bounds.Extents) + Vector3(margin, margin, margin);
			if (light.Type == 2 && !faces[0].Intersects(bounds)) continue;
			for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
				if ((faceMask & (1u << face)) && faces[face].Intersects(bounds))
					needed |= 1u << face;
			coverage.Add(*visible, influence, frame.CameraMatrix);
		}
	}
	return needed;
}

void SceneLocalShadows::CullJob::Execute(size_t index)
{
	const auto& light = Frame->Lights[index];
	auto& output = Frame->Cache[index];
	if (!Frame->SelectionChanged && !output.SelectionDirty) return;
	output.SelectionDirty = false;
	output.Needed = 0;
	output.ScreenPixels = 0;
	if (!(light.ShadowFlags & ShadowEnabled) || (light.Type != 1 && light.Type != 2) || !Frame->Ceiling)
		return;
	const auto& faces = output.Frusta.value();
	const UINT allFaces = (1u << LocalShadowFaceCount(light)) - 1;
	const BoundingBox influence(light.Position, XMFLOAT3(light.Range, light.Range, light.Range));
	if (!Frame->Camera->Intersects(influence)) return;
	ScreenCoverage coverage;
	for (const UINT root : {Owner->_staticRoot, Owner->_dynamicRoot})
		if (root != UINT_MAX)
			output.Needed |= Owner->NeededFaces(root, light, faces, allFaces & ~output.Needed, *Frame, influence,
			                                    coverage);
	const float margin = light.Range / 16 * 0.3f + 0.01f;
	for (const auto& grass : Owner->_grass)
	{
		if (!grass.Visible || !grass.InstanceCount) continue;
		const auto visible = VisibleBounds(grass.Bounds, Frame->CameraPlanes, Frame->CameraCorners);
		if (!visible || !LocalShadowRangeIntersects(light, *visible)) continue;
		auto bounds = *visible;
		bounds.Extents = Vector3(bounds.Extents) + Vector3(margin, margin, margin);
		if (light.Type == 2 && !faces[0].Intersects(bounds)) continue;
		for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
			if (faces[face].Intersects(bounds)) output.Needed |= 1u << face;
		coverage.Add(*visible, influence, Frame->CameraMatrix);
	}
	output.ScreenPixels = float(coverage.Area() * Frame->Width * Frame->Height / 4);
}

void SceneLocalShadows::GatherMissing(Slot& frame, UINT index, UINT faces)
{
	auto& cached = frame.Cache[index];
	const UINT missing = faces & ~cached.ListsValid;
	for (UINT face = 0; face < 6; ++face)
	{
		if (!(missing & (1u << face))) continue;
		cached.Faces[face].Opaque.clear();
		cached.Faces[face].Masked.clear();
	}
	const UINT root = _staticOnly ? _staticRoot : _dynamicRoot;
	if (missing && root != UINT_MAX)
		Gather(root, frame.Lights[index], cached.Frusta.value(), missing, cached.Faces);
	cached.ListsValid |= missing;
}

void SceneLocalShadows::PackView(Slot& target, const FaceCommands& commands, UINT light, UINT face,
                                GDX12LocalShadowTile tile)
{
	const size_t opaqueCount = commands.Opaque.size(), maskedCount = commands.Masked.size();
	if (target.Packed.size() + opaqueCount + maskedCount > UINT_MAX / sizeof(GDX12IndirectDrawArgs))
		throw std::length_error("Local shadow indirect buffer exceeds the buffer size limit");
	target.Views.push_back({light, face, UINT(target.Packed.size()), UINT(opaqueCount), UINT(maskedCount), tile});
	target.Packed.insert(target.Packed.end(), commands.Opaque.begin(), commands.Opaque.end());
	target.Packed.insert(target.Packed.end(), commands.Masked.begin(), commands.Masked.end());
}

void SceneLocalShadows::Pack(Slot& frame)
{
	frame.Views.clear();
	frame.RemoteViews.clear();
	frame.Packed.clear();
	for (UINT lightIndex = 0; lightIndex < frame.Lights.size(); ++lightIndex)
	{
		auto& light = frame.Lights[lightIndex];
		auto& cache = frame.Cache[lightIndex];
		light.ShadowFirstSlice = cache.ReadyFaces ? 0 : UINT_MAX;
		light.ShadowFlags |= LocalShadowFaceMask;
		light._padding[2] = cache.ReadyFaces;
		std::copy(cache.Tiles.begin(), cache.Tiles.end(), light.ShadowTiles);
		GatherMissing(frame, lightIndex, cache.ReadyFaces);
		const bool everyFrame = !_staticOnly && (light.ShadowFlags & ShadowEveryFrame);
		const UINT dirty = cache.ReadyFaces & (everyFrame ? UINT_MAX : (~cache.MapsValid | cache.LocalFallback));
		for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
		{
			if (!(dirty & (1u << face))) continue;
			PackView(frame, cache.Faces[face], lightIndex, face, cache.Tiles[face]);
		}
	}
}

void SceneLocalShadows::UpdateCamera(Slot& frame)
{
	XMFLOAT4X4 viewProjection;
	// Cull and estimate coverage against one frustum covering the full +/- half-pixel jitter.
	// CameraViewToClip is the stable projection before per-frame jitter is applied.
	const auto guard = Matrix::CreateScale(float(frame.Width) / (float(frame.Width) + 1.0f),
	                                      float(frame.Height) / (float(frame.Height) + 1.0f), 1.0f);
	const auto viewProjectionMatrix = _common.CameraView * _common.CameraViewToClip * guard;
	XMStoreFloat4x4(&viewProjection, XMMatrixTranspose(viewProjectionMatrix));
	if (frame.Camera && std::memcmp(&frame.CameraMatrix, &viewProjection, sizeof(viewProjection)) == 0)
	{
		return;
	}
	frame.SelectionChanged = true;
	frame.CameraMatrix = viewProjection;
	frame.Camera.emplace(viewProjection);
	const auto matrix = XMLoadFloat4x4(&viewProjection);
	const XMVECTOR planes[] = {matrix.r[3] + matrix.r[0],
	                           matrix.r[3] - matrix.r[0],
	                           matrix.r[3] + matrix.r[1],
	                           matrix.r[3] - matrix.r[1],
	                           matrix.r[2],
	                           matrix.r[3] - matrix.r[2]};
	for (UINT i = 0; i < 6; ++i)
	{
		XMStoreFloat4(&frame.CameraPlanes[i], planes[i]);
	}
	const auto inverse = XMMatrixInverse(nullptr, viewProjectionMatrix);
	for (UINT i = 0; i < 8; ++i)
	{
		XMStoreFloat3(&frame.CameraCorners[i],
		              XMVector3TransformCoord(XMVectorSet(i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : 0.f, 1),
		                                      inverse));
	}
}

void SceneLocalShadows::UpdateLights(Slot& frame)
{
	frame.Lights = _resources.LocalLights;
	for (const auto& light : frame.Lights)
	{
		if (!std::isfinite(light.Position.x) || !std::isfinite(light.Position.y) || !std::isfinite(light.Position.z) ||
		    !std::isfinite(light.Range) || light.Range <= 0)
		{
			throw std::invalid_argument("Invalid local shadow light bounds");
		}
	}
	bool sameLights = frame.Cache.size() == frame.Lights.size();
	for (UINT i = 0; sameLights && i < frame.Lights.size(); ++i)
	{
		sameLights = frame.Cache[i].HasLight && frame.Cache[i].Light.Type == frame.Lights[i].Type &&
		             GetLocalShadowKey(frame.Cache[i].Light) == GetLocalShadowKey(frame.Lights[i]);
	}
	if (!sameLights)
	{
		frame.Cache.clear();
		frame.Cache.resize(frame.Lights.size());
		frame.SelectionChanged = true;
	}
	for (UINT i = 0; i < frame.Lights.size(); ++i)
	{
		auto source = frame.Lights[i];
		source.ShadowFlags &= ~ShadowEveryFrame;
		source.ShadowFirstSlice = 0;
		auto& cached = frame.Cache[i];
		if (ShouldRenderLocalShadow(source, cached.HasLight ? &cached.Light : nullptr) ||
		    (source.ShadowFlags & ShadowEnabled) != (cached.Light.ShadowFlags & ShadowEnabled))
		{
			cached.ListsValid = cached.MapsValid = 0;
			cached.PublicationOrders = {};
			cached.SelectionDirty = true;
			cached.Frusta = std::array<GDX12LocalShadowFrustum, 6>{
			    GDX12LocalShadowFrustum(source.ShadowViewProj[0]), GDX12LocalShadowFrustum(source.ShadowViewProj[1]),
			    GDX12LocalShadowFrustum(source.ShadowViewProj[2]), GDX12LocalShadowFrustum(source.ShadowViewProj[3]),
			    GDX12LocalShadowFrustum(source.ShadowViewProj[4]), GDX12LocalShadowFrustum(source.ShadowViewProj[5])};
		}
		cached.Light = source;
		cached.HasLight = true;
	}
}

void SceneLocalShadows::Freeze(UINT slot, const std::vector<GDX12RenderPrimitive>& grass, UINT width, UINT height)
{
	if (!_common.Jobs) throw std::logic_error("Local shadow preparation requires JobSystem");
	UpdateCasters(slot);
	UpdateGrass(grass);
	auto& frame = _slots.at(slot);
	const UINT ceiling = _resources.ShadowQuality.LocalResolution;
	frame.SelectionChanged = !frame.Camera || frame.SceneRevision != _sceneRevision ||
	                         frame.GrassReceiverRevision != _grassReceiverRevision ||
	                         frame.Ceiling != ceiling || frame.Width != width || frame.Height != height;
	frame.Ceiling = ceiling;
	frame.Width = width;
	frame.Height = height;
	frame.SceneRevision = _sceneRevision;
	frame.GrassReceiverRevision = _grassReceiverRevision;
	frame.GrassCasterRevision = _grassCasterRevision;
	UpdateLights(frame);
	if (!_staticOnly)
	{
		UpdateCamera(frame);
		frame.Ready = frame.Cull.Schedule(*_common.Jobs, frame.Lights.size(), 1);
	}
}

void SceneLocalShadows::Complete(UINT slot)
{
	auto& frame = _slots.at(slot);
	frame.Ready.Complete();
	const UINT lightCount = (std::max)(1u, UINT(frame.Lights.size()));
	if (!frame.LightUpload)
	{
		frame.LightUpload = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(_resources.Device, lightCount,
		                                                                                  EBufferType::Upload, false);
		frame.LightUpload->TrackChanges();
	}
	frame.LightUpload->Resize(lightCount);
	if (frame.Lights.empty())
	{
		frame.LightUpload->CopyData(0, GDX12LocalLightConstants{});
	}
	else
	{
		frame.LightUpload->CopyData(0, frame.Lights.data(), UINT(frame.Lights.size()));
	}
	const UINT commandCount = (std::max)(1u, UINT(frame.Packed.size()));
	if (!frame.CommandUpload)
	{
		frame.CommandUpload = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(
		    _resources.Device, commandCount, EBufferType::Upload, false);
		frame.Commands = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources.Device, commandCount,
		                                                                            EBufferType::Default, false);
	}
	frame.CommandUpload->Resize(commandCount);
	frame.Commands->Resize(commandCount);
	if (!frame.Packed.empty())
	{
		frame.CommandUpload->CopyData(0, frame.Packed.data(), UINT(frame.Packed.size()));
	}
}

void SceneLocalShadows::Submitted(UINT slot)
{
	auto& frame = _slots.at(slot);
	for (const auto& view : frame.Views)
	{
		auto& cache = frame.Cache[view.Light];
		cache.MapsValid |= 1u << view.Face;
		cache.ResidentFaces |= 1u << view.Face;
		cache.RemoteFaces &= ~(1u << view.Face);
	}
	for (const auto& view : frame.RemoteViews)
	{
		auto& cache = frame.Cache[view.Light];
		cache.MapsValid |= 1u << view.Face;
		cache.ResidentFaces |= 1u << view.Face;
		cache.LocalFallback &= ~(1u << view.Face);
	}
}

namespace
{
class FreeShadowPages
{
public:
	FreeShadowPages(UINT side, UINT pages) : _free(pages, {{0, 0, LONG(side), LONG(side)}}) {}

	bool Reserve(const GDX12LocalShadowTile& tile)
	{
		auto& page = _free.at(tile.Page);
		const D3D12_RECT used{LONG(tile.X), LONG(tile.Y), LONG(tile.X + tile.Side), LONG(tile.Y + tile.Side)};
		const auto contains = [](const D3D12_RECT& outer, const D3D12_RECT& inner)
		{
			return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
			    outer.bottom >= inner.bottom;
		};
		if (std::none_of(page.begin(), page.end(), [&](const auto& free) { return contains(free, used); }))
			return false;
		std::vector<D3D12_RECT> pieces;
		for (const auto& free : page)
		{
			if (used.right <= free.left || used.left >= free.right || used.bottom <= free.top || used.top >= free.bottom)
			{
				pieces.push_back(free);
				continue;
			}
			if (used.left > free.left) pieces.push_back({free.left, free.top, used.left, free.bottom});
			if (used.right < free.right) pieces.push_back({used.right, free.top, free.right, free.bottom});
			if (used.top > free.top) pieces.push_back({free.left, free.top, free.right, used.top});
			if (used.bottom < free.bottom) pieces.push_back({free.left, used.bottom, free.right, free.bottom});
		}
		page.clear();
		for (size_t i = 0; i < pieces.size(); ++i)
		{
			bool contained = false;
			for (size_t j = 0; j < pieces.size() && !contained; ++j)
				contained = i != j && contains(pieces[j], pieces[i]) &&
				    (j < i || !contains(pieces[i], pieces[j]));
			if (!contained) page.push_back(pieces[i]);
		}
		return true;
	}

	bool Place(UINT side, GDX12LocalShadowTile& tile)
	{
		for (UINT page = 0; page < _free.size(); ++page)
		{
			const D3D12_RECT* first = nullptr;
			for (const auto& rect : _free[page])
				if (rect.right - rect.left >= LONG(side) && rect.bottom - rect.top >= LONG(side) &&
				    (!first || std::tie(rect.top, rect.left) < std::tie(first->top, first->left)))
					first = &rect;
			if (!first) continue;
			tile = {page, UINT(first->left), UINT(first->top), side};
			return Reserve(tile);
		}
		return false;
	}

private:
	std::vector<std::vector<D3D12_RECT>> _free;
};
}

void SceneLocalShadows::UpdateLayout(const Slot& frame, UINT effectiveCeiling)
{
	std::vector<UINT> tiers{0};
	for (UINT side = 16; side < effectiveCeiling; side *= 2) tiers.push_back(side);
	if (effectiveCeiling) tiers.push_back(effectiveCeiling);

	auto& requests = _placementRequests;
	requests.clear();
	requests.reserve(frame.Cache.size());
	for (UINT i = 0; i < frame.Cache.size(); ++i)
	{
		const auto& cache = frame.Cache[i];
		if (!effectiveCeiling || !(cache.Light.ShadowFlags & ShadowEnabled))
			continue;
		const auto id = GetLocalShadowKey(cache.Light);
		const auto key = std::make_pair(id.EntityId, id.Generation);
		const auto old = _previousPlacement.find(key);
		const UINT previous = old == _previousPlacement.end() ? 0 : old->second.Resolution;
		if (!cache.Needed && !previous) continue;
		const float detail = std::sqrt(cache.ScreenPixels);
		UINT desired = 1;
		for (UINT tier = 2; tier < tiers.size(); ++tier)
		{
			const float threshold = 0.5f * (tiers[tier - 1] + tiers[tier]) * (previous >= tiers[tier] ? 0.8f : 1.2f);
			if (detail < threshold) break;
			desired = tier;
		}
		if (!cache.Needed)
			desired = UINT(std::lower_bound(tiers.begin(), tiers.end(), previous) - tiers.begin());
		requests.push_back({i, LocalShadowFaceCount(cache.Light), desired, cache.Needed ? cache.ScreenPixels : -1.f, key});
	}
	const UINT pages = frame.Depth ? frame.Depth->Pages : 0;
	const UINT pageSide = frame.Depth ? frame.Depth->Side : 0;
	auto& layout = _layout;
	if (layout.Ceiling == effectiveCeiling && layout.Side == pageSide && layout.Pages == pages &&
	    layout.Placements.size() == frame.Cache.size() && layout.Requests == requests)
		return;

	layout.Ceiling = effectiveCeiling;
	layout.Side = pageSide;
	layout.Pages = pages;
	layout.Requests = requests;
	std::sort(requests.begin(), requests.end(), [](const PlacementRequest& a, const PlacementRequest& b)
	{
		return a.Pixels != b.Pixels ? a.Pixels > b.Pixels : a.Key < b.Key;
	});

	UINT64 freeTexels = UINT64(pageSide) * pageSide * pages;
	const auto area = [](const PlacementRequest& request, UINT side) { return UINT64(side) * side * request.Faces; };
	const auto coverageCost = [&](UINT side)
	{
		UINT64 cost = 0;
		for (const auto& request : requests)
			if (frame.Cache[request.Index].Needed)
				cost += area(request, (std::min)(tiers[request.Desired], side));
		return cost;
	};
	// Leave room for visible lights before spending the remaining texels on detail.
	// Otherwise a few full-page groups turn the later lights into unoccluded fill.
	UINT coverageSide = GetLocalShadowProfile(GDX12LocalShadowLevel::Low).Face;
	UINT64 pendingCoverage = coverageCost(coverageSide);
	while (coverageSide > 16 && pendingCoverage > freeTexels)
	{
		coverageSide /= 2;
		pendingCoverage = coverageCost(coverageSide);
	}
	FreeShadowPages free(pageSide, pages);
	layout.OccupiedTexels.assign(pages, 0);
	layout.Placed = layout.Downgraded = layout.Skipped = 0;
	layout.Placements.assign(frame.Cache.size(), {});
	decltype(_previousPlacement) placements;
	for (const auto& request : requests)
	{
		const auto previous = _previousPlacement.find(request.Key);
		const bool needed = frame.Cache[request.Index].Needed != 0;
		const UINT coverage = (std::min)(tiers[request.Desired], coverageSide);
		if (needed) pendingCoverage -= area(request, coverage);
		auto& placed = layout.Placements[request.Index];
		for (UINT tier = request.Desired; tier; --tier)
		{
			const UINT side = tiers[tier];
			const UINT64 texels = area(request, side);
			if (texels > freeTexels || (needed && side > coverage && pendingCoverage > freeTexels - texels))
				continue;
			const auto retain = [&](const auto& tiles)
			{
				if (tiles[0].Side != side) return false;
				auto trial = free;
				for (UINT face = 0; face < request.Faces; ++face)
					if (!trial.Reserve(tiles[face])) return false;
				free = std::move(trial);
				placed.Tiles = tiles;
				return true;
			};
			// All frame slots use the same placement history. A slot's depth cache
			// can retain individual faces when it catches up to that layout.
			bool fits = previous != _previousPlacement.end() && retain(previous->second.Tiles);
			if (!fits && needed)
			{
				auto trial = free;
				fits = true;
				for (UINT face = 0; face < request.Faces; ++face)
					if (!trial.Place(side, placed.Tiles[face]))
					{
						fits = false;
						break;
					}
				if (fits) free = std::move(trial);
				else placed.Tiles = {};
			}
			if (fits)
			{
				freeTexels -= texels;
				placed.Resolution = side;
				placed.Generation = previous != _previousPlacement.end() && previous->second.Tiles == placed.Tiles
				    ? previous->second.Generation : ++_generation;
				placements.emplace(request.Key, placed);
				for (UINT face = 0; face < request.Faces; ++face)
					layout.OccupiedTexels[placed.Tiles[face].Page] += UINT64(side) * side;
				if (needed)
				{
					++layout.Placed;
					if (tier != request.Desired) ++layout.Downgraded;
				}
				break;
			}
			if (!needed) break;
		}
		if (needed && !placed.Resolution) ++layout.Skipped;
	}
	_previousPlacement.swap(placements);
}

void SceneLocalShadows::Allocate(UINT slot, SceneLocalShadows& staticPass, UINT effectiveCeiling,
                                const std::vector<ShadowCrossSync>& pageSync)
{
	auto& frame = _slots.at(slot);
	auto& statics = staticPass._slots.at(slot);
	frame.Ready.Complete();
	UpdateLayout(frame, effectiveCeiling);
	frame.OccupiedTexels = _layout.OccupiedTexels;
	frame.Placed = _layout.Placed;
	frame.Downgraded = _layout.Downgraded;
	frame.Skipped = _layout.Skipped;
	for (UINT i = 0; i < frame.Cache.size(); ++i)
	{
		for (auto* pass : {&frame, &statics})
		{
			auto& cache = pass->Cache[i];
			const auto& placed = _layout.Placements[i];
			if (cache.Tiles != placed.Tiles)
			{
				for (UINT face = 0; face < cache.Tiles.size(); ++face)
					if (cache.Tiles[face] != placed.Tiles[face])
					{
						const UINT mask = ~(1u << face);
						cache.MapsValid &= mask;
						cache.ResidentFaces &= mask;
						cache.LocalFallback &= mask;
					cache.RemoteFaces &= mask;
						cache.ContentVersions[face] = 0;
						cache.PublicationOrders[face] = 0;
					}
				cache.Tiles = placed.Tiles;
			}
			cache.Resolution = placed.Resolution;
			cache.Generation = placed.Generation;
		}
		// A named tile is sampled only after its queued update or an already
		// resident map. Partial per-slot admission makes whole lights flicker.
		auto& cache = frame.Cache[i];
		UINT latest = 0;
		for (UINT face = 0; face < LocalShadowFaceCount(cache.Light); ++face)
		{
			const auto& tile = cache.Tiles[face];
			const UINT bit = 1u << face;
			if (tile.Side && pageSync.at(tile.Page) == ShadowCrossSync::Latest) latest |= bit;
			if ((cache.LatestFaces ^ latest) & bit) cache.PublicationOrders[face] = 0;
		}
		const UINT changed = cache.LatestFaces ^ latest;
		cache.MapsValid &= ~changed;
		cache.LocalFallback &= ~changed;
		cache.LatestFaces = latest;
		cache.ReadyFaces = cache.Resolution ? cache.Needed : 0;
		statics.Cache[i].ReadyFaces = cache.ReadyFaces;
	}
	Pack(frame);
	staticPass.Pack(statics);
}

void SceneLocalShadows::PrepareAtlas(Slot& frame, UINT side, UINT pages)
{
	if (frame.Depth && frame.Depth->Pages == pages && frame.Depth->Side == side) return;
	frame.Depth = CreateAtlas(side, pages);
}

std::unique_ptr<SceneLocalShadows::Atlas> SceneLocalShadows::CreateAtlas(UINT side, UINT pages)
{
	assert(side && pages && pages <= MaxLocalShadowAtlasCount);
	auto atlas = std::make_unique<Atlas>();
	GDX12TextureDesc texture;
	texture.Width = texture.Height = side;
	texture.Format = DXGI_FORMAT_D32_FLOAT;
	texture.SRVDesc = ShadowTexture::View(pages);
	texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
	const auto desc = ShadowTexture::Depth(side, pages);
	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1;
	texture.ExternalResource =
	    _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
	texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
	atlas->Texture.Initialize(texture);
	atlas->Side = side;
	atlas->Pages = pages;
	for (UINT page = 0; page < pages; ++page)
	{
		auto descriptor = std::make_unique<GDX12Descriptor>();
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
		dsv.Format = DXGI_FORMAT_D32_FLOAT;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		dsv.Texture2DArray.FirstArraySlice = page;
		dsv.Texture2DArray.ArraySize = 1;
		descriptor->InitAsDSV(atlas->Texture.GetResource()->D3DResource.Get(), &dsv, _dsvHeap.get(),
		                      _dsvHeap->GetAvailableIndex());
		atlas->Dsvs.push_back(std::move(descriptor));
	}
	return atlas;
}

void SceneLocalShadows::PrepareAtlases(UINT side, UINT pages)
{
	if (pages)
		for (auto& frame : _slots) PrepareAtlas(frame, side, pages);
}

void SceneLocalShadows::MarkDepthWritten(UINT slot, UINT light, UINT face)
{
	auto& cache = _slots.at(slot).Cache;
	if (light < cache.size()) cache[light].ContentVersions[face] = ++_depthVersion;
}

GDX12Texture* SceneLocalShadows::Output(UINT slot)
{
	auto& frame = _slots.at(slot);
	if (frame.Depth) return &frame.Depth->Texture;
	if (!_fallback.IsInitialized())
	{
		GDX12TextureDesc texture{};
		texture.Width = texture.Height = 1;
		texture.Format = DXGI_FORMAT_R32_FLOAT;
		texture.SRVDesc = ShadowTexture::View(1);
		texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
		texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
		_fallback.Initialize(texture);
	}
	return &_fallback;
}

std::string SceneLocalShadows::Distribution(UINT slot) const
{
	const auto& frame = _slots.at(slot);
	if (!frame.Depth) return "0 pages";
	const auto& atlas = *frame.Depth;
	const auto used = std::count_if(frame.OccupiedTexels.begin(), frame.OccupiedTexels.end(), [](auto area) { return area > 0; });
	std::string result = std::to_string(used) + "/" + std::to_string(atlas.Pages) + " pages of " +
	    std::to_string(atlas.Side) + " x " + std::to_string(atlas.Side) + "; page fill";
	const UINT64 pageArea = UINT64(atlas.Side) * atlas.Side;
	for (const auto area : frame.OccupiedTexels)
		result += " " + (area && area * 100 < pageArea ? std::string("<1") : std::to_string(area * 100 / pageArea)) + "%";
	result += "; lights: " + std::to_string(frame.Placed) + " placed, " + std::to_string(frame.Downgraded) +
	    " reduced, " + std::to_string(frame.Skipped) + " skipped";
	std::map<UINT, UINT> faces;
	for (UINT i = 0; i < frame.Cache.size(); ++i)
		if (frame.Cache[i].Resolution)
			faces[frame.Cache[i].Resolution] += LocalShadowFaceCount(frame.Lights[i]);
	for (const auto& [side, count] : faces)
		result += "; " + std::to_string(count) + " faces at " + std::to_string(side);
	return result;
}

UINT64 SceneLocalShadows::ShadowTexels(UINT slot) const
{
	UINT64 pixels = 0;
	const auto& frame = _slots.at(slot);
	for (UINT i = 0; i < frame.Lights.size(); ++i)
	{
		const auto& cache = frame.Cache[i];
		const UINT faces = __popcnt(cache.ReadyFaces);
		pixels += UINT64(cache.Resolution) * cache.Resolution * faces;
	}
	return pixels;
}

void SceneLocalShadows::ResetLayout()
{
	_layout = {};
	_previousPlacement.clear();
	for (auto& frame : _slots)
	{
		frame.Work.Commands.clear();
		frame.Depth.reset();
		frame.OccupiedTexels.clear();
		for (auto& cache : frame.Cache)
		{
			cache.Resolution = 0;
			cache.Tiles = {};
			cache.ContentVersions = {};
			cache.PublicationOrders = {};
			cache.MapsValid = cache.ResidentFaces = cache.ReadyFaces = cache.LocalFallback = cache.RemoteFaces = cache.LatestFaces = 0;
		}
	}
}

void SceneLocalShadows::AppendGrassViews(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
                                       Id atlas, Id fallback, const Atlas* fallbackDepth, const GrassAppender& appendGrass)
{
	const auto& frame = _slots.at(slot);
	const auto& views = frame.Views;
	const auto cameraPosition = _common.CameraView.Invert().Translation();
	Id cameraResource = Invalid;
	D3D12_GPU_VIRTUAL_ADDRESS cameraAddress = 0;
	if (!graph.Device)
	{
		auto& camera = *_resources.FrameConstants.at(slot)->CameraCB;
		cameraResource = NativeWork::ResourceFor(graph, bindings, camera.GetResource().D3DResource.Get(),
		                                         "local grass shadow camera");
		cameraAddress = camera.GetElementAddress(_common.ActiveCameraCBufferIndex);
	}
	for (UINT i = 0; i < views.size(); ++i)
	{
		const auto& view = views[i];
		GeometryTarget grassTarget;
		grassTarget.View = {GDX12GeometryStage::Shadow,
		                    Matrix(frame.Lights[view.Light].ShadowViewProj[view.Face]).Transpose(), cameraPosition,
		                    cameraAddress};
		grassTarget.View.Key = (2ull << 60) | (uint64_t(view.Light) * 6 + view.Face);
		grassTarget.View.Filter = _staticOnly ? GDX12GeometryFilter::Static : GDX12GeometryFilter::Dynamic;
		grassTarget.View.FrameSlot = slot;
		grassTarget.View.DepthFormat = DXGI_FORMAT_D32_FLOAT;
		const auto* depth = view.DrawIntoFinal ? fallbackDepth : frame.Depth.get();
		grassTarget.Depth = view.DrawIntoFinal ? fallback : atlas;
		grassTarget.Camera = cameraResource;
		grassTarget.Dsv = depth->Dsvs[view.Tile.Page]->CPUHandle;
		grassTarget.X = view.Tile.X;
		grassTarget.Y = view.Tile.Y;
		grassTarget.Width = grassTarget.Height = view.Tile.Side;
		appendGrass(grassTarget, graph, bindings);
	}
}

void SceneLocalShadows::DrawShadowViews(ID3D12GraphicsCommandList* cmd, const Slot& frame,
                                        ID3D12CommandSignature* signature, ID3D12Resource* target,
                                        ID3D12PipelineState* opaquePipeline, ID3D12PipelineState* maskedPipeline,
                                        bool clearDepth, const Atlas* fallbackDepth)
{
	for (const auto& view : frame.Views)
	{
		if (!clearDepth && !view.OpaqueCount && !view.MaskedCount)
			continue;
		const auto& tile = view.Tile;
		const auto* depth = view.DrawIntoFinal ? fallbackDepth : frame.Depth.get();
		const auto dsv = depth->Dsvs[tile.Page]->CPUHandle;
		const D3D12_VIEWPORT viewport{float(tile.X), float(tile.Y), float(tile.Side), float(tile.Side), 0, 1};
		const D3D12_RECT rect{LONG(tile.X), LONG(tile.Y), LONG(tile.X + tile.Side), LONG(tile.Y + tile.Side)};
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &rect);
		cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		if (clearDepth)
			cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 1, &rect);
		cmd->SetGraphicsRoot32BitConstant(0, 0, 0);
		cmd->SetGraphicsRoot32BitConstant(0, view.Light * 6 + view.Face, 1);
		if (view.OpaqueCount)
		{
			cmd->SetPipelineState(opaquePipeline);
			cmd->ExecuteIndirect(signature, view.OpaqueCount, target,
			                     UINT64(view.First) * sizeof(GDX12IndirectDrawArgs), nullptr, 0);
		}
		if (view.MaskedCount)
		{
			cmd->SetPipelineState(maskedPipeline);
			cmd->SetGraphicsRoot32BitConstant(0, view.Light * 6 + view.Face, 1);
			cmd->ExecuteIndirect(signature, view.MaskedCount, target,
			                     UINT64(view.First + view.OpaqueCount) * sizeof(GDX12IndirectDrawArgs), nullptr, 0);
		}
	}
}
void SceneLocalShadows::Append(
    Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene, GDX12GeometryBuffer::RecordingBinding geometry,
    GDX12UploadBuffer<GDX12LocalLightConstants>& lights,
    const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass,
    const std::array<Id, 5>* inputs, const std::vector<Access>& materialTextures, Atlas* fallbackDepth)
{
	auto& frame = _slots.at(slot);
	auto& work = frame.Work;
	work.Commands.clear();
	if (frame.Views.empty())
	{
		return;
	}
	const auto resource = [&](ID3D12Resource* value, const char* name)
	{ return NativeWork::ResourceFor(graph, bindings, value, name); };
	const bool ownViews = std::any_of(frame.Views.begin(), frame.Views.end(), [](const auto& view) { return !view.DrawIntoFinal; });
	const bool fallbackViews = std::any_of(frame.Views.begin(), frame.Views.end(), [](const auto& view) { return view.DrawIntoFinal; });
	const Id atlas = ownViews ? resource(frame.Depth->Texture.GetResource()->D3DResource.Get(), "local shadow atlas") : Invalid;
	const Id fallback = fallbackViews ?
	    resource(fallbackDepth->Texture.GetResource()->D3DResource.Get(), "local shadow atlas") : Invalid;
	for (const auto& view : frame.Views) MarkDepthWritten(slot, view.Light, view.Face);
	if (!_staticOnly && frame.Packed.empty())
	{
		if (appendGrass)
			AppendGrassViews(graph, bindings, slot, atlas, fallback, fallbackDepth, appendGrass);
		return;
	}
	const Id commands = resource(frame.Commands->GetResource().D3DResource.Get(), "CPU-culled local shadow commands");
	auto* target = frame.Commands->GetResource().D3DResource.Get();
	if (!frame.Packed.empty())
	{
		auto* source = frame.CommandUpload->GetResource().D3DResource.Get();
		const UINT64 bytes = UINT64(frame.Packed.size()) * sizeof(GDX12IndirectDrawArgs);
		work.Add(graph, "local shadow packed command upload",
		         {{resource(source, "local shadow command upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
		          {commands, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		         [=](auto* cmd) { cmd->CopyBufferRegion(target, 0, source, 0, bytes); });
	}
	const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	const auto sceneIds = inputs ? *inputs : std::array<Id, 5>{SceneInstances, SceneMaterials, SceneTransforms,
	                                                          SceneVertices, SceneIndices};
	const Id lightId = resource(lights.GetResource().D3DResource.Get(), "local shadow light data");
	std::vector<Access> accesses{
	    {sceneIds[0], Mode::Read, read},
	    {sceneIds[1], Mode::Read, read},
	    {sceneIds[2], Mode::Read, read},
	    {sceneIds[3], Mode::Read,
	     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
	    {sceneIds[4], Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER},
	    {commands, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
	    {lightId, Mode::Read, graph.Resources.at(lightId).Memory == Heap::Upload
	                              ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : read}};
	for (const Id dest : {atlas, fallback})
		if (dest != Invalid) accesses.push_back({dest, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE});
	accesses.insert(accesses.end(), materialTextures.begin(), materialTextures.end());
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	auto* root = _root.get();
	auto* opaquePipeline = _opaqueDraw.Get();
	auto* maskedPipeline = _maskedDraw.Get();
	auto* signature = _signature.Get();
	const auto lightSrv = lights.GetSRV()->GPUHandle;
	const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
	const bool clearDepth = _staticOnly;
	work.Add(graph, "local shadow CPU lists ExecuteIndirect", std::move(accesses),
	         [=, &frame](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
		         BindShadowGeometry(cmd, root, scene, geometry);
		         cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(3), lightSrv);
		         cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(4), textures);
	         DrawShadowViews(cmd, frame, signature, target, opaquePipeline, maskedPipeline, clearDepth, fallbackDepth);
	         });
	if (appendGrass)
	{
		AppendGrassViews(graph, bindings, slot, atlas, fallback, fallbackDepth, appendGrass);
	}
}
} // namespace BistroRender
