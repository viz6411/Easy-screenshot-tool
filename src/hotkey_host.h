// hotkey_host.h - dedicated hidden window that owns the global hotkeys and their message loop.
// Why: Editor::run() pumps its own GetMessageW loop; hotkeys registered on nullptr would be
// dispatched by that nested loop instead of the main one, so a capture could trigger the editor
// recursively. A dedicated window with GWLP_HWNDPARENT cleared keeps hotkey dispatch isolated.
#pragma once
#include <windows.h>
#include <vector>

class HotkeyHost {
public:
    // callbacks receive the id passed to RegisterHotKey
    using Handler = void(*)(int id);

    bool create(Handler h) {
        handler_ = h;
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &HotkeyHost::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ShotMvpHotkeyHost";
        static bool registered = false;
        if (!registered) { RegisterClassW(&wc); registered = true; }

        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"ShotMvp hotkeys", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
        return hwnd_ != nullptr;
    }

    bool register_key(int id, UINT mods, UINT vk) {
        if (!hwnd_) return false;
        return RegisterHotKey(hwnd_, id, mods, vk) != FALSE;
    }

    void unregister_all() {
        if (!hwnd_) return;
        for (int id : ids_) UnregisterHotKey(hwnd_, id);
        ids_.clear();
    }

    // Pump hotkeys until quit_ is set (by WM_QUIT or explicit stop()).
    int pump() {
        MSG msg;
        while (!quit_ && GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.hwnd == hwnd_ && msg.message == WM_HOTKEY) {
                if (handler_) handler_((int)msg.wParam);
                continue;
            }
            if (msg.message == WM_QUIT) { quit_ = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

    void stop() { quit_ = true; if (hwnd_) PostMessageW(hwnd_, WM_NULL, 0, 0); }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        HotkeyHost* self = nullptr;
        if (m == WM_NCCREATE) {
            self = (HotkeyHost*)((CREATESTRUCTW*)l)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            return TRUE;
        }
        self = (HotkeyHost*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, m, w, l);
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
    }

    Handler handler_ = nullptr;
    HWND hwnd_ = nullptr;
    bool quit_ = false;
    std::vector<int> ids_;
};

