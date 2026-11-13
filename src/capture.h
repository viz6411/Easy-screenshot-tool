// capture.h - screen/window/region capture via GDI (no external deps)
#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>

struct DIB {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // top-down, 4 bytes/px

    uint8_t* px(int x, int y) { return &rgba[((size_t)y * w + x) * 4]; }
    const uint8_t* px(int x, int y) const { return &rgba[((size_t)y * w + x) * 4]; }
};

inline bool capture_rect(int x0, int y0, int w, int h, DIB& out) {
    if (w <= 0 || h <= 0) return false;
    HDC sdc = GetDC(nullptr);
    if (!sdc) return false;
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP hbmp = CreateCompatibleBitmap(sdc, w, h);
    if (!mdc || !hbmp) {
        if (hbmp) DeleteObject(hbmp);
        if (mdc) DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
        return false;
    }
    HBITMAP old = (HBITMAP)SelectObject(mdc, hbmp);

    // CAPTUREBLT (0x40000000) includes layered windows -> captures overlays/tooltips.
    BitBlt(mdc, 0, 0, w, h, sdc, x0, y0, SRCCOPY | 0x40000000u);

    struct BIH {
        DWORD biSize; LONG biWidth; LONG biHeight; WORD biPlanes; WORD biBitCount;
        DWORD biCompression; DWORD biSizeImage; LONG biXPelsPerMeter; LONG biYPelsPerMeter;
        DWORD biClrUsed; DWORD biClrImportant;
    } bih = {};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h;            // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = 0;        // BI_RGB

    std::vector<uint8_t> buf((size_t)w * 4 * h);
    int got = GetDIBits(sdc, hbmp, 0, h, buf.data(), (LPBITMAPINFO)&bih, DIB_RGB_COLORS);

    out.w = w; out.h = h;
    out.rgba.assign((size_t)w * 4 * h, 0);
    if (got > 0) {
        // GetDIBits yields BGRA -> convert to RGBA in place
        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
            uint8_t b = buf[i], g = buf[i + 1], r = buf[i + 2], a = buf[i + 3];
            out.rgba[i]     = r;
            out.rgba[i + 1] = g;
            out.rgba[i + 2] = b;
            out.rgba[i + 3] = a ? a : 255;
        }
    }

    SelectObject(mdc, old);
    DeleteObject(hbmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return got > 0;
}

inline bool capture_screen(DIB& out) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return false;
    return capture_rect(0, 0, w, h, out);
}

inline bool capture_window(HWND hwnd, DIB& out) {
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;

    // Prefer PrintWindow (renders even when occluded), fall back to screen BitBlt.
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP hbmp = CreateCompatibleBitmap(sdc, w, h);
    bool ok = false;
    if (mdc && hbmp) {
        HBITMAP old = (HBITMAP)SelectObject(mdc, hbmp);
        // PW_RENDERFULLCONTENT = 2 (Windows 8.1+)
        ok = PrintWindow(hwnd, mdc, 2) != FALSE;
        if (!ok) {
            BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY | 0x40000000u);
            ok = true;
        }
        struct BIH {
            DWORD biSize; LONG biWidth; LONG biHeight; WORD biPlanes; WORD biBitCount;
            DWORD biCompression; DWORD biSizeImage; LONG biXPelsPerMeter; LONG biYPelsPerMeter;
            DWORD biClrUsed; DWORD biClrImportant;
        } bih = {};
        bih.biSize = sizeof(bih); bih.biWidth = w; bih.biHeight = -h;
        bih.biPlanes = 1; bih.biBitCount = 32; bih.biCompression = 0;
        std::vector<uint8_t> buf((size_t)w * 4 * h, 0);
        if (GetDIBits(sdc, hbmp, 0, h, buf.data(), (LPBITMAPINFO)&bih, DIB_RGB_COLORS) > 0) {
            out.w = w; out.h = h; out.rgba.assign((size_t)w * 4 * h, 0);
            for (size_t i = 0; i + 3 < buf.size(); i += 4) {
                out.rgba[i]     = buf[i + 2];
                out.rgba[i + 1] = buf[i + 1];
                out.rgba[i + 2] = buf[i];
                out.rgba[i + 3] = buf[i + 3] ? buf[i + 3] : 255;
            }
        } else {
            ok = false;
        }
        SelectObject(mdc, old);
        DeleteObject(hbmp);
        DeleteDC(mdc);
    }
    ReleaseDC(nullptr, sdc);
    return ok;
}

inline HWND window_at_point(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (!h) return nullptr;
    // Walk up to the top-level frame so we capture the whole window, not a child control.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto getAncestor = (HWND(WINAPI*)(HWND, UINT))GetProcAddress(user32, "GetAncestor");
    if (getAncestor) {
        HWND root = getAncestor(h, 2 /*GA_ROOT*/);
        if (root) h = root;
    }
    return h;
}

