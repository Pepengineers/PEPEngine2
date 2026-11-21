#include "BistroApplication.h"
#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/App.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.Core/AssetManager.h"
#include "Components/BistroGrassComponents.h"
#include "Techniques/Grass/BistroGrass.h"
#include "BistroScene.h"
#include "BistroBounds.h"
#include "BistroCharacters.h"
#include "Techniques/Water/BistroWater.h"
#include "BistroReflections.h"
#include "BistroBenchmark.h"
#include "Techniques/UI/BistroInfoPanel.h"
#include "Techniques/UI/BistroUICanvases.h"
#include "Components/BistroInteractionSource.h"
#include "Components/FluidRenderComponent.h"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <WindowsX.h>
#include "Engine.UI/UI_test.h"

namespace
{
constexpr float SunDiskScale = 2.5f;
constexpr float MouseDegreesPerPixel = 0.15f;
constexpr float CameraPitchLimit = 89.0f;
constexpr float CameraSpeedStep = 1.25f;
constexpr float MinCameraSpeed = 0.1f;
constexpr float MaxCameraSpeed = 10000.0f;
constexpr float CameraSpeedBoost = 4.0f;
const Vector3 InitialCameraPosition{400, 230, -370};
const Vector3 InitialCameraRotation{8, 40, 0};

AutoConsoleVariable<int> BistroSkySource(L"App.Sky.Source", 2,
                                         L"Ordinary Bistro sky: 0 Off, 1 original SkyBox, 2 SkyAtmosphere",
                                         EConsoleVariableFlags::Default,
                                         [](int value) { return value >= 0 && value <= 2; });
AutoConsoleVariable<bool> BistroSunCycle(
    L"App.Sky.SunCycle.Enabled", true,
    L"Move the original Sun through a repeating daylight arc; disable for manual rotation");
AutoConsoleVariable<float> BistroSunCyclePeriod(L"App.Sky.SunCycle.PeriodSeconds", 120.f,
                                                L"Seconds from sunrise through zenith to sunset; repeats without night",
                                                EConsoleVariableFlags::Default,
                                                [](float value) { return std::isfinite(value) && value > 0; });
void Require(bool value, const char* message)
{
	if (!value)
	{
		throw std::runtime_error(message);
	}
}

void ValidateStartupAdapters(ConsoleModule& console, const std::filesystem::path& startup,
                             const std::vector<DeviceDesc>& adapters)
{
    Require(!adapters.empty(), "Bistro requires a hardware GPU");
    for (size_t index = 0; index < adapters.size(); ++index)
        Log::Info("Bistro", "Hardware GPU [", index, "] ", adapters[index].Name);

    ConsoleVariablePatch reset{
        {L"Render.PrimaryAdapter", "0"},
        {L"App.CloudPicture.Adapter", "-1"},
        {L"App.AO.Adapter", "-1"},
        {L"App.Particles.Adapter", "-1"},
        {L"App.Water.Adapter", "-1"},
        {L"App.Grass.Wind.Adapter", "-1"}};
    for (const auto& poster : BistroUICanvases::Posters) reset.emplace_back(poster.Adapter, "-1");
    bool invalid = false;
    for (const auto& [name, fallback] : reset)
    {
        const int index = console.FindConsoleVariable(name).GetInt();
        if ((index == -1 && fallback == "-1") || (index >= 0 && size_t(index) < adapters.size())) continue;
        Log::Warning("Bistro", name, " requests missing hardware GPU [", index,
                     "]. Switching all techniques to primary GPU [0].");
        invalid = true;
    }
    if (!invalid) return;

    for (const auto& path : {startup, ConsoleModule::GetDefaultConfigPath()})
    {
        const auto result = console.UpdateIni(path, reset);
        if (!result.Error.empty()) throw ConsoleConfigError(path.u8string() + ": " + result.Error);
    }
    const auto result = console.ApplyPatch(reset);
    if (!result.Error.empty()) throw ConsoleConfigError(result.Error);
}

std::vector<DeviceDesc> PrepareStartup()
{
	const auto root = Engine::Core::GetExecutableDirectory();
	std::filesystem::create_directories(root / "Config");
	const auto startup = root / "Configs/Startup.ini";
	std::filesystem::copy_file(startup, ConsoleModule::GetDefaultConfigPath(),
	                           std::filesystem::copy_options::overwrite_existing);

	ConsoleModule console;
	std::vector<DeviceDesc> adapters;
	console.InitializeWithOverlay({}, {}, [&] {
		adapters = GDX12DeviceFactory::GetDeviceDescriptors();
		ValidateStartupAdapters(console, startup, adapters);
	});
	Log::Info("Bistro", "SCENE EXECUTOR; primary=", console.FindConsoleVariable(L"Render.PrimaryAdapter").GetInt(),
	          "; App.Async.Queues=",
	          console.FindConsoleVariable(L"App.Async.Queues").GetBool(),
	          " App.Water.Adapter=", console.FindConsoleVariable(L"App.Water.Adapter").GetInt(),
	          " App.Water.CrossSync=", console.FindConsoleVariable(L"App.Water.CrossSync").GetInt());
	return adapters;
}

class BistroApplication final : public App
{
  public:
	explicit BistroApplication(std::vector<DeviceDesc> adapters)
	    : App(GetModuleHandleW(nullptr)), _adapters(std::move(adapters)) {}
	bool AddModules() override
	{
		auto pipeline = std::make_unique<BistroRenderPipeline>();
		_bistroPipeline = pipeline.get();
		Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline), std::move(_adapters)));
		const auto scene = std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro";
		for (const auto* part : {"Exterior/exterior.obj", "Interior/interior.obj"})
		{
			Require(std::filesystem::is_regular_file(scene / part), "Missing Bistro scene part");
		}
		Locator.RegisterModule(std::make_shared<SceneManagerModule>(scene));
		return BenchmarkEngine::AddModules();
	}
	bool Initialize() override
	{
		if (!App::Initialize())
		{
			return false;
		}
		auto scenes = Locator.GetModule<SceneManagerModule>();

		auto& world = *scenes->GetWorld();
		world.SetPaused(false);
		InitializeSky(world);
		_bistroReflections.Update(world, 0);
		UpdateSky(world);
		_bistroGrass.Update(world, 0);
		CreateBistroParticles(world);
		_bistroWater.Update(world, 0);
		world.GetECS().ForEach<FluidRenderComponent>(
		    [](Entity, const FluidRenderComponent& fluid)
		    {
			    Log::Info("Bistro", "Bistro water target=", BistroWater::ParticleCount.GetValue(),
			              "; seed particles=", fluid.GetParticles() ? fluid.GetParticles()->size() : 0);
		    });
		_bistroCharacters.Update(world, 0, GetJobs());
		_bistroBounds = std::make_unique<BistroBounds::Scene>(world);
		_bistroBounds->UpdateEmitters(world);
		BistroParticles::Update(world);
		InitializeCamera(world);
		_benchmark.Initialize(world);
		Engine::UI::EnableApplicationUIInput();
		Engine::UI::SetDebugReconstructionStatusVisible(false);
		IndirectCommandBuildSystem commands;
		commands.Schedule(GetJobs(), world, 0).Complete();
		Log::Info("Bistro",
		          "Loaded Bistro interior/exterior; fixed-step interior physics enabled; sun and local shadows");
		return true;
	}
	void Update(const GameTimer& timer) override
	{
		auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
		_benchmark.Update(world, *Locator.GetModule<ConsoleModule>());
		_bistroReflections.Update(world, timer.DeltaTime());
		const float sceneDelta = world.IsPaused() ? 0.f : timer.DeltaTime() * world.GetTimeScale();
		UpdateSky(world, sceneDelta);
		_bistroWater.Update(world, timer.DeltaTime());
		_bistroCharacters.Update(world, timer.DeltaTime(), GetJobs());
		_bistroGrass.Update(world, timer.DeltaTime());
		_bistroBounds->UpdateEmitters(world);
		BistroParticles::Update(world);
		App::Update(timer);
		auto render = Locator.GetModule<RenderModule>();
		_bistroReflections.DrawBounds(world, *render);
		_info.Draw(*render, *_bistroPipeline, _benchmark, timer.DeltaTime());
	}
	void Render(const GameTimer& timer) override
	{
		App::Render(timer);
		if (_benchmark.Active())
			_benchmark.FrameRendered(*Locator.GetModule<SceneManagerModule>()->GetWorld(), *Locator.GetModule<ConsoleModule>());
	}
	int Run()
	{
		const auto result = App::Run();
		if (_benchmark.Active())
			_benchmark.Stop(*Locator.GetModule<SceneManagerModule>()->GetWorld(), *Locator.GetModule<ConsoleModule>());
		return result;
	}

  private:
	std::vector<DeviceDesc> _adapters;
	Entity _skyEntity = InvalidEntity;
	double _sunCyclePhase = 0;
	BistroGrass::Scene _bistroGrass;
	BistroCharacters::Scene _bistroCharacters;
	BistroWater::Scene _bistroWater;
	BistroReflections::Scene _bistroReflections{1, true};
	BistroBenchmark _benchmark;
	BistroInfoPanel _info;
	BistroRenderPipeline* _bistroPipeline = nullptr;
	std::unique_ptr<BistroBounds::Scene> _bistroBounds;
	POINT _lastMousePos{};
	float _cameraSpeed = 500.f;
	void InitializeSky(World& world)
	{
		_skyEntity = world.GetSkyEntity();
		Require(world.GetECS().Has<SkyBoxComponent>(_skyEntity) && world.GetECS().Has<LightComponent>(_skyEntity),
		        "Bistro sky must use the existing Sun");
		if (!world.GetECS().Has<SkyAtmosphereComponent>(_skyEntity))
		{
			world.GetECS().Add<SkyAtmosphereComponent>(_skyEntity);
			auto& atmosphere = world.GetECS().Get<SkyAtmosphereComponent>(_skyEntity);
			auto settings = atmosphere.GetSettings();
			settings.SunAngularRadiusRadians *= SunDiskScale;
			atmosphere.SetSettings(settings);
		}
	}

	void InitializeCamera(World& world)
	{
		auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
		camera.Location = InitialCameraPosition;
		camera.Rotation = InitialCameraRotation;
		camera.DirtyFlag = true;
		world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
	}

	void UpdateSky(World& world, float deltaTime = 0)
	{
		const auto source = static_cast<SkySource>(BistroSkySource.GetValue());
		if (source == SkySource::SkyAtmosphere)
		{
			auto& transform = world.GetECS().Get<TransformComponent>(_skyEntity);
			bool wrapped = false;
			if (BistroSunCycle.GetValue())
			{
				const double nextPhase = _sunCyclePhase + double(deltaTime) / BistroSunCyclePeriod.GetValue();
				wrapped = nextPhase >= 1.0;
				_sunCyclePhase = std::fmod(nextPhase, 1.0);
				transform.Rotation.x = float(_sunCyclePhase * 180.0);
				transform.Rotation.z = 0;
			}
			const auto rotation = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(transform.Rotation.y),
			                                                     XMConvertToRadians(transform.Rotation.x),
			                                                     XMConvertToRadians(transform.Rotation.z));
			auto& atmosphere = world.GetECS().Get<SkyAtmosphereComponent>(_skyEntity);
			auto settings = atmosphere.GetSettings();
			const Vector3 directionToSun = rotation.Forward();
			if (settings.DirectionToSun != directionToSun)
			{
				settings.DirectionToSun = directionToSun;
				if (wrapped)
				{
					atmosphere.SetSettings(settings);
				}
				else
				{
					atmosphere.UpdateSunDirection(directionToSun);
				}
			}
		}
		if (world.GetSkySource() == source && (source == SkySource::Off || world.GetSkyEntity() == _skyEntity))
		{
			return;
		}
		world.SetSkySource(source, _skyEntity);
		Log::Info("Bistro", "Ordinary sky source=", static_cast<unsigned>(source),
		          "; atmosphere quality=", GetSkyAtmosphereQuality(), "; daylight cycle=", BistroSunCycle.GetValue(),
		          "; daylight seconds=", BistroSunCyclePeriod.GetValue());
	}

	void OnPauseChanged(bool paused) override
	{
		if (paused && _bistroPipeline) _bistroPipeline->PauseIndependentClocks();
	}

	void OnMouseDown(WPARAM, int x, int y) override
	{
		_lastMousePos = {x, y};
		if (Engine::UI::WantsMouseInput())
		{
			return;
		}
		SetCapture(GetWindow()->GetWindowHandle());
	}

	void OnMouseUp(WPARAM, int, int) override { ReleaseCapture(); }

	void OnMouseMove(WPARAM buttons, int x, int y) override
	{
		if (!_benchmark.Active() && !Engine::UI::WantsMouseInput() && (buttons & MK_RBUTTON))
		{
			auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
			auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
			camera.Rotation.x = std::clamp(camera.Rotation.x + (y - _lastMousePos.y) * MouseDegreesPerPixel,
			                               -CameraPitchLimit, CameraPitchLimit);
			camera.Rotation.y =
			    std::remainder(camera.Rotation.y - (x - _lastMousePos.x) * MouseDegreesPerPixel, 360.0f);
			camera.DirtyFlag = true;
		}
		_lastMousePos = {x, y};
	}

	void OnMouseWheelMove(WPARAM rotation) override
	{
		if (Engine::UI::WantsMouseInput())
		{
			return;
		}
		_cameraSpeed =
		    std::clamp(_cameraSpeed * (GET_WHEEL_DELTA_WPARAM(rotation) > 0 ? CameraSpeedStep : 1.0f / CameraSpeedStep),
		               MinCameraSpeed, MaxCameraSpeed);
	}

	void OnKeyboardInput(const GameTimer& timer) override
	{
		if (_benchmark.Active() || bInactive || Engine::UI::WantsKeyboardInput())
		{
			return;
		}
		auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
		auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
		const auto rotation = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(camera.Rotation.y),
		                                                     XMConvertToRadians(camera.Rotation.x), 0.0f);
		Vector3 movement = Vector3::Zero;
		if (GetAsyncKeyState('W') & 0x8000)
		{
			movement -= rotation.Forward();
		}
		if (GetAsyncKeyState('S') & 0x8000)
		{
			movement += rotation.Forward();
		}
		if (GetAsyncKeyState('A') & 0x8000)
		{
			movement += rotation.Right();
		}
		if (GetAsyncKeyState('D') & 0x8000)
		{
			movement -= rotation.Right();
		}
		if (GetAsyncKeyState('Q') & 0x8000)
		{
			movement -= Vector3::Up;
		}
		if (GetAsyncKeyState('E') & 0x8000)
		{
			movement += Vector3::Up;
		}
		if (movement.LengthSquared() > 0.0f)
		{
			movement.Normalize();
			const float boost = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? CameraSpeedBoost : 1.0f;
			camera.Location += movement * _cameraSpeed * boost * timer.DeltaTime();
			camera.DirtyFlag = true;
		}
	}
};
} // namespace

int RunBistroApplication()
{
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
	try
	{
		BistroApplication app(PrepareStartup());
		Require(app.Initialize(), "Application initialization failed");
		return app.Run();
	}
	catch (const DxException& error)
	{
		Log::Error("App", error.ToString());
	}
	catch (const std::exception& error)
	{
		Log::Error("App", error.what());
	}
	Log::Flush();
	return 1;
}
