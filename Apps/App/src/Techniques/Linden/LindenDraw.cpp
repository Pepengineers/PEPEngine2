#include "LindenDraw.h"
#include "LindenMesh.h"
#include "LindenSimulation.h"
#include "Techniques/NativeWork.h"
#include "Techniques/ProbeInputMirror.h"
#include "Techniques/ShadowGeometry.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"

namespace BistroRender
{
LindenDraw::LindenDraw(GDX12DeviceResources& resources, bool captureOnly) : _resources(resources)
{
	GDX12RootSignatureDesc root;
	root.IndexedLocalShadows = true;
	root.Constants = {18}; // view-projection, batch material, first pulled index
	root.NumSingleCBVSlots = 3;
	root.NumSingleSRVSlots = 11;
	root.SRVRanges.emplace_back(Texture2D_RangeLength);
	root.VolatileSRVData = true;
	root.StaticSamplers = GetStaticSamplers();
	CD3DX12_STATIC_SAMPLER_DESC shadow(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
	shadow.AddressU = shadow.AddressV = shadow.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	shadow.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	shadow.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	root.StaticSamplers.push_back(shadow);
	_root = std::make_unique<GDX12RootSignature>(resources.Device, root);

	auto& compiler = GDX12ShaderCompiler::GetInstance();
	for (auto stage : {Stage::Depth, Stage::DepthNormals, Stage::GBuffer, Stage::Shadow, Stage::Capture})
	{
		if (captureOnly && stage != Stage::Capture && stage != Stage::Shadow) continue;
		const bool depth = stage == Stage::Depth || stage == Stage::Shadow;
		const bool normals = stage == Stage::DepthNormals;
		const D3D_SHADER_MACRO prepass[] = {{"LINDEN_DEPTH_NORMALS", "1"}, {nullptr, nullptr}};
		const D3D_SHADER_MACRO capture[] = {{"VEGETATION_CAPTURE", "1"}, {nullptr, nullptr}};
		const auto* defines = stage == Stage::Capture ? capture : normals ? prepass : nullptr;
		auto vs = compiler.CompileShader(resources.Device, APP_SHADERS_FOLDER "LindenRender.hlsl", defines,
		                                 depth ? "DepthVS" : "VS", "vs");
		auto ps = compiler.CompileShader(resources.Device, APP_SHADERS_FOLDER "LindenRender.hlsl", defines,
		                                 depth ? "DepthPS" : normals ? "PrepassPS" : stage == Stage::Capture ? "CapturePS" : "PS", "ps");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = _root->GetRootSignature().Get();
		desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		desc.RasterizerState.FrontCounterClockwise = TRUE;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		if (stage == Stage::Shadow)
		{
			desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			desc.RasterizerState.DepthBias = 250;
			desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
		}
		if (stage == Stage::GBuffer)
		{
			desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
			desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			desc.NumRenderTargets = 5;
			desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
			desc.RTVFormats[2] = desc.RTVFormats[3] = desc.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		}
		if (stage == Stage::Capture || normals)
		{
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
		}
		desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		desc.SampleMask = UINT_MAX;
		desc.SampleDesc.Count = 1;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(resources.Device->GetDevice()->CreateGraphicsPipelineState(
		    &desc, IID_PPV_ARGS(&_pipelines.at(static_cast<size_t>(stage)))));
	}
}

void LindenDraw::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, UINT slot,
                        const GeometryTarget& target, const GDX12Descriptor& materials,
                        const std::shared_ptr<const LindenMesh>& mesh, const LindenDisplayedPose& pose,
                        const CaptureInputs* remote)
{
	if (!mesh || !target.View.IncludeApplicationGeometry)
		return;
	XMFLOAT4X4 clip;
	XMStoreFloat4x4(&clip, target.View.ViewProjection.Transpose());
	const auto bounds = mesh->WindBounds(mesh->Bounds);
	if (!LocalShadowFaceIntersects(clip, bounds))
		return;
	Stage stage;
	switch (target.View.Stage)
	{
	case GDX12GeometryStage::Depth: stage = Stage::Depth; break;
	case GDX12GeometryStage::DepthNormals: stage = Stage::DepthNormals; break;
	case GDX12GeometryStage::GBuffer: stage = Stage::GBuffer; break;
	case GDX12GeometryStage::Shadow: stage = Stage::Shadow; break;
	case GDX12GeometryStage::ReflectionCapture: stage = Stage::Capture; break;
	default: return;
	}
	if (remote && stage != Stage::Capture && stage != Stage::Shadow)
		throw std::invalid_argument("Unsupported remote linden view");

	auto id = [&](auto* buffer, const char* name)
	{ return NativeWork::ResourceFor(graph, bindings, buffer->GetResource().D3DResource.Get(), name); };
	if (_mesh != mesh)
	{
		const UINT count = static_cast<UINT>(mesh->Indices.size()), vertices = static_cast<UINT>(mesh->Vertices.size());
		const auto makeBuffers = [&](EBufferType type)
		{
			const auto flags = type == EBufferType::Upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			auto charge = remote ? remote->Inputs.ReserveShadowBuffers({UINT64(count) * sizeof(UINT), UINT64(vertices) * sizeof(Vector2)}, flags) : nullptr;
			auto buffers = std::make_shared<Buffers>(_resources.Device, count, vertices, type);
			buffers->Memory = std::move(charge);
			return buffers;
		};
		auto upload = makeBuffers(EBufferType::Upload);
		upload->Indices.CopyData(0, mesh->Indices.data(), count);
		std::vector<Vector2> uvs;
		uvs.reserve(vertices);
		for (const auto& vertex : mesh->Vertices) uvs.push_back(vertex.UV);
		upload->UVs.CopyData(0, uvs.data(), vertices);
		auto buffers = makeBuffers(EBufferType::Default);
		auto* heap = _resources.SRV_UAV_Heap.get();
		buffers->Indices.CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
		buffers->UVs.CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
		work.Add(graph, "linden upload triangle indices and UVs",
		         {{id(&buffers->Indices, "linden indices"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST},
		          {id(&buffers->UVs, "linden UVs"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST},
		          {id(&upload->Indices, "linden index upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
		          {id(&upload->UVs, "linden UV upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
		         [buffers, upload, count, vertices](auto* cmd)
		         {
			         cmd->CopyBufferRegion(buffers->Indices.GetResource().D3DResource.Get(), 0,
			                               upload->Indices.GetResource().D3DResource.Get(), 0, UINT64(count) * sizeof(UINT));
			         cmd->CopyBufferRegion(buffers->UVs.GetResource().D3DResource.Get(), 0,
			                               upload->UVs.GetResource().D3DResource.Get(), 0, UINT64(vertices) * sizeof(Vector2));
		         });
		_buffers = std::move(buffers);
		_mesh = mesh;
	}

	std::vector<Access> accesses = target.AdditionalReads;
	auto read = [&](Id resource, State state)
	{
		if (graph.Resources.at(resource).Memory == Heap::Upload) state = D3D12_RESOURCE_STATE_GENERIC_READ;
		if (std::none_of(accesses.begin(), accesses.end(), [resource](auto access) { return access.Resource == resource; }))
			accesses.push_back({resource, Mode::Read, state});
	};
	const bool capture = stage == Stage::Capture;
	const auto* frame = remote ? nullptr : _resources.FrameConstants.at(slot).get();
	const auto buffers = _buffers;
	read(id(&buffers->Indices, "linden indices"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	read(id(&buffers->UVs, "linden UVs"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	read(remote ? remote->Pose : id(pose.Current, "linden displayed vertices"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (stage == Stage::GBuffer)
		read(id(pose.Previous, "linden previous displayed vertices"), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	read(remote ? remote->Materials : SceneMaterials, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	if (target.Camera != Invalid) read(target.Camera, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	if (capture) read(remote ? remote->Main : id(frame->MainCB.get(), "linden main constants"),
	                  D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	accesses.push_back({target.Depth, stage == Stage::GBuffer ? Mode::Read : Mode::Write,
	                    State(stage == Stage::GBuffer ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE)});
	for (auto color : target.Colors) accesses.push_back({color, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET});

	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 12> srvs{};
	srvs[0] = remote ? remote->PoseSrv : pose.Current->GetSRV()->GPUHandle;
	srvs[1] = materials.GPUHandle;
	srvs[8] = buffers->Indices.GetSRV()->GPUHandle;
	srvs[9] = buffers->UVs.GetSRV()->GPUHandle;
	if (stage == Stage::GBuffer) srvs[10] = pose.Previous->GetSRV()->GPUHandle;
	srvs[11] = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
	D3D12_GPU_VIRTUAL_ADDRESS shadowConstants = 0;
	if (capture)
	{
		const auto& lighting = *target.View.CaptureLighting;
		srvs[2] = lighting.SunShadow->GetSRV()->GPUHandle;
		srvs[3] = lighting.Irradiance->GetSRV()->GPUHandle;
		srvs[4] = lighting.Prefiltered->GetSRV()->GPUHandle;
		srvs[5] = lighting.BRDF->GetSRV()->GPUHandle;
		srvs[6] = remote ? remote->LocalLights : lighting.LocalLightSrv.ptr ? lighting.LocalLightSrv
		                                                     : frame->ShadowLights->GetSRV()->GPUHandle;
		srvs[7] = lighting.LocalShadows->GetSRV()->GPUHandle;
		shadowConstants = lighting.ShadowConstants;
	}
	const auto main = remote ? remote->MainAddress : frame->MainCB->GetElementAddress(0);
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	auto* root = _root.get();
	const auto pipeline = _pipelines.at(static_cast<size_t>(stage));
	const UINT b = root->GetBRootParamIndex(0), t = root->GetTRootParamIndex(0);
	work.Add(graph, (stage == Stage::DepthNormals ? "linden DepthNormals view " : "linden vertex fetch view ") +
	                   std::to_string(target.View.Key), std::move(accesses),
	         [=, keepBuffers = buffers](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
		         cmd->SetPipelineState(pipeline.Get());
		         cmd->SetGraphicsRoot32BitConstants(b, 16, &clip, 0);
		         if (target.View.CameraConstants) cmd->SetGraphicsRootConstantBufferView(b + 1, target.View.CameraConstants);
		         if (capture)
		         {
			         cmd->SetGraphicsRootConstantBufferView(b + 2, main);
			         cmd->SetGraphicsRootConstantBufferView(b + 3, shadowConstants);
		         }
		         for (UINT i = 0; i < srvs.size(); ++i)
			         if (srvs[i].ptr) cmd->SetGraphicsRootDescriptorTable(t + i, srvs[i]);
		         const D3D12_VIEWPORT viewport{float(target.X), float(target.Y), float(target.Width), float(target.Height), 0, 1};
		         const D3D12_RECT scissor{LONG(target.X), LONG(target.Y), LONG(target.X + target.Width), LONG(target.Y + target.Height)};
		         cmd->RSSetViewports(1, &viewport);
		         cmd->RSSetScissorRects(1, &scissor);
		         cmd->OMSetRenderTargets(UINT(target.Rtvs.size()), target.Rtvs.data(), FALSE, &target.Dsv);
		         cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		         for (const auto& batch : mesh->Batches)
		         {
			         if ((target.View.Filter == GDX12GeometryFilter::Static && batch.Leaves) ||
			             (target.View.Filter == GDX12GeometryFilter::Dynamic && !batch.Leaves)) continue;
			         const UINT draw[] = {batch.MaterialIndex, batch.FirstIndex};
			         cmd->SetGraphicsRoot32BitConstants(b, 2, draw, 16);
			         cmd->DrawInstanced(batch.IndexCount, 1, 0, 0);
		         }
	         });
}
}
