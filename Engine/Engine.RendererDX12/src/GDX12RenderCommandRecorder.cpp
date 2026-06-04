#include "Engine.RendererDX12/GDX12RenderCommandRecorder.h"

GDX12RenderCommandRecorder::GDX12RenderCommandRecorder() : _cameraCBIndex(0)
{
}

void GDX12RenderCommandRecorder::DrawFromCamera(int cameraCBIndex)
{
	_cameraCBIndex = cameraCBIndex;
}

void GDX12RenderCommandRecorder::ClearCommands()
{
	_cameraCBIndex = 0;
}


