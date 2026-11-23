#include "BistroApplication.h"
#include "BistroIniMigration.h"
#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/BistroLightPlacements.h"
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
#include <unordered_map>
#include <WindowsX.h>
#include <shellapi.h>
#include "Engine.UI/UI_test.h"

namespace
{
#if !defined(_DEBUG) && !defined(DEBUG)
bool SkipAutomaticBenchmark()
{
	if (BistroBenchmark::SkipBenchmark.GetValue()) return true;
	int count = 0;
	auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
	if (!args) return false;
	const bool skip = std::any_of(args + 1, args + count, [](const wchar_t* arg)
	{
		return _wcsicmp(arg, L"-SkipBenchmark") == 0;
	});
	LocalFree(args);
	return skip;
}
#endif

struct BistroLocalShadowSettings
{
	explicit BistroLocalShadowSettings(const std::wstring& name)
	    : Adapter(L"Render.Shadows.Local." + name + L".Adapter", -1, L"Hardware GPU index; -1 uses GPU0",
	              EConsoleVariableFlags::Default, [](int index) { return index >= -1; }),
	      CrossSync(L"Render.Shadows.Local." + name + L".CrossSync", 1, L"Remote shadow freshness: 1 Exact, 3 Latest",
	                EConsoleVariableFlags::Default, [](int mode) { return mode == 1 || mode == 3; })
	{}
	AutoConsoleVariable<int> Adapter, CrossSync;
};

const auto LocalShadowSettings = []
{
	std::unordered_map<std::string, std::unique_ptr<BistroLocalShadowSettings>> settings;
	for (const auto& fixture : BistroLightPlacements)
	{
		const std::string name = fixture.Name;
		settings.emplace(name, std::make_unique<BistroLocalShadowSettings>(std::wstring(name.begin(), name.end())));
	}
	return settings;
}();

void UpdateLocalShadowSettings(World& world)
{
	world.GetECS().ForEach<NameComponent, LightComponent>([](Entity, const NameComponent& name, LightComponent& light)
	{
		if (light.Type == LightType::Directional) return;
		const auto settings = LocalShadowSettings.find(name.value);
		if (settings == LocalShadowSettings.end()) return;
		light.ShadowAdapter = settings->second->Adapter.GetValue();
		light.CrossSync = static_cast<ShadowCrossSync>(settings->second->CrossSync.GetValue());
	});
}

constexpr float SunDiskScale = 2.5f;
constexpr float DaylightSunElevation = 70.f;
constexpr float MouseDegreesPerPixel = 0.15f;
constexpr float CameraPitchLimit = 89.0f;
constexpr float CameraSpeedStep = 1.25f;
constexpr float MinCameraSpeed = 0.1f;
constexpr float MaxCameraSpeed = 10000.0f;
constexpr float CameraSpeedBoost = 4.0f;
const Vector3 InitialCameraPosition{400, 230, -370};
const Vector3 InitialCameraRotation{8, 40, 0};

AutoConsoleVariable<int> BistroSkySource(L"Render.Sky.Source", 2,
                                         L"Ordinary Bistro sky: 0 Off, 1 original SkyBox, 2 SkyAtmosphere",
                                         EConsoleVariableFlags::Default,
                                         [](int value) { return value >= 0 && value <= 2; });
AutoConsoleVariable<bool> BistroSunCycle(
    L"Render.Sky.SunCycle.Enabled", true,
    L"Start in daylight, fade to dusk, and return to daylight");
AutoConsoleVariable<float> BistroSunCyclePeriod(L"Render.Sky.SunCycle.PeriodSeconds", 120.f,
                                                L"Seconds for a full daylight to dusk and back cycle",
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
        {L"App.UI.CloudPicture.Adapter", "-1"},
        {L"Render.AO.Adapter", "-1"},
        {L"Render.Particles.Adapter", "-1"},
        {L"Render.Water.Adapter", "-1"},
        {L"Render.Sky.SkyView.Adapter", "-1"},
        {L"Render.Sky.AerialPerspective.Adapter", "-1"},
        {L"Render.Wind.Adapter", "-1"}};
    for (const auto& poster : BistroUICanvases::Posters)
        if (poster.Adapter) reset.emplace_back(poster.Adapter, "-1");
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
	const auto queueDefaults = BistroBenchmark::DefaultsPath();
	const auto overlay = std::filesystem::exists(queueDefaults) ? queueDefaults : std::filesystem::path{};
	console.InitializeWithOverlay(overlay, {}, [&] {
		adapters = GDX12DeviceFactory::GetDeviceDescriptors();
		ValidateStartupAdapters(console, startup, adapters);
	}, MigrateBistroIniKey);
	if (!overlay.empty())
		Log::Info("BistroBenchmark", "Loaded startup queue defaults from ", overlay.u8string());
	Log::Info("Bistro", "SCENE EXECUTOR; primary=", console.FindConsoleVariable(L"Render.PrimaryAdapter").GetInt(),
	          "; Render.Queues.AsyncEnabled=",
	          console.FindConsoleVariable(L"Render.Queues.AsyncEnabled").GetBool(),
	          " Render.Water.Adapter=", console.FindConsoleVariable(L"Render.Water.Adapter").GetInt(),
	          " Render.Water.CrossSync=", console.FindConsoleVariable(L"Render.Water.CrossSync").GetInt());
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
		UpdateLocalShadowSettings(world);
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
		_benchmark.Initialize(world, _bistroCharacters, *_bistroPipeline);
		Engine::UI::EnableApplicationUIInput();
		Engine::UI::HideSettings();
		Engine::UI::SetDebugReconstructionStatusVisible(false);
		IndirectCommandBuildSystem commands;
		commands.Schedule(GetJobs(), world, 0).Complete();
#if !defined(_DEBUG) && !defined(DEBUG)
		_autoBenchmarkPending = BistroBenchmark::AutoStart.GetValue() && !SkipAutomaticBenchmark();
#endif
		Log::Info("Bistro",
		          "Loaded Bistro interior/exterior; fixed-step interior physics enabled; sun and local shadows");
		return true;
	}
	void Update(const GameTimer& timer) override
	{
		auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
		auto render = Locator.GetModule<RenderModule>();
		if (_autoBenchmarkPending && _bistroPipeline->PresentedFrames())
		{
			_autoBenchmarkPending = false;
			_benchmark.RequestAutomatic();
		}
		_info.ApplyResolution(*GetWindow());
		_benchmark.Update(world, *Locator.GetModule<ConsoleModule>(), *render, *_bistroPipeline, *GetWindow());
		if (_benchmark.Active() && (!_runActive || _legNumber != _benchmark.LegNumber()))
		{
			_info.Hide();
			Engine::UI::HideSettings();
			_legNumber = _benchmark.LegNumber();
			RefreshWindowCaption();
		}
		if (!_benchmark.Active() && _runActive)
		{
			_info.ShowFull();
			RefreshWindowCaption();
		}
		_runActive = _benchmark.Active();
		Engine::UI::SetSettingsEditingEnabled(!_runActive);
		UpdateLocalShadowSettings(world);
		_bistroReflections.Update(world, timer.DeltaTime());
		const float sceneDelta = world.IsPaused() ? 0.f : timer.DeltaTime() * world.GetTimeScale();
		UpdateSky(world, sceneDelta);
		_bistroWater.Update(world, timer.DeltaTime());
		_bistroCharacters.Update(world, timer.DeltaTime(), GetJobs());
		_bistroGrass.Update(world, timer.DeltaTime());
		_bistroBounds->UpdateEmitters(world);
		BistroParticles::Update(world);
		App::Update(timer);
		_bistroReflections.DrawBounds(world, *render);
		_info.Draw(*render, *_bistroPipeline, _benchmark, timer.DeltaTime());
	}
	void Render(const GameTimer& timer) override
	{
		const auto presented = _bistroPipeline->PresentedFrames();
		App::Render(timer);
		if (_benchmark.Active())
			_benchmark.FrameRendered(*Locator.GetModule<SceneManagerModule>()->GetWorld(), *Locator.GetModule<ConsoleModule>(),
			    *Locator.GetModule<RenderModule>(), *_bistroPipeline, _bistroPipeline->PresentedFrames() != presented);
	}
	int Run()
	{
		try
		{
			const auto result = App::Run();
			StopBenchmark();
			return result;
		}
		catch (...)
		{
			StopBenchmark("error");
			throw;
		}
	}

  private:
	std::wstring StatusCaption() const override
	{
		std::wstring status;
		if (_benchmark.Active()) status = _benchmark.Caption() + L" | Esc: cancel | ";
		return status + L"F1: CVars | F6: compact | F7: full | F8: textures";
	}
	bool OnKeyReleased(WPARAM key) override
	{
		if (key == VK_ESCAPE && _benchmark.Active()) { _benchmark.RequestCancel(); return true; }
		if (key == VK_F6 || key == VK_F7) { _info.Toggle(key == VK_F6); return true; }
		if (key == VK_F8)
		{
			using BistroRender::SceneTextureInspector;
			if (!_benchmark.Active()) SceneTextureInspector::SetVisible(!SceneTextureInspector::Visible());
			return true;
		}
		return false;
	}
	bool _runActive = false;
	uint64_t _legNumber = 0;
	void StopBenchmark(const char* status = "cancelled")
	{
		if (_benchmark.Active())
			_benchmark.Stop(*Locator.GetModule<SceneManagerModule>()->GetWorld(), *Locator.GetModule<ConsoleModule>(),
			    *_bistroPipeline, status);
		Engine::UI::SetSettingsEditingEnabled(true);
	}
	std::vector<DeviceDesc> _adapters;
	Entity _skyEntity = InvalidEntity;
	double _sunCyclePhase = 0;
	BistroGrass::Scene _bistroGrass;
	BistroCharacters::Scene _bistroCharacters{20261122};
	BistroWater::Scene _bistroWater;
	BistroReflections::Scene _bistroReflections{1, true};
	BistroBenchmark _benchmark;
	bool _autoBenchmarkPending = false;
	BistroInfoPanel _info;
	BistroRenderPipeline* _bistroPipeline = nullptr;
	std::unique_ptr<BistroBounds::Scene> _bistroBounds;
	POINT _lastMousePos{};
	float _cameraSpeed = 500.f;
	void InitializeSky(World& world)
	{
		_sunCyclePhase = 0;
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
			auto angles = world.GetECS().Get<TransformComponent>(_skyEntity).Rotation;
			if (BistroSunCycle.GetValue())
			{
				_sunCyclePhase = std::fmod(_sunCyclePhase + double(deltaTime) / BistroSunCyclePeriod.GetValue(), 1.0);
				const double dusk = 0.5 * (1.0 - std::cos(_sunCyclePhase * XM_2PI));
				angles.x = DaylightSunElevation * float(1.0 - dusk * dusk);
				angles.z = 0;
			}
			const auto rotation = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(angles.y),
			                                                     XMConvertToRadians(angles.x),
			                                                     XMConvertToRadians(angles.z));
			auto& atmosphere = world.GetECS().Get<SkyAtmosphereComponent>(_skyEntity);
			const Vector3 directionToSun = rotation.Forward();
			if (atmosphere.GetSettings().DirectionToSun != directionToSun)
				atmosphere.UpdateSunDirection(directionToSun);
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
		if (paused && _bistroPipeline)
		{
			StopBenchmark();
			_bistroPipeline->PauseIndependentClocks();
		}
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
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
