#include "BistroInfoPanel.h"
#include "BistroIniMigration.h"
#include "BistroPlot.h"
#include "BistroBenchmark.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.UI/UI_test.h"
#include "../../../Resources/Resource.h"
#include <directxtex/DirectXTex.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr ImVec4 Rgb(unsigned rgb)
{
    return {float((rgb >> 16) & 255) / 255.f, float((rgb >> 8) & 255) / 255.f, float(rgb & 255) / 255.f, 1.f};
}
constexpr ImVec4 Pond = Rgb(0x101b1a), Mint = Rgb(0x60dab4), Lime = Rgb(0xb5ed7d);
constexpr std::pair<ImGuiCol, ImVec4> FullPalette[] = {
    {ImGuiCol_WindowBg, Pond},                          {ImGuiCol_ChildBg, Rgb(0x182522)},
    {ImGuiCol_PopupBg, Rgb(0x20302c)},                  {ImGuiCol_Border, Rgb(0x3c534b)},
    {ImGuiCol_Separator, Rgb(0x344b43)},                {ImGuiCol_Text, Rgb(0xeff6f0)},
    {ImGuiCol_TextDisabled, Rgb(0xacc1b7)},             {ImGuiCol_CheckMark, Mint},
    {ImGuiCol_NavCursor, Lime},                         {ImGuiCol_Button, Rgb(0x283e35)},
    {ImGuiCol_ButtonHovered, Rgb(0x385648)},            {ImGuiCol_ButtonActive, Rgb(0x416853)},
    {ImGuiCol_FrameBg, Rgb(0x121f1b)},                  {ImGuiCol_FrameBgHovered, Rgb(0x263d32)},
    {ImGuiCol_FrameBgActive, Rgb(0x304b3b)},            {ImGuiCol_Header, Rgb(0x24392e)},
    {ImGuiCol_HeaderHovered, Rgb(0x334c3d)},            {ImGuiCol_HeaderActive, Rgb(0x294535)},
    {ImGuiCol_Tab, Rgb(0x16231f)},                      {ImGuiCol_TabHovered, Rgb(0x334c3d)},
    {ImGuiCol_TabSelected, Rgb(0x294535)},              {ImGuiCol_TabSelectedOverline, Mint},
    {ImGuiCol_TableHeaderBg, Rgb(0x24352e)},            {ImGuiCol_SliderGrab, Mint},
    {ImGuiCol_SliderGrabActive, Lime},
};

// The app's icon group contains transparent PNGs. Keep the header mark in the UI atlas
// so the launcher can draw it before initializing the world renderer.
ImFontAtlasRectId LoadEngineMark(ImFontAtlas& atlas)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto group = FindResourceW(module, MAKEINTRESOURCEW(IDI_PEPENGINE2), RT_GROUP_ICON);
    if (!group) return ImFontAtlasRectId_Invalid;
    auto* directory = static_cast<PBYTE>(LockResource(LoadResource(module, group)));
    if (!directory) return ImFontAtlasRectId_Invalid;
    const int id = LookupIconIdFromDirectoryEx(directory, TRUE, 128, 128, LR_DEFAULTCOLOR);
    const auto icon = FindResourceW(module, MAKEINTRESOURCEW(id), RT_ICON);
    if (!icon) return ImFontAtlasRectId_Invalid;
    const auto* data = static_cast<const uint8_t*>(LockResource(LoadResource(module, icon)));
    struct Apartment
    {
        HRESULT Status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(Status)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.Status) && apartment.Status != RPC_E_CHANGED_MODE) return ImFontAtlasRectId_Invalid;
    DirectX::ScratchImage decoded;
    if (!data || FAILED(DirectX::LoadFromWICMemory(data, SizeofResource(module, icon), DirectX::WIC_FLAGS_FORCE_RGB,
        nullptr, decoded))) return ImFontAtlasRectId_Invalid;
    const auto* bitmap = decoded.GetImage(0, 0, 0);
    if (!bitmap || (bitmap->format != DXGI_FORMAT_R8G8B8A8_UNORM && bitmap->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
        return ImFontAtlasRectId_Invalid;
    ImFontAtlasRect rect;
    const auto rectId = atlas.AddCustomRect(int(bitmap->width), int(bitmap->height), &rect);
    if (rectId == ImFontAtlasRectId_Invalid) return rectId;
    if (atlas.TexData->Format != ImTextureFormat_RGBA32)
    {
        atlas.RemoveCustomRect(rectId);
        return ImFontAtlasRectId_Invalid;
    }
    atlas.TexPixelsUseColors = atlas.TexData->UseColors = true;
    for (int y = 0; y < rect.h; ++y)
        std::memcpy(atlas.TexData->GetPixelsAt(rect.x, rect.y + y), bitmap->pixels + y * bitmap->rowPitch, rect.w * 4);
    return rectId;
}

bool PageButton(const char* label, bool selected, ImVec2 size)
{
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Pond);
        ImGui::PushStyleColor(ImGuiCol_Button, Mint);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0x8de8c9));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(0x4cb995));
    }
    const bool pressed = ImGui::Button(label, size);
    if (selected) ImGui::PopStyleColor(4);
    return pressed;
}

}

void BistroInfoPanel::InitializeFont()
{
    auto* atlas = ImGui::GetIO().Fonts;
    if (atlas->Fonts.empty()) atlas->AddFontDefault();
    wchar_t windowsDir[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (length && length < MAX_PATH)
    {
        const auto fonts = std::filesystem::path(windowsDir) / L"Fonts";
        ImFontConfig config;
        config.Flags |= ImFontFlags_NoLoadError;
        _fullFont = atlas->AddFontFromFileTTF((fonts / L"segoeui.ttf").u8string().c_str(), 18.f, &config);
        _headingFont = atlas->AddFontFromFileTTF((fonts / L"seguisb.ttf").u8string().c_str(), 18.f, &config);
    }
    if (!_fullFont) _fullFont = atlas->AddFontDefaultVector();
    if (!_headingFont) _headingFont = _fullFont;
    _engineMark = LoadEngineMark(*atlas);
}

void BistroInfoPanel::ShowSettings(bool openResults, const std::string& error)
{
    _count = _next = 0;
    _lastPresent.reset();
    _gpuWork.clear();
    _timingEpoch = _packetCursor = 0;
    _displayError = error;
    _tab = openResults ? Tab::Results : Tab::Setup;
}

void BistroInfoPanel::ApplyPreset(const std::filesystem::path& path)
{
    ConsoleVariablePatch patch;
    auto result = ConsoleModule::ReadPatchIni(path, patch, [](std::wstring& key) {
        MigrateBistroIniKey(key);
        if (const auto page = LocalShadowPageSettingsIndex(key)) RegisterLocalShadowPageSettings(*page);
    });
    if (result.Error.empty())
        result = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->ApplyPatch(patch);
    _displayError = result.Error.empty() ? "" : "Could not apply preset: " + result.Error;
}

namespace
{
struct RunMode
{
    const char* Name;
    const char* Description;
    BistroRun::Mode Kind;
    BistroPlacement::Profile Profile;
};

constexpr RunMode RunModes[] = {
    {"Manual", "Fly the route once with your current settings.", BistroRun::Mode::Manual, BistroPlacement::Performance},
    {"Performance", "Find the best settings targeting 60 FPS.", BistroRun::Mode::Profile, BistroPlacement::Performance},
    {"Quality", "Prioritize visual quality targeting 30 FPS.", BistroRun::Mode::Profile, BistroPlacement::Quality},
};

void DrawModeCard(const RunMode& mode, BistroRun::Launch& launch, ImFont* heading, float height)
{
    const float em = ImGui::GetFontSize();
    const bool selected = launch.Kind == mode.Kind && (mode.Kind == BistroRun::Mode::Manual || mode.Profile == launch.Profile);
    const float width = ImGui::GetContentRegionAvail().x;
    const bool wide = width >= 25.f * em;
    const bool showIcon = width >= 19.f * em;
    const float inset = wide ? width * .305f : (showIcon ? 5.65f : 2.65f) * em;
    const float textWidth = std::max(em, width - inset - .8f * em);
    const float titleSize = std::min(1.68f * em,
        em * textWidth / std::max(em, heading->CalcTextSizeA(em, FLT_MAX, 0.f, mode.Name).x));
    const float descriptionSize = 1.10f * em;
    const float descriptionHeight = ImGui::GetFont()->CalcTextSizeA(descriptionSize, FLT_MAX, textWidth, mode.Description).y;
    const float textHeight = titleSize + .20f * em + descriptionHeight;
    height = std::max(height, textHeight + .65f * em);
    if (ImGui::InvisibleButton(mode.Name, {width, height}, ImGuiButtonFlags_EnableNav))
    {
        launch.Kind = mode.Kind;
        launch.Profile = mode.Profile;
    }
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const auto accent = ImGui::GetColorU32(selected ? Lime : Mint);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
    const ImU32 fill = selected ? ImGui::GetColorU32(hovered ? ImGuiCol_HeaderHovered : ImGuiCol_HeaderActive) :
        ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    draw->AddRectFilled(a, b, fill, .45f * em);
    draw->AddRect(a, b, focused ? ImGui::GetColorU32(ImGuiCol_NavCursor) : selected ? accent : ImGui::GetColorU32(ImGuiCol_Border),
        .45f * em, selected || focused ? .14f * em : 1.f);

    const float middle = (a.y + b.y) * .5f;
    const ImVec2 radio{a.x + (wide ? width * .058f : 1.22f * em), middle};
    draw->AddCircle(radio, .47f * em, selected ? accent : ImGui::GetColorU32(ImGuiCol_Text), 24, .11f * em);
    if (selected) draw->AddCircleFilled(radio, .25f * em, accent, 24);
    if (showIcon)
    {
        const float x = a.x + (wide ? width * .177f : 3.5f * em);
        const float iconScale = (wide ? 1.65f : 1.30f) * em;
        const auto p = [&](float dx, float dy) { return ImVec2{x + dx * iconScale, middle + dy * iconScale}; };
        const float stroke = .10f * iconScale;
        if (mode.Kind == BistroRun::Mode::Manual)
            draw->AddTriangle(p(-.48f,-.68f), p(.65f, 0), p(-.48f,.68f), accent, stroke);
        else if (mode.Profile == BistroPlacement::Performance)
        {
            for (int i = 0; i < 3; ++i)
                draw->AddRect(p(-.65f + .5f * i, .13f - .43f * i), p(-.36f + .5f * i, .61f), accent, 0, stroke);
        }
        else
        {
            const ImVec2 layer[]{p(-.78f,-.25f), p(0,-.67f), p(.78f,-.25f), p(0,.17f)};
            draw->AddPolyline(layer, 4, accent, stroke, ImDrawFlags_Closed);
            draw->AddLine(p(-.78f,.18f), p(0,.62f), accent, stroke);
            draw->AddLine(p(0,.62f), p(.78f,.18f), accent, stroke);
        }
    }
    if (wide)
    {
        const float divider = a.x + width * .266f;
        const float half = std::min(height * .32f, 2.15f * em);
        draw->AddLine({divider, middle - half}, {divider, middle + half}, ImGui::GetColorU32(ImGuiCol_Border));
    }
    const float left = a.x + inset, top = middle - textHeight * .5f;
    draw->PushClipRect(a, b, true);
    draw->AddText(heading, titleSize, {left, top}, ImGui::GetColorU32(ImGuiCol_Text), mode.Name);
    draw->AddText(ImGui::GetFont(), descriptionSize, {left, top + titleSize + .20f * em},
        ImGui::GetColorU32(Rgb(0xcadbcf)), mode.Description, nullptr, textWidth);
    draw->PopClipRect();
}

void DrawRoutePreview(const BistroRun::Map& map, float height)
{
    if (std::string_view(map.Route) != "bistro-flight-v1")
    {
        ImGui::TextWrapped("Route: %s", map.Route);
        return;
    }
    using namespace BistroBenchmarkRoute;
    const float em = ImGui::GetFontSize();
    ImGui::Dummy({ImGui::GetContentRegionAvail().x, height});
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_WindowBg), .35f * em);
    const auto grid = ImGui::GetColorU32(ImGuiCol_Border, .4f);
    const float radius = .35f * em;
    for (float x = a.x + em; x < b.x - radius; x += 1.25f * em) draw->AddLine({x, a.y + 1}, {x, b.y - 1}, grid);
    for (float y = a.y + em; y < b.y - radius; y += 1.25f * em) draw->AddLine({a.x + 1, y}, {b.x - 1, y}, grid);
    const struct { const Point* Points; size_t Count; } legs[]{
        {Red, std::size(Red)}, {Blue, std::size(Blue)}, {Green, std::size(Green)}, {Yellow, std::size(Yellow)}};
    // Project the flight's world-space XZ footprint in the same scale on both axes.
    ImVec2 low{FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX};
    for (const auto& leg : legs)
        for (size_t i = 0; i < leg.Count; ++i)
        {
            const auto& p = leg.Points[i].Position;
            low.x = std::min(low.x, p.x); high.x = std::max(high.x, p.x);
            low.y = std::min(low.y, p.z); high.y = std::max(high.y, p.z);
        }
    const float scale = std::max(0.f, std::min((b.x - a.x - 2 * em) / (high.x - low.x),
        (b.y - a.y - 1.1f * em) / (high.y - low.y)));
    const auto project = [&](const Point& point) {
        return ImVec2{(a.x + b.x) * .5f + (point.Position.x - (high.x + low.x) * .5f) * scale,
            (a.y + b.y) * .5f - (point.Position.z - (high.y + low.y) * .5f) * scale};
    };
    const auto accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    for (const auto& leg : legs)
        for (size_t i = 1; i < leg.Count; ++i)
            draw->AddLine(project(leg.Points[i-1]), project(leg.Points[i]), accent, std::max(1.25f, .09f * em));
    draw->AddCircleFilled(project(Red[0]), .23f * em, ImGui::GetColorU32(Lime));
    draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), .35f * em);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera route viewed from above");
}

std::string OutputLabel(BistroPlacement::Size size)
{
    return std::to_string(size.first) + u8" × " + std::to_string(size.second);
}

void DrawOutputPicker(BistroPlacement::Size& output)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##Output", OutputLabel(output).c_str())) return;
    std::vector<BistroPlacement::Size> sizes{{960,540}, {1280,720}, {1280,800}, {1600,900}, {1920,1080},
        {2560,1440}, {3840,2160}, output};
    const auto client = ImGui::GetMainViewport()->Size;
    if (client.x > 0 && client.y > 0) sizes.emplace_back(unsigned(client.x), unsigned(client.y));
    for (const auto& monitor : ImGui::GetPlatformIO().Monitors)
        if (monitor.MainSize.x > 0 && monitor.MainSize.y > 0)
            sizes.emplace_back(unsigned(monitor.MainSize.x), unsigned(monitor.MainSize.y));
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    for (const auto& size : sizes)
    {
        const bool selected = size == output;
        if (ImGui::Selectable(OutputLabel(size).c_str(), selected)) output = size;
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}
}

void BistroInfoPanel::DrawLaunchControls(BistroBenchmark& benchmark)
{
    const float em = ImGui::GetFontSize(), gap = ImGui::GetStyle().ItemSpacing.y;
    const auto area = ImGui::GetContentRegionAvail();
    const bool narrowFooter = area.x < 25.f * em;
    const float actionHeight = 2.4f * em;
    const float footerHeight = actionHeight + (narrowFooter ? em + gap : 0.f);
    ImGui::BeginDisabled(!benchmark.CanStart());
    if (ImGui::BeginChild("Setup choices", {0, std::max(1.f, area.y - footerHeight - 2 * gap - 1.f)},
        ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const bool columns = width >= 40.f * em;
        if (!_parametersOpen)
        {
            const float panelHeight = std::max(17.7f * em, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - gap);
            const auto heading = [&](const char* name, float scale) {
                ImGui::PushFont(_headingFont, scale * ImGui::GetStyle().FontSizeBase);
                ImGui::TextUnformatted(name);
                ImGui::PopFont();
            };
            if (ImGui::BeginChild("Run mode", {columns ? std::floor(width * .57f) : 0.f, columns ? panelHeight : 17.7f * em},
                ImGuiChildFlags_Borders))
            {
                heading("Run mode", 1.52f);
                const bool profile = _launch.Kind == BistroRun::Mode::Profile;
                const float policy = profile ? ImGui::GetFrameHeightWithSpacing() : 0.f;
                const float rowHeight = std::max(3.65f * em, std::floor((ImGui::GetContentRegionAvail().y - policy - 2 * gap) / 3));
                for (const auto& mode : RunModes) DrawModeCard(mode, _launch, _headingFont, rowHeight);
                if (profile) ImGui::Checkbox("Include upscalers in search", &_launch.AllowUpscalers);
            }
            ImGui::EndChild();
            if (columns) ImGui::SameLine();
            if (ImGui::BeginChild("Map and output", {0, columns ? panelHeight : 17.7f * em}, ImGuiChildFlags_Borders))
            {
                heading("Map and output", 1.52f);
                heading("Map", 1.08f);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##Map", _launch.Level->Name))
                {
                    for (const auto& map : BistroRun::Maps)
                    {
                        const bool selected = &map == _launch.Level;
                        if (ImGui::Selectable(map.Name, selected)) _launch.Level = &map;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                float bottom = 1.08f * em + ImGui::GetFrameHeight() + 2 * gap;
#if defined(_DEBUG) || defined(DEBUG)
                bottom += ImGui::GetFrameHeightWithSpacing();
#endif
                DrawRoutePreview(*_launch.Level, std::max(2.8f * em, ImGui::GetContentRegionAvail().y - bottom));
                heading("Output resolution", 1.08f);
                DrawOutputPicker(_launch.Output);
#if defined(_DEBUG) || defined(DEBUG)
                if (ImGui::Button("Explore map (Debug)", {-FLT_MIN, 0})) _requestedDebugMap = _launch.Level;
#endif
            }
            ImGui::EndChild();
        }
        ImGui::SetNextItemOpen(_parametersOpen);
        _parametersOpen = ImGui::CollapsingHeader("Render parameters");
        if (_parametersOpen)
        {
            if (ImGui::BeginChild("Render parameter editor", {0, std::max(12.f * em, ImGui::GetContentRegionAvail().y)},
                ImGuiChildFlags_Borders))
                Engine::UI::DrawSettingsEditor();
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    const bool manual = _launch.Kind == BistroRun::Mode::Manual;
    const char* mode = manual ? "Manual" : (_launch.Profile == BistroPlacement::Performance ? "Performance" : "Quality");
    const char* action = manual ? "Start benchmark" :
        (_launch.Profile == BistroPlacement::Performance ? "Find Performance" : "Find Quality");
    const std::string summary = std::string(mode) + u8"   ·   " + _launch.Level->Name + u8"   ·   " + OutputLabel(_launch.Output);
    const auto start = ImGui::GetCursorScreenPos();
    const float buttonWidth = narrowFooter ? area.x : std::max(10.f * em, ImGui::CalcTextSize(action).x + 2 * em);
    ImGui::SetCursorScreenPos({start.x, start.y + (narrowFooter ? 0.f : (actionHeight - em) * .5f)});
    ImGui::PushClipRect(start, {start.x + (narrowFooter ? area.x : area.x - buttonWidth - em), start.y + actionHeight}, true);
    ImGui::TextUnformatted(summary.c_str());
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos({start.x + area.x - buttonWidth, start.y + (narrowFooter ? em + gap : 0.f)});
    ImGui::PushStyleColor(ImGuiCol_Button, Lime);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0xc7f496));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(0x98cf66));
    ImGui::PushStyleColor(ImGuiCol_Text, Pond);
    ImGui::PushFont(_headingFont, 0.f);
    if (ImGui::Button(action, {buttonWidth, actionHeight}))
        benchmark.Launch(_launch, *BenchmarkEngine::GetLocator().GetModule<ConsoleModule>());
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
}

void BistroInfoPanel::DrawSettings(RenderModule& render, BistroBenchmark& benchmark)
{
    if (!ImGui::GetCurrentContext()) return;
    if (!_initializedSize)
    {
        if (const auto* common = render.GetRenderPipelineCommonData(); common && common->WindowWidth && common->WindowHeight)
            _launch.Output = {common->WindowWidth, common->WindowHeight};
        _initializedSize = true;
    }
    _compare.UpdateHistory(benchmark.ReportPath());
    const auto* vp = ImGui::GetMainViewport();
    const float size = std::min(vp->WorkSize.x / 55.f, vp->WorkSize.y / 32.f);
    ImGui::PushFont(_fullFont, std::clamp(std::round(size), 18.f, 48.f));
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {.95f * em, .68f * em});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {.6f * em, .30f * em});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {.65f * em, .40f * em});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, .45f * em);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, .30f * em);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, .30f * em);
    for (const auto& [color, value] : FullPalette) ImGui::PushStyleColor(color, value);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    if (ImGui::Begin("Benchmark", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 start = ImGui::GetCursorPos();
        const float navHeight = 2.60f * em;
        const float baseSize = ImGui::GetStyle().FontSizeBase;
        ImGui::BeginGroup();
        ImFontAtlasRect mark;
        auto* atlas = ImGui::GetIO().Fonts;
        if (atlas->GetCustomRect(_engineMark, &mark))
        {
            ImGui::Image(atlas->TexRef, {2.85f * em, 2.85f * em}, mark.uv0, mark.uv1);
            ImGui::SameLine(0.f, .68f * em);
        }
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.f, 0.f});
        ImGui::PushFont(_headingFont, 1.65f * baseSize);
        ImGui::TextColored(Lime, "PEPENGINE2");
        ImGui::PopFont();
        ImGui::PushFont(_fullFont, baseSize);
        ImGui::TextUnformatted("Benchmark");
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        ImGui::EndGroup();
        const ImVec2 titleSize = ImGui::GetItemRectSize();
        const float navWidth = std::clamp(width * .125f, 5.1f * em, 8.f * em);
        const float navGap = ImGui::GetStyle().ItemSpacing.x * .35f;
        if (titleSize.x + 2.f * navWidth + 2.f * navGap < width)
            ImGui::SetCursorPos({start.x + width - 2.f * navWidth - navGap, start.y + (titleSize.y - navHeight) * .5f});
        ImGui::PushFont(_headingFont, 1.12f * baseSize);
        if (PageButton("Setup", _tab == Tab::Setup, {navWidth, navHeight})) _tab = Tab::Setup;
        ImGui::SameLine(0.f, navGap);
        if (PageButton("Results", _tab == Tab::Results, {navWidth, navHeight})) _tab = Tab::Results;
        ImGui::PopFont();
        ImGui::Separator();
        if (!_displayError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 146, 255));
            ImGui::TextWrapped("%s", _displayError.c_str());
            ImGui::PopStyleColor();
        }
        else if (!benchmark.Message().empty() && benchmark.Message() != "completed")
            ImGui::TextWrapped("%s", benchmark.Message().c_str());
        if (_tab == Tab::Setup) DrawLaunchControls(benchmark);
        else
        {
            std::filesystem::path preset;
            _compare.DrawHistory(benchmark.ReportPath(), preset, _headingFont);
            if (!preset.empty()) ApplyPreset(preset);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(int(std::size(FullPalette)));
    ImGui::PopStyleVar(6);
    ImGui::PopFont();
}

void BistroInfoPanel::RecordPresent(std::chrono::steady_clock::time_point now,
    BistroRender::QueueSubmission& submissions, bool manualGpuTimings)
{
    if (_lastPresent)
    {
        // The Windows steady clock and DX12 GetClockCalibration both use the QPC epoch.
        _presentBeginMs[_next] = std::chrono::duration<double, std::milli>(_lastPresent->time_since_epoch()).count();
        _presentMs[_next] = std::chrono::duration<float, std::milli>(now - *_lastPresent).count();
        _next = (_next + 1) % _presentMs.size();
        _count = std::min(_count + 1, _presentMs.size());
    }
    _lastPresent = now;
    if (!_count) return;
    const double first = _presentBeginMs[_count == _presentMs.size() ? _next : 0];
    for (const auto& packet : submissions.ReadBenchmarkIntervals(_timingEpoch, _packetCursor, manualGpuTimings))
    {
        if (packet.EndMs <= first) continue;
        auto [device, added] = _gpuWork.try_emplace(packet.Device);
        if (added) device->second.Adapter = submissions.AdapterLabel(packet.Device);
        auto& intervals = device->second.Intervals;
        // Keep the union of retired packets, including delayed and overlapping queue submissions.
        auto it = std::lower_bound(intervals.begin(), intervals.end(), packet.BeginMs,
            [](const auto& interval, double begin) { return interval.second < begin; });
        double begin = packet.BeginMs, end = packet.EndMs;
        if (it == intervals.end() || end < it->first)
            intervals.insert(it, {begin, end});
        else
        {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            auto next = it + 1;
            while (next != intervals.end() && next->first <= end)
            {
                end = std::max(end, next->second);
                ++next;
            }
            *it = {begin, end};
            intervals.erase(it + 1, next);
        }
    }
    for (auto& [id, gpu] : _gpuWork)
    {
        auto& intervals = gpu.Intervals;
        intervals.erase(intervals.begin(), std::lower_bound(intervals.begin(), intervals.end(), first,
            [](const auto& interval, double cutoff) { return interval.second < cutoff; }));
    }
}

void BistroInfoPanel::DrawMeasurement(const BistroBenchmark& benchmark) const
{
    if (!ImGui::GetCurrentContext()) return;
    const auto* vp = ImGui::GetMainViewport();
    const float size = std::clamp(std::round(vp->Size.y / 36.f), 16.f, 40.f);
    ImGui::PushFont(_fullFont, size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos({vp->Pos.x, vp->Pos.y + vp->Size.y}, ImGuiCond_Always, {0,1});
    ImGui::SetNextWindowSize({vp->Size.x, 0});
    ImGui::SetNextWindowBgAlpha(.9f);
    if (ImGui::Begin("##Measurement", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
    {
        const int seconds = int(std::ceil(std::max(0., benchmark.HudRemainingSeconds())));
        ImGui::Text("%s   |   %d:%02d remaining   |   Esc to cancel", benchmark.HudPhase(), seconds / 60, seconds % 60);
        if (_count)
        {
            const auto last = (_next + _presentMs.size() - 1) % _presentMs.size();
            BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize("Present interval   1000.0 ms").x);
            ImGui::Text("Present interval   %.1f ms", _presentMs[last]);
            const auto first = _count == _presentMs.size() ? _next : 0;
            const float peak = std::max(1.f, *std::max_element(_presentMs.begin(), _presentMs.begin() + _count));
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 size{std::max(1.f, ImGui::GetContentRegionAvail().x), 2.f * ImGui::GetFontSize()};
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, ImGui::GetColorU32(ImGuiCol_FrameBg));
            const auto point = [&](size_t i, double ms) {
                return ImVec2{origin.x + size.x * float(i) / float(std::max<size_t>(1, _count - 1)),
                    origin.y + size.y * (1.f - std::clamp(float(ms) / peak, 0.f, 1.f))};
            };
            const auto presentColor = IM_COL32(235, 239, 242, 255);
            for (size_t i = 1; i < _count; ++i)
                draw->AddLine(point(i - 1, _presentMs[(first + i - 1) % _presentMs.size()]),
                    point(i, _presentMs[(first + i) % _presentMs.size()]), presentColor, 1.5f);
            ImGui::Dummy(size);
            for (const auto& [id, gpu] : _gpuWork)
            {
                if (gpu.Intervals.empty()) continue;
                const auto color = ImColor::HSV(std::fmod(.46f + .382f * float(id), 1.f), .65f, .98f);
                ImGui::TextColored(color, "%s: timed GPU work (incl. background)", gpu.Adapter.c_str());
                auto interval = gpu.Intervals.begin();
                std::optional<ImVec2> previous;
                for (size_t i = 0; i < _count; ++i)
                {
                    const auto frame = (first + i) % _presentMs.size();
                    const double begin = _presentBeginMs[frame], end = begin + _presentMs[frame];
                    while (interval != gpu.Intervals.end() && interval->second <= begin) ++interval;
                    double work = 0;
                    for (auto it = interval; it != gpu.Intervals.end() && it->first < end; ++it)
                        work += std::min(end, it->second) - std::max(begin, it->first);
                    if (work <= 0) { previous.reset(); continue; }
                    const auto p = point(i, work);
                    if (previous) draw->AddLine(*previous, p, color, 1.7f);
                    else draw->AddCircleFilled(p, 1.5f, color);
                    previous = p;
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopFont();
}
