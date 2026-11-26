#include "BistroLoadingScreen.h"
#include "../../../Resources/Resource.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
using namespace Gdiplus;

constexpr wchar_t WindowClass[] = L"PEPENGINE2.Bistro.Loading";
constexpr std::array<const wchar_t*, 4> Phases{L"Renderer", L"Scene assets", L"World", L"First frame"};
const Color Pond(16, 27, 26), Leaf(181, 237, 125), Mint(96, 218, 180), White(242, 248, 238);
const Color Muted(172, 193, 180), Track(51, 72, 64);

void Text(Graphics& graphics, const std::wstring& value, RectF area, float size, Color ink,
    bool bold = false, bool right = false)
{
    const Font font(L"Segoe UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    const SolidBrush brush(ink);
    StringFormat format;
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    if (right) format.SetAlignment(StringAlignmentFar);
    graphics.DrawString(value.c_str(), -1, &font, area, &format, &brush);
}

struct Layout
{
    Layout(int width, int height) : W(float(width)), H(float(height)),
        U(std::max(.3f, W < 1.23f * H ? std::min(W / 510.f, H / 660.f) : std::min(W / 960.f, H / 540.f))),
        Pad(38 * U), Inner(W - 2 * Pad), Stacked(W < 1.23f * H) {}

    RectF Art() const
    {
        if (Stacked) return {Pad, .12f * H, Inner, .30f * H};
        return {.52f * W, .12f * H, .48f * W - Pad, .60f * H};
    }
    float TitleY() const { return H * (Stacked ? .44f : .24f); }
    float DetailY() const { return H * (Stacked ? .56f : .49f); }
    float CopyWidth() const { return Stacked ? Inner : .49f * W - Pad; }
    RectF CancelButton() const { return {W - Pad - 110 * U, H - Pad - 32 * U, 110 * U, 32 * U}; }
    float W, H, U, Pad, Inner;
    bool Stacked;
};

// Keep the scaled illustration and all static type in a device-compatible bitmap.
struct Surface
{
    Surface(HDC source, int width, int height)
    {
        Dc = CreateCompatibleDC(source);
        Bitmap = CreateCompatibleBitmap(source, width, height);
        if (Dc && Bitmap) Previous = SelectObject(Dc, Bitmap);
    }
    ~Surface()
    {
        if (Previous) SelectObject(Dc, Previous);
        if (Bitmap) DeleteObject(Bitmap);
        if (Dc) DeleteDC(Dc);
    }
    HDC Dc{};
    HBITMAP Bitmap{};
    HGDIOBJ Previous{};
};

void WaitWithSentMessages(DWORD count, const HANDLE* handles)
{
    // Creating or destroying an owned HWND can send messages to the owner on another thread.
    while (MsgWaitForMultipleObjectsEx(count, handles, INFINITE, QS_SENDMESSAGE, 0) == WAIT_OBJECT_0 + count)
    {
        MSG message;
        PeekMessageW(&message, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE);
    }
}
}

struct BistroLoadingScreen::Impl
{
    struct Progress
    {
        unsigned Phase = 0;
        std::wstring Detail;
        size_t Completed = 0, Total = 0;
    };

    Impl(HWND owner, const std::wstring& map) : Owner(owner), Title(L"Loading " + map)
    {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        try { Worker = std::thread([this, ready] { Run(ready); }); }
        catch (...) { if (ready) CloseHandle(ready); throw; }
        if (ready)
        {
            // Show the first frame before the render thread starts building D3D resources.
            HANDLE handles[]{ready, Worker.native_handle()};
            WaitWithSentMessages(2, handles);
            CloseHandle(ready);
        }
    }
    ~Impl()
    {
        Stopping = true;
        HANDLE thread = Worker.native_handle();
        PostThreadMessageW(GetThreadId(thread), WM_NULL, 0, 0);
        WaitWithSentMessages(1, &thread);
        Worker.join();
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            self->PaintClient(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            self->PaintClient(window, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP:
        {
            RECT rect{};
            GetClientRect(window, &rect);
            if (Layout(rect.right, rect.bottom).CancelButton().Contains(float(short(LOWORD(lp))), float(short(HIWORD(lp)))))
                self->CancelRequested = true;
            return 0;
        }
        case WM_CLOSE:
            self->CancelRequested = true;
            PostMessageW(self->Owner, WM_CLOSE, 0, 0);
            return 0;
        default: return DefWindowProcW(window, message, wp, lp);
        }
    }

    void ReadArtwork()
    {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_PEPE_LOADING), RT_RCDATA);
        if (!resource) return;
        const DWORD length = SizeofResource(module, resource);
        const auto* bytes = static_cast<const BYTE*>(LockResource(LoadResource(module, resource)));
        if (!length || !bytes) return;
        Stream.Attach(SHCreateMemStream(bytes, length));
        if (!Stream) return;
        std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(Stream.Get()));
        if (decoded && decoded->GetLastStatus() == Ok && decoded->GetWidth() && decoded->GetHeight())
        {
            Artwork = std::move(decoded);
            Background.reset();
        }
    }

    void DrawBackdrop(HDC dc, const Layout& l)
    {
        Graphics g(dc);
        g.SetPageUnit(UnitPixel);
        g.Clear(Pond);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const float u = l.U;
        const auto art = l.Art();
        const float diameter = std::min(art.Width, art.Height) * .94f;
        const float x = art.X + (art.Width - diameter) * .5f, y = art.Y + (art.Height - diameter) * .5f;
        const SolidBrush wash(Color(25, 45, 38));
        const Pen rim(Color(45, 72, 57), std::max(1.f, u));
        g.FillEllipse(&wash, x, y, diameter, diameter);
        g.DrawEllipse(&rim, x, y, diameter, diameter);
        if (Artwork)
        {
            const float w = float(Artwork->GetWidth()), h = float(Artwork->GetHeight());
            const float fit = std::min(art.Width / w, art.Height / h);
            const RectF dest(art.X + (art.Width - w * fit) * .5f, art.Y + (art.Height - h * fit) * .5f, w * fit, h * fit);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.DrawImage(Artwork.get(), dest, 0.f, 0.f, w, h, UnitPixel);
        }
        else
        {
            const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_PEPENGINE2),
                IMAGE_ICON, 64, 64, LR_SHARED));
            if (icon)
            {
                const int d = std::max(1, int(diameter * .53f));
                HDC iconDc = g.GetHDC();
                if (iconDc)
                {
                    DrawIconEx(iconDc, int(x + (diameter - d) / 2), int(y + (diameter - d) / 2), icon, d, d, 0, nullptr, DI_NORMAL);
                    g.ReleaseHDC(iconDc);
                }
            }
        }
        Text(g, L"PEPENGINE2", {l.Pad, l.Pad * .78f, l.Inner, 32 * u}, 22 * u, Leaf, true);
        const Pen rule(Track, std::max(1.f, u));
        g.DrawLine(&rule, l.Pad, l.Pad + 33 * u, l.W - l.Pad, l.Pad + 33 * u);
        Text(g, Title, {l.Pad, l.TitleY(), l.CopyWidth(), l.DetailY() - l.TitleY() - 4 * u}, 48 * u, White, true);
    }

    void DrawProgress(HDC dc, const Layout& l)
    {
        Progress state;
        {
            const std::lock_guard lock(Mutex);
            state = Current;
        }
        Graphics g(dc);
        g.SetPageUnit(UnitPixel);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const float u = l.U, y = l.DetailY();
        const ULONGLONG elapsed = GetTickCount64() - Started;
        const bool cancelling = CancelRequested.load();
        Text(g, cancelling ? L"Cancelling" : Phases[state.Phase], {l.Pad, y, l.CopyWidth(), 34 * u}, 24 * u, Mint, true);
        const std::wstring detail = cancelling ? L"Finishing the current operation." : state.Detail;
        const float barY = l.H * .78f, gap = 5 * u;
        const float localY = std::min(y + 93 * u, barY - 25 * u);
        const bool counted = state.Total && !cancelling;
        const float detailEnd = counted ? localY - 14 * u : barY - 8 * u;
        Text(g, detail, {l.Pad, y + 36 * u, l.CopyWidth(), detailEnd - y - 36 * u}, 16 * u, White);
        const SolidBrush track(Track), complete(Mint), active(Color(72, 95, 65)), current(Leaf);
        if (counted)
        {
            const std::wstring count = std::to_wstring(state.Completed) + L" / " + std::to_wstring(state.Total);
            const float numberWidth = (6.f + 10.f * count.size()) * u;
            const float width = std::max(1.f, l.CopyWidth() - numberWidth - 12 * u);
            g.FillRectangle(&track, l.Pad, localY, width, 4 * u);
            g.FillRectangle(&complete, l.Pad, localY, width * float(std::min(state.Completed, state.Total)) / float(state.Total), 4 * u);
            Text(g, count, {l.Pad + width + 8 * u, localY - 11 * u, numberWidth + 4 * u, 25 * u}, 15 * u, Mint, true, true);
        }
        const float segment = (l.Inner - (Phases.size() - 1) * gap) / Phases.size();
        for (unsigned i = 0; i < Phases.size(); ++i)
        {
            const float x = l.Pad + i * (segment + gap);
            g.FillRectangle(i < state.Phase ? &complete : i == state.Phase ? &active : &track, x, barY, segment, 5 * u);
            if (i == state.Phase)
            {
                const float pulse = cancelling ? .5f : .5f - .5f * std::cos(float(elapsed % 1800) * .00349066f);
                g.FillRectangle(&current, x + pulse * segment * .75f, barY, segment * .25f, 5 * u);
            }
            Text(g, Phases[i], {x, barY + 10 * u, segment, 25 * u}, 14 * u, i == state.Phase ? Leaf : Muted, i == state.Phase);
        }
        const auto cancelBox = l.CancelButton();
        const auto seconds = elapsed / 1000;
        wchar_t clock[48];
        swprintf_s(clock, L"%llu:%02llu elapsed", seconds / 60, seconds % 60);
        Text(g, clock, {l.Pad, cancelBox.Y + 4 * u, l.Inner - cancelBox.Width, 28 * u}, 16 * u, Muted);
        if (!cancelling)
        {
            const Pen border(Track, std::max(1.f, u));
            g.DrawRectangle(&border, cancelBox);
            Text(g, L"Esc   Cancel", {cancelBox.X + 9 * u, cancelBox.Y + 5 * u, cancelBox.Width - 12 * u, 25 * u}, 15 * u, White);
        }
    }

    void PaintClient(HWND window, HDC dc)
    {
        RECT bounds{};
        if (!dc || !GetClientRect(window, &bounds) || IsRectEmpty(&bounds)) return;
        const int width = bounds.right, height = bounds.bottom;
        const Layout layout(width, height);
        if (!Background || Size.cx != width || Size.cy != height)
        {
            Size = {width, height};
            Background = std::make_unique<Surface>(dc, width, height);
            Frame = std::make_unique<Surface>(dc, width, height);
            if (Background->Previous) DrawBackdrop(Background->Dc, layout);
        }
        HDC target = Frame->Previous ? Frame->Dc : dc;
        if (Background->Previous) BitBlt(target, 0, 0, width, height, Background->Dc, 0, 0, SRCCOPY);
        else DrawBackdrop(target, layout);
        DrawProgress(target, layout);
        if (target != dc) BitBlt(dc, 0, 0, width, height, target, 0, 0, SRCCOPY);
    }

    void Follow(HWND window)
    {
        RECT bounds{};
        GetClientRect(Owner, &bounds);
        if (!IsWindowVisible(Owner) || IsIconic(Owner) || IsRectEmpty(&bounds))
        {
            if (IsWindowVisible(window)) ShowWindow(window, SW_HIDE);
            return;
        }
        MapWindowPoints(Owner, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
        RECT previous{};
        GetWindowRect(window, &previous);
        if (!EqualRect(&bounds, &previous) || !IsWindowVisible(window))
            SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    void Run(HANDLE ready)
    {
        SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(Owner));
        const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        GdiplusStartupInput input;
        ULONG_PTR token{};
        if (GdiplusStartup(&token, &input, nullptr) == Ok)
        {
            WNDCLASSW cls{};
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpfnWndProc = Proc;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.lpszClassName = WindowClass;
            RegisterClassW(&cls);
            HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WindowClass, Title.c_str(), WS_POPUP,
                0, 0, 0, 0, Owner, nullptr, cls.hInstance, this);
            if (window)
            {
                Follow(window); // Paint immediately, before PNG decoding or resampling.
                if (ready) SetEvent(ready);
                ReadArtwork();
                ULONGLONG nextPaint = 0;
                while (!Stopping.load() && IsWindow(Owner) && IsWindow(window))
                {
                    MSG msg{};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                    if (Stopping.load()) break;
                    if (GetTickCount64() >= nextPaint)
                    {
                        Follow(window);
                        nextPaint = GetTickCount64() + 80;
                    }
                    const auto now = GetTickCount64();
                    MsgWaitForMultipleObjectsEx(0, nullptr, now < nextPaint ? DWORD(nextPaint - now) : 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
                DestroyWindow(window);
            }
            Frame.reset();
            Background.reset();
            Artwork.reset();
            Stream.Reset();
            GdiplusShutdown(token);
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    }

    HWND Owner;
    const std::wstring Title;
    const ULONGLONG Started = GetTickCount64();
    std::mutex Mutex;
    Progress Current;
    std::atomic<bool> Stopping{false}, CancelRequested{false};
    Microsoft::WRL::ComPtr<IStream> Stream;
    std::unique_ptr<Bitmap> Artwork;
    std::unique_ptr<Surface> Background, Frame;
    SIZE Size{};
    std::thread Worker;
};

BistroLoadingScreen::BistroLoadingScreen(HWND owner, const std::wstring& mapName)
    : _impl(std::make_unique<Impl>(owner, mapName)) {}

BistroLoadingScreen::~BistroLoadingScreen() = default;

void BistroLoadingScreen::Update(Phase phase, const std::wstring& detail, size_t completed, size_t total)
{
    const std::lock_guard lock(_impl->Mutex);
    _impl->Current = {phase, detail, completed, total};
}

bool BistroLoadingScreen::Cancelled() const { return _impl->CancelRequested.load(); }
void BistroLoadingScreen::RequestCancel() { _impl->CancelRequested = true; }
