#include "SceneGeometry.h"
#include "NativeWork.h"
#include "ShadowGeometry.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>
#include <fstream>

namespace BistroRender
{
namespace
{
constexpr DXGI_FORMAT Formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
constexpr UINT UploadBucketCount = 4;
constexpr UINT CounterBytes = sizeof(UINT);
constexpr UINT EmptyGeometryBytes = 64;
constexpr Id GBufferResourceBegin = 12;
constexpr Id DepthResource = 17;
constexpr Id VertexResource = SceneVertices;
constexpr Id IndexResource = SceneIndices;
constexpr Id SkinResourceBegin = 20;
constexpr Id PaletteUploadOperation = 0;
constexpr Id SkinningOperation = 1;
constexpr Id SceneUploadOperation = 2;
constexpr Id ClearCountsOperation = 3;
constexpr Id CullingOperation = 4;
constexpr Id DepthOperation = 5;
constexpr Id GBufferOperation = 6;
constexpr Id BatchingOperationCount = 4;
constexpr State ShaderRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
} // namespace
SceneGeometry::SceneGeometry(RenderPipelineContext& c)
    : _resources(c.Primary), _slots(c.Primary.FrameConstants.size()), _common(c.Common)
{
	auto& r = c.Primary;
	_grass = std::make_unique<SceneGrass>(c);
	for (auto& frame : r.FrameConstants)
	{
		frame->MaterialCache->TrackChanges();
	}
	_emptyVertices = r.Device->CreateResource(
	    D3D12_HEAP_TYPE_DEFAULT,
	    CD3DX12_RESOURCE_DESC::Buffer(EmptyGeometryBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
	    D3D12_RESOURCE_STATE_COMMON);
	_emptyIndices = r.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, CD3DX12_RESOURCE_DESC::Buffer(EmptyGeometryBytes),
	                                         D3D12_RESOURCE_STATE_COMMON);

	GDX12RootSignatureDesc d;
	d.NumSingleCBVSlots = 1;
	d.NumSingleSRVSlots = 3;
	d.NumSingleUAVSlots = 4;
	_cullRoot = std::make_unique<GDX12RootSignature>(r.Device, d);
	const D3D_SHADER_MACRO cullDefines[] = {{"LATE_PROBE_PREVIEW", "1"}, {nullptr, nullptr}};
	_cull = GDX12RenderHelpers::CreateComputePipeline(r.Device, _cullRoot.get(), SHADERS_FOLDER "Culling.hlsl", "CS",
	                                                  cullDefines);
	d = {};
	d.NumSingleUAVSlots = 2;
	_clearRoot = std::make_unique<GDX12RootSignature>(r.Device, d);
	_clear =
	    GDX12RenderHelpers::CreateComputePipeline(r.Device, _clearRoot.get(), SHADERS_FOLDER "BufferClear.hlsl", "CS");
	d = {};
	d.InstancedGeometry = true;
	d.Constants = {1};
	d.NumSingleCBVSlots = 2;
	d.NumSingleSRVSlots = 3;
	d.SRVRanges.emplace_back(Texture2D_RangeLength);
	d.StaticSamplers = GetStaticSamplers();
	_drawRoot = std::make_unique<GDX12RootSignature>(r.Device, d);
	_signature = GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device, _drawRoot.get());
	auto& compiler = GDX12ShaderCompiler::GetInstance();
	D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
	p.pRootSignature = _drawRoot->GetRootSignature().Get();
	p.InputLayout = {r.InputLayouts.at("Default").data(), UINT(r.InputLayouts.at("Default").size())};
	p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	p.RasterizerState.FrontCounterClockwise = TRUE;
	p.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	p.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	p.SampleMask = UINT_MAX;
	p.SampleDesc.Count = 1;
	p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	auto vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "DepthVS", "vs");
	auto ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "DepthPS", "ps");
	p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_depth)));
	vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "VS", "vs");
	ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "PS", "ps");
	p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
	p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	p.NumRenderTargets = 5;
	std::copy(std::begin(Formats), std::end(Formats), p.RTVFormats);
	ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_gbuffer)));
}
void SceneGeometry::BufferFor(Buffer& b, UINT64 bytes, UINT stride, bool upload)
{
	bytes = (std::max)(bytes, UINT64(stride));
	if (b.Data && b.Bytes == bytes)
	{
		return;
	}
	b.Bytes = bytes;
	b.Revision = 0;
	if (upload)
	{
		b.Source = _resources.Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
		                                             D3D12_RESOURCE_STATE_GENERIC_READ);
	}
	auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	b.Data = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
	auto* heap = _resources.SRV_UAV_Heap.get();
	const auto srvIndex = b.Srv.HeapIndex == UINT_MAX ? heap->GetAvailableIndex(SceneDescriptorStart) : b.Srv.HeapIndex;
	const auto uavIndex = b.Uav.HeapIndex == UINT_MAX ? heap->GetAvailableIndex(SceneDescriptorStart) : b.Uav.HeapIndex;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.NumElements = UINT(bytes / stride);
	srv.Buffer.StructureByteStride = stride;
	b.Srv.InitAsSRV(b.Data.Get(), &srv, heap, srvIndex);
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = UINT(bytes / stride);
	uav.Buffer.StructureByteStride = stride;
	b.Uav.InitAsUAV(b.Data.Get(), nullptr, &uav, heap, uavIndex);
}
void SceneGeometry::Resize(UINT width, UINT height)
{
	_width = width;
	_height = height;
	for (auto& s : _slots)
	{
		s.PlannedBindings.clear();
		for (UINT i = 0; i < 5; ++i)
		{
			if (!s.Color[i])
			{
				s.Color[i] = std::make_unique<GDX12Texture>();
				GDX12RenderHelpers::InitializeRenderTarget(*s.Color[i], _resources, width, height, Formats[i],
				                                           {0, 0, 0, 0});
			}
			else
			{
				s.Color[i]->Resize(width, height);
			}
		}
		auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, width, height, 1, 1, 1, 0,
		                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		D3D12_CLEAR_VALUE clear{};
		clear.Format = DXGI_FORMAT_D32_FLOAT;
		s.Depth = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
		if (!s.DepthView)
		{
			s.DepthView = std::make_unique<GDX12Texture>();
		}
		GDX12TextureDesc depthView;
		depthView.ExternalResource = s.Depth;
		depthView.Width = width;
		depthView.Height = height;
		depthView.Format = DXGI_FORMAT_R32_FLOAT;
		depthView.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthView.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
		depthView.SRVHeapIndex = s.DepthView->IsInitialized()
		                             ? s.DepthView->GetSRV()->HeapIndex
		                             : depthView.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
		depthView.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		depthView.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		depthView.SRVDesc.Texture2D.MipLevels = 1;
		s.DepthView->Initialize(depthView);
		auto* heap = _resources.DSVHeap.get();
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
		dsv.Format = DXGI_FORMAT_D32_FLOAT;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		s.Dsv.InitAsDSV(s.Depth.Get(), &dsv, heap,
		                s.Dsv.HeapIndex == UINT_MAX ? heap->GetAvailableIndex() : s.Dsv.HeapIndex);
		dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
		s.ReadDepth.InitAsDSV(s.Depth.Get(), &dsv, heap,
		                      s.ReadDepth.HeapIndex == UINT_MAX ? heap->GetAvailableIndex() : s.ReadDepth.HeapIndex);
	}
}
void SceneGeometry::Freeze(UINT index, UINT camera, std::vector<GDX12SkinningWork> skin)
{
	auto& s = _slots.at(index);
	auto& f = *_resources.FrameConstants.at(index);
	// CPU preparation is complete. These per-frame uploads cannot change until
	// the returned frame fence; workers only record copies of assigned ranges.
	auto input = [&](Buffer& destination, auto& buffer, bool frameLocal = false)
	{
		BufferFor(destination, UINT64(buffer->GetElementCount()) * buffer->GetElementSize(), buffer->GetElementSize(),
		          true);
		const auto revision = buffer->GetRevision();
		destination.Upload =
		    destination.Revision != revision || destination.Origin.Get() != buffer->GetResource().D3DResource.Get();
		if (!destination.Upload)
		{
			return;
		}
		const UINT stride = buffer->GetElementSize(), count = buffer->GetElementCount();
		destination.Ranges.clear();
		const bool resident =
		    destination.Revision && destination.Origin.Get() == buffer->GetResource().D3DResource.Get();
		// The global indirect buffer has several slot consumers; its journal cannot describe an older slot.
		auto changed = frameLocal && resident ? buffer->ChangedElements() : std::vector<UINT>{};
		if (changed.empty())
		{
			destination.Ranges.push_back({0, UINT(destination.Bytes / stride)});
		}
		else
		{
			std::sort(changed.begin(), changed.end());
			for (UINT element : changed)
			{
				if (element >= count)
				{
					throw std::out_of_range("Scene upload change exceeds buffer size");
				}
				if (!destination.Ranges.empty() && element <= destination.Ranges.back().second)
				{
					destination.Ranges.back().second = (std::max)(destination.Ranges.back().second, element + 1);
				}
				else
				{
					destination.Ranges.push_back({element, element + 1});
				}
			}
		}
		void* data = nullptr;
		D3D12_RANGE noRead{};
		ThrowIfFailed(destination.Source->Map(0, &noRead, &data));
		const auto* source = buffer->GetCPUData();
		if (!resident)
		{
			std::memset(data, 0, SIZE_T(destination.Bytes));
		}
		for (const auto& [begin, end] : destination.Ranges)
		{
			const UINT readableEnd = (std::min)(end, count);
			if (begin >= readableEnd)
			{
				continue;
			}
			auto* target = static_cast<BYTE*>(data) + UINT64(begin) * stride;
			if (stride == sizeof(*source))
			{
				std::memcpy(target, source + begin, SIZE_T(readableEnd - begin) * stride);
			}
			else
			{
				for (UINT i = begin; i < readableEnd; ++i)
				{
					std::memcpy(static_cast<BYTE*>(data) + UINT64(i) * stride, source + i, sizeof(*source));
				}
			}
		}
		destination.Source->Unmap(0, nullptr);
		destination.Revision = revision;
		destination.Origin = buffer->GetResource().D3DResource;
	};
	input(s.Buffers[SceneInstances], f.InstanceCache, true);
	input(s.Buffers[SceneCommands], _resources.IndirectCommandsCache);
	input(s.Buffers[SceneMaterials], f.MaterialCache, true);
	input(s.Buffers[SceneTransforms], f.TransformCache, true);
	{
		s.FrameBuffers[0].Data = f.MainCB->GetResource().D3DResource;
		s.FrameBuffers[0].Bytes = UINT64(f.MainCB->GetElementCount()) * f.MainCB->GetElementSize();
		s.FrameBuffers[1].Data = f.CameraCB->GetResource().D3DResource;
		s.FrameBuffers[1].Bytes = UINT64(f.CameraCB->GetElementCount()) * f.CameraCB->GetElementSize();
	}
	s.FrameStrides = {f.MainCB->GetElementSize(), f.CameraCB->GetElementSize()};
	s.Draws = _resources.IndirectCommandsCache->GetElementCount();
	for (UINT i : {4u, 6u})
	{
		BufferFor(s.Buffers[i], UINT64((std::max)(s.Draws, 1u)) * sizeof(GDX12IndirectDrawArgs),
		          sizeof(GDX12IndirectDrawArgs));
	}
	for (UINT i : {5u, 7u})
	{
		BufferFor(s.Buffers[i], CounterBytes, CounterBytes);
	}
	s.Camera = s.FrameBuffers[1].Data->GetGPUVirtualAddress() + UINT64(camera) * s.FrameStrides[1];
	s.Main = s.FrameBuffers[0].Data->GetGPUVirtualAddress();
	s.Geometry = _resources.GeometryBuffer->FreezeRecordingBinding();
	if (!s.Geometry.VertexResource)
	{
		s.Geometry.VertexResource = _emptyVertices.Get();
		s.Geometry.Vertices = {_emptyVertices->GetGPUVirtualAddress(), EmptyGeometryBytes, sizeof(Vertex)};
	}
	if (!s.Geometry.IndexResource)
	{
		s.Geometry.IndexResource = _emptyIndices.Get();
		s.Geometry.Indices = {_emptyIndices->GetGPUVirtualAddress(), EmptyGeometryBytes, DXGI_FORMAT_R32_UINT};
	}
	s.Skin = std::move(skin);
	s.Batches = _resources.SkeletalBatchCount;
	s.InstanceCount = _resources.SkeletalInstanceCount;
	if (s.Batches)
	{
		if (!s.Batcher)
		{
			s.Batcher = std::make_unique<GDX12InstanceBatcher>(_resources.Device);
		}
		s.Batcher->ReserveRecording(s.Draws, s.Batches, s.InstanceCount);
		if (!s.BatchCounter)
		{
			s.BatchCounter = _resources.Device->CreateResource(
			    D3D12_HEAP_TYPE_DEFAULT,
			    CD3DX12_RESOURCE_DESC::Buffer(CounterBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
			    D3D12_RESOURCE_STATE_COMMON);
		}
	}
	s.SkinBindings.clear();
	s.SkinDeclarations.clear();
	s.SkinIds.clear();
	for (const auto& work : s.Skin)
	{
		std::array<Id, 5> ids{};
		for (UINT j = 0; j < 5; ++j)
		{
			const auto found = std::find(s.SkinBindings.begin(), s.SkinBindings.end(), work.Resources[j]);
			if (found != s.SkinBindings.end())
			{
				ids[j] = SkinResourceBegin + Id(found - s.SkinBindings.begin());
			}
			else
			{
				ids[j] = SkinResourceBegin + Id(s.SkinBindings.size());
				s.SkinBindings.push_back(work.Resources[j]);
				const State initial = j == 0 ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : 0;
				s.SkinDeclarations.push_back({"skin input " + std::to_string(ids[j]), initial, initial, j == 1, j != 0,
				                              j == 0 ? Heap::Upload : Heap::Default});
			}
		}
		s.SkinIds.push_back(ids);
	}
}
Schedule SceneGeometry::Plan(UINT index, const AppendTechnique& append, const AppendTechnique& prelude,
                             bool singleDirect)
{
	auto& s = _slots.at(index);
	s.PlannedBindings.clear();
	Graph g;
	for (UINT i = 0; i < 8; ++i)
	{
		g.Resources.push_back({"scene buffer " + std::to_string(i), 0, 0, true, true});
	}
	for (UINT i = 0; i < 4; ++i)
	{
		g.Resources.push_back({"frozen upload " + std::to_string(i), D3D12_RESOURCE_STATE_GENERIC_READ,
		                       D3D12_RESOURCE_STATE_GENERIC_READ, false, false, Heap::Upload});
	}
	for (UINT i = 0; i < 5; ++i)
	{
		g.Resources.push_back({"GBuffer " + std::to_string(i), 0, 0});
	}
	g.Resources.push_back({"depth", 0, 0});
	g.Resources.push_back({"scene vertices", 0, 0, true, true});
	g.Resources.push_back({"scene indices", 0, 0, false, true});
	g.Resources.insert(g.Resources.end(), s.SkinDeclarations.begin(), s.SkinDeclarations.end());
	Operation palettes{"skin palette uploads"}, skinning{"skin deformation"};
	palettes.Target = Queue::Copy;
	skinning.Target = Queue::Compute;
	const auto add = [](Operation& op, Access access)
	{
		if (std::none_of(op.Accesses.begin(), op.Accesses.end(),
		                 [&](const auto& a) { return a.Resource == access.Resource; }))
		{
			op.Accesses.push_back(access);
		}
	};
	for (const auto& ids : s.SkinIds)
	{
		add(palettes, {ids[0], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
		add(palettes, {ids[1], Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
		for (UINT j = 1; j < 5; ++j)
		{
			add(skinning, {ids[j], Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
		}
	}
	if (!s.Skin.empty())
	{
		skinning.Accesses.push_back({VertexResource, Mode::Write, UnorderedAccess});
	}
	g.Operations.push_back(palettes);
	g.Operations.push_back(skinning);
	Operation upload{"scene upload buckets"};
	upload.Lists = UploadBucketCount;
	upload.Target = Queue::Copy;
	for (UINT i = 0; i < 4; ++i)
	{
		if (s.Buffers[i].Upload)
		{
			upload.Accesses.push_back({i, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
			upload.Accesses.push_back({8 + i, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
		}
	}
	g.Operations.push_back(upload);
	g.Operations.push_back(
	    {"reset visible counts", {{5, Mode::Write, UnorderedAccess}, {7, Mode::Write, UnorderedAccess}}});
	Operation cull{"GPU scene culling"};
	for (UINT i = 0; i < 3; ++i)
	{
		cull.Accesses.push_back({i, Mode::Read, ShaderRead});
	}
	for (UINT i = 4; i < 8; ++i)
	{
		cull.Accesses.push_back({i, Mode::ReadWrite, UnorderedAccess});
	}
	cull.Target = Queue::Compute;
	for (auto& a : cull.Accesses)
	{
		if (a.Usage == Mode::Read)
		{
			a.Required = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		}
	}
	g.Operations[ClearCountsOperation].Target = Queue::Compute;
	g.Operations.push_back(cull);
	Operation depth{"scene depth ExecuteIndirect"};
	for (UINT i : {0u, 2u, 3u})
	{
		depth.Accesses.push_back({i, Mode::Read, ShaderRead});
	}
	for (UINT i : {4u, 5u})
	{
		depth.Accesses.push_back({i, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
	}
	depth.Accesses.push_back({DepthResource, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE});
	g.Operations.push_back(depth);
	auto draw = depth;
	draw.Name = "scene GBuffer ExecuteIndirect";
	draw.Accesses.back() = {DepthResource, Mode::Read, D3D12_RESOURCE_STATE_DEPTH_READ};
	for (auto* operation : {&g.Operations.back(), &draw})
	{
		operation->Accesses.push_back(
		    {VertexResource, Mode::Read,
		     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
		operation->Accesses.push_back({IndexResource, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER});
	}
	for (UINT i = 12; i < 17; ++i)
	{
		draw.Accesses.push_back({i, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET});
	}
	g.Operations.push_back(draw);
	if (s.Batches)
	{
		for (UINT i = 0; i < 5; ++i)
		{
			s.BatchIds[i] = Id(g.Resources.size());
			g.Resources.push_back({"skeletal batching " + std::to_string(i), 0, 0, true, true, Heap::Default});
		}
		for (Id op : {DepthOperation, GBufferOperation})
		{
			auto& accesses = g.Operations[op].Accesses;
			accesses.erase(std::remove_if(accesses.begin(), accesses.end(),
			                              [](auto a) { return a.Resource == 4 || a.Resource == 5; }),
			               accesses.end());
			accesses.push_back({s.BatchIds[1], Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
			accesses.push_back({s.BatchIds[4], Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
			accesses.push_back({s.BatchIds[2], Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
		}
		std::vector<Operation> batching;
		batching.push_back({"skeletal private counter snapshot",
		                    {{5, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
		                     {s.BatchIds[4], Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}});
		for (UINT stage = 0; stage < 3; ++stage)
		{
			Operation operation{"skeletal batching stage " + std::to_string(stage),
			                    {{0, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
			                     {4, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}}};
			for (UINT i = 0; i < 5; ++i)
			{
				operation.Accesses.push_back({s.BatchIds[i], Mode::ReadWrite, UnorderedAccess});
			}
			batching.push_back(operation);
		}
		g.Operations.insert(g.Operations.begin() + DepthOperation, batching.begin(), batching.end());
	}
	for (UINT i = 0; i < 2; ++i)
	{
		s.FrameIds[i] = Id(g.Resources.size());
		const State state = D3D12_RESOURCE_STATE_GENERIC_READ;
		g.Resources.push_back({"frame constants " + std::to_string(i), state, state, false, false, Heap::Upload});
	}

	const State frameRead = D3D12_RESOURCE_STATE_GENERIC_READ;
	g.Operations[CullingOperation].Accesses.push_back({s.FrameIds[1], Mode::Read, frameRead});
	for (UINT op : {s.Batches ? DepthOperation + BatchingOperationCount : DepthOperation,
	                s.Batches ? GBufferOperation + BatchingOperationCount : GBufferOperation})
	{
		for (UINT i = 0; i < 2; ++i)
		{
			g.Operations[op].Accesses.push_back({s.FrameIds[i], Mode::Read, frameRead});
		}
	}
	GeometryTarget depthTarget;
	depthTarget.View = {GDX12GeometryStage::Depth, _common.CameraView * _common.CameraProjection,
	                    _common.CameraView.Invert().Translation(), s.Camera};
	depthTarget.View.Key = 1;
	depthTarget.View.FrameSlot = index;
	depthTarget.View.DepthFormat = DXGI_FORMAT_D32_FLOAT;
	depthTarget.Depth = DepthResource;
	depthTarget.Dsv = s.Dsv.CPUHandle;
	depthTarget.Width = _width;
	depthTarget.Height = _height;
	depthTarget.Camera = s.FrameIds[1];
	depthTarget.View.SceneDepth = s.DepthView.get();
	auto gbufferTarget = depthTarget;
	gbufferTarget.View.Stage = GDX12GeometryStage::GBuffer;
	gbufferTarget.Dsv = s.ReadDepth.CPUHandle;
	gbufferTarget.View.ColorTargetCount = 5;
	gbufferTarget.Colors.clear();
	gbufferTarget.Rtvs.clear();
	for (UINT i = 0; i < 5; ++i)
	{
		gbufferTarget.Colors.push_back(GBufferResourceBegin + i);
		gbufferTarget.Rtvs.push_back(s.Color[i]->GetRTV()->CPUHandle);
		gbufferTarget.View.ColorFormats[i] = Formats[i];
	}
	const std::vector<GeometryTarget> targets{depthTarget, gbufferTarget};
	s.PlannedBindings = Resources(index);
	_grass->Append(g, index, s.PlannedBindings, s.Buffers[2].Srv, targets);
	if (append)
	{
		append(g, s.PlannedBindings);
	}
	const Id preludeBegin = Id(g.Operations.size());
	if (prelude)
	{
		prelude(g, s.PlannedBindings);
	}
	std::vector<Id> order;
	for (Id id = preludeBegin; id < g.Operations.size(); ++id)
	{
		order.push_back(id);
	}
	for (Id id = 0; id < preludeBegin; ++id)
	{
		if (g.Operations[id].Accesses.empty())
		{
			continue;
		}
		order.push_back(id);
	}
	std::vector<Id> remap(g.Operations.size(), Invalid);
	for (Id i = 0; i < order.size(); ++i)
	{
		remap[order[i]] = i;
	}
	Graph reordered;
	reordered.Resources = std::move(g.Resources);
	for (auto id : order)
	{
		auto op = std::move(g.Operations[id]);
		for (auto& dep : op.Dependencies)
		{
			dep = remap.at(dep);
			if (dep == Invalid)
			{
				throw std::logic_error("Scene plan removed an explicit producer");
			}
		}
		reordered.Operations.push_back(std::move(op));
	}
	auto schedule = Compile(reordered, singleDirect);
	for (auto& packet : schedule.Packets)
	{
		if (packet.Operation != Invalid)
		{
			packet.Operation = order.at(packet.Operation);
		}
	}
	return schedule;
}
void SceneGeometry::AppendGrassView(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
                                    const GeometryTarget& target)
{
	_grass->Append(graph, slot, bindings, _slots.at(slot).Buffers[2].Srv, {target}, true);
}
std::vector<ID3D12Resource*> SceneGeometry::Resources(UINT index) const
{
	const auto& s = _slots.at(index);
	std::vector<ID3D12Resource*> result;
	for (auto& b : s.Buffers)
	{
		result.push_back(b.Data.Get());
	}
	for (UINT i = 0; i < 4; ++i)
	{
		result.push_back(s.Buffers[i].Source.Get());
	}
	for (auto& c : s.Color)
	{
		result.push_back(c->GetResource()->D3DResource.Get());
	}
	result.push_back(s.Depth.Get());
	result.push_back(s.Geometry.VertexResource ? s.Geometry.VertexResource : s.Buffers[0].Data.Get());
	result.push_back(s.Geometry.IndexResource ? s.Geometry.IndexResource : s.Buffers[1].Data.Get());
	result.insert(result.end(), s.SkinBindings.begin(), s.SkinBindings.end());
	if (s.Batches)
	{
		for (auto* resource : s.Batcher->RecordingResources())
		{
			result.push_back(resource);
		}
		result.push_back(s.BatchCounter.Get());
	}
	for (const auto& buffer : s.FrameBuffers)
	{
		result.push_back(buffer.Data.Get());
	}

	return result;
}
void SceneGeometry::Record(UINT index, Id op, UINT bucket, ID3D12GraphicsCommandList* cmd) const
{
	const auto& s = _slots.at(index);
	const auto& b = s.Buffers;
	if (_grass && _grass->Record(index, op, cmd))
	{
		return;
	}
	if (s.Batches && op >= DepthOperation && op < DepthOperation + BatchingOperationCount)
	{
		if (op == DepthOperation)
		{
			cmd->CopyBufferRegion(s.BatchCounter.Get(), 0, b[5].Data.Get(), 0, CounterBytes);
		}
		else
		{
			s.Batcher->RecordStage(cmd, op - (DepthOperation + 1), s.Draws, s.Batches,
			                       b[0].Data->GetGPUVirtualAddress(), b[4].Data->GetGPUVirtualAddress(),
			                       s.BatchCounter->GetGPUVirtualAddress());
		}
		return;
	}
	if (s.Batches && op >= DepthOperation + BatchingOperationCount)
	{
		op -= BatchingOperationCount;
	}
	if (op == PaletteUploadOperation)
	{
		for (const auto& skin : s.Skin)
		{
			cmd->CopyBufferRegion(skin.Resources[1], 0, skin.Resources[0], 0,
			                      UINT64(skin.BoneCount) * sizeof(GDX12SkinPalette));
		}
		return;
	}
	if (op == SkinningOperation)
	{
		for (const auto& skin : s.Skin)
		{
			skin.Record(cmd, s.Geometry.Vertices.BufferLocation);
		}
		return;
	}
	op -= SceneUploadOperation;
	if (op == 0)
	{
		RecordUploads(s, bucket, cmd);
		return;
	}
	ID3D12DescriptorHeap* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	cmd->SetDescriptorHeaps(1, &heap);
	if (op == 1)
	{
		cmd->SetComputeRootSignature(_clearRoot->GetRootSignature().Get());
		cmd->SetPipelineState(_clear.Get());
		cmd->SetComputeRootDescriptorTable(_clearRoot->GetURootParamIndex(0), b[5].Uav.GPUHandle);
		cmd->SetComputeRootDescriptorTable(_clearRoot->GetURootParamIndex(1), b[7].Uav.GPUHandle);
		cmd->Dispatch(1, 1, 1);
	}
	else if (op == 2)
	{
		cmd->SetComputeRootSignature(_cullRoot->GetRootSignature().Get());
		cmd->SetPipelineState(_cull.Get());
		cmd->SetComputeRootConstantBufferView(_cullRoot->GetBRootParamIndex(0), s.Camera);
		for (UINT i = 0; i < 3; ++i)
		{
			cmd->SetComputeRootDescriptorTable(_cullRoot->GetTRootParamIndex(i), b[i].Srv.GPUHandle);
		}
		for (UINT i = 0; i < 4; ++i)
		{
			cmd->SetComputeRootDescriptorTable(_cullRoot->GetURootParamIndex(i), b[4 + i].Uav.GPUHandle);
		}
		if (s.Draws)
		{
			cmd->Dispatch((s.Draws + 63) / 64, 1, 1);
		}
	}
	else if (op == 3 || op == 4)
	{
		RecordGeometry(s, op == 3, cmd);
	}
}

void SceneGeometry::RecordUploads(const Slot& slot, UINT bucket, ID3D12GraphicsCommandList* cmd) const
{
	const auto& b = slot.Buffers;
	for (UINT i = 0; i < 4; ++i)
	{
		if (!b[i].Upload)
		{
			continue;
		}
		const UINT stride = i == 0   ? sizeof(GDX12InstanceData)
		                    : i == 1 ? sizeof(GDX12IndirectDrawArgs)
		                    : i == 2 ? sizeof(GDX12MaterialConstants)
		                             : sizeof(GDX12TransformConstants);
		const UINT count = UINT(b[i].Bytes / stride), begin = count * bucket / UploadBucketCount,
		           end = count * (bucket + 1) / UploadBucketCount;
		for (const auto& range : b[i].Ranges)
		{
			const UINT first = (std::max)(begin, range.first), last = (std::min)(end, range.second);
			if (last > first)
			{
				cmd->CopyBufferRegion(b[i].Data.Get(), UINT64(first) * stride, b[i].Source.Get(),
				                      UINT64(first) * stride, UINT64(last - first) * stride);
			}
		}
	}
}
void SceneGeometry::RecordGeometry(const Slot& s, bool depth, ID3D12GraphicsCommandList* cmd) const
{
	const auto& b = s.Buffers;
	cmd->SetGraphicsRootSignature(_drawRoot->GetRootSignature().Get());
	cmd->SetPipelineState(depth ? _depth.Get() : _gbuffer.Get());
	cmd->SetGraphicsRootConstantBufferView(_drawRoot->GetBRootParamIndex(1), s.Main);
	cmd->SetGraphicsRootConstantBufferView(_drawRoot->GetBRootParamIndex(2), s.Camera);
	const UINT buffers[] = {2, 3, 0};
	for (UINT i = 0; i < 3; ++i)
	{
		cmd->SetGraphicsRootDescriptorTable(_drawRoot->GetTRootParamIndex(i), b[buffers[i]].Srv.GPUHandle);
	}
	cmd->SetGraphicsRootDescriptorTable(_drawRoot->GetTRootParamIndex(3),
	                                    _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
	const auto geom = _drawRoot->GetGeometryRootParamIndex();
	// Valid bindings even for branches unused by static geometry.
	const auto vertex = s.Geometry.Vertices.BufferLocation;
	cmd->SetGraphicsRootShaderResourceView(geom, vertex ? vertex : b[0].Data->GetGPUVirtualAddress());
	cmd->SetGraphicsRootShaderResourceView(geom + 1, s.Batches ? s.Batcher->VisibleInstancesAddress()
	                                                           : b[0].Data->GetGPUVirtualAddress());
	D3D12_VIEWPORT vp{0, 0, float(_width), float(_height), 0, 1};
	D3D12_RECT rect{0, 0, LONG(_width), LONG(_height)};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &rect);
	D3D12_CPU_DESCRIPTOR_HANDLE rt[5];
	for (UINT i = 0; i < 5; ++i)
	{
		rt[i] = s.Color[i]->GetRTV()->CPUHandle;
	}
	const auto dsv = depth ? s.Dsv.CPUHandle : s.ReadDepth.CPUHandle;
	cmd->OMSetRenderTargets(depth ? 0 : 5, rt, FALSE, &dsv);
	if (depth)
	{
		cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
	}
	else
	{
		const float clear[4] = {};
		for (auto handle : rt)
		{
			cmd->ClearRenderTargetView(handle, clear, 0, nullptr);
		}
	}
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	if (s.Draws && s.Geometry.VertexResource)
	{
		cmd->IASetVertexBuffers(0, 1, &s.Geometry.Vertices);
		cmd->IASetIndexBuffer(&s.Geometry.Indices);
		cmd->ExecuteIndirect(_signature.Get(), s.Draws,
		                     s.Batches ? s.Batcher->RecordingResources()[1] : b[4].Data.Get(), 0,
		                     s.Batches ? s.BatchCounter.Get() : b[5].Data.Get(), 0);
	}
}
} // namespace BistroRender
