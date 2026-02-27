#include <Engine/Debug/DebugRenderSysImpl.h>
//#include "GameFramework/GameFramework.h"
//#include "VertexPositionTex.h"
//#include "VertexPositionNormalBinormalTangentColorTex.h"
//#include "GameFramework/CommandQueue.h"
//#include "StaticMesh.h"
//#include "directx/d3d12.h"

using namespace DirectX::SimpleMath;


namespace gfw
{

	DebugRenderSysImpl::DebugRenderSysImpl(Microsoft::WRL::ComPtr<ID3D12Device> device)
	{
		//D3D11_BUFFER_DESC constDesc = {};
		//constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		//constDesc.CPUAccessFlags = 0;
		//constDesc.MiscFlags = 0;
		//constDesc.Usage = D3D11_USAGE_DEFAULT;
		//constDesc.ByteWidth = sizeof(Matrix);
		//
		//game->Device->CreateBuffer(&constDesc, nullptr, &constBuf);

		InitPrimitives(device);
		InitQuads();
		InitMeshes();
	}


	void DebugRenderSysImpl::InitPrimitives(Microsoft::WRL::ComPtr<ID3D12Device> device)
	{
		//auto device = GameFramework::Get()->GetDevice();
		int NUM_FRAMES = 3;
		m_vertexBufs.resize(NUM_FRAMES);
		for(int i = 0; i < NUM_FRAMES; ++i)
			m_vertexBufs[i] = std::make_unique<UploadBuffer<VertexPositionColor>>(device.Get(), MaxPointsCount, false);

		ComPtr<ID3DBlob> vertexBlob = d3dUtil::CompileShader(SHADERS_ENGINE_DIR L"\\Debug.hlsl", nullptr, "VSMain", "vs_4_0");
		ComPtr<ID3DBlob> pixelBlob  = d3dUtil::CompileShader(SHADERS_ENGINE_DIR L"\\Debug.hlsl", nullptr, "PSMain", "ps_4_0");

		D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		// Create a root signature.
		D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
		/*if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
		{
			featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
		}*/

		 // Allow input layout and deny unnecessary access to certain pipeline stages.
		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
			| D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
			| D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
			| D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
			| D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

		CD3DX12_ROOT_PARAMETER1 rootParameters[1];
		rootParameters[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDescription;
		CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC::Init_1_1(rootSignatureDescription, _countof(rootParameters), rootParameters, 0, nullptr, rootSignatureFlags);

		// Serialize the root signature.
		ComPtr<ID3DBlob> rootSignatureBlob;
		ComPtr<ID3DBlob> errorBlob;
		ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDescription,
			featureData.HighestVersion, &rootSignatureBlob, &errorBlob));
		// Create the root signature.
		ThrowIfFailed(device->CreateRootSignature(0, rootSignatureBlob->GetBufferPointer(), 
			rootSignatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)));

		D3D12_RT_FORMAT_ARRAY rtvFormats = {};
		rtvFormats.NumRenderTargets = 1;
		rtvFormats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

		struct PipelineStateStream
		{
			CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE pRootSignature;
			CD3DX12_PIPELINE_STATE_STREAM_INPUT_LAYOUT InputLayout;
			CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY PrimitiveTopologyType;
			CD3DX12_PIPELINE_STATE_STREAM_VS VS;
			CD3DX12_PIPELINE_STATE_STREAM_PS PS;
			CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER RasterizerState;
			CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL DepthStencilState;
			CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT DSVFormat;
			CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS RTVFormats;
		} pipelineStateStream;

		pipelineStateStream.pRootSignature = m_RootSignature.Get();
		pipelineStateStream.InputLayout = { inputLayout, _countof(inputLayout) };
		pipelineStateStream.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		pipelineStateStream.VS = CD3DX12_SHADER_BYTECODE(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize());
		pipelineStateStream.PS = CD3DX12_SHADER_BYTECODE(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize());
		pipelineStateStream.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		pipelineStateStream.RTVFormats = rtvFormats;
		pipelineStateStream.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		pipelineStateStream.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		(&pipelineStateStream.RasterizerState)->CullMode = D3D12_CULL_MODE_BACK;
		(&pipelineStateStream.RasterizerState)->FillMode = D3D12_FILL_MODE_WIREFRAME;
		(&pipelineStateStream.DepthStencilState)->DepthEnable = true;

		D3D12_PIPELINE_STATE_STREAM_DESC pipelineStateStreamDesc = {
			sizeof(PipelineStateStream), &pipelineStateStream
		};
		Microsoft::WRL::ComPtr<ID3D12Device2> device2;
		device->QueryInterface(IID_PPV_ARGS(&device2));

		device2->CreatePipelineState(&pipelineStateStreamDesc, IID_PPV_ARGS(&m_PipelineState));

		lines.reserve(100);
	}


	void DebugRenderSysImpl::InitQuads()
	{
		//quadProjMatrix = Matrix::CreateOrthographicOffCenter(0, static_cast<float>(game->Display->ClientWidth), static_cast<float>(game->Display->ClientHeight), 0, 0.1f, 1000.0f);
		//
		//ID3DBlob* errorCode = nullptr;
		//
		//D3DCompileFromFile(L"Shaders/TexturedShader.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &vertexQuadCompResult, &errorCode);
		//game->Device->CreateVertexShader(vertexQuadCompResult->GetBufferPointer(), vertexQuadCompResult->GetBufferSize(), nullptr, &vertexQuadShader);
		//
		//if (errorCode)errorCode->Release();
		//
		//D3DCompileFromFile(L"Shaders/TexturedShader.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &pixelQuadCompResult, &errorCode);
		//game->Device->CreatePixelShader(pixelQuadCompResult->GetBufferPointer(), pixelQuadCompResult->GetBufferSize(), nullptr, &pixelQuadShader);
		//
		//if (errorCode)errorCode->Release();
		//
		//quadLayout = VertexPositionTex::GetLayout(vertexQuadCompResult);
		//quadBindingStride = VertexPositionTex::Stride;
		//
		//quads.reserve(10);
		//
		//auto points = new Vector4[8]{
		//	Vector4(1, 1, 0.0f, 1.0f), Vector4(1.0f, 1.0f, 0.0f, 0.0f),
		//	Vector4(0, 1, 0.0f, 1.0f), Vector4(0.0f, 1.0f, 0.0f, 0.0f),
		//	Vector4(1, 0, 0.0f, 1.0f), Vector4(1.0f, 0.0f, 0.0f, 0.0f),
		//	Vector4(0, 0, 0.0f, 1.0f), Vector4(0.0f, 0.0f, 0.0f, 0.0f),
		//};
		//
		//D3D11_BUFFER_DESC bufDesc = {};
		//{
		//	bufDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		//	bufDesc.CPUAccessFlags = 0;
		//	bufDesc.MiscFlags = 0;
		//	bufDesc.Usage = D3D11_USAGE_DEFAULT;
		//	bufDesc.ByteWidth = sizeof(float) * 4 * 8;
		//}
		//
		//D3D11_SUBRESOURCE_DATA subData = {};
		//subData.pSysMem = points;
		//
		//game->Device->CreateBuffer(&bufDesc, &subData, &quadBuf);
		//
		//delete[] points;
		//
		//float borderCol[] = { 1.0f, 0.0f, 0.0f, 1.0f };
		//
		//D3D11_SAMPLER_DESC samplDesc = {};
		//{
		//	samplDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		//	samplDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		//	samplDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		//	samplDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		//	samplDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
		//	samplDesc.BorderColor[0] = 1.0f;
		//	samplDesc.BorderColor[1] = 0.0f;
		//	samplDesc.BorderColor[2] = 0.0f;
		//	samplDesc.BorderColor[3] = 1.0f;
		//	samplDesc.MaxLOD = static_cast<float>(INT_MAX);
		//}
		//game->Device->CreateSamplerState(&samplDesc, &quadSampler);
		//
		//D3D11_RASTERIZER_DESC rastDesc = {};
		//{
		//	rastDesc.CullMode = D3D11_CULL_NONE;
		//	rastDesc.FillMode = D3D11_FILL_SOLID;
		//}
		//
		//game->Device->CreateRasterizerState(&rastDesc, &quadRastState);
	}


	void DebugRenderSysImpl::InitMeshes()
	{
		//ID3DBlob* errorCode;
		//
		//D3DCompileFromFile(L"Shaders/Simple.hlsl", nullptr, nullptr, "VSMainMesh", "vs_5_0", D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &vertexMeshCompResult, &errorCode);
		//game->Device->CreateVertexShader(vertexMeshCompResult->GetBufferPointer(), vertexMeshCompResult->GetBufferSize(), nullptr, &vertexMeshShader);
		//
		//if (errorCode) errorCode->Release();
		//
		//D3DCompileFromFile(L"Shaders/Simple.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &pixelMeshCompResult, &errorCode);
		//game->Device->CreatePixelShader(pixelMeshCompResult->GetBufferPointer(), pixelMeshCompResult->GetBufferSize(), nullptr, &pixelMeshShader);
		//
		//if (errorCode) errorCode->Release();
		//
		//meshLayout = VertexPositionNormalBinormalTangentColorTex::GetLayout(vertexMeshCompResult);
		//
		//D3D11_BUFFER_DESC bufDesc = {};
		//{
		//	bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		//	bufDesc.CPUAccessFlags = 0;
		//	bufDesc.MiscFlags = 0;
		//	bufDesc.Usage = D3D11_USAGE_DEFAULT;
		//	bufDesc.ByteWidth = sizeof(MeshConstData);
		//}
		//
		//game->Device->CreateBuffer(&bufDesc, nullptr, &meshBuf);
	}


	void DebugRenderSysImpl::DrawPrimitives(Camera camera, Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue,
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
		const D3D12_VIEWPORT* m_Viewport,
		const D3D12_RECT* m_ScissorRect,
		IRenderTargetProvider* renderTarget,
		uint32_t currIndex)
	{
		if (isPrimitivesDirty) {
			UpdateLinesBuffer(currIndex);
			pointsCount = static_cast<int>(lines.size());
		
			isPrimitivesDirty = false;
		}
		
		if (pointsCount == 0)
		{
			return;
		}
		
		/*if(camera == nullptr)
			camera = GameFramework::Get()->GameCamera;*/

		//auto commandQueue = GameFramework::Get()->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
		//auto commandList = commandQueue->GetCommandList();
		//Window* window = GameFramework::Get()->MainWindow;

		auto rtv = renderTarget->GetCurrentRTV();
		auto dsv = renderTarget->GetDSV();
		commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

		commandList->SetPipelineState(m_PipelineState.Get());
		commandList->SetGraphicsRootSignature(m_RootSignature.Get());

		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

		//uint32_t currIndex = GameFramework::Get()->GetCurrentIndex();

		D3D12_VERTEX_BUFFER_VIEW m_VertexBufferView;
		m_VertexBufferView.BufferLocation = m_vertexBufs[currIndex]->Resource()->GetGPUVirtualAddress();
		m_VertexBufferView.SizeInBytes = sizeof(VertexPositionColor) * MaxPointsCount;
		m_VertexBufferView.StrideInBytes = sizeof(VertexPositionColor);
		commandList->IASetVertexBuffers(0, 1, &m_VertexBufferView);

		commandList->RSSetViewports(1, m_Viewport);
		commandList->RSSetScissorRects(1, m_ScissorRect);


		Matrix modelMatrix = Matrix::Identity;
		//Matrix mvpMatrix = modelMatrix * camera->GetCameraMatrix(); // mCamera.GetView() * mCamera.GetProj()
		Matrix mvpMatrix = modelMatrix * camera.GetView() * camera.GetProj();
		commandList->SetGraphicsRoot32BitConstants(0, sizeof(Matrix) / 4, &mvpMatrix, 0);


		commandList->DrawInstanced(pointsCount, 1, 0, 0);

		//ID3D12CommandList* cmdsLists[] = { commandList.Get() };
		//commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
		//commandQueue->ExecuteCommandList(commandList);
	}


	void DebugRenderSysImpl::DrawQuads()
	{
		//if (quads.empty()) return;
		//
		//game->Context->OMSetDepthStencilState(depthState, 0);
		//game->Context->RSSetState(quadRastState);
		//
		//game->Context->VSSetShader(vertexQuadShader, nullptr, 0);
		//game->Context->PSSetShader(pixelQuadShader, nullptr, 0);
		//
		//game->Context->IASetInputLayout(quadLayout);
		//game->Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		//
		//const UINT offset = 0;
		//game->Context->IASetVertexBuffers(0, 1, &quadBuf, &quadBindingStride, &offset);
		//
		//game->Context->VSSetConstantBuffers(0, 1, &constBuf);
		//
		//for (auto& quad : quads) {
		//	auto mat = quad.TransformMat * quadProjMatrix;
		//	game->Context->UpdateSubresource(constBuf, 0, nullptr, &mat, 0, 0);
		//
		//	game->Context->PSSetShaderResources(0, 1, &quad.Srv);
		//	game->Context->PSSetSamplers(0, 1, &quadSampler);
		//
		//	game->Context->Draw(4, 0);
		//}
	}


	void DebugRenderSysImpl::DrawMeshes()
	{
		//if (meshes.empty()) return;
		//
		//game->Context->OMSetDepthStencilState(depthState, 0);
		//game->Context->RSSetState(rastState);
		//
		//game->Context->VSSetShader(vertexMeshShader, nullptr, 0);
		//game->Context->PSSetShader(pixelMeshShader, nullptr, 0);
		//
		//game->Context->IASetInputLayout(meshLayout);
		//game->Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		//
		//game->Context->VSSetConstantBuffers(1, 1, &meshBuf);
		//
		//const UINT offset = 0;
		//for (auto& mesh : meshes) {
		//	game->Context->IASetVertexBuffers(0, 1, &mesh.Mesh->VertexBuffer, &mesh.Mesh->Stride, &offset);
		//	game->Context->IASetIndexBuffer(mesh.Mesh->IndexBuffer, DXGI_FORMAT_R32_UINT, 0);
		//
		//	MeshConstData data = MeshConstData{
		//		mesh.Transform * camera->ViewMatrix * camera->ProjMatrix,
		//		mesh.Color
		//	};
		//
		//	//game->Context->UpdateSubresource(meshBuf, 0, nullptr, &data, 0, 0);
		//
		//	//game->Context->DrawIndexed(mesh.Mesh->IndexCount, 0, 0);
		//}
	}


	void DebugRenderSysImpl::UpdateLinesBuffer(uint32_t currIndex)
	{
		//uint32_t currIndex = GameFramework::Get()->GetCurrentIndex();
		m_vertexBufs[currIndex]->CopyElementsData(lines.data(), lines.size());
	}


	void DebugRenderSysImpl::SetCamera(Camera* inCamera)
	{
		camera = inCamera;
	}


	void DebugRenderSysImpl::Draw(Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue,
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
		const D3D12_VIEWPORT* m_Viewport,
		const D3D12_RECT* m_ScissorRect,
		IRenderTargetProvider* renderTarget,
		uint32_t currIndex)
	{
		DrawPrimitives(*camera,
			commandQueue,
			commandList,
			m_Viewport,
			m_ScissorRect,
			renderTarget,
			currIndex);
		DrawQuads();
		DrawMeshes();
	}


	void DebugRenderSysImpl::Clear()
	{
		lines.clear();
		isPrimitivesDirty = true;
		//quads.clear();
		//meshes.clear();
	}


	void DebugRenderSysImpl::DrawBoundingBox(const DirectX::BoundingBox& box)
	{
		Vector3 corners[8];

		box.GetCorners(&corners[0]);

		DrawLine(corners[0], corners[1], Color(0.0f, 1.0f, 0.0f, 0.2f));
		DrawLine(corners[1], corners[2], Color(0.0f, 1.0f, 0.0f, 0.2f));
		DrawLine(corners[2], corners[3], Color(0.0f, 1.0f, 0.0f, 0.2f));
		DrawLine(corners[3], corners[0], Color(0.0f, 1.0f, 0.0f, 0.2f));

		DrawLine(corners[4], corners[5], Color(0.0f, 1.0f, 1.0f, 0.2f));
		DrawLine(corners[5], corners[6], Color(0.0f, 1.0f, 1.0f, 0.2f));
		DrawLine(corners[6], corners[7], Color(0.0f, 1.0f, 1.0f, 0.2f));
		DrawLine(corners[7], corners[4], Color(0.0f, 1.0f, 1.0f, 0.2f));

		DrawLine(corners[0], corners[4], Color(0.0f, 0.0f, 1.0f, 0.2f));
		DrawLine(corners[1], corners[5], Color(0.0f, 0.0f, 1.0f, 0.2f));
		DrawLine(corners[2], corners[6], Color(0.0f, 0.0f, 1.0f, 0.2f));
		DrawLine(corners[3], corners[7], Color(0.0f, 0.0f, 1.0f, 0.2f));
	}

	void DebugRenderSysImpl::DrawBoundingBox(const DirectX::BoundingBox& box, const DirectX::SimpleMath::Color& color)
	{
		Vector3 corners[8];

		box.GetCorners(&corners[0]);

		DrawLine(corners[0], corners[1], color);
		DrawLine(corners[1], corners[2], color);
		DrawLine(corners[2], corners[3], color);
		DrawLine(corners[3], corners[0], color);

		DrawLine(corners[4], corners[5], color);
		DrawLine(corners[5], corners[6], color);
		DrawLine(corners[6], corners[7], color);
		DrawLine(corners[7], corners[4], color);

		DrawLine(corners[0], corners[4], color);
		DrawLine(corners[1], corners[5], color);
		DrawLine(corners[2], corners[6], color);
		DrawLine(corners[3], corners[7], color);
	}


	void DebugRenderSysImpl::DrawBoundingBox(const DirectX::BoundingBox& box, const DirectX::SimpleMath::Matrix& transform)
	{
		Vector3 corners[8];
		box.GetCorners(&corners[0]);

		for (auto& corner : corners) {
			corner = Vector3::Transform(corner, transform);
		}

		DrawLine(corners[0], corners[1], Color(0.0f, 1.0f, 0.0f, 1.0f));
		DrawLine(corners[1], corners[2], Color(0.0f, 1.0f, 0.0f, 1.0f));
		DrawLine(corners[2], corners[3], Color(0.0f, 1.0f, 0.0f, 1.0f));
		DrawLine(corners[3], corners[0], Color(0.0f, 1.0f, 0.0f, 1.0f));

		DrawLine(corners[4], corners[5], Color(0.0f, 1.0f, 1.0f, 1.0f));
		DrawLine(corners[5], corners[6], Color(0.0f, 1.0f, 1.0f, 1.0f));
		DrawLine(corners[6], corners[7], Color(0.0f, 1.0f, 1.0f, 1.0f));
		DrawLine(corners[7], corners[4], Color(0.0f, 1.0f, 1.0f, 1.0f));

		DrawLine(corners[0], corners[4], Color(0.0f, 0.0f, 1.0f, 1.0f));
		DrawLine(corners[1], corners[5], Color(0.0f, 0.0f, 1.0f, 1.0f));
		DrawLine(corners[2], corners[6], Color(0.0f, 0.0f, 1.0f, 1.0f));
		DrawLine(corners[3], corners[7], Color(0.0f, 0.0f, 1.0f, 1.0f));
	}

	void DebugRenderSysImpl::DrawLine(const DirectX::SimpleMath::Vector3& pos0, const DirectX::SimpleMath::Vector3& pos1,
		const DirectX::SimpleMath::Color& color)
	{
		lines.emplace_back(VertexPositionColor
			{
				pos0,
				color.ToVector4()
			});
		lines.emplace_back(VertexPositionColor
			{
				pos1,
				color.ToVector4()
			});
		
		isPrimitivesDirty = true;
	}


	void DebugRenderSysImpl::DrawArrow(const DirectX::SimpleMath::Vector3& p0, const DirectX::SimpleMath::Vector3& p1,
		const DirectX::SimpleMath::Color& color, const DirectX::SimpleMath::Vector3& n)
	{
		DrawLine(p0, p1, color);

		auto a = Vector3::Lerp(p0, p1, 0.85f);

		auto diff = p1 - p0;
		auto side = n.Cross(diff) * 0.05f;

		DrawLine(a + side, p1, color);
		DrawLine(a - side, p1, color);
	}


	void DebugRenderSysImpl::DrawPoint(const DirectX::SimpleMath::Vector3& pos, const float& size)
	{
		lines.emplace_back(VertexPositionColor
			{
				Vector3(pos.x + size, pos.y, pos.z),
				Vector4(1.0f, 0.0f, 0.0f, 1.0f)
			});
		lines.emplace_back(VertexPositionColor
			{
				Vector3(pos.x - size, pos.y, pos.z),
				Vector4(1.0f, 0.0f, 0.0f, 1.0f)
			});
		lines.emplace_back(VertexPositionColor
			{
				Vector3(pos.x, pos.y + size, pos.z),
				Vector4(0.0f, 1.0f, 0.0f, 1.0f)
			});
		lines.emplace_back(VertexPositionColor
			{
				Vector3(pos.x, pos.y - size, pos.z),
				Vector4(0.0f, 1.0f, 0.0f, 1.0f)
			});
		lines.emplace_back(VertexPositionColor
			{
				Vector3(pos.x, pos.y, pos.z + size),
				Vector4(0.0f, 0.0f, 1.0f, 1.0f)
			});
		lines.emplace_back(VertexPositionColor
			{
				Vector3(pos.x, pos.y, pos.z - size),
				Vector4(0.0f, 0.0f, 1.0f, 1.0f)
			});
		
		isPrimitivesDirty = true;
	}


	void DebugRenderSysImpl::DrawCircle(const double& radius, const DirectX::SimpleMath::Color& color, const DirectX::SimpleMath::Matrix& transform, int density)
	{
		double angleStep = DirectX::XM_PI * 2 / density;

		for (int i = 0; i < density; i++)
		{
			auto point0X = radius * cos(angleStep * i);
			auto point0Y = radius * sin(angleStep * i);

			auto point1X = radius * cos(angleStep * (i + 1));
			auto point1Y = radius * sin(angleStep * (i + 1));

			auto p0 = Vector3::Transform(Vector3(static_cast<float>(point0X), static_cast<float>(point0Y), 0), transform);
			auto p1 = Vector3::Transform(Vector3(static_cast<float>(point1X), static_cast<float>(point1Y), 0), transform);

			DrawLine(p0, p1, color);
		}
	}


	void DebugRenderSysImpl::DrawSphere(const double& radius, const DirectX::SimpleMath::Color& color, const DirectX::SimpleMath::Matrix& transform, int density)
	{
		DrawCircle(radius, color, transform, density);
		DrawCircle(radius, color, Matrix::CreateRotationX(DirectX::XM_PIDIV2) * transform, density);
		DrawCircle(radius, color, Matrix::CreateRotationY(DirectX::XM_PIDIV2) * transform, density);
	}

	void DebugRenderSysImpl::DrawPlane(const DirectX::SimpleMath::Vector4& p, const DirectX::SimpleMath::Color& color, float sizeWidth, float sizeNormal, bool drawCenterCross)
	{
		auto dir = Vector3(p.x, p.y, p.z);
		if (dir.Length() == 0.0f) return;
		dir.Normalize();

		auto up = Vector3(0, 0, 1);
		auto right = dir.Cross(up);
		if (right.Length() < 0.01f) {
			up = Vector3(0, 1, 0);
			right = dir.Cross(up);
		}
		right.Normalize();

		up = right.Cross(dir);

		auto pos = -dir * p.w;

		auto leftPoint = pos - right * sizeWidth;
		auto rightPoint = pos + right * sizeWidth;
		auto downPoint = pos - up * sizeWidth;
		auto upPoint = pos + up * sizeWidth;

		DrawLine(leftPoint + up * sizeWidth, rightPoint + up * sizeWidth, color);
		DrawLine(leftPoint - up * sizeWidth, rightPoint - up * sizeWidth, color);
		DrawLine(downPoint - right * sizeWidth, upPoint - right * sizeWidth, color);
		DrawLine(downPoint + right * sizeWidth, upPoint + right * sizeWidth, color);


		if (drawCenterCross) {
			DrawLine(leftPoint, rightPoint, color);
			DrawLine(downPoint, upPoint, color);
		}

		DrawPoint(pos, 0.5f);
		DrawArrow(pos, pos + dir * sizeNormal, color, right);
	}


	void DebugRenderSysImpl::DrawFrustrum(const DirectX::SimpleMath::Matrix& view, const DirectX::SimpleMath::Matrix& proj)
	{
		const auto corners = MathHelper::GetFrustumCornersWorldSpace(view, proj);

		//for (int i = 0; i < corners.size(); ++i) {
		//	DrawPoint(corners[i].ToVec3(), 2.0f * (i+1));
		//}

		auto invView = view.Invert();
		DrawPoint(invView.Translation(), 1.0f);

		DrawLine(corners[0], corners[1], Vector4(0.0f, 0.0f, 1.0f, 1.0f));
		DrawLine(corners[2], corners[3], Vector4(0.0f, 0.0f, 1.0f, 1.0f));
		DrawLine(corners[4], corners[5], Vector4(0.0f, 0.0f, 1.0f, 1.0f));
		DrawLine(corners[6], corners[7], Vector4(0.0f, 0.0f, 1.0f, 1.0f));

		DrawLine(corners[0], corners[2], Vector4(0.0f, 1.0f, 0.0f, 1.0f));
		DrawLine(corners[1], corners[3], Vector4(0.0f, 0.5f, 0.0f, 1.0f));
		DrawLine(corners[4], corners[6], Vector4(0.0f, 1.0f, 0.0f, 1.0f));
		DrawLine(corners[5], corners[7], Vector4(0.0f, 0.5f, 0.0f, 1.0f));

		DrawLine(corners[0], corners[4], Vector4(1.0f, 0.0f, 0.0f, 1.0f));
		DrawLine(corners[1], corners[5], Vector4(0.5f, 0.0f, 0.0f, 1.0f));
		DrawLine(corners[2], corners[6], Vector4(1.0f, 0.0f, 0.0f, 1.0f));
		DrawLine(corners[3], corners[7], Vector4(0.5f, 0.0f, 0.0f, 1.0f));
	}


	void DebugRenderSysImpl::DrawTextureOnScreen(ComPtr<ID3D12Resource> tex, int x, int y, int width, int height, int zOrder)
	{
		//if (quads.size() >= QuadMaxDrawCount) return;
		//
		//QuadInfo quad = {};
		////quad.Srv = tex;
		//quad.TransformMat = Matrix::CreateScale(static_cast<float>(width), static_cast<float>(height), 1.0f)
		//	* Matrix::CreateTranslation(static_cast<float>(x), static_cast<float>(y), static_cast<float>(zOrder));
		//
		//quads.emplace_back(quad);
	}


	/*void DebugRenderSysImpl::DrawStaticMesh(const StaticMesh& mesh, const DirectX::SimpleMath::Matrix& transform, const DirectX::SimpleMath::Color& color)
	{
		MeshInfo meshInfo = {
			&mesh,
			color.ToVector4(),
			transform
		};

		meshes.emplace_back(meshInfo);
	}*/
}

