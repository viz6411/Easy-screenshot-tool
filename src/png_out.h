// png_out.h - PNG read/write through GDI+ (gdiplus), the system's own image codec.
//
// Card #17: the writer this replaces deflated in stored blocks - not compressed at all, because
// the toolchain has no zlib - so a saved frame weighed about as much as its raw pixels. GDI+ ships
// with Windows, encodes real PNG and decodes it, which is also what opening a file needs (#16).
// WIC was tried first and is unusable from this toolchain: mingw-w64's wincodec.h declares
// IWICBitmapEncoder::Initialize/CreateFrame and IWICFormatConverter with signatures that do not
// match the real COM interfaces, so every call through them lands on the wrong vtable slot.
#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <cstdlib>
#include <vector>

#include "capture.h"

namespace png {

inline bool gdiplus_ready() {
    static ULONG_PTR token = 0;
    static bool ok = false, tried = false;
    if (!tried) {
        tried = true;
        Gdiplus::GdiplusStartupInput in;
        ok = (Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok);
    }
    return ok;
}

inline bool png_encoder_cls(CLSID& out) {
    static CLSID cached = {};
    static bool found = false;
    if (found) { out = cached; return true; }

    UINT n = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&n, &bytes) != Gdiplus::Ok || n == 0 || bytes == 0) return false;
    Gdiplus::ImageCodecInfo* infos = (Gdiplus::ImageCodecInfo*)malloc(bytes);
    if (!infos) return false;
    bool ok = false;
    if (Gdiplus::GetImageEncoders(n, bytes, infos) == Gdiplus::Ok) {
        for (UINT i = 0; i < n; ++i) {
            if (infos[i].MimeType && wcscmp(infos[i].MimeType, L"image/png") == 0) {
                out = cached = infos[i].Clsid;
                ok = true;
                break;
            }
        }
    }
    free(infos);
    found = ok;
    return ok;
}

// rgba: width*height*4 bytes, row-major, top-down.
inline bool write(const wchar_t* path, int w, int h, const std::vector<uint8_t>& rgba) {
    if (!gdiplus_ready() || w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4) return false;
    CLSID cls;
    if (!png_encoder_cls(cls)) return false;

    // GDI+ 32bppARGB holds a pixel as 0xAARRGGBB, i.e. B,G,R,A in memory; our DIB is R,G,B,A.
    std::vector<uint8_t> bgra(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4) {
        bgra[i]     = rgba[i + 2];
        bgra[i + 1] = rgba[i + 1];
        bgra[i + 2] = rgba[i];
        bgra[i + 3] = rgba[i + 3];
    }

    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, bgra.data());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    return bmp.Save(path, &cls, nullptr) == Gdiplus::Ok;
}

// Decodes a file GDI+ can read into the app's own RGBA DIB (top-down, 4 bytes/px).
inline bool read(const wchar_t* path, DIB& out) {
    if (!gdiplus_ready()) return false;
    Gdiplus::Bitmap src(path);
    if (src.GetLastStatus() != Gdiplus::Ok) return false;

    int w = src.GetWidth(), h = src.GetHeight();   // mingw's gdiplus names these GetWidth/GetHeight
    if (w <= 0 || h <= 0) return false;

    Gdiplus::Rect area = { 0, 0, w, h };
    Gdiplus::BitmapData bd = {};
    if (src.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;

    out.w = w; out.h = h;
    out.rgba.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = (const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
        uint8_t* d = &out.rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x) {
            d[x * 4]     = s[x * 4 + 2];
            d[x * 4 + 1] = s[x * 4 + 1];
            d[x * 4 + 2] = s[x * 4];
            d[x * 4 + 3] = s[x * 4 + 3] ? s[x * 4 + 3] : 255;   // same rule capture_rect uses
        }
    }
    src.UnlockBits(&bd);
    return true;
}

} // namespace png
