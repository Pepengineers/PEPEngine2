#pragma once

#include <Engine/RHI/DX12/UploadBuffer.h>
#include <Engine/Math/SimpleMath.h>

#include <Engine/Scene/Camera.h>
#include <Engine/Render/IRenderTargetProvider.h>
#include <Engine/RHI/DX12/d3dx12.h>

using Microsoft::WRL::ComPtr;

namespace gfw
{
	//class Camera;

	struct VertexPositionColor
	{
		DirectX::SimpleMath::Vector3 position;
		DirectX::SimpleMath::Vector4 color;
	};

	class DebugRenderSysImpl
	{
	public:
		friend class GameFramework;

		Camera* camera = nullptr;
		//ID3D11Buffer* constBuf;

		DebugRenderSysImpl(Microsoft::WRL::ComPtr<ID3D12Device> device);

		~DebugRenderSysImpl()
		{
			for (auto& vb : m_vertexBufs)
				vb.reset();

			m_vertexBufs.clear();

			pointsCount = 0;
			isPrimitivesDirty = false;

			lines.clear();

			m_PipelineState.Reset();
			m_RootSignature.Reset();

			/*
			if (quadBuf) quadBuf->Release();
			if (pixelQuadShader) pixelQuadShader->Release();
			if (vertexQuadShader) vertexQuadShader->Release();
			if (pixelQuadCompResult) pixelQuadCompResult->Release();
			if (vertexQuadCompResult) vertexQuadCompResult->Release();
			if (quadLayout) quadLayout->Release();
			if (quadSampler) quadSampler->Release();
			if (quadRastState) quadRastState->Release();
			*/

			// meshes.clear();
		}



#pragma region Primitives
		std::vector<VertexPositionColor> lines;

		//Microsoft::WRL::ComPtr<ID3D12Resource> m_VertexBuffer;
		std::vector<std::unique_ptr<UploadBuffer<VertexPositionColor>>> m_vertexBufs;

		// Root signature
		Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;

		// Pipeline state object.
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PipelineState;

		bool isPrimitivesDirty = false;

		const int MaxPointsCount = 1024 * 1024;
		int pointsCount = 0;

#pragma endregion Primitives

#pragma region Quads

		//DirectX::SimpleMath::Matrix quadProjMatrix;
		//
		//struct QuadInfo
		//{
		//	//ID3D11ShaderResourceView* Srv;
		//	DirectX::SimpleMath::Matrix TransformMat;
		//};
		//
		//std::vector<QuadInfo> quads;
		//
		//const UINT QuadMaxDrawCount = 100;


		//ID3D11Buffer* quadBuf;
		//UINT quadBindingStride;
		//
		//ID3D11PixelShader* pixelQuadShader;
		//ID3D11VertexShader* vertexQuadShader;
		//ID3DBlob* pixelQuadCompResult;
		//ID3DBlob* vertexQuadCompResult;
		//
		//ID3D11InputLayout* quadLayout;
		//ID3D11SamplerState* quadSampler;
		//
		//ID3D11RasterizerState* quadRastState;

#pragma endregion Quads

#pragma region Meshes

		/*struct MeshInfo
		{
			const StaticMesh* Mesh;
			DirectX::SimpleMath::Vector4 Color;
			DirectX::SimpleMath::Matrix Transform;
		};


		struct MeshConstData
		{
			DirectX::SimpleMath::Matrix Transform;
			DirectX::SimpleMath::Vector4 Color;
		};*/

		//std::vector<MeshInfo> meshes;

		//ID3D11VertexShader* vertexMeshShader;
		//ID3D11PixelShader* pixelMeshShader;
		//ID3DBlob* pixelMeshCompResult;
		//ID3DBlob* vertexMeshCompResult;
		//
		//ID3D11InputLayout* meshLayout;
		//
		//ID3D11Buffer* meshBuf;

#pragma endregion Meshes

	protected:

		void InitPrimitives(Microsoft::WRL::ComPtr<ID3D12Device> device);
		void InitQuads();
		void InitMeshes();

		void DrawPrimitives(Camera camera, Microsoft::WRL::ComPtr<ID3D12CommandQueue> mCommandQueue,
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
			const D3D12_VIEWPORT* m_Viewport,
			const D3D12_RECT* m_ScissorRect,
			IRenderTargetProvider* renderTarget,
			uint32_t currIndex);
		void DrawQuads();
		void DrawMeshes();

		void UpdateLinesBuffer(uint32_t currIndex);

	public:
		virtual void SetCamera(Camera* inCamera);
		virtual void Draw(Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue,
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
			const D3D12_VIEWPORT* m_Viewport,
			const D3D12_RECT* m_ScissorRect,
			IRenderTargetProvider* renderTarget,
			uint32_t currIndex);
		virtual void Clear();

	public:
		virtual void DrawBoundingBox(const DirectX::BoundingBox& box);
		virtual void DrawBoundingBox(const DirectX::BoundingBox& box, const DirectX::SimpleMath::Color& color);
		virtual void DrawBoundingBox(const DirectX::BoundingBox& box, const DirectX::SimpleMath::Matrix& transform);
		virtual void DrawLine(const DirectX::SimpleMath::Vector3& pos0, const DirectX::SimpleMath::Vector3& pos1, const DirectX::SimpleMath::Color& color);
		virtual void DrawArrow(const DirectX::SimpleMath::Vector3& p0, const DirectX::SimpleMath::Vector3& p1, const DirectX::SimpleMath::Color& color, const DirectX::SimpleMath::Vector3& n);
		virtual void DrawPoint(const DirectX::SimpleMath::Vector3& pos, const float& size);
		virtual void DrawCircle(const double& radius, const DirectX::SimpleMath::Color& color, const DirectX::SimpleMath::Matrix& transform, int density);
		virtual void DrawSphere(const double& radius, const DirectX::SimpleMath::Color& color, const DirectX::SimpleMath::Matrix& transform, int density);
		virtual void DrawPlane(const DirectX::SimpleMath::Vector4& p, const DirectX::SimpleMath::Color& color, float sizeWidth, float sizeNormal, bool drawCenterCross);

		virtual void DrawFrustrum(const DirectX::SimpleMath::Matrix& view, const DirectX::SimpleMath::Matrix& proj);

		virtual void DrawTextureOnScreen(ComPtr<ID3D12Resource> tex, int x, int y, int width, int height, int zOrder);

		//virtual void DrawStaticMesh(const StaticMesh& mesh, const DirectX::SimpleMath::Matrix& transform, const DirectX::SimpleMath::Color& color);
	};

}