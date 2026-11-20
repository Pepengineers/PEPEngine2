#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Systems/LookAtTargetSystem.h"
#include "App.Base/Systems/SplineFollowSystem.h"
#include "BistroBenchmarkRoute.h"
#include "Components/BistroInteractionSource.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include <imgui/imgui.h>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>

class BistroBenchmark
{
    using Clock = std::chrono::steady_clock;
    using Point = BistroBenchmarkRoute::Point;
    struct Pose { Vector3 Position, Forward; };

    class Shot
    {
    public:
        template<size_t N> Shot(const Point (&points)[N], float speed)
            : _points(points), _count(N)
        {
            for (size_t i = 0; i + 1 < N; ++i)
            {
                const auto delta = points[i+1].Position - points[i].Position;
                const float length = delta.Length();
                assert(length > 0);
                const float duration = length / speed;
                _times.push_back(_times.back() + duration);
                _curve.Points.back().LeaveTangent = Tangent(i) * (length / 3);
                SplinePoint next{};
                next.Position = points[i+1].Position;
                next.ArriveTangent = Tangent(i+1) * (-length / 3);
                _curve.Points.push_back(next);
            }
        }

        float Duration() const { return _times.back(); }
        float At(size_t i) const { return _times.at(i); }

        Pose Sample(float time) const
        {
            time = std::clamp(time, 0.f, Duration());
            const auto next = std::upper_bound(_times.begin(), _times.end(), time);
            const size_t i = std::min(size_t(next - _times.begin() - 1), _count - 2);
            const float t = (time - _times[i]) / (_times[i+1] - _times[i]);
            const auto position = EvaluateSplineSegment(_curve.Points[i], _curve.Points[i+1], t);
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
        Vector3 Tangent(size_t i) const
        {
            auto result = _points[std::min(i+1, _count-1)].Position - _points[i > 0 ? i-1 : i].Position;
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
        std::vector<float> _times{0};
    };

    static constexpr std::array Techniques{
        L"App.Async.AO", L"App.Async.Environment", L"App.Async.LightGrid", L"App.Async.Particles",
        L"App.Async.Water", L"App.Async.CloudPicture"};
    static constexpr unsigned Cases = 1u << Techniques.size();
    static constexpr double Warmup = 3.;
    enum class Action { None, Start, Cancel };

public:
    bool Active() const { return _active; }

    void Stop(World& world, ConsoleModule& console)
    {
        if (_active) Finish(world, console, false);
    }

    void Initialize(World& world)
    {
        world.GetECS().ForEach<NameComponent>([&](Entity entity, const NameComponent& name)
        {
            if (name.value == "BistroEntranceDoor") _door = entity;
            if (name.value == "BistroEntranceFountain") _fountain = entity;
        });
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, MAX_PATH) && ImGui::GetCurrentContext())
        {
            const auto font = std::filesystem::path(windows) / "Fonts/segoeui.ttf";
            if (std::filesystem::is_regular_file(font))
                _font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font.u8string().c_str(), 16.f);
        }
    }

    void Draw()
    {
        ImGui::SetNextWindowPos({14, 14}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Bistro benchmark", nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking))
        {
            if (_font) ImGui::PushFont(_font);
            if (!_active)
            {
                if (ImGui::Button(_font ? u8"\u0411\u0435\u043d\u0447\u043c\u0430\u0440\u043a" : "Benchmark"))
                    _action = Action::Start;
            }
            else
            {
                if (ImGui::Button("Cancel")) _action = Action::Cancel;
                ImGui::Text("Configuration %u / %u", _case + 1, Cases);
                ImGui::TextUnformatted(_elapsed < Warmup ? "Warming up" : "Measuring flight");
                ImGui::ProgressBar(float(std::clamp((_elapsed - Warmup) / Duration(), 0., 1.)));
            }
            if (_font) ImGui::PopFont();
            if (!_message.empty()) ImGui::TextUnformatted(_message.c_str());
        }
        ImGui::End();
    }

    void Update(World& world, ConsoleModule& console)
    {
        const auto action = std::exchange(_action, Action::None);
        try
        {
            if (action == Action::Start && !_active) Start(world, console);
            if (action == Action::Cancel && _active) Finish(world, console, false);
            if (!_active) return;
            if (_nextCase)
            {
                if (_case == Cases) { Finish(world, console, true); return; }
                Apply(console, Placement(_case));
                _nextCase = false;
                _elapsed = 0;
                _caseFrames.clear();
                world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            }
            const float time = float(std::max(0., _elapsed - Warmup));
            const auto pose = Flight(time);
            auto& ecs = world.GetECS();
            auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
            camera.Location = pose.Position;
            camera.Rotation = MakeEulerFromForwardLH(pose.Forward);
            camera.DirtyFlag = true;
            AnimateDoor(ecs, time);
            const auto source = pose.Position - Vector3(0, 95, 0);
            auto& influence = ecs.Get<BistroInteractionSource>(world.ActiveCamera);
            const Vector3 previous(influence.PositionRadius.x, influence.PositionRadius.y, influence.PositionRadius.z);
            const auto start = (source - previous).LengthSquared() > 250000 ? source : previous;
            influence.PositionRadius = {source.x, source.y, source.z, 65};
            influence.PreviousHeight = {start.x, start.y, start.z, 190};
            _frameStart = Clock::now();
        }
        catch (const std::exception& error)
        {
            Fail(world, console, error.what());
        }
    }

    void FrameRendered(World& world, ConsoleModule& console)
    {
        if (!_active) return;
        const double seconds = std::chrono::duration<double>(Clock::now() - _frameStart).count();
        try
        {
            if (_elapsed >= Warmup)
                _caseFrames.push_back({ _elapsed - Warmup, seconds * 1000 });
            _elapsed += seconds;
            if (_elapsed >= Warmup + Duration())
            {
                WriteCase();
                ++_case;
                _nextCase = true;
            }
        }
        catch (const std::exception& error)
        {
            Fail(world, console, error.what());
        }
    }

private:
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
        if (_door == InvalidEntity) return;
        const float yellowTime = time - (_red.Duration() + _blue.Duration() + _green.Duration());
        const auto opening = [&](size_t threshold)
        {
            const auto offset = yellowTime - _yellow.At(threshold);
            return Ease((offset + 1.55f) / 1.15f) * Ease((1.55f - offset) / 1.15f);
        };
        const float angle = -95 * std::max(opening(BistroBenchmarkRoute::EntryDoor), opening(BistroBenchmarkRoute::ExitDoor));
        auto& door = ecs.Get<TransformComponent>(_door);
        if (door.Rotation.y == angle) return;
        const Vector3 hinge{-37.4f, 0, -16.3f};
        door.Location = hinge - Vector3::Transform(hinge, Matrix::CreateRotationY(XMConvertToRadians(angle)));
        door.Rotation.y = angle;
        door.DirtyFlag = true;
        if (_fountain != InvalidEntity)
        {
            auto& fountain = ecs.Get<ParticleEmitterComponent>(_fountain);
            auto settings = fountain.GetSettings();
            settings.CollisionBoxCenter = door.Location + Vector3::Transform({-80.037f, 166.15f, -94.836f},
                Matrix::CreateRotationY(XMConvertToRadians(angle)));
            settings.CollisionBoxYawDegrees = 28.5f + angle;
            fountain.SetSettings(settings);
        }
    }

    static ConsoleVariablePatch Placement(unsigned mask)
    {
        ConsoleVariablePatch patch;
        for (size_t i = 0; i < Techniques.size(); ++i)
            patch.emplace_back(Techniques[i], (mask & (1u << i)) ? "true" : "false");
        return patch;
    }

    static void Apply(ConsoleModule& console, const ConsoleVariablePatch& patch)
    {
        if (const auto result = console.ApplyPatch(patch); !result.Error.empty())
            throw std::runtime_error(result.Error);
    }

    void Start(World& world, ConsoleModule& console)
    {
        if (_door == InvalidEntity)
            throw std::runtime_error("Entrance door mesh was not found");
        auto patch = Placement(0);
        patch.insert(patch.end(), {{L"App.Async.Queues", "true"}, {L"App.CloudPicture.Adapter", "-1"},
            {L"Render.FrameGeneration.Provider", "0"}, {L"App.Sky.SunCycle.Enabled", "false"}});
        _previous.clear();
        for (const auto& entry : patch)
            _previous.emplace_back(entry.first, console.FindConsoleVariable(entry.first).GetString());

        const auto directory = Engine::Core::GetExecutableDirectory() / "BenchmarkResults";
        std::filesystem::create_directories(directory);
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        _path = directory / ("bistro-" + std::to_string(stamp) + ".csv");
        _csv.open(_path);
        _csv.imbue(std::locale::classic());
        _csv << std::fixed << std::setprecision(4)
             << "type,configuration,frame,route_seconds,frame_ms,mean_ms,p95_ms"
                ",AO,Environment,LightGrid,Particles,Water,CloudPicture\n";
        _csv.flush();
        if (!_csv) throw std::runtime_error("Cannot write Bistro benchmark CSV");

        auto& ecs = world.GetECS();
        const auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        _previousPose = {camera.Location, camera.Rotation};
        ecs.Add<BistroInteractionSource>(world.ActiveCamera);
        _active = true;
        Apply(console, patch);
        _message.clear();
        _case = 0;
        _best = 0;
        _mean = _p95 = std::numeric_limits<double>::infinity();
        _nextCase = true;
    }

    void WriteCase()
    {
        WriteFrames();
        std::vector<double> values;
        values.reserve(_caseFrames.size());
        for (const auto& frame : _caseFrames) values.push_back(frame[1]);
        if (values.empty()) throw std::runtime_error("Benchmark has no measured frames");
        const double mean = std::accumulate(values.begin(), values.end(), 0.) / values.size();
        const auto rank = (values.size() * 95 + 99) / 100 - 1;
        auto percentile = values.begin() + rank;
        std::nth_element(values.begin(), percentile, values.end());
        const auto p95 = *percentile;
        _csv << "summary," << _case << ',' << values.size() << ",,," << mean << ',' << p95;
        for (size_t i = 0; i < Techniques.size(); ++i) _csv << ',' << ((_case >> i) & 1u);
        _csv << '\n';
        _csv.flush();
        if (!_csv) throw std::runtime_error("Cannot write Bistro benchmark results");
        if (mean < _mean || (mean == _mean && p95 < _p95))
        {
            _mean = mean;
            _p95 = p95;
            _best = _case;
        }
        _caseFrames.clear();
    }

    void WriteFrames()
    {
        for (size_t i = 0; i < _caseFrames.size(); ++i)
        {
            const auto& frame = _caseFrames[i];
            _csv << "frame," << _case << ',' << i << ',' << frame[0] << ',' << frame[1] << ",,";
            for (size_t tech = 0; tech < Techniques.size(); ++tech) _csv << ',' << ((_case >> tech) & 1u);
            _csv << '\n';
        }
    }

    void Finish(World& world, ConsoleModule& console, bool completed)
    {
        if (completed)
        {
            _csv << "winner," << _best << ",,,," << _mean << ',' << _p95 << '\n';
            _csv.flush();
            if (!_csv) throw std::runtime_error("Cannot write Bistro benchmark winner");
        }
        else if (_csv)
        {
            WriteFrames();
            _csv << "cancelled," << _case << '\n';
        }
        auto& ecs = world.GetECS();
        auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        camera.Location = _previousPose.Position;
        camera.Rotation = _previousPose.Forward;
        camera.DirtyFlag = true;
        ecs.Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
        ecs.Remove<BistroInteractionSource>(world.ActiveCamera);
        AnimateDoor(ecs, 0);
        auto restore = _previous;
        if (completed)
        {
            auto best = Placement(_best);
            best.emplace_back(L"App.Async.Queues", "true");
            for (auto& entry : restore)
                for (const auto& replacement : best)
                    if (entry.first == replacement.first) entry.second = replacement.second;
        }
        Apply(console, restore);
        _active = false;
        _csv.close();
        _message = completed ? "Winner applied: configuration " + std::to_string(_best) : "Benchmark cancelled";
        Log::Info("BistroBenchmark", _message, "; CSV: ", _path.string());
    }

    void Fail(World& world, ConsoleModule& console, std::string error)
    {
        if (_active) Finish(world, console, false);
        if (_csv.is_open()) _csv.close();
        _csv.clear();
        _message = std::move(error);
        Log::Error("BistroBenchmark", _message);
    }

    Shot _red{BistroBenchmarkRoute::Red, 780};
    Shot _blue{BistroBenchmarkRoute::Blue, 590};
    Shot _green{BistroBenchmarkRoute::Green, 760};
    Shot _yellow{BistroBenchmarkRoute::Yellow, 255};
    Entity _door = InvalidEntity;
    Entity _fountain = InvalidEntity;
    Pose _previousPose{};
    ConsoleVariablePatch _previous;
    ImFont* _font = nullptr;
    Action _action = Action::None;
    bool _active = false, _nextCase = false;
    unsigned _case = 0, _best = 0;
    double _elapsed = 0, _mean = 0, _p95 = 0;
    Clock::time_point _frameStart;
    std::filesystem::path _path;
    std::ofstream _csv;
    std::vector<std::array<double, 2>> _caseFrames;
    std::string _message;
};
