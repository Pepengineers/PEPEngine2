#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include "Engine.RendererDX12/GDX12Material.h"

struct RenderCommand
{

};

struct DrawMeshCommand : RenderCommand
{
	MeshHandle Mesh;
	std::vector<GDX12Material*> Materials;
	int TransformCBIndex;

	DrawMeshCommand(MeshHandle mesh, std::vector<GDX12Material*> materials, int transformCBIndex)
		: Mesh(mesh), Materials(materials), TransformCBIndex(transformCBIndex)
	{

	}
};

class GDX12RenderCommandRecorder
{
public:
	GDX12RenderCommandRecorder();

	void DrawMesh(MeshHandle mesh, std::vector<GDX12Material*> materials, int transformCBIndex);
	void DrawFromCamera(int cameraCBIndex);
	void ClearCommands();

private:
	friend class RenderModule;

	std::vector<DrawMeshCommand> _drawMeshCommands;
	int _cameraCBIndex;
};