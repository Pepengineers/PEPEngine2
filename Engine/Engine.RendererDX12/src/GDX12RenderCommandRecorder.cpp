#include "Engine.RendererDX12/GDX12RenderCommandRecorder.h"

GDX12RenderCommandRecorder::GDX12RenderCommandRecorder() : _cameraCBIndex(0)
{
}

void GDX12RenderCommandRecorder::DrawMesh(MeshHandle mesh, std::vector<GDX12Material*> materials, int transformCBIndex)
{
	_drawMeshCommands.push_back(DrawMeshCommand(mesh, materials, transformCBIndex));
}

void GDX12RenderCommandRecorder::DrawFromCamera(int cameraCBIndex)
{
	_cameraCBIndex = cameraCBIndex;
}

void GDX12RenderCommandRecorder::ClearCommands()
{
	_drawMeshCommands.clear();
	_cameraCBIndex = 0;
}


