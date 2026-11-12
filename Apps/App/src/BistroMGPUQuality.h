#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <array>
#include <vector>
#include <string>

namespace BistroQualitySearch
{
    enum class Profile { Performance, Quality };

    struct RouteSettings
    {
        bool Secondary = false;
        bool Exact = false;
        int UpdateHz = 0;
        auto Key() const { return std::tie(Secondary, Exact, UpdateHz); }
        bool operator==(const RouteSettings& other) const { return Key() == other.Key(); }
        bool operator<(const RouteSettings& other) const { return Key() < other.Key(); }
    };

    struct ScheduledRoute
    {
        bool Secondary = false, Exact = true;
        int Interval = 1;
        auto Key() const { return std::tie(Secondary, Exact, Interval); }
        bool operator==(const ScheduledRoute& other) const { return Key() == other.Key(); }
        bool operator<(const ScheduledRoute& other) const { return Key() < other.Key(); }
    };

    struct LocalShadowRoute
    {
        std::uint32_t Entity = 0;
        ScheduledRoute Route;
        auto Key() const { return std::tie(Entity, Route); }
        bool operator==(const LocalShadowRoute& other) const { return Key() == other.Key(); }
        bool operator<(const LocalShadowRoute& other) const { return Key() < other.Key(); }
    };

    struct ReflectionSearchSettings
    {
        int Count = 4, Resolution = 256, FilterSamples = 64;
        int DistanceMetres = -1, Schedule = 0, Implementation = 0, Placement = 0;
        std::array<unsigned, 4> SecondaryFaces{};
        auto Key() const { return std::tie(Count, Resolution, FilterSamples, DistanceMetres, Schedule,
            Implementation, Placement, SecondaryFaces); }
        void Canonicalize()
        {
            if (!Count) { *this = {}; Count = 0; return; }
            for (size_t i = static_cast<size_t>(Count); i < SecondaryFaces.size(); ++i) SecondaryFaces[i] = 0;
            if (Count == 1) { if (Schedule == 1) Schedule = 0; if (Schedule == 3) Schedule = 2; }
            if (DistanceMetres == -1)
            {
                Schedule = 0;
                if (Placement) for (int i = 0; i < Count; ++i) SecondaryFaces[i] = Placement == 2 ? 63u : 0u;
                Placement = 0;
            }
            if (Placement) SecondaryFaces = {};
        }
        auto Quality() const { return std::make_tuple(Count, Resolution, FilterSamples,
            DistanceMetres < 0 ? 100000 : DistanceMetres, -Schedule); }
        bool operator==(const ReflectionSearchSettings& other) const { return Key() == other.Key(); }
        bool operator<(const ReflectionSearchSettings& other) const { return Key() < other.Key(); }
    };

    struct MGPUSettings
    {
        bool Enabled = false;
        std::uint64_t Primary = 0, Secondary = 0;
        RouteSettings Water, Grass;
        ScheduledRoute Sun, Particles;
        std::vector<LocalShadowRoute> LocalShadows;
        ReflectionSearchSettings Reflections;
        auto Key() const { return std::tie(Enabled, Primary, Secondary, Water, Grass, Sun, Particles,
            LocalShadows, Reflections); }
        bool operator==(const MGPUSettings& other) const { return Key() == other.Key(); }
        bool operator<(const MGPUSettings& other) const { return Key() < other.Key(); }
    };

    inline double FreshnessBudgetMs(Profile profile)
    {
        return 2000.0 / (profile == Profile::Performance ? 60.0 : 30.0);
    }

    struct TechniqueMeasurement
    {
        bool Active = false, RequestedSecondary = false, EffectiveSecondary = false, Ready = false;
        double SourceAgeMs = 0, SimulationDebtSeconds = 0, MissingSeconds = 0;
        std::uint64_t SourceSequence = 0;
        std::string Name;
        double FullSourceAgeMs = 0, WaitMs = 0;
        std::uint64_t ExportBytes = 0, ImportBytes = 0;
    };

    struct FreshnessMeasurements
    {
        std::uint64_t Samples = 0, FallbackFrames = 0, MissingFrames = 0, OverAgeFrames = 0;
        double MaxSourceAgeMs = 0, MaxDebtSeconds = 0;
        double FirstDebtFloor = std::numeric_limits<double>::infinity();
        double LastDebtFloor = std::numeric_limits<double>::infinity();
        double FirstWindow = -1, LastWindow = -1;
        double Windows = 0, SumX = 0, SumY = 0, SumXX = 0, SumXY = 0, SumYY = 0;

        void RecordWindow(double x, double y)
        {
            ++Windows;
            SumX += x; SumY += y; SumXX += x * x; SumXY += x * y; SumYY += y * y;
        }

        void Observe(const TechniqueMeasurement& value, double elapsedSeconds, Profile profile)
        {
            if (!value.Active) return;
            ++Samples;
            FallbackFrames += value.RequestedSecondary != value.EffectiveSecondary;
            MissingFrames += !value.Ready || value.MissingSeconds > 0;
            const auto budget = FreshnessBudgetMs(profile);
            const bool finite = std::isfinite(value.SourceAgeMs) && std::isfinite(value.SimulationDebtSeconds) &&
                std::isfinite(value.MissingSeconds);
            OverAgeFrames += !finite || value.MissingSeconds < 0 || value.SourceAgeMs < 0 || value.SourceAgeMs > budget ||
                value.SimulationDebtSeconds < 0 || value.SimulationDebtSeconds * 1000 > budget;
            if (!finite) return;
            MaxSourceAgeMs = (std::max)(MaxSourceAgeMs, value.SourceAgeMs);
            MaxDebtSeconds = (std::max)(MaxDebtSeconds, value.SimulationDebtSeconds);
            const auto window = std::floor(elapsedSeconds);
            if (FirstWindow < 0) FirstWindow = window;
            if (window == FirstWindow) FirstDebtFloor = (std::min)(FirstDebtFloor, value.SimulationDebtSeconds);
            if (window != LastWindow)
            {
                if (LastWindow >= 0) RecordWindow(LastWindow, LastDebtFloor);
                LastWindow = window; LastDebtFloor = value.SimulationDebtSeconds;
            }
            else LastDebtFloor = (std::min)(LastDebtFloor, value.SimulationDebtSeconds);
        }

        bool Valid(bool required = false) const
        {
            if ((required && !Samples) || FallbackFrames || MissingFrames || OverAgeFrames) return false;
            auto trend = *this;
            if (LastWindow >= 0) trend.RecordWindow(LastWindow, LastDebtFloor);
            if (trend.Windows < 3)
                return LastWindow <= FirstWindow || LastDebtFloor <= FirstDebtFloor + 0.0001;
            const auto xx = trend.SumXX - trend.SumX * trend.SumX / trend.Windows;
            const auto xy = trend.SumXY - trend.SumX * trend.SumY / trend.Windows;
            const auto yy = trend.SumYY - trend.SumY * trend.SumY / trend.Windows;
            const auto slope = xy / xx;
            const auto error = std::sqrt((std::max)(0.0, yy - xy * slope) / (trend.Windows - 2) / xx);
            return slope <= (std::max)(0.000001, 3 * error);
        }
    };
}
