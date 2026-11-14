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
#include <cmath>
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

// Which group of the toolbar a control belongs to: tools (raised chip), history (flat, outline
// only), output (recessed niche). Groups must read apart from each other without a caption.
enum ControlGroup { CG_TOOL = 0, CG_PLAIN, CG_SUNKEN };

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

// Undo and redo are one arc drawn twice: once counter-clockwise with the head at its
// upper-left end, once as its mirror. Sampling the arc is simpler than a bezier and at
// toolbar glyph size it reads as a curve. The parameters live in one place so the stroke
// and the filled head cannot drift apart.
struct ArcSpec {
    float cx, cy, r, dir, v0, v1;

    D2D1_POINT_2F at(float deg) const {
        float a = deg * 3.14159265358979f / 180.0f;
        D2D1_POINT_2F p; p.x = cx + dir * r * cosf(a); p.y = cy - r * sinf(a); return p;
    }
    // Unit tangent in the direction of the sweep.
    D2D1_POINT_2F tangent(float deg) const {
        float a = deg * 3.14159265358979f / 180.0f;
        float tx = -dir * sinf(a), ty = -cosf(a);
        float m = sqrtf(tx * tx + ty * ty);
        if (m < 1e-6f) m = 1.0f;
        D2D1_POINT_2F t; t.x = tx / m; t.y = ty / m; return t;
    }
};

inline ArcSpec arc_for(int g, float cx, float cy, float b) {
    ArcSpec a;
    a.dir = (g == G_UNDO) ? 1.0f : -1.0f;      // redo is undo seen in a mirror
    a.cx = cx + a.dir * b * 0.04f;
    a.cy = cy + b * 0.04f;
    a.r = b * 0.34f;
    a.v0 = -25.0f;                              // tail, low on the tail side
    a.v1 = 160.0f;                              // head, high on the head side
    return a;
}

inline void add_arc(ID2D1GeometrySink* s, const ArcSpec& a) {
    const int steps = 16;
    D2D1_POINT_2F p = a.at(a.v0);
    s->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i <= steps; ++i) s->AddLine(a.at(a.v0 + (a.v1 - a.v0) * (float)i / (float)steps));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
}

inline void add_arc_head(ID2D1GeometrySink* s, const ArcSpec& a, float b) {
    D2D1_POINT_2F e = a.at(a.v1);
    D2D1_POINT_2F t = a.tangent(a.v1);
    D2D1_POINT_2F n; n.x = -t.y; n.y = t.x;
    float hl = b * 0.24f, hw = b * 0.16f;
    s->BeginFigure(D2D1_POINT_2F{ e.x + t.x * hl, e.y + t.y * hl }, D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(D2D1_POINT_2F{ e.x + n.x * hw, e.y + n.y * hw });
    s->AddLine(D2D1_POINT_2F{ e.x - n.x * hw, e.y - n.y * hw });
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
}

// Chisel highlighter, built as a tool and not as three blobs: a hollow body (drawn as a tube
// outline), a filled chisel head, a ferrule that is wider than the body and stands apart from
// both the head and the body, and the horizontal streak it has just laid. The hollow body is
// the whole point: a filled 9px-wide body at a 2.8px stroke merges into one blob, which is how
// this icon read in 0.1.
struct MarkerShape {
    D2D1_POINT_2F nib[4], ferrule[4], streak[4];
    D2D1_POINT_2F body[4];          // A0, A1, B1, B0 - the tube outline
};

inline void marker_shape(float cx, float cy, float b, MarkerShape& m) {
    const float ux = 0.7071f, uy = -0.7071f;              // along the pen, up and right
    const float vx = 0.7071f, vy = 0.7071f;               // across the pen, down and right
    const float nx = cx - b * 0.195f, ny = cy + b * 0.16f;  // nib tip, lower-left of the box
    const float t1 = b * 0.135f;                          // top of the chisel head
    const float f0 = b * 0.19f, f1 = b * 0.245f;          // ferrule, with a gap on both sides
    const float t2 = b * 0.30f, t3 = b * 0.70f;           // body, ending short of the box edge
    const float wTip = b * 0.13f, wTop = b * 0.09f;       // chisel: wide at the tip, narrower up
    const float wFer = b * 0.17f, wBody = b * 0.135f;     // the ferrule is wider than the body

    m.nib[0] = D2D1_POINT_2F{ nx + vx * wTip,  ny + vy * wTip };
    m.nib[1] = D2D1_POINT_2F{ nx - vx * wTip,  ny - vy * wTip };
    m.nib[2] = D2D1_POINT_2F{ nx + ux * t1 - vx * wTop, ny + uy * t1 - vy * wTop };
    m.nib[3] = D2D1_POINT_2F{ nx + ux * t1 + vx * wTop, ny + uy * t1 + vy * wTop };

    m.ferrule[0] = D2D1_POINT_2F{ nx + ux * f0 - vx * wFer, ny + uy * f0 - vy * wFer };
    m.ferrule[1] = D2D1_POINT_2F{ nx + ux * f0 + vx * wFer, ny + uy * f0 + vy * wFer };
    m.ferrule[2] = D2D1_POINT_2F{ nx + ux * f1 + vx * wFer, ny + uy * f1 + vy * wFer };
    m.ferrule[3] = D2D1_POINT_2F{ nx + ux * f1 - vx * wFer, ny + uy * f1 - vy * wFer };

    m.body[0] = D2D1_POINT_2F{ nx + ux * t2 - vx * wBody, ny + uy * t2 - vy * wBody };
    m.body[1] = D2D1_POINT_2F{ nx + ux * t3 - vx * wBody, ny + uy * t3 - vy * wBody };
    m.body[2] = D2D1_POINT_2F{ nx + ux * t3 + vx * wBody, ny + uy * t3 + vy * wBody };
    m.body[3] = D2D1_POINT_2F{ nx + ux * t2 + vx * wBody, ny + uy * t2 + vy * wBody };

    // The streak lies flat along the bottom edge of the box: it is what separates this glyph
    // from a second diagonal arrow, and it touches the nib end.
    m.streak[0] = D2D1_POINT_2F{ cx - b * 0.44f, cy + b * 0.32f };
    m.streak[1] = D2D1_POINT_2F{ cx + b * 0.02f, cy + b * 0.32f };
    m.streak[2] = D2D1_POINT_2F{ cx + b * 0.02f, cy + b * 0.44f };
    m.streak[3] = D2D1_POINT_2F{ cx - b * 0.44f, cy + b * 0.44f };
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

    ID2D1SolidColorBrush *brText = nullptr, *brMuted = nullptr, *brSecondary = nullptr,
                          *brAccent = nullptr,
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

        ID2D1SolidColorBrush* brs[10] = {};
        const D2D1_COLOR_F cols[10] = { th->textPrimary, th->textTertiary, th->accentGlyph, th->floating,
                                        th->borderStrong, th->controlHover, th->accentFill, th->focusRing,
                                        th->borderSoft, th->textSecondary };
        for (int i = 0; i < 10; ++i) {
            HRESULT hr = rt->CreateSolidColorBrush(cols[i], &brs[i]);
            if (FAILED(hr) && SUCCEEDED(worst)) worst = hr;
        }
        brText = brs[0]; brMuted = brs[1]; brAccent = brs[2]; brSurface = brs[3];
        brBorder = brs[4]; brHover = brs[5]; brFill = brs[6]; brFocus = brs[7]; brSoft = brs[8];
        brSecondary = brs[9];                 // the save path in the status strip (8.7 case 2)

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
        rel((IUnknown*)brSecondary);
        rel((IUnknown*)fmtCaption); rel((IUnknown*)fmtBody); rel((IUnknown*)fmtLabel);
        solid = dashed = nullptr;
        brText = brMuted = brAccent = brSurface = brBorder = brHover = brFill = brFocus = brSoft = nullptr;
        brSecondary = nullptr;
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

    void control(float x, float y, float w, float h, ControlState st, ControlGroup grp = CG_TOOL) {
        if (!rt || !th) return;
        // The group decides the resting surface; the state decides the change on top of it.
        // Tools sit on `control`, history sits on the pill surface (only its border is bright),
        // output sits in a `canvas` recess. Three surfaces is what makes the groups read without
        // a caption; a single surface plus separator lines was the 0.1 layout.
        D2D1_COLOR_F fillc = th->control, borderc = th->borderSoft;
        bool no_fill = false, no_border = false;
        float bw = 1.0f;
        switch (grp) {
            case CG_PLAIN:
                if (st == ST_NORMAL) { no_fill = true; borderc = th->borderStrong; }
                else if (st == ST_HOVER) fillc = th->control;
                else if (st == ST_PRESSED) fillc = th->controlPressed;
                else if (st == ST_DISABLED) { fillc = th->canvas; no_border = true; }
                else { fillc = th->control; }
                break;
            case CG_SUNKEN:
                fillc = th->canvas;
                if (st == ST_HOVER) fillc = th->control;
                else if (st == ST_PRESSED) fillc = th->controlPressed;
                break;
            default:
                if (st == ST_HOVER) fillc = th->controlHover;
                else if (st == ST_PRESSED) fillc = th->controlPressed;
                else if (st == ST_ACTIVE) fillc = th->accentFill;
                else if (st == ST_DISABLED) fillc = th->canvas;
                break;
        }
        if (st == ST_ACTIVE && grp == CG_TOOL) { borderc = th->accentGlyph; bw = 1.5f; }
        ID2D1SolidColorBrush* brush = nullptr;
        if (!no_fill && !cache_brush(fillc, &brush)) return;
        D2D1_ROUNDED_RECT rr = R2(x, y, w, h, th->rControl);
        if (!no_fill) rt->FillRoundedRectangle(&rr, brush);
        if (!no_border) {
            ID2D1SolidColorBrush* bb = nullptr;
            if (cache_brush(borderc, &bb)) rt->DrawRoundedRectangle(&rr, bb, bw, solid);
        }
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

    // Text with explicit alignment: the shared formats are leading-aligned, and a caption has to
    // sit centred in its cell box (TOOLBAR-SPEC 8.1), which DrawText through a format cannot do.
    void draw_layout(const wchar_t* s, IDWriteTextFormat* fmt, float x, float y, float w, float h,
                     ID2D1SolidColorBrush* brush, DWRITE_TEXT_ALIGNMENT align,
                     DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        if (!dw_ || !fmt || !brush || !s || !*s) return;
        IDWriteTextLayout* l = nullptr;
        if (FAILED(dw_->CreateTextLayout(s, (UINT)wcslen(s), fmt, w, h, &l)) || !l) return;
        l->SetTextAlignment(align);
        l->SetParagraphAlignment(pa);
        D2D1_POINT_2F at = { x, y };
        rt->DrawTextLayout(at, l, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        l->Release();
    }

    // ---- glyphs ----------------------------------------------------------------------
    // Ink by state (TOOLBAR-SPEC 8.2): resting and selected use the accent, hover/press switch to
    // textPrimary because accentGlyph only scores 3.0-3.6:1 on the hover/press surfaces.
    void glyph(int g, int slot, float cx, float cy, ControlState st, ID2D1SolidColorBrush* resting = nullptr) {
        if (!rt || !th || !fac) return;
        ID2D1SolidColorBrush* def = resting ? resting : brAccent;
        ID2D1SolidColorBrush* ink = (st == ST_DISABLED)  ? brMuted
                              : (st == ST_HOVER || st == ST_PRESSED) ? brText
                              : def;
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

    // Measured width of a string in the format's own metrics. The pill sizes its labelled cells
    // from this, so a longer caption changes the cell width instead of overflowing the pill.
    float measure(const wchar_t* s, IDWriteTextFormat* fmt) const {
        if (!dw_ || !fmt || !s || !*s) return 0.0f;
        IDWriteTextLayout* l = nullptr;
        if (FAILED(dw_->CreateTextLayout(s, (UINT)wcslen(s), fmt, 4000.0f, 200.0f, &l)) || !l)
            return 0.0f;
        DWRITE_TEXT_METRICS m;
        l->GetMetrics(&m);
        l->Release();
        return m.widthIncludingTrailingWhitespace;
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
        return g == G_ARROW || g == G_UNDO || g == G_REDO || g == G_MARKER || g == G_SAVE;
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
                // Only the body is stroked: two side edges and the far end, left open at the
                // ferrule end. Filling this shape is what made the 0.1 glyph read as a blob.
                MarkerShape m;
                marker_shape(cx, cy, b, m);
                s->BeginFigure(m.body[0], D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(m.body[1]);
                s->AddLine(m.body[2]);
                s->AddLine(m.body[3]);
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_CROP: {
                // The classic crop marks: a top edge that turns down at its left end and a
                // bottom edge that turns up at its right end, both running past each other so
                // the middle reads as a frame and the ends as cut lines.
                s->BeginFigure(D2D1_POINT_2F{ Rt - b * 0.10f, cy - b * 0.28f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.28f, cy - b * 0.28f });
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.28f, B - b * 0.10f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.10f, cy + b * 0.28f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.28f, cy + b * 0.28f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.28f, T + b * 0.10f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_UNDO:
            case G_REDO:
                // Tail low on one side, head high on the other: undo sweeps from the right,
                // up over the top and around to the left (counter-clockwise); redo is the
                // same shape seen in a mirror. Both get a filled head so the direction of
                // the curl reads at a glance instead of looking like a random hook.
                add_arc(s, arc_for(g, cx, cy, b));
                break;
            case G_SAVE: {
                // A tray, and an arrow that visibly enters it: the shaft crosses the tray's
                // open top and the filled head sits inside the tray.
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.10f, cy + b * 0.14f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ L + b * 0.10f, B - b * 0.06f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.10f, B - b * 0.06f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.10f, cy + b * 0.14f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ cx, T + b * 0.08f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx, cy + b * 0.10f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                break;
            }
            case G_COPY: {
                // Two sheets of the same size, one shifted up and to the right. The sheet in
                // back is drawn only where it is not covered, so the overlap is visible; two
                // closed rectangles next to each other read as one box plus an L.
                s->BeginFigure(D2D1_POINT_2F{ cx - b * 0.16f, cy - b * 0.02f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx - b * 0.16f, T + b * 0.10f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.10f, T + b * 0.10f });
                s->AddLine(D2D1_POINT_2F{ Rt - b * 0.10f, cy + b * 0.20f });
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->BeginFigure(D2D1_POINT_2F{ L + b * 0.10f, cy - b * 0.02f }, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.16f, cy - b * 0.02f });
                s->AddLine(D2D1_POINT_2F{ cx + b * 0.16f, B - b * 0.10f });
                s->AddLine(D2D1_POINT_2F{ L + b * 0.10f, B - b * 0.10f });
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
            case G_REDO:
                add_arc_head(s, arc_for(g, cx, cy, b), b);
                break;
            case G_MARKER: {
                // Head, ferrule and streak carry the mass; the body stays hollow (see build_line).
                MarkerShape m;
                marker_shape(cx, cy, b, m);
                const D2D1_POINT_2F* parts[3] = { m.nib, m.ferrule, m.streak };
                for (int i = 0; i < 3; ++i) {
                    s->BeginFigure(parts[i][0], D2D1_FIGURE_BEGIN_FILLED);
                    s->AddLine(parts[i][1]);
                    s->AddLine(parts[i][2]);
                    s->AddLine(parts[i][3]);
                    s->EndFigure(D2D1_FIGURE_END_CLOSED);
                }
                break;
            }
            case G_SAVE: {
                // Filled arrowhead sitting inside the tray, so the glyph reads as "put the
                // result somewhere" instead of as an up/down arrow pair.
                float hw = b * 0.17f;
                s->BeginFigure(D2D1_POINT_2F{ cx, cy + b * 0.30f }, D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1_POINT_2F{ cx - hw, cy + b * 0.08f });
                s->AddLine(D2D1_POINT_2F{ cx + hw, cy + b * 0.08f });
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                break;
            }
            default: break;
        }
    }
};

} // namespace ui
