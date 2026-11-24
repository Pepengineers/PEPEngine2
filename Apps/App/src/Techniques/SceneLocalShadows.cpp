#include "SceneLocalShadows.h"
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

SceneLocalShadows::SceneLocalShadows(GDX12DeviceResources& r, RenderPipelineCommonData& common, bool staticOnly)
    : _resources(r), _common(common), _slots(r.FrameConstants.size()), _staticOnly(staticOnly)
{
	_dsvHeap = std::make_unique<GDX12DescriptorHeap>(
	    r.Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
	    UINT(r.FrameConstants.size()) * D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION * 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
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
		frame.Indexed = r.Device->GetDeviceFeatures().Features.HighestShaderModel() >= D3D_SHADER_MODEL_6_6;
	}
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
			const UINT valid = cache.MapsValid | cache.StagedValid | (listsChanged ? cache.ListsValid : 0);
			if (!valid || !LocalShadowRangeIntersects(cache.Light, bounds)) continue;
			for (UINT face = 0; face < LocalShadowFaceCount(cache.Light); ++face)
			{
				const UINT bit = 1u << face;
				if ((valid & bit) && cache.Frusta.value()[face].Intersects(bounds))
				{
					if (listsChanged) cache.ListsValid &= ~bit;
					cache.MapsValid &= ~bit;
					cache.StagedValid &= ~bit;
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
				cache.MapsValid = cache.StagedValid = 0;
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
			cache.ListsValid = cache.MapsValid = cache.StagedValid = cache.LocalFallback = 0;
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

void SceneLocalShadows::Pack(Slot& frame)
{
	frame.Views.clear();
	frame.RemoteViews.clear();
	frame.Packed.clear();
	for (UINT lightIndex = 0; lightIndex < frame.Lights.size(); ++lightIndex)
	{
		auto& light = frame.Lights[lightIndex];
		auto& cached = frame.Cache[lightIndex];
		light.ShadowFirstSlice = cached.ReadyFaces ? cached.FirstSlice : UINT_MAX;
		light.ShadowFlags |= LocalShadowFaceMask;
		light._padding[2] = cached.ReadyFaces;
		GatherMissing(frame, lightIndex, cached.ReadyFaces | cached.StageThisFrame);
		if (frame.Indexed && light.ShadowFirstSlice != UINT_MAX)
			SetLocalShadowAtlas(light, frame.Atlases.at(cached.Resolution).Texture.GetSRV()->HeapIndex);
		const bool everyFrame = !_staticOnly && (light.ShadowFlags & ShadowEveryFrame);
		const UINT dirty = cached.ReadyFaces & (everyFrame ? UINT_MAX : (~cached.MapsValid | cached.LocalFallback));
		// Exact bootstrap may need faces that are valid only in this frame slot.
		const UINT update = dirty && cached.Latest && !(cached.ReadyFaces & ~cached.ResidentFaces)
		    ? cached.ReadyFaces : dirty;
		const auto append = [&](UINT mask, UINT side, UINT first, bool staged)
		{
			for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
			{
				if (!(mask & (1u << face))) continue;
				const auto& commands = cached.Faces[face];
				const size_t opaqueCount = commands.Opaque.size();
				const size_t maskedCount = commands.Masked.size();
				if (frame.Packed.size() + opaqueCount + maskedCount > UINT_MAX / sizeof(GDX12IndirectDrawArgs))
					throw std::length_error("Local shadow indirect buffer exceeds the buffer size limit");
				frame.Views.push_back({lightIndex, face, first + face, UINT(frame.Packed.size()),
				                       UINT(opaqueCount), UINT(maskedCount), side, (dirty & (1u << face)) != 0, staged});
				frame.Packed.insert(frame.Packed.end(), commands.Opaque.begin(), commands.Opaque.end());
				frame.Packed.insert(frame.Packed.end(), commands.Masked.begin(), commands.Masked.end());
			}
		};
		append(update, cached.Resolution, cached.FirstSlice, false);
		if (cached.StageThisFrame)
			append(cached.StageThisFrame, frame.StagedLayout->Sizes[lightIndex], frame.StagedLayout->Starts[lightIndex], true);
	}
}

void SceneLocalShadows::UpdateCamera(Slot& frame)
{
	XMFLOAT4X4 viewProjection;
	const auto viewProjectionMatrix = _common.CameraView * _common.CameraProjection;
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
		frame.StagedLayout.reset();
		++frame.LayoutRevision;
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
		const auto placement = std::find_if(_placements.begin(), _placements.end(), [&](const auto& entry)
		{ return entry.first == GetLocalShadowKey(source); });
		const Id owner = placement == _placements.end() ? 0 : placement->second;
		const bool latest = owner && std::find(_latestPlacements.begin(), _latestPlacements.end(),
		                                      GetLocalShadowKey(source)) != _latestPlacements.end();
		if (cached.Owner != owner || cached.Latest != latest)
			cached.MapsValid = cached.LocalFallback = 0;
		cached.Owner = owner;
		cached.Latest = latest;
		if (ShouldRenderLocalShadow(source, cached.HasLight ? &cached.Light : nullptr) ||
		    (source.ShadowFlags & ShadowEnabled) != (cached.Light.ShadowFlags & ShadowEnabled))
		{
			cached.ListsValid = cached.MapsValid = cached.StagedFaces = cached.StagedValid = 0;
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
	for (const bool staged : {false, true})
		for (auto& [resolution, atlas] : (staged ? frame.StagedAtlases : frame.Atlases))
		{
			if (_staticOnly) atlas.StaticDepthVersions.resize(atlas.Slices);
			std::vector<bool> updated(atlas.Slices);
			for (const auto& view : frame.Views)
			{
				if (view.Resolution != resolution || view.Staged != staged) continue;
				updated[view.Slice] = true;
				if (_staticOnly) atlas.StaticDepthVersions[view.Slice] = ++_staticDepthVersion;
				auto& descriptor = atlas.Dsvs[view.Slice];
				if (descriptor) continue;
				descriptor = std::make_unique<GDX12Descriptor>();
				D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
				dsv.Format = DXGI_FORMAT_D32_FLOAT;
				dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
				dsv.Texture2DArray.FirstArraySlice = view.Slice;
				dsv.Texture2DArray.ArraySize = 1;
				descriptor->InitAsDSV(atlas.Texture.GetResource()->D3DResource.Get(), &dsv, _dsvHeap.get(),
				                      _dsvHeap->GetAvailableIndex());
			}
			for (UINT slice = 0; slice < atlas.Slices; ++slice)
				if (!updated[slice]) atlas.Dsvs[slice].reset();
		}
}

void SceneLocalShadows::Submitted(UINT slot)
{
	auto& frame = _slots.at(slot);
	for (const auto& view : frame.Views)
	{
		auto& cache = frame.Cache[view.Light];
		if (view.Staged)
		{
			cache.StagedFaces |= 1u << view.Face;
			cache.StagedValid |= 1u << view.Face;
			continue;
		}
		cache.MapsValid |= 1u << view.Face;
		cache.ResidentFaces |= 1u << view.Face;
	}
	for (const auto& view : frame.RemoteViews)
	{
		auto& cache = frame.Cache[view.Light];
		cache.MapsValid |= 1u << view.Face;
		cache.ResidentFaces |= 1u << view.Face;
		cache.LocalFallback &= ~(1u << view.Face);
	}
}

void SceneLocalShadows::ApplyLayout(Slot& frame, const std::vector<UINT>& sizes, const std::vector<UINT>& starts)
{
	for (UINT i = 0; i < frame.Cache.size(); ++i)
	{
		auto& cached = frame.Cache[i];
		if (cached.Resolution != sizes[i] || cached.FirstSlice != starts[i])
		{
			cached.Resolution = sizes[i];
			cached.FirstSlice = starts[i];
			cached.MapsValid = cached.LocalFallback = cached.ResidentFaces = 0;
			++frame.LayoutRevision;
		}
	}
}

void SceneLocalShadows::Allocate(UINT slot, SceneLocalShadows& staticPass, const QueueSubmission& submissions, Id transparencyOwner)
{
	using ShadowMemory::Bytes;
	auto& frame = _slots.at(slot);
	auto& statics = staticPass._slots.at(slot);
	frame.Ready.Complete();
	const UINT ceiling = frame.Ceiling;
	std::vector<UINT> tiers{0};
	if (ceiling)
	{
		for (UINT side = 16; side < ceiling; side *= 2) tiers.push_back(side);
		tiers.push_back(ceiling);
	}
	struct Candidate
	{
		UINT Index, Faces, Desired;
		float Pixels;
		std::pair<UINT, uint64_t> Key;
	};
	std::vector<Candidate> candidates;
	std::map<Id, GDX12Device*> devices{{0, _resources.Device}};
	if (transparencyOwner) devices.emplace(transparencyOwner, &submissions.Device(transparencyOwner));
	for (UINT i = 0; i < frame.Lights.size(); ++i)
	{
		const auto& light = frame.Lights[i];
		const auto& cache = frame.Cache[i];
		if (!cache.Needed) continue;
		if (cache.Owner) devices.emplace(cache.Owner, &submissions.Device(cache.Owner));
		const auto id = GetLocalShadowKey(light);
		const auto key = std::make_pair(id.EntityId, id.Generation);
		const auto old = _previousResolution.find(key);
		const UINT previous = old == _previousResolution.end() ? 0 : old->second.Resolution;
		const float detail = std::sqrt(cache.ScreenPixels);
		UINT desired = 1;
		for (UINT tier = 2; tier < tiers.size(); ++tier)
		{
			const float threshold = (0.5f * (tiers[tier - 1] + tiers[tier])) *
			    (previous >= tiers[tier] ? 0.8f : 1.2f);
			if (detail < threshold) break;
			desired = tier;
		}
		candidates.push_back({i, LocalShadowFaceCount(light), desired, cache.ScreenPixels, key});
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
	{
		return a.Pixels != b.Pixels ? a.Pixels > b.Pixels : a.Key < b.Key;
	});

	struct Demand
	{
		UINT Slices = 0;
		std::map<std::pair<Id, bool>, UINT> Owners;
	};
	std::vector<Demand> demand(tiers.size());
	std::vector<UINT> selected(frame.Lights.size()), retained(tiers.size());
	const auto capacity = [&](UINT tier) {
		return demand[tier].Slices ? (std::max)(demand[tier].Slices, retained[tier]) : 0;
	};
	const auto adjust = [&](const Candidate& c, UINT tier, int direction)
	{
		if (!tier) return;
		auto& level = demand[tier];
		const auto& cache = frame.Cache[c.Index];
		level.Slices = UINT(int(level.Slices) + direction * int(c.Faces));
		if (cache.Owner)
		{
			auto& count = level.Owners[{cache.Owner, cache.Latest}];
			count = UINT(int(count) + direction * int(c.Faces));
		}
	};
	std::map<std::tuple<Id, UINT, UINT, bool>, Bytes> depths;
	std::map<std::tuple<Id, UINT, UINT>, Bytes> transfers;
	const auto depth = [&](Id owner, UINT side, UINT count, bool dsv)
	{
		auto [it, inserted] = depths.try_emplace({owner, side, count, dsv});
		if (inserted)
			it->second = ShadowMemory::Size(*devices.at(owner), ShadowMemory::Depth(side, count,
			    dsv ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE));
		return it->second;
	};
	const auto transfer = [&](Id owner, UINT side, UINT count)
	{
		auto [it, inserted] = transfers.try_emplace({owner, side, count});
		if (inserted) it->second = ShadowMemory::TransferBytes(*_resources.Device, *devices.at(owner), side, count);
		return it->second;
	};
	const auto cost = [&]()
	{
		std::map<Id, Bytes> result;
		for (UINT tier = 1; tier < demand.size(); ++tier)
		{
			const auto& level = demand[tier];
			if (!level.Slices) continue;
			if (level.Slices > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
			{
				result[0] = UINT64_MAX;
				break;
			}
			const UINT side = tiers[tier];
			result[0] += 2 * depth(0, side, capacity(tier), true) * _slots.size();
			if (transparencyOwner)
			{
				const auto copy = transfer(transparencyOwner, side, level.Slices);
				result[0] += copy * _slots.size();
				result[transparencyOwner] += (depth(transparencyOwner, side, capacity(tier), false) + copy) * _slots.size();
			}
			for (const auto& [lane, count] : level.Owners)
			{
				if (!count) continue;
				const auto owner = lane.first;
				const auto copy = transfer(owner, side, count);
				const auto staging = depth(0, side, count, false);
				result[0] += (2 * copy + (lane.second ? staging : 0)) * _slots.size();
				result[owner] += (depth(owner, side, count, true) + depth(owner, side, count, false) +
				                  2 * copy) * _slots.size();
			}
		}
		return result;
	};
	const auto fitsRetiredSlot = [&]()
	{
		const auto& primary = ShadowMemory::For(_resources.Device->GetAdapterLuid());
		Bytes existing = primary.Allocated, replacement = 0;
		for (const auto* current : {&frame, &statics})
			for (const auto* atlases : {&current->Atlases, &current->StagedAtlases})
				for (const auto& [side, atlas] : *atlases)
					existing -= atlas.Memory.Size();
		for (UINT tier = 1; tier < demand.size(); ++tier)
			if (demand[tier].Slices)
				replacement += 2 * depth(0, tiers[tier], capacity(tier), true);
		return replacement == 0 || (existing <= primary.Limit && replacement <= primary.Limit - existing);
	};
	const auto fits = [&](const auto& charges)
	{
		for (const auto& [owner, bytes] : charges)
			if (bytes > ShadowMemory::For(devices.at(owner)->GetAdapterLuid()).Limit) return false;
		return fitsRetiredSlot();
	};

	bool limited = false;
	for (const auto& c : candidates)
	{
		adjust(c, 1, 1);
		if (fits(cost())) selected[c.Index] = 1;
		else
		{
			adjust(c, 1, -1);
			limited = true;
		}
	}
	if (!frame.Indexed && tiers.size() > 2)
	{
		UINT desired = 1;
		for (const auto& c : candidates)
			if (selected[c.Index]) desired = (std::max)(desired, c.Desired);
		// The legacy shader binds one array. Preserve the minimum allocation for every admitted light.
		for (UINT tier = desired; tier > 1; --tier)
		{
			std::swap(demand[1], demand[tier]);
			if (fits(cost()))
			{
				for (auto& level : selected)
					if (level) level = tier;
				break;
			}
			std::swap(demand[1], demand[tier]);
			limited = true;
		}
	}
	else
	{
		for (;;)
		{
			const auto baseline = cost();
			const Candidate* best = nullptr;
			double bestRatio = -1;
			struct Upgrade
			{
				Bytes Extra = 0;
				bool Admitted = false;
			};
			std::map<std::tuple<UINT, UINT, Id, bool>, Upgrade> upgrades;
			for (const auto& c : candidates)
			{
				const UINT tier = selected[c.Index];
				if (!tier || tier >= c.Desired) continue;
				const auto& cache = frame.Cache[c.Index];
				auto [it, inserted] = upgrades.try_emplace({tier, c.Faces, cache.Owner, cache.Latest});
				auto& estimate = it->second;
				if (inserted)
				{
					adjust(c, tier, -1);
					adjust(c, tier + 1, 1);
					const auto trial = cost();
					estimate.Admitted = fits(trial);
					if (estimate.Admitted)
					{
						for (const auto& [owner, bytes] : trial)
						{
							const auto old = baseline.find(owner);
							const Bytes previous = old == baseline.end() ? 0 : old->second;
							if (bytes > previous) estimate.Extra += bytes - previous;
						}
					}
					adjust(c, tier + 1, -1);
					adjust(c, tier, 1);
				}
				if (!estimate.Admitted) { limited = true; continue; }
				const double gain = (std::min)(double(c.Pixels), double(tiers[tier + 1]) * tiers[tier + 1]) -
				                    (std::min)(double(c.Pixels), double(tiers[tier]) * tiers[tier]);
				const double ratio = gain / (std::max)(Bytes(1), estimate.Extra);
				if (ratio > bestRatio || (ratio == bestRatio && c.Key < best->Key))
				{
					best = &c;
					bestRatio = ratio;
				}
			}
			if (!best) break;
			auto& tier = selected[best->Index];
			adjust(*best, tier, -1);
			adjust(*best, tier + 1, 1);
			++tier;
		}
	}
	for (UINT tier = 1; tier < tiers.size(); ++tier)
	{
		const auto current = frame.Atlases.find(tiers[tier]);
		if (!demand[tier].Slices || current == frame.Atlases.end() || current->second.Slices <= demand[tier].Slices ||
		    current->second.Slices > 2 * demand[tier].Slices) continue;
		retained[tier] = current->second.Slices;
		if (!fits(cost())) retained[tier] = 0;
	}
	constexpr Bytes transitionBudget = 2ull * 6 * 512 * 512;
	std::vector<UINT> sizes(frame.Lights.size()), starts(frame.Lights.size());
	const auto buildLayout = [&]
	{
		std::fill(sizes.begin(), sizes.end(), 0);
		std::fill(starts.begin(), starts.end(), UINT_MAX);
		for (UINT tier = 1; tier < tiers.size(); ++tier)
		{
			if (!demand[tier].Slices) continue;
			std::vector<bool> used(capacity(tier));
			for (const auto& c : candidates)
			{
				if (selected[c.Index] != tier) continue;
				const auto& cache = frame.Cache[c.Index];
				sizes[c.Index] = tiers[tier];
				if (cache.Resolution != tiers[tier] || cache.FirstSlice == UINT_MAX ||
				    cache.FirstSlice + c.Faces > used.size()) continue;
				starts[c.Index] = cache.FirstSlice;
				std::fill_n(used.begin() + cache.FirstSlice, c.Faces, true);
			}
			for (UINT faces : {6u, 1u})
			{
				bool compact = false;
				for (const auto& c : candidates)
				{
					if (selected[c.Index] != tier || c.Faces != faces || starts[c.Index] != UINT_MAX) continue;
					const auto free = std::search_n(used.begin(), used.end(), faces, false);
					if (free == used.end())
					{
						compact = true;
						break;
					}
					starts[c.Index] = UINT(free - used.begin());
					std::fill_n(free, faces, true);
				}
				if (!compact) continue;
				UINT next = 0;
				for (const auto& c : candidates)
				{
					if (selected[c.Index] != tier) continue;
					starts[c.Index] = next;
					next += c.Faces;
				}
				assert(next == demand[tier].Slices);
				break;
			}
		}
	};
	std::vector<UINT> protectedFaces(frame.Lights.size());
	for (const auto& c : candidates)
	{
		const auto& current = frame.Cache[c.Index];
		if (selected[c.Index] && current.Resolution <= ceiling)
			protectedFaces[c.Index] = current.Needed & current.ReadyFaces & current.ResidentFaces & statics.Cache[c.Index].ResidentFaces;
	}
	const auto preservesPublishedFaces = [&]
	{
		Bytes work = 0;
		for (const auto& c : candidates)
		{
			const auto faces = protectedFaces[c.Index];
			if (!faces) continue;
			const auto side = sizes[c.Index];
			if (!side) return false;
			const auto& old = frame.Cache[c.Index];
			if (side != old.Resolution || starts[c.Index] != old.FirstSlice ||
			    capacity(selected[c.Index]) != frame.Atlases.at(old.Resolution).Slices)
			{
				work += 2ull * side * side * __popcnt(faces);
				if (work > transitionBudget) return false;
			}
		}
		return true;
	};
	buildLayout();
	auto& primary = *_resources.Device;
	const auto discardStage = [&]
	{
		for (auto* current : {&frame, &statics})
		{
			current->StagedLayout.reset();
			current->StagedAtlases.clear();
			for (auto& cache : current->Cache)
				cache.StagedFaces = cache.StagedValid = cache.StageThisFrame = 0;
		}
	};
	const bool mustEvict = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c)
	{
		const auto side = frame.Cache[c.Index].Resolution;
		return side && (!selected[c.Index] || side > ceiling);
	});
	if (frame.StagedLayout)
	{
		const auto& target = *frame.StagedLayout;
		const bool newReceiver = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c)
		{ return selected[c.Index] && !target.Sizes[c.Index]; });
		if (mustEvict || newReceiver || !ShadowMemory::Fits(primary, 0) ||
		    (!target.Capacities.empty() && target.Capacities.rbegin()->first > ceiling))
			discardStage();
	}
	else if (!frame.StagedAtlases.empty())
		discardStage();

	// Only changed destination arrays need a second allocation while the published maps are read.
	if (!frame.StagedLayout && !mustEvict && !preservesPublishedFaces())
	{
		ShadowLayout target{sizes, starts, {}};
		std::map<UINT, UINT> replacements;
		Bytes overlap = 0;
		for (UINT tier = 1; tier < tiers.size(); ++tier)
		{
			const UINT side = tiers[tier], count = capacity(tier);
			if (!count) continue;
			target.Capacities.emplace(side, count);
			const auto old = frame.Atlases.find(side);
			const bool unchanged = old != frame.Atlases.end() && old->second.Slices == count &&
			    std::all_of(candidates.begin(), candidates.end(), [&](const Candidate& c)
			    {
				    const auto& cache = frame.Cache[c.Index];
				    return sizes[c.Index] != side || (cache.Resolution == side && cache.FirstSlice == starts[c.Index]);
			    });
			if (unchanged) continue;
			replacements.emplace(side, count);
			overlap += 2 * depth(0, side, count, true);
		}
		if (!replacements.empty() && ShadowMemory::Fits(primary, overlap))
		{
			frame.StagedLayout = target;
			statics.StagedLayout = std::move(target);
			for (const auto& [side, count] : replacements)
			{
				PrepareAtlas(frame, side, count, true);
				staticPass.PrepareAtlas(statics, side, count, true);
			}
		}
	}

	bool holdingPublished = bool(frame.StagedLayout);
	bool adoptingStage = false;
	if (holdingPublished)
	{
		const auto& target = *frame.StagedLayout;
		adoptingStage = std::all_of(candidates.begin(), candidates.end(), [&](const Candidate& c)
		{
			if (!frame.StagedAtlases.count(target.Sizes[c.Index])) return true;
			const auto& now = frame.Cache[c.Index];
			return (now.Needed & ~(now.StagedFaces & statics.Cache[c.Index].StagedFaces)) == 0;
		});
		if (adoptingStage)
		{
			// The slot fence has retired all old readers and the last writes to the staged arrays.
			holdingPublished = false;
			sizes = target.Sizes;
			starts = target.Starts;
			ApplyLayout(frame, sizes, starts);
			staticPass.ApplyLayout(statics, sizes, starts);
			for (size_t i = 0; i < sizes.size(); ++i)
			{
				if (!frame.StagedAtlases.count(sizes[i])) continue;
				auto& current = frame.Cache[i];
				auto& staticDepth = statics.Cache[i];
				staticDepth.MapsValid = staticDepth.StagedValid;
				staticDepth.ResidentFaces = staticDepth.StagedFaces;
				current.MapsValid = current.StagedValid & staticDepth.StagedValid;
				current.ResidentFaces = current.StagedFaces;
				current.LocalFallback = current.Owner ? current.MapsValid : 0;
			}
			for (auto* current : {&frame, &statics})
			{
				for (auto it = current->Atlases.begin(); it != current->Atlases.end();)
					if (!target.Capacities.count(it->first) || current->StagedAtlases.count(it->first))
						it = current->Atlases.erase(it);
					else
						++it;
				current->Atlases.merge(current->StagedAtlases);
				++current->LayoutRevision;
			}
			discardStage();
		}
	}

	bool deferred = false;
	if (!holdingPublished && !adoptingStage && !preservesPublishedFaces())
	{
		const auto target = selected;
		const auto targetDemand = demand;
		const auto targetRetained = retained;
		std::fill(selected.begin(), selected.end(), 0);
		std::fill(demand.begin(), demand.end(), Demand{});
		std::fill(retained.begin(), retained.end(), 0);
		for (const auto& c : candidates)
		{
			const auto& old = frame.Cache[c.Index];
			const auto tier = UINT(std::find(tiers.begin(), tiers.end(), old.Resolution) - tiers.begin());
			if (!target[c.Index] || !tier || tier == tiers.size()) continue;
			selected[c.Index] = tier;
			adjust(c, tier, 1);
			retained[tier] = frame.Atlases.at(old.Resolution).Slices;
		}
		buildLayout();
		if (!fitsRetiredSlot() || !preservesPublishedFaces())
		{
			// A budget drop can require evicting the old layout after the slot fence.
			selected = target;
			demand = targetDemand;
			retained = targetRetained;
			limited = true;
		}
		else
		{
			deferred = true;
			const auto tryMove = [&](const Candidate& c, UINT next)
			{
				auto& tier = selected[c.Index];
				const UINT old = tier;
				const UINT oldCapacity = retained[next];
				adjust(c, old, -1);
				adjust(c, next, 1);
				tier = next;
				const bool singleArray = frame.Indexed ||
				    std::count_if(demand.begin(), demand.end(), [](const auto& d) { return d.Slices != 0; }) <= 1;
				const auto canPublish = [&]
				{
					if (!singleArray || !fits(cost())) return false;
					buildLayout();
					return preservesPublishedFaces();
				};
				if (next)
				{
					// Reserve the destination once, so later faces do not force another rebuild on arrival.
					retained[next] = (std::max)({oldCapacity, targetDemand[next].Slices, targetRetained[next]});
					if (canPublish()) return true;
				}
				retained[next] = oldCapacity;
				if (canPublish()) return true;
				tier = old;
				adjust(c, next, -1);
				adjust(c, old, 1);
				return false;
			};
			for (const auto& c : candidates)
			{
				const UINT old = selected[c.Index], next = target[c.Index];
				if (old == next || (!next && protectedFaces[c.Index])) continue;
				if (!tryMove(c, next) && !old)
					for (UINT lower = next; lower > 1;)
						if (tryMove(c, --lower)) break;
			}
		}
		buildLayout();
	}
	decltype(_previousResolution) placements;
	for (const auto& c : candidates)
	{
		auto& cache = frame.Cache[c.Index];
		const UINT side = holdingPublished ? cache.Resolution : sizes[c.Index];
		const auto old = _previousResolution.find(c.Key);
		if (side)
		{
			cache.Generation = old != _previousResolution.end() && old->second.Resolution == side
			    ? old->second.Generation : ++_generation;
			placements.emplace(c.Key, Placement{side, cache.Generation});
		}
	}
	_previousResolution.swap(placements);

	if (!holdingPublished && !adoptingStage)
	{
		for (auto* current : {&frame, &statics})
		{
			for (auto it = current->Atlases.begin(); it != current->Atlases.end();)
			{
				const auto tier = size_t(std::find(tiers.begin(), tiers.end(), it->first) - tiers.begin());
				if (tier < tiers.size() && capacity(UINT(tier)) == it->second.Slices)
				{
					++it;
					continue;
				}
				for (auto& cache : current->Cache)
					if (cache.Resolution == it->first) cache.MapsValid = cache.ResidentFaces = 0;
				++current->LayoutRevision;
				it = current->Atlases.erase(it);
			}
		}
		for (UINT tier = 1; tier < tiers.size(); ++tier)
		{
			if (!demand[tier].Slices) continue;
			PrepareAtlas(frame, tiers[tier], capacity(tier));
			staticPass.PrepareAtlas(statics, tiers[tier], capacity(tier));
		}
		ApplyLayout(frame, sizes, starts);
		staticPass.ApplyLayout(statics, sizes, starts);
	}
	// Repair placements that were already visible before admitting cold faces.
	Bytes work = 0;
	bool transitioning = deferred || holdingPublished;
	for (auto* current : {&frame, &statics})
		for (auto& cache : current->Cache) cache.ReadyFaces = cache.StageThisFrame = 0;
	for (const auto& c : candidates)
	{
		auto& current = frame.Cache[c.Index];
		current.ReadyFaces = current.Needed & current.ResidentFaces & statics.Cache[c.Index].ResidentFaces;
	}
	const auto admit = [&](UINT requested, UINT side, UINT& ready)
	{
		const Bytes faceWork = 2ull * side * side;
		for (UINT face = 0; face < 6; ++face)
		{
			const UINT bit = 1u << face;
			if (!(requested & ~ready & bit)) continue;
			if (faceWork > transitionBudget - work)
			{
				transitioning = true;
				continue;
			}
			work += faceWork;
			ready |= bit;
		}
	};
	for (const auto& c : candidates)
	{
		auto& current = frame.Cache[c.Index];
		if (current.Resolution)
			admit(protectedFaces[c.Index], current.Resolution, current.ReadyFaces);
	}
	if (holdingPublished)
	{
		for (const auto& c : candidates)
		{
			auto& current = frame.Cache[c.Index];
			auto& staticDepth = statics.Cache[c.Index];
			const UINT side = frame.StagedLayout->Sizes[c.Index];
			if (!frame.StagedAtlases.count(side)) continue;
			const UINT rendered = current.StagedFaces & staticDepth.StagedFaces;
			admit(current.Needed & ~rendered, side, current.StageThisFrame);
			staticDepth.StageThisFrame = current.StageThisFrame;
		}
	}
	for (const auto& c : candidates)
	{
		auto& current = frame.Cache[c.Index];
		if (current.Resolution)
			admit(current.Needed & ~protectedFaces[c.Index], current.Resolution, current.ReadyFaces);
	}
	for (const auto& c : candidates)
		statics.Cache[c.Index].ReadyFaces = frame.Cache[c.Index].ReadyFaces;
	assert(work <= transitionBudget);
	frame.MigrationTexels = work;
	frame.Limit = limited ? "DXGI/array capacity" : "";
	if (transitioning)
	{
		if (!frame.Limit.empty()) frame.Limit += ", ";
		frame.Limit += holdingPublished ? "staging shadow layout" : "transition work";
	}
	if (!frame.Indexed && ceiling)
	{
		if (!frame.Limit.empty()) frame.Limit += ", ";
		frame.Limit += "single array on Shader Model < 6.6";
	}
	Pack(frame);
	staticPass.Pack(statics);
}

void SceneLocalShadows::PrepareAtlas(Slot& frame, UINT resolution, UINT slices, bool staged)
{
	assert(slices && slices <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION && resolution);
	auto& atlas = (staged ? frame.StagedAtlases : frame.Atlases)[resolution];
	if (atlas.Slices == slices) return;
	for (auto& cached : frame.Cache)
		if (!staged && cached.Resolution == resolution) cached.MapsValid = cached.ResidentFaces = 0;
	GDX12TextureDesc texture;
	texture.Width = texture.Height = resolution;
	texture.Format = DXGI_FORMAT_D32_FLOAT;
	texture.SRVDesc = ShadowMemory::View(slices);
	texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
	const auto desc = ShadowMemory::Depth(resolution, slices);
	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1;
	texture.ExternalResource =
	    _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
	texture.SRVHeapIndex = atlas.Texture.IsInitialized() ? atlas.Texture.GetSRV()->HeapIndex
	                                                    : texture.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
	atlas.Texture.Initialize(texture);
	atlas.Memory.Set(*_resources.Device, ShadowMemory::Size(*_resources.Device, desc));
	atlas.Dsvs.clear();
	atlas.Dsvs.resize(slices);
	atlas.StaticDepthVersions.clear();
	atlas.Slices = slices;
	++frame.LayoutRevision;
}

GDX12Texture* SceneLocalShadows::Output(UINT slot)
{
	auto& frame = _slots.at(slot);
	if (!frame.Atlases.empty()) return &frame.Atlases.begin()->second.Texture;
	if (!_fallback.IsInitialized())
	{
		GDX12TextureDesc texture{};
		texture.Width = texture.Height = 1;
		texture.Format = DXGI_FORMAT_R32_FLOAT;
		texture.SRVDesc = ShadowMemory::View(1);
		texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
		texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
		_fallback.Initialize(texture);
	}
	return &_fallback;
}

std::vector<GDX12Texture*> SceneLocalShadows::Outputs(UINT slot)
{
	std::vector<GDX12Texture*> result;
	for (auto& [resolution, atlas] : _slots.at(slot).Atlases)
		result.push_back(&atlas.Texture);
	return result;
}

std::string SceneLocalShadows::Distribution(UINT slot)
{
	auto& frame = _slots.at(slot);
	std::map<UINT, UINT> lights;
	for (const auto& light : frame.Cache)
		if (light.Resolution) ++lights[light.Resolution];
	std::string result;
	for (auto& [resolution, atlas] : frame.Atlases)
	{
		if (!result.empty()) result += '|';
		const auto shape = atlas.Texture.GetResource()->D3DResource->GetDesc();
		result += std::to_string(shape.Width) + "x" + std::to_string(shape.Height) + ":" +
		          std::to_string(lights[resolution]) + "/" + std::to_string(shape.DepthOrArraySize) +
		          "/" + std::to_string(atlas.Memory.Size()) + "B";
	}
	return (result.empty() ? "none" : result) + (frame.Limit.empty() ? "" : " (" + frame.Limit + ")");
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
	_previousResolution.clear();
	for (auto& frame : _slots)
	{
		frame.Atlases.clear();
		frame.StagedAtlases.clear();
		frame.StagedLayout.reset();
		++frame.LayoutRevision;
		for (auto& cache : frame.Cache)
		{
			cache.Resolution = 0;
			cache.FirstSlice = UINT_MAX;
			cache.MapsValid = cache.ResidentFaces = cache.ReadyFaces = 0;
			cache.StagedFaces = cache.StagedValid = cache.StageThisFrame = 0;
		}
	}
}

void SceneLocalShadows::AppendGrassViews(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot, const std::map<std::pair<UINT, bool>, Id>& atlases,
                                         const GrassAppender& appendGrass)
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
		grassTarget.View.Key = (2ull << 60) | (uint64_t(view.Staged) << 59) | (uint64_t(view.Light) * 6 + view.Face);
		grassTarget.View.Filter = _staticOnly ? GDX12GeometryFilter::Static : GDX12GeometryFilter::Dynamic;
		grassTarget.View.FrameSlot = slot;
		grassTarget.View.DepthFormat = DXGI_FORMAT_D32_FLOAT;
		grassTarget.Depth = atlases.at({view.Resolution, view.Staged});
		grassTarget.Camera = cameraResource;
		grassTarget.Dsv = frame.AtlasFor(view.Resolution, view.Staged).Dsvs[view.Slice]->CPUHandle;
		grassTarget.Width = grassTarget.Height = view.Resolution;
		appendGrass(grassTarget, graph, bindings);
	}
}

void SceneLocalShadows::DrawShadowViews(ID3D12GraphicsCommandList* cmd, const Slot& frame,
                                        ID3D12CommandSignature* signature, ID3D12Resource* target,
                                        ID3D12PipelineState* opaquePipeline, ID3D12PipelineState* maskedPipeline,
                                        bool clearDepth)
{
	for (const auto& view : frame.Views)
	{
		if (!clearDepth && !view.OpaqueCount && !view.MaskedCount)
			continue;
		const auto dsv = frame.AtlasFor(view.Resolution, view.Staged).Dsvs[view.Slice]->CPUHandle;
		const D3D12_VIEWPORT viewport{0, 0, float(view.Resolution), float(view.Resolution), 0, 1};
		const D3D12_RECT rect{0, 0, LONG(view.Resolution), LONG(view.Resolution)};
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &rect);
		cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		if (clearDepth)
			cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
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
    const std::array<Id, 5>* inputs, const std::vector<Access>& materialTextures)
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
	std::map<std::pair<UINT, bool>, Id> atlases;
	for (const auto& view : frame.Views)
	{
		const auto key = std::make_pair(view.Resolution, view.Staged);
		if (!atlases.count(key))
			atlases.emplace(key, resource(frame.AtlasFor(view.Resolution, view.Staged).Texture.GetResource()->D3DResource.Get(),
			                              "local shadow depth array"));
	}
	if (!_staticOnly && frame.Packed.empty())
	{
		if (appendGrass)
			AppendGrassViews(graph, bindings, slot, atlases, appendGrass);
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
	for (const auto& [resolution, atlas] : atlases)
		accesses.push_back({atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE});
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
		         DrawShadowViews(cmd, frame, signature, target, opaquePipeline, maskedPipeline, clearDepth);
	         });
	if (appendGrass)
	{
		AppendGrassViews(graph, bindings, slot, atlases, appendGrass);
	}
}
} // namespace BistroRender
