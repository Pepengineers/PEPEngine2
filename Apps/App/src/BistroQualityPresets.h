#pragma once

#include "BistroQualitySearch.h"
#include "Common/ConsoleVariables.h"
#include <array>
#include <iterator>

namespace BistroQuality
{
    inline constexpr int SchemaVersion = 1;
    inline constexpr const char* TierNames[] = {"Off", "Low", "Medium", "High", "Epic"};

    inline AutoConsoleVariable<int> OutputWidth(L"App.Quality.OutputWidth", 0,
        L"Saved Bistro client width. 0 uses the startup size.", EConsoleVariableFlags::Default,
        [](int value) { return value >= 0 && value <= 16384; });
    inline AutoConsoleVariable<int> OutputHeight(L"App.Quality.OutputHeight", 0,
        L"Saved Bistro client height. 0 uses the startup size.", EConsoleVariableFlags::Default,
        [](int value) { return value >= 0 && value <= 16384; });

    using BistroQualitySearch::Axis;
    using BistroQualitySearch::Candidate;
    using BistroQualitySearch::OffGroup;
    enum class ApplyMode { Live, RecreateTechnique, RestartWorld, RestartProcess };

    struct Field
    {
        const wchar_t* Key;
        std::array<const char*, 5> Values;
    };

    constexpr Field Constant(const wchar_t* key, const char* value)
    {
        return {key, {value, value, value, value, value}};
    }
    constexpr Field Enable(const wchar_t* key)
    {
        return {key, {"false", "true", "true", "true", "true"}};
    }
    constexpr Field Quality(const wchar_t* key)
    {
        return {key, {"0", "1", "2", "3", "4"}};
    }

    inline constexpr Field AO[] = {
        Enable(L"Render.AO.Enabled"), Quality(L"Render.AO.Quality"),
        Constant(L"Render.AO.SSAO.Samples", "-1"), Constant(L"Render.AO.HBAO.Directions", "-1"),
        Constant(L"Render.AO.HBAO.Steps", "-1"), Constant(L"Render.AO.XeGTAO.Slices", "-1"),
        Constant(L"Render.AO.XeGTAO.Steps", "-1"), Constant(L"Render.AO.DenoisePasses", "-1"),
        {L"Render.AO.CACAO.Quality", {"-1", "0", "1", "3", "4"}}
    };
    inline constexpr Field SSR[] = {
        Enable(L"Render.SSR.Enabled"), Quality(L"Render.SSR.Quality"),
        Constant(L"Render.SSR.MaxIntersections", "-1"), Constant(L"Render.SSR.MinOccupancy", "-1"),
        Constant(L"Render.SSR.MostDetailedMip", "-1"), Constant(L"Render.SSR.SamplesPerQuad", "-1"),
        Constant(L"Render.SSR.RoughnessCutoff", "-1"), Constant(L"Render.SSR.TemporalStability", "-1"),
        Constant(L"Render.SSR.VarianceGuided", "true"), Constant(L"Render.SSR.VarianceThreshold", "0.05")
    };
    inline constexpr Field DoF[] = {
        Enable(L"Render.DOF.Enabled"), Quality(L"Render.DOF.Quality"),
        Constant(L"Render.DOF.Rings", "0"), Constant(L"Render.DOF.FixedCocLimit", "true"),
        Constant(L"Render.DOF.RingMerge", "true")
    };
    inline constexpr Field Blur[] = {
        Enable(L"Render.MotionBlur.Enabled"), Quality(L"Render.MotionBlur.Quality"),
        Constant(L"Render.MotionBlur.Samples", "0")
    };
    inline constexpr Field Sky[] = {
        Enable(L"Render.SkyBox.Enabled"), Quality(L"Render.SkyBox.Quality"),
        Constant(L"Render.SkyBox.PresetIBLBudgets", "true")
    };
    inline constexpr Field Reflections[] = {
        Enable(L"Render.Reflection.Enabled"), Quality(L"Render.Reflection.Quality"),
        {L"App.ReflectionProbes.Resolution", {"128", "64", "128", "256", "512"}},
        {L"App.ReflectionProbes.PrefilterSamples", {"64", "32", "64", "128", "256"}}
    };
    inline constexpr Field Grass[] = {
        Enable(L"Render.Grass.Enabled"), Quality(L"Render.Grass.Quality"),
        {L"App.Grass.MaxBlades", {"15000", "15000", "30000", "45000", "60000"}},
        {L"App.Grass.Segments", {"1", "1", "2", "3", "4"}},
        Constant(L"App.Grass.DensityPerSquareMetre", "1500")
    };
    inline constexpr Field Wind[] = {
        Enable(L"Render.Wind.Enabled"), Quality(L"Render.Wind.Quality"), Enable(L"App.Grass.Wind.Enabled"),
        {L"App.Grass.Wind.GridResolution", {"32", "32", "64", "128", "256"}},
        {L"App.Grass.Wind.PressureIterations", {"8", "8", "20", "40", "80"}}
    };
    inline constexpr Field Water[] = {
        Enable(L"Render.Fluid.Enabled"), Quality(L"Render.Fluid.Quality"),
        {L"App.Water.PresetParticleCount", {"25000", "25000", "50000", "75000", "100000"}},
        {L"App.Water.FilterRadiusPixels", {"3", "3", "4", "5", "6"}}
    };
    inline constexpr Field Particles[] = {
        Enable(L"Render.Particles.Enabled"), Quality(L"Render.Particles.Quality"),
        Enable(L"Render.Particles.DrawEnabled"), Enable(L"Render.Particles.SimulationEnabled"),
        {L"App.Particles.Count", {"0", "125000", "250000", "375000", "500000"}},
        {L"App.Particles.EdgeSamples", {"1", "1", "2", "3", "4"}},
        {L"App.Particles.MinPixelSize", {"0", "1.5", "0.75", "0.375", "0"}}
    };
    inline constexpr Field Characters[] = {
        {L"App.NPC.PresetCount", {"0", "2", "4", "7", "10"}},
        {L"App.Fox.PresetCount", {"0", "1", "2", "4", "5"}}
    };
    inline constexpr Field Skinning[] = {
        Enable(L"Render.Skinning.Enabled"), Quality(L"Render.Skinning.Quality"),
        {L"Render.Skinning.PoseRateHz", {"0", "15", "20", "30", "60"}}
    };
    inline constexpr Field Props[] = {
        Enable(L"App.Physics.SimulationEnabled"), Constant(L"App.Physics.CountScale", "1"),
        {L"App.Physics.ActiveTablewareFraction", {"0", "0.25", "0.5", "0.75", "1"}}
    };
    inline constexpr Field Sun[] = {{L"Render.Shadows.Sun.Resolution", {"0", "512", "1024", "2048", "4096"}}};
    inline constexpr Field Local[] = {{L"Render.Shadows.Local.Resolution", {"0", "128", "256", "384", "512"}}};
    inline constexpr Field Anisotropy[] = {{L"Render.Textures.Anisotropy", {"2", "2", "4", "8", "16"}}};

    struct Technique
    {
        const char* Name;
        Axis Search;
        ApplyMode Mode;
        const Field* Fields;
        size_t FieldCount;
        const wchar_t* OwnerCount;
        const char* Prerequisite;
        bool HasOff;

        template<size_t N>
        constexpr Technique(const char* name, OffGroup group, ApplyMode mode, const Field (&fields)[N],
            bool cpu = false, const wchar_t* count = nullptr, const char* prerequisite = nullptr, bool manualOff = false)
            : Name(name), Search{group, cpu}, Mode(mode), Fields(fields), FieldCount(N),
              OwnerCount(count), Prerequisite(prerequisite), HasOff(group != OffGroup::Never || manualOff) {}
    };

    inline constexpr Technique Techniques[] = {
        {"Ambient occlusion", OffGroup::Post, ApplyMode::Live, AO},
        {"Screen-space reflections", OffGroup::Post, ApplyMode::RecreateTechnique, SSR},
        {"Depth of field", OffGroup::Post, ApplyMode::RecreateTechnique, DoF},
        {"Motion blur", OffGroup::Post, ApplyMode::Live, Blur},
        {"Skybox IBL", OffGroup::Never, ApplyMode::RecreateTechnique, Sky},
        {"Reflection probes", OffGroup::Never, ApplyMode::RecreateTechnique, Reflections,
            false, L"App.ReflectionProbes.Count", nullptr, true},
        {"Grass", OffGroup::Simulation, ApplyMode::RecreateTechnique, Grass, false, L"App.Grass.Count"},
        {"Wind", OffGroup::Simulation, ApplyMode::RecreateTechnique, Wind, false, L"App.Grass.Count", "Grass"},
        {"Water", OffGroup::Simulation, ApplyMode::RestartWorld, Water, false, L"App.Water.Count"},
        {"Particles", OffGroup::Simulation, ApplyMode::RecreateTechnique, Particles},
        {"Physics props", OffGroup::Simulation, ApplyMode::RestartWorld, Props, true},
        {"Characters", OffGroup::Characters, ApplyMode::RestartWorld, Characters, true},
        {"Skinning", OffGroup::Characters, ApplyMode::Live, Skinning, true, nullptr, "Characters"},
        {"Sun shadows", OffGroup::Shadows, ApplyMode::RecreateTechnique, Sun},
        {"Local shadows", OffGroup::Shadows, ApplyMode::RecreateTechnique, Local},
        {"Texture anisotropy", OffGroup::Never, ApplyMode::RestartProcess, Anisotropy}
    };

    inline ConsoleVariablePatch Preset(const Technique& technique, int tier)
    {
        if (tier < 0 || tier > 4 || (tier == 0 && !technique.HasOff))
            throw std::invalid_argument("Bistro technique does not support this quality level");
        ConsoleVariablePatch patch;
        patch.reserve(technique.FieldCount);
        for (size_t i = 0; i < technique.FieldCount; ++i)
            patch.emplace_back(technique.Fields[i].Key, technique.Fields[i].Values[tier]);
        return patch;
    }

    inline const char* CurrentTier(const Technique& technique, const ConsoleModule& console)
    {
        std::array<bool, 5> matches{technique.HasOff, true, true, true, true};
        for (size_t i = 0; i < technique.FieldCount; ++i)
        {
            const auto& field = technique.Fields[i];
            const auto effective = console.FindConsoleVariable(field.Key).GetString();
            for (size_t tier = 0; tier < matches.size(); ++tier) matches[tier] &= effective == field.Values[tier];
        }
        for (size_t tier = 0; tier < matches.size(); ++tier)
            if (matches[tier]) return TierNames[tier];
        return "Custom";
    }

    inline std::vector<const Technique*> SearchTechniques(const ConsoleModule& console)
    {
        std::vector<const Technique*> selected;
        for (const auto& technique : Techniques)
            if (technique.Mode != ApplyMode::RestartProcess &&
                (!technique.OwnerCount || console.FindConsoleVariable(technique.OwnerCount).GetInt() > 0))
                selected.push_back(&technique);
        return selected;
    }

    inline std::vector<Axis> SearchAxes(const std::vector<const Technique*>& techniques)
    {
        std::vector<Axis> axes;
        for (const auto* technique : techniques)
        {
            auto axis = technique->Search;
            if (technique->Prerequisite)
                for (size_t i = 0; i < techniques.size(); ++i)
                    if (std::string_view(techniques[i]->Name) == technique->Prerequisite) axis.Prerequisite = i;
            axes.push_back(axis);
        }
        return axes;
    }

    inline ConsoleVariablePatch ProfilePatch(const std::vector<const Technique*>& techniques, const Candidate& candidate)
    {
        if (candidate.Tiers.size() != techniques.size()) throw std::invalid_argument("Invalid Bistro quality candidate");
        ConsoleVariablePatch patch{
            {L"Render.Upscaler.Provider", "0"}, {L"Render.FrameGeneration.Provider", "0"},
            {L"App.Quality.OutputWidth", std::to_string(candidate.Width)},
            {L"App.Quality.OutputHeight", std::to_string(candidate.Height)}
        };
        for (size_t i = 0; i < techniques.size(); ++i)
        {
            auto fields = Preset(*techniques[i], candidate.Tiers[i]);
            patch.insert(patch.end(), std::make_move_iterator(fields.begin()), std::make_move_iterator(fields.end()));
        }
        return patch;
    }
}
