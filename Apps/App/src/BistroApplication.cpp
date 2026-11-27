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
#include "BistroTaskbar.h"
#include "Techniques/UI/BistroInfoPanel.h"
#include "Techniques/UI/BistroLoadingScreen.h"
#include "Techniques/UI/BistroUICanvases.h"
#include "Techniques/UI/SceneTextureInspector.h"
#include "Components/BistroInteractionSource.h"
#include "Components/FluidRenderComponent.h"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <optional>
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

bool IsLegacyLocalShadowPlacement(const std::wstring& key)
{
	constexpr std::wstring_view prefix = L"Render.Shadows.Local.";
	if (_wcsnicmp(key.c_str(), prefix.data(), prefix.size()) != 0) return false;
	const auto suffix = key.find_last_of(L'.');
	if (suffix <= prefix.size() ||
	    (_wcsicmp(key.c_str() + suffix, L".Adapter") != 0 && _wcsicmp(key.c_str() + suffix, L".CrossSync") != 0))
		return false;
	const auto fixtureName = key.substr(prefix.size(), suffix - prefix.size());
	return std::any_of(std::begin(BistroLightPlacements), std::end(BistroLightPlacements), [&](const auto& fixture)
	{
		const std::string_view name(fixture.Name);
		const std::wstring wide(name.begin(), name.end());
		return _wcsicmp(wide.c_str(), fixtureName.c_str()) == 0;
	});
}

constexpr float SunDiskScale = 2.5f;
constexpr float DaylightSunElevation = 70.f;
#if defined(_DEBUG) || defined(DEBUG)
constexpr float MouseDegreesPerPixel = 0.15f;
constexpr float CameraPitchLimit = 89.0f;
constexpr float CameraSpeedStep = 1.25f;
constexpr float MinCameraSpeed = 0.1f;
constexpr float MaxCameraSpeed = 10000.0f;
constexpr float CameraSpeedBoost = 4.0f;
#endif
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
        {L"Render.Device.PrimaryAdapter", "0"},
        {L"Render.CloudPicture.Adapter", "-1"},
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
	}, [](std::wstring& key)
	{
		MigrateBistroConfigKey(key);
		if (const auto page = LocalShadowPageSettingsIndex(key))
		{
			RegisterLocalShadowPageSettings(*page);
			return;
		}
		// Fixture placement has no stable mapping to a physical page.
		if (IsLegacyLocalShadowPlacement(key)) key.clear();
	});
	if (!overlay.empty())
		Log::Info("BistroBenchmark", "Loaded startup queue defaults from ", overlay.u8string());
	Log::Info("Bistro", "SCENE EXECUTOR; primary=", console.FindConsoleVariable(L"Render.Device.PrimaryAdapter").GetInt(),
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
		Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline), _adapters, true));
		Locator.RegisterModule(std::make_shared<SceneManagerModule>());
		return BenchmarkEngine::AddModules();
	}
	bool Initialize() override
	{
		if (!App::Initialize())
		{
			return false;
		}
		_info.InitializeFont();
		Engine::UI::EnableApplicationUIInput();
		Engine::UI::HideSettings();
		Engine::UI::SetDebugReconstructionStatusVisible(false);
#if !defined(_DEBUG) && !defined(DEBUG)
		_autoBenchmarkPending = BistroBenchmark::AutoStart.GetValue() && !SkipAutomaticBenchmark();
#endif
		RefreshWindowCaption();
		Log::Info("Bistro", "Benchmark settings ready; VSync on");
		return true;
	}

	void LoadBistro(const BistroRun::Map& map)
	{
		const auto scene = std::filesystem::path(ASSETS_FOLDER) / map.Scene;
		for (const auto* part : {"Exterior/exterior.obj", "Interior/interior.obj"})
			Require(std::filesystem::is_regular_file(scene / part), "Missing Bistro scene part");
		auto render = Locator.GetModule<RenderModule>();
		ReportLoading(L"Initializing the render pipeline and frame resources");
		render->BeginSceneRendering();
		ReportLoading(L"Preparing render targets for the selected output");
		render->OnResize();
		render->ResetWorldHistory();
		auto scenes = Locator.GetModule<SceneManagerModule>();
		_loadingPhase = BistroLoadingScreen::SceneAssets;
		ReportLoading(L"Opening scene files");
		scenes->LoadMap(scene);
		_loadingPhase = BistroLoadingScreen::World;
		ReportLoading(L"Preparing sunlight and reflection probes");
		_map = std::make_unique<MapScene>();
		auto& world = *scenes->GetWorld();
		world.SetPaused(false);
		InitializeSky(world);
		_map->Reflections.Update(world, 0);
		UpdateSky(world);
		ReportLoading(L"Creating vegetation, particles and water");
		_map->Grass.Update(world, 0);
		CreateBistroParticles(world);
		_map->Water.Update(world, 0);
		world.GetECS().ForEach<FluidRenderComponent>(
		    [](Entity, const FluidRenderComponent& fluid)
		    {
			    Log::Info("Bistro", "Bistro water target=", BistroWater::ParticleCount.GetValue(),
			              "; seed particles=", fluid.GetParticles() ? fluid.GetParticles()->size() : 0);
		    });
		ReportLoading(L"Placing characters and setting up scene bounds");
		_map->Characters.Update(world, 0, GetJobs());
		_map->Bounds = std::make_unique<BistroBounds::Scene>(world);
		_map->Bounds->UpdateEmitters(world);
		BistroParticles::Update(world);
		ReportLoading(_measurementMap ? L"Preparing the camera route and physics snapshots" : L"Preparing the free camera");
		InitializeCamera(world);
		if (_measurementMap) _benchmark.Initialize(world, _map->Characters, *_bistroPipeline);
		_loadingPhase = BistroLoadingScreen::FirstFrame;
		ReportLoading(L"Building draw commands for the first frame");
		IndirectCommandBuildSystem commands;
		commands.Schedule(GetJobs(), world, 0).Complete();
		ReportLoading(L"Rendering the first frame");
		Log::Info("Bistro", "Loaded ", map.Name, " / ", _measurementMap ? map.Route : "free camera");
	}
	void Update(const GameTimer& timer) override
	{
		auto render = Locator.GetModule<RenderModule>();
		auto console = Locator.GetModule<ConsoleModule>();
		if (!_map && _autoBenchmarkPending && _settingsPresented)
		{
			_autoBenchmarkPending = false;
			if (_benchmark.CanStart())
			{
				auto launch = _info.SelectedLaunch();
				launch.Kind = BistroRun::Mode::Automatic;
				_benchmark.Launch(std::move(launch), *console);
			}
		}
		if (std::exchange(_resizePending, false) && !bMinimized) App::OnResize();
#if defined(_DEBUG) || defined(DEBUG)
		if (std::exchange(_leaveDebugMap, false)) ReturnToSettings();
		if (const auto* level = _info.TakeDebugMap(); !_map && level) StartMap(*level);
#endif
		if (const auto* launch = _benchmark.PendingLaunch(); !_map && launch) StartMap(*launch->Level, launch->Output);
		if (_map && _measurementMap && !_loadingScreen)
		{
			auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
			_benchmark.Update(world, *console, *render, *_bistroPipeline, *GetWindow());
			if (!_benchmark.Active()) ReturnToSettings();
		}
		if (!_map)
		{
			Engine::UI::BeginFrame();
			_info.DrawSettings(*render, _benchmark);
			return;
		}
		auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
		if (_legNumber != _benchmark.LegNumber())
		{
			_legNumber = _benchmark.LegNumber();
			RefreshWindowCaption();
		}
		_map->Reflections.Update(world, timer.DeltaTime());
		const float sceneDelta = world.IsPaused() ? 0.f : timer.DeltaTime() * world.GetTimeScale();
		UpdateSky(world, sceneDelta);
		_map->Water.Update(world, timer.DeltaTime());
		_map->Characters.Update(world, timer.DeltaTime(), GetJobs());
		_map->Grass.Update(world, timer.DeltaTime());
		_map->Bounds->UpdateEmitters(world);
		BistroParticles::Update(world);
		App::Update(timer);
		_map->Reflections.DrawBounds(world, *render);
		if (_measurementMap && !_loadingScreen) _info.DrawMeasurement(_benchmark);
	}
	void Render(const GameTimer& timer) override
	{
		auto render = Locator.GetModule<RenderModule>();
		if (!_map)
		{
			if (render->PresentSettings()) _settingsPresented = true;
			return;
		}
		const auto framesBefore = _bistroPipeline->PresentedFrames();
		App::Render(timer);
		if (_loadingScreen)
		{
			const bool cancelled = _loadingScreen->Cancelled();
			// The first draw can compile shaders. Finish it before starting the route or its warmup clock.
			// A covering window can make Present report occlusion even when that draw was submitted.
			_loadingScreen.reset();
			Timer.Tick(0);
			if (cancelled)
			{
				if (_measurementMap) _benchmark.AbortLaunch("Map loading cancelled", BistroBenchmark::Outcome::Cancelled);
				ReturnToSettings("Map loading cancelled");
			}
			else if (!_measurementMap) RefreshWindowCaption();
			return;
		}
		if (!_measurementMap) return;
		const bool presented = _bistroPipeline->PresentedFrames() != framesBefore;
		if (presented)
			_info.RecordPresent(_bistroPipeline->PresentTime(), _bistroPipeline->BenchmarkSubmissions(), _benchmark.ManualGpuTimings());
		_benchmark.FrameRendered(*Locator.GetModule<SceneManagerModule>()->GetWorld(), *Locator.GetModule<ConsoleModule>(),
		    *render, *_bistroPipeline, presented);
		if (_benchmark.Active()) _taskbar.Set(GetWindow()->GetWindowHandle(), BistroTaskbar::State::Running, _benchmark.RunPercent());
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
	void OnLoadingProgress(const std::wstring& detail, size_t completed, size_t total) override
	{
		if (_loadingScreen) _loadingScreen->Update(_loadingPhase, detail, completed, total);
	}
	bool LoadingCancelRequested() const override
	{
		return _loadingScreen && _loadingScreen->Cancelled();
	}
	LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override
	{
		if (_loadingScreen && msg == WM_KEYUP && wParam == VK_ESCAPE) _loadingScreen->RequestCancel();
		_taskbar.OnMessage(hwnd, msg, wParam);
		return App::MsgProc(hwnd, msg, wParam, lParam);
	}

	void SetMeasurementWindow(BistroPlacement::Size size)
	{
		const HWND hwnd = GetWindow()->GetWindowHandle();
		_savedPlacement.length = sizeof(_savedPlacement);
		Require(GetWindowPlacement(hwnd, &_savedPlacement), "Cannot save window placement");
		_savedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
		_windowChanged = true;
		MONITORINFO monitor{sizeof(monitor)};
		Require(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor), "Cannot locate benchmark monitor");
		if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
		SetLastError(0);
		const auto previous = SetWindowLongPtrW(hwnd, GWL_STYLE, (_savedStyle & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP);
		Require(previous || !GetLastError(), "Cannot switch to a borderless benchmark window");
		const auto& rect = monitor.rcMonitor;
		const int x = rect.left + (rect.right - rect.left - int(size.first)) / 2;
		const int y = rect.top + (rect.bottom - rect.top - int(size.second)) / 2;
		Require(SetWindowPos(hwnd, nullptr, x, y, int(size.first), int(size.second),
		    SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED), "Cannot set benchmark output size");
	}
	void StartMap(const BistroRun::Map& map, std::optional<BistroPlacement::Size> measurementSize = {})
	{
		_measurementMap = measurementSize.has_value();
		Engine::UI::HideSettings();
		Engine::UI::SetSettingsEditingEnabled(!_measurementMap);
		SetCaptionTimingEnabled(!_measurementMap);
		std::string error;
		bool cancelled = false;
		try
		{
			if (measurementSize) SetMeasurementWindow(*measurementSize);
			_loadingPhase = BistroLoadingScreen::Renderer;
			_loadingScreen.emplace(GetWindow()->GetWindowHandle(), std::filesystem::u8path(map.Name).wstring());
			if (LoadWithProgress(L"Opening the map", [&] { LoadBistro(map); }))
			{
				// Loading pumps WM_SIZE without resizing a partially initialized scene.
				_resizePending = bMinimized;
				if (!bMinimized)
				{
					uint16_t width, height;
					GetWindow()->GetWindowSize(width, height);
					GetWindow()->UpdateClientSize(width, height);
					const auto* frame = Locator.GetModule<RenderModule>()->GetRenderPipelineCommonData();
					if (width && height && (width != frame->WindowWidth || height != frame->WindowHeight))
						App::OnResize();
				}
				Timer.Tick(0);
#if defined(_DEBUG) || defined(DEBUG)
				if (!_measurementMap) BistroRender::SceneTextureInspector::SetVisible(true);
#endif
				return;
			}
			cancelled = true;
			error = "Map loading cancelled";
		}
		catch (const std::exception& failure) { error = failure.what(); }
		catch (const DxException& failure) { error = WStringToString(failure.ToString()); }
		if (cancelled) Log::Info("Bistro", error);
		else Log::Warning("Bistro", error);
		if (_measurementMap) _benchmark.AbortLaunch(error, cancelled ? BistroBenchmark::Outcome::Cancelled : BistroBenchmark::Outcome::Failed);
		ReturnToSettings(error);
	}
	void ReturnToSettings(const std::string& error = {})
	{
		const auto outcome = _benchmark.TakeOutcome();
		const bool completed = outcome == BistroBenchmark::Outcome::Completed;
		const bool failed = outcome == BistroBenchmark::Outcome::Failed;
		auto* hwnd = GetWindow()->GetWindowHandle();
		_taskbar.Set(hwnd, completed ? BistroTaskbar::State::Completed : failed ? BistroTaskbar::State::Failed : BistroTaskbar::State::Idle);
		if (_measurementMap) _benchmark.ReleaseScene();
#if defined(_DEBUG) || defined(DEBUG)
		else BistroRender::SceneTextureInspector::SetVisible(false);
#endif
		Engine::UI::HideSettings();
		auto render = Locator.GetModule<RenderModule>();
		render->BeginSettingsRendering();
		Locator.GetModule<SceneManagerModule>()->Uninitialize();
		_map.reset();
		_measurementMap = false;
		if (std::exchange(_windowChanged, false))
		{
			SetWindowLongPtrW(hwnd, GWL_STYLE, _savedStyle);
			SetWindowPlacement(hwnd, &_savedPlacement);
			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		}
		if (!bMinimized) App::OnResize();
		_resizePending = false;
		_loadingScreen.reset();
		Engine::UI::SetSettingsEditingEnabled(true);
		_info.ShowSettings(completed || (failed && error.empty()), error);
		SetCaptionTimingEnabled(true);
		RefreshWindowCaption();
		Log::Info("Bistro", "Returned to benchmark settings; VSync on");
	}
	std::wstring StatusCaption() const override
	{
		if (_benchmark.Active()) return _benchmark.Caption() + L" | Esc: cancel";
#if defined(_DEBUG) || defined(DEBUG)
		if (_map && !_measurementMap) return L"Debug map | WASD + RMB: camera | F1: settings | F8: textures | Esc: launcher";
#endif
		return L"Benchmark settings";
	}
	bool OnKeyReleased(WPARAM key) override
	{
#if defined(_DEBUG) || defined(DEBUG)
		if (_map && !_measurementMap)
		{
			if (key == VK_ESCAPE) { _leaveDebugMap = true; return true; }
			if (key == VK_F8)
			{
				using BistroRender::SceneTextureInspector;
				SceneTextureInspector::SetVisible(!SceneTextureInspector::Visible());
				return true;
			}
			return false; // The engine settings window handles F1 and its own input.
		}
#endif
		if (key == VK_ESCAPE && _map) { _benchmark.RequestCancel(); return true; }
		return key == VK_F1 || key == VK_F6 || key == VK_F7 || key == VK_F8;
	}
	uint64_t _legNumber = 0;
	std::optional<BistroLoadingScreen> _loadingScreen;
	BistroLoadingScreen::Phase _loadingPhase = BistroLoadingScreen::Renderer;
	bool _measurementMap = false;
	bool _settingsPresented = false, _windowChanged = false;
	WINDOWPLACEMENT _savedPlacement{};
	LONG_PTR _savedStyle = 0;

	void StopBenchmark(const char* status = "cancelled")
	{
		if (_benchmark.Active() && _map)
			_benchmark.Stop(*Locator.GetModule<SceneManagerModule>()->GetWorld(), *Locator.GetModule<ConsoleModule>(),
			    *_bistroPipeline, status);
	}
	// WM_SIZE can arrive inside a benchmark patch. Resize once all requested settings are known.
	void OnResize() override { _resizePending = true; }
	bool _resizePending = false;
	std::vector<DeviceDesc> _adapters;
	Entity _skyEntity = InvalidEntity;
	double _sunCyclePhase = 0;
	struct MapScene
	{
		BistroGrass::Scene Grass;
		BistroCharacters::Scene Characters{20261122};
		BistroWater::Scene Water;
		BistroReflections::Scene Reflections;
		std::unique_ptr<BistroBounds::Scene> Bounds;
	};
	std::unique_ptr<MapScene> _map;
	BistroBenchmark _benchmark;
	BistroTaskbar _taskbar;
	bool _autoBenchmarkPending = false;
	BistroInfoPanel _info;
	BistroRenderPipeline* _bistroPipeline = nullptr;
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
		if (paused && _map)
		{
			StopBenchmark();
			_bistroPipeline->PauseIndependentClocks();
		}
	}
#if defined(_DEBUG) || defined(DEBUG)
	bool _leaveDebugMap = false;
	POINT _lastMousePos{};
	float _cameraSpeed = 500.f;
	void OnMouseDown(WPARAM buttons, int x, int y) override
	{
		_lastMousePos = {x, y};
		if (!_map || _measurementMap || !(buttons & MK_RBUTTON) || Engine::UI::WantsMouseInput())
		{
			return;
		}
		SetCapture(GetWindow()->GetWindowHandle());
	}

	void OnMouseUp(WPARAM buttons, int, int) override
	{
		if (_map && !_measurementMap && !(buttons & MK_RBUTTON) && GetCapture() == GetWindow()->GetWindowHandle()) ReleaseCapture();
	}

	void OnMouseMove(WPARAM buttons, int x, int y) override
	{
		if (_map && !_measurementMap && !Engine::UI::WantsMouseInput() && (buttons & MK_RBUTTON))
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
		if (!_map || _measurementMap || Engine::UI::WantsMouseInput())
		{
			return;
		}
		_cameraSpeed =
		    std::clamp(_cameraSpeed * (GET_WHEEL_DELTA_WPARAM(rotation) > 0 ? CameraSpeedStep : 1.0f / CameraSpeedStep),
		               MinCameraSpeed, MaxCameraSpeed);
	}

	void OnKeyboardInput(const GameTimer& timer) override
	{
		if (!_map || _measurementMap)
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
#endif
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
