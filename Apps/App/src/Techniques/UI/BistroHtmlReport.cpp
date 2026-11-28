#include "BistroComparePanel.h"
#include "BistroReportArchive.h"
#include "BistroReportValues.h"
#include <iomanip>
#include <initializer_list>
#include <locale>
#include <algorithm>
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

struct FieldGroup
{
    const Settings& Before;
    const Settings& After;
    FieldKind Kind;
};

void Comparison(std::ostream& html, const char* title, const char* leftLabel, const char* rightLabel,
    std::initializer_list<FieldGroup> groups, bool expanded = false)
{
    std::vector<ComparedField> rows;
    bool comparable = false, hasBefore = false;
    for (const auto& group : groups)
    {
        const auto& a = group.Before;
        const auto& b = group.After;
        const bool both = !a.empty() && !b.empty();
        comparable |= both && group.Kind != FieldKind::Metric;
        hasBefore |= !a.empty();
        for (const auto& [key, value] : a)
            rows.push_back(CompareField(key, value, Value(b, key), group.Kind, both));
        for (const auto& [key, value] : b)
            if (!a.count(key)) rows.push_back(CompareField(key, {}, value, group.Kind, both));
    }
    if (rows.empty()) return;
    const auto changes = std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.Delta != Change::None; });
    html << "<details" << (expanded ? " open" : "") << (comparable ? " class=\"changes-only\"" : "") << "><summary>" << title;
    if (comparable) html << " <small>" << changes << (changes == 1 ? " change" : " changes") << "</small>";
    html << "</summary>";
    if (comparable)
    {
        html << "<div class=\"switch filter\"><button aria-pressed=\"true\">Changes only</button>"
             << "<button aria-pressed=\"false\">All settings</button></div>";
        if (!changes) html << "<p class=\"no-changes muted\">No recorded differences.</p>";
    }
    html << "<div class=\"scroll\" tabindex=\"0\" role=\"region\" aria-label=\"" << Escape(title)
         << "\"><table class=\"settings-table\"><thead><tr>";
    if (hasBefore) html << "<th>" << leftLabel << "</th>";
    html << "<th>Technique or setting</th><th>" << rightLabel << "</th></tr></thead><tbody>";
    for (const auto& row : rows)
    {
        html << (row.Delta == Change::None ? "<tr class=\"same\">" : "<tr>");
        if (hasBefore) html << "<td>" << (row.Before.empty() ? "Not recorded" : Escape(row.Before)) << "</td>";
        html << "<th>" << Escape(row.Label) << "</th><td>" << (row.After.empty() ? "Not recorded" : Escape(row.After));
        if (row.Delta != Change::None)
            html << " <span class=\"delta " << ChangeLabel(row.Delta) << "\">" << ChangeLabel(row.Delta) << "</span>";
        html << "</td></tr>";
    }
    html << "</tbody></table></div></details>";
}
}

std::string BistroComparePanel::ReportBody(const Run& run, const Winner* selected, const Selection& current,
    std::vector<fs::path>& measuredPasses, std::vector<fs::path>& completedPasses)
{
    std::ostringstream html;
    html.imbue(std::locale::classic());
    html << std::setprecision(12);
    html << "<header><div><h1>" << Escape(run.MapName) << "</h1><p class=\"muted\">" << Escape(run.Timestamp)
         << "</p></div><a class=\"action\" href=\"#sources\">Source files &darr;</a></header>";
    if (run.BaseBudget && run.AddedRoutes && run.Budget)
        html << "<p>Search budget " << Decimal(*run.Budget) << " s / base " << Decimal(*run.BaseBudget)
             << " s + full reconstruction routes " << Decimal(*run.AddedRoutes) << " s</p>";
    if (run.Elapsed) html << "<p>Elapsed " << Decimal(*run.Elapsed) << " s</p>";
    if (run.BaseBudget)
    {
        html << "<details><summary>Search phases</summary><ul>";
        for (size_t i = 0; i < run.Phases.size(); ++i)
        {
            const auto& phase = run.Phases[i];
            if (phase.Base && phase.Total)
                html << "<li>" << SearchPhaseLabels[i] << " / " << Decimal(*phase.Total) << " s total / "
                     << Decimal(*phase.Base) << " s base + " << Decimal(phase.Added.value_or(0.)) << " s full routes</li>";
        }
        html << "</ul></details>";
    }
    const bool both = run.Winners[0].Available() && run.Winners[1].Available();
    if (both)
    {
        html << "<nav aria-label=\"Image mode\"><span>Image mode</span><div class=\"switch\">";
        for (int mode = 0; mode < 2; ++mode)
            html << "<button data-view=\"v" << mode << "\" aria-pressed=\"" << (selected == &run.Winners[mode] ? "true" : "false")
                 << "\">" << (mode ? "Upscaled" : "Native") << "</button>";
        html << "</div></nav>";
    }

    const auto view = [&](const Winner* placement, const Selection& measurement, std::array<Selection, 2>& cache) {
        const Winner* winner = placement && placement->Confirmed() ? placement : nullptr;
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
        if (run.Mode == "Manual" && run.FinalStatus == "invalid measurement")
        {
            html << "<p>" << Escape(run.StopReason) << "</p>";
            if (!run.ManualReconstructionFallback.empty())
                html << "<p>" << Escape(run.ManualReconstructionFallback) << "</p>";
        }
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
            Comparison(html, "Placement metrics and configuration", "Single GPU", "Multi-GPU",
                {{metrics(a), metrics(b), FieldKind::Metric}});
            Comparison(html, "GPU placement and technique changes", "Single GPU", "Multi-GPU",
                {{a.GpuSettings, b.GpuSettings, FieldKind::Placement}, {a.Techniques, b.Techniques, FieldKind::Technique}}, true);
        }

        html << "<h2>Frame timing</h2>";
        const auto lines = RunLines(placement, measurement, cache);
        const bool hasTrace = lines[0].Data || lines[1].Data;
        const bool pair = lines[0].Data && lines[1].Data;
        const bool comparable = ComparableFrames(lines);
        html << "<figure data-target=\"" << run.Target() << "\" data-comparable=\"" << (comparable ? "true" : "false") << "\">";
        if (hasTrace)
        {
            html << "<div class=\"plot-controls\"><div class=\"plot-tools\"><strong class=\"plot-title\">Frame timing";
            if (placement) html << " &middot; " << mode;
            html << "</strong><div class=\"switch\"><button aria-pressed=\"true\">Frame time (ms)</button><button aria-pressed=\"false\">FPS</button></div>"
                 << "<div class=\"plot-zoom\" role=\"group\" aria-label=\"Chart tools\" hidden>"
                 << "<button class=\"chart-icon zoom-button plus\" data-scale=\"0.8\" aria-label=\"Zoom in\"><span aria-hidden=\"true\"></span></button>"
                 << "<button class=\"chart-icon zoom-button\" data-scale=\"1.25\" aria-label=\"Zoom out\"><span aria-hidden=\"true\"></span></button>"
                 << "<button class=\"chart-icon reset-chart\" data-scale=\"1\" aria-label=\"Reset view\"><span aria-hidden=\"true\"></span></button>"
                 << "<button class=\"chart-icon help-chart\" aria-label=\"Chart help\" aria-expanded=\"false\"><span aria-hidden=\"true\">?</span></button></div>"
                 << "<div class=\"plot-pan\" role=\"group\" aria-label=\"Move time window\" hidden>"
                 << "<button class=\"chart-icon pan-earlier\" data-pan=\"-0.5\" aria-label=\"Pan earlier\"></button>"
                 << "<button class=\"chart-icon pan-later\" data-pan=\"0.5\" aria-label=\"Pan later\"></button></div></div>"
                 << "<button class=\"chart-icon expand-chart\" aria-label=\"Expand chart\" aria-haspopup=\"dialog\" aria-expanded=\"false\"></button>"
                 << "<div class=\"plot-help\" hidden><strong>Chart gestures</strong><ul><li>Drag a box to zoom</li><li>Click a point to pin its frames</li><li>Wheel to zoom at the pointer</li>"
                 << "<li>Right-drag or Shift-drag to pan</li><li>Use the pan arrows to move the time window</li><li>Double-click to reset</li>"
                 << "<li>On the plot, arrow keys step through frames</li></ul></div></div>"
                 << "<svg tabindex=\"0\" role=\"group\" aria-label=\"Frame timing along the route. Use arrow keys to inspect frames.\"></svg>"
                 << "<div class=\"route-axis\">Route time (s)</div>";
        }
        html << "<figcaption><div class=\"plot-legend\">";
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const auto& line = lines[i];
            if (!line.Data)
            {
                if (!line.Label.empty()) html << "<span>" << Escape(line.Label) << "</span>";
                continue;
            }
            const auto& data = *line.Data;
            const auto& pass = _runs[data.RunIndex].Passes[data.PassIndex];
            measuredPasses.push_back(pass.Path);
            html << "<div class=\"series " << (i == 0 ? "first" : "second") << "\" data-trace=\"";
            for (auto index : data.Trace) html << index << ' ';
            html << "\" data-frames=\"";
            std::map<std::string, size_t> sections;
            for (const auto& frame : data.Samples)
            {
                const auto section = sections.try_emplace(frame.Section, sections.size()).first->second;
                html << frame.RouteSeconds << ',' << frame.IntervalMs << ',' << frame.FrameMs << ',' << frame.Number << ',' << section << ' ';
            }
            html << "\" title=\"" << Escape(pass.Label)
                 << " &middot; " << data.Samples.size() << " frames &middot; Interval measured "
                 << (data.AfterRender ? "after Render / Present" : "before Present") << "\"><span class=\"series-label\" data-fps=\"";
            const auto role = pair ? (i == 0 ? "A / " : "B / ") : "";
            html << role << Escape(line.Caption(true)) << "\">" << role << Escape(line.Caption(false)) << "</span>";
            for (const auto& [name, index] : sections) html << "<span hidden data-section=\"" << index << "\">" << Escape(name) << "</span>";
            html << "</div>";
        }
        if (hasTrace && run.Target() > 0)
            html << "<span class=\"target-key\">Target " << Decimal(1000. / run.Target()) << " ms</span>";
        html << "</div>";
        if (hasTrace)
        {
            html << "<div class=\"frame-readout\"><dl class=\"frame-values\"></dl><div class=\"frame-actions\">";
            html << "<details class=\"frame-details\"><summary>Frame details</summary><div class=\"frame-samples\"></div></details>"
                 << "<button class=\"frame-unpin\" disabled>Follow pointer</button>";
            if (pair && !comparable) html << "<p class=\"frame-note\">" << FrameDeltaNote << "</p>";
            html << "</div></div>";
        }
        else if (lines[0].Label.empty() && lines[1].Label.empty())
            html << "<p class=\"muted\">" << (measurement.Error.empty() ? "Full-route frame trace unavailable." : Escape(measurement.Error)) << "</p>";
        html << "</figcaption></figure>";
        if (measurement.Mode == "Async" && measurement.ScopeMeanMs > 0 && measurement.ScopeP95Ms > 0)
            html << "<p>Timed CPU scope &nbsp; " << Decimal(measurement.ScopeMeanMs) << " ms mean &nbsp;/&nbsp; "
                 << Decimal(measurement.ScopeP95Ms) << " ms P95</p>";
        if (winner) html << "<h2>Selected preset / " << mode << "</h2>";
        const auto image = ImageSummary(settings);
        if (!image.empty()) html << "<p>" << Escape(image) << "</p>";
        if (placement == &run.Winners[1] && run.Winners[0].Confirmed() && winner)
            Comparison(html, "Techniques compared with Native", "Native", "Upscaled",
                {{run.Winners[0].GpuSettings, winner->GpuSettings, FieldKind::Placement},
                 {run.Winners[0].Techniques, winner->Techniques, FieldKind::Technique}}, true);
        const auto& observed = placement ? static_cast<const PlacementResult&>(*placement) : measurement.Observed;
        if (!placement || placement->Placements[1].Techniques.empty())
        {
            Comparison(html, "Measured techniques and owners", "", "Measured value",
                {{{}, observed.GpuSettings, FieldKind::Placement}, {{}, observed.Techniques, FieldKind::Technique}});
        }
        if (!settings.empty()) Comparison(html, winner ? "Preset settings" : "Pass settings", "Before run",
            winner ? "Selected preset" : "Measured pass", {{run.Before, settings, FieldKind::Setting}});
        html << "</article>";
    };

    view(selected, current, _placementPasses);
    if (both)
    {
        const auto& other = run.Winners[selected == &run.Winners[0] ? 1 : 0];
        Selection measurement;
        if (other.Confirmed()) Select(measurement, other.RunIndex, other.PassIndex);
        std::array<Selection, 2> cache;
        view(&other, measurement, cache);
    }

    html << "<h2>Run details</h2>";
    if (!run.RouteId.empty()) html << "<p>Route &nbsp; " << Escape(run.RouteId) << "</p>";
    for (const auto& [id, adapter] : run.Adapters)
        html << "<p>" << (id == "-1" ? "Primary GPU" : "Eligible peer") << " &nbsp; " << Escape(adapter) << "</p>";
    if (!run.Passes.empty())
    {
        html << "<details><summary>Completed passes <small>" << run.Passes.size()
             << "</small></summary><div class=\"scroll\" tabindex=\"0\" role=\"region\" aria-label=\"Completed passes\">"
             << "<table class=\"passes\"><thead><tr><th scope=\"col\">Pass</th><th scope=\"col\" class=\"number\">Average FPS</th>"
             << "<th scope=\"col\" class=\"number\">P95 (ms)</th><th scope=\"col\">Source</th></tr></thead><tbody>";
        for (const auto& pass : run.Passes)
        {
            html << "<tr><th scope=\"row\">" << Escape(pass.Label) << "</th><td class=\"number\">"
                 << (pass.Timing.Fps > 0 ? Decimal(pass.Timing.Fps) : "&mdash;") << "</td><td class=\"number\">"
                 << (pass.Timing.P95 > 0 ? Decimal(pass.Timing.P95) : "&mdash;") << "</td><td>"
                 << PassSourceMarker(completedPasses.size()) << "</td></tr>";
            completedPasses.push_back(pass.Path);
        }
        html << "</tbody></table></div></details>";
    }
    const auto writeReconstruction = [&](const auto& groups, const auto& columns, const char* heading)
    {
        if (groups.empty()) return;
        html << "<h2>" << heading << "</h2>";
        for (const auto& entry : groups)
        {
            const auto& group = entry.second;
            html << "<details><summary>" << Escape(group.Mode) << " / " << Escape(group.Status)
                 << "</summary><div class=\"scroll\" tabindex=\"0\" role=\"region\" aria-label=\"" << Escape(group.Mode)
                 << "\"><table class=\"reconstruction\"><thead><tr>";
            for (size_t col : columns)
                html << "<th scope=\"col\"" << (col == 5 || col == 6 ? " class=\"number\"" : "") << ">"
                     << ReconstructionColumns[col] << "</th>";
            html << "</tr></thead><tbody>";
            for (const auto& trial : group.Participants)
            {
                html << "<tr>";
                const auto row = ReconstructionRow(run, group, trial);
                for (size_t col : columns)
                    html << (col == 0 ? "<th scope=\"row\">" : col == 5 || col == 6 ? "<td class=\"number\">" : "<td>")
                         << Escape(row[col]) << (col == 0 ? "</th>" : "</td>");
                html << "</tr>";
            }
            html << "</tbody></table></div></details>";
        }
    };
    writeReconstruction(run.ReconstructionDiscovery, DiscoveryColumns, "Reconstruction availability");
    writeReconstruction(run.Reconstructions, ComparisonColumns, "Reconstruction comparisons");
    return html.str();
}
