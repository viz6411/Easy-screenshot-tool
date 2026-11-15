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
#include <commctrl.h>
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
    int backdrop = 0;              // text only: 0 none / 1 light / 2 dark (TOOLBAR-SPEC 8.5)
};

// A committed step of the edit history. A crop changes the image itself, so it has to carry both
// sides of itself: undoing a crop without the previous pixels is what made crops un-undoable in 0.1.
// shortcut: a crop stores two full copies of the image; upgrade if crop history ever gets deep.
struct Change {
    bool is_crop = false;
    Stroke stroke;                 // the stroke that was added (redo side of a stroke change)
    DIB img_before, img_after;
    std::vector<Stroke> st_before, st_after;
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
        frame_w_ = frameW; frame_h_ = frameH;

        // Captions are measured through DirectWrite, which needs the text formats, which need the
        // window; so the first pass sizes the window from the caption-free layout and fit_window()
        // corrects it once the real caption widths are known.
        fit_ = FIT_PLAIN_1;          // the narrowest layout until fit_window() measures the real one
        labels_ = false;
        build_slots();
        int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
        int w = (std::min)((std::max)(img_.w + PAD * 2, pill_w_ + theme_.s24) + frameW, scrW - 8);
        int h = 300 + frameH;

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
        fit_window(scrW, scrH, frameW, frameH);

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
        return produced_;
    }

    // Output happens while the editor is open (card #8): neither action closes the window, and the
    // owner supplies the file/clipboard work because it owns settings.ini.
    enum OutputResult { OUT_SAVED = 0, OUT_CANCELLED, OUT_FAILED };
    using SaveFn = OutputResult (*)(const DIB& img, HWND owner, std::wstring& path_out);
    using CopyFn = bool (*)(const DIB& img);
    void set_output(SaveFn s, CopyFn c) { save_fn_ = s; copy_fn_ = c; }

    bool produced() const { return produced_; }
    // Card #22: the editor already asked "Сохранить / Не сохранять / Отмена" and the answer was
    // "Не сохранять". main.cpp must not announce a discarded capture on top of that answer.
    bool discard_confirmed() const { return discard_confirmed_; }

    // Bake committed strokes into the image so the saved PNG contains them (image-space px).
    DIB compose_result() {
        DIB out = img_;
        for (auto& s : strokes_) draw_stroke_img(out, s);
        return out;
    }

private:
    // ---- slot model --------------------------------------------------------------------
    enum Kind { KIND_TOOL, KIND_COLOR, KIND_WIDTH, KIND_BACKDROP, KIND_ACTION, KIND_SEP };
    enum Group { GRP_TOOLS = 0, GRP_COLORS, GRP_PROPS, GRP_HISTORY, GRP_OUTPUT, GRP_COUNT };

    struct Slot {
        Kind kind = KIND_SEP;
        Group group = GRP_TOOLS;
        int row = 0;               // the pill can carry the same groups on two rows
        int id = 0;                // index within its group
        Tool tool = TOOL_ARROW;
        COLORREF color = 0;
        int width = 0;             // KIND_WIDTH, or backdrop mode 0 none / 1 light / 2 dark
        int action = -1;           // 0 undo, 1 redo, 2 copy, 3 save
        int glyph = 0;
        std::wstring caption;      // empty = glyph only
        float x = 0, y = 0, w = 0, h = 0;   // physical px in client space (the whole cell)
    };

    int PAD = 16, PAD_TOP = 8, PAD_BOTTOM = 12, FOOT_H = 26;
    int pill_w_ = 0;
    float pill_x_ = 0, pill_y_ = 0, pill_h_ = 0;
    // One geometry shared by painting and hit-testing: the band the picture may occupy and the top
    // edge of the status strip that bounds it from below (card #24).
    float strip_y_ = 0, img_top_ = 0, img_bottom_ = 0;
    bool strip_hidden_ = false, growing_ = false;
    std::vector<Slot> slots_;
    std::wstring group_caption_[GRP_COUNT];
    static const int MAX_ROWS = 3;                 // the toolbar reflows to at most three rows (#24)
    float grp_x_[MAX_ROWS][GRP_COUNT] = {};        // left edge of a group on a row, -1 when absent
    float grp_w_[MAX_ROWS][GRP_COUNT] = {};

    // Fit levels, richest first: labels on one row, labels wrapped to two rows, then the same
    // without captions. Every level is measured from the real client width, so the pill adapts
    // to the monitor instead of assuming a 2560-wide calibrated display.
    // Six layouts: captions on/off x 1..3 rows (docs/WIN11-UI-GUIDE.md R9). Every level caches the
    // partition of the five groups that makes its widest row as narrow as possible, so the pill is
    // never wider than the window that shows it - that is what broke when the window was resized
    // (card #24).
    enum Fit { FIT_CAP_1 = 0, FIT_CAP_2, FIT_CAP_3, FIT_PLAIN_1, FIT_PLAIN_2, FIT_PLAIN_3 };
    static const int LEVELS = 6;
    Fit fit_ = FIT_CAP_1;
    float fit_w_[LEVELS] = { 0, 0, 0, 0, 0, 0 };
    int pill_min_ = 0;
    bool labels_ = true;

    void apply_metric_padding() {
        PAD = theme_.s16; PAD_TOP = theme_.s12;
        // Card #9: the gap under the image is one constant (PAD_BOTTOM), the separator is the
        // top edge of the strip, and the strip is tall enough to hold a whole caption line.
        PAD_BOTTOM = theme_.s16;
        FOOT_H = 2 * theme_.s6 + caption_line();
    }

    int caption_line() const { return theme_.fsCaption * 4 / 3; }
    // A signed cell carries two caption bands: the group's own line on top, the control's caption
    // under it (card #22 needs a caption in the output group, and the group caption has never had
    // a row of its own). `pill_h_for` below must use this same function - the two numbers are the
    // pill height and the row positions, and if they disagree the rows slide out of the pill.
    int cell_h_for(bool signed_cell) const {
        return signed_cell ? theme_.ctlH + 2 * theme_.s4 + 2 * caption_line() : theme_.ctlH;
    }
    int cell_h() const { return cell_h_for(labels_); }
    int rows_() const { return (fit_ % MAX_ROWS) + 1; }
    static bool fit_has_labels(int f) { return f <= FIT_CAP_3; }

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
                measure_fits();                       // every cached measurement is DPI-scaled (R5)
                layout();
                InvalidateRect(h, nullptr, TRUE);
                return 0;
            }

            case WM_SIZE: {
                int cw = (int)LOWORD(lp), ch = (int)HIWORD(lp);
                if (rt_ && cw > 0 && ch > 0) {
                    bind_dc();                         // the target follows the new DIB, no swap chain
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
            case WM_KEYUP:       on_key_up((int)wp); return 0;
            case WM_CHAR:        on_char((wchar_t)wp); return 0;

            // Card #22: one closing path only - the cross and Esc both end up here, so the unsaved
            // question cannot be avoided by choosing the other way out.
            case WM_CLOSE:
                if (dirty_ && !ask_discard()) return 0;    // "Отмена": the window stays open
                DestroyWindow(h);
                return 0;

            case WM_GETMINMAXINFO: {
                MINMAXINFO* mmi = (MINMAXINFO*)lp;
                // Both floors come from what the layout can actually draw (WIN11-UI-GUIDE R6): the
                // narrowest pill (captions off, three rows) and a height that still fits the tallest
                // toolbar this width can select plus the status strip.
                int pill_floor = pill_min_ ? pill_min_ : pill_w_;
                mmi->ptMinTrackSize.x = (std::max)(420 + frame_w_,
                                                   (pill_floor + theme_.s24 + 16) + frame_w_);
                int shortest = pill_h_for(FIT_PLAIN_1);
                for (int f = 0; f < LEVELS; ++f)
                    if (fit_w_[f] + 2 * theme_.s8 <= mmi->ptMinTrackSize.x - frame_w_ &&
                        pill_h_for(f) < shortest) shortest = pill_h_for(f);
                mmi->ptMinTrackSize.y = (std::max)(240, shortest + FOOT_H + theme_.s12 + PAD_TOP +
                                                    PAD_BOTTOM + frame_h_);
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
        rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;   // the DIB section we draw into is opaque
        rp.dpiX = 96.0f; rp.dpiY = 96.0f;            // sizes are pre-scaled by the theme
        rp.usage = D2D1_RENDER_TARGET_USAGE_NONE;

        // Drawing goes to a DIB section that is copied into the window, not to a DXGI swap chain.
        // The swap chain path presented nothing on this machine (hybrid iGPU/dGPU with the dGPU
        // busy): the window stayed white while all the drawing calls succeeded. A layered window
        // with a DIB (the selection overlay) composites fine, so the editor uses the same route.
        ID2D1DCRenderTarget* dc = nullptr;
        hr = factory_->CreateDCRenderTarget(&rp, &dc);
        if (FAILED(hr)) { dbg("init: CreateDCRenderTarget", hr); return false; }
        rt_ = dc;
        bind_dc();

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
        if (memdc_) { DeleteDC(memdc_); memdc_ = nullptr; }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
        if (factory_) { factory_->Release(); factory_ = nullptr; }
    }

    // The D2D target is bound to a top-down 32 bpp DIB in a memory DC; paint() copies that DIB
    // into the window. Resizing means a new DIB and a new BindDC, never a swap chain resize.
    void bind_dc() {
        RECT cr; GetClientRect(hwnd_, &cr);
        int w = cr.right - cr.left, h = cr.bottom - cr.top;
        if (!rt_ || w <= 0 || h <= 0) return;
        if (memdc_) { DeleteDC(memdc_); memdc_ = nullptr; }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // negative: top-down rows
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(hwnd_);
        memdc_ = CreateCompatibleDC(screen);
        dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(hwnd_, screen);
        if (!memdc_ || !dib_) { dbg("bind: dib section failed"); return; }
        SelectObject(memdc_, dib_);
        RECT area = { 0, 0, w, h };
        HRESULT bhr = ((ID2D1DCRenderTarget*)rt_)->BindDC(memdc_, &area);
        if (FAILED(bhr)) dbg("bind: BindDC failed", bhr);
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
    // build_slots() is the only place the toolbar's structure exists. Groups are measured first
    // (captions through IDWrite metrics, never through a guessed character width), then placed, so
    // the pill can never be narrower than what is drawn on it and the Save control can never
    // hang outside it again.
    struct Grp {
        Group id = GRP_TOOLS;
        Kind kind = KIND_TOOL;
        float cell = 0, gap = 0, content = 0;
        std::vector<Slot> slots;
    };

    static const wchar_t* tool_caption(int i) {
        static const wchar_t* c[6] = { L"Стрелка", L"Текст", L"Прямоугольник",
                                       L"Овал", L"Маркер", L"Кадрирование" };
        return c[i];
    }
    static const wchar_t* output_caption(int i) {
        static const wchar_t* c[2] = { L"Копировать", L"Сохранить" };
        return c[i];
    }

    std::vector<Grp> build_groups(bool with_labels) {
        const Theme& t = theme_;
        const int glyphs[6] = { ui::G_ARROW, ui::G_TEXT, ui::G_RECT, ui::G_ELLIPSE, ui::G_MARKER, ui::G_CROP };
        const Tool tools[6] = { TOOL_ARROW, TOOL_TEXT, TOOL_RECT, TOOL_ELLIPSE, TOOL_MARKER, TOOL_CROP };
        const int acts[2] = { 2, 3 };                       // copy, save (undo/redo are their own group)

        // The widest measured caption sets the cell for its whole group: every cell in a group is
        // the same width, so the pitch stays uniform and a caption can never push its neighbours.
        float tool_cap = 0, out_cap = 0;
        if (with_labels) {
            for (int i = 0; i < 6; ++i) tool_cap = (std::max)(tool_cap, cv_.measure(tool_caption(i), cv_.fmtCaption));
            for (int i = 0; i < 2; ++i) out_cap = (std::max)(out_cap, cv_.measure(output_caption(i), cv_.fmtCaption));
        }

        std::vector<Grp> gs(GRP_COUNT);
        auto mk = [&](Group g, Kind kind, float cell, float gap) {
            Grp& gr = gs[g]; gr.id = g; gr.kind = kind; gr.cell = cell; gr.gap = gap;
            return &gr;
        };
        auto add = [&](Grp& gr, int id, int glyph, Tool tool, COLORREF col, int val, int action,
                       const wchar_t* cap) {
            Slot s; s.kind = gr.kind; s.group = gr.id; s.id = id; s.glyph = glyph; s.tool = tool;
            s.color = col; s.width = val; s.action = action;
            if (cap) s.caption = cap;
            gr.slots.push_back(s);
        };

        // Level 3 (no captions) restores the 0.1 geometry exactly: every cell is ctlH wide with an
        // s2 gap, which is what makes the plain pill measure the same 1350 px it did in 0.1.
        Grp* tg = mk(GRP_TOOLS, KIND_TOOL, with_labels ? tool_cap + 2.0f * t.s4 : (float)t.ctlH,
                     with_labels ? (float)t.s6 : (float)t.s2);
        for (int i = 0; i < 6; ++i) add(*tg, i, glyphs[i], tools[i], 0, 0, -1, with_labels ? tool_caption(i) : nullptr);

        Grp* cg = mk(GRP_COLORS, KIND_COLOR, (float)t.ctlH, (float)t.s2);
        for (int i = 0; i < (int)palette_.size(); ++i) add(*cg, i, -1, TOOL_ARROW, palette_[i], 0, -1, nullptr);

        // Card #7: with the text tool this group carries the three backdrop states instead of the
        // three line widths - same cells, same gap, same separators, so the pill never resizes
        // when the tool changes.
        bool props_backdrop = (tool_ == TOOL_TEXT);
        Grp* pg = mk(GRP_PROPS, props_backdrop ? KIND_BACKDROP : KIND_WIDTH, (float)t.ctlH, (float)t.s2);
        for (int i = 0; i < (int)widths_.size(); ++i)
            add(*pg, i, -1, TOOL_ARROW, 0, props_backdrop ? i : widths_[i], -1, nullptr);

        Grp* hg = mk(GRP_HISTORY, KIND_ACTION, (float)t.ctlH, (float)t.s2);
        add(*hg, 0, ui::G_UNDO, TOOL_ARROW, 0, 0, 0, nullptr);
        add(*hg, 1, ui::G_REDO, TOOL_ARROW, 0, 0, 1, nullptr);

        Grp* og = mk(GRP_OUTPUT, KIND_ACTION, with_labels ? out_cap + 2.0f * t.s4 : (float)t.ctlH,
                     with_labels ? (float)t.s6 : (float)t.s2);
        add(*og, 2, ui::G_COPY, TOOL_ARROW, 0, 0, acts[0], with_labels ? output_caption(0) : nullptr);
        add(*og, 3, ui::G_SAVE, TOOL_ARROW, 0, 0, acts[1], with_labels ? output_caption(1) : nullptr);

        // Card #22: the output group carries its own caption - whether the edits are still unsaved.
        // Its width is reserved whatever `dirty_` currently says, so flipping the state must not
        // resize the pill (a pill that changes width mid-session re-flows every control under the
        // pointer).
        const wchar_t* out_dirty_cap = L"есть несохранённые правки";
        const wchar_t* out_clean_cap = L"нет несохранённых правок";
        if (with_labels) {
            float cap_w = (std::max)(cv_.measure(out_dirty_cap, cv_.fmtCaption),
                                     cv_.measure(out_clean_cap, cv_.fmtCaption));
            float grp_w = 2.0f * out_cap + 4.0f * t.s4 + (float)t.s6;
            if (cap_w > grp_w) out_cap = (cap_w - (float)t.s6 - 4.0f * t.s4) / 2.0f;
        }

        group_caption_[GRP_TOOLS] = L"";
        group_caption_[GRP_COLORS] = with_labels ? L"Цвет" : L"";
        group_caption_[GRP_PROPS] = with_labels
            ? (tool_ == TOOL_TEXT ? L"Фон" : L"Толщина") : L"";
        group_caption_[GRP_HISTORY] = L"";
        group_caption_[GRP_OUTPUT] = with_labels ? (dirty_ ? out_dirty_cap : out_clean_cap) : L"";

        for (auto& gr : gs) {
            int n = (int)gr.slots.size();
            gr.content = n * gr.cell + (n > 0 ? (n - 1) : 0) * gr.gap;
        }
        return gs;
    }

    float row_width(const std::vector<Grp>& gs, int from, int to) const {
        float w = 0;
        for (int i = from; i <= to; ++i) {
            if (i > from) w += gs[i - 1].gap + 2.0f * theme_.s8;   // the separator sits in this gap
            w += gs[i].content;
        }
        return w;
    }

    // Where the groups break into rows so that the widest row is as narrow as possible. Five groups
    // is small enough to try every contiguous break (C(4,rows-1) combinations) once per DPI.
    struct Partition {
        float widest = 0;
        int rows = 1;
        int start[MAX_ROWS + 1] = { 0 };    // group index where each row begins, last entry = GRP_COUNT
    };

    Partition best_partition(const std::vector<Grp>& gs, int rows) const {
        Partition best; best.rows = rows; best.widest = 1e18f;
        auto consider = [&](Partition p) {
            p.widest = 0;
            for (int r = 0; r < p.rows; ++r)
                p.widest = (std::max)(p.widest, row_width(gs, p.start[r], p.start[r + 1] - 1));
            if (p.widest < best.widest) best = p;
        };
        Partition p = {}; p.start[0] = 0;
        if (rows == 1) { p.rows = 1; p.start[1] = GRP_COUNT; consider(p); return best; }
        for (int a = 1; a < GRP_COUNT; ++a) {
            Partition q = p; q.rows = 2; q.start[1] = a; q.start[2] = GRP_COUNT;
            if (rows == 2) { consider(q); continue; }
            for (int b = a + 1; b < GRP_COUNT; ++b) {
                Partition t = q; t.rows = 3; t.start[2] = b; t.start[3] = GRP_COUNT;
                consider(t);
            }
        }
        return best;
    }

    float pill_width_for(const Partition& p) const { return p.widest + 2.0f * theme_.s12; }

    Partition parts_[LEVELS];

    // Measures all six layouts. Needs the text formats, so it runs after the D2D target exists - and
    // again whenever the DPI changes, because every number here is DPI-scaled (R2/R5).
    void measure_fits() {
        for (int f = 0; f < LEVELS; ++f) {
            std::vector<Grp> gs = build_groups(fit_has_labels(f));
            parts_[f] = best_partition(gs, (f % MAX_ROWS) + 1);
            fit_w_[f] = pill_width_for(parts_[f]);
        }
        pill_min_ = (int)fit_w_[FIT_PLAIN_3];
    }

    // The richest layout that fits BOTH the width and the height this window has: the toolbar may
    // reflow, it may not stick out of the window and it may not cover the status strip (#24).
    Fit fit_for(int client_w, int client_h) const {
        int shortestW = -1;
        for (int f = 0; f < LEVELS; ++f) {
            if (fit_w_[f] + 2.0f * theme_.s8 > (float)client_w) continue;
            if (shortestW < 0 || pill_h_for(f) < pill_h_for(shortestW)) shortestW = f;
            if (theme_.s12 + pill_h_for(f) + FOOT_H <= client_h) return (Fit)f;
        }
        // Nothing fits vertically either: keep the commands visible with the shortest toolbar and
        // let layout() grow the window back (R6 - the min-track size normally forbids this case).
        return shortestW >= 0 ? (Fit)shortestW : (Fit)FIT_PLAIN_3;
    }

    int pill_h_for(int f) const {
        int rows = (f % MAX_ROWS) + 1;
        int cell = cell_h_for(fit_has_labels(f));
        return rows * cell + (rows + 1) * theme_.s6;
    }

    // Sizes the window from the measured pill: the richest layout that still fits on this screen
    // (TOOLBAR-SPEC 8.1 levels 1-3). Nothing here is tied to a particular monitor - the levels are
    // compared against the client width the screen actually allows.
    void fit_window(int scrW, int scrH, int frameW, int frameH) {
        measure_fits();
        int chosen = FIT_PLAIN_3;
        for (int f = 0; f < LEVELS; ++f) {
            int wantClientW = (std::max)(img_.w + 2 * PAD, (int)fit_w_[f] + theme_.s24);
            int winW = (std::min)(wantClientW + frameW, scrW - 8);
            if ((int)fit_w_[f] + 2 * theme_.s8 <= winW - frameW) { chosen = f; break; }
        }
        fit_ = (Fit)chosen; labels_ = fit_has_labels(chosen); build_slots();

        int wantClientH = theme_.s12 + pill_h_for(chosen) + PAD_TOP + img_.h + PAD_BOTTOM + FOOT_H;
        int w = (std::min)((std::max)(img_.w + 2 * PAD, pill_w_ + theme_.s24) + frameW, scrW - 8);
        int h = (std::min)(wantClientH + frameH, scrH - 8);
        if (w < 420 + frameW) w = (std::min)(420 + frameW, scrW - 8);
        if (h < 300 + frameH) h = (std::min)(300 + frameH, scrH - 8);

        int px = (std::max)(0, (scrW - w) / 2), py = (std::max)(0, (scrH - h) / 2);
        SetWindowPos(hwnd_, nullptr, px, py, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
        layout();
        dbg("fit_window applied");
    }

    void build_slots() {
        slots_.clear();
        labels_ = fit_has_labels(fit_);
        int rows = (fit_ % MAX_ROWS) + 1;
        std::vector<Grp> gs = build_groups(labels_);
        // Re-partition for the groups as they are right now: the property group swaps its caption
        // with the tool, so a partition measured earlier is not necessarily the narrowest today.
        Partition p = best_partition(gs, rows);
        pill_w_ = (int)(pill_width_for(p) + 0.5f);
        pill_h_ = (float)(rows * cell_h() + (rows + 1) * theme_.s6);

        const float cellH = (float)cell_h();
        for (int r = 0; r < MAX_ROWS; ++r) for (int g = 0; g < GRP_COUNT; ++g) grp_x_[r][g] = -1.0f;
        for (int r = 0; r < p.rows; ++r) {
            int from = p.start[r];
            int to = (r + 1 < p.rows ? p.start[r + 1] : GRP_COUNT) - 1;
            float span = row_width(gs, from, to);
            float x = (pill_w_ - span) / 2.0f;              // each row is centred in the pill
            float rowTop = (float)(r * (cell_h() + theme_.s6));
            for (int i = from; i <= to; ++i) {
                if (i > from) {
                    Slot s; s.kind = KIND_SEP; s.group = gs[i].id; s.row = r;
                    s.x = x + gs[i - 1].gap / 2.0f + theme_.s8;
                    s.y = rowTop; s.w = 1.0f; s.h = cellH;
                    slots_.push_back(s);
                    x += gs[i - 1].gap + 2.0f * theme_.s8;
                }
                float groupStart = x;
                for (auto& sl : gs[i].slots) {
                    sl.row = r;
                    sl.x = x;
                    sl.y = rowTop;
                    sl.w = gs[i].cell;
                    sl.h = cellH;
                    slots_.push_back(sl);
                    x += gs[i].cell + gs[i].gap;
                }
                grp_x_[r][gs[i].id] = groupStart;
                grp_w_[r][gs[i].id] = gs[i].content;
            }
        }
    }

    void layout() {
        if (!hwnd_) return;
        RECT cr; GetClientRect(hwnd_, &cr);
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        const Theme& t = theme_;

        fit_ = fit_for(cw, ch);
        labels_ = fit_has_labels(fit_);
        build_slots();                          // rows are already centred inside the pill
        pill_x_ = (float)((cw - pill_w_) / 2);
        pill_y_ = (float)t.s12;
        for (auto& s : slots_) { s.x += pill_x_; s.y += pill_y_ + t.s6; }

        // A window shorter than this toolbar + strip is not something the layout can draw, so grow
        // it back once (R6). growing_ keeps the WM_SIZE this triggers from looping.
        if (pill_y_ + pill_h_ + (float)FOOT_H > (float)ch && !growing_) {
            growing_ = true;
            int need = (std::min)((int)(pill_y_ + pill_h_ + FOOT_H + t.s6) + frame_h_,
                                  GetSystemMetrics(SM_CYSCREEN) - 8);
            SetWindowPos(hwnd_, nullptr, 0, 0, cw + frame_w_, need, SWP_NOMOVE | SWP_NOZORDER);
            growing_ = false;
            GetClientRect(hwnd_, &cr);
            ch = cr.bottom - cr.top;
        }
        strip_hidden_ = pill_y_ + pill_h_ + (float)FOOT_H > (float)ch;
        strip_y_ = (float)(ch - FOOT_H);
        img_top_ = pill_y_ + pill_h_ + (float)PAD_TOP;
        img_bottom_ = (std::max)(img_top_, strip_y_ - (float)PAD_BOTTOM);
    }

    // The property group belongs to the tool: line widths for a stroke, the three backdrops for
    // text. Choosing another tool rebuilds the pill, so its controls are the ones that tool has.
    void set_tool(Tool tl) {
        if (tl == tool_) return;
        if (text_edit_) commit_text();
        tool_ = tl;
        layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
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
        //    edges of a screenshot read cleanly against the dark canvas. The picture and everything
        //    drawn on it are clipped to the band between the pill and the strip: shrinking the
        //    window used to slide them under the toolbar and under the strip (card #24).
        float imgX = (float)((int)((W - img_.w) / 2));
        float imgY = img_top_;
        img_org_x_ = (int)imgX; img_org_y_ = (int)imgY;
        rt_->PushAxisAlignedClip(ui::R(0.0f, imgY, W, img_bottom_ - imgY), D2D1_ANTIALIAS_MODE_ALIASED);

        if (img_bitmap_) {
            D2D1_RECT_F dst = ui::R(imgX, imgY, (float)img_.w, (float)img_.h);
            rt_->DrawBitmap(img_bitmap_, &dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            D2D1_ROUNDED_RECT orr = ui::RR(imgX - 0.5f, imgY - 0.5f, (float)img_.w + 1.0f,
                                           (float)img_.h + 1.0f, 2.0f);
            rt_->DrawRoundedRectangle(&orr, cv_.brBorder, 1.0f, cv_.solid);
        }

        // 2. Annotations as geometry, so the preview on screen matches what gets exported.
        for (auto& s : strokes_) paint_stroke(s);
        if (drawing_) paint_stroke(cur_);
        if (text_edit_) paint_text(cur_, text_buffer_, true);
        if (crop_active_) {
            D2D1_ROUNDED_RECT rr = cv_.R2rr((std::min)(crop_a_.x, crop_b_.x) + img_org_x_,
                                            (std::min)(crop_a_.y, crop_b_.y) + img_org_y_,
                                            fabs((double)(crop_b_.x - crop_a_.x)),
                                            fabs((double)(crop_b_.y - crop_a_.y)), 2.0f);
            rt_->DrawRoundedRectangle(&rr, cv_.brAccent, 2.0f, cv_.solid);
        }

        rt_->PopAxisAlignedClip();

        // 3. The pill.
        paint_pill();

        // 4. Status strip: its top edge IS the separator (card #9), the image bottom sits
        //    PAD_BOTTOM above it, and every string of the strip lies below it.
        // With no vertical room for the strip (only possible if WM_GETMINMAXINFO was bypassed) the
        // strip is moved off the target instead of being painted over the toolbar.
        float stripY = strip_hidden_ ? (float)H + 400.0f : strip_y_;
        if (!strip_hidden_)
            rt_->DrawLine(D2D1_POINT_2F{ (float)t.s16, stripY },
                      D2D1_POINT_2F{ W - (float)t.s16, stripY }, cv_.brSoft);
        if (cv_.fmtCaption) {
            float ty = stripY + (float)t.s6, th = (float)(FOOT_H - 2 * t.s6);
            wchar_t buf[96];
            wsprintfW(buf, L"%d \x00D7 %d px", img_.w, img_.h);
            cv_.draw_layout(buf, cv_.fmtCaption, (float)t.s16, ty, 200.0f, th, cv_.brMuted,
                            DWRITE_TEXT_ALIGNMENT_LEADING);

            // Output confirmations live here (card #8): the editor stays open, so the only place
            // the result can be announced is this line.
            const wchar_t* msg = status_msg_.empty() ? nullptr : status_msg_.c_str();
            ID2D1SolidColorBrush* mb = cv_.brSecondary;   // 8.7 case 2: the path reads as textSecondary
            if (msg && status_bad_) mb = cv_.brAccent;
            if (msg) {
                float mw = cv_.measure(msg, cv_.fmtCaption);
                cv_.draw_layout(msg, cv_.fmtCaption, (std::max)((float)t.s16 + 210.0f, W - (float)t.s16 - mw),
                                ty, mw + 1.0f, th, mb, DWRITE_TEXT_ALIGNMENT_LEADING);
            } else {
                const wchar_t* hint = L"1-6 \x0438\x043D\x0441\x0442\x0440\x0443\x043C\x0435\x043D\x0442\x044B   "
                                      L"Ctrl+Z   Ctrl+Y   Ctrl+S   Ctrl+C   Esc";
                cv_.draw_layout(hint, cv_.fmtCaption, (float)t.s16 + 210.0f, ty,
                                W - 240.0f, th, cv_.brMuted, DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }

        HRESULT hr = rt_->EndDraw();
        if (hr != S_OK && hr != D2DERR_RECREATE_TARGET) {
            dbg("paint: EndDraw failed", hr);
            wprintf(L"EndDraw failed hr=0x%08X\n", (unsigned)hr);
        }
        if (memdc_ && dib_) {                 // the drawn frame reaches the window as a GDI blit
            RECT cr; GetClientRect(hwnd_, &cr);
            HDC hdc = GetDC(hwnd_);
            BitBlt(hdc, 0, 0, cr.right, cr.bottom, memdc_, 0, 0, SRCCOPY);
            ReleaseDC(hwnd_, hdc);
        }
    }

    // The chip is the only filled surface of a control (8.1): ctlH x ctlH, centred horizontally in
    // the cell and sitting in the top ctlH pixels of it. The glyph centres on the chip, never on
    // the cell - otherwise a 153 px signed cell would push its icon 48 px away from its caption.
    float chip_x(const Slot& s) const { return s.x + (s.w - theme_.ctlH) / 2.0f; }

    ui::ControlGroup group_of(const Slot& s) const {
        if (s.group == GRP_HISTORY) return ui::CG_PLAIN;    // pill surface + bright outline
        if (s.group == GRP_OUTPUT)  return ui::CG_SUNKEN;   // canvas recess
        return ui::CG_TOOL;                                 // tools, colours, properties
    }

    void paint_pill() {
        const Theme& t = theme_;
        D2D1_ROUNDED_RECT pr = ui::RR(pill_x_, pill_y_, (float)pill_w_, pill_h_, t.rFloating);
        rt_->FillRoundedRectangle(&pr, cv_.brSurface);
        rt_->DrawRoundedRectangle(&pr, cv_.brBorder, 1.0f, cv_.solid);

        const float capH = (float)caption_line();
        ID2D1SolidColorBrush* brCap = nullptr;
        cv_.brush(t.textSecondary, &brCap);

        for (auto& s : slots_) {
            const float cx = chip_x(s);
            ID2D1SolidColorBrush* cap_br = brCap;
            switch (s.kind) {
                case KIND_SEP:
                    rt_->DrawLine(D2D1_POINT_2F{ s.x, s.y }, D2D1_POINT_2F{ s.x, s.y + s.h }, cv_.brSoft);
                    continue;

                case KIND_TOOL: {
                    bool on = (s.tool == tool_);
                    ui::ControlState st = on ? ui::ST_ACTIVE : state_of(s);
                    cv_.control(cx, s.y, (float)t.ctlH, (float)t.ctlH, st, group_of(s));
                    cv_.glyph(s.glyph, s.id + 1, cx + t.ctlH / 2.0f, s.y + t.ctlH / 2.0f, st, cv_.brAccent);
                    if (on || st == ui::ST_HOVER) cap_br = cv_.brText;
                    break;
                }

                case KIND_ACTION: {
                    bool off = (s.action == 0 && undo_.empty()) || (s.action == 1 && redo_.empty()) ||
                               (s.action == 3 && !edited_);  // card #22: nothing edited, nothing to save
                    ui::ControlState st = off ? ui::ST_DISABLED : state_of(s);
                    cv_.control(cx, s.y, (float)t.ctlH, (float)t.ctlH, st, group_of(s));
                    // History has no accent state, but its resting glyph is the accent (8.2); output
                    // is neutral in every state - its ink is text, never the accent.
                    ID2D1SolidColorBrush* rest = (s.group == GRP_HISTORY) ? cv_.brAccent : cv_.brText;
                    cv_.glyph(s.glyph, s.id + 9, cx + t.ctlH / 2.0f, s.y + t.ctlH / 2.0f, st, rest);
                    if (st == ui::ST_HOVER) cap_br = cv_.brText;
                    break;
                }

                case KIND_COLOR: {
                    bool on = (s.color == color_);
                    bool hov = (hover_slot_ == slot_index(s));
                    ID2D1SolidColorBrush* cb = nullptr;
                    if (!cv_.brush(col2(s.color), &cb)) break;
                    // 8.2: a colour control has no chip at rest and no accent state - the selected
                    // colour is the ring around the swatch. The chip appears only under the pointer.
                    if (hov) cv_.control(cx, s.y, (float)t.ctlH, (float)t.ctlH, ui::ST_HOVER, ui::CG_PLAIN);
                    float side = 22.0f * t.k;
                    float sx = cx + (t.ctlH - side) / 2.0f, sy = s.y + (t.ctlH - side) / 2.0f;
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
                        rt_->DrawRoundedRectangle(&ring, cv_.brAccent, 1.5f, cv_.solid);
                    }
                    break;
                }

                case KIND_WIDTH: {
                    bool on = (s.width == width_);
                    cv_.control(cx, s.y, (float)t.ctlH, (float)t.ctlH,
                                on ? ui::ST_ACTIVE : state_of(s), group_of(s));
                    // The sample draws in the colour you are actually about to draw with, so
                    // the control previews the real stroke rather than a neutral grey tick.
                    D2D1_COLOR_F c = col2(color_);
                    if (!on) c.a = 0.55f;
                    ID2D1SolidColorBrush* ink = nullptr;
                    if (!cv_.brush(c, &ink)) ink = on ? cv_.brAccent : cv_.brMuted;
                    float half = t.ctlH * 0.34f, gx = cx + t.ctlH / 2.0f, gy = s.y + t.ctlH / 2.0f;
                    float thick = (1.5f + (float)std::max(1, s.width) * 0.75f) * t.k;
                    rt_->DrawLine(D2D1_POINT_2F{ gx - half, gy }, D2D1_POINT_2F{ gx + half, gy },
                                  ink, thick, cv_.solid);
                    if (on) cap_br = cv_.brText;
                    break;
                }

                case KIND_BACKDROP: {
                    // Card #7: the three text backdrops are swatches - same cell, same square,
                    // same ring as a colour swatch. Only fill, border and ring change (8.5).
                    bool on = (s.width == backdrop_);
                    bool hov = (hover_slot_ == slot_index(s));
                    cv_.control(cx, s.y, (float)t.ctlH, (float)t.ctlH,
                                on ? ui::ST_ACTIVE : state_of(s), group_of(s));
                    float side = 22.0f * t.k;
                    float sx = cx + (t.ctlH - side) / 2.0f, sy = s.y + (t.ctlH - side) / 2.0f;
                    D2D1_ROUNDED_RECT sq = ui::RR(sx, sy, side, side, 5.0f * t.k);
                    ID2D1SolidColorBrush* fb = nullptr;
                    if (s.width == 1) cv_.brush(t.textPrimary, &fb);
                    else if (s.width == 2) cv_.brush(t.canvas, &fb);
                    if (fb) rt_->FillRoundedRectangle(&sq, fb);
                    ID2D1SolidColorBrush* bb = nullptr;
                    cv_.brush(s.width == 1 ? t.borderSoft : t.borderStrong, &bb);
                    float bw = (s.width == 0) ? t.strokeW : 1.0f;
                    if (bb) {
                        rt_->DrawRoundedRectangle(&sq, bb, bw, cv_.solid);
                        if (s.width == 0)   // the "no fill" sign: contour plus one diagonal
                            rt_->DrawLine(D2D1_POINT_2F{ sx, sy }, D2D1_POINT_2F{ sx + side, sy + side },
                                          bb, t.strokeW, cv_.solid);
                    }
                    if (on) {
                        D2D1_ROUNDED_RECT ring = ui::RR(sx - 3.0f * t.k, sy - 3.0f * t.k,
                                                        side + 6.0f * t.k, side + 6.0f * t.k, 8.0f * t.k);
                        rt_->DrawRoundedRectangle(&ring, cv_.brAccent, 1.5f, cv_.solid);
                    }
                    if (on || hov) cap_br = cv_.brText;
                    break;
                }
            }

            // Second caption band: the control's own caption, under the group caption of the first
            // band (card #22). In the unsigned levels captions are empty and nothing moves.
            if (!s.caption.empty() && cv_.fmtCaption)
                cv_.draw_layout(s.caption.c_str(), cv_.fmtCaption, s.x,
                                s.y + t.ctlH + t.s4 + capH + t.s4,
                                s.w, capH, cap_br, DWRITE_TEXT_ALIGNMENT_CENTER,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Group captions stand in the same 25 px band, left-aligned to their own group, and cost
        // no width: the band is already paid for by the signed cells (8.1).
        int rows = rows_();
        for (int r = 0; r < rows; ++r) {
            float bandY = pill_y_ + t.s6 + r * (cell_h() + t.s6) + t.ctlH + t.s4;
            for (int g = 0; g < GRP_COUNT; ++g) {
                if (group_caption_[g].empty() || grp_w_[r][g] <= 0.0f) continue;
                // The unsaved-edits caption is the louder of the two wordings (card #22):
                // textSecondary on the pill surface is 7.36:1, and textTertiary would be 4.27:1 -
                // under AA for caption-size text, so the warning uses textPrimary.
                ID2D1SolidColorBrush* gb = (g == GRP_OUTPUT && dirty_) ? cv_.brText : brCap;
                cv_.draw_layout(group_caption_[g].c_str(), cv_.fmtCaption, grp_x_[r][g], bandY,
                                grp_w_[r][g], capH, gb, DWRITE_TEXT_ALIGNMENT_LEADING,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    static COLORREF to_rgb(const D2D1_COLOR_F& c) {
        auto q = [](float v) { if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; return (int)(v * 255.0f + 0.5f); };
        return RGB(q(c.r), q(c.g), q(c.b));
    }

    D2D1_COLOR_F backdrop_color(int mode) const {
        return mode == 1 ? theme_.textPrimary : theme_.canvas;   // 8.5: no new tokens
    }

    // Ink stays readable on the backdrop: only pairs under 3:1 are touched, and the replacement is
    // the opposite half of the same textPrimary/canvas pair (the three cases in 8.5).
    D2D1_COLOR_F readable_ink(D2D1_COLOR_F ink, int mode) const {
        D2D1_COLOR_F bg = backdrop_color(mode);
        if (Theme::contrast(ink, bg) >= 3.0f) return ink;
        bool light = Theme::lum(bg) > 0.5f;
        return Theme::mix(ink, light ? theme_.canvas : theme_.textPrimary, 0.75f);
    }

    // The anchor of a text stroke is the top-left of its layout box, which is also where the
    // export rasteriser puts it. Backdrop = that box inflated by s4, radius rControl (8.5).
    void text_box(const Stroke& s, const std::wstring& txt, float& x, float& y, float& w, float& h) const {
        x = img_org_x_ + (float)s.pts.front().x;
        y = img_org_y_ + (float)s.pts.front().y;
        w = cv_.measure(txt.c_str(), cv_.fmtBody);
        if (w < 1.0f) w = 1.0f;
        h = (float)(theme_.fsBody * 4 / 3);
    }

    void paint_text(const Stroke& s, const std::wstring& txt, bool caret) {
        if (txt.empty() || !cv_.fmtBody || s.pts.empty()) return;
        const Theme& t = theme_;
        float x, y, w, h;
        text_box(s, txt, x, y, w, h);
        D2D1_COLOR_F ink = col2(s.color);
        if (s.backdrop) {
            ID2D1SolidColorBrush* fb = nullptr;
            if (cv_.brush(backdrop_color(s.backdrop), &fb)) {
                D2D1_ROUNDED_RECT rr = cv_.R2rr(x - t.s4, y - t.s4, w + 2.0f * t.s4, h + 2.0f * t.s4,
                                                t.rControl);
                rt_->FillRoundedRectangle(&rr, fb);
            }
            ink = readable_ink(ink, s.backdrop);
        }
        ID2D1SolidColorBrush* ib = nullptr;
        if (!cv_.brush(ink, &ib)) return;
        cv_.draw_layout(txt.c_str(), cv_.fmtBody, x, y, w, h, ib, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (caret) {          // static caret: a blinking one is not needed to read "you are typing"
            rt_->DrawLine(D2D1_POINT_2F{ x + w + 2.0f, y }, D2D1_POINT_2F{ x + w + 2.0f, y + h },
                          cv_.brText, 1.5f * t.k, cv_.solid);
        }
    }

    // Strokes are drawn as geometry so the on-screen preview is antialiased.
    void paint_stroke(const Stroke& s) {
        if (s.pts.empty()) return;
        if (s.tool == TOOL_TEXT) { paint_text(s, s.text, false); return; }

        const Theme& t = theme_;
        D2D1_COLOR_F c = col2(s.color);
        if (s.tool == TOOL_MARKER) c.a = 0.45f;

        ID2D1SolidColorBrush* br = nullptr;
        if (!cv_.brush(c, &br)) return;
        const float w = (float)std::max(1, s.width) * t.k;

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
            if (text_edit_) commit_text();          // a control click keeps what has been typed
            hover_slot_ = slot_index(*s);
            switch (s->kind) {
                case KIND_TOOL:  set_tool(s->tool); return;
                case KIND_COLOR: color_ = s->color; InvalidateRect(hwnd_, nullptr, TRUE); return;
                case KIND_WIDTH: width_ = s->width; InvalidateRect(hwnd_, nullptr, TRUE); return;
                case KIND_BACKDROP: backdrop_ = s->width; InvalidateRect(hwnd_, nullptr, TRUE); return;
                case KIND_ACTION: do_action(s->action); return;
                default: break;
            }
        }

        // Card #24: under the visible band there is no picture to draw on. Strokes started there
        // used to be invisible on screen and still ended up in the exported file.
        if (!on_visible_image(mx, my)) return;

        if (tool_ == TOOL_CROP) {
            crop_active_ = true;
            crop_a_ = { imgX(mx), imgY(my) };
            crop_b_ = crop_a_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (tool_ == TOOL_TEXT) { start_text(mx, my); return; }

        drawing_ = true;
        text_buffer_.clear();
        cur_ = Stroke{ tool_, { { imgX(mx), imgY(my) } }, color_, width_, L"", backdrop_ };
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Card #6: text is click -> type -> Enter, not drag. The click that places the caret is
    // released long before the first key is typed, so mouse-up must not end the edit.
    // Card #21: a click while a label is being typed finishes that label and places a NEW one at
    // the clicked point. Moving the anchor instead (what this did) meant the text being typed
    // jumped to every new click and a second label was impossible without switching tools.
    void start_text(int mx, int my) {
        if (text_edit_) commit_text();
        text_edit_ = true;
        text_buffer_.clear();
        cur_ = Stroke{ TOOL_TEXT, { { imgX(mx), imgY(my) } }, color_, width_, L"", backdrop_ };
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void commit_text() {
        if (!text_edit_) return;
        text_edit_ = false;
        cur_.text = text_buffer_;
        text_buffer_.clear();
        if (!cur_.text.empty()) commit_stroke();
        cur_ = Stroke{};
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void cancel_text() {
        text_edit_ = false;
        text_buffer_.clear();
        cur_ = Stroke{};
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void commit_stroke() {
        strokes_.push_back(cur_);
        Change c;
        c.stroke = cur_;
        undo_.push_back(c);
        redo_.clear();
        mark_edited();
    }

    // Card #22, as the owner settled it: two questions, two counters.
    //   edited_ - the document differs from the captured frame: that is what makes Save worth having
    //             at all, and it is never cleared by an output (a copied frame can still be saved).
    //   dirty_  - edits that have not left the editor yet: what the output caption shows and what
    //             closing the editor has to ask about. Copy and Save clear it, nothing else does.
    void mark_edited() { edited_ = true; set_dirty(true); }

    void set_dirty(bool d) {
        if (dirty_ == d) return;
        dirty_ = d;
        layout();                     // the caption text lives in the slots, not only in the painter
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void do_action(int a) {
        if (a == 0 || a == 1) {
            std::vector<Change>& from = (a == 0) ? undo_ : redo_;
            std::vector<Change>& to = (a == 0) ? redo_ : undo_;
            if (from.empty()) return;
            Change c = from.back();          // copied, not moved: both sides stay undoable
            from.pop_back();
            if (c.is_crop) {
                img_ = (a == 0) ? c.img_before : c.img_after;
                strokes_ = (a == 0) ? c.st_before : c.st_after;
                refresh_image_bitmap();
                ensure_image_visible();
            } else if (a == 0) {
                if (!strokes_.empty()) strokes_.pop_back();
            } else {
                strokes_.push_back(c.stroke);
            }
            to.push_back(std::move(c));
            mark_edited();                        // an undo changes the result too (#22)
        } else if (a == 2) {                                    // clipboard only, no file (8.7)
            if (copy_fn_ && copy_fn_(compose_result())) {
                produced_ = true; set_dirty(false);   // the edits have left the editor (#22)
                status_msg_ = L"Скопировано в буфер обмена"; status_bad_ = false;
            } else {
                status_msg_ = L"Не удалось скопировать"; status_bad_ = true;
            }
        } else if (a == 3) {                                    // file, and the window stays open
            if (!save_fn_ || !edited_) return;                   // Save is disabled in that state (#22)
            std::wstring path;
            OutputResult r = save_fn_(compose_result(), hwnd_, path);
            if (r == OUT_SAVED) {
                produced_ = true; dirty_ = false;
                status_msg_ = L"Сохранено: " + path; status_bad_ = false;
            } else if (r == OUT_CANCELLED) {
                status_msg_ = L"Сохранение отменено"; status_bad_ = false;
            } else {
                status_msg_ = L"Не удалось сохранить: " + path; status_bad_ = true;
            }
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // Card #22: closing with unsaved edits asks instead of deciding for the user. TaskDialog and not
    // MessageBox because the three answers are the product's own words, and the acceptance harness
    // drives dialogs through BM_CLICK - the same route every other dialog in this project uses.
    bool ask_discard() {
        const int ID_SAVE = 100, ID_DROP = 101, ID_CANCEL = 102;
        TASKDIALOG_BUTTON btns[3] = { { ID_SAVE, L"Сохранить" }, { ID_DROP, L"Не сохранять" },
                                      { ID_CANCEL, L"Отмена" } };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof(c);
        c.hwndParent = hwnd_;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = L"Закрыть редактор?";
        c.pszMainInstruction = L"Есть несохранённые правки. Сохранить их перед закрытием?";
        c.pButtons = btns;
        c.cButtons = 3;
        c.nDefaultButton = ID_CANCEL;               // the safe answer is the one Enter gives
        int pressed = ID_CANCEL;
        if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return false;
        if (pressed == ID_SAVE) {
            do_action(3);                           // the ordinary Save path, folder dialog included
            return !dirty_;                         // a cancelled or failed save keeps the window open
        }
        if (pressed == ID_DROP) { discard_confirmed_ = true; return true; }
        return false;
    }

    // The band the picture actually occupies on screen: between the pill and the strip.
    bool on_visible_image(int mx, int my) const {
        return (float)my >= img_top_ && (float)my <= img_bottom_;
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
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (!drawing_ || text_edit_) return;
        drawing_ = false;
        on_move(mx, my);
        if (cur_.pts.size() >= 2) commit_stroke();
        cur_ = Stroke{};
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // Ctrl state is read from this window's own key messages. GetKeyState reports the physical
    // keyboard of whoever owns it, so a Ctrl shortcut could only be produced by injecting keys into
    // the whole session - which is what made every automated check fight the owner for the keyboard
    // (card #21). Real presses and posted ones both arrive through this window's queue.
    void on_key_up(int vk) { if (vk == VK_CONTROL) ctrl_held_ = false; }

    void on_key(int vk) {
        if (vk == VK_CONTROL) { ctrl_held_ = true; return; }
        bool ctrl = ctrl_held_ || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (text_edit_ && !ctrl) {
            if (vk == VK_RETURN) { commit_text(); return; }
            if (vk == VK_ESCAPE) { cancel_text(); return; }   // Esc closes the text, not the editor
        }
        if (ctrl && vk == 'Z') { do_action(0); return; }
        if (ctrl && vk == 'Y') { do_action(1); return; }
        if (ctrl && vk == 'S') { do_action(3); return; }
        if (ctrl && vk == 'C') { do_action(2); return; }
        if (vk == VK_ESCAPE) { PostMessageW(hwnd_, WM_CLOSE, 0, 0); return; }   // one closing path (#22)
        if (vk == VK_DELETE) {
            if (!undo_.empty() && !undo_.back().is_crop) do_action(0);
            return;
        }
        if (!drawing_ && !text_edit_) {
            const int tools[6] = { TOOL_ARROW, TOOL_TEXT, TOOL_RECT, TOOL_ELLIPSE, TOOL_MARKER, TOOL_CROP };
            if (vk >= '1' && vk <= '6') set_tool((Tool)tools[vk - '1']);
        }
    }

    void on_char(wchar_t c) {
        if (!text_edit_) return;
        if (c == 8 && !text_buffer_.empty()) { text_buffer_.pop_back(); InvalidateRect(hwnd_, nullptr, TRUE); return; }
        if (c == 3) return;                       // Ctrl+C is an action, not text
        if (c >= 32) { text_buffer_ += c; InvalidateRect(hwnd_, nullptr, TRUE); }
    }

    int imgX(int mx) const { return (std::max)(0, (std::min)(img_.w - 1, mx - img_org_x_)); }
    int imgY(int my) const { return (std::max)(0, (std::min)(img_.h - 1, my - img_org_y_)); }

    // A crop is one history step carrying both sides of itself (card #5): undoing it has to bring
    // back the pixels it removed and the annotations that were rebased into it.
    void apply_crop(POINT a, POINT b) {
        int x0 = (std::min)(a.x, b.x), x1 = (std::max)(a.x, b.x);
        int y0 = (std::min)(a.y, b.y), y1 = (std::max)(a.y, b.y);
        if (x1 - x0 < 8 || y1 - y0 < 8) return;

        Change c;
        c.is_crop = true;
        c.img_before = img_;
        c.st_before = strokes_;

        DIB out;
        out.w = x1 - x0; out.h = y1 - y0;
        out.rgba.assign((size_t)out.w * out.h * 4, 0);
        for (int y = 0; y < out.h; ++y)
            memcpy(&out.rgba[(size_t)y * out.w * 4], img_.px(0, y0 + y), (size_t)out.w * 4);

        std::vector<Stroke> kept;
        for (auto& s : strokes_) {
            bool inside = true;
            for (auto& p : s.pts) if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) { inside = false; break; }
            if (inside) { for (auto& p : s.pts) { p.x -= x0; p.y -= y0; } kept.push_back(s); }
        }
        img_ = std::move(out);
        strokes_ = std::move(kept);
        c.img_after = img_;
        c.st_after = strokes_;

        undo_.push_back(std::move(c));
        redo_.clear();
        refresh_image_bitmap();
        mark_edited();                // a crop is an edit (#22)
    }

    // Undoing a crop puts back pixels that are larger than the window may currently be.
    void ensure_image_visible() {
        if (!hwnd_) return;
        RECT cr; GetClientRect(hwnd_, &cr);
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        layout();
        int needW = (std::max)(img_.w + 2 * PAD, pill_w_ + theme_.s24);
        int needH = theme_.s12 + (int)pill_h_ + PAD_TOP + img_.h + PAD_BOTTOM + FOOT_H;
        if (cw >= needW && ch >= needH) return;
        int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
        int w = (std::min)((std::max)(cw, needW) + frame_w_, scrW - 8);
        int h = (std::min)((std::max)(ch, needH) + frame_h_, scrH - 8);
        SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        dbg("ensure_image_visible: grew window");
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
    // Exported text is real font text, not a filled bar: 0.1 painted a solid rectangle and the
    // harness could only ever see "there are coloured pixels here". GDI is used because the export
    // target is a plain DIB; the same readable_ink()/backdrop numbers as the preview are applied,
    // so the file and the screen agree.
    void draw_text_img(DIB& d, const Stroke& s) {
        if (s.text.empty()) return;
        const Theme& t = theme_;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = d.w;
        bi.bmiHeader.biHeight = -d.h;                 // top-down: (0,0) is the image top-left
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP oldbmp = bmp ? (HBITMAP)SelectObject(dc, bmp) : nullptr;
        size_t n = (size_t)d.w * d.h;
        if (bmp && bits) {
            uint8_t* px = (uint8_t*)bits;
            for (size_t i = 0; i < n; ++i) {          // RGBA -> BGRA, GDI ignores alpha
                px[i * 4 + 0] = d.rgba[i * 4 + 2];
                px[i * 4 + 1] = d.rgba[i * 4 + 1];
                px[i * 4 + 2] = d.rgba[i * 4 + 0];
                px[i * 4 + 3] = 0;
            }
            HFONT font = CreateFontW(-t.fsBody, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT oldfont = (HFONT)SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SIZE ts = { 0, 0 };
            GetTextExtentPoint32W(dc, s.text.c_str(), (int)s.text.size(), &ts);
            int bx = s.pts.front().x, by = s.pts.front().y;
            int bw = (std::max)((int)ts.cx, 1), bh = (std::max)((int)ts.cy, t.fsBody * 4 / 3);
            D2D1_COLOR_F ink = col2(s.color);
            if (s.backdrop) {
                COLORREF bc = to_rgb(backdrop_color(s.backdrop));
                HBRUSH br = CreateSolidBrush(bc), oldbr = (HBRUSH)SelectObject(dc, br);
                HPEN pen = CreatePen(PS_SOLID, 1, bc), oldpen = (HPEN)SelectObject(dc, pen);
                int rd = (int)(5.0f * t.k);
                RoundRect(dc, bx - t.s4, by - t.s4, bx + bw + t.s4, by + bh + t.s4, rd * 2, rd * 2);
                SelectObject(dc, oldpen); DeleteObject(pen);
                SelectObject(dc, oldbr); DeleteObject(br);
                ink = readable_ink(ink, s.backdrop);
            }
            SetTextColor(dc, to_rgb(ink));
            RECT r{ bx, by, bx + bw, by + bh };
            DrawTextW(dc, s.text.c_str(), (int)s.text.size(), &r,
                      DT_NOCLIP | DT_LEFT | DT_TOP | DT_SINGLELINE);
            SelectObject(dc, oldfont); DeleteObject(font);
            uint8_t* src = (uint8_t*)bits;
            for (size_t i = 0; i < n; ++i) {          // BGRA -> RGBA
                d.rgba[i * 4 + 0] = src[i * 4 + 2];
                d.rgba[i * 4 + 1] = src[i * 4 + 1];
                d.rgba[i * 4 + 2] = src[i * 4 + 0];
            }
        }
        if (oldbmp) SelectObject(dc, oldbmp);
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
    }

    void draw_stroke_img(DIB& d, const Stroke& s) {
        if (s.pts.empty()) return;
        if (s.tool == TOOL_TEXT) { draw_text_img(d, s); return; }
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
    ID2D1RenderTarget* rt_ = nullptr;      // a DC target: D2D draws into memdc_/dib_
    HDC memdc_ = nullptr;
    HBITMAP dib_ = nullptr;
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

    std::vector<Stroke> strokes_;              // what is currently on the image
    std::vector<Change> undo_, redo_;          // committed steps, crops included
    Stroke cur_;
    bool drawing_ = false, crop_active_ = false, text_edit_ = false;
    bool ctrl_held_ = false;                    // Ctrl as this window saw it, not as GetKeyState says
    POINT crop_a_{}, crop_b_{};
    std::wstring text_buffer_;
    int hover_slot_ = -1;
    int backdrop_ = 0;                          // 0 none / 1 light / 2 dark, applies to new text
    std::wstring status_msg_;
    bool status_bad_ = false;
    bool produced_ = false;                     // at least one Save/Copy happened in this session
    bool dirty_ = false;                        // edits that have not left through Save/Copy yet (#22)
    bool edited_ = false;                       // the document differs from the captured frame (#22)
    bool discard_confirmed_ = false;            // the user already answered "Не сохранять" (#22)
    SaveFn save_fn_ = nullptr;
    CopyFn copy_fn_ = nullptr;
    int frame_w_ = 0, frame_h_ = 0;
};
