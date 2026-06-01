#include "Engine.RendererDX12/GDX12RenderCommandRecorder.h"

GDX12RenderCommandRecorder::GDX12RenderCommandRecorder() : _cameraCBIndex(0), _hasCameraFrustum(false)
{
}

void GDX12RenderCommandRecorder::DrawMesh(
	MeshHandle mesh,
	const std::vector<GDX12Material*>& materials,
	int transformCBIndex,
	const Matrix& world)
{
	_drawMeshCommands.push_back(DrawMeshCommand(mesh, materials, transformCBIndex, world));
}

void GDX12RenderCommandRecorder::DrawFromCamera(int cameraCBIndex, const BoundingFrustum& cameraFrustum)
{
	_cameraCBIndex = cameraCBIndex;
	_cameraFrustum = cameraFrustum;
	_hasCameraFrustum = true;
}

void GDX12RenderCommandRecorder::ClearCommands()
{
	_drawMeshCommands.clear();
	_cameraCBIndex = 0;
	_hasCameraFrustum = false;
}


