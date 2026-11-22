#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Systems/LookAtTargetSystem.h"
#include "App.Base/Systems/SplineFollowSystem.h"
#include "BistroArch.h"
#include "BistroBenchmarkRoute.h"
#include "BistroBenchmarkCsv.h"
#include "BistroCharacters.h"
#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Components/FluidRenderComponent.h"
#include "BistroEntrance.h"
#include "Components/BistroInteractionSource.h"
#include "Common/ConsoleVariables.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include <imgui/imgui.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <bitset>
#include <deque>
#include <optional>
#include <tuple>

class BistroBenchmark
{
    using Clock = std::chrono::steady_clock;
    using Point = BistroBenchmarkRoute::Point;
    struct Pose { Vector3 Position, Forward; };

    class Shot
    {
    public:
        template<size_t N> explicit Shot(const Point (&points)[N])
            : _points(points), _count(N)
        {
            for (size_t i = 0; i + 1 < N; ++i)
            {
                const auto delta = points[i+1].Position - points[i].Position;
                const float length = delta.Length();
                assert(length > 0);
                _distances.push_back(_distances.back() + length);
                _curve.Points.back().LeaveTangent = Tangent(i) * (length / 3);
                SplinePoint next{};
                next.Position = points[i+1].Position;
                next.ArriveTangent = Tangent(i+1) * (-length / 3);
                _curve.Points.push_back(next);
            }
            _times.resize(N);
        }

        float Distance() const { return _distances.back(); }
        float Distance(size_t begin, size_t end) const { return _distances[end] - _distances[begin]; }

        template<class Pace> void SetPace(Pace secondsPerCm)
        {
            for (size_t i = 1; i < _times.size(); ++i)
                _times[i] = _times[i - 1] + _points[i - 1].HoldSeconds + Distance(i - 1, i) * secondsPerCm(i);
        }

        float Duration() const { return _times.back(); }
        float At(size_t i) const { return _times.at(i); }

        Pose Sample(float time) const
        {
            time = std::clamp(time, 0.f, Duration());
            const auto next = std::upper_bound(_times.begin(), _times.end(), time);
            const size_t i = std::min(size_t(next - _times.begin() - 1), _count - 2);
            const float departure = _times[i] + _points[i].HoldSeconds;
            if (time < departure)
            {
                const auto& point = _points[i];
                return {point.Position, point.Target - point.Position};
            }
            const float u = (time - departure) / (_times[i + 1] - departure);
            const float speed = SegmentSpeed(i);
            const float start = KnotSpeed(i) / speed;
            const float finish = KnotSpeed(i + 1) / speed;
            // Hermite timing shares the same velocity at each knot and stops at a held pose.
            const float t = u * (start + u * (3 - 2 * start - finish + u * (start + finish - 2)));
            const auto position = EvaluateSplineSegment(_curve.Points[i], _curve.Points[i+1], t);
            if (_points[i].AlongPaintingWall && _points[i + 1].AlongPaintingWall)
            {
                // Height and path knots do not interrupt the pan between paintings.
                size_t from = i;
                size_t to = i + 1;
                while (_points[from].Target == Vector3::Zero) --from;
                while (_points[to].Target == Vector3::Zero) ++to;
                const float panStart = _times[from] + _points[from].HoldSeconds;
                const float panTime = (time - panStart) / (_times[to] - panStart);
                return {position, BlendDirection(_points[from].Target - position,
                    _points[to].Target - position, panTime)};
            }
            auto tangent = EvaluateSplineSegment(_curve.Points[i], _curve.Points[i+1], std::min(1.f,t + .002f)) -
                EvaluateSplineSegment(_curve.Points[i], _curve.Points[i+1], std::max(0.f,t - .002f));
            tangent.Normalize();
            const auto look = [&](const Point& p)
            {
                if (p.Target == Vector3::Zero) return tangent;
                auto direction = p.Target - position;
                direction.Normalize();
                return direction;
            };
            const auto a = look(_points[i]);
            const auto b = look(_points[i+1]);
            return {position, BlendDirection(a, b, t)};
        }
    private:
        float SegmentSpeed(size_t i) const
        {
            return Distance(i, i + 1) / (_times[i + 1] - _times[i] - _points[i].HoldSeconds);
        }

        float KnotSpeed(size_t i) const
        {
            if (_points[i].HoldSeconds > 0) return 0;
            if (i == 0) return SegmentSpeed(0);
            if (i + 1 == _count) return SegmentSpeed(i - 1);
            return std::min(SegmentSpeed(i - 1), SegmentSpeed(i));
        }

        Vector3 Tangent(size_t i) const
        {
            size_t before = i > 0 ? i - 1 : i;
            size_t after = std::min(i + 1, _count - 1);
            if (_points[i].AlongPaintingWall)
            {
                if (!_points[before].AlongPaintingWall) before = i;
                if (!_points[after].AlongPaintingWall) after = i;
            }
            auto result = _points[after].Position - _points[before].Position;
            if (result.LengthSquared() < .01f)
                result = _points[i+1].Position - _points[i].Position;
            result.Normalize();
            return result;
        }
        const Point* _points;
        size_t _count;
        SplineCurveComponent _curve = [&]
        {
            SplineCurveComponent curve;
            SplinePoint first{};
            first.Position = _points[0].Position;
            curve.Points.push_back(first);
            return curve;
        }();
        std::vector<float> _distances{0};
        std::vector<float> _times;
    };

    static constexpr std::array Techniques{
        L"App.Async.AO", L"App.Async.Environment", L"App.Async.LightGrid", L"App.Async.Particles",
        L"App.Async.Water", L"App.Async.CloudPicture"};
    static constexpr double Budget = 600, FinishReserve = 4, Warmup = 3;
    static constexpr unsigned Cases = 1u << Techniques.size();
    inline static AutoConsoleVariable<float> FlightSeconds{L"App.Benchmark.FlightSeconds", 100.f,
        L"Flight duration excluding warmup, including five fixed painting stops.",
        EConsoleVariableFlags::Default,
        [](float seconds) { return std::isfinite(seconds) && seconds >= BistroBenchmarkRoute::TotalPaintingHoldSeconds + 1; }};
    enum class Action { None, Automatic, Search, Run, Exhaustive, Cancel };
    enum class Stage { Run, Baseline, Control, Repeat, Singles, Pairs, Neighbors, Elimination, Tiebreak, Final, Exhaustive };
    using Stats = BistroBenchmarkData::Stats;
    using Csv = BistroBenchmarkData::Csv;
    using Config = std::vector<std::pair<std::string, std::string>>;
    struct Window { double Begin, End, Weight, AdditionalWarmup = 0; };
    using Windows = std::array<Window, 4>;
    using Measurements = std::array<Stats, 4>;
    using PhysicsStates = std::array<std::function<void()>, 4>;
    struct Checkpoint { double Time; std::function<void()> Restore; };
    struct Probe
    {
        Window Range{};
        Stats Reference;
        unsigned Groups = 0;
        size_t Phase = 0;
        std::function<void()> Restore;
    };
    struct Rating
    {
        std::array<double, 4> Gain{};
        std::array<unsigned, 4> Samples{};

        void Add(size_t phase, double gain)
        {
            Gain[phase] += gain;
            ++Samples[phase];
        }
    };

public:
    inline static AutoConsoleVariable<bool> AutoStart{L"App.Benchmark.AutoStart", false,
        L"Run quick Bistro queue tuning at Release startup."};
    inline static AutoConsoleVariable<bool> SkipBenchmark{L"App.Benchmark.SkipBenchmark", false,
        L"Skip automatic Bistro queue tuning at Release startup."};

    bool Active() const { return _active; }

    static std::filesystem::path DefaultsPath()
    {
        return ConsoleModule::GetDefaultConfigPath().parent_path() / "BistroQueueDefaults.ini";
    }

    void RequestAutomatic() { if (!_active) Request(Action::Automatic); }

    static int QueueGroup(std::string_view name)
    {
        const auto starts = [name](std::string_view prefix) { return name.substr(0, prefix.size()) == prefix; };
        if (starts("AO ") || starts("CACAO ") || starts("XeGTAO ") || starts("SSAO/")) return 0;
        if (starts("cached environment / ") || starts("camera environment / ")) return 1;
        if (name == "GPU light grid masks") return 2;
        if (starts("particle simulation ")) return 3;
        if (starts("water seed / ") || name == "water pack position and speed") return 4;
        if (name == "cloud picture generation" || name == "cloud picture GPU0 import") return 5;
        return -1;
    }

    void Stop(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, const char* status = "cancelled")
    {
        if (!_active) return;
        try { Finish(world, console, pipeline, status); }
        catch (const std::exception& error) { Fail(world, console, pipeline, error.what()); }
    }

    void Initialize(World& world, BistroCharacters::Scene& characters)
    {
        _characters = &characters;
        if (std::filesystem::exists(DefaultsPath())) _startupDefault = CurrentMask(ConsoleModule{});
        world.GetECS().ForEach<NameComponent>([&](Entity entity, const NameComponent& name)
        {
            for (size_t leaf = 0; leaf < _doors.size(); ++leaf)
                if (name.value == BistroEntrance::Names[leaf]) _doors[leaf] = entity;
            if (name.value == "BistroEntranceFountain") _fountain = entity;
        });
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, MAX_PATH) && ImGui::GetCurrentContext())
        {
            const auto font = std::filesystem::path(windows) / "Fonts/segoeui.ttf";
            if (std::filesystem::is_regular_file(font))
            {
                static constexpr ImWchar glyphs[]{0x20, 0xff, 0x410, 0x44f, 0};
                _font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font.u8string().c_str(), 16.f, nullptr, glyphs);
            }
        }
    }

    void DrawControls()
    {
        if (!_active)
        {
            if (_font) ImGui::PushFont(_font);
            if (ImGui::Button(_font ? u8"\u0411\u044b\u0441\u0442\u0440\u044b\u0439" : "Quick"))
                Request(Action::Search);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Tune queues using short samples and a full finalist run, up to 10 minutes.");
            ImGui::SameLine();
            if (ImGui::Button(_font ? u8"\u041f\u043e\u043b\u043d\u044b\u0439" : "Full"))
                Request(Action::Exhaustive);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%u full flights, about %.0f minutes. Saves the fastest layout as local startup defaults.",
                    Cases, Cases * (Warmup + FlightSeconds.GetValue()) / 60.);
            ImGui::SameLine();
            if (ImGui::Button(_font ? u8"\u041f\u0440\u043e\u0433\u043e\u043d" : "Run"))
                Request(Action::Run);
            if (_font) ImGui::PopFont();
            if (!_message.empty()) DrawRunName(_message.c_str());
            if (!_savedPath.empty()) DrawRunName(_savedPath.c_str());
            return;
        }

        if (ImGui::SmallButton("Cancel")) _action = Action::Cancel;
        ImGui::SameLine();
        if (_stage == Stage::Exhaustive)
            ImGui::Text("%s %u/%u | %.1fs", StageName(), _mask + 1, Cases, SecondsSince(_started));
        else
            ImGui::Text("%s | %.1fs", StageName(), SecondsSince(_started));
        if (_search) DrawRunName(PlacementName(_mask).c_str());
        const double pass = LegWarmup() + _window.End - _preroll;
        const double flightProgress = std::clamp(_elapsed / pass, 0., 1.);
        const auto progress = _stage == Stage::Exhaustive ? (_mask + flightProgress) / Cases : flightProgress;
        ImGui::ProgressBar(float(progress), {-1, 0},
            Measuring() ? SectionName(Section(_routeTime)) : "Warmup");
    }

    void Update(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline)
    {
        const auto action = std::exchange(_action, Action::None);
        try
        {
            if (action == Action::Cancel) { Stop(world, console, pipeline); return; }
            if (action != Action::None && !_active) Start(world, console, pipeline, action);
            if (!_active) return;
            if (TimedSearch() && Remaining() <= FinishReserve)
            {
                Finish(world, console, pipeline, "deadline", 0);
                return;
            }
            if (_legDone) CompleteLeg(world, console, render, pipeline);
            if (!_active) return;

            _routeTime = std::min(_window.End, _preroll + std::max(0., _elapsed - LegWarmup()));
            if (_stage == Stage::Baseline)
            {
                CaptureWindows(world, pipeline, _windows, _windowPhysics);
                CaptureWindows(world, pipeline, _extraWindows, _extraPhysics);
                CaptureCheckpoint(world, pipeline);
            }
            if (_search) pipeline.SetBenchmarkTime(Warmup + _routeTime);
            const auto pose = Flight(float(_routeTime));
            auto& ecs = world.GetECS();
            auto& camera = ecs.Get<TransformComponent>(_camera);
            camera.Location = pose.Position;
            camera.Rotation = MakeEulerFromForwardLH(pose.Forward);
            camera.DirtyFlag = true;
            AnimateDoor(ecs, float(_routeTime));
            const auto source = pose.Position - Vector3(0, 95, 0);
            auto& influence = ecs.Get<BistroInteractionSource>(_camera);
            const Vector3 previous(influence.PositionRadius.x, influence.PositionRadius.y, influence.PositionRadius.z);
            const auto start = (source - previous).LengthSquared() > 250000 ? source : previous;
            const float ceiling = BistroArch::SwarmBounds.Center.y + BistroArch::SwarmBounds.Extents.y;
            influence.PositionRadius = {source.x, source.y, source.z, 180};
            influence.PreviousHeight = {start.x, start.y, start.z, std::max(190.f, ceiling + 40.f - source.y)};
            _frameStart = Clock::now();
        }
        catch (const std::exception& error) { Fail(world, console, pipeline, error.what()); }
    }

    void FrameRendered(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline,
        bool presented)
    {
        if (!_active) return;
        const auto now = Clock::now();
        try
        {
            if (!presented)
            {
                Finish(world, console, pipeline, "interrupted: frame not presented");
                return;
            }
            if (Measuring() && _routeTime < _window.End)
            {
                if (!_legConfigured)
                {
                    const auto config = EffectiveConfig(render);
                    if (!_configured)
                    {
                        WriteConfig(console, pipeline, config);
                        if (!_search)
                        {
                            _searchConfig = RenderConfig(console);
                            _effective = config;
                        }
                    }
                    if (_stage == Stage::Baseline || (_stage == Stage::Exhaustive && _mask == 0))
                        _effective = config;
                    if (_search && config != _effective)
                        throw std::runtime_error("Bistro benchmark display or reconstruction changed");
                    _legConfigured = true;
                }
                unsigned groups = 0;
                if (_stage == Stage::Baseline || _stage == Stage::Control || _roundNeedsControl)
                {
                    for (const auto& packet : pipeline.LastSchedule().Packets)
                        if (const int group = QueueGroup(packet.Name); group >= 0)
                            groups |= 1u << group;
                    _legGroups |= groups;
                    if (_stage == Stage::Baseline)
                    {
                        _activity[Phase(_routeTime)] |= groups;
                        CaptureActivity(groups);
                    }
                }
                _frames.push_back({_routeTime, Seconds(_frameStart, now) * 1000,
                    Seconds(_lastPresent, now) * 1000, Section(_routeTime), groups});
            }
            _lastPresent = now;
            _elapsed = Seconds(_legStarted, now);
            _legDone = _elapsed >= LegWarmup() + _window.End - _preroll;
            if (TimedSearch() && Remaining() <= FinishReserve)
                Finish(world, console, pipeline, "deadline", 0);
        }
        catch (const std::exception& error) { Fail(world, console, pipeline, error.what()); }
    }
private:
    static void DrawRunName(const char* name)
    {
        const auto min = ImGui::GetCursorScreenPos();
        const auto width = ImGui::GetContentRegionAvail().x;
        const auto height = ImGui::GetTextLineHeight();
        if (width <= 0) return;
        const ImVec2 max{min.x + width, min.y + height};
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        draw->AddText(min, ImGui::GetColorU32(ImGuiCol_TextDisabled), name);
        draw->PopClipRect();
        ImGui::Dummy({width, height});
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(name).x > width)
            ImGui::SetTooltip("%s", name);
    }

    static float Ease(float t)
    {
        t = std::clamp(t, 0.f, 1.f);
        return t*t*(3 - 2*t);
    }

    static Vector3 BlendDirection(Vector3 a, Vector3 b, float t)
    {
        auto ra = MakeEulerFromForwardLH(a);
        const auto rb = MakeEulerFromForwardLH(b);
        const auto blend = Ease(t);
        ra.x += (rb.x - ra.x) * blend;
        ra.y += std::remainder(rb.y - ra.y, 360.f) * blend;
        const auto yaw = XMConvertToRadians(ra.y);
        const auto pitch = XMConvertToRadians(ra.x);
        return {std::cos(pitch)*std::sin(yaw), -std::sin(pitch), std::cos(pitch)*std::cos(yaw)};
    }

    float Duration() const { return 2 * _red.Duration() + _blue.Duration() + _green.Duration() + _yellow.Duration(); }

    void SetFlightDuration(float seconds)
    {
        using namespace BistroBenchmarkRoute;
        constexpr float movingSeconds = 75.f;
        constexpr float greenSeconds = 35.f;
        constexpr float exteriorSeconds = 20.f;
        constexpr float interiorSeconds = movingSeconds - greenSeconds - exteriorSeconds;
        const float scale = (seconds - TotalPaintingHoldSeconds) / movingSeconds;
        const float interior = _yellow.Distance(BistroBenchmarkRoute::EntryDoor, BistroBenchmarkRoute::ExitDoor);
        const float exterior = 2 * _red.Distance() + _blue.Distance() + _yellow.Distance() - interior;
        const float outsidePace = exteriorSeconds * scale / exterior;
        _red.SetPace([&](size_t) { return outsidePace; });
        _blue.SetPace([&](size_t) { return outsidePace; });
        _yellow.SetPace([&](size_t i)
        {
            return i > EntryDoor && i <= ExitDoor ? interiorSeconds * scale / interior : outsidePace;
        });

        constexpr size_t ArchEntrance = 4, ArchExit = 6, TreeStart = 9, TreeEnd = 21;
        constexpr size_t ReturnArchEntrance = 25, ReturnArchExit = 27;
        const float streetPace = 20.f / _green.Distance();
        constexpr float archPace = 1.f / 300.f, connectorPace = 1.f / 500.f;
        const float street = _green.Distance(0, ArchEntrance) + _green.Distance(ReturnArchExit, std::size(Green) - 1);
        const float arch = _green.Distance(ArchEntrance, ArchExit) + _green.Distance(ReturnArchEntrance, ReturnArchExit);
        const float connectors = _green.Distance(ArchExit, TreeStart) + _green.Distance(TreeEnd, ReturnArchEntrance);
        const float orbitSeconds = greenSeconds - street * streetPace - arch * archPace - connectors * connectorPace;
        const float orbitPace = orbitSeconds / _green.Distance(TreeStart, TreeEnd);
        _green.SetPace([&](size_t i)
        {
            if (i <= ArchEntrance || i > ReturnArchExit) return streetPace * scale;
            if (i <= ArchExit || i > ReturnArchEntrance) return archPace * scale;
            if (i <= TreeStart || i > TreeEnd) return connectorPace * scale;
            return orbitPace * scale;
        });
        assert(std::abs(Duration() - seconds) < seconds * .0001f);
    }

    Pose Flight(float time) const
    {
        const auto fly = [](const Shot& shot, const Shot& next, float t, float nextTime)
        {
            auto pose = shot.Sample(t);
            constexpr float transition = .65f;
            const float blend = (t - shot.Duration() + transition) / transition;
            if (blend > 0)
                pose.Forward = BlendDirection(pose.Forward, next.Sample(nextTime).Forward, blend);
            return pose;
        };
        if (time < _red.Duration()) return _red.Sample(time);
        time -= _red.Duration();
        if (time < _blue.Duration()) return fly(_blue, _green, time, 0);
        time -= _blue.Duration();
        if (time < _green.Duration()) return fly(_green, _yellow, time, 0);
        time -= _green.Duration();
        if (time < _yellow.Duration()) return fly(_yellow, _red, time, _red.Duration());
        return _red.Sample(_red.Duration() - (time - _yellow.Duration()));
    }

    void AnimateDoor(WorldECS& ecs, float time)
    {
        const float yellowTime = time - (_red.Duration() + _blue.Duration() + _green.Duration());
        const auto opening = [&](size_t threshold)
        {
            const auto offset = yellowTime - _yellow.At(threshold);
            return Ease((offset + 1.55f) / 1.15f) * Ease((1.55f - offset) / 1.15f);
        };
        const float swing = 85 * std::max(opening(BistroBenchmarkRoute::EntryDoor), opening(BistroBenchmarkRoute::ExitDoor));
        for (size_t leaf = 0; leaf < _doors.size(); ++leaf)
        {
            auto& door = ecs.Get<TransformComponent>(_doors[leaf]);
            const float angle = -BistroEntrance::Side(leaf) * swing;
            if (door.Rotation.y == angle) continue;
            const auto hinge = BistroEntrance::Hinge(leaf);
            door.Location = hinge - Vector3::Transform(hinge, BistroEntrance::Rotation(angle));
            door.Rotation.y = angle;
            door.DirtyFlag = true;
        }
        if (_fountain != InvalidEntity)
        {
            auto& fountain = ecs.Get<ParticleEmitterComponent>(_fountain);
            auto settings = fountain.GetSettings();
            const bool closed = swing == 0;
            // One particle box can cover the closed doorway only.
            if (settings.CollisionBoxEnabled != closed)
            {
                settings.CollisionBoxEnabled = closed;
                fountain.SetSettings(settings);
            }
        }
    }

    static ConsoleVariablePatch Placement(unsigned mask)
    {
        ConsoleVariablePatch patch;
        for (size_t i = 0; i < Techniques.size(); ++i)
            patch.emplace_back(Techniques[i], (mask & (1u << i)) ? "true" : "false");
        return patch;
    }

    static ConsoleVariablePatch EnabledPlacement(unsigned mask)
    {
        auto patch = Placement(mask);
        patch.emplace_back(L"App.Async.Queues", "true");
        return patch;
    }

    static unsigned CurrentMask(const ConsoleModule& console)
    {
        unsigned mask = 0;
        for (size_t i = 0; i < Techniques.size(); ++i)
            if (console.FindConsoleVariable(Techniques[i]).GetBool()) mask |= 1u << i;
        return mask;
    }

    static void Apply(ConsoleModule& console, const ConsoleVariablePatch& patch)
    {
        if (const auto result = console.ApplyPatch(patch); !result.Error.empty())
            throw std::runtime_error(result.Error);
    }

    static double Seconds(Clock::time_point begin, Clock::time_point end)
    {
        return std::chrono::duration<double>(end - begin).count();
    }
    static double SecondsSince(Clock::time_point begin) { return Seconds(begin, Clock::now()); }
    double Remaining() const { return Budget - SecondsSince(_started); }
    bool TimedSearch() const { return _search && _stage != Stage::Exhaustive; }
    double LegWarmup() const { return Warmup + _window.AdditionalWarmup; }
    bool Measuring() const { return _elapsed >= LegWarmup() + _window.Begin - _preroll; }

    void Request(Action action)
    {
        _requestedAt = Clock::now();
        _action = action;
    }

    const char* StageName() const
    {
        if (_roundNeedsControl && (_stage == Stage::Tiebreak || _stage == Stage::Elimination)) return "Short window control";
        constexpr std::array names{"Run", "Baseline", "Short window controls", "Control repeat", "Single transfers", "Combinations",
            "Neighbor masks", "Elimination expansion", "Boundary recheck", "Finalist", "Full sweep"};
        return names.at(size_t(_stage));
    }

    static std::string PlacementName(unsigned mask)
    {
        constexpr std::array names{"AO", "Environment", "LightGrid", "Particles", "Water", "CloudPicture"};
        static_assert(names.size() == Techniques.size());
        std::string result;
        for (size_t i = 0; i < names.size(); ++i)
            if (mask & (1u << i)) result += (result.empty() ? "COMPUTE: " : " + ") + std::string(names[i]);
        return result.empty() ? "Benchmark groups on DIRECT" : result;
    }

    size_t Section(double time) const
    {
        return size_t(std::upper_bound(_sections.begin(), _sections.end(), time) - _sections.begin());
    }

    static const char* SectionName(size_t section)
    {
        constexpr std::array names{"Approach", "Fountain", "Arch and tree", "Bistro interior", "Cloud painting",
            "Painting corridor", "Return"};
        return names.at(section);
    }

    size_t Phase(double time) const
    {
        return size_t(std::upper_bound(_phaseEnds.begin(), _phaseEnds.end() - 1, time) - _phaseEnds.begin());
    }

    void SetWindows()
    {
        const double blue = _red.Duration();
        const double green = blue + _blue.Duration();
        const double yellow = green + _green.Duration();
        const double entry = yellow + _yellow.At(BistroBenchmarkRoute::EntryDoor);
        const double clouds = yellow + _yellow.At(10); // Painting C hosts CloudPicture.
        const double returnFlight = yellow + _yellow.Duration();
        const double paintingEnd = clouds + BistroBenchmarkRoute::PaintingHoldSeconds;
        _sections = {blue, green, entry, clouds, paintingEnd, returnFlight};
        _phaseEnds = {green, entry, clouds, Duration()};
        const auto window = [](double begin, double end, double anchor, double length, double weight)
        {
            length = std::min(length, end - begin);
            begin = std::clamp(anchor, begin, std::max(begin, end - length));
            return Window{begin, begin + length, weight};
        };
        _windows = {{
            window(blue, green, blue + _blue.At(3), 1.5, green),
            window(green, yellow, green + _green.At(4), 1.5, entry - green),
            window(entry, clouds, yellow + _yellow.At(7), 1.5, clouds - entry),
            window(clouds, paintingEnd, clouds + .5, 1.5, Duration() - clouds),
        }};
        _extraWindows = {{
            window(blue, green, blue + _blue.At(10), 2., _windows[0].Weight),
            window(green, yellow, green + _green.At(13), 2., _windows[1].Weight),
            window(entry, clouds, entry, 2., _windows[2].Weight),
            window(paintingEnd, returnFlight, yellow + _yellow.At(16) + 1., 2., _windows[3].Weight),
        }};
    }

    static Config EffectiveConfig(RenderModule& render)
    {
        const auto& common = *render.GetRenderPipelineCommonData();
        const auto dimensions = [](unsigned width, unsigned height)
        {
            return std::to_string(width) + "x" + std::to_string(height);
        };
        return {{"OutputResolution", dimensions(common.WindowWidth, common.WindowHeight)},
            {"RenderResolution", dimensions(common.DownscaledWidth, common.DownscaledHeight)},
            {"EffectiveUpscaler", std::to_string(int(render.GetReconstructionStatus().Effective))},
            {"EffectiveFrameGeneration", std::to_string(int(render.GetFrameGenerationStatus().Effective))}};
    }

    static ConsoleVariablePatch RenderConfig(ConsoleModule& console)
    {
        ConsoleVariablePatch config;
        for (auto prefix : {L"App.", L"Render."})
            for (const auto* variable : console.GetConsoleVariables(prefix))
                config.emplace_back(variable->GetName(), variable->GetString());
        return config;
    }

    static ConsoleVariablePatch SearchConfig(ConsoleModule& console)
    {
        auto config = RenderConfig(console);
        config.erase(std::remove_if(config.begin(), config.end(), [](const auto& entry)
        {
            return std::find(Techniques.begin(), Techniques.end(), entry.first) != Techniques.end();
        }), config.end());
        return config;
    }

    void WriteConfig(ConsoleModule& console, const BistroRenderPipeline& pipeline, const Config& effective)
    {
        for (const auto& [name, value] : RenderConfig(console))
            _csv.Config(std::filesystem::path(name).u8string(), value);
        for (const auto& [name, value] : effective) _csv.Config(name, value);
        for (const auto& [name, value] : pipeline.BenchmarkAdapters()) _csv.Config(name, value);
        _configured = true;
    }

    void Start(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, Action action)
    {
        if (std::find(_doors.begin(), _doors.end(), InvalidEntity) != _doors.end())
            throw std::runtime_error("Both Bistro entrance door leaves are required");
        _started = _requestedAt;
        _search = action != Action::Run;
        _stage = action == Action::Exhaustive ? Stage::Exhaustive : _search ? Stage::Baseline : Stage::Run;
        _sweepBest = {};
        _sweepBestMask = 0;
        _message.clear();
        _savedPath.clear();
        _configured = false;
        _frames.clear();
        _activity = {};
        _seen = {};
        _scores = {};
        _ratings = {};
        _reference = {};
        _activityProbes = {};
        _checkpoints.clear();
        _round = 0;
        _coveredGroups = 0;
        _usedExpansions = {};
        _roundProbe = nullptr;
        _roundNeedsControl = false;
        _observedMeanNoise = 0;
        _meanThreshold = .01;
        _p95Threshold = .02;
        _probeScale = 1;
        _pending.clear();
        _previous.clear();
        _initialScene = {};
        _windowPhysics = {};
        _extraPhysics = {};
        _legOpen = false;
        _userMask = CurrentMask(console);
        _mask = _search ? 0 : _userMask;
        SetFlightDuration(FlightSeconds.GetValue());
        SetWindows();

        auto directory = Engine::Core::GetExecutableDirectory() / "BenchmarkResults";
        if (_stage == Stage::Exhaustive) directory /= "Exhaustive";
        std::filesystem::create_directories(directory);
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        const auto id = std::to_string(stamp) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++_runId);
        _path = directory / ("bistro-" + id + ".csv");
        _csv.Open(_path);
        _csv.Config("RunId", id);
#if defined(_DEBUG) || defined(DEBUG)
        _csv.Config("Build", "Debug");
#else
        _csv.Config("Build", "Release");
#endif
        _csv.Config("Mode", action == Action::Exhaustive ? "exhaustive_search" :
            action == Action::Automatic ? "automatic_search" : _search ? "manual_search" : "manual_run");
        _csv.Config("RouteVersion", "1");
        _csv.Config("RouteSeconds", Csv::Number(Duration()));
        _csv.Config("WarmupSeconds", Csv::Number(Warmup));
        for (size_t i = 0; i < Techniques.size(); ++i)
            _csv.Config("MaskBit" + std::to_string(i), std::filesystem::path(Techniques[i]).u8string());

        auto& ecs = world.GetECS();
        _camera = world.ActiveCamera;
        const auto& camera = ecs.Get<TransformComponent>(_camera);
        _previousPose = {camera.Location, camera.Rotation};
        _hadInfluence = ecs.Has<BistroInteractionSource>(_camera);
        if (_hadInfluence) _previousInfluence = ecs.Get<BistroInteractionSource>(_camera);
        else ecs.Add<BistroInteractionSource>(_camera);
        _active = true;
        if (_search)
        {
            auto patch = EnabledPlacement(0);
            patch.insert(patch.end(), {{L"Render.FrameGeneration.Provider", "0"}, {L"App.Sky.SunCycle.Enabled", "false"}});
            for (const auto* variable : console.GetConsoleVariables(L"App."))
            {
                const auto name = variable->GetName();
                const auto pos = name.find(L".Adapter");
                if (variable->GetFlags() == EConsoleVariableFlags::Default &&
                    ((pos != name.npos && (pos + 8 == name.size() || name[pos + 8] == L'.')) ||
                    name == L"App.ReflectionProbes.FirstAdapter"))
                    patch.emplace_back(name, "-1");
            }
            if (const auto result = console.ApplyPatch(patch, &_previous); !result.Error.empty())
                throw std::runtime_error(result.Error);
            _searchConfig = SearchConfig(console);
            if (TimedSearch() && Remaining() < Warmup + Duration() + FinishReserve)
            {
                Finish(world, console, pipeline, "baseline does not fit", 0);
                return;
            }
            _initialScene = CaptureScene(world, pipeline);
            if (_stage == Stage::Baseline) _checkpoints.push_back({-Warmup, _initialScene});
        }
        BeginLeg(world, pipeline, {0, Duration(), 0});
    }

    void BeginLeg(World& world, BistroRenderPipeline& pipeline, Window window)
    {
        _window = window;
        _preroll = std::max(0., window.Begin - 1.);
        _elapsed = 0;
        _routeTime = _preroll;
        _legDone = false;
        _legOpen = true;
        _legConfigured = false;
        _legGroups = 0;
        _frames.clear();
        auto& ecs = world.GetECS();
        ecs.Get<CameraComponent>(_camera).InvalidateHistory();
        ecs.Get<BistroInteractionSource>(_camera) = {};
        if (_search)
        {
            if (_stage == Stage::Final || _stage == Stage::Exhaustive) _initialScene();
            else if (_stage != Stage::Baseline)
            {
                const auto& restore = _roundProbe ? _roundProbe->Restore : _windowPhysics[_windowIndex];
                restore();
            }
            ecs.ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
            {
                const auto burst = emitter.GetBurstTotal();
                emitter.Reset();
                for (auto remaining = burst; remaining;)
                {
                    const auto count = UINT(std::min<uint64_t>(remaining, 1048576));
                    emitter.Emit(count);
                    remaining -= count;
                }
            });
            ecs.ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid) { fluid.Reset(); });
            pipeline.ResetBenchmarkClouds();
        }
        _lastPresent = _legStarted = Clock::now();
    }

    void CaptureWindows(World& world, BistroRenderPipeline& pipeline, const Windows& windows, PhysicsStates& snapshots)
    {
        for (size_t i = 0; i < windows.size(); ++i)
            if (!snapshots[i] && _routeTime >= windows[i].Begin - 1 - Warmup)
                snapshots[i] = _routeTime == 0 ? _initialScene : CaptureScene(world, pipeline);
    }

    std::function<void()> CaptureScene(World& world, BistroRenderPipeline& pipeline)
    {
        return [physics = pipeline.CaptureBenchmarkPhysics(world), characters = _characters->CaptureReplayState(world)]
        {
            physics();
            characters();
        };
    }

    void CaptureCheckpoint(World& world, BistroRenderPipeline& pipeline)
    {
        const double time = _elapsed - Warmup;
        if (time - _checkpoints.back().Time < .5) return;
        _checkpoints.push_back({time, CaptureScene(world, pipeline)});
        while (_checkpoints.size() > 1 && _checkpoints[1].Time <= time - Warmup - 3.)
            _checkpoints.pop_front();
    }

    void CaptureActivity(unsigned groups)
    {
        for (size_t group = 0; group < Techniques.size(); ++group)
        {
            auto& probe = _activityProbes[group];
            if (!(groups & (1u << group)) || probe.Restore) continue;
            const auto begin = std::min(_routeTime, double(Duration()) - 1.5);
            const double entry = std::max(0., begin - 1.) - Warmup;
            const auto checkpoint = std::find_if(_checkpoints.rbegin(), _checkpoints.rend(),
                [entry](const auto& sample) { return sample.Time <= entry; });
            if (checkpoint == _checkpoints.rend()) continue;
            probe = {{begin, begin + 1.5, 0, entry - checkpoint->Time}, {}, 0, Phase(begin), checkpoint->Restore};
        }
    }

    void WriteLeg(const char* status)
    {
        if (!_legOpen) return;
        _legOpen = false;
        const double end = std::clamp(_preroll + _elapsed - LegWarmup(), _window.Begin, _window.End);
        _csv.Leg(StageName(), _mask, _frames, _window.Begin, end, _elapsed, status, SectionName,
            _stage == Stage::Elimination || _stage == Stage::Tiebreak ? _round : 0,
            _window.Begin == 0 && _window.End == Duration());
    }

    static bool Comparable(Stats stats) { return stats.Count >= 3 && stats.Mean > 0 && stats.P95 > 0; }

    void ReportBaselineWindows(const Windows& windows)
    {
        for (const auto& window : windows)
            _csv.Window("Baseline window", 0, SectionName(Section(window.Begin)), window.Begin, window.End,
                BistroBenchmarkData::Measure(_frames, window.Begin, window.End));
    }

    void SetProbeActivity(Probe& probe)
    {
        for (const auto& frame : _frames)
            if (probe.Range.Begin <= frame.RouteSeconds && frame.RouteSeconds < probe.Range.End)
                probe.Groups |= frame.Groups;
    }

    void PrepareEliminationWindows()
    {
        for (size_t phase = 0; phase < _windows.size(); ++phase)
        {
            auto& primary = _primaryProbes[phase];
            auto& extra = _extraProbes[phase];
            primary = {_windows[phase], {}, 0, phase, _windowPhysics[phase]};
            extra = {_extraWindows[phase], {}, 0, phase, _extraPhysics[phase]};
            SetProbeActivity(primary);
            SetProbeActivity(extra);
        }
        for (auto& probe : _activityProbes)
            if (probe.Restore) SetProbeActivity(probe);
        _checkpoints.clear();
    }

    void Report(std::string_view name, double value) { _csv.Config(name, Csv::Number(value)); }

    double Score(const Rating& rating) const
    {
        double gain = 0, weight = 0;
        for (size_t phase = 0; phase < _windows.size(); ++phase)
        {
            assert(rating.Samples[phase] > 0);
            gain += _windows[phase].Weight * rating.Gain[phase] / rating.Samples[phase];
            weight += _windows[phase].Weight;
        }
        return gain / weight;
    }

    void Rank(std::vector<unsigned>& masks) const
    {
        std::sort(masks.begin(), masks.end(), [&](unsigned a, unsigned b)
        {
            if (_scores[a] != _scores[b]) return _scores[a] > _scores[b];
            return a < b;
        });
    }

    std::vector<unsigned> Leaders() const
    {
        std::vector<unsigned> masks;
        for (unsigned mask = 1; mask < Cases; ++mask)
            if (_scores[mask].value_or(0) > 0) masks.push_back(mask);
        Rank(masks);
        return masks;
    }

    void AddPairs()
    {
        unsigned combined = 0;
        const auto leaders = Leaders();
        for (size_t i = 0; i < std::min(size_t(3), leaders.size()); ++i) combined |= leaders[i];
        if (combined) _pending.push_back(combined);
        std::vector<std::pair<unsigned, double>> pairs;
        unsigned withoutGain = 0;
        double bestOverlap = -1;
        for (unsigned a = 1; a < Cases; a <<= 1)
            for (unsigned b = a << 1; b < Cases; b <<= 1)
            {
                if ((_activeGroups & (a | b)) != (a | b)) continue;
                double overlap = 0;
                for (size_t i = 0; i < _activity.size(); ++i)
                    if ((_activity[i] & (a | b)) == (a | b))
                        overlap += _phaseEnds[i] - (i ? _phaseEnds[i - 1] : 0);
                pairs.emplace_back(a | b, overlap);
                if (_scores[a] && *_scores[a] <= 0 && _scores[b] && *_scores[b] <= 0 && overlap > bestOverlap)
                {
                    withoutGain = a | b;
                    bestOverlap = overlap;
                }
            }
        if (withoutGain) _pending.push_back(withoutGain);
        std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        unsigned added = 0;
        for (const auto& pair : pairs)
            if (std::find(_pending.begin(), _pending.end(), pair.first) == _pending.end())
            {
                _pending.push_back(pair.first);
                if (++added == 2) break;
            }
    }

    void AddNeighbors()
    {
        auto leaders = Leaders();
        if (leaders.size() > 2) leaders.resize(2);
        for (const auto mask : leaders)
            for (unsigned bit = 1; bit < Cases; bit <<= 1)
                if ((_activeGroups & bit) && (mask ^ bit) && !_seen[mask ^ bit] &&
                    std::find(_pending.begin(), _pending.end(), mask ^ bit) == _pending.end())
                {
                    _pending.push_back(mask ^ bit);
                    break;
                }
    }

    static double NominalCost(const Windows& windows, size_t first = 0)
    {
        double seconds = 0;
        for (size_t i = first; i < windows.size(); ++i)
            seconds += Warmup + windows[i].End - std::max(0., windows[i].Begin - 1.);
        return seconds;
    }

    bool Fits(double seconds) const { return Remaining() > seconds * _probeScale + 1 + _finalReserve; }
    static double ProbeCost(const Probe& probe)
    {
        const auto& w = probe.Range;
        return Warmup + w.AdditionalWarmup + w.End - std::max(0., w.Begin - 1.);
    }

    void NextProbe(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
    {
        for (;;)
        {
            while (!_pending.empty())
            {
                const unsigned mask = _pending.front();
                _pending.pop_front();
                if (_seen[mask]) continue;
                double cost = NominalCost(_windows);
                if (_stage == Stage::Neighbors) cost += 6 * ((Leaders().size() + 1) / 2);
                if (!Fits(cost)) break;
                _seen[mask] = true;
                _mask = mask;
                _windowIndex = 0;
                _measurements = {};
                _probeStarted = Clock::now();
                Apply(console, Placement(mask));
                BeginLeg(world, pipeline, _windows[0]);
                return;
            }
            _pending.clear();
            switch (_stage)
            {
            case Stage::Control:
                _stage = Stage::Repeat;
                _seen.reset(0);
                _pending.push_back(0);
                break;
            case Stage::Repeat:
                if (!_repeatComparable)
                {
                    Finish(world, console, pipeline, "baseline windows not comparable", 0);
                    return;
                }
                _stage = Stage::Singles;
                for (unsigned bit = 1; bit < Cases; bit <<= 1)
                    if (_activeGroups & bit) _pending.push_back(bit);
                break;
            case Stage::Singles:
                _stage = Stage::Pairs;
                AddPairs();
                break;
            case Stage::Pairs:
                _stage = Stage::Neighbors;
                AddNeighbors();
                break;
            default:
                _ranked.clear();
                for (unsigned mask = 1; mask < Cases; ++mask)
                    if (_scores[mask]) _ranked.push_back(mask);
                Rank(_ranked);
                _round = 1;
                CutRound(world, console, pipeline);
                return;
            }
        }
    }

    void StartFinal(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
    {
        _roundProbe = nullptr;
        _roundNeedsControl = false;
        const bool gain = !_ranked.empty() && _scores[_ranked.front()].value_or(0) > 0;
        if (!gain || Remaining() < _finalReserve)
        {
            Finish(world, console, pipeline, gain ? "no time for finalist" : "no advantage in short comparisons", 0);
            return;
        }
        _stage = Stage::Final;
        _mask = _ranked.front();
        Apply(console, Placement(_mask));
        BeginLeg(world, pipeline, {0, Duration(), 0});
    }

    Probe* BoundaryProbe(unsigned missing)
    {
        if (!missing)
        {
            for (size_t i = 0; i < _extraProbes.size(); ++i)
                if (!_usedExpansions[i] && _extraProbes[i].Restore) return &_extraProbes[i];
            return &_primaryProbes.back();
        }
        Probe* best = nullptr;
        size_t covered = 0;
        const auto consider = [&](auto& probes)
        {
            for (auto& probe : probes)
            {
                const auto overlap = std::bitset<Techniques.size()>(probe.Groups & missing).count();
                if (overlap > covered && probe.Restore)
                {
                    best = &probe;
                    covered = overlap;
                }
            }
        };
        consider(_extraProbes);
        consider(_activityProbes);
        return best;
    }

    void StartRoundLeg(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
    {
        _roundNeedsControl = !Comparable(_roundProbe->Reference);
        _mask = _roundNeedsControl ? 0 : _roundMasks[_roundIndex];
        _probeStarted = Clock::now();
        Apply(console, Placement(_mask));
        BeginLeg(world, pipeline, _roundProbe->Range);
    }

    bool TryBoundaryCheck(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, unsigned missing)
    {
        const auto passing = _ranked[_cut - 1];
        auto* probe = BoundaryProbe(missing);
        if (!probe || !Fits((Comparable(probe->Reference) ? 2 : 3) * ProbeCost(*probe) + 1))
        {
            _csv.Decision(_round, passing, "recheck", probe ? "skipped: budget" : "skipped: no comparable active window");
            return false;
        }
        _stage = Stage::Tiebreak;
        _roundProbe = probe;
        _roundMasks = {passing, _ranked[_cut]};
        _roundIndex = 0;
        _csv.Decision(_round, passing, "recheck", "scheduled");
        _csv.Decision(_round, passing, "recheck_activity_mask", std::to_string(probe->Groups));
        if (Comparable(probe->Reference))
            _csv.Decision(_round, 0, "matched_control_mean_ms", Csv::Number(probe->Reference.Mean));
        StartRoundLeg(world, console, pipeline);
        return true;
    }

    void CutRound(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
    {
        for (const auto mask : _ranked)
            _csv.Decision(_round, mask, "ranked_gain", Csv::Number(*_scores[mask]));
        if (_ranked.size() < 2)
        {
            StartFinal(world, console, pipeline);
            return;
        }
        _cut = (_ranked.size() + 1) / 2;
        const auto passing = _ranked[_cut - 1];
        const auto dropping = _ranked[_cut];
        const double gap = *_scores[passing] - *_scores[dropping];
        const unsigned missing = (passing ^ dropping) & ~_coveredGroups;
        const bool close = gap <= _observedMeanNoise;
        _csv.Decision(_round, passing, "last_advancing", Csv::Number(*_scores[passing]));
        _csv.Decision(_round, dropping, "first_eliminated", Csv::Number(*_scores[dropping]));
        _csv.Decision(_round, passing, "boundary_gap", Csv::Number(gap));
        _csv.Decision(_round, passing, "uncovered_mask", std::to_string(missing));
        _csv.Decision(_round, passing, "boundary_reason",
            missing ? (close ? "noise and uncovered activity" : "uncovered activity") : (close ? "noise" : "clear"));
        if ((missing || close) && TryBoundaryCheck(world, console, pipeline, missing)) return;
        AdvanceRound(world, console, pipeline);
    }

    void AdvanceRound(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline)
    {
        for (size_t i = 0; i < _ranked.size(); ++i)
            _csv.Decision(_round, _ranked[i], "decision", i < _cut ? "advance" : "eliminate");
        _ranked.resize(_cut);
        if (_ranked.size() < 2)
        {
            StartFinal(world, console, pipeline);
            return;
        }

        for (size_t i = 0; i < _extraProbes.size(); ++i)
        {
            auto& probe = _extraProbes[i];
            const auto legs = _ranked.size() + (Comparable(probe.Reference) ? 0 : 1);
            if (_usedExpansions[i] || !probe.Restore || !Fits(legs * ProbeCost(probe))) continue;
            _usedExpansions[i] = true;
            _roundProbe = &probe;
            _stage = Stage::Elimination;
            ++_round;
            _roundMasks = _ranked;
            _roundIndex = 0;
            if (Comparable(probe.Reference))
                _csv.Decision(_round, 0, "matched_control_mean_ms", Csv::Number(probe.Reference.Mean));
            for (auto mask : _ranked) _csv.Decision(_round, mask, "entered", Csv::Number(*_scores[mask]));
            StartRoundLeg(world, console, pipeline);
            return;
        }
        _csv.Decision(_round, _ranked.front(), "expansion", "skipped: budget or no comparable new window");
        StartFinal(world, console, pipeline);
    }

    void CompleteRoundLeg(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, Stats stats)
    {
        auto& probe = *_roundProbe;
        const double cost = ProbeCost(probe);
        _probeScale = std::max(_probeScale, SecondsSince(_probeStarted) / cost);
        const bool recheck = _stage == Stage::Tiebreak;
        const bool enoughData = Comparable(stats);
        if (_roundNeedsControl)
        {
            if (enoughData)
            {
                probe.Reference = stats;
                probe.Groups = _legGroups;
                _csv.Decision(_round, 0, "matched_control_mean_ms", Csv::Number(stats.Mean));
                _csv.Decision(_round, 0, "matched_control_activity_mask", std::to_string(probe.Groups));
                if (recheck)
                {
                    const unsigned missing = (_roundMasks[0] ^ _roundMasks[1]) & ~_coveredGroups;
                    if (missing && !(probe.Groups & missing))
                    {
                        _csv.Decision(_round, 0, "recheck", "control did not show the distinguishing activity");
                        if (!TryBoundaryCheck(world, console, pipeline, missing)) AdvanceRound(world, console, pipeline);
                        return;
                    }
                }
            }
        }
        else if (enoughData)
            _roundGains[_mask] = 1 - stats.Mean / probe.Reference.Mean;
        if (!_roundNeedsControl) ++_roundIndex;
        if (enoughData && _roundIndex < _roundMasks.size() && Fits((_roundMasks.size() - _roundIndex) * cost))
        {
            StartRoundLeg(world, console, pipeline);
            return;
        }

        if (!enoughData || _roundIndex < _roundMasks.size())
        {
            _csv.Decision(_round, _mask, recheck ? "recheck" : "expansion",
                enoughData ? "incomplete: budget; prior scores kept" : "incomplete: insufficient frames; prior scores kept");
            if (recheck) AdvanceRound(world, console, pipeline);
            else StartFinal(world, console, pipeline);
            return;
        }

        if (recheck)
        {
            const auto updated = [&](unsigned mask)
            {
                auto rating = _ratings[mask];
                rating.Add(probe.Phase, _roundGains[mask]);
                const auto score = Score(rating);
                _csv.Decision(_round, mask, "recheck_gain", Csv::Number(score));
                return score;
            };
            const auto passing = _roundMasks[0];
            const auto dropping = _roundMasks[1];
            const auto a = updated(passing);
            const auto b = updated(dropping);
            const bool tied = std::abs(b - a) <= _observedMeanNoise;
            const bool swap = tied ? dropping < passing : b > a;
            if (swap) std::swap(_ranked[_cut - 1], _ranked[_cut]);
            if (_cut == 1) _scores[_ranked.front()] = swap ? b : a;
            _csv.Decision(_round, _ranked[_cut - 1], "recheck",
                tied ? "advanced by mask order after tied recheck" : "advanced after paired recheck");
            AdvanceRound(world, console, pipeline);
            return;
        }

        for (auto mask : _roundMasks)
        {
            _ratings[mask].Add(probe.Phase, _roundGains[mask]);
            _scores[mask] = Score(_ratings[mask]);
        }
        _coveredGroups |= probe.Groups;
        Rank(_ranked);
        CutRound(world, console, pipeline);
    }

    void CompleteLeg(World& world, ConsoleModule& console, RenderModule& render, BistroRenderPipeline& pipeline)
    {
        if (!BistroBenchmarkData::CoversWindow(_frames, _window.Begin, _window.End))
        {
            Finish(world, console, pipeline, "incomplete: route coverage");
            return;
        }
        const auto stats = BistroBenchmarkData::Measure(_frames, _window.Begin, _window.End);
        if (_stage == Stage::Run)
        {
            const bool changed = _configured &&
                (EffectiveConfig(render) != _effective || RenderConfig(console) != _searchConfig);
            Finish(world, console, pipeline, changed ? "render settings changed" : "completed");
            return;
        }
        const auto placement = Placement(_mask);
        const bool placementChanged = std::any_of(placement.begin(), placement.end(), [&](const auto& entry)
        {
            return console.FindConsoleVariable(entry.first).GetBool() != (entry.second == "true");
        });
        if (placementChanged || EffectiveConfig(render) != _effective || SearchConfig(console) != _searchConfig)
        {
            Finish(world, console, pipeline, "render settings changed",
                TimedSearch() ? std::optional<unsigned>{0} : std::nullopt);
            return;
        }
        WriteLeg("completed");
        if (_stage == Stage::Exhaustive)
        {
            if (_mask == 0) _baseline = stats;
            if (_mask == 0 || std::tie(stats.Mean, stats.P95) < std::tie(_sweepBest.Mean, _sweepBest.P95))
            {
                _sweepBest = stats;
                _sweepBestMask = _mask;
            }
            if (_mask + 1 < Cases)
            {
                Apply(console, Placement(++_mask));
                BeginLeg(world, pipeline, {0, Duration(), 0});
                return;
            }
            const double gain = 1 - _sweepBest.Mean / _baseline.Mean;
            Report("BaselineMeanMs", _baseline.Mean);
            Report("BaselineP95Ms", _baseline.P95);
            Report("FastestMask", _sweepBestMask);
            Report("FastestMeanMs", _sweepBest.Mean);
            Report("FastestP95Ms", _sweepBest.P95);
            Report("MeanGain", gain);
            Finish(world, console, pipeline, "full sweep completed", _sweepBestMask, gain);
            return;
        }
        if (_stage == Stage::Baseline)
        {
            _baseline = stats;
            ReportBaselineWindows(_windows);
            ReportBaselineWindows(_extraWindows);
            PrepareEliminationWindows();
            _finalReserve = SecondsSince(_started) + FinishReserve;
            _activeGroups = 0;
            for (size_t i = 0; i < _activity.size(); ++i)
            {
                _activeGroups |= _activity[i];
                Report("ActiveMask.Phase" + std::to_string(i), _activity[i]);
                Report("PhaseEndSeconds." + std::to_string(i), _phaseEnds[i]);
            }
            if (!Comparable(stats) || !_activeGroups)
            {
                Finish(world, console, pipeline, "no usable baseline or active groups", 0);
                return;
            }
            if (!Fits(2 * NominalCost(_windows)))
            {
                Finish(world, console, pipeline, "no time for comparable windows and finalist", 0);
                return;
            }
            _stage = Stage::Control;
            _repeatComparable = false;
            _pending.push_back(0);
            NextProbe(world, console, pipeline);
            return;
        }
        if (_stage == Stage::Elimination || _stage == Stage::Tiebreak)
        {
            CompleteRoundLeg(world, console, pipeline, stats);
            return;
        }
        if (_stage == Stage::Final)
        {
            const double gain = 1 - stats.Mean / _baseline.Mean;
            const double p95Change = stats.P95 / _baseline.P95 - 1;
            Report("BaselineMeanMs", _baseline.Mean);
            Report("BaselineP95Ms", _baseline.P95);
            Report("FinalistMask", _mask);
            Report("FinalistMeanMs", stats.Mean);
            Report("FinalistP95Ms", stats.P95);
            Report("MeanGain", gain);
            Report("P95Change", p95Change);
            const bool win = Comparable(stats) && gain > _meanThreshold && p95Change <= _p95Threshold;
            Finish(world, console, pipeline, !Comparable(stats) ? "finalist data insufficient" :
                win ? "winner applied" : gain <= _meanThreshold ? "mean gain below threshold" : "p95 above tolerance",
                win ? _mask : 0, gain);
            return;
        }

        const auto& windows = _windows;
        if (_stage == Stage::Control) _coveredGroups |= _legGroups;
        _measurements[_windowIndex++] = stats;
        const double used = NominalCost(windows) - NominalCost(windows, _windowIndex);
        _probeScale = std::max(_probeScale, SecondsSince(_probeStarted) / used);
        if (_windowIndex < windows.size())
        {
            if (Fits(NominalCost(windows, _windowIndex))) BeginLeg(world, pipeline, windows[_windowIndex]);
            else NextProbe(world, console, pipeline);
            return;
        }
        if (_stage == Stage::Control)
        {
            _reference = _measurements;
            for (size_t i = 0; i < _primaryProbes.size(); ++i)
                _primaryProbes[i].Reference = _reference[i];
        }
        else if (_stage == Stage::Repeat)
        {
            _repeatComparable = true;
            for (size_t i = 0; i < windows.size(); ++i)
            {
                const auto& first = _reference[i];
                const auto& second = _measurements[i];
                if (!Comparable(first) || !Comparable(second))
                {
                    _repeatComparable = false;
                    break;
                }
                _observedMeanNoise = std::max(_observedMeanNoise, std::abs(second.Mean / first.Mean - 1));
                _meanThreshold = std::max(_meanThreshold, _observedMeanNoise);
                _p95Threshold = std::max(_p95Threshold, std::abs(second.P95 / first.P95 - 1));
            }
        }
        else
        {
            Rating rating;
            for (size_t i = 0; i < windows.size(); ++i)
            {
                if (!Comparable(_reference[i]) || !Comparable(_measurements[i]))
                {
                    NextProbe(world, console, pipeline);
                    return;
                }
                rating.Add(i, 1 - _measurements[i].Mean / _reference[i].Mean);
            }
            _ratings[_mask] = rating;
            _scores[_mask] = Score(rating);
            Report(std::string(StageName()) + ".RelativeMeanGain.Mask" + std::to_string(_mask), *_scores[_mask]);
        }
        NextProbe(world, console, pipeline);
    }

    void RestoreSettings(ConsoleModule& console, std::optional<unsigned> selected)
    {
        if (_previous.empty()) return;
        auto restore = _previous;
        if (selected)
            for (const auto& entry : EnabledPlacement(*selected))
                for (auto& saved : restore)
                    if (saved.first == entry.first) saved.second = entry.second;
        Apply(console, restore);
    }

    void Finish(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, const char* status,
        std::optional<unsigned> selected = {}, double gain = 0)
    {
        std::string error;
        try
        {
            if (!_configured)
                for (const auto& [name, value] : RenderConfig(console))
                    _csv.Config(std::filesystem::path(name).u8string(), value);
            WriteLeg(status);
            if (TimedSearch())
            {
                Report("MeanGainThreshold", _meanThreshold);
                Report("ObservedMeanNoise", _observedMeanNoise);
                Report("P95Tolerance", _p95Threshold);
            }
            _csv.Flush();
        }
        catch (const std::exception& failure) { error = failure.what(); }
        _active = false;
        pipeline.SetBenchmarkTime({});
        auto& ecs = world.GetECS();
        auto& camera = ecs.Get<TransformComponent>(_camera);
        camera.Location = _previousPose.Position;
        camera.Rotation = _previousPose.Forward;
        camera.DirtyFlag = true;
        ecs.Get<CameraComponent>(_camera).InvalidateHistory();
        if (_hadInfluence) ecs.Get<BistroInteractionSource>(_camera) = _previousInfluence;
        else ecs.Remove<BistroInteractionSource>(_camera);
        AnimateDoor(ecs, 0);
        if (_initialScene)
        {
            try { _initialScene(); }
            catch (const std::exception& failure) { error = failure.what(); }
            _initialScene = {};
        }
        _windowPhysics = {};
        _extraPhysics = {};
        _primaryProbes = {};
        _extraProbes = {};
        _activityProbes = {};
        _checkpoints.clear();
        _roundProbe = nullptr;
        if (!error.empty())
        {
            selected.reset();
            status = "error";
        }
        const auto defaultMask = _startupDefault.value_or(0);
        bool timedWinner = TimedSearch() && selected.value_or(0) != 0;
        if (TimedSearch() && selected && !timedWinner) selected = defaultMask;
        if (timedWinner && Remaining() <= 1)
        {
            selected = defaultMask;
            timedWinner = false;
            status = "deadline";
        }
        try
        {
            RestoreSettings(console, selected);
            if (timedWinner && Remaining() <= 0)
            {
                selected = defaultMask;
                timedWinner = false;
                RestoreSettings(console, selected);
                status = "deadline";
            }
            if (TimedSearch()) _csv.Result(selected.value_or(_userMask), SecondsSince(_started), status);
            _csv.Close();
            if (timedWinner && Remaining() <= 0)
            {
                selected = defaultMask;
                timedWinner = false;
                RestoreSettings(console, selected);
                status = "deadline while closing CSV";
                _csv.AppendResult(_path, *selected, SecondsSince(_started), status);
            }
        }
        catch (const std::exception& failure)
        {
            error = failure.what();
            selected.reset();
            RestoreSettings(console, {});
            _csv.Close();
        }

        if (_stage == Stage::Exhaustive)
        {
            if (selected)
            {
                const auto path = DefaultsPath();
                const auto result = console.SavePatchIni(path, EnabledPlacement(*selected));
                if (result.Error.empty())
                {
                    _startupDefault = *selected;
                    status = "full sweep defaults saved";
                    Log::Info("BistroBenchmark", "Full sweep ", Cases, "/", Cases, "; mask=", *selected,
                        "; ", PlacementName(*selected), "; mean=", _sweepBest.Mean, " ms; p95=", _sweepBest.P95,
                        " ms; baseline mean=", _baseline.Mean, " ms; baseline p95=", _baseline.P95,
                        " ms; mean gain=", 100 * gain, "%; defaults: ", path.u8string());
                }
                else
                {
                    error = "Cannot save queue defaults: " + result.Error;
                    selected.reset();
                    RestoreSettings(console, {});
                    status = "defaults write failed";
                }
            }
            try { _csv.AppendResult(_path, selected.value_or(_userMask), SecondsSince(_started), status); }
            catch (const std::exception& failure) { if (error.empty()) error = failure.what(); }
        }

        _message = status;
        if (selected)
        {
            char measured[64]{};
            if (timedWinner || _stage == Stage::Exhaustive)
                std::snprintf(measured, sizeof(measured), " | %.1fs | %.1f%% lower mean", SecondsSince(_started), 100 * gain);
            else
                std::snprintf(measured, sizeof(measured), " | %.1fs", SecondsSince(_started));
            const auto layout = timedWinner || _stage == Stage::Exhaustive ? PlacementName(*selected) :
                _startupDefault ? "Startup defaults kept: " + PlacementName(*selected) : "Default kept";
            _message = layout + " (" + status + ")" + measured;
        }
        if (!error.empty())
        {
            _message = error;
            Log::Error("BistroBenchmark", _message, " | CSV: ", _path.u8string());
        }
        else _savedPath = _path.u8string();
        Log::Info("BistroBenchmark", _message, " | CSV: ", _savedPath);
    }

    void Fail(World& world, ConsoleModule& console, BistroRenderPipeline& pipeline, std::string error)
    {
        try
        {
            if (_active) Finish(world, console, pipeline, "error");
            else if (_csv.IsOpen()) _csv.Close();
        }
        catch (const std::exception& cleanup)
        {
            error += " | ";
            error += cleanup.what();
        }
        _message = std::move(error);
        Log::Error("BistroBenchmark", _message);
    }

    Shot _red{BistroBenchmarkRoute::Red}, _blue{BistroBenchmarkRoute::Blue};
    Shot _green{BistroBenchmarkRoute::Green}, _yellow{BistroBenchmarkRoute::Yellow};
    std::array<Entity, BistroEntrance::Names.size()> _doors{InvalidEntity, InvalidEntity};
    Entity _fountain = InvalidEntity, _camera = InvalidEntity;
    Pose _previousPose{};
    BistroInteractionSource _previousInfluence{};
    ConsoleVariablePatch _previous, _searchConfig;
    Config _effective;
    std::function<void()> _initialScene;
    PhysicsStates _windowPhysics, _extraPhysics;
    BistroCharacters::Scene* _characters = nullptr;
    ImFont* _font = nullptr;
    Action _action = Action::None;
    Stage _stage = Stage::Run;
    bool _active = false, _search = false, _legDone = false, _legOpen = false;
    bool _configured = false, _legConfigured = false, _hadInfluence = false, _repeatComparable = false;
    unsigned _mask = 0, _userMask = 0, _activeGroups = 0, _legGroups = 0, _runId = 0;
    size_t _windowIndex = 0;
    double _elapsed = 0, _routeTime = 0, _preroll = 0, _finalReserve = 0, _probeScale = 1;
    double _meanThreshold = .01, _p95Threshold = .02;
    Clock::time_point _requestedAt, _started, _legStarted, _frameStart, _lastPresent, _probeStarted;
    std::filesystem::path _path;
    Csv _csv;
    Window _window{};
    Windows _windows{}, _extraWindows{};
    std::array<double, 6> _sections{};
    std::array<double, 4> _phaseEnds{};
    std::array<unsigned, 4> _activity{};
    Stats _baseline{};
    Stats _sweepBest{};
    unsigned _sweepBestMask = 0;
    std::optional<unsigned> _startupDefault;
    Measurements _reference{}, _measurements{};
    std::bitset<Cases> _seen;
    std::array<std::optional<double>, Cases> _scores;
    std::array<Rating, Cases> _ratings;
    std::array<double, Cases> _roundGains;
    std::array<Probe, 4> _primaryProbes, _extraProbes;
    std::array<Probe, Techniques.size()> _activityProbes;
    std::bitset<4> _usedExpansions;
    std::deque<Checkpoint> _checkpoints;
    Probe* _roundProbe = nullptr;
    bool _roundNeedsControl = false;
    std::vector<unsigned> _ranked, _roundMasks;
    unsigned _round = 0, _coveredGroups = 0;
    size_t _cut = 0, _roundIndex = 0;
    double _observedMeanNoise = 0;
    std::deque<unsigned> _pending;
    std::vector<BistroBenchmarkData::Frame> _frames;
    std::string _message, _savedPath;
};
