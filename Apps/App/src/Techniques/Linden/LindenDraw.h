#pragma once
#include "Techniques/GeometryTarget.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"

class GDX12DeviceResources;
namespace BistroRender
{
struct LindenMesh;
struct LindenDisplayedPose;
struct NativeWork;
class LindenDraw
{
  public:
	explicit LindenDraw(GDX12DeviceResources&, bool captureOnly = false);
	void Reset() { _mesh.reset(); _buffers.reset(); }
	struct CaptureInputs
	{
		Id Pose, Materials, Main;
		D3D12_GPU_DESCRIPTOR_HANDLE PoseSrv, LocalLights;
		D3D12_GPU_VIRTUAL_ADDRESS MainAddress;
	};
	void Append(Graph&, std::vector<ID3D12Resource*>&, NativeWork&, UINT slot, const GeometryTarget&,
	            const GDX12Descriptor& materials, const std::shared_ptr<const LindenMesh>&, const LindenDisplayedPose&,
	            const CaptureInputs* remote = nullptr);

  private:
	enum class Stage { Depth, DepthNormals, GBuffer, Shadow, Capture };
	GDX12DeviceResources& _resources;
	std::unique_ptr<GDX12RootSignature> _root;
	std::array<ComPtr<ID3D12PipelineState>, 5> _pipelines;
	std::shared_ptr<const LindenMesh> _mesh;
	struct Buffers
	{
		GDX12UploadBuffer<UINT> Indices;
		GDX12UploadBuffer<Vector2> UVs;
		Buffers(GDX12Device* device, UINT indices, UINT vertices, EBufferType type)
		    : Indices(device, indices, type, false), UVs(device, vertices, type, false) {}
	};
	std::shared_ptr<Buffers> _buffers;
};
}
