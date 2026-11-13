// clipboard.h - put RGBA DIB on the clipboard as CF_DIB (PNG-compatible)
#pragma once
#include <windows.h>
#include <vector>
#include "capture.h"

inline bool copy_dib_to_clipboard(const DIB& img) {
    if (img.w <= 0 || img.h <= 0) return false;

    // Build a BITMAPINFOHEADER + bottom-up BGRA pixels (standard CF_DIB layout).
    const int rowBytes = ((img.w * 32 + 31) & ~31) / 8;
    const DWORD imgSize = (DWORD)rowBytes * img.h;

    HGLOBAL hMem = GlobalAlloc(GHND, sizeof(BITMAPINFOHEADER) + imgSize);
    if (!hMem) return false;

    uint8_t* p = (uint8_t*)GlobalLock(hMem);
    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)p;
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = img.w;
    bih->biHeight = -img.h;              // top-down is accepted by CF_DIB readers
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = imgSize;

    uint8_t* dst = p + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* src = img.px(0, y);
        uint8_t* outRow = dst + (size_t)y * rowBytes;
        for (int x = 0; x < img.w; ++x) {
            outRow[x * 4 + 0] = src[x * 4 + 2];   // B
            outRow[x * 4 + 1] = src[x * 4 + 1];   // G
            outRow[x * 4 + 2] = src[x * 4 + 0];   // R
            outRow[x * 4 + 3] = 255;              // A (ignored by most readers)
        }
    }
    GlobalUnlock(hMem);

    if (!OpenClipboard(nullptr)) { GlobalFree(hMem); return false; }
    EmptyClipboard();
    HANDLE set = SetClipboardData(CF_DIB, hMem); bool ok = (set != nullptr);
    CloseClipboard();
    if (!ok) GlobalFree(hMem);          // on success the clipboard owns hMem
    return ok;
}


