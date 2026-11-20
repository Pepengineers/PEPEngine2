#pragma once
#include "LindenMesh.h"
#include "Techniques/NativeWork.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <array>

namespace BistroRender
{
struct LindenWindSample;
struct LindenPoseVertex
{
	XMFLOAT3 Position, Normal;
};
static_assert(sizeof(LindenPoseVertex) == 24);

struct LindenDisplayedPose
{
	GDX12UploadBuffer<LindenPoseVertex>* Current = nullptr;
	GDX12UploadBuffer<LindenPoseVertex>* Previous = nullptr;
	uint64_t Revision = 0;
};

class LindenSimulation
{
  public:
	explicit LindenSimulation(GDX12DeviceResources&);
	LindenDisplayedPose Append(Graph&, std::vector<ID3D12Resource*>&, NativeWork&,
	                           const std::shared_ptr<const LindenMesh>&, const LindenWindSample&);
	uint64_t DisplayRevision() const { return _pendingRevision; }

  private:
	struct FragmentPose
	{
		XMFLOAT4 Rotation;
		XMFLOAT3 Translation;
	};
	static_assert(sizeof(FragmentPose) == 28);
	struct Resident
	{
		std::shared_ptr<const LindenMesh> Mesh;
		std::unique_ptr<GDX12UploadBuffer<LindenVertex>> Rest;
		std::unique_ptr<GDX12UploadBuffer<LindenFragment>> Fragments;
		std::unique_ptr<GDX12UploadBuffer<FragmentPose>> Response;
		std::array<std::unique_ptr<GDX12UploadBuffer<LindenPoseVertex>>, 3> Poses;
	};
	struct Frame
	{
		std::unique_ptr<Resident> Retired;
		std::unique_ptr<GDX12UploadBuffer<LindenVertex>> RestUpload;
		std::unique_ptr<GDX12UploadBuffer<LindenFragment>> FragmentUpload;
	};
	GDX12DeviceResources& _resources;
	std::unique_ptr<GDX12RootSignature> _root;
	ComPtr<ID3D12PipelineState> _response, _deform;
	std::unique_ptr<Resident> _resident;
	std::vector<Frame> _frames;
	UINT _lastComputed = 0, _lastDisplayed = 0;
	uint64_t _pendingRevision = 0;
};
}
