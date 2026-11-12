#include <App.Base/App.h>
#include "Engine.UI/UI_test.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <psapi.h>
#include <shellapi.h>
#include <d3d12sdklayers.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#include "BistroScene.h"
#include "BistroReflections.h"
#include "BistroGrass.h"
#include "Rendering/BistroRenderPipeline.h"
#include "BistroWater.h"
#include "BistroCharacters.h"
#include "BistroBounds.h"
#include "BistroBenchmarkFlight.h"
#include "BistroQualityPresets.h"
#include "Common/RuntimePaths.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "App.Base/LoadingSettings.h"

namespace
{
    using BistroQualitySearch::Profile;
    using BistroBenchmarkFlight::Phase;

    double QualityTime()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void CheckQualityConfig(const ConsoleConfigResult& result)
    {
        if (!result.Error.empty()) throw std::runtime_error(result.Error);
    }

    class BistroApp final : public App
    {
    public:
        explicit BistroApp(const HINSTANCE instance, unsigned characterSeed = std::random_device{}())
            : App(instance), _reflections(BistroQuality::MGPUProfile.GetValue() ? ReflectionSeed : characterSeed ^ ReflectionSeed), _characterSeed(characterSeed)
        {
            _characters.emplace(characterSeed);
        }

        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            ValidateQualityHardware();
            ValidateShadowBudget();
            ApplySavedResolution();
            SetupWorld();
            return true;
        }

        void SetupWorld()
        {
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            const auto transforms = static_cast<std::uint64_t>(BistroCharacters::GetCount(false)) +
                BistroCharacters::GetCount(true) + BistroReflections::Count.GetValue() +
                BistroGrass::Count.GetValue() + BistroWater::Count.GetValue();
            if (transforms > (std::numeric_limits<UINT>::max)()) throw std::overflow_error("Too many Bistro transforms");
            Locator.GetModule<RenderModule>()->ReserveTransformCapacity(static_cast<UINT>(transforms));
            UpdateSceneAdditions(world, 0);
            _bounds = std::make_unique<BistroBounds::Scene>(world);
            _bounds->UpdateEmitters(world);
            BistroParticles::Update(world);
            auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
            // Start at eye height just outside the main entrance, facing the interior.
            camera.Location = Vector3(-300.0f, 200.0f, 20.0f);
            camera.Rotation = Vector3(0.0f, 117.0f, 0.0f);
            camera.DirtyFlag = true;
            world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            _indirect.Schedule(_jobs, world, 0).Complete();
        }

        void RestartWorld(unsigned seed, bool benchmark = false)
        {
            auto scenes = Locator.GetModule<SceneManagerModule>();
            if (scenes->GetWorldCount() > 1) throw std::logic_error("Bistro reset needs a single active world");
            _bounds.reset();
            _characters.emplace(seed);
            _reflections.ResetWorld(benchmark || BistroQuality::MGPUProfile.GetValue() ? ReflectionSeed : _characterSeed ^ ReflectionSeed);
            _grass = {};
            _water.ResetWorld();
            scenes->Initialize();
            Locator.GetModule<RenderModule>()->ResetWorldHistory();
            _benchmarkStarted = false;
            BenchmarkRequested = 0;
            BenchmarkTicks = 0;
            SetupWorld();
            Timer.Reset();
        }

        void StartQuality(std::optional<Profile> profile, std::filesystem::path output, bool exitWhenDone,
            std::filesystem::path saveWinner = {})
        {
            if (_qualityActive || BistroRenderPipeline::IsCollisionViewEnabled())
                throw std::runtime_error("Close collision view before starting a Bistro quality run");
            uint16_t width, height;
            GetWindow()->GetWindowSize(width, height);
            if (!width || !height) throw std::runtime_error("Bistro needs a visible client area");
            const HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
            if (!lock) throw std::runtime_error("Cannot open the Bistro validation mutex");
            const auto acquired = WaitForSingleObject(lock, 0);
            if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
            {
                CloseHandle(lock);
                throw std::runtime_error("Another GPU validation is running");
            }
            _qualityLock.reset(lock);
            try
            {
                auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
                const auto& transform = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
                _qualityCameraLocation = transform.Location;
                _qualityCameraRotation = transform.Rotation;
                _qualityCameraFov = world.GetECS().Get<CameraComponent>(world.ActiveCamera).FOV;
                _qualityInterrupted = false;
                _qualityBackground = bInactive;
                _qualityProfile = profile;
                _qualityWidth = width;
                _qualityHeight = height;
                _qualityOutput = output.empty() ? Engine::Core::GetExecutableDirectory() / "Benchmarks" /
                    ("Bistro-" + std::to_string(GetTickCount64())) : std::move(output);
                if (std::filesystem::exists(_qualityOutput) && !std::filesystem::is_empty(_qualityOutput))
                    throw std::runtime_error("Choose an empty output directory for the Bistro quality run");
                std::filesystem::create_directories(_qualityOutput);
                _qualitySaveWinner = std::move(saveWinner);
                _qualityExit = exitWhenDone;
                _qualityRun = 0;
                _qualityTechniques = BistroQuality::SearchTechniques(ConsoleModule{});
                _qualityWinner.reset();
                _winnerPatch.clear();
                if (profile)
                {
                    const auto& status = Locator.GetModule<RenderModule>()->GetMGPUStatus();
                    if (status.Requested && !status.SecondaryReady)
                        throw std::runtime_error("MGPU search requires the requested secondary adapter: " + status.FallbackReason);
                    BistroQualitySearch::MGPUSettings pair;
                    pair.Enabled = status.Requested;
                    pair.Primary = status.PrimaryLuid;
                    pair.Secondary = status.ActiveSecondaryLuid;
                    world.GetECS().ForEach<LightComponent>([&](Entity entity, const LightComponent& light)
                    {
                        if (light.Type != LightType::Directional && light.Enabled && light.CastShadows && light.Range > 0)
                            pair.LocalShadows.push_back({entity, {}});
                    });
                    std::sort(pair.LocalShadows.begin(), pair.LocalShadows.end());
                    std::optional<size_t> waterAxis = _qualityTechniques.size(), grassAxis = _qualityTechniques.size();
                    std::array<std::optional<size_t>, 3> scheduledAxes{};
                    auto axes = BistroQuality::SearchAxes(_qualityTechniques);
                    for (size_t i = 0; i < _qualityTechniques.size(); ++i)
                    {
                        if (std::string_view(_qualityTechniques[i]->Name) == "Water") waterAxis = i;
                        const std::string_view name(_qualityTechniques[i]->Name);
                        if (name == "Grass") grassAxis = i;
                        if (name == "Sun shadows") scheduledAxes[0] = i;
                        if (name == "Local shadows") scheduledAxes[1] = i;
                        if (name == "Particles") scheduledAxes[2] = i;
                        if (pair.Enabled && name == "Reflection probes") axes[i].MinTier = axes[i].MaxTier = 1;
                    }
                    _qualitySearch.emplace(*profile, std::move(axes), width, height,
                        pair, waterAxis, grassAxis, scheduledAxes);
                }
                Engine::UI::SuspendSettings(true);
                _qualityActive = true;
                _qualityStatus = "Preparing Bistro...";
            }
            catch (...)
            {
                _qualityLock.reset();
                throw;
            }
        }

        int Run()
        {
            const int result = App::Run();
            return _qualityActive && _qualityExit ? EXIT_FAILURE : result;
        }

        void BenchmarkFrame(double fixedStep = -1)
        {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            if (!_benchmarkStarted)
            {
                const auto previous = world.BeforePhysicsStep;
                world.BeforePhysicsStep = [this, previous](float step)
                {
                    if (previous) previous(step);
                    ++BenchmarkTicks;
                };
                Timer.Reset();
                _benchmarkStarted = true;
            }
            Timer.Start();
            Timer.Tick(fixedStep);
            if (!world.IsPaused()) BenchmarkRequested += double(Timer.DeltaTime()) * world.GetTimeScale();
            Update(Timer);
            Render(Timer);
        }

        void ResetBenchmarkClock() { Timer.Reset(); }
        double BenchmarkRequested = 0;
        uint64_t BenchmarkTicks = 0;

    protected:
        bool AddModules() override
        {
            const auto scenePath = std::filesystem::path(ASSETS_FOLDER) / "Scenes" / "Amazon Lumberyard Bistro";
            for (const auto* part : { L"Exterior/exterior.obj", L"Interior/interior.obj" })
            {
                if (!std::filesystem::is_regular_file(scenePath / part))
                {
                    throw std::runtime_error("Missing Bistro scene part: " + (scenePath / part).string());
                }
            }

            auto scenes = std::make_shared<SceneManagerModule>(scenePath, true);
            auto pipeline = std::make_unique<BistroRenderPipeline>(scenes);
            _qualityPipeline = pipeline.get();
            auto renderer = std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline));
            Locator.RegisterModule(renderer);
            ConfigureBistroProps(*scenes, GetJobs());
            Locator.RegisterModule(scenes);
            return BenchmarkEngine::AddModules();
        }

        void Update(const GameTimer& timer) override
        {
            GDX12_SCOPED_CPU_EVENT("Game update");
            auto renderer = Locator.GetModule<RenderModule>();
            ProcessQualityAction();
            if (_qualityActive) BeginQualityFrame(*renderer);
            renderer->BeginFrame();
            DrawQualityPanel();
            if (!_qualityActive) OnKeyboardInput(timer);
            renderer->Update();
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            UpdateSceneAdditions(world, timer.DeltaTime());
            _bounds->UpdateEmitters(world);
            BistroParticles::Update(world);
            if (world.IsPaused())
            {
                _lights.Schedule(_jobs, world, 0).Complete();
                _gpuData.Schedule(_jobs, world, 0).Complete();
                DrawSceneBounds(world);
                return;
            }
            const float dt = timer.DeltaTime();
            JobHandle work;
            try
            {
                work = _movement.Schedule(_jobs, world, dt);
                work = _circle.Schedule(_jobs, world, dt, work);
                work = _spline.Schedule(_jobs, world, dt, work);
                work = _lookAt.Schedule(_jobs, world, dt, work);
                work = _lights.Schedule(_jobs, world, dt, work);
                work.Complete();
                UpdatePhysics(world, dt);
                if (_qualityActive) BenchmarkRequested += dt * world.GetTimeScale();
                work = _gpuData.Schedule(_jobs, world, dt);
                work.Complete();
                DrawSceneBounds(world);
            }
            catch (...)
            {
                try { work.Complete(); } catch (...) {}
                throw;
            }
        }

        void Render(const GameTimer& timer) override
        {
            App::Render(timer);
            if (_qualityActive) EndQualityFrame(*Locator.GetModule<RenderModule>());
        }

        void OnPauseChanged(bool paused) override
        {
            if (_qualityActive && paused) _qualityInterrupted = true;
        }

        void OnMouseDown(WPARAM, int x, int y) override
        {
            _lastMousePos = { x, y };
            SetCapture(GetWindow()->GetWindowHandle());
        }

        void OnMouseUp(WPARAM, int, int) override
        {
            ReleaseCapture();
        }

        void OnMouseMove(WPARAM buttons, int x, int y) override
        {
            if (!_qualityActive && (buttons & MK_RBUTTON))
            {
                auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
                auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
                camera.Rotation.x = std::clamp(camera.Rotation.x + (y - _lastMousePos.y) * 0.15f, -89.0f, 89.0f);
                camera.Rotation.y = std::remainder(camera.Rotation.y - (x - _lastMousePos.x) * 0.15f, 360.0f);
                camera.DirtyFlag = true;
            }
            _lastMousePos = { x, y };
        }

        void OnMouseWheelMove(WPARAM rotation) override
        {
            _cameraSpeed = std::clamp(_cameraSpeed * (GET_WHEEL_DELTA_WPARAM(rotation) > 0 ? 1.25f : 0.8f), 0.1f, 10000.0f);
        }

        void OnKeyboardInput(const GameTimer& timer) override
        {
            if (bInactive || Engine::UI::WantsKeyboardInput()) return;
            auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
            const auto rotation = Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(camera.Rotation.y), XMConvertToRadians(camera.Rotation.x), 0.0f);
            Vector3 movement = Vector3::Zero;
            if (GetAsyncKeyState('W') & 0x8000) movement -= rotation.Forward();
            if (GetAsyncKeyState('S') & 0x8000) movement += rotation.Forward();
            if (GetAsyncKeyState('A') & 0x8000) movement += rotation.Right();
            if (GetAsyncKeyState('D') & 0x8000) movement -= rotation.Right();
            if (GetAsyncKeyState('Q') & 0x8000) movement -= Vector3::Up;
            if (GetAsyncKeyState('E') & 0x8000) movement += Vector3::Up;
            if (movement.LengthSquared() > 0.0f)
            {
                movement.Normalize();
                const float boost = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4.0f : 1.0f;
                camera.Location += movement * _cameraSpeed * boost * timer.DeltaTime();
                camera.DirtyFlag = true;
            }
        }

    private:
        static constexpr unsigned QualitySeed = BistroQuality::WorldSeed;
        static constexpr unsigned ReflectionSeed = BistroQuality::ProbeSeed;

        void ValidateQualityHardware()
        {
            if (!BistroQuality::MGPUProfile.GetValue()) return;
            if (BistroQuality::SavedSchema.GetValue() != BistroQuality::SchemaVersion ||
                BistroQuality::SavedScenario.GetValue() != BistroQuality::ScenarioVersion ||
                BistroQuality::SavedWorldSeed.GetValue() != QualitySeed || BistroQuality::SavedProbeSeed.GetValue() != ReflectionSeed)
                throw std::runtime_error("Saved MGPU profile uses a different benchmark scenario; rerun the search");
            const auto& actual = Locator.GetModule<RenderModule>()->GetMGPUStatus();
            if (!actual.Requested || !actual.SecondaryReady ||
                BistroQuality::PrimaryLuid.GetValue() != actual.PrimaryLuid ||
                BistroQuality::SecondaryLuid.GetValue() != actual.ActiveSecondaryLuid)
                throw std::runtime_error("Saved MGPU profile pair " + std::to_string(BistroQuality::PrimaryLuid.GetValue()) +
                    "/" + std::to_string(BistroQuality::SecondaryLuid.GetValue()) + " differs from active pair " +
                    std::to_string(actual.PrimaryLuid) + "/" + std::to_string(actual.ActiveSecondaryLuid) +
                    " (requested=" + std::to_string(actual.Requested) + ", ready=" + std::to_string(actual.SecondaryReady) +
                    "); rerun the search");
        }

        void ResizeQualityClient(int width, int height)
        {
            uint16_t actualWidth, actualHeight;
            GetWindow()->GetWindowSize(actualWidth, actualHeight);
            if (actualWidth == width && actualHeight == height) return;
            GetWindow()->SetWindowSize(static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            GetWindow()->GetWindowSize(actualWidth, actualHeight);
            if (actualWidth != width || actualHeight != height)
                throw std::runtime_error("Bistro window did not reach the requested client size");
        }

        void ApplySavedResolution()
        {
            const auto width = BistroQuality::OutputWidth.GetValue();
            const auto height = BistroQuality::OutputHeight.GetValue();
            if ((width == 0) != (height == 0)) throw std::invalid_argument("Bistro output dimensions must be specified together");
            if (width) ResizeQualityClient(width, height);
        }

        void ValidateShadowBudget()
        {
            const auto primary = Locator.GetModule<RenderModule>()->GetMGPUDevices().Primary;
            const auto capacity = primary->GetMemoryAllocator()->GetMemoryCapacity(DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
            if (!capacity) return;
            const auto quality = ReadShadowQuality();
            const auto probes = GetReflectionSettings().Enabled && BistroReflections::Count.GetValue() > 0;
            std::uint64_t slices = 0;
            Locator.GetModule<SceneManagerModule>()->GetWorld()->GetECS().ForEach<LightComponent>(
                [&](Entity, const LightComponent& light)
                {
                    if (light.Enabled && light.CastShadows && light.Range > 0 && light.Type != LightType::Directional)
                        slices += light.Type == LightType::Point ? 6 : 1;
                });
            const auto local = std::uint64_t(quality.LocalResolution);
            const auto sun = std::uint64_t(quality.SunResolution);
            const auto atlasBytes = (2 + probes) * local * local * slices * sizeof(float) +
                (4 + probes) * sun * sun * sizeof(float);
            if (atlasBytes > capacity)
                throw std::runtime_error("Requested Bistro shadow atlases exceed the adapter's local memory capacity");
        }

        void ApplyPresetPatch(const ConsoleVariablePatch& patch, BistroQuality::ApplyMode mode, unsigned seed, bool benchmark = false)
        {
            ConsoleModule console;
            ConsoleVariablePatch before;
            uint16_t width, height;
            GetWindow()->GetWindowSize(width, height);
            Locator.GetModule<RenderModule>()->DrainFrameWork();
            BistroQuality::RegisterProfileShadowVariables(patch);
            CheckQualityConfig(console.ApplyPatch(patch, &before));
            try
            {
                ValidateQualityHardware();
                ValidateShadowBudget();
                ApplySavedResolution();
                if (mode == BistroQuality::ApplyMode::RestartWorld) RestartWorld(seed, benchmark);
                else if (mode == BistroQuality::ApplyMode::RecreateTechnique)
                    Locator.GetModule<RenderModule>()->DrainFrameWork();
            }
            catch (...)
            {
                CheckQualityConfig(console.ApplyPatch(before));
                ResizeQualityClient(width, height);
                if (mode == BistroQuality::ApplyMode::RestartWorld) RestartWorld(_characterSeed);
                throw;
            }
        }

        void SetFlightPose(const BistroBenchmarkFlight::Pose& pose)
        {
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
            const auto direction = pose.Target - pose.Position;
            camera.Location = pose.Position;
            camera.Rotation = Vector3(
                XMConvertToDegrees(std::atan2(-direction.y, std::hypot(direction.x, direction.z))),
                XMConvertToDegrees(std::atan2(direction.x, direction.z)), 0);
            camera.DirtyFlag = true;
        }

        static uint64_t WorkloadFingerprint(World& world)
        {
            uint64_t hash = 14695981039346656037ull;
            const auto append = [&](const void* data, size_t count)
            {
                const auto* bytes = static_cast<const unsigned char*>(data);
                for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
            };
            auto& ecs = world.GetECS();
            ecs.ForEach<NameComponent, TransformComponent>([&](Entity entity, const NameComponent& name,
                const TransformComponent& transform)
            {
                append(name.value.c_str(), name.value.size() + 1);
                append(&transform.Location.x, 3 * sizeof(float));
                append(&transform.Rotation.x, 3 * sizeof(float));
                if (ecs.Has<RigidBodyComponent>(entity))
                {
                    const auto motion = ecs.Get<RigidBodyComponent>(entity).MotionType;
                    append(&motion, sizeof(motion));
                }
            });
            return hash;
        }

        bool PrepareQualityTrial()
        {
            if (_qualitySearch)
            {
                _qualityRequest = _qualitySearch->Next();
                if (!_qualityRequest)
                {
                    _qualityWinner = _qualitySearch->Winner();
                    if (_qualityWinner)
                        _winnerPatch = BistroQuality::ProfilePatch(_qualityTechniques, _qualityWinner->Settings, *_qualityProfile);
                    FinishQuality(false);
                    return false;
                }
                _trialPatch = BistroQuality::ProfilePatch(_qualityTechniques, _qualityRequest->Settings, *_qualityProfile);
            }
            else
            {
                if (_qualityRun)
                {
                    FinishQuality(false);
                    return false;
                }
                _trialPatch = {{L"Render.Upscaler.Provider", "0"}, {L"Render.FrameGeneration.Provider", "0"},
                    {L"App.Quality.OutputWidth", std::to_string(_qualityWidth)},
                    {L"App.Quality.OutputHeight", std::to_string(_qualityHeight)}};
            }

            ++_qualityRun;
            _setupStarted = QualityTime();
            try
            {
                ConsoleVariablePatch previous;
                Locator.GetModule<RenderModule>()->DrainFrameWork();
                BistroQuality::RegisterProfileShadowVariables(_trialPatch);
                CheckQualityConfig(ConsoleModule{}.ApplyPatch(_trialPatch, &previous));
                if (_basePatch.empty()) _basePatch = std::move(previous);
                ValidateShadowBudget();
                ApplySavedResolution();
                RestartWorld(QualitySeed, true);
            }
            catch (const std::exception& error)
            {
                RejectQualityTrial(error.what());
                return false;
            }
            catch (const DxException& error)
            {
                auto* primary = Locator.GetModule<RenderModule>()->GetMGPUDevices().Primary;
                if (!primary || FAILED(primary->GetDevice()->GetDeviceRemovedReason())) throw;
                RejectQualityTrial(WStringToString(error.ToString()));
                return false;
            }
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            _qualityWorkload = WorkloadFingerprint(world);
            const auto before = world.BeforePhysicsStep;
            world.BeforePhysicsStep = [this, before](float dt)
            {
                if (before) before(dt);
                ++BenchmarkTicks;
            };
            SetFlightPose(BistroBenchmarkFlight::SampleRoute(0));
            world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            _flight.emplace();
            _setupFrames = 0;
            _trialReady = false;
            _freshness = {};
            _routeSamples.clear();
            _freshnessRequired.clear();
            _qualityStatus = "Trial " + std::to_string(_qualityRun) + " | Preparing resources";
            return true;
        }

        void BeginQualityFrame(RenderModule& renderer)
        {
            if (_qualityInterrupted || bInactive != _qualityBackground)
            {
                FinishQuality(true, _qualityInterrupted ? "Pause was requested during the trial." :
                    "Window activation changed during the trial: expected inactive=" + std::to_string(_qualityBackground) +
                    ", actual inactive=" + std::to_string(bInactive));
                return;
            }
            if (!_flight)
            {
                PrepareQualityTrial();
                return;
            }
            if (!_trialReady)
            {
                Timer.Tick(0, 0);
                return;
            }
            uint16_t width, height;
            GetWindow()->GetWindowSize(width, height);
            if (width != BistroQuality::OutputWidth.GetValue() || height != BistroQuality::OutputHeight.GetValue())
            {
                FinishQuality(true, "Client size changed during the trial: expected " +
                    std::to_string(BistroQuality::OutputWidth.GetValue()) + "x" +
                    std::to_string(BistroQuality::OutputHeight.GetValue()) + ", actual " +
                    std::to_string(width) + "x" + std::to_string(height));
                return;
            }
            SetFlightPose(_flight->BeginFrame(static_cast<std::uint64_t>(renderer.GetFrameSubmissionId()) + 1,
                QualityTime()));
        }

        void EndQualityFrame(RenderModule& renderer)
        {
            if (!_flight) return;
            if (!_trialReady)
            {
                const auto [ready, total] = renderer.GetWorldProbeReadiness();
                if (ready < total)
                {
                    if (++_setupFrames > 192) RejectQualityTrial("Reflection probes did not finish their initial captures");
                    return;
                }
                renderer.DrainFrameWork();
                const auto* common = renderer.GetRenderPipelineCommonData();
                const auto& reconstruction = renderer.GetReconstructionStatus();
                const auto& generation = renderer.GetFrameGenerationStatus();
                if (reconstruction.Requested.Provider != GDX12UpscalerProvider::Native ||
                    reconstruction.Effective != GDX12UpscalerProvider::Native ||
                    generation.Requested != GDX12FrameGenerationProvider::Off ||
                    generation.Effective != GDX12FrameGenerationProvider::Off ||
                    common->DownscaledWidth != common->WindowWidth || common->DownscaledHeight != common->WindowHeight ||
                    common->ActiveCameraJitterOffsetX != 0 || common->ActiveCameraJitterOffsetY != 0)
                {
                    RejectQualityTrial("Bistro quality trial requires native rendering and FG Off");
                    return;
                }
                _setupSeconds = QualityTime() - _setupStarted;
                _trialReady = true;
                if (BistroQuality::MGPUProfile.GetValue())
                {
                    _freshnessRequired.insert("physics");
                    for (const auto& sample : _qualityPipeline->GetQualityMeasurements())
                        if (sample.Active) _freshnessRequired.insert(sample.Name);
                }
                Timer.Reset();
                BenchmarkRequested = 0;
                BenchmarkTicks = 0;
                _qualityStatus = "Trial " + std::to_string(_qualityRun) + " | 10-second stationary warmup";
                return;
            }
            const bool measuring = _flight->State() == Phase::Flight;
            _flight->EndFrame(QualityTime());
            if (measuring && BistroQuality::MGPUProfile.GetValue())
            {
                auto sample = _qualityPipeline->GetQualityMeasurements();
                const double physicsDebt = (std::max)(0.0, BenchmarkRequested - BenchmarkTicks * double(1.0f / 60));
                BistroQualitySearch::TechniqueMeasurement physics;
                physics.Name = "physics";
                physics.Active = physics.Ready = true;
                physics.SourceAgeMs = physicsDebt * 1000;
                physics.SimulationDebtSeconds = physicsDebt;
                physics.SourceSequence = BenchmarkTicks;
                sample.push_back(std::move(physics));
                const auto target = BistroQuality::TargetProfile.GetValue() == 0 ? Profile::Performance : Profile::Quality;
                std::set<std::string> observed;
                for (auto& value : sample)
                {
                    observed.insert(value.Name);
                    if (!value.Active && _freshnessRequired.count(value.Name))
                    {
                        value.Active = true;
                        value.Ready = false;
                    }
                    _freshness[value.Name].Observe(value, _flight->Samples().back().Elapsed, target);
                }
                for (const auto& name : _freshnessRequired)
                    if (!observed.count(name)) ++_freshness[name].MissingFrames;
                _routeSamples.push_back(sample);
            }
            if (_flight->State() == Phase::WarmupReady)
            {
                _warmupFps = _flight->StableWarmupFps();
                _measurementTicks = BenchmarkTicks;
                _measurementRequested = BenchmarkRequested;
                _flight->StartFlight();
                _qualityStatus = "Trial " + std::to_string(_qualityRun) + " | 100-second exterior flight";
            }
            else if (_flight->State() == Phase::Complete)
            {
                SetFlightPose(BistroBenchmarkFlight::SampleRoute(1));
                const auto metrics = _flight->Summarize();
                renderer.DrainFrameWork();
                ExportQualityTrial(renderer, metrics, ValidateGpuMessages(renderer));
                bool temporalValid = true;
                for (const auto& [name, value] : _freshness)
                    temporalValid &= value.Valid(_freshnessRequired.count(name) != 0);
                for (const auto& name : _freshnessRequired)
                    temporalValid &= _freshness.count(name) != 0;
                if (_qualitySearch) _qualitySearch->Submit({metrics.AverageFps, _warmupFps, temporalValid});
                _flight.reset();
            }
        }

        static bool ValidateGpuMessages(RenderModule& renderer)
        {
            bool enabled = false;
            const auto devices = renderer.GetMGPUDevices();
            for (auto* device : {devices.Primary, devices.Secondary})
            {
                if (!device) continue;
                ComPtr<ID3D12InfoQueue> messages;
                const auto result = device->GetDevice().As(&messages);
                if (result == E_NOINTERFACE) continue;
                ThrowIfFailed(result);
                enabled = true;
                for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index)
                {
                    SIZE_T size = 0;
                    ThrowIfFailed(messages->GetMessage(index, nullptr, &size));
                    std::vector<BYTE> bytes(size);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                    ThrowIfFailed(messages->GetMessage(index, message, &size));
                    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(device->GetDeviceFeatures().Name + ": " + message->pDescription);
                }
            }
            return enabled;
        }

        void ExportQualityTrial(RenderModule& renderer, const BistroBenchmarkFlight::Metrics& metrics, bool debugValidation)
        {
            const auto stem = _qualityOutput / ("trial-" + std::to_string(_qualityRun));
            std::ofstream frames(stem.wstring() + L".csv");
            frames << std::setprecision(12) << "frame_id,elapsed_seconds,progress,segment,frame_seconds,fps\n";
            for (const auto& frame : _flight->Samples())
                frames << frame.FrameId << ',' << frame.Elapsed << ',' << frame.Progress << ',' << frame.Segment
                    << ',' << frame.Duration << ',' << frame.Fps << '\n';
            frames.close();
            if (!frames) throw std::runtime_error("Cannot export Bistro frame samples");
            if (!_routeSamples.empty())
            {
                std::ofstream routes(stem.wstring() + L"-routes.csv");
                routes << std::setprecision(12) << "frame_id,technique,active,requested_secondary,effective_secondary,ready,source_age_ms,simulation_debt_seconds,missing_seconds,source_sequence,full_source_age_ms,wait_ms,export_bytes,import_bytes\n";
                for (size_t frame = 0; frame < _routeSamples.size(); ++frame)
                    for (size_t technique = 0; technique < _routeSamples[frame].size(); ++technique)
                    {
                        const auto& value = _routeSamples[frame][technique];
                        routes << _flight->Samples()[frame].FrameId << ',' << value.Name << ','
                            << value.Active << ',' << value.RequestedSecondary << ',' << value.EffectiveSecondary << ','
                            << value.Ready << ',' << value.SourceAgeMs << ',' << value.SimulationDebtSeconds << ','
                            << value.MissingSeconds << ',' << value.SourceSequence << ',' << value.FullSourceAgeMs << ',' << value.WaitMs << ','
                            << value.ExportBytes << ',' << value.ImportBytes << '\n';
                    }
                routes.close();
                if (!routes) throw std::runtime_error("Cannot export Bistro route samples");
            }
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto physics = Locator.GetModule<PhysicsModule>();
            size_t moving = 0, sleeping = 0, statics = 0, water = 0, particles = 0;
            size_t grassBlades = 0;
            UINT grassSegments = 0, windGrid = 0, windPressure = 0;
            const auto controls = GetGrassWindControls();
            if (controls.GrassEnabled && controls.GrassQuality > 0)
                world.GetECS().ForEach<GrassRenderComponent>([&](Entity, const GrassRenderComponent& grass)
                {
                    if (!grass.Enabled || !grass.Material) return;
                    const auto settings = ResolveGrassSettings(grass, controls.GrassQuality);
                    const auto count = GrassBladeCount(settings);
                    if (!count) return;
                    grassBlades += count;
                    grassSegments = (std::max)(grassSegments, settings.Segments);
                    auto& ecs = world.GetECS();
                    if (!controls.WindEnabled || !controls.WindQuality || grass.WindEntity == InvalidEntity ||
                        !ecs.Has<WindFieldComponent>(grass.WindEntity)) return;
                    const auto& wind = ecs.Get<WindFieldComponent>(grass.WindEntity);
                    if (!wind.Enabled) return;
                    const auto field = ResolveWindSettings(wind, controls.WindQuality);
                    windGrid = (std::max)(windGrid, field.GridResolution);
                    if (field.Implementation == GDX12WindImplementation::ProjectedGrid)
                        windPressure = (std::max)(windPressure, field.PressureIterations);
                });
            world.GetECS().ForEach<RigidBodyComponent>([&](Entity, const RigidBodyComponent& body)
            {
                if (body.MotionType != PhysicsMotionType::Dynamic) { ++statics; return; }
                Vector3 velocity;
                bool active = false;
                if (physics->TryGetBodyVelocity(world, body.Body, velocity, active))
                    ++(active ? moving : sleeping);
            });
            if (GetFluidControls().Enabled)
                world.GetECS().ForEach<FluidRenderComponent>([&](Entity, const FluidRenderComponent& fluid)
                {
                    if (const auto& seed = fluid.GetParticles()) water += seed->size();
                });
            world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
            {
                if (emitter.IsEnabled()) particles += emitter.GetSettings().MaxParticles;
            });
            const double requested = BenchmarkRequested - _measurementRequested;
            const auto ticks = BenchmarkTicks - _measurementTicks;
            const auto gpuMemory = renderer.GetMemoryStatistics();
            auto* primary = renderer.GetMGPUDevices().Primary;
            const auto luid = primary->GetAdapterLuid();
            const auto [readyProbes, configuredProbes] = renderer.GetWorldProbeReadiness();
            D3D12MA::Budget localBudget{};
            primary->GetMemoryAllocator()->GetBudget(&localBudget, nullptr);
            D3D12MA::Budget secondaryBudget{};
            if (auto* secondary = renderer.GetMGPUDevices().Secondary)
                secondary->GetMemoryAllocator()->GetBudget(&secondaryBudget, nullptr);
            PROCESS_MEMORY_COUNTERS_EX memory = {};
            memory.cb = sizeof(memory);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
            std::ofstream report(stem.wstring() + L".json");
            report << std::setprecision(12) << "{\n  \"schema\": " << BistroQuality::SchemaVersion
                << ",\n  \"route\": " << std::quoted(BistroBenchmarkFlight::RouteVersion)
                << ",\n  \"adapter_luid\": " << (std::uint64_t(static_cast<std::uint32_t>(luid.HighPart)) << 32 | luid.LowPart)
                << ",\n  \"adapter\": " << std::quoted(primary->GetDeviceFeatures().Name)
                << ",\n  \"build\": "
#ifdef _DEBUG
                << "\"Debug\""
#else
                << "\"Release\""
#endif
                << ",\n  \"debug_layer\": " << (debugValidation ? "true" : "false")
                << ",\n  \"seed\": " << QualitySeed
                << ",\n  \"background_window\": " << std::boolalpha << _qualityBackground << std::noboolalpha
                << ",\n  \"initial_workload\": \"" << std::hex << _qualityWorkload << std::dec << '"'
                << ",\n  \"prepare_seconds\": " << _setupSeconds
                << ",\n  \"warmup_protocol_seconds\": " << BistroBenchmarkFlight::WarmupSeconds
                << ",\n  \"flight_protocol_seconds\": " << BistroBenchmarkFlight::FlightSeconds
                << ",\n  \"effective_upscaler\": \"Native\",\n  \"effective_frame_generation\": \"Off\""
                << ",\n  \"render_width\": " << renderer.GetRenderPipelineCommonData()->DownscaledWidth
                << ",\n  \"render_height\": " << renderer.GetRenderPipelineCommonData()->DownscaledHeight
                << ",\n  \"sample_count\": " << metrics.FrameCount << ",\n  \"seconds\": " << metrics.Duration
                << ",\n  \"avg_fps\": " << metrics.AverageFps
                << ",\n  \"min_frame_seconds\": " << metrics.MinFrameTime
                << ",\n  \"max_frame_seconds\": " << metrics.MaxFrameTime
                << ",\n  \"p50_seconds\": " << metrics.P50FrameTime << ",\n  \"p95_seconds\": " << metrics.P95FrameTime
                << ",\n  \"p99_seconds\": " << metrics.P99FrameTime << ",\n  \"one_percent_low\": " << metrics.OnePercentLow
                << ",\n  \"point_one_percent_low\": " << metrics.PointOnePercentLow
                << ",\n  \"physics_ticks\": " << ticks << ",\n  \"physics_requested_seconds\": " << requested
                << ",\n  \"physics_debt_start\": " << _measurementRequested - _measurementTicks * double(1.0f / 60)
                << ",\n  \"physics_debt_end\": " << BenchmarkRequested - BenchmarkTicks * double(1.0f / 60)
                << ",\n  \"active_bodies\": " << moving << ",\n  \"sleeping_bodies\": " << sleeping
                << ",\n  \"static_bodies\": " << statics << ",\n  \"people\": " << BistroCharacters::GetCount(false)
                << ",\n  \"foxes\": " << BistroCharacters::GetCount(true)
                << ",\n  \"water_particles\": " << water << ",\n  \"particle_capacity\": " << particles
                << ",\n  \"grass_blades\": " << grassBlades << ",\n  \"grass_max_segments\": " << grassSegments
                << ",\n  \"wind_grid\": " << windGrid << ",\n  \"wind_pressure_iterations\": " << windPressure
                << ",\n  \"published_probes\": " << readyProbes << ",\n  \"configured_probes\": " << configuredProbes
                << ",\n  \"gpu_allocation_bytes\": " << gpuMemory.AllocationBytes
                << ",\n  \"gpu_local_usage_bytes\": " << localBudget.UsageBytes
                << ",\n  \"gpu_local_budget_bytes\": " << localBudget.BudgetBytes
                << ",\n  \"secondary_local_usage_bytes\": " << secondaryBudget.UsageBytes
                << ",\n  \"secondary_local_budget_bytes\": " << secondaryBudget.BudgetBytes
                << ",\n  \"process_peak_commit_bytes\": " << memory.PeakPagefileUsage;
            if (_warmupFps) report << ",\n  \"stable_warmup_fps\": " << *_warmupFps;
            const auto& mgpu = renderer.GetMGPUStatus();
            report << ",\n  \"mgpu_requested\": " << std::boolalpha << mgpu.Requested
                << ",\n  \"secondary_ready\": " << mgpu.SecondaryReady
                << ",\n  \"secondary_luid\": " << mgpu.ActiveSecondaryLuid
                << ",\n  \"secondary_adapter\": " << std::quoted(renderer.GetMGPUDevices().Secondary ?
                    renderer.GetMGPUDevices().Secondary->GetDeviceFeatures().Name : "")
                << ",\n  \"visual_quality_verified\": false,\n  \"freshness\": [";
            size_t freshnessIndex = 0;
            for (const auto& [name, value] : _freshness)
            {
                report << (freshnessIndex++ ? "," : "") << "{\"technique\":\"" << name
                    << "\",\"valid\":" << value.Valid(_freshnessRequired.count(name) != 0) << ",\"samples\":" << value.Samples
                    << ",\"fallback_frames\":" << value.FallbackFrames << ",\"missing_frames\":" << value.MissingFrames
                    << ",\"over_age_frames\":" << value.OverAgeFrames << ",\"max_source_age_ms\":" << value.MaxSourceAgeMs
                    << ",\"max_simulation_debt_seconds\":" << value.MaxDebtSeconds << "}";
            }
            report << "]" << std::noboolalpha;
            const auto writePatch = [&](const char* label, const ConsoleVariablePatch& patch)
            {
                report << ",\n  " << std::quoted(label) << ": {";
                const char* separator = "";
                for (const auto& [key, value] : patch)
                {
                    report << separator << "\n    " << std::quoted(std::filesystem::path(key).u8string())
                        << ": " << std::quoted(value);
                    separator = ",";
                }
                report << "\n  }";
            };
            writePatch("candidate", _trialPatch);
            ConsoleVariablePatch effective;
            ConsoleModule console;
            for (const auto& technique : BistroQuality::Techniques)
                for (size_t i = 0; i < technique.FieldCount; ++i)
                {
                    const auto* key = technique.Fields[i].Key;
                    effective.emplace_back(key, console.FindConsoleVariable(key).GetString());
                }
            for (const auto& [key, value] : _trialPatch)
                if (std::none_of(effective.begin(), effective.end(), [&](const auto& field) { return field.first == key; }))
                    effective.emplace_back(key, console.FindConsoleVariable(key).GetString());
            writePatch("effective_cvars", effective);
            report << "\n}\n";
            report.close();
            if (!report) throw std::runtime_error("Cannot export Bistro trial metrics");
        }

        void RestoreQualityBaseline()
        {
            if (_basePatch.empty()) return;
            Locator.GetModule<RenderModule>()->DrainFrameWork();
            CheckQualityConfig(ConsoleModule{}.ApplyPatch(_basePatch));
            ResizeQualityClient(_qualityWidth, _qualityHeight);
            RestartWorld(_characterSeed);
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto& transform = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
            transform.Location = _qualityCameraLocation;
            transform.Rotation = _qualityCameraRotation;
            transform.DirtyFlag = true;
            auto& camera = world.GetECS().Get<CameraComponent>(world.ActiveCamera);
            camera.FOV = _qualityCameraFov;
            camera.InvalidateHistory();
        }

        void RejectQualityTrial(std::string_view reason)
        {
            std::ofstream note(_qualityOutput / ("trial-" + std::to_string(_qualityRun) + ".rejected.txt"));
            note << reason << '\n';
            if (_qualitySearch)
            {
                RestoreQualityBaseline();
                _qualitySearch->Submit({std::nullopt, std::nullopt, false});
                _flight.reset();
            }
            else FinishQuality(true, "Candidate rejected: " + std::string(reason));
            _qualityStatus = "Candidate rejected: " + std::string(reason);
        }

        void FinishQuality(bool cancelled, std::string reason = {})
        {
            if (cancelled) Log::Warning("Quality", "Bistro quality run cancelled: ", reason);
            _flight.reset();
            _qualitySearch.reset();
            _qualityRequest.reset();
            _qualityActive = false;
            Engine::UI::SuspendSettings(false);
            RestoreQualityBaseline();
            _basePatch.clear();
            _qualityLock.reset();
            _qualityStatus = cancelled ? "Bistro quality run cancelled: " + reason : "Bistro quality run complete.";
            std::ofstream summary(_qualityOutput / "result.json");
            summary << "{\n  \"schema\": " << BistroQuality::SchemaVersion
                << ",\n  \"cancelled\": " << std::boolalpha << cancelled
                << ",\n  \"cancellation_reason\": " << std::quoted(reason) << ",\n  \"trials\": " << _qualityRun;
            if (_qualityWinner)
            {
                const auto& winner = *_qualityWinner;
                _qualityStatus = (winner.TargetReached ? "Target reached: " : "Target not reached. Best measured: ") +
                    std::to_string(winner.AverageFps) + " FPS at " + std::to_string(winner.Settings.Width) + " × " +
                    std::to_string(winner.Settings.Height);
                summary << ",\n  \"profile\": \"" << (*_qualityProfile == Profile::Quality ? "Quality" : "Performance")
                    << "\",\n  \"targetReached\": " << winner.TargetReached << ",\n  \"stable\": " << winner.Stable
                    << ",\n  \"avg_fps\": " << std::setprecision(12) << winner.AverageFps
                    << ",\n  \"width\": " << winner.Settings.Width << ",\n  \"height\": " << winner.Settings.Height
                    << ",\n  \"tiers\": {";
                const char* separator = "";
                for (size_t i = 0; i < _qualityTechniques.size(); ++i)
                {
                    summary << separator << "\n    " << std::quoted(_qualityTechniques[i]->Name) << ": "
                        << std::quoted(BistroQuality::TierNames[winner.Settings.Tiers[i]]);
                    separator = ",";
                }
                summary << "\n  }";
            }
            else if (_qualityProfile)
            {
                summary << ",\n  \"targetReached\": false";
                if (!cancelled) _qualityStatus = "No valid measured candidate. Original settings restored.";
            }
            summary << "\n}\n";
            summary.close();
            if (!summary) throw std::runtime_error("Cannot export Bistro result");
            if (_qualityWinner && !_qualitySaveWinner.empty())
                CheckQualityConfig(ConsoleModule{}.SavePatchIni(_qualitySaveWinner,
                    BistroQuality::SavedProfilePatch(_winnerPatch, _qualityWinner->Settings)));
            if (_qualityExit) PostQuitMessage(cancelled || (_qualityProfile && !_qualityWinner) ? EXIT_FAILURE : EXIT_SUCCESS);
        }

        std::filesystem::path SavedQualityProfile()
        {
            const auto luid = Locator.GetModule<RenderModule>()->GetMGPUDevices().Primary->GetAdapterLuid();
            const auto& status = Locator.GetModule<RenderModule>()->GetMGPUStatus();
            const auto file = "Bistro-" + std::to_string(static_cast<std::uint32_t>(luid.HighPart)) + "-" +
                std::to_string(luid.LowPart) + (status.Requested ? "-secondary-" + std::to_string(status.ActiveSecondaryLuid) : "") + ".ini";
            return Engine::Core::GetExecutableDirectory() / "Config" / "Profiles" / file;
        }

        void ProcessQualityAction()
        {
            const bool toggle = !bInactive && (GetAsyncKeyState(VK_F2) & 0x8000);
            if (toggle && !_qualityPanelKey) _qualityPanel = !_qualityPanel;
            _qualityPanelKey = toggle;
            const auto action = std::exchange(_qualityAction, 0);
            if (!action) return;
            try
            {
                if (action == 1 || action == 2) StartQuality(action == 1 ? Profile::Performance : Profile::Quality, {}, false);
                if (action == 3) FinishQuality(true, "Cancelled from the quality panel.");
                if (action == 4) ApplyPresetPatch(_winnerPatch, BistroQuality::ApplyMode::RestartWorld, QualitySeed, true);
                if (action == 5)
                {
                    const auto path = SavedQualityProfile();
                    CheckQualityConfig(ConsoleModule{}.SavePatchIni(path,
                        BistroQuality::SavedProfilePatch(_winnerPatch, _qualityWinner->Settings)));
                    _qualityStatus = "Saved " + path.u8string();
                }
                if (action == 6)
                {
                    const auto& technique = BistroQuality::Techniques[_manualTechnique];
                    const auto patch = BistroQuality::Preset(technique, _manualTier);
                    if (technique.Mode == BistroQuality::ApplyMode::RestartProcess)
                    {
                        CheckQualityConfig(ConsoleModule{}.SavePatchIni(Engine::Core::GetExecutableDirectory() /
                            "Config" / "BistroSampler.ini", patch));
                        _qualityStatus = "Restart with -Quality.Overlay=Config/BistroSampler.ini";
                    }
                    else
                    {
                        ApplyPresetPatch(patch, technique.Mode, _characterSeed);
                        _qualityStatus = std::string(technique.Name) + ": " + BistroQuality::TierNames[_manualTier];
                    }
                }
                if (action == 7) RestartWorld(_characterSeed);
                if (action == 8)
                {
                    ConsoleVariablePatch patch;
                    CheckQualityConfig(ConsoleModule::ReadPatchIni(SavedQualityProfile(), patch));
                    ApplyPresetPatch(patch, BistroQuality::ApplyMode::RestartWorld, QualitySeed, true);
                    _qualityStatus = "Applied saved device profile.";
                }
            }
            catch (const std::exception& error)
            {
                if (!_bounds || !Locator.GetModule<SceneManagerModule>()->GetWorld()) throw;
                _qualityStatus = error.what();
            }
        }

        void DrawQualityPanel()
        {
            if (!_qualityPanel && !_qualityActive) return;
            static const auto names = []
            {
                std::array<const char*, std::size(BistroQuality::Techniques)> values{};
                for (size_t i = 0; i < values.size(); ++i) values[i] = BistroQuality::Techniques[i].Name;
                return values;
            }();
            std::array<const char*, names.size()> current{};
            if (!_qualityActive)
                for (size_t i = 0; i < current.size(); ++i)
                    current[i] = BistroQuality::CurrentTier(BistroQuality::Techniques[i], ConsoleModule{});
            _qualityAction = Engine::UI::DrawBistroQualityPanel(_qualityPanel, _qualityStatus, _qualityActive,
                _qualityWinner.has_value(), names.data(), current.data(), names.size(), _manualTechnique, _manualTier);
        }

        struct UnlockQuality
        {
            void operator()(void* lock) const { ReleaseMutex(lock); CloseHandle(lock); }
        };
        std::unique_ptr<void, UnlockQuality> _qualityLock;
        std::optional<Profile> _qualityProfile;
        std::optional<BistroQualitySearch::Search> _qualitySearch;
        std::optional<BistroQualitySearch::Request> _qualityRequest;
        std::optional<BistroQualitySearch::Selection> _qualityWinner;
        std::optional<BistroBenchmarkFlight::Sampler> _flight;
        std::optional<double> _warmupFps;
        BistroRenderPipeline* _qualityPipeline = nullptr;
        std::map<std::string, BistroQualitySearch::FreshnessMeasurements> _freshness;
        std::set<std::string> _freshnessRequired;
        std::vector<std::vector<BistroQualitySearch::TechniqueMeasurement>> _routeSamples;
        std::filesystem::path _qualityOutput, _qualitySaveWinner;
        ConsoleVariablePatch _basePatch, _trialPatch, _winnerPatch;
        std::vector<const BistroQuality::Technique*> _qualityTechniques;
        std::string _qualityStatus = "Apply a technique preset or run the exterior benchmark.";
        int _manualTechnique = 0, _manualTier = 2, _qualityAction = 0;
        uint16_t _qualityWidth = 0, _qualityHeight = 0;
        size_t _qualityRun = 0, _setupFrames = 0;
        double _setupStarted = 0, _setupSeconds = 0, _measurementRequested = 0;
        uint64_t _measurementTicks = 0, _qualityWorkload = 0;
        Vector3 _qualityCameraLocation, _qualityCameraRotation;
        float _qualityCameraFov = 60;
        bool _qualityActive = false, _qualityExit = false, _trialReady = false;
        bool _qualityPanel = false, _qualityPanelKey = false, _qualityInterrupted = false, _qualityBackground = false;

        void DrawSceneBounds(World& world)
        {
            if (BistroRenderPipeline::IsCollisionViewEnabled()) return;
            auto renderer = Locator.GetModule<RenderModule>();
            _reflections.DrawBounds(world, *renderer);
            _bounds->DrawBounds(world, *renderer);
        }

        void UpdateSceneAdditions(World& world, float deltaSeconds)
        {
            _reflections.Update(world, deltaSeconds);
            _grass.Update(world, deltaSeconds);
            _water.Update(world, deltaSeconds);
            _characters->Update(world, deltaSeconds, _jobs, Locator.GetModule<PhysicsModule>().get());
        }

        BistroReflections::Scene _reflections;
        BistroGrass::Scene _grass;
        BistroWater::Scene _water;
        const unsigned _characterSeed;
        std::optional<BistroCharacters::Scene> _characters;
        std::unique_ptr<BistroBounds::Scene> _bounds;
        bool _benchmarkStarted = false;
        POINT _lastMousePos = {};
        float _cameraSpeed = 100.0f;
    };
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, PSTR, int)
{
#if defined(DEBUG) || defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    bool unattendedQuality = false;
    try
    {
        std::filesystem::path benchmarkOutput;
        std::filesystem::path qualityOverlay, qualityCustom, qualityOutput, qualitySaveWinner;
        std::optional<Profile> qualityProfile;
        int benchmarkFrames = 1;
        int benchmarkWarmupFrames = 0;
        std::wstring budgetArgument;
        int argumentCount = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments) throw std::runtime_error("Cannot read startup arguments");
        const std::unique_ptr<void, decltype(&LocalFree)> argumentStorage(arguments, &LocalFree);
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            constexpr std::wstring_view prefix = L"-Quality.Overlay=";
            constexpr std::wstring_view customPrefix = L"-Quality.Custom=";
            if (argument.substr(0, prefix.size()) == prefix)
            {
                if (!qualityOverlay.empty() || argument.size() == prefix.size())
                    throw std::invalid_argument("Specify one nonempty Quality.Overlay path");
                qualityOverlay = argument.substr(prefix.size());
            }
            if (argument.substr(0, customPrefix.size()) == customPrefix)
            {
                if (!qualityCustom.empty() || argument.size() == customPrefix.size())
                    throw std::invalid_argument("Specify one nonempty Quality.Custom path");
                qualityCustom = argument.substr(customPrefix.size());
            }
        }
        for (const auto& path : {ConsoleModule::GetDefaultConfigPath(), qualityOverlay, qualityCustom})
        {
            if (path.empty() || !std::filesystem::exists(path)) continue;
            ConsoleVariablePatch patch;
            CheckQualityConfig(ConsoleModule::ReadPatchIni(path, patch));
            BistroQuality::RegisterProfileShadowVariables(patch);
        }
        ConsoleModule{}.InitializeWithOverlay(qualityOverlay, qualityCustom);
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            constexpr std::wstring_view qualityOutputPrefix = L"-Quality.BenchmarkOutput=";
            constexpr std::wstring_view qualitySearchPrefix = L"-Quality.Search=";
            constexpr std::wstring_view qualitySavePrefix = L"-Quality.SaveWinner=";
            if (argument.substr(0, qualityOutputPrefix.size()) == qualityOutputPrefix)
            {
                unattendedQuality = true;
                qualityOutput = argument.substr(qualityOutputPrefix.size());
                if (qualityOutput.empty()) throw std::invalid_argument("Quality.BenchmarkOutput must be nonempty");
            }
            if (argument.substr(0, qualitySearchPrefix.size()) == qualitySearchPrefix)
            {
                unattendedQuality = true;
                const auto value = argument.substr(qualitySearchPrefix.size());
                if (value == L"Performance") qualityProfile = Profile::Performance;
                else if (value == L"Quality") qualityProfile = Profile::Quality;
                else throw std::invalid_argument("Quality.Search must be Performance or Quality");
            }
            if (argument.substr(0, qualitySavePrefix.size()) == qualitySavePrefix)
            {
                qualitySaveWinner = argument.substr(qualitySavePrefix.size());
                if (qualitySaveWinner.empty()) throw std::invalid_argument("Quality.SaveWinner must be nonempty");
            }
            constexpr std::wstring_view wavesPrefix = L"-App.Physics.Waves.";
            if (argument.substr(0, wavesPrefix.size()) == wavesPrefix)
            {
                const auto equals = argument.find(L'=');
                if (equals == std::wstring_view::npos || !ConsoleModule{}.FindConsoleVariable(argument.substr(1, equals - 1)).SetFromString(
                    WStringToString(std::wstring(argument.substr(equals + 1)))))
                    throw std::invalid_argument("Invalid App.Physics.Waves argument");
            }
            constexpr std::wstring_view physicsPrefix = L"-App.Physics.CountScale=";
            if (argument.substr(0, physicsPrefix.size()) == physicsPrefix &&
                !BistroPhysics::CountScale.SetFromString(WStringToString(std::wstring(argument.substr(physicsPrefix.size())))))
                throw std::invalid_argument("App.Physics.CountScale must be finite and within 0..16");
            constexpr std::wstring_view particlesPrefix = L"-App.Particles.CountScale=";
            if (argument.substr(0, particlesPrefix.size()) == particlesPrefix &&
                !BistroParticles::CountScale.SetFromString(WStringToString(std::wstring(argument.substr(particlesPrefix.size())))))
            {
                Log::Error("Application", "App.Particles.CountScale must be finite and at most 4194304/250000");
                Log::Flush();
                return EXIT_FAILURE;
            }
            constexpr std::wstring_view budgetPrefix = L"-Loading.TransientBudgetMiB=";
            if (argument.substr(0, budgetPrefix.size()) == budgetPrefix) budgetArgument = argument;
            constexpr std::wstring_view prefix = L"-Loading.BenchmarkOutput=";
            if (argument.substr(0, prefix.size()) == prefix)
                benchmarkOutput = std::wstring(argument.substr(prefix.size()));
            constexpr std::wstring_view framesPrefix = L"-Loading.BenchmarkFrames=";
            constexpr std::wstring_view warmupPrefix = L"-Loading.BenchmarkWarmupFrames=";
            if (argument.substr(0, warmupPrefix.size()) == warmupPrefix)
            {
                const std::wstring value(argument.substr(warmupPrefix.size()));
                size_t consumed = 0;
                benchmarkWarmupFrames = std::stoi(value, &consumed);
                if (consumed != value.size() || benchmarkWarmupFrames < 0 || benchmarkWarmupFrames > 10000)
                    throw std::runtime_error("Loading.BenchmarkWarmupFrames must be between 0 and 10000");
            }
            if (argument.substr(0, framesPrefix.size()) == framesPrefix)
            {
                const std::wstring value(argument.substr(framesPrefix.size()));
                size_t consumed = 0;
                benchmarkFrames = std::stoi(value, &consumed);
                if (consumed != value.size() || benchmarkFrames < 1 || benchmarkFrames > 10000)
                    throw std::runtime_error("Loading.BenchmarkFrames must be between 1 and 10000");
            }
        }
        if ((!qualityOutput.empty() || qualityProfile) && !benchmarkOutput.empty())
            throw std::invalid_argument("Choose either the loading benchmark or the quality benchmark");
        if (!qualitySaveWinner.empty() && !qualityProfile)
            throw std::invalid_argument("Quality.SaveWinner requires Quality.Search");
        const auto started = std::chrono::steady_clock::now();
        // Keep ordinary benchmark processes on the same character routes and wave seed.
        BistroApp app(instance, benchmarkOutput.empty() && !BistroQuality::MGPUProfile.GetValue() ?
            std::random_device{}() : BistroQuality::WorldSeed);
        if (!budgetArgument.empty()) ApplyLoadingArgument(budgetArgument);
        if (!app.Initialize()) return EXIT_FAILURE;
        if (!qualityOutput.empty() || qualityProfile)
        {
            app.StartQuality(qualityProfile, std::move(qualityOutput), true, std::move(qualitySaveWinner));
            return app.Run();
        }
        const auto initialized = std::chrono::steady_clock::now();
        if (!benchmarkOutput.empty())
        {
            app.BenchmarkFrame();
            BenchmarkEngine::GetLocator().GetModule<RenderModule>()->WaitForUploads();
            const auto ready = std::chrono::steady_clock::now();
            app.ResetBenchmarkClock();
            for (int frame = 0; frame < benchmarkWarmupFrames; ++frame) app.BenchmarkFrame();
            const auto requestedStart = app.BenchmarkRequested;
            const auto ticksStart = app.BenchmarkTicks;
            std::vector<double> frameSamples;
            frameSamples.reserve(benchmarkFrames - 1);
            const auto measuring = std::chrono::steady_clock::now();
            for (int frame = 1; frame < benchmarkFrames; ++frame)
            {
                const auto start = std::chrono::steady_clock::now();
                app.BenchmarkFrame();
                frameSamples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            BenchmarkEngine::GetLocator().GetModule<RenderModule>()->WaitForUploads();
            const auto sampled = std::chrono::steady_clock::now();
            auto renderer = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            const auto gpuMemory = renderer->GetMemoryStatistics();
            const auto graph = renderer->GetRenderGraphStatistics();
            std::ofstream graphOutput(benchmarkOutput.wstring() + L".dot");
            renderer->WriteRenderGraph(graphOutput);
            PROCESS_MEMORY_COUNTERS_EX memory = {};
            memory.cb = sizeof(memory);
            if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
                throw std::runtime_error("Cannot read process memory counters");
            std::ofstream report(benchmarkOutput);
            if (!report) throw std::runtime_error("Cannot open loading benchmark output");
            report << "{\n  \"initialize_seconds\": " << std::chrono::duration<double>(initialized - started).count()
                << ",\n  \"ready_seconds\": " << std::chrono::duration<double>(ready - started).count()
                << ",\n  \"budget_mib\": " << GetLoadingBudgetBytes() / (1024u * 1024u)
                << ",\n  \"import_seconds\": " << LastLoadingMeasurements.ImportSeconds
                << ",\n  \"texture_seconds\": " << LastLoadingMeasurements.TextureSeconds
                << ",\n  \"publish_seconds\": " << LastLoadingMeasurements.PublishSeconds
                << ",\n  \"prepared_textures\": " << LastLoadingMeasurements.TextureCount
                << ",\n  \"max_import_tasks\": " << LastLoadingMeasurements.MaxImportTasks
                << ",\n  \"max_texture_tasks\": " << LastLoadingMeasurements.MaxTextureTasks
                << ",\n  \"sampled_frames\": " << benchmarkFrames - 1
                << ",\n  \"warmup_frames\": " << benchmarkWarmupFrames
                << ",\n  \"character_seed\": 12345"
                << ",\n  \"physics_requested_seconds\": " << app.BenchmarkRequested - requestedStart
                << ",\n  \"physics_ticks\": " << app.BenchmarkTicks - ticksStart
                << ",\n  \"physics_simulated_seconds\": " << double(app.BenchmarkTicks - ticksStart) * (1.0f / 60.0f)
                << ",\n  \"physics_debt_start\": " << requestedStart - double(ticksStart) * (1.0f / 60.0f)
                << ",\n  \"physics_debt_end\": " << app.BenchmarkRequested - double(app.BenchmarkTicks) * (1.0f / 60.0f)
                << ",\n  \"physics_count_scale\": " << BistroPhysics::CountScale.GetValue()
                << ",\n  \"particle_count_scale\": " << BistroParticles::CountScale.GetValue()
                << ",\n  \"particle_budget\": " << BistroParticles::GetBudget()
                << ",\n  \"frame_ms\": " << (benchmarkFrames > 1 ? std::chrono::duration<double, std::milli>(sampled - measuring).count() / (benchmarkFrames - 1) : 0)
                << ",\n  \"graph_passes\": " << graph.Passes
                << ",\n  \"graph_culled_passes\": " << graph.CulledPasses
                << ",\n  \"graph_logical_textures\": " << graph.TransientTextures
                << ",\n  \"graph_physical_textures\": " << graph.PhysicalTextures
                << ",\n  \"graph_logical_bytes\": " << graph.LogicalBytes
                << ",\n  \"graph_physical_bytes\": " << graph.PhysicalBytes
                << ",\n  \"gpu_allocation_count\": " << gpuMemory.AllocationCount
                << ",\n  \"gpu_block_count\": " << gpuMemory.BlockCount
                << ",\n  \"gpu_allocation_bytes\": " << gpuMemory.AllocationBytes
                << ",\n  \"gpu_block_bytes\": " << gpuMemory.BlockBytes
                << ",\n  \"working_set_bytes\": " << memory.WorkingSetSize
                << ",\n  \"private_bytes\": " << memory.PrivateUsage
                << ",\n  \"peak_working_set_bytes\": " << memory.PeakWorkingSetSize
                << ",\n  \"peak_commit_bytes\": " << memory.PeakPagefileUsage
                << ",\n  \"frame_samples_ms\": [";
            for (size_t i = 0; i < frameSamples.size(); ++i) report << (i ? ", " : "") << frameSamples[i];
            report << "]\n}\n";
            report.close();
            if (!report) throw std::runtime_error("Cannot write loading benchmark output");
            return EXIT_SUCCESS;
        }
        return app.Run();
    }
    catch (const ConsoleConfigError&)
    {
        return EXIT_FAILURE;
    }
    catch (const DxException& error)
    {
        Log::Error("Application", "DirectX failure: ", error.ToString());
        Log::Flush();
        if (!unattendedQuality) MessageBoxW(nullptr, error.ToString().c_str(), L"Bistro App", MB_OK | MB_ICONERROR);
    }
    catch (const std::exception& error)
    {
        Log::Error("Application", "Application failed: ", error.what());
        Log::Flush();
        if (!unattendedQuality) MessageBoxA(nullptr, error.what(), "Bistro App", MB_OK | MB_ICONERROR);
    }
    return EXIT_FAILURE;
}
