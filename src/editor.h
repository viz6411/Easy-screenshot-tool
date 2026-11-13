// editor.h - ShotMvp annotation editor.
//
// Rendering is Direct2D + DirectWrite driven by the tokens in theme.h and the drawing layer in
// ui_draw.h. Nothing is painted with raw GDI text onto a blitted canvas any more: that was how
// Undo/Redo/Save ended up invisible in the first MVP (the canvas blit painted over the text).
//
// Layout model: one floating pill toolbar (CleanShot-like) centred at the top of the client
// area, a status strip at the bottom, and the captured image in between. Every control is a
// Slot produced by layout(); the painter and the hit-tester consume the same vector, so what
// you click is exactly what you see.
#pragma once
#include <windows.h>
#include <cstdio>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <memory>

#include "capture.h"
#include "theme.h"
#include "ui_draw.h"
#include "dpi.h"

enum Tool { TOOL_ARROW = 0, TOOL_TEXT, TOOL_RECT, TOOL_ELLIPSE, TOOL_MARKER, TOOL_CROP };

struct Stroke {
    Tool tool;
    std::vector<POINT> pts;
    COLORREF color;
    int width;
    std::wstring text;
};

// Startup trail for the test harness, enabled with SHOTMVP_DEBUG=1. It writes to a file
// because the interesting failures happen before any console output would be flushed.
static void dbg(const char* msg, HRESULT hr = 0) {
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("SHOTMVP_DEBUG", nullptr, 0) ? 1 : 0;
    if (!on) return;
    FILE* f = fopen("dbg_editor.log", "a");
    if (!f) return;
    fprintf(f, "%-30s hr=0x%08X gle=%lu\n", msg, (unsigned)hr, (unsigned long)GetLastError());
    fclose(f);
}

class Editor {
public:
    // ---- public API ---------------------------------------------------------------------
    bool run(DIB img) {
        dbg("run: enter");
        img_ = std::move(img);
        if (img_.w <= 0 || img_.h <= 0) { dbg("run: empty image"); return false; }

        WNDCLASSW wc = {};
        wc.lpfnWndProc = &Editor::wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"ShotMvpEditorV2";
        static bool registered = false;
        if (!registered) { RegisterClassW(&wc); registered = true; }

        // Theme::load wants a DPI value (168 here), not the 1.75 ratio. Handing it the ratio
        // is the bug that once collapsed every control to a couple of pixels.
        theme_ = Theme::load(dpi_value());
        apply_metric_padding();

        // The requested size is the OUTER size (AdjustWindowRectEx), so the client area really
        // is "pill + gap + image + gap + status strip" and nothing gets cut off.
        RECT fr{ 0, 0, 1000, 1000 };
        AdjustWindowRectEx(&fr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        int frameW = fr.right - fr.left - 1000;
        int frameH = fr.bottom - fr.top - 1000;

        build_slots();                        // sizes the pill from the slots that will be drawn
        int pillH = (int)pill_h_;
        int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);

        int wantClientW = (std::max)(img_.w + PAD * 2, pill_w_ + theme_.s24);
        int wantClientH = theme_.s12 + pillH + PAD_TOP + img_.h + PAD_BOTTOM + FOOT_H;

        int w = (std::min)(wantClientW + frameW, scrW - 8);
        int h = (std::min)(wantClientH + frameH, scrH - 8);
        if (w < 420 + frameW) w = (std::min)(420 + frameW, scrW - 8);
        if (h < 300 + frameH) h = (std::min)(300 + frameH, scrH - 8);

        int px = (std::max)(0, (scrW - w) / 2), py = (std::max)(0, (scrH - h) / 2);

        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"ShotMvp", WS_OVERLAPPEDWINDOW,
                                px, py, w, h, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) { dbg("run: CreateWindowExW failed"); return false; }

        if (!init_d2d()) {
            dbg("run: init_d2d failed");
            wprintf(L"Direct2D init failed - cannot paint the editor\n");
            DestroyWindow(hwnd_); hwnd_ = nullptr;
            return false;
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        // This process normally sits in the background (its hotkey host is a hidden window), and
        // Windows refuses SetForegroundWindow from a background process: the editor used to open
        // *behind* the app that was in focus. Attaching our input thread to the foreground
        // thread's input queue is the documented way round it.
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(hwnd_);
        if (!SetForegroundWindow(hwnd_)) {
            HWND fg = GetForegroundWindow();
            DWORD other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            if (other && other != GetCurrentThreadId()) AttachThreadInput(GetCurrentThreadId(), other, TRUE);
            SetForegroundWindow(hwnd_);
            BringWindowToTop(hwnd_);
            if (other && other != GetCurrentThreadId()) AttachThreadInput(GetCurrentThreadId(), other, FALSE);
        }
        dbg("run: window shown");

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        dbg("run: message loop exited");
        shutdown_d2d();
        DestroyWindow(hwnd_); hwnd_ = nullptr;
        return saved_;
    }

    // Bake committed strokes into the image so the saved PNG contains them (image-space px).
    DIB compose_result() {
        DIB out = img_;
        for (auto& s : undo_) draw_stroke_img(out, s);
        return out;
    }

    bool saved() const { return saved_; }
    bool copy_requested() const { return copy_requested_; }

private:
    // ---- slot model --------------------------------------------------------------------
    enum Kind { KIND_TOOL, KIND_COLOR, KIND_WIDTH, KIND_ACTION, KIND_SEP };

    struct Slot {
        Kind kind = KIND_SEP;
        int id = 0;              // index within its group
        Tool tool = TOOL_ARROW;
        COLORREF color = 0;
        int width = 0;
        int action = -1;         // 0 undo, 1 redo, 2 copy, 3 save
        int glyph = 0;
        float x = 0, y = 0, w = 0, h = 0;   // physical px in client space
    };

    int PAD = 16, PAD_TOP = 8, PAD_BOTTOM = 12, FOOT_H = 26;
    int pill_w_ = 0;
    float pill_x_ = 0, pill_y_ = 0, pill_h_ = 0;
    std::vector<Slot> slots_;

    void apply_metric_padding() {
        PAD = theme_.s16; PAD_TOP = theme_.s12; PAD_BOTTOM = theme_.s12;
        FOOT_H = theme_.s24;
    }

    // ---- messages ----------------------------------------------------------------------
    LRESULT on_msg(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        switch (m) {
            case WM_ERASEBKGND: return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps; BeginPaint(h, &ps);
                paint();
                EndPaint(h, &ps);
                return 0;
            }

            case WM_DPICHANGED: {
                const RECT* suggested = (const RECT*)lp;
                float newDpi = ((float)LOWORD(wp) * 256.0f + (float)HIWORD(wp)) / 256.0f;
                SetWindowPos(h, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                theme_ = Theme::load(newDpi);
                apply_metric_padding();
                cv_.shutdown();                       // text formats carry the old font sizes
                cv_.init(rt_, factory_, dwrite_, &theme_);
                layout();
                InvalidateRect(h, nullptr, TRUE);
                return 0;
            }

            case WM_SIZE: {
                int cw = (int)LOWORD(lp), ch = (int)HIWORD(lp);
                if (rt_ && cw > 0 && ch > 0) {
                    D2D1_SIZE_U sz = { (UINT32)cw, (UINT32)ch };
                    rt_->Resize(&sz);
                    cv_.clear_cache();                // glyph geometry depends on slot positions
                    layout();
                    InvalidateRect(h, nullptr, TRUE);
                }
                return 0;
            }

            case WM_LBUTTONDOWN: on_down((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
            case WM_MOUSEMOVE:  on_move((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
            case WM_LBUTTONUP:   on_up((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
            case WM_KEYDOWN:     on_key((int)wp); return 0;
            case WM_CHAR:        on_char((wchar_t)wp); return 0;

            case WM_GETMINMAXINFO: {
                MINMAXINFO* mmi = (MINMAXINFO*)lp;
                mmi->ptMinTrackSize.x = (LONG)(pill_w_ + theme_.s24 + 16);
                mmi->ptMinTrackSize.y = 240;
                return 0;
            }

            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, wp, lp);
    }

    static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        Editor* self = nullptr;
        if (m == WM_NCCREATE) {
            self = (Editor*)((CREATESTRUCTW*)lp)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            return TRUE;
        }
        self = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, m, wp, lp);
        return self->on_msg(h, m, wp, lp);
    }

    // ---- Direct2D setup ----------------------------------------------------------------
    bool init_d2d() {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
        if (FAILED(hr)) { dbg("init: D2D1CreateFactory", hr); return false; }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 (IUnknown**)&dwrite_);
        if (FAILED(hr)) { dbg("init: DWriteCreateFactory", hr); return false; }

        RECT cr; GetClientRect(hwnd_, &cr);
        D2D1_RENDER_TARGET_PROPERTIES rp = {};
        rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        rp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        rp.dpiX = 96.0f; rp.dpiY = 96.0f;            // sizes are pre-scaled by the theme
        rp.usage = D2D1_RENDER_TARGET_USAGE_NONE;

        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = {};
        hp.hwnd = hwnd_;
        hp.pixelSize.width = (UINT32)(cr.right - cr.left);
        hp.pixelSize.height = (UINT32)(cr.bottom - cr.top);
        hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

        hr = factory_->CreateHwndRenderTarget(&rp, &hp, &rt_);
        if (FAILED(hr)) { dbg("init: CreateHwndRenderTarget", hr); return false; }

        hr = cv_.init(rt_, factory_, dwrite_, &theme_);
        dbg("init: canvas resources", hr);
        if (!cv_.fmtCaption) dbg("init: text format missing");

        layout();
        refresh_image_bitmap();
        return rt_ != nullptr;
    }

    void shutdown_d2d() {
        cv_.shutdown();
        if (img_bitmap_) { img_bitmap_->Release(); img_bitmap_ = nullptr; }
        if (rt_) { rt_->Release(); rt_ = nullptr; }
        if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
        if (factory_) { factory_->Release(); factory_ = nullptr; }
    }

    void refresh_image_bitmap() {
        if (img_bitmap_) { img_bitmap_->Release(); img_bitmap_ = nullptr; }
        if (!rt_ || img_.w <= 0) return;

        // Our DIB is RGBA; the render target wants BGRA.
        bgra_.assign((size_t)img_.w * img_.h * 4, 0);
        for (int y = 0; y < img_.h; ++y) {
            const uint8_t* src = img_.px(0, y);
            uint8_t* dst = &bgra_[(size_t)y * img_.w * 4];
            for (int x = 0; x < img_.w; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
        D2D1_SIZE_U sz = { (UINT32)img_.w, (UINT32)img_.h };
        D2D1_BITMAP_PROPERTIES bp = {};
        bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bp.dpiX = 96.0f; bp.dpiY = 96.0f;
        HRESULT hr = rt_->CreateBitmap(sz, bgra_.data(), (UINT32)img_.w * 4, bp, &img_bitmap_);
        dbg(img_bitmap_ ? "init: image bitmap created" : "init: image bitmap FAILED", hr);
        img_version_++;
    }

    // ---- pill geometry --------------------------------------------------------------------
    // build_slots() is the only place the toolbar's structure exists. It lays the slots out
    // relative to x=0 and only then measures the run, so the pill can never be narrower than
    // what is drawn on it. The previous version reserved room for "three separators" while the
    // loop emitted four, and the Save icon ended up hanging outside the pill.
    void build_slots() {
        slots_.clear();
        const Theme& t = theme_;
        float x = 0, top = 0;
        const float csize = (float)t.ctlH;
        pill_h_ = (float)(t.ctlH + t.s6 * 2);

        auto push = [&](Kind kind, int id, int glyph, Tool tool, COLORREF col, int width, int action) {
            Slot s; s.kind = kind; s.id = id; s.glyph = glyph; s.tool = tool;
            s.color = col; s.width = width; s.action = action;
            s.x = x; s.y = top; s.w = csize; s.h = csize;
            slots_.push_back(s);
            x += csize + (float)t.s2;
        };
        auto sep = [&]() {
            x += t.s8;
            Slot s; s.kind = KIND_SEP; s.x = x - t.s4 - 1.0f; s.y = top;
            s.w = 1.0f; s.h = pill_h_ - t.s6 * 2;
            slots_.push_back(s);
            x += t.s8;
        };

        const int glyphs[6] = { ui::G_ARROW, ui::G_TEXT, ui::G_RECT, ui::G_ELLIPSE, ui::G_MARKER, ui::G_CROP };
        const Tool tools[6] = { TOOL_ARROW, TOOL_TEXT, TOOL_RECT, TOOL_ELLIPSE, TOOL_MARKER, TOOL_CROP };
        for (int i = 0; i < 6; ++i) {
            push(KIND_TOOL, i, glyphs[i], tools[i], 0, 0, -1);
            if (i == 3) sep();                     // shapes | freehand and crop
        }
        sep();                                     // tools | colours
        for (int i = 0; i < (int)palette_.size(); ++i) push(KIND_COLOR, i, -1, TOOL_ARROW, palette_[i], 0, -1);
        sep();                                     // colours | line widths
        for (int i = 0; i < (int)widths_.size(); ++i) push(KIND_WIDTH, i, -1, TOOL_ARROW, 0, widths_[i], -1);
        sep();                                     // widths | actions
        const int acts[4] = { ui::G_UNDO, ui::G_REDO, ui::G_COPY, ui::G_SAVE };
        for (int i = 0; i < 4; ++i) push(KIND_ACTION, i, acts[i], TOOL_ARROW, 0, 0, i);

        float right = 0;
        for (auto& s : slots_) right = (std::max)(right, s.x + s.w);
        pill_w_ = (int)(right + 2 * t.s12);        // the run plus inner padding on both sides
    }

    void layout() {
        if (!hwnd_) return;
        RECT cr; GetClientRect(hwnd_, &cr);
        int cw = cr.right - cr.left;
        const Theme& t = theme_;

        build_slots();
        if (pill_w_ > cw - 2 * t.s8) pill_w_ = cw - 2 * t.s8;   // never wider than the client
        pill_x_ = (float)((cw - pill_w_) / 2);
        pill_y_ = (float)t.s12;
        for (auto& s : slots_) { s.x += pill_x_ + t.s12; s.y += pill_y_ + t.s6; }
    }

    // ---- painting ----------------------------------------------------------------------
    void paint() {
        if (!rt_) return;
        RECT cr; GetClientRect(hwnd_, &cr);
        const float W = (float)(cr.right - cr.left), H = (float)(cr.bottom - cr.top);
        const Theme& t = theme_;

        rt_->BeginDraw();
        rt_->Clear(t.canvas);
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // 1. The captured pixels, centred under the pill, with a hairline outline so the
        //    edges of a screenshot read cleanly against the dark canvas.
        float imgX = (float)((int)((W - img_.w) / 2));
        if (imgX < t.s8) imgX = (float)t.s8;
        float imgY = pill_y_ + pill_h_ + (float)PAD_TOP;
        img_org_x_ = (int)imgX; img_org_y_ = (int)imgY;

        if (img_bitmap_) {
            D2D1_RECT_F dst = ui::R(imgX, imgY, (float)img_.w, (float)img_.h);
            rt_->DrawBitmap(img_bitmap_, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            D2D1_ROUNDED_RECT orr = ui::RR(imgX - 0.5f, imgY - 0.5f, (float)img_.w + 1.0f,
                                           (float)img_.h + 1.0f, 2.0f);
            rt_->DrawRoundedRectangle(&orr, cv_.brBorder, 1.0f, cv_.solid);
        }

        // 2. Annotations as geometry, so the preview on screen matches what gets exported.
        for (auto& s : undo_) paint_stroke(s);
        if (drawing_) paint_stroke(cur_);
        if (crop_active_) {
            D2D1_ROUNDED_RECT rr = cv_.R2rr((std::min)(crop_a_.x, crop_b_.x) + img_org_x_,
                                            (std::min)(crop_a_.y, crop_b_.y) + img_org_y_,
                                            fabs((double)(crop_b_.x - crop_a_.x)),
                                            fabs((double)(crop_b_.y - crop_a_.y)), 2.0f);
            rt_->DrawRoundedRectangle(&rr, cv_.brAccent, 2.0f, cv_.solid);
        }

        // 3. The pill.
        paint_pill();

        // 4. Status strip: dimensions then the shortcut hint, deliberately quiet.
        float stripY = H - (float)FOOT_H;
        rt_->DrawLine(D2D1_POINT_2F{ (float)t.s16, stripY - (float)t.s8 },
                      D2D1_POINT_2F{ W - (float)t.s16, stripY - (float)t.s8 }, cv_.brSoft);
        if (cv_.fmtCaption && cv_.brMuted) {
            wchar_t buf[96];
            wsprintfW(buf, L"%d \x00D7 %d", img_.w, img_.h);
            D2D1_RECT_F fr = ui::R((float)t.s16, stripY, 200, (float)FOOT_H);
            rt_->DrawText(buf, (UINT)wcslen(buf), cv_.fmtCaption, &fr, cv_.brMuted,
                          D2D1_DRAW_TEXT_OPTIONS_NONE);

            const wchar_t* hint = L"1-6 \x0438\x043D\x0441\x0442\x0440\x0443\x043C\x0435\x043D\x0442\x044B   "
                                  L"Ctrl+Z   Ctrl+Y   Ctrl+S   Ctrl+C   Esc";
            D2D1_RECT_F hr = ui::R((float)t.s16 + 210.0f, stripY, W - 240.0f, (float)FOOT_H);
            rt_->DrawText(hint, (UINT)wcslen(hint), cv_.fmtCaption, &hr, cv_.brMuted,
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        HRESULT hr = rt_->EndDraw();
        if (hr != S_OK && hr != D2DERR_RECREATE_TARGET) {
            dbg("paint: EndDraw failed", hr);
            wprintf(L"EndDraw failed hr=0x%08X\n", (unsigned)hr);
        }
    }

    void paint_pill() {
        const Theme& t = theme_;
        D2D1_ROUNDED_RECT pr = ui::RR(pill_x_, pill_y_, (float)pill_w_, pill_h_, t.rFloating);
        rt_->FillRoundedRectangle(&pr, cv_.brSurface);
        rt_->DrawRoundedRectangle(&pr, cv_.brBorder, 1.0f, cv_.solid);

        for (auto& s : slots_) {
            switch (s.kind) {
                case KIND_SEP:
                    rt_->DrawLine(D2D1_POINT_2F{ s.x, pill_y_ + t.s6 },
                                  D2D1_POINT_2F{ s.x, pill_y_ + pill_h_ - t.s6 }, cv_.brSoft);
                    break;

                case KIND_TOOL: {
                    bool on = (s.tool == tool_);
                    ui::ControlState st = on ? ui::ST_ACTIVE : state_of(s);
                    cv_.control(s.x, s.y, s.w, s.h, st);
                    cv_.glyph(s.glyph, s.id + 1, s.x + s.w / 2, s.y + s.h / 2, st);
                    break;
                }

                case KIND_ACTION: {
                    bool off = (s.action == 0 && undo_.empty()) || (s.action == 1 && redo_.empty());
                    ui::ControlState st = off ? ui::ST_DISABLED : state_of(s);
                    cv_.control(s.x, s.y, s.w, s.h, st);
                    cv_.glyph(s.glyph, s.id + 9, s.x + s.w / 2, s.y + s.h / 2, st);
                    break;
                }

                case KIND_COLOR: {
                    bool on = (s.color == color_);
                    bool hov = (hover_slot_ == slot_index(s));
                    ID2D1SolidColorBrush* cb = nullptr;
                    if (!cv_.brush(col2(s.color), &cb)) break;
                    if (hov || on) cv_.control(s.x, s.y, s.w, s.h, hov && !on ? ui::ST_HOVER : ui::ST_ACTIVE);
                    float side = 22.0f * t.k;
                    float sx = s.x + (s.w - side) / 2.0f, sy = s.y + (s.h - side) / 2.0f;
                    D2D1_ROUNDED_RECT sq = ui::RR(sx, sy, side, side, 5.0f * t.k);
                    rt_->FillRoundedRectangle(&sq, cb);
                    // A swatch whose colour sits close to the pill surface reads as an empty
                    // hole (that is exactly what happened to the near-black swatch), so those
                    // swatches get the stronger outline instead of the hairline.
                    float sl = 0.2126f * GetRValue(s.color) + 0.7152f * GetGValue(s.color) +
                               0.0722f * GetBValue(s.color);
                    float pl = 0.2126f * t.floating.r + 0.7152f * t.floating.g + 0.0722f * t.floating.b;
                    pl *= 255.0f;
                    bool too_close = fabsf(sl - pl) < 26.0f;
                    rt_->DrawRoundedRectangle(&sq, too_close ? cv_.brBorder : cv_.brSoft, 1.0f, cv_.solid);
                    if (on) {
                        D2D1_ROUNDED_RECT ring = ui::RR(sx - 3.0f * t.k, sy - 3.0f * t.k,
                                                        side + 6.0f * t.k, side + 6.0f * t.k, 8.0f * t.k);
                        rt_->DrawRoundedRectangle(&ring, cv_.brAccent, 2.0f, cv_.solid);
                    }
                    break;
                }

                case KIND_WIDTH: {
                    bool on = (s.width == width_);
                    cv_.control(s.x, s.y, s.w, s.h, on ? ui::ST_ACTIVE : state_of(s));
                    // The sample draws in the colour you are actually about to draw with, so
                    // the control previews the real stroke rather than a neutral grey tick.
                    D2D1_COLOR_F c = col2(color_);
                    if (!on) c.a = 0.55f;
                    ID2D1SolidColorBrush* ink = nullptr;
                    if (!cv_.brush(c, &ink)) ink = on ? cv_.brAccent : cv_.brMuted;
                    float half = s.w * 0.26f, cx = s.x + s.w / 2, cy = s.y + s.h / 2;
                    float thick = (1.5f + (float)std::max(1, s.width) * 0.75f) * t.k;
                    rt_->DrawLine(D2D1_POINT_2F{ cx - half, cy }, D2D1_POINT_2F{ cx + half, cy },
                                  ink, thick, cv_.solid);
                    break;
                }
            }
        }
    }

    ui::ControlState state_of(const Slot& s) {
        return (hover_slot_ == slot_index(s)) ? ui::ST_HOVER : ui::ST_NORMAL;
    }

    int slot_index(const Slot& s) const {
        for (int i = 0; i < (int)slots_.size(); ++i)
            if (slots_[i].kind == s.kind && slots_[i].id == s.id) return i;
        return -1;
    }

    static D2D1_COLOR_F col2(COLORREF c) {
        return D2D1_COLOR_F{ GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f };
    }

    // Strokes are drawn as geometry so the on-screen preview is antialiased.
    void paint_stroke(const Stroke& s) {
        if (s.pts.empty()) return;
        const Theme& t = theme_;
        D2D1_COLOR_F c = col2(s.color);
        if (s.tool == TOOL_MARKER) c.a = 0.45f;

        ID2D1SolidColorBrush* br = nullptr;
        if (!cv_.brush(c, &br)) return;
        const float w = (float)std::max(1, s.width) * t.k;

        if (s.tool == TOOL_TEXT) {
            if (s.text.empty() || !cv_.fmtBody) return;
            POINT p = s.pts.front();
            D2D1_RECT_F r = ui::R(img_org_x_ + (float)p.x, img_org_y_ + (float)p.y - t.fsBody * 1.2f,
                                  (float)std::max(240, (int)s.text.size() * t.fsBody),
                                  (float)(t.fsH2 + t.s8));
            rt_->DrawText(s.text.c_str(), (UINT)s.text.size(), cv_.fmtBody, &r, br,
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
            return;
        }

        auto P = [&](POINT p) { return D2D1_POINT_2F{ img_org_x_ + (float)p.x, img_org_y_ + (float)p.y }; };

        if ((s.tool == TOOL_RECT || s.tool == TOOL_ELLIPSE) && s.pts.size() >= 2) {
            POINT a = s.pts.front(), b = s.pts.back();
            float x0 = (float)std::min(a.x, b.x), y0 = (float)std::min(a.y, b.y);
            float ww = (float)fabs((double)(b.x - a.x)), hh = (float)fabs((double)(b.y - a.y));
            if (s.tool == TOOL_RECT) {
                D2D1_ROUNDED_RECT rr = cv_.R2rr(img_org_x_ + x0, img_org_y_ + y0, ww, hh, 2.0f * t.k);
                rt_->DrawRoundedRectangle(&rr, br, w, cv_.solid);
            } else {
                ID2D1PathGeometry* g = nullptr;
                if (SUCCEEDED(factory_->CreatePathGeometry(&g))) {
                    ID2D1GeometrySink* sink = nullptr;
                    if (SUCCEEDED(g->Open(&sink))) {
                        cv_.ellipse_body(sink, img_org_x_ + x0 + ww / 2, img_org_y_ + y0 + hh / 2, ww / 2, hh / 2);
                        sink->Close();
                        rt_->DrawGeometry(g, br, w, cv_.solid);
                        sink->Release();
                    }
                    g->Release();
                }
            }
            return;
        }

        if (s.tool == TOOL_ARROW && s.pts.size() >= 2) {
            POINT a = s.pts.front(), b = s.pts.back();
            double ang = atan2((double)(b.y - a.y), (double)(b.x - a.x));
            float head = (12.0f + (float)s.width * 2.5f) * t.k;
            float back = head * 0.75f;

            D2D1_POINT_2F from = P(a);
            D2D1_POINT_2F to = { img_org_x_ + (float)b.x - (float)cos(ang) * back,
                                 img_org_y_ + (float)b.y - (float)sin(ang) * back };
            rt_->DrawLine(from, to, br, w, cv_.solid);

            ID2D1PathGeometry* hg = nullptr;
            if (SUCCEEDED(factory_->CreatePathGeometry(&hg))) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(hg->Open(&sink))) {
                    D2D1_POINT_2F tip = P(b);
                    double a1 = ang + 2.55, a2 = ang - 2.55;
                    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({ tip.x + (float)cos(a1) * head, tip.y + (float)sin(a1) * head });
                    sink->AddLine({ tip.x - (float)cos(ang) * head * 0.55f,
                                    tip.y - (float)sin(ang) * head * 0.55f });
                    sink->AddLine({ tip.x + (float)cos(a2) * head, tip.y + (float)sin(a2) * head });
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    rt_->FillGeometry(hg, br);
                    sink->Release();
                }
                hg->Release();
            }
            return;
        }

        ID2D1PathGeometry* g = nullptr;
        if (FAILED(factory_->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g->Open(&sink))) {
            sink->BeginFigure(P(s.pts.front()), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < s.pts.size(); ++i) sink->AddLine(P(s.pts[i]));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            rt_->DrawGeometry(g, br, w, cv_.solid);
            sink->Release();
        }
        g->Release();
    }

    // ---- interaction -------------------------------------------------------------------
    const Slot* hit(int mx, int my) const {
        for (auto& s : slots_) {
            if (s.kind == KIND_SEP) continue;
            if (mx >= s.x && mx < s.x + s.w && my >= s.y && my < s.y + s.h) return &s;
        }
        return nullptr;
    }

    void on_down(int mx, int my) {
        if (const Slot* s = hit(mx, my)) {
            hover_slot_ = slot_index(*s);
            switch (s->kind) {
                case KIND_TOOL:  tool_ = s->tool; InvalidateRect(hwnd_, nullptr, TRUE); return;
                case KIND_COLOR: color_ = s->color; InvalidateRect(hwnd_, nullptr, TRUE); return;
                case KIND_WIDTH: width_ = s->width; InvalidateRect(hwnd_, nullptr, TRUE); return;
                case KIND_ACTION: do_action(s->action); return;
                default: break;
            }
        }

        if (tool_ == TOOL_CROP) {
            crop_active_ = true;
            crop_a_ = { imgX(mx), imgY(my) };
            crop_b_ = crop_a_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        drawing_ = true;
        text_buffer_.clear();
        cur_ = Stroke{ tool_, { { imgX(mx), imgY(my) } }, color_, width_, L"" };
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void do_action(int a) {
        if (a == 0) {                                   // undo
            if (!undo_.empty()) { redo_.push_back(undo_.back()); undo_.pop_back(); }
        } else if (a == 1) {                            // redo
            if (!redo_.empty()) { undo_.push_back(redo_.back()); redo_.pop_back(); }
        } else if (a == 2) {                            // copy to clipboard and close
            copy_requested_ = true; saved_ = true; PostQuitMessage(0); return;
        } else if (a == 3) {                            // save and close
            copy_requested_ = false; saved_ = true; PostQuitMessage(0); return;
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void on_move(int mx, int my) {
        if (drawing_) {
            POINT p{ imgX(mx), imgY(my) };
            if (cur_.tool == TOOL_RECT || cur_.tool == TOOL_ELLIPSE || cur_.tool == TOOL_ARROW) {
                if (cur_.pts.size() < 2) cur_.pts.push_back(p); else cur_.pts[1] = p;
            } else {
                cur_.pts.push_back(p);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (crop_active_) { crop_b_ = { imgX(mx), imgY(my) }; InvalidateRect(hwnd_, nullptr, FALSE); return; }

        const Slot* s = hit(mx, my);
        int idx = s ? slot_index(*s) : -1;
        if (idx != hover_slot_) { hover_slot_ = idx; InvalidateRect(hwnd_, nullptr, TRUE); }
    }

    void on_up(int mx, int my) {
        if (crop_active_) {
            crop_active_ = false;
            apply_crop(crop_a_, crop_b_);
            refresh_image_bitmap();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (!drawing_) return;
        drawing_ = false;
        on_move(mx, my);
        if (cur_.tool == TOOL_TEXT) {
            cur_.text = text_buffer_;
            if (cur_.text.empty()) { InvalidateRect(hwnd_, nullptr, TRUE); return; }
        }
        bool enough = cur_.pts.size() >= 2 || cur_.tool == TOOL_TEXT;
        if (enough) { undo_.push_back(cur_); redo_.clear(); }
        cur_ = Stroke{};
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void on_key(int vk) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && vk == 'Z') { do_action(0); return; }
        if (ctrl && vk == 'Y') { do_action(1); return; }
        if (ctrl && vk == 'S') { do_action(3); return; }
        if (ctrl && vk == 'C') { do_action(2); return; }
        if (vk == VK_ESCAPE) { discard_ = true; saved_ = false; PostQuitMessage(0); return; }
        if (vk == VK_DELETE) { if (!undo_.empty()) { undo_.pop_back(); InvalidateRect(hwnd_, nullptr, TRUE); } return; }
        if (!drawing_) {
            const int tools[6] = { TOOL_ARROW, TOOL_TEXT, TOOL_RECT, TOOL_ELLIPSE, TOOL_MARKER, TOOL_CROP };
            if (vk >= '1' && vk <= '6') { tool_ = (Tool)tools[vk - '1']; InvalidateRect(hwnd_, nullptr, TRUE); }
        }
    }

    void on_char(wchar_t c) {
        if (drawing_ && cur_.tool == TOOL_TEXT) {
            if (c == 8 && !text_buffer_.empty()) { text_buffer_.pop_back(); InvalidateRect(hwnd_, nullptr, TRUE); return; }
            if (c >= 32) { text_buffer_ += c; InvalidateRect(hwnd_, nullptr, TRUE); }
        }
    }

    int imgX(int mx) const { return (std::max)(0, (std::min)(img_.w - 1, mx - img_org_x_)); }
    int imgY(int my) const { return (std::max)(0, (std::min)(img_.h - 1, my - img_org_y_)); }

    void apply_crop(POINT a, POINT b) {
        int x0 = (std::min)(a.x, b.x), x1 = (std::max)(a.x, b.x);
        int y0 = (std::min)(a.y, b.y), y1 = (std::max)(a.y, b.y);
        if (x1 - x0 < 8 || y1 - y0 < 8) return;
        DIB out;
        out.w = x1 - x0; out.h = y1 - y0;
        out.rgba.assign((size_t)out.w * out.h * 4, 0);
        for (int y = 0; y < out.h; ++y)
            memcpy(&out.rgba[(size_t)y * out.w * 4], img_.px(0, y0 + y), (size_t)out.w * 4);

        std::vector<Stroke> kept;
        for (auto& s : undo_) {
            bool inside = true;
            for (auto& p : s.pts) if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) { inside = false; break; }
            if (inside) { for (auto& p : s.pts) { p.x -= x0; p.y -= y0; } kept.push_back(s); }
        }
        img_ = std::move(out);
        undo_ = std::move(kept);
        redo_.clear();
    }

    // ---- export rasterisers: the saved PNG is produced here, in image-space px ---------
    void put_img(DIB& d, int x, int y, COLORREF col) {
        if (x < 0 || y < 0 || x >= d.w || y >= d.h) return;
        uint8_t* p = d.px(x, y);
        p[0] = GetRValue(col); p[1] = GetGValue(col); p[2] = GetBValue(col); p[3] = 255;
    }
    void line_img(DIB& d, int x0, int y0, int x1, int y1, COLORREF col, int th) {
        int dx = abs(x1 - x0), dy = -abs(y1 - y0);
        int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while (true) {
            for (int o = 0; o < th; ++o) put_img(d, x0 + o, y0, col);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    void ellipse_img(DIB& d, int cx, int cy, int rx, int ry, COLORREF col, int th) {
        for (int a = 0; a < 3600; ++a) {
            double t = a * 3.14159265 / 1800.0;
            int x = cx + (int)(rx * cos(t)), y = cy + (int)(ry * sin(t));
            for (int o = 0; o < th; ++o) put_img(d, x, y + o, col);
        }
    }
    void arrow_head_img(DIB& d, int x, int y, double ang, COLORREF col, int th) {
        for (int s = 0; s < 14 + th * 2; ++s) {
            for (int sign : { -1, 1 }) {
                double a = ang + sign * 0.5;
                int px_ = x - (int)(cos(a) * s), py_ = y - (int)(sin(a) * s);
                for (int o = 0; o < th; ++o) put_img(d, px_, py_ + o, col);
            }
        }
    }
    void draw_stroke_img(DIB& d, const Stroke& s) {
        if (s.pts.empty()) return;
        if (s.tool == TOOL_TEXT) {
            if (s.text.empty()) return;
            POINT p = s.pts.front();
            int w = (int)s.text.size() * 11;
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < w; ++x) put_img(d, p.x + x, p.y - 16 + y, s.color);
            return;
        }
        if ((s.tool == TOOL_RECT || s.tool == TOOL_ELLIPSE) && s.pts.size() >= 2) {
            POINT a = s.pts.front(), b = s.pts.back();
            int x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
            int y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
            if (s.tool == TOOL_RECT) {
                for (int x = x0; x <= x1; ++x)
                    for (int o = 0; o < s.width; ++o) { put_img(d, x, y0 + o, s.color); put_img(d, x, y1 + o, s.color); }
                for (int y = y0; y <= y1; ++y)
                    for (int o = 0; o < s.width; ++o) { put_img(d, x0 + o, y, s.color); put_img(d, x1 + o, y, s.color); }
            } else {
                ellipse_img(d, (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2, s.color, s.width);
            }
            return;
        }
        if (s.tool == TOOL_ARROW && s.pts.size() >= 2) {
            POINT a = s.pts.front(), b = s.pts.back();
            line_img(d, a.x, a.y, b.x, b.y, s.color, s.width);
            arrow_head_img(d, b.x, b.y, atan2((double)(b.y - a.y), (double)(b.x - a.x)), s.color, s.width);
            return;
        }
        for (size_t i = 1; i < s.pts.size(); ++i)
            line_img(d, s.pts[i - 1].x, s.pts[i - 1].y, s.pts[i].x, s.pts[i].y, s.color, s.width);
    }

    // ---- state -------------------------------------------------------------------------
    HWND hwnd_ = nullptr;
    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ui::Canvas cv_;
    Theme theme_;

    ID2D1Bitmap* img_bitmap_ = nullptr;
    std::vector<uint8_t> bgra_;
    int img_version_ = 0;
    int img_org_x_ = 0, img_org_y_ = 0;

    DIB img_;
    Tool tool_ = TOOL_ARROW;
    COLORREF color_ = RGB(0xE4, 0x2A, 0x31);        // annotations stay the product red
    int width_ = 4;
    std::vector<COLORREF> palette_ = {
        RGB(0xE4,0x2A,0x31), RGB(0x31,0x9E,0x14), RGB(0xC8,0x3C,0xB4),
        RGB(0x1F,0x78,0xD7), RGB(0xE8,0xB8,0x00), RGB(0x22,0x22,0x22), RGB(0xFF,0xFF,0xFF)
    };
    std::vector<int> widths_ = { 2, 4, 8 };

    std::vector<Stroke> undo_, redo_;
    Stroke cur_;
    bool drawing_ = false, crop_active_ = false;
    POINT crop_a_{}, crop_b_{};
    std::wstring text_buffer_;
    int hover_slot_ = -1;
    bool saved_ = false, discard_ = false, copy_requested_ = false;
};
