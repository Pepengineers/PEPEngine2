#pragma once

#include <windows.h>

inline RECT VisibleClientOnMonitor(HWND window, HMONITOR preferred = nullptr)
{
    RECT bounds{};
    if (!GetClientRect(window, &bounds) || IsRectEmpty(&bounds)) return {};
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    MONITORINFO monitor{sizeof(monitor)};
    if ((preferred && GetMonitorInfoW(preferred, &monitor)) ||
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        IntersectRect(&bounds, &bounds, &monitor.rcWork);
    return bounds;
}
