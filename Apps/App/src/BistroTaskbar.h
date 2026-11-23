#pragma once

#include "../Resources/Resource.h"
#include <shobjidl.h>
#include <wrl/client.h>

class BistroTaskbar
{
public:
    enum class State { Idle, Running, Completed, Failed };

    void OnMessage(HWND hwnd, UINT msg, WPARAM wParam)
    {
        if (msg == WM_CREATE)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(
                Icon(IDI_PEPENGINE2, GetSystemMetrics(SM_CXICON))));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(
                Icon(IDI_PEPENGINE2, GetSystemMetrics(SM_CXSMICON))));
        }
        else if (_created && msg == _created)
        {
            _list.Reset();
            if ((SUCCEEDED(_apartment) || _apartment == RPC_E_CHANGED_MODE) &&
                SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(_list.GetAddressOf()))) && SUCCEEDED(_list->HrInit()))
                Apply(hwnd);
            else _list.Reset();
        }
        else if (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE &&
            (_state == State::Completed || _state == State::Failed))
            Set(hwnd, State::Idle);
    }

    void Set(HWND hwnd, State state, int percent = -1)
    {
        if ((state == State::Completed || state == State::Failed) && GetForegroundWindow() == hwnd)
            state = State::Idle;
        if (state == _state && percent == _percent) return;
        const bool valueOnly = state == State::Running && state == _state && percent >= 0 && _percent >= 0;
        _state = state;
        _percent = percent;
        if (!_list) return;
        if (valueOnly) _list->SetProgressValue(hwnd, percent, 100);
        else Apply(hwnd);
    }

    ~BistroTaskbar()
    {
        _list.Reset();
        if (SUCCEEDED(_apartment)) CoUninitialize();
    }

private:
    static HICON Icon(int resource, int size)
    {
        return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource),
            IMAGE_ICON, size, size, LR_SHARED));
    }

    void Apply(HWND hwnd)
    {
        const bool running = _state == State::Running;
        _list->SetProgressState(hwnd, running ? (_percent < 0 ? TBPF_INDETERMINATE : TBPF_NORMAL) : TBPF_NOPROGRESS);
        if (running && _percent >= 0) _list->SetProgressValue(hwnd, _percent, 100);
        const bool done = _state == State::Completed;
        const bool failed = _state == State::Failed;
        _list->SetOverlayIcon(hwnd, done || failed ? Icon(done ? IDI_BENCHMARK_COMPLETED :
            IDI_BENCHMARK_FAILED, GetSystemMetrics(SM_CXSMICON)) : nullptr,
            done ? L"Benchmark completed" : failed ? L"Benchmark failed" : L"");
    }

    const UINT _created = RegisterWindowMessageW(L"TaskbarButtonCreated");
    const HRESULT _apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<ITaskbarList3> _list;
    State _state = State::Idle;
    int _percent = -1;
};
