#pragma once
#include "App.Base/Modules/SceneManagerModule.h"
#include "Components/BistroWaterComponents.h"
#include "Techniques/Water/GDX12Fluid.h"

class BistroWaterRendering final
{
  public:
	explicit BistroWaterRendering(std::shared_ptr<SceneManagerModule> scenes);
	~BistroWaterRendering();
	std::vector<GDX12FluidInstance> CollectScene(float deltaSeconds, bool advanceWhilePaused = false);

  private:
	void Subscribe(World&);
	void Unsubscribe(World&);
	struct Subscription
	{
		World* Scene;
		ListenerHandle Created, Updated;
	};
	std::shared_ptr<SceneManagerModule> _scenes;
	ListenerHandle _created = 0, _destroyed = 0;
	std::vector<Subscription> _worlds;
	uint64_t _owner = 0;
};
