// theme.h - ShotMvp design tokens.
//
// One dark ramp (single hue, only lightness changes), four text levels, one accent taken from
// the Windows system accent, an 8/6/4 spacing grid, a radius scale and a 1.25 type ratio.
// Everything here is pre-scaled for the monitor DPI so the rest of the code can just read
// integers and stay crisp at 175% scaling.
//
// Rule for the rest of the app: no literal colors in drawing calls. If a color is needed, it
// comes from this struct.
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <cmath>

struct Theme {
    bool  dark = true;
    float dpi  = 96.0f;
    float k    = 1.0f;          // dpi / 96

    // --- Surfaces: elevation is lightness only, one hue -------------------------------
    D2D1_COLOR_F canvas;        // base desktop of the editor
    D2D1_COLOR_F panel;         // docked panel
    D2D1_COLOR_F floating;      // pill/toolbar floating above content
    D2D1_COLOR_F menu;          // menus and popovers, one step above floating

    // --- Controls ---------------------------------------------------------------------
    D2D1_COLOR_F control;       // resting control surface
    D2D1_COLOR_F controlHover;
    D2D1_COLOR_F controlPressed;
    D2D1_COLOR_F accentFill;    // active tool: tinted surface
    D2D1_COLOR_F accentGlyph;   // active tool: glyph color, contrast-checked

    // --- Borders ----------------------------------------------------------------------
    D2D1_COLOR_F borderSoft;    // group separation
    D2D1_COLOR_F borderStrong;  // control outline
    D2D1_COLOR_F focusRing;     // keyboard focus

    // --- Text: four levels ------------------------------------------------------------
    D2D1_COLOR_F textPrimary, textSecondary, textTertiary, textMuted;

    // --- Spacing grid (physical px, DPI-scaled) ---------------------------------------
    int s2, s4, s6, s8, s12, s16, s24;

    // --- Radius scale -----------------------------------------------------------------
    float rControl, rPanel, rFloating, rPopover;

    // --- Type scale: 1.25 ratio from a 13px body --------------------------------------
    int fsCaption;   // 11
    int fsBody;      // 13
    int fsH4;        // 16
    int fsH2;        // 20

    // --- Controls ---------------------------------------------------------------------
    int ctlH;        // visible control height
    int ctlHit;      // hit area (>= ctlH + 8)
    int glyphBox;    // icon slot
    int glyphSize;   // drawn icon size
    float strokeW;   // icon stroke weight

    // --- Helpers ----------------------------------------------------------------------
    static D2D1_COLOR_F rgba(int r, int g, int b, float a = 1.0f) {
        return D2D1_COLOR_F{ r / 255.0f, g / 255.0f, b / 255.0f, a };
    }
    static float lum(const D2D1_COLOR_F& c) {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }
    // WCAG contrast ratio. lum() is a fast lightness for surfaces; contrast decisions that
    // decide whether ink is readable must use the linearised sRGB form, otherwise a dark pair
    // like #222222 on #1A1C20 scores ~1.07 instead of looking "close enough" to lum().
    static float contrast(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
        auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
        auto L = [&](const D2D1_COLOR_F& c) {
            return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
        };
        float la = L(a), lb = L(b);
        if (la < lb) { float t = la; la = lb; lb = t; }
        return (la + 0.05f) / (lb + 0.05f);
    }
    static D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
        return D2D1_COLOR_F{ a.r + (b.r - a.r) * t,
                             a.g + (b.g - a.g) * t,
                             a.b + (b.b - a.b) * t,
                             a.a + (b.a - a.a) * t };
    }
    // Raise an accent until it is legible on a dark surface, keeping its hue.
    static D2D1_COLOR_F legible(D2D1_COLOR_F c, float target) {
        for (int i = 0; i < 8 && lum(c) < target; ++i) c = mix(c, rgba(255, 255, 255), 0.25f);
        return c;
    }

    static bool apps_use_light_theme() {
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
        DWORD v = 0, sz = sizeof(v);
        LONG r = RegQueryValueExW(h, L"AppsUseLightTheme", nullptr, nullptr, (BYTE*)&v, &sz);
        RegCloseKey(h);
        return r == ERROR_SUCCESS && v != 0;
    }

    static D2D1_COLOR_F windows_accent(bool* ok) {
        COLORREF c = 0; BOOL opaque = FALSE;
        if (FAILED(DwmGetColorizationColor(&c, &opaque)) || c == 0) {
            if (ok) *ok = false;
            return rgba(96, 148, 224);       // sane default accent
        }
        if (ok) *ok = true;
        return rgba((int)GetRValue(c), (int)GetGValue(c), (int)GetBValue(c));
    }

    static Theme load(float monitor_dpi) {
        Theme t;
        t.dpi = monitor_dpi > 0 ? monitor_dpi : 96.0f;
        // Defensive against the exact bug this project hit: a scale factor (1.0-4.0)
        // handed to load() instead of a DPI (96-384) would make every size collapse to
        // near zero, and DirectWrite would then reject a font size of 0.
        if (t.dpi < 48.0f) t.dpi = (t.dpi > 0.5f ? t.dpi * 96.0f : 96.0f);
        t.k = t.dpi / 96.0f;
        t.dark = !apps_use_light_theme();   // stage 6 will paint the light ramp too

        // Dark ramp: one hue (cool slate), only lightness changes.
        t.canvas   = rgba(0x1A, 0x1C, 0x20);
        t.panel    = rgba(0x23, 0x26, 0x2B);
        t.floating = rgba(0x21, 0x24, 0x29);
        t.menu     = rgba(0x36, 0x3D, 0x47);

        t.control       = rgba(0x36, 0x3D, 0x47);
        t.controlHover  = rgba(0x42, 0x4A, 0x56);
        t.controlPressed= rgba(0x4C, 0x55, 0x61);

        t.borderSoft   = rgba(255, 255, 255, 0.07f);
        t.borderStrong = rgba(255, 255, 255, 0.14f);
        // R16 / WCAG 1.4.11: at 0.30 white the ring measured 2.9:1 against the pill surface - under
        // the 3:1 floor. 0.62 puts it at ~7:1 against both the pill and a control fill.
        t.focusRing    = rgba(255, 255, 255, 0.62f);

        t.textPrimary   = rgba(0xEC, 0xEF, 0xF4);
        t.textSecondary = rgba(0xAB, 0xB3, 0xBE);
        t.textTertiary  = rgba(0x7E, 0x87, 0x94);
        t.textMuted     = rgba(0x5C, 0x64, 0x71);

        bool ok = false;
        D2D1_COLOR_F acc = windows_accent(&ok);
        acc = legible(acc, 0.55f);
        t.accentGlyph = acc;
        t.accentFill  = D2D1_COLOR_F{ acc.r, acc.g, acc.b, 0.24f };

        // Spacing grid.
        auto S = [&](int v) { return (int)((float)v * t.k + 0.5f); };
        t.s2 = S(2); t.s4 = S(4); t.s6 = S(6); t.s8 = S(8);
        t.s12 = S(12); t.s16 = S(16); t.s24 = S(24);

        // Radius scale.
        t.rControl  = 6.0f  * t.k;
        t.rPanel    = 10.0f * t.k;
        t.rFloating = 12.0f * t.k;
        t.rPopover  = 12.0f * t.k;

        // Type scale (11 / 13 / 16 / 20 at 100%).
        t.fsCaption = S(11); t.fsBody = S(13); t.fsH4 = S(16); t.fsH2 = S(20);

        // Controls: 32px visual, 40px hit area, 20px glyph on a 24px slot.
        t.ctlH     = S(32);
        t.ctlHit   = S(40);
        t.glyphBox = S(24);
        t.glyphSize= S(20);
        t.strokeW  = 1.6f * t.k;

        return t;
    }
};
