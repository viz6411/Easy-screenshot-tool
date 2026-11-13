// dpi.h - make the process per-monitor DPI aware as early as possible.
// Why: on this machine the panel is 2560x1600 at 175% scaling (DPI 168). A non-aware process
// gets virtualized coordinates (1463x914) and Windows bitmap-stretches its windows, which made
// captures come out zoomed-in with the bottom-right of the screen cut off.
#pragma once
#include <windows.h>

// Diagnostics so the test harness can prove which path succeeded instead of guessing:
// method 0 = nothing worked, 1 = per-monitor v2 context, 2 = shcore, 3 = system aware.
inline int   g_dpi_method = 0;
inline DWORD g_dpi_err1 = 0, g_dpi_err2 = 0, g_dpi_err3 = 0;

inline void enable_dpi_awareness() {
    // Preferred path: per-monitor awareness v2 (Windows 10 1703+).
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using FnCtx = BOOL(WINAPI*)(HANDLE);
        auto setCtx = (FnCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (setCtx) {
            if (setCtx((HANDLE)-4)) { g_dpi_method = 1; return; }
            g_dpi_err1 = GetLastError();
        } else {
            g_dpi_err1 = 127;   // export not present
        }
    } else {
        g_dpi_err1 = 127;
    }
    // Fallback: shcore (Windows 8.1+). PROCESS_PER_MONITOR_DPI_AWARE = 2
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using FnSh = HRESULT(WINAPI*)(int);
        auto setSh = (FnSh)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (setSh) {
            HRESULT hr = setSh(2);
            if (SUCCEEDED(hr)) { g_dpi_method = 2; return; }
            g_dpi_err2 = (DWORD)hr;
        } else {
            g_dpi_err2 = 127;
        }
    }
    // Last resort: system-wide awareness.
    if (SetProcessDPIAware()) g_dpi_method = 3;
    else g_dpi_err3 = GetLastError();
}

// DPI of the monitor containing a point (falls back to 96).
inline UINT dpi_for_monitor(HMONITOR mon) {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto f = (Fn)GetProcAddress(shcore, "GetDpiForMonitor");
        UINT x = 96, y = 96;
        // MDT_EFFECTIVE_DPI = 0
        if (f && SUCCEEDED(f(mon, 0, &x, &y)) && x) return x;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using Fn = UINT(WINAPI*)(void);
        auto f = (Fn)GetProcAddress(user32, "GetDpiForSystem");
        if (f) { UINT d = f(); return d ? d : 96; }
    }
    return 96;
}

// The actual DPI of the monitor under the cursor's general area (96, 120, 144, 168, ...).
// theme.h expects a DPI value, not a ratio; use this one when loading the theme.
inline float dpi_value() {
    HMONITOR mon = MonitorFromPoint(POINT{ 1, 1 }, 2 /*MONITOR_DEFAULTTONEAREST*/);
    UINT d = dpi_for_monitor(mon);
    return d ? (float)d : 96.0f;
}

inline double dpi_scale() {
    HMONITOR mon = MonitorFromPoint(POINT{ 1, 1 }, 2 /*MONITOR_DEFAULTTONEAREST*/);
    UINT d = dpi_for_monitor(mon);
    double s = (double)d / 96.0;
    return s > 0.0 ? s : 1.0;
}

// Report the awareness the process actually ended up with, so a test can prove the fix took.
inline int dpi_awareness_level() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return -1;
    // GetWindowDpiAwarenessContext needs a window; use the desktop window as a proxy for the process.
    using FnCtx = HANDLE(WINAPI*)(HWND);
    using FnVal = int(WINAPI*)(HANDLE);
    auto getCtx = (FnCtx)GetProcAddress(user32, "GetWindowDpiAwarenessContext");
    auto getVal = (FnVal)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
    if (!getCtx || !getVal) return -1;
    HANDLE ctx = getCtx(GetDesktopWindow());
    if (!ctx) return -1;
    return getVal(ctx);   // 0=UNAWARE 1=SYSTEM_AWARE 2=PER_MONITOR 3=DPI_UNCHANGED 4=PMv2
}
