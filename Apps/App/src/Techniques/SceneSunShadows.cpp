#include "SceneSunShadows.h"
#include "ShadowGeometry.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace BistroRender
{
namespace
{
void DrawCascadeTiles(ID3D12GraphicsCommandList* cmd, UINT cascadeCount, UINT update, UINT resolution,
                      D3D12_CPU_DESCRIPTOR_HANDLE dsv, ID3D12CommandSignature* signature, UINT draws,
                      const std::array<ID3D12Resource*, 4>& commands, ID3D12Resource* counts,
                      const std::array<ID3D12PipelineState*, 2>& pipelines)
{
	const UINT64 commandSectionBytes = UINT64(draws) * sizeof(GDX12IndirectDrawArgs);
	for (UINT cascade = 0; cascade < cascadeCount; ++cascade)
	{
		if (!(update & (1u << cascade)))
		{
			continue;
		}
		const auto region = SunShadowRegion(cascade, resolution);
		const D3D12_VIEWPORT viewport{
		    float(region.X), float(region.Y), float(region.Width), float(region.Height), 0, 1};
		const D3D12_RECT rect{LONG(region.X), LONG(region.Y), LONG(region.X + region.Width),
		                      LONG(region.Y + region.Height)};
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 1, &rect);
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &rect);
		if (draws)
		{
			cmd->SetGraphicsRoot32BitConstant(0, 0, 0);
			for (UINT stream = 0; stream < pipelines.size(); ++stream)
			{
				cmd->SetPipelineState(pipelines[stream]);
				cmd->SetGraphicsRoot32BitConstant(0, cascade, 1);
				cmd->ExecuteIndirect(signature, draws, commands[cascade], stream * commandSectionBytes,
				                     counts, (2 * cascade + stream) * sizeof(UINT));
			}
		}
	}
}
} // namespace
SceneSunShadows::SceneSunShadows(GDX12DeviceResources& r, RenderPipelineCommonData& common)
    : _resources(r), _common(common), _slots(r.FrameConstants.size())
{
	GDX12RootSignatureDesc graphics;
	graphics.InstancedGeometry = true;
	graphics.Constants = {2};
	graphics.NumSingleCBVSlots = 1;
	graphics.NumSingleSRVSlots = 3;
	graphics.StaticSamplers = GetStaticSamplers();
	graphics.SRVRanges.emplace_back(Texture2D_RangeLength);
	_graphicsRoot = std::make_unique<GDX12RootSignature>(r.Device, graphics);
	GDX12RootSignatureDesc compute;
	compute.Constants = {1};
	compute.NumSingleCBVSlots = 1;
	compute.NumSingleSRVSlots = 3;
	compute.NumSingleUAVSlots = 5;
	_computeRoot = std::make_unique<GDX12RootSignature>(r.Device, compute);
	D3D_SHADER_MACRO defines[] = {{"SHADOW_CULLING", "1"}, {"SHADOW_CASCADE_MASK", "1"}, {nullptr, nullptr}};
	_cull = GDX12RenderHelpers::CreateComputePipeline(r.Device, _computeRoot.get(), SHADERS_FOLDER "ShadowPass.hlsl",
	                                                  "CS", defines);
	_clear = GDX12RenderHelpers::CreateComputePipeline(r.Device, _computeRoot.get(), SHADERS_FOLDER "ShadowPass.hlsl",
	                                                   "ClearCS", defines);
	auto& compiler = GDX12ShaderCompiler::GetInstance();
	const D3D_SHADER_MACRO unbatched[] = {{"SHADOW_UNBATCHED", "1"}, {nullptr, nullptr}};
	auto vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "ShadowPass.hlsl", unbatched, "VS", "vs");
	auto ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "ShadowPass.hlsl", nullptr, "PS", "ps");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
	p.pRootSignature = _graphicsRoot->GetRootSignature().Get();
	p.InputLayout = {r.InputLayouts.at("Default").data(), UINT(r.InputLayouts.at("Default").size())};
	p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	p.RasterizerState.FrontCounterClockwise = TRUE;
	p.RasterizerState.DepthBias = 500;
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
	auto depthVS = compiler.CompileShader(r.Device, SHADERS_FOLDER "ShadowPass.hlsl", depthDefines, "VS", "vs");
	p.VS = {depthVS->GetBufferPointer(), depthVS->GetBufferSize()};
	p.PS = {};
	ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_opaqueDraw)));
	_signature = GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device, _graphicsRoot.get());
}

void SceneSunShadows::RebuildBounds(Slot& s, UINT slot)
{
	s.HasBounds = false;
	const auto& frame = *_resources.FrameConstants.at(slot);
	const auto* instances = frame.InstanceCache->GetCPUData();
	const auto* materials = frame.MaterialCache->GetCPUData();
	const auto* commands = _resources.IndirectCommandsCache->GetCPUData();
	for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
	{
		if (!commands[i].InstanceCount)
		{
			continue;
		}
		const auto& instance = instances[commands[i].InstanceID];
		const auto& material = materials[instance.MaterialIndex];
		if (!instance.CastsShadows || material.RenderLayer == 2 || material.ReflectionProbePreview)
		{
			continue;
		}
		const BoundingBox bounds(instance.BoundingBoxCenter, instance.BoundingBoxExtents);
		if (s.HasBounds)
		{
			BoundingBox::CreateMerged(s.Bounds, s.Bounds, bounds);
		}
		else
		{
			s.Bounds = bounds;
		}
		s.HasBounds = true;
	}
	s.BoundsReady = true;
}

void SceneSunShadows::Freeze(UINT slot, bool castersRebuilt, const std::vector<BoundingBox>& changedCasters,
                             const std::vector<GDX12RenderPrimitive>& grass)
{
	for (auto& cached : _slots)
	{
		if (castersRebuilt)
		{
			cached.BoundsReady = false;
			cached.DirtyCascades = (1u << std::size(cached.Settings.ViewProj)) - 1;
			continue;
		}
		for (const auto& bounds : changedCasters)
		{
			if (cached.HasBounds)
			{
				BoundingBox::CreateMerged(cached.Bounds, cached.Bounds, bounds);
			}
			else
			{
				cached.Bounds = bounds;
				cached.HasBounds = true;
			}
		}
		for (UINT cascade = 0; !changedCasters.empty() && cascade < cached.Settings.CascadeCount; ++cascade)
		{
			const UINT bit = 1u << cascade;
			if (cached.DirtyCascades & bit)
			{
				continue;
			}
			const GDX12LocalShadowFrustum frustum(cached.Settings.ViewProj[cascade]);
			if (std::any_of(changedCasters.begin(), changedCasters.end(),
			                [&](const BoundingBox& bounds) { return frustum.Intersects(bounds); }))
			{
				cached.DirtyCascades |= bit;
			}
		}
	}
	auto& s = _slots.at(slot);
	if (!s.BoundsReady)
	{
		RebuildBounds(s, slot);
	}
	BoundingBox bounds = s.Bounds;
	bool hasBounds = s.HasBounds;
	std::vector<VegetationCaster> vegetation;
	vegetation.reserve(grass.size());
	for (const auto& primitive : grass)
	{
		if (!primitive.Visible || !primitive.CastsShadows || !primitive.InstanceCount)
		{
			continue;
		}
		if (hasBounds)
		{
			BoundingBox::CreateMerged(bounds, bounds, primitive.Bounds);
		}
		else
		{
			bounds = primitive.Bounds;
		}
		hasBounds = true;
		vegetation.push_back({primitive.Revision(), primitive.Bounds});
	}
	_resources.ShadowSettings = BuildShadowCascades(
	    _common.CameraView.Invert(), XMConvertToRadians(_common.ActiveCameraFOV),
	    float(_common.DownscaledWidth) / _common.DownscaledHeight, _common.ActiveCameraNearPlane,
	    _common.ActiveCameraFarPlane, Vector3(_resources.Lighting.LightDirection), hasBounds ? &bounds : nullptr,
	    _resources.ShadowQuality.SunResolution, _resources.ShadowQuality.SunCascades);
	const UINT flags = _resources.Lighting.SunShadowFlags;
	const bool fullUpdate = (flags & ShadowEveryFrame) || !s.Rendered ||
	                        s.Resolution != _resources.ShadowQuality.SunResolution ||
	                        s.Revision != _resources.Lighting.SunShadowRevision ||
	                        std::memcmp(&s.Settings, &_resources.ShadowSettings, sizeof(s.Settings)) != 0;
	const UINT cascades = (1u << _resources.ShadowSettings.CascadeCount) - 1;
	const auto byRevision = [](const auto& a, const auto& b) { return a.Revision < b.Revision; };
	std::sort(vegetation.begin(), vegetation.end(), byRevision);
	if (fullUpdate)
	{
		s.DirtyCascades |= cascades;
	}
	else if ((s.DirtyCascades & cascades) != cascades)
	{
		// Compare both sets so moved or removed casters also invalidate their previous tiles.
		std::vector<VegetationCaster> changed;
		std::set_symmetric_difference(s.Vegetation.begin(), s.Vegetation.end(), vegetation.begin(), vegetation.end(),
		                              std::back_inserter(changed), byRevision);
		for (UINT cascade = 0; !changed.empty() && cascade < s.Settings.CascadeCount; ++cascade)
		{
			const UINT bit = 1u << cascade;
			if (s.DirtyCascades & bit)
			{
				continue;
			}
			const GDX12LocalShadowFrustum frustum(s.Settings.ViewProj[cascade]);
			if (std::any_of(changed.begin(), changed.end(),
			                [&](const VegetationCaster& caster) { return frustum.Intersects(caster.Bounds); }))
			{
				s.DirtyCascades |= bit;
			}
		}
	}
	s.Vegetation = std::move(vegetation);
	s.Update = (flags & ShadowEnabled) ? s.DirtyCascades & cascades : 0;
}

void SceneSunShadows::PrepareAtlas(Slot& s, UINT resolution)
{
	if (s.Resolution == resolution)
	{
		return;
	}
	GDX12TextureDesc texture;
	texture.Width = texture.Height = (std::max)(1u, resolution * 2);
	texture.Format = texture.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
	texture.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
	texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	texture.SRVDesc.Texture2D.MipLevels = 1;
	texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
	texture.SRVHeapIndex = s.Atlas.IsInitialized() ? s.Atlas.GetSRV()->HeapIndex
	                                               : texture.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
	texture.CreateDSV = true;
	texture.DSVHeap = _resources.DSVHeap.get();
	texture.DSVHeapIndex = s.Atlas.IsInitialized() ? s.Atlas.GetDSV()->HeapIndex : texture.DSVHeap->GetAvailableIndex();
	texture.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	texture.ClearValue = {1, 0, 0, 0};
	const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, texture.Width, texture.Height, 1, 1, 1, 0,
	                                               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1;
	texture.ExternalResource =
	    _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
	s.Atlas.Initialize(texture);
	s.Resolution = resolution;
	s.Rendered = false;
}

void SceneSunShadows::PrepareIndirectBuffers(Slot& s, UINT draws)
{
	if (draws > UINT_MAX / (2 * sizeof(GDX12IndirectDrawArgs)))
	{
		throw std::length_error("Sun shadow command sections exceed buffer size limit");
	}
	const auto buffer = [&](auto& target, UINT count)
	{
		using Buffer = typename std::remove_reference_t<decltype(target)>::element_type;
		if (!target || target->GetElementCount() < count)
		{
			target = std::make_unique<Buffer>(_resources.Device, count, EBufferType::Default, false);
			target->CreateUAV(_resources.SRV_UAV_Heap.get(),
			                  _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
		}
	};
	buffer(s.Counters, 2 * UINT(s.Commands.size()));
	for (auto& commands : s.Commands)
	{
		buffer(commands, 2 * (std::max)(draws, 1u));
	}
}

void SceneSunShadows::AppendGrassViews(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot, Id atlas,
                                       UINT update, const GrassAppender& appendGrass)
{
	auto& s = _slots.at(slot);
	const UINT cascadeCount = _resources.ShadowSettings.CascadeCount, resolution = s.Resolution;
	const auto dsv = s.Atlas.GetDSV()->CPUHandle;
	const auto cameraPosition = _common.CameraView.Invert().Translation();
	Id cameraId = Invalid;
	D3D12_GPU_VIRTUAL_ADDRESS cameraAddress = 0;
	if (!graph.Device)
	{
		auto& camera = *_resources.FrameConstants.at(slot)->CameraCB;
		cameraId = NativeWork::ResourceFor(graph, bindings, camera.GetResource().D3DResource.Get(), "sun grass camera constants");
		cameraAddress = camera.GetElementAddress(_common.ActiveCameraCBufferIndex);
	}
	for (UINT cascade = 0; cascade < cascadeCount; ++cascade)
	{
		if (!(update & (1u << cascade)))
		{
			continue;
		}
		const auto region = SunShadowRegion(cascade, resolution);
		GeometryTarget target;
		target.View = {GDX12GeometryStage::Shadow, Matrix(_resources.ShadowSettings.ViewProj[cascade]).Transpose(),
		               cameraPosition, cameraAddress};
		target.View.Key = (4ull << 60) | cascade;
		target.View.FrameSlot = slot;
		target.View.DepthFormat = DXGI_FORMAT_D32_FLOAT;
		target.Depth = atlas;
		target.Camera = cameraId;
		target.Dsv = dsv;
		target.X = region.X;
		target.Y = region.Y;
		target.Width = region.Width;
		target.Height = region.Height;
		appendGrass(target, graph, bindings);
	}
}

void SceneSunShadows::Append(
    Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene, UINT draws, GDX12GeometryBuffer::RecordingBinding geometry,
    const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass,
    UINT mask, const std::array<Id, 6>* inputs, const std::vector<Access>& materialTextures)
{
	auto& s = _slots.at(slot);
	auto& work = s.Work;
	work.Commands.clear();
	const UINT resolution = _resources.ShadowQuality.SunResolution;
	const auto resource = [&](ID3D12Resource* value, const char* name)
	{ return NativeWork::ResourceFor(graph, bindings, value, name); };
	PrepareAtlas(s, resolution);
	const Id atlas = resource(s.Atlas.GetResource()->D3DResource.Get(), "sun shadow atlas");
	const auto dsv = s.Atlas.GetDSV()->CPUHandle;
	const UINT update = s.Update & mask;
	if (!update || !resolution)
	{
		if (!s.Rendered)
		{
			work.Add(graph, "initialize sun shadow atlas", {{atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}},
			         [=](auto* cmd) { cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr); });
			s.Rendered = true;
		}
		return;
	}
	PrepareIndirectBuffers(s, draws);
	const Id counters = resource(s.Counters->GetResource().D3DResource.Get(), "sun visible counts");
	const auto cbv = _resources.FrameConstants.at(slot)->ShadowCB->GetElementAddress(0);
	const Id constants = resource(_resources.FrameConstants.at(slot)->ShadowCB->GetResource().D3DResource.Get(),
	                              "sun cascade constants");
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	auto* compute = _computeRoot.get();
	auto* clear = _clear.Get();
	auto* cull = _cull.Get();
	const auto countsUav = s.Counters->GetUAV()->GPUHandle;
	work.Add(graph, "sun reset visible counts", {{counters, Mode::Write, UnorderedAccess}},
	         [=](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetComputeRootSignature(compute->GetRootSignature().Get());
		         cmd->SetPipelineState(clear);
		         cmd->SetComputeRootDescriptorTable(compute->GetURootParamIndex(4), countsUav);
		         cmd->Dispatch(1, 1, 1);
	         });
	const auto ids = inputs ? *inputs : std::array<Id, 6>{SceneInstances, SceneMaterials, SceneTransforms,
	                                                       SceneVertices, SceneIndices, SceneCommands};
	std::vector<Access> culling{{ids[0], Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
	                            {ids[5], Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
	                            {ids[1], Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
	                            {constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
	                            {counters, Mode::ReadWrite, UnorderedAccess}};
	std::vector<Access> rendering{
	    {ids[0], Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
	    {ids[1], Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
	    {ids[2], Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
	    {ids[3], Mode::Read,
	     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
	    {ids[4], Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER},
	    {constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
	    {counters, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
	    {atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}};
	rendering.insert(rendering.end(), materialTextures.begin(), materialTextures.end());
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> uavs{};
	std::array<ID3D12Resource*, 4> commands{};
	for (UINT i = 0; i < 4; ++i)
	{
		commands[i] = s.Commands[i]->GetResource().D3DResource.Get();
		const Id id = resource(commands[i], "sun visible commands");
		uavs[i] = s.Commands[i]->GetUAV()->GPUHandle;
		culling.push_back({id, Mode::Write, UnorderedAccess});
		rendering.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
	}
	const UINT cascadeCount = _resources.ShadowSettings.CascadeCount;
	work.Add(graph, "sun cull " + std::to_string(cascadeCount) + " cascades", std::move(culling),
	         [=](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetComputeRootSignature(compute->GetRootSignature().Get());
		         cmd->SetPipelineState(cull);
		         cmd->SetComputeRoot32BitConstant(0, update, 0);
		         cmd->SetComputeRootConstantBufferView(compute->GetBRootParamIndex(1), cbv);
		         for (UINT i = 0; i < 3; ++i)
		         {
			         cmd->SetComputeRootDescriptorTable(compute->GetTRootParamIndex(i), scene[i]);
		         }
		         for (UINT i = 0; i < 4; ++i)
		         {
			         cmd->SetComputeRootDescriptorTable(compute->GetURootParamIndex(i), uavs[i]);
		         }
		         cmd->SetComputeRootDescriptorTable(compute->GetURootParamIndex(4), countsUav);
		         constexpr UINT CullThreads = 64; // ShadowPass.hlsl CS
		         if (draws)
		         {
			         cmd->Dispatch((draws + CullThreads - 1) / CullThreads, 1, 1);
		         }
	         });
	auto* graphics = _graphicsRoot.get();
	const std::array<ID3D12PipelineState*, 2> pipelines = {_opaqueDraw.Get(), _maskedDraw.Get()};
	auto* signature = _signature.Get();
	auto* counts = s.Counters->GetResource().D3DResource.Get();
	const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
	work.Add(graph, "sun atlas " + std::to_string(cascadeCount) + " cascades ExecuteIndirect", std::move(rendering),
	         [=](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetGraphicsRootSignature(graphics->GetRootSignature().Get());
		         cmd->SetGraphicsRootConstantBufferView(graphics->GetBRootParamIndex(1), cbv);
		         BindShadowGeometry(cmd, graphics, scene, geometry);
		         cmd->SetGraphicsRootDescriptorTable(graphics->GetTRootParamIndex(3), textures);
		         cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		         DrawCascadeTiles(cmd, cascadeCount, update, resolution, dsv, signature, draws, commands, counts,
		                          pipelines);
	         });
	if (appendGrass)
	{
		AppendGrassViews(graph, bindings, slot, atlas, update, appendGrass);
	}
	s.Settings = _resources.ShadowSettings;
	s.Revision = _resources.Lighting.SunShadowRevision;
	s.Rendered = true;
	s.DirtyCascades &= ~update;
}
} // namespace BistroRender
