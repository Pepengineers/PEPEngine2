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
	const std::vector<GDX12Material*>* Materials;
	int TransformCBIndex;
	Matrix World;

	DrawMeshCommand(
		MeshHandle mesh,
		const std::vector<GDX12Material*>& materials,
		int transformCBIndex,
		const Matrix& world)
		: Mesh(mesh), Materials(&materials), TransformCBIndex(transformCBIndex), World(world)
	{

	}
};

class GDX12RenderCommandRecorder
{
public:
	GDX12RenderCommandRecorder();

	void DrawMesh(MeshHandle mesh, const std::vector<GDX12Material*>& materials, int transformCBIndex, const Matrix& world);
	void DrawFromCamera(int cameraCBIndex, const BoundingFrustum& cameraFrustum);
	void ClearCommands();

private:
	friend class RenderModule;

	std::vector<DrawMeshCommand> _drawMeshCommands;
	int _cameraCBIndex;
	BoundingFrustum _cameraFrustum;
	bool _hasCameraFrustum;
};