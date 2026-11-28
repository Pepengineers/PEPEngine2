#include "Engine.UI/PepengineTheme.h"
#include <Windows.h>
#include <directxtex/DirectXTex.h>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <utility>

namespace Engine::UI::PepengineTheme
{
namespace
{
struct BrandAtlas
{
    ImFontAtlas* Atlas = nullptr;
    ImFont* Body = nullptr;
    ImFont* Heading = nullptr;
    ImFontAtlasRectId Mark = ImFontAtlasRectId_Invalid;
};
BrandAtlas Brand;

constexpr ImVec4 Rgb(unsigned rgb)
{
    return {float((rgb >> 16) & 255) / 255.f, float((rgb >> 8) & 255) / 255.f, float(rgb & 255) / 255.f, 1.f};
}
constexpr std::pair<ImGuiCol, ImVec4> Palette[] = {
    {ImGuiCol_WindowBg, Ink},                           {ImGuiCol_ChildBg, Panel},
    {ImGuiCol_PopupBg, Rgb(0x20302c)},                  {ImGuiCol_Border, Border},
    {ImGuiCol_Separator, Border},                      {ImGuiCol_Text, Paper},
    {ImGuiCol_TextDisabled, Rgb(0xb2c4b9)},            {ImGuiCol_CheckMark, Lime},
    {ImGuiCol_NavCursor, Lime},                        {ImGuiCol_Button, Rgb(0x283e35)},
    {ImGuiCol_ButtonHovered, Rgb(0x385648)},           {ImGuiCol_ButtonActive, Rgb(0x416853)},
    {ImGuiCol_FrameBg, Rgb(0x121f1b)},                 {ImGuiCol_FrameBgHovered, Rgb(0x263d32)},
    {ImGuiCol_FrameBgActive, Rgb(0x304b3b)},           {ImGuiCol_Header, Rgb(0x294533)},
    {ImGuiCol_HeaderHovered, Rgb(0x344f3d)},           {ImGuiCol_HeaderActive, Rgb(0x37573e)},
    {ImGuiCol_TitleBg, Ink},                           {ImGuiCol_TitleBgActive, Rgb(0x233b33)},
    {ImGuiCol_TitleBgCollapsed, Ink},                  {ImGuiCol_CheckboxSelectedBg, Rgb(0x314e39)},
    {ImGuiCol_Tab, Rgb(0x16231f)},                     {ImGuiCol_TabHovered, Rgb(0x334c3d)},
    {ImGuiCol_TabSelected, Rgb(0x294535)},             {ImGuiCol_TabSelectedOverline, Mint},
    {ImGuiCol_TableHeaderBg, Rgb(0x24352e)},           {ImGuiCol_TableBorderStrong, Border},
    {ImGuiCol_TableBorderLight, Rgb(0x2b3e35)},        {ImGuiCol_SliderGrab, Mint},
    {ImGuiCol_SliderGrabActive, Lime},                 {ImGuiCol_ScrollbarBg, Ink},
    {ImGuiCol_ScrollbarGrab, Rgb(0x456354)},           {ImGuiCol_ScrollbarGrabHovered, Rgb(0x60816c)},
    {ImGuiCol_ScrollbarGrabActive, Rgb(0x759b80)},     {ImGuiCol_TextSelectedBg, Rgb(0x3b614e)},
    {ImGuiCol_ResizeGrip, Rgb(0x3d594b)},              {ImGuiCol_ResizeGripHovered, Mint},
    {ImGuiCol_ResizeGripActive, Lime},
};

ImFontAtlasRectId AddImage(ImFontAtlas& atlas, const uint8_t* data, DWORD size)
{
    struct Apartment
    {
        HRESULT Status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(Status)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.Status) && apartment.Status != RPC_E_CHANGED_MODE) return ImFontAtlasRectId_Invalid;
    DirectX::ScratchImage decoded;
    if (!data || FAILED(DirectX::LoadFromWICMemory(data, size, DirectX::WIC_FLAGS_FORCE_RGB,
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

// The app's icon group contains transparent PNGs. Keep the header mark in the UI atlas
// so the launcher can draw it before initializing the world renderer.
ImFontAtlasRectId LoadEngineMark(ImFontAtlas& atlas, int iconResource)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto group = FindResourceW(module, MAKEINTRESOURCEW(iconResource), RT_GROUP_ICON);
    if (!group) return ImFontAtlasRectId_Invalid;
    auto* directory = static_cast<PBYTE>(LockResource(LoadResource(module, group)));
    if (!directory) return ImFontAtlasRectId_Invalid;
    const int id = LookupIconIdFromDirectoryEx(directory, TRUE, 128, 128, LR_DEFAULTCOLOR);
    const auto icon = FindResourceW(module, MAKEINTRESOURCEW(id), RT_ICON);
    if (!icon) return ImFontAtlasRectId_Invalid;
    const auto* data = static_cast<const uint8_t*>(LockResource(LoadResource(module, icon)));
    return AddImage(atlas, data, SizeofResource(module, icon));
}
}

ImFontAtlasRectId LoadResourceImage(ImFontAtlas& atlas, int id)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto image = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!image) return ImFontAtlasRectId_Invalid;
    const auto* data = static_cast<const uint8_t*>(LockResource(LoadResource(module, image)));
    return AddImage(atlas, data, SizeofResource(module, image));
}

void Initialize(ImFontAtlas& atlas, int iconResource)
{
    Reset();
    Brand.Atlas = &atlas;
    if (atlas.Fonts.empty()) atlas.AddFontDefault();
    wchar_t windowsDir[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (length && length < MAX_PATH)
    {
        const auto fonts = std::filesystem::path(windowsDir) / L"Fonts";
        ImFontConfig config;
        config.Flags |= ImFontFlags_NoLoadError;
        Brand.Body = atlas.AddFontFromFileTTF((fonts / L"segoeui.ttf").u8string().c_str(), 18.f, &config);
        Brand.Heading = atlas.AddFontFromFileTTF((fonts / L"seguisb.ttf").u8string().c_str(), 18.f, &config);
    }
    if (!Brand.Body) Brand.Body = atlas.AddFontDefaultVector();
    if (!Brand.Heading) Brand.Heading = Brand.Body;
    Brand.Mark = LoadEngineMark(atlas, iconResource);
}

void Reset() { Brand = {}; }

ImFont* BodyFont() { return ImGui::GetIO().Fonts == Brand.Atlas ? Brand.Body : nullptr; }
ImFont* HeadingFont() { return ImGui::GetIO().Fonts == Brand.Atlas ? Brand.Heading : nullptr; }

void Push()
{
    for (const auto& [slot, color] : Palette) ImGui::PushStyleColor(slot, color);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 5.f);
}

void Pop()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(int(std::size(Palette)));
}

void DrawBrand(float iconSize)
{
    const float em = ImGui::GetFontSize();
    if (iconSize <= 0.f) iconSize = 1.65f * em;
    ImGui::PushFont(HeadingFont(), ImGui::GetStyle().FontSizeBase * (.68f * iconSize / em));
    constexpr char name[] = "Pepengine2";
    const auto label = ImGui::CalcTextSize(name);
    // Measuring text can grow the atlas. Resolve the icon UVs after the glyphs have been loaded.
    auto* atlas = ImGui::GetIO().Fonts;
    ImFontAtlasRect mark;
    const bool hasIcon = atlas == Brand.Atlas && atlas->GetCustomRect(Brand.Mark, &mark);
    const float inset = hasIcon ? iconSize + .45f * em : 0.f;
    ImGui::Dummy({inset + label.x, iconSize});
    const auto pos = ImGui::GetItemRectMin();
    auto* draw = ImGui::GetWindowDrawList();
    if (hasIcon)
        draw->AddImage(atlas->TexRef, pos, {pos.x + iconSize, pos.y + iconSize}, mark.uv0, mark.uv1);
    draw->AddText({pos.x + inset, pos.y + (iconSize - label.y) * .5f}, ImGui::GetColorU32(Paper), name);
    ImGui::PopFont();
}

void DrawToolHeading(const char* title)
{
    const float em = ImGui::GetFontSize();
    const float height = 1.95f * em;
    const auto start = ImGui::GetCursorScreenPos();
    const float right = start.x + ImGui::GetContentRegionAvail().x;
    DrawBrand(height);
    const float divider = ImGui::GetItemRectMax().x + .9f * em;
    const float left = divider + .9f * em;
    ImGui::PushFont(HeadingFont(), 1.48f * ImGui::GetStyle().FontSizeBase);
    const auto label = ImGui::CalcTextSize(title);
    if (left + label.x <= right)
    {
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddLine({divider, start.y + .2f * em}, {divider, start.y + height - .2f * em}, ImGui::GetColorU32(Border));
        draw->AddText({left, start.y + (height - label.y) * .5f}, ImGui::GetColorU32(Paper), title);
    }
    ImGui::PopFont();
}
}
