#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// This pass acts as a breakpoint in the pipeline
// It Doesn't do any GPU commands, but used by Render() function to determine sync points
// It is required to be on both devices
// As a result, devices will wait until both of them reach their respective SyncPass
class GDX12SyncPass : public GDX12RenderPass
{
public:
	GDX12SyncPass() { _flags = RENDER_PASS_FLAG_SYNC_DEVICES; }
};