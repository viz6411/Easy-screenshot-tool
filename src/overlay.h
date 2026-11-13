// overlay.h - full-screen region selection (translucent mask + draggable rect)
#pragma once
#include <windows.h>
#include <vector>
#include "capture.h"

struct Selection { int x = 0, y = 0, w = 0, h = 0; bool ok = false; };

class Overlay {
public:
    // Captures the screen first, then shows a frozen screenshot with a dimmed mask,
    // so the user picks a region on an image that cannot change underneath.
    Selection run() {
        sel_ = {};
        if (!capture_screen(base_)) return sel_;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = &Overlay::wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = L"ShotMvpOverlay";
        static bool registered = false;
        if (!registered) { RegisterClassW(&wc); registered = true; }

        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName, L"ShotMvp selection",
                                WS_POPUP, 0, 0, sw, sh, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) return sel_;

        paint_mask();                       // blit base + dim mask into a bitmap
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);

        MSG msg;
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        // Drain any WM_QUIT left in the thread queue: the editor runs its own nested loop
        // immediately after us, and a queued quit would close it before it ever appears.
        MSG drain;
        while (PeekMessageW(&drain, nullptr, WM_QUIT, WM_QUIT, TRUE)) { }
        return sel_;
    }

    void on_mouse(UINT msg, WPARAM wp, LPARAM lp) {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        if (msg == WM_LBUTTONDOWN) {
            dragging_ = true; anchor_ = {mx, my}; start_ = {mx, my}; rect_ = {};
        } else if (msg == WM_MOUSEMOVE && dragging_) {
            rect_ = norm(anchor_.x, anchor_.y, mx, my);
            sel_ = {rect_.l, rect_.t, rect_.r - rect_.l, rect_.b - rect_.t, true};
            paint_mask();
        } else if (msg == WM_LBUTTONUP && dragging_) {
            dragging_ = false;
            rect_ = norm(anchor_.x, anchor_.y, mx, my);
            if (rect_.r - rect_.l >= 4 && rect_.b - rect_.t >= 4) {
                sel_ = {rect_.l, rect_.t, rect_.r - rect_.l, rect_.b - rect_.t, true};
                finish();
            }
        } else if (msg == WM_RBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            if (msg == WM_RBUTTONDOWN) { done_ = true; PostQuitMessage(0); }
            else if (sel_.ok) finish();
        }
    }

    void on_key(WPARAM vk) {
        if (vk == VK_ESCAPE) { sel_ = {}; sel_.ok = false; done_ = true; PostQuitMessage(0); }
        else if (vk == VK_RETURN) { if (sel_.ok) finish(); }
    }

private:
    struct NRect { int l = 0, t = 0, r = 0, b = 0; };

    static NRect norm(int x0, int y0, int x1, int y1) {
        NRect r;
        r.l = (std::min)(x0, x1); r.t = (std::min)(y0, y1);
        r.r = (std::max)(x0, x1); r.b = (std::max)(y0, y1);
        return r;
    }

    void finish() { done_ = true; PostQuitMessage(0); }

    static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Overlay* self = nullptr;
        if (m == WM_NCCREATE) {
            auto cs = (CREATESTRUCTW*)l;
            self = (Overlay*)cs->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            return TRUE;
        }
        self = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
            case WM_ERASEBKGND: return 1;
            case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_LBUTTONDBLCLK:
                self->on_mouse(m, w, l); return 0;
            case WM_KEYDOWN: self->on_key(w); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void paint_mask() {
        int w = base_.w, h = base_.h;
        std::vector<uint8_t> img = base_.rgba;   // copy of the frozen screen

        // Dim everything outside the chosen rect.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                bool inside = rect_.r > rect_.l && x >= rect_.l && x < rect_.r &&
                              y >= rect_.t && y < rect_.b;
                if (!inside) {
                    uint8_t* p = base_.px(x, y);
                    (void)p;
                    uint8_t* d = &img[((size_t)y * w + x) * 4];
                    d[0] = (uint8_t)(d[0] / 3); d[1] = (uint8_t)(d[1] / 3); d[2] = (uint8_t)(d[2] / 3);
                }
            }
        }

        // Draw selection border + corner handles in the product blue.
        if (rect_.r > rect_.l) {
            draw_rect(img, w, h, rect_.l, rect_.t, rect_.r - 1, rect_.b - 1, 0x1F78D7, 2);
            int hs = 4;
            auto handle = [&](int cx, int cy) {
                draw_rect(img, w, h, cx - hs, cy - hs, cx + hs, cy + hs, 0xFFFFFF, 2);
            };
            handle(rect_.l, rect_.t); handle(rect_.r - 1, rect_.t);
            handle(rect_.l, rect_.b - 1); handle(rect_.r - 1, rect_.b - 1);

            // size label near the rect
            wchar_t lbl[64];
            wsprintfW(lbl, L"%d x %d", rect_.r - rect_.l, rect_.b - rect_.t);
            draw_text(img, w, h, rect_.l + 6, (rect_.t > 22 ? rect_.t - 20 : rect_.b + 4), lbl, 0xFFFFFF);
        }

        BITMAPV5HEADER bi = {};
        bi.bV5Size = sizeof(BITMAPV5HEADER);
        bi.bV5Width = w; bi.bV5Height = -h;
        bi.bV5Planes = 1; bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask   = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask  = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        HDC sdc = GetDC(nullptr);
        HDC mdc = CreateCompatibleDC(sdc);
        uint8_t* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(sdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (hbmp && bits && mdc) {
            // RGBA -> BGRA for the DIB section
            for (size_t i = 0; i + 3 < img.size(); i += 4) {
                bits[i]     = img[i + 2];
                bits[i + 1] = img[i + 1];
                bits[i + 2] = img[i];
                bits[i + 3] = 255;         // fully opaque: the mask is baked into the pixels
            }
            HBITMAP old = (HBITMAP)SelectObject(mdc, hbmp);

            POINT ptSrc = {0, 0}, ptDst = {0, 0};
            SIZE sz = {w, h};
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            HDC hdc = GetDC(hwnd_);
            UpdateLayeredWindow(hwnd_, hdc, &ptDst, &sz, mdc, &ptSrc, 0, &bf, ULW_ALPHA);
            ReleaseDC(hwnd_, hdc);
            SelectObject(mdc, old);
        }
        if (hbmp) DeleteObject(hbmp);
        if (mdc) DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
    }

    static void draw_rect(std::vector<uint8_t>& img, int W, int H, int x0, int y0, int x1, int y1,
                          uint32_t rgb, int thick) {
        auto put = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= W || y >= H) return;
            uint8_t* d = &img[((size_t)y * W + x) * 4];
            d[0] = (uint8_t)((rgb >> 16) & 0xFF);
            d[1] = (uint8_t)((rgb >> 8) & 0xFF);
            d[2] = (uint8_t)(rgb & 0xFF);
            d[3] = 255;
        };
        for (int t = 0; t < thick; ++t) {
            for (int x = x0; x <= x1; ++x) { put(x, y0 + t); put(x, y1 - t); }
            for (int y = y0; y <= y1; ++y) { put(x0 + t, y); put(x1 - t, y); }
        }
    }

    // Tiny built-in 5x7 bitmap font so the overlay needs no font plumbing.
    static void draw_text(std::vector<uint8_t>& img, int W, int H, int x, int y, const wchar_t* s, uint32_t rgb) {
        auto glyph = [](wchar_t c, int col, int row) -> bool {
            struct G { wchar_t ch; uint8_t rows[7]; };
            static const G table[] = {
                {L'0',{0x7C,0x82,0x92,0x92,0x82,0x7C,0}}, {L'1',{0x10,0x30,0x10,0x10,0x10,0x38,0}},
                {L'2',{0x7C,0x02,0x7C,0x40,0x40,0x7E,0}}, {L'3',{0x7C,0x02,0x1C,0x02,0x02,0x7C,0}},
                {L'4',{0x0C,0x14,0x24,0x7E,0x04,0x04,0}}, {L'5',{0x7E,0x40,0x7C,0x02,0x02,0x7C,0}},
                {L'6',{0x7C,0x40,0x7C,0x82,0x82,0x7C,0}}, {L'7',{0x7E,0x02,0x04,0x08,0x10,0x10,0}},
                {L'8',{0x7C,0x82,0x7C,0x82,0x82,0x7C,0}}, {L'9',{0x7C,0x82,0x82,0x7C,0x02,0x7C,0}},
                {L'x',{0,0x44,0x28,0x10,0x28,0x44,0}},   {L' ',{0,0,0,0,0,0,0}},
            };
            for (auto& g : table) if (g.ch == c) return (g.rows[row] >> col) & 1;
            return false;
        };
        int cx = x;
        for (const wchar_t* p = s; *p; ++p) {
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (glyph(*p, col, row)) draw_rect(img, W, H, cx + col, y + row, cx + col, y + row, rgb, 1);
            cx += 7;
        }
    }

    HWND hwnd_ = nullptr;
    bool done_ = false;
    bool dragging_ = false;
    POINT anchor_{0,0}, start_{0,0};
    NRect rect_{};
    Selection sel_{};
    DIB base_;
};

