#include "BistroComparePanel.h"
#include "BistroReportArchive.h"
#include "BistroReportValues.h"
#include "BistroPlacementSearch.h"
#include "Common/RuntimePaths.h"
#include <imgui/imgui.h>
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

using namespace BistroReport;
namespace fs = std::filesystem;

namespace
{
std::string Decimal(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

void Comparison(std::ostream& html, const char* title, const Settings& left, const Settings& right,
    const char* leftLabel, const char* rightLabel, bool techniques = false)
{
    if (left.empty() && right.empty()) return;
    html << "<details><summary>" << title << "</summary><div class=\"scroll\"><table><thead><tr><th>Setting</th>";
    if (!left.empty()) html << "<th>" << leftLabel << "</th>";
    html << "<th>" << rightLabel << "</th></tr></thead><tbody>";
    std::set<std::string> keys;
    for (const auto& item : left) keys.insert(item.first);
    for (const auto& item : right) keys.insert(item.first);
    for (const auto& key : keys)
    {
        std::string setting = key;
        if (techniques)
            for (const auto& entry : BistroPlacement::Techniques)
                if (key == entry.Pass && entry.Quality) { setting = fs::path(entry.Quality).u8string(); break; }
        const auto cell = [&](const Settings& values) {
            const auto& raw = Value(values, key);
            const auto label = techniques && !values.empty() && raw.empty() ? "Inactive" : DisplayValue(setting, raw);
            html << "<td>" << Escape(label) << "</td>";
        };
        const bool changed = !left.empty() && !right.empty() && Value(left, key) != Value(right, key);
        html << (changed ? "<tr class=\"change\">" : "<tr>") << "<th>" << Escape(techniques ? key : SettingLabel(key)) << "</th>";
        if (!left.empty()) cell(left);
        cell(right);
        html << "</tr>";
    }
    html << "</tbody></table></div></details>";
}
}

std::string BistroComparePanel::ReportBody(const Run& run, const Winner* selected, const Selection& current,
    std::vector<fs::path>& measuredPasses)
{
    std::ostringstream html;
    html.imbue(std::locale::classic());
    html << "<header><div><h1>" << Escape(run.MapName) << "</h1><p class=\"muted\">" << Escape(run.Timestamp)
         << "</p></div><a class=\"action\" href=\"#sources\">Source files &darr;</a></header>";
    const bool both = run.Winners[0].Available() && run.Winners[1].Available();
    if (both)
    {
        html << "<nav aria-label=\"Image mode\"><span>Image mode</span><div class=\"switch\">";
        for (int mode = 0; mode < 2; ++mode)
            html << "<button data-view=\"v" << mode << "\" aria-pressed=\"" << (selected == &run.Winners[mode] ? "true" : "false")
                 << "\">" << (mode ? "Upscaled" : "Native") << "</button>";
        html << "</div></nav>";
    }

    const auto view = [&](const Winner* placement, const Selection& measurement) {
        const Winner* winner = placement && placement->Confirmed() ? placement : nullptr;
        const bool trace = !measurement.Samples.empty() && measurement.Error.empty();
        const auto& settings = winner ? winner->Settings : measurement.Settings;
        const auto& measuredOutput = Value(settings, "Render.Display.OutputResolution");
        const char* mode = placement == &run.Winners[1] ? "Upscaled" : "Native";
        html << "<article id=\"v" << (placement == &run.Winners[1] ? 1 : 0) << "\"" << (placement == selected ? "" : " hidden")
             << "><p class=\"context\">" << Escape(run.DisplayMode);
        if (placement && !both) html << " &nbsp;/&nbsp; " << mode;
        const auto& output = measuredOutput.empty() ? run.Output : measuredOutput;
        if (!output.empty()) html << " &nbsp;/&nbsp; " << Escape(output);
        const auto& outcome = run.FinalStatus.empty() ? run.StopReason : run.FinalStatus;
        if (!outcome.empty()) html << " <span class=\"status\">" << Escape(outcome) << "</span>";
        html << "</p>";
        if (placement && !winner) html << "<p>No confirmed preset for this image mode.</p>";
        if (!run.FinalMask.empty()) html << "<p>Final async queue mask &nbsp; " << Escape(run.FinalMask) << "</p>";
        const auto timing = SummaryTiming(winner, measurement);
        const auto metric = [&](const char* label, double value, const char* unit) {
            html << "<div class=\"metric\"><dt>" << label << "</dt><dd>" << (value > 0 ? Decimal(value) : "&mdash;");
            if (value > 0 && *unit) html << "<small>" << unit << "</small>";
            html << "</dd></div>";
        };
        if (timing.Fps > 0 || timing.P95 > 0)
        {
            html << "<dl class=\"metrics\">";
            metric("Mean frame time", timing.MeanMs(), "ms");
            metric("Average FPS", timing.Fps, "");
            metric("P95 frame time", timing.P95, "ms");
            html << "</dl>";
        }
        if (run.Target() > 0 && timing.Fps > 0)
            html << "<p>Target &nbsp; " << run.Target() << " FPS &nbsp;/&nbsp; <strong>" << run.TargetStatus(timing.Fps, winner) << "</strong></p>";

        if (placement && (!placement->Placements[1].Fields.empty() || !Value(placement->Fields, "Comparison").empty()))
        {
            html << "<h2>Single GPU vs Multi-GPU</h2>";
            if (const auto& gpu = Value(placement->Fields, "Winner"); !gpu.empty())
                html << "<p>Winning placement &nbsp; <strong>" << Escape(gpu) << "</strong></p>";
            if (auto gain = Number(Value(placement->Fields, "SameImageFpsGainPct")))
                html << "<p>Same-image FPS change &nbsp; " << (*gain >= 0 ? "+" : "") << Decimal(*gain) << "%</p>";
            if (const auto& reason = Value(placement->Fields, "Comparison"); !reason.empty()) html << "<p>" << Escape(reason) << "</p>";
            const auto& a = placement->Placements[0];
            const auto& b = placement->Placements[1];
            if (!placement->Improvements.empty())
            {
                html << "<p>Quality gains in the multi-GPU candidate</p><ul>";
                for (const auto& text : placement->Improvements) html << "<li>" << Escape(text) << "</li>";
                html << "</ul>";
            }
            else if (Number(Value(a.Fields, "FPS")) && Number(Value(b.Fields, "FPS")))
                html << "<p class=\"muted\">No technique quality gains recorded.</p>";
            const auto metrics = [](const PlacementResult& result) {
                Settings fields;
                for (const auto& [key, label] : {std::pair{"Status", "Status"}, {"Output", "Output resolution"},
                    {"Render", "Internal resolution"}, {"FPS", "Average FPS"}, {"P95ms", "P95 frame time (ms)"}})
                    if (const auto& raw = Value(result.Fields, key); !raw.empty()) fields[label] = raw;
                if (const auto& raw = Value(result.Fields, "Provider"); !raw.empty())
                    fields["Upscaler"] = DisplayValue("Render.Upscaler.EffectiveProvider", raw);
                return fields;
            };
            Comparison(html, "Placement metrics and configuration", metrics(a), metrics(b), "Single GPU", "Multi-GPU");
            Comparison(html, "Technique quality", a.Techniques, b.Techniques, "Single GPU", "Multi-GPU", true);
            Comparison(html, "GPU placement", a.GpuSettings, b.GpuSettings, "Single GPU", "Multi-GPU");
        }

        html << "<h2>Frame timing</h2>";
        if (trace)
        {
            const auto& pass = _runs[measurement.RunIndex].Passes[measurement.PassIndex];
            measuredPasses.push_back(pass.Path);
            html << "<p class=\"muted\">" << Escape(pass.Label) << " &middot; " << measurement.Samples.size() << " frames</p>"
                 << "<figure data-target=\"" << run.Target() << "\" data-begin=\"" << measurement.Samples.front().RouteSeconds
                 << "\" data-end=\"" << measurement.Samples.back().RouteSeconds << "\" data-points=\"";
            // Keep the same chronological extrema as the in-app plot. All raw samples are in the pass CSV.
            for (size_t i : measurement.Trace)
                html << measurement.Samples[i].RouteSeconds << ',' << measurement.Samples[i].IntervalMs << ' ';
            html << "\"><div class=\"switch\"><button aria-pressed=\"true\">Frame time (ms)</button><button aria-pressed=\"false\">FPS</button></div>"
                 << "<svg viewBox=\"0 0 800 196\" role=\"img\" aria-label=\"Frame timing along the benchmark route\"></svg>"
                 << "<figcaption>Route time (s)";
            if (run.Target() > 0) html << " &middot; Dashed target " << run.Target() << " FPS / " << Decimal(1000. / run.Target()) << " ms";
            html << "</figcaption></figure><p class=\"muted\">Interval measured "
                 << (measurement.AfterRender ? "after Render / Present" : "before Present") << "</p>";
            if (measurement.Mode == "Async" && measurement.ScopeMeanMs > 0 && measurement.ScopeP95Ms > 0)
                html << "<p>Timed CPU scope &nbsp; " << Decimal(measurement.ScopeMeanMs) << " ms mean &nbsp;/&nbsp; "
                     << Decimal(measurement.ScopeP95Ms) << " ms P95</p>";
        }
        else html << "<p class=\"muted\">" << (measurement.Error.empty() ? "Full-route frame trace unavailable." : Escape(measurement.Error)) << "</p>";
        if (winner) html << "<h2>Selected preset / " << mode << "</h2>";
        const auto image = ImageSummary(settings);
        if (!image.empty()) html << "<p>" << Escape(image) << "</p>";
        if (placement == &run.Winners[1] && run.Winners[0].Confirmed() && winner)
            Comparison(html, "Techniques compared with Native", run.Winners[0].Techniques, winner->Techniques, "Native", "Upscaled", true);
        const auto& observed = placement ? static_cast<const PlacementResult&>(*placement) : measurement.Observed;
        if (!placement || placement->Placements[1].Techniques.empty())
        {
            Comparison(html, "Measured techniques", {}, observed.Techniques, "", "Measured value", true);
            Comparison(html, "Measured owners", {}, observed.GpuSettings, "", "Measured value");
        }
        if (!settings.empty()) Comparison(html, winner ? "Preset settings" : "Pass settings", run.Before, settings,
            "Before run", winner ? "Selected preset" : "Measured pass");
        html << "</article>";
    };

    view(selected, current);
    if (both)
    {
        const auto& other = run.Winners[selected == &run.Winners[0] ? 1 : 0];
        Selection measurement;
        if (other.Confirmed()) Select(measurement, other.RunIndex, other.PassIndex);
        view(&other, measurement);
    }

    html << "<h2>Run details</h2>";
    if (!run.RouteId.empty()) html << "<p>Route &nbsp; " << Escape(run.RouteId) << "</p>";
    for (const auto& [id, adapter] : run.Adapters)
        html << "<p>" << (id == "-1" ? "Primary GPU" : "Eligible peer") << " &nbsp; " << Escape(adapter) << "</p>";
    if (!run.Passes.empty())
    {
        html << "<details><summary>Completed passes <small>" << run.Passes.size()
             << "</small></summary><div class=\"scroll\"><table><thead><tr><th>Pass</th><th>Average FPS</th><th>P95 (ms)</th></tr></thead><tbody>";
        for (const auto& pass : run.Passes)
            html << "<tr><th>" << Escape(pass.Label) << "</th><td>" << (pass.Timing.Fps > 0 ? Decimal(pass.Timing.Fps) : "&mdash;")
                 << "</td><td>" << (pass.Timing.P95 > 0 ? Decimal(pass.Timing.P95) : "&mdash;") << "</td></tr>";
        html << "</tbody></table></div></details>";
    }
    return html.str();
}

void BistroComparePanel::DrawExportStatus(const Run& run, const Winner* placement, const Selection& selection, bool requested)
{
    try
    {
        if (_exportJob.valid() && _exportJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            _exportPath = _exportJob.get();
        if (requested)
        {
            _exportPath.clear();
            _exportError.clear();
            _exportRun = run.Path;
            std::vector<fs::path> measured;
            auto body = ReportBody(run, placement, selection, measured);
            _exportJob = std::async(std::launch::async, WriteArchive, Engine::Core::GetExecutableDirectory() / "BenchmarkResults",
                run.Path, run.Modified, std::move(body), std::move(measured));
        }
    }
    catch (const std::exception& e) { _exportError = e.what(); }
    if (_exportRun != run.Path) return;
    if (_exportJob.valid()) ImGui::TextDisabled("Exporting ZIP...");
    if (!_exportError.empty()) ImGui::TextWrapped("Export: %s", _exportError.c_str());
    if (!_exportPath.empty())
    {
        ImGui::TextWrapped("Saved ZIP: %s", _exportPath.u8string().c_str());
        if (ImGui::SmallButton("Open export folder") &&
            reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", _exportPath.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            _exportError = "Could not open the export folder.";
    }
}
