// ui_draw.h - Direct2D drawing layer: control states + a vector glyph library.
//
// Icons are built as geometry on a 24-unit slot with a 20-unit drawing box, 1.6px stroke,
// round caps and joins, and a consistent optical weight. They are cached per control slot and
// rebuilt only when the window size or DPI changes.
//
// Depth strategy for this product: borders only (no drop shadows). On a dark surface shadows
// barely read, so elevation comes from the surface ramp in theme.h plus a border.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <map>
#include <string>
#include "theme.h"

namespace ui {

typedef ID2D1RenderTarget RT;

enum Glyph {
    G_ARROW = 0, G_TEXT, G_RECT, G_ELLIPSE, G_MARKER, G_CROP,
    G_UNDO, G_REDO, G_SAVE, G_COPY, G_OPEN, G_BLUR, G_TRASH, G_COUNT
};

enum ControlState { ST_NORMAL = 0, ST_HOVER, ST_ACTIVE, ST_PRESSED, ST_DISABLED };

// Build a full ellipse out of four cubic beziers (no arc API needed).
inline void add_ellipse(ID2D1GeometrySink* s, float cx, float cy, float rx, float ry) {
    const float K = 0.5522847498f;
    s->BeginFigure(D2D1_POINT_2F{ cx + rx, cy }, D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddBezier(D2D1_BEZIER_SEGMENT{ { cx + rx, cy + ry * K }, { cx + rx * K, cy + ry }, { cx, cy + ry } });
    s->AddBezier(D2D1_BEZIER_SEGMENT{ { cx - rx * K, cy + ry }, { cx - rx, cy + ry * K }, { cx - rx, cy } });
    s->AddBezier(D2D1_BEZIER_SEGMENT{ { cx - rx, cy - ry * K }, { cx - rx * K, cy - ry }, { cx, cy - ry } });
    s->AddBezier(D2D1_BEZIER_SEGMENT{ { cx + rx * K, cy - ry }, { cx + rx, cy - ry * K }, { cx + rx, cy } });
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
}

inline D2D1_RECT_F R(float x, float y, float w, float h) {
    D2D1_RECT_F r; r.left = x; r.top = y; r.right = x + w; r.bottom = y + h; return r;
}
inline D2D1_ROUNDED_RECT RR(float x, float y, float w, float h, float rad) {
    D2D1_ROUNDED_RECT r; r.rect = R(x, y, w, h); r.radiusX = rad; r.radiusY = rad; return r;
}

class Canvas {
public:
    RT* rt = nullptr;
    ID2D1Factory* fac = nullptr;
    const Theme* th = nullptr;

    IDWriteFactory* dw_ = nullptr;

    ID2D1StrokeStyle* solid = nullptr;    // round caps + round joins
    ID2D1StrokeStyle* dashed = nullptr;   // 6/4 custom dash, round caps

    ID2D1SolidColorBrush *brText = nullptr, *brMuted = nullptr, *brAccent = nullptr,
                          *brSurface = nullptr, *brBorder = nullptr, *brHover = nullptr,
                          *brFill = nullptr, *brFocus = nullptr, *brSoft = nullptr;

    IDWriteTextFormat *fmtCaption = nullptr, *fmtBody = nullptr, *fmtLabel = nullptr;

    // Returns the first failing sub-call so the caller can log the real cause instead of
    // crashing later on a null brush / null text format.
    HRESULT init(RT* target, ID2D1Factory* factory, IDWriteFactory* write_factory, const Theme* theme) {
        rt = target; fac = factory; dw_ = write_factory; th = theme;
        HRESULT worst = S_OK;

        D2D1_STROKE_STYLE_PROPERTIES p = {};
        p.startCap = D2D1_CAP_STYLE_ROUND;
        p.endCap   = D2D1_CAP_STYLE_ROUND;
        p.dashCap  = D2D1_CAP_STYLE_ROUND;
        p.lineJoin = D2D1_LINE_JOIN_ROUND;
        p.dashStyle = D2D1_DASH_STYLE_SOLID;
        { HRESULT hr = fac->CreateStrokeStyle(&p, nullptr, 0, &solid); if (FAILED(hr) && SUCCEEDED(worst)) worst = hr; }

        p.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        float dash[] = { 6.0f, 4.0f };
        { HRESULT hr = fac->CreateStrokeStyle(&p, dash, 2, &dashed); if (FAILED(hr) && SUCCEEDED(worst)) worst = hr; }

        ID2D1SolidColorBrush* brs[9] = {};
        const D2D1_COLOR_F cols[9] = { th->textPrimary, th->textTertiary, th->accentGlyph, th->floating,
                                       th->borderStrong, th->controlHover, th->accentFill, th->focusRing,
                                       th->borderSoft };
        for (int i = 0; i < 9; ++i) {
            HRESULT hr = rt->CreateSolidColorBrush(cols[i], &brs[i]);
            if (FAILED(hr) && SUCCEEDED(worst)) worst = hr;
        }
        brText = brs[0]; brMuted = brs[1]; brAccent = brs[2]; brSurface = brs[3];
        brBorder = brs[4]; brHover = brs[5]; brFill = brs[6]; brFocus = brs[7]; brSoft = brs[8];

        auto mk = [&](DWRITE_FONT_WEIGHT w, FLOAT size, IDWriteTextFormat** out) {
            HRESULT hr = dw_->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
            if (FAILED(hr)) { *out = nullptr; if (SUCCEEDED(worst)) worst = hr; }
        };
        DWRITE_FONT_WEIGHT light  = DWRITE_FONT_WEIGHT_LIGHT;   // unused, keeps enum referenced
        (void)light;
        mk(DWRITE_FONT_WEIGHT_NORMAL, (FLOAT)th->fsCaption, &fmtCaption);
        mk(DWRITE_FONT_WEIGHT_SEMI_BOLD, (FLOAT)th->fsBody,  &fmtBody);
        mk(DWRITE_FONT_WEIGHT_BOLD, (FLOAT)th->fsH4,   &fmtLabel);

        return worst;
    }

    void shutdown() {
        clear_cache();
        auto rel = [](IUnknown* p) { if (p) p->Release(); };
        rel((IUnknown*)solid); rel((IUnknown*)dashed);
        rel((IUnknown*)brText); rel((IUnknown*)brMuted); rel((IUnknown*)brAccent);
        rel((IUnknown*)brSurface); rel((IUnknown*)brBorder); rel((IUnknown*)brHover);
        rel((IUnknown*)brFill); rel((IUnknown*)brFocus); rel((IUnknown*)brSoft);
        rel((IUnknown*)fmtCaption); rel((IUnknown*)fmtBody); rel((IUnknown*)fmtLabel);
        solid = dashed = nullptr;
        brText = brMuted = brAccent = brSurface = brBorder = brHover = brFill = brFocus = brSoft = nullptr;
        fmtCaption = fmtBody = fmtLabel = nullptr;
    }

    void clear_cache() {
        for (auto& kv : cache) {
            if (kv.second.line) kv.second.line->Release();
            if (kv.second.fill) kv.second.fill->Release();
        }
        cache.clear();
    }

    // ---- surfaces --------------------------------------------------------------------
    void surface(const D2D1_RECT_F& r, float radius, ID2D1SolidColorBrush* fill,
                 ID2D1SolidColorBrush* border) {
        D2D1_ROUNDED_RECT rr; rr.rect = r; rr.radiusX = radius; rr.radiusY = radius;
        rt->FillRoundedRectangle(&rr, fill);
        if (border) rt->DrawRoundedRectangle(&rr, border, 1.0f, solid);
    }

    void hline(float x0, float x1, float y, ID2D1SolidColorBrush* b) {
        rt->DrawLine(D2D1_POINT_2F{ x0, y }, D2D1_POINT_2F{ x1, y }, b);
    }

    void control(float x, float y, float w, float h, ControlState st) {
        if (!rt || !th) return;
        D2D1_COLOR_F fillc = th->control;
        ID2D1SolidColorBrush* brush = nullptr;
        switch (st) {
            case ST_NORMAL:   fillc = th->control;        break;
            case ST_HOVER:    fillc = th->controlHover;   break;
            case ST_PRESSED:  fillc = th->controlPressed; break;
            case ST_ACTIVE:   fillc = th->accentFill;     break;
            case ST_DISABLED: fillc = th->canvas;         break;   // disabled reads as a recess
        }
        if (!cache_brush(fillc, &brush)) return;
        D2D1_ROUNDED_RECT rr = R2(x, y, w, h, th->rControl);
        rt->FillRoundedRectangle(&rr, brush);
        if (st == ST_ACTIVE) rt->DrawRoundedRectangle(&rr, brAccent, 1.5f, solid);
        else                 rt->DrawRoundedRectangle(&rr, brSoft, 1.0f, solid);
    }

    void text(const wchar_t* s, float x, float y, float w, float h,
              IDWriteTextFormat* fmt, ID2D1SolidColorBrush* brush,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER) {
        if (!fmt || !brush) return;
        IDWriteTextFormat* f = fmt;
        if (align != DWRITE_TEXT_ALIGNMENT_LEADING && fmt == fmtBody) {
            // fmtBody is created as centred below; keep it simple and just set alignment here.
        }
        D2D1_RECT_F r = R(x, y, w, h);
        rt->DrawText(s, (UINT)wcslen(s), f, &r, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // ---- glyphs ----------------------------------------------------------------------
    void glyph(int g, int slot, float cx, float cy, ControlState st) {
        if (!rt || !th || !fac) return;
        ID2D1SolidColorBrush* ink = (st == ST_DISABLED) ? brMuted
                              : (st == ST_ACTIVE)       ? brAccent : brText;
        Geo geo = geometry_for(g, slot, cx, cy);
        if (geo.line) rt->DrawGeometry(geo.line, ink, th->strokeW, solid);
        if (geo.fill) rt->FillGeometry(geo.fill, ink);
    }

    void dashed_glyph(int g, int slot, float cx, float cy, ControlState st) {
        if (!rt || !th || !fac) return;
        ID2D1SolidColorBrush* ink = (st == ST_ACTIVE) ? brAccent : brText;
        Geo geo = geometry_for(g, slot, cx, cy);
        if (geo.line) rt->DrawGeometry(geo.line, ink, th->strokeW, dashed);
        if (geo.fill) rt->FillGeometry(geo.fill, ink);
    }


    static D2D1_ROUNDED_RECT R2rr(float x, float y, float w, float h, float rad) {
        D2D1_ROUNDED_RECT r; r.rect = R(x, y, w, h); r.radiusX = rad; r.radiusY = rad; return r;
    }

    // Shared colored brushes, created once per color. Used by the editor for swatches and
    // annotation strokes so we are not creating a brush on every draw call.
    bool brush(D2D1_COLOR_F c, ID2D1SolidColorBrush** out) { return cache_brush(c, out); }

    void ellipse_body(ID2D1GeometrySink* s, float cx, float cy, float rx, float ry) {
        add_ellipse(s, cx, cy, rx, ry);
    }

private:
    struct Geo { ID2D1PathGeometry* line = nullptr; ID2D1PathGeometry* fill = nullptr; };
    std::map<int, Geo> cache;
    std::map<std::string, ID2D1SolidColorBrush*> brush_cache;

    D2D1_ROUNDED_RECT R2(float x, float y, float w, float h, float rad) {
        D2D1_ROUNDED_RECT r; r.rect = R(x, y, w, h); r.radiusX = rad; r.radiusY = rad; return r;
    }

    bool cache_brush(D2D1_COLOR_F c, ID2D1SolidColorBrush** out) {
        if (!rt) return false;
        char key[48];
        wsprintfA(key, "%d_%d_%d_%d", (int)(c.r * 255), (int)(c.g * 255),
                  (int)(c.b * 255), (int)(c.a * 255));
        auto it = brush_cache.find(key);
        if (it != brush_cache.end()) { *out = it->second; return true; }
        ID2D1SolidColorBrush* b = nullptr;
        if (FAILED(rt->CreateSolidColorBrush(c, &b)) || !b) return false;
        brush_cache[key] = b;
        *out = b;
        return true;
    }

    Geo geometry_for(int g, int slot, float cx, float cy) {
        int key = g * 4096 + slot;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;

        Geo geo;
        fac->CreatePathGeometry(&geo.line);
        ID2D1GeometrySink* s = nullptr;
        if (geo.line && SUCCEEDED(geo.line->Open(&s))) { build_line(g, s, cx, cy); s->Close(); }

        if (needs_fill(g)) {
            fac->CreatePathGeometry(&geo.fill);
            ID2D1GeometrySink* s2 = nullptr;
            if (geo.fill && SUCCEEDED(geo.fill->Open(&s2))) { build_fill(g, s2, cx, cy); s2->Close(); }
        }
        cache[key] = geo;
        return geo;
    }

    static bool needs_fill(int g) {
        return g == G_ARROW || g == G_UNDO || g == G_REDO || g == G_MARKER;
    }

    // All glyphs are centred on (cx,cy) inside a box of `b` = glyphSize.
    void build_line(int g, ID2D1GeometrySink* s, float cx, float cy) {
        const float b = (float)th->glyphSize;
        const float h = b * 0.5f;
        const float L = cx - h, T = cy - h, Rt = cx + h, B = cy + h;

        switch (g) {
            case G_ARROW: {
                // Shaft from lower-left to upper-right, head at the upper-right corner.
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.06f, B - b * 0.06f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.22f, cy - h + b * 0.22f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_TEXT: {
                // "A": two legs and a crossbar, drawn with strokes so the weight matches the other tools.
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.36f, B }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx, T });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.36f, B });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.20f, cy + b * 0.14f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.20f, cy + b * 0.14f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_RECT:
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.08f, T + b * 0.14f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.08f, T + b * 0.14f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.08f, B - b * 0.14f });
                s->AddLine(D2D1_POINT_2F{ L + b * 0.08f, B - b * 0.14f });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            case G_ELLIPSE:
                add_ellipse(s, cx, cy, h * 0.92f, h * 0.70f);
                break;
            case G_MARKER: {
                // Highlighter: a slanted wide bar with a chisel tip.
                float w = b * 0.26f;
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.12f, B - b * 0.10f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx + w * 0.5f, cy - b * 0.10f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.06f, T + b * 0.10f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                (void)w;
                break;
            }
            case G_CROP: {
                // Crop marks: a vertical stroke whose foot turns right, and a horizontal
                // stroke whose right end turns down. No diagonal - it read as a letter.
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.17f, T + b * 0.05f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.17f, B - b * 0.17f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.32f, B - b * 0.17f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.17f, cy - b * 0.17f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.05f, cy - b * 0.17f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.05f, cy + b * 0.32f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_UNDO:
            case G_REDO: {
                float dir = (g == G_UNDO) ? 1.0f : -1.0f;
                float y0 = cy + b * 0.20f;
                s->BeginFigure(D2D1_POINT_2F{ cx + dir * b * 0.40f, y0 }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddBezier(D2D1_BEZIER_SEGMENT{
                    { cx + dir * b * 0.42f, cy - b * 0.30f },
                    { cx - dir * b * 0.10f, cy - b * 0.42f },
                    { cx - dir * b * 0.40f, cy - b * 0.06f } });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_SAVE: {
                // Down arrow into a tray.
                s->BeginFigure(D2D1_POINT_2F{ cx, T + b * 0.10f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx, cy + b * 0.14f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.12f, cy + b * 0.12f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ L + b * 0.12f, B });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.12f, B });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.12f, cy + b * 0.12f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_COPY: {
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.10f, T + b * 0.06f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.06f, T + b * 0.06f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.06f, cy + b * 0.10f });
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.10f, cy + b * 0.10f });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.06f, cy + b * 0.16f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.16f, cy + b * 0.16f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.16f, B });
                s->AddLine(D2D1_POINT_2F{ L + b * 0.06f, B });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            case G_OPEN: {
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.08f, cy - b * 0.16f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.02f, cy - b * 0.16f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.10f, cy - b * 0.30f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.08f, cy - b * 0.30f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.08f, cy - b * 0.16f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ L + b * 0.08f, B - b * 0.08f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.08f, B - b * 0.08f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.08f, cy - b * 0.02f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.02f, cy - b * 0.02f });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            case G_BLUR: {
                // Three stacked soft bars of decreasing weight reads as "pixelate".
                float xs[4] = { 0.16f, 0.34f, 0.52f, 0.70f };
                for (int i = 0; i < 4; ++i) {
                    float y = T + b * (0.22f + 0.19f * i);
                    s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.36f + b * xs[i] * 0.6f, y }, D2D1_FIGURE_BEGIN_HOLLOW);
                    s->AddLine(D2D1_POINT_2F{ cx + b * 0.36f, y });
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                }
                break;
            }
            case G_TRASH: {
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.30f, cy - b * 0.22f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.30f, cy - b * 0.22f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.22f, cy - b * 0.10f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.18f, B - b * 0.08f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.18f, B - b * 0.08f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.22f, cy - b * 0.10f });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            default: break;
        }
    }

    void build_fill(int g, ID2D1GeometrySink* s, float cx, float cy) {
        const float b = (float)th->glyphSize;
        const float h = b * 0.5f;
        switch (g) {
            case G_ARROW: {
                // Solid triangular head at the end of the shaft.
                float tipx = cx + h * 0.96f, tipy = cy - h * 0.96f;
                float back = b * 0.34f;
                s->BeginFigure(D2D1_POINT_2F{ tipx, tipy }, D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1_POINT_2F{ tipx - back, tipy + back * 0.35f });
                s->AddLine(D2D1_POINT_2F{ tipx - back * 0.35f, tipy + back });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            case G_UNDO:
            case G_REDO: {
                float dir = (g == G_UNDO) ? 1.0f : -1.0f;
                float px = cx + dir * b * 0.40f, py = cy + b * 0.20f;
                float sz = b * 0.20f;
                s->BeginFigure(D2D1_POINT_2F{ px, py - sz }, D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1_POINT_2F{ px - dir * sz * 1.1f, py });
                s->AddLine(D2D1_POINT_2F{ px, py + sz });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            case G_MARKER: {
                // Filled highlighter body.
                float w = b * 0.30f;
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.40f, cy + b * 0.34f }, D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.40f + w, cy + b * 0.34f - w });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.16f, cy - b * 0.02f });
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.24f, cy + b * 0.06f });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            default: break;
        }
    }
};

} // namespace ui
