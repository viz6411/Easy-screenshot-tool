// main.cpp - ShotMvp: standalone screenshot tool MVP
// Flow: global hotkey -> region overlay OR active-window capture -> annotation editor -> PNG + clipboard
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <ctime>
#include <cstdio>

#include "dpi.h"
#include "capture.h"
#include "clipboard.h"
#include "png_out.h"
#include "config.h"
#include "overlay.h"
#include "editor.h"
#include "hotkey_host.h"


static Config g_cfg;

// Run DPI awareness during static initialization, before any entry point or window call.
// MinGW's -municode startup can reach wmain without going through wWinMain, and the CRT may
// query metrics before our first statement, so an early global ctor is the only ordering that
// is safe for both subsystems.
static struct DpiInit {
    DpiInit() { enable_dpi_awareness(); }
} g_dpi_init;
class HotkeyHost;
static bool register_with_fallback(HotkeyHost& host, int id, Hotkey& hk, const wchar_t* label_for_report);
static const int HK_REGION = 1;
static const int HK_WINDOW = 2;

static std::wstring timestamp_name(const std::wstring& fmt) {
    time_t t = time(nullptr);
    struct tm lt; localtime_s(&lt, &t);
    wchar_t out[512];
    // Only the tokens we support: %Y %m %d %H %M %S
    std::wstring s;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == L'%' && i + 1 < fmt.size()) {
            wchar_t tmp[8];
            switch (fmt[++i]) {
                case L'Y': wsprintfW(tmp, L"%04d", lt.tm_year + 1900); s += tmp; continue;
                case L'm': wsprintfW(tmp, L"%02d", lt.tm_mon + 1);     s += tmp; continue;
                case L'd': wsprintfW(tmp, L"%02d", lt.tm_mday);        s += tmp; continue;
                case L'H': wsprintfW(tmp, L"%02d", lt.tm_hour);        s += tmp; continue;
                case L'M': wsprintfW(tmp, L"%02d", lt.tm_min);         s += tmp; continue;
                case L'S': wsprintfW(tmp, L"%02d", lt.tm_sec);         s += tmp; continue;
                default: s += L'%'; s += fmt[i]; continue;
            }
        }
        s += fmt[i];
    }
    if (s.size() < 512) { wcscpy_s(out, s.c_str()); return out; }
    return s;
}

static std::wstring unique_path(const std::wstring& dir, const std::wstring& base) {
    std::wstring p = dir + L"\\" + base + L".png";
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return p;
    for (int i = 2; i < 10000; ++i) {
        wchar_t suf[16]; wsprintfW(suf, L"_%d", i);
        std::wstring q = dir + L"\\" + base + suf + L".png";
        if (GetFileAttributesW(q.c_str()) == INVALID_FILE_ATTRIBUTES) return q;
    }
    return p;
}

static void notify(const wchar_t* msg) {
    // Non-modal toast: a topmost borderless STATIC window that destroys itself after ~1.6s.
    // MessageBoxTimeoutW is not declared by MinGW headers, and a modal box would block the
    // hotkey message loop, so we implement the toast directly.
    struct Toast {
        static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
            switch (m) {
                case WM_CREATE:
                    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                    SetTimer(h, 1, 1600, nullptr);
                    return 0;
                case WM_TIMER: KillTimer(h, 1); DestroyWindow(h); return 0;
                case WM_DESTROY: return 0;
            }
            return DefWindowProcW(h, m, w, l);
        }
    };

    static bool registered = false;
    const wchar_t* cls = L"ShotMvpToast";
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = Toast::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hbrBackground = CreateSolidBrush(RGB(0x22, 0x22, 0x22));
        RegisterClassW(&wc);
        registered = true;
    }

    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls, L"ShotMvp",
                             WS_POPUP, 0, 0, 460, 92, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h) return;

    HWND label = CreateWindowExW(0, L"STATIC", msg, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 12, 10, 436, 72, h, nullptr, GetModuleHandleW(nullptr), nullptr);
    HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    if (label && f) SendMessageW(label, WM_SETFONT, (WPARAM)f, TRUE);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    SetWindowPos(h, HWND_TOPMOST, sw - 480, 24, 460, 92, SWP_SHOWWINDOW);
}

// ---- output of the editor (card #8): these run while the editor window is open, because
// "Сохранить"/"Копировать" must produce their result at the moment of the click.

// System folder picker, modal with the editor as its owner (8.7 case 1).
static std::wstring pick_folder(HWND owner) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Выбор папки для скриншотов";
    // BIF_USENEWUI is the modern folder dialog with the "Folder:" edit field (8.7 case 1);
    // MinGW's header has no BIF_NEWSSTYLEINTERFACE, and BIF_USENEWUI is the same UI.
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";                       // "Отмена"
    wchar_t path[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE && path[0];
    CoTaskMemFree(pidl);
    return ok ? path : L"";
}

// "Сохранить как" for the save_ask=1 mode (8.7 case 4): the name is pre-filled from name_format.
static std::wstring ask_file(HWND owner, const std::wstring& dir, const std::wstring& base) {
    wchar_t file[MAX_PATH * 2] = {};
    wcsncpy_s(file, (base + L".png").c_str(), _TRUNCATE);
    wchar_t filter[] = L"PNG (*.png)\0*.png\0\0";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return L"";     // "Отмена"
    return file;
}

static Editor::OutputResult save_output(const DIB& out, HWND owner, std::wstring& path_out) {
    std::wstring base = timestamp_name(g_cfg.name_format);
    std::wstring file;

    if (!g_cfg.save_dir_chosen) {                // never picked a folder: ask once, then remember it
        std::wstring dir = pick_folder(owner);
        if (dir.empty()) return Editor::OUT_CANCELLED;
        g_cfg.save_dir = dir;
        g_cfg.save_dir_chosen = true;
        g_cfg.save();
        file = unique_path(dir, base);
    } else if (g_cfg.save_ask) {
        file = ask_file(owner, g_cfg.save_dir, base);
        if (file.empty()) return Editor::OUT_CANCELLED;
    } else {
        file = unique_path(g_cfg.save_dir, base);
    }

    path_out = file;
    SHCreateDirectoryExW(owner, g_cfg.save_dir.c_str(), nullptr);
    if (!png::write(file.c_str(), out.w, out.h, out.rgba)) return Editor::OUT_FAILED;
    if (g_cfg.copy_to_clipboard) copy_dib_to_clipboard(out);
    return Editor::OUT_SAVED;
}

static bool copy_output(const DIB& out) { return copy_dib_to_clipboard(out); }

// Non-interactive self test: exercises capture -> crop -> annotate -> PNG -> clipboard.
// Prints one line per step so a test harness can assert on it. Returns 0 on success.

static int run_selftest() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    wprintf(L"step: enter selftest\n");
    int fails = 0;
    auto report = [&](const wchar_t* name, bool ok) {
        DWORD err = GetLastError();
        wprintf(L"%-22ls %ls%ls\n", name, ok ? L"OK" : L"FAIL", ok ? L"" : L"");
        if (!ok) { wprintf(L"     last error = %lu\n", err); ++fails; }
    };

    // 1. full screen capture (must cover the PHYSICAL panel, not a DPI-virtualized subset)
    DIB full;
    bool ok = capture_screen(full);
    report(L"capture_screen", ok && full.w > 0 && full.h > 0);
    int phys_w = GetSystemMetrics(SM_CXSCREEN), phys_h = GetSystemMetrics(SM_CYSCREEN);
    int aware = dpi_awareness_level();
    static const wchar_t* aware_names[] = {L"UNAWARE", L"SYSTEM_AWARE", L"PER_MONITOR",
                                           L"DPI_UNCHANGED", L"PER_MONITOR_V2"};
    const wchar_t* aware_txt = (aware >= 0 && aware <= 4) ? aware_names[aware] : L"UNKNOWN";
    wprintf(L"     screen %dx%d bytes=%zu | metrics=%dx%d | awareness=%d | method=%d err1=%lu err2=%lu err3=%lu\n",
            full.w, full.h, full.rgba.size(), phys_w, phys_h, aware,
            g_dpi_method, g_dpi_err1, g_dpi_err2, g_dpi_err3);
    wprintf(L"     awareness name: %ls\n", aware_txt);
    // A DPI-unaware process reports a smaller virtual screen and silently loses the bottom-right
    // part of the desktop, so the capture must match the physical metrics exactly.
    report(L"capture_full_screen", ok && full.w == phys_w && full.h == phys_h);

    // 2. region capture (a sub-rect of the real desktop)
    int rx = full.w / 4, ry = full.h / 6, rw = full.w / 2, rh = full.h / 2;
    DIB reg;
    ok = capture_rect(rx, ry, rw, rh, reg);
    report(L"capture_region", ok && reg.w == rw && reg.h == rh);

    // 3. window capture: desktop window is always present
    DIB winimg;
    ok = capture_window(GetDesktopWindow(), winimg);
    report(L"capture_window", ok && winimg.w > 0 && winimg.h > 0);

    // 4. annotation primitives on the captured region (draw directly into the DIB so the
    //    test does not depend on a visible editor window)
    auto putpx = [&](DIB& d, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= d.w || y >= d.h) return;
        uint8_t* p = d.px(x, y); p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    // frame
    for (int x = 4; x < reg.w - 4; ++x) { putpx(reg, x, 4, 0xE4, 0x2A, 0x31); putpx(reg, x, reg.h - 5, 0xE4, 0x2A, 0x31); }
    for (int y = 4; y < reg.h - 4; ++y) { putpx(reg, 4, y, 0xE4, 0x2A, 0x31); putpx(reg, reg.w - 5, y, 0xE4, 0x2A, 0x31); }
    // diagonal line (Bresenham) as an arrow stand-in
    for (int i = 0; i < (std::min)(reg.w, reg.h); ++i) putpx(reg, 20 + i, 20 + i, 0x1F, 0x78, 0xD7);
    bool annotated = true;
    uint8_t* probe = reg.px(4, 4);
    annotated = (probe[0] == 0xE4 && probe[1] == 0x2A && probe[2] == 0x31);
    report(L"annotate", annotated);

    // 5. PNG encode + write to temp dir
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring outp = std::wstring(tmp) + L"shotmvp_selftest.png";
    bool wrote = png::write(outp.c_str(), reg.w, reg.h, reg.rgba);
    report(L"png_write", wrote);
    if (wrote) {
        DWORD attr = GetFileAttributesW(outp.c_str());
        HANDLE hf = CreateFileW(outp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        bool sig_ok = false, ihdr_ok = false;
        if (hf != INVALID_HANDLE_VALUE) {
            BYTE head[33] = {};
            DWORD rd = 0; ReadFile(hf, head, sizeof(head), &rd, nullptr);
            CloseHandle(hf);
            sig_ok = (rd >= 8 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G');
            // IHDR width/height at offset 16..23 (big endian) after length+type at 8..15
            if (rd >= 24) {
                int w_ = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
                int h_ = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
                ihdr_ok = (w_ == reg.w && h_ == reg.h);
            }
        }
        report(L"png_signature", sig_ok);
        report(L"png_dimensions", ihdr_ok);
        // Card #17: the stored-block writer made a saved frame about the size of its raw pixels.
        // The number is reported so the acceptance test has something to compare against.
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        DWORD raw = (DWORD)((size_t)reg.w * reg.h * 4);
        if (GetFileAttributesExW(outp.c_str(), GetFileExInfoStandard, &fad))
            wprintf(L"     wrote %ls attr=%lu size=%lu bytes (raw pixels %lu)\n",
                    outp.c_str(), attr, fad.nFileSizeLow, raw);
        else
            wprintf(L"     wrote %ls attr=%lu size=unknown (raw pixels %lu)\n",
                    outp.c_str(), attr, raw);
    }

    // 6. clipboard round-trip: copy then read back CF_DIB and compare dimensions
    bool copied = copy_dib_to_clipboard(reg);
    report(L"clipboard_copy", copied);
    if (copied) {
        bool has = IsClipboardFormatAvailable(CF_DIB) != FALSE;
        report(L"clipboard_format", has);
        if (has && OpenClipboard(nullptr)) {
            HANDLE hData = GetClipboardData(CF_DIB);
            bool dim_ok = false; int cw = 0, chh = 0;
            if (hData) {
                void* p = GlobalLock(hData);
                if (p) {
                    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)p;
                    cw = bih->biWidth; chh = abs(bih->biHeight);
                    dim_ok = (cw == reg.w && chh == reg.h);
                    GlobalUnlock(hData);
                }
            }
            CloseClipboard();
            report(L"clipboard_roundtrip", dim_ok);
            wprintf(L"     clipboard DIB %dx%d (expected %dx%d)\n", cw, chh, reg.w, reg.h);
        }
    }

    // 7. hotkey registration: same fallback logic the real run uses, so the test reflects reality.
    HotkeyHost host;
    host.create(nullptr);
    Hotkey hk_cfg = g_cfg.hotkey;                     // copy: fallback may rewrite it
    bool hk = register_with_fallback(host, 9001, hk_cfg, L"Region hotkey");
    report(L"hotkey_register", hk);
    wprintf(L"     requested %ls -> active %ls\n",
            g_cfg.hotkey.to_text().c_str(), hk_cfg.to_text().c_str());
    if (hk) host.unregister_all();

    // 8. settings persistence: save the working combination, reload and compare.
    g_cfg.hotkey = hk_cfg;
    g_cfg.save();
    Config c2; c2.load();
    bool cfg_ok = (c2.name_format == g_cfg.name_format) && (c2.hotkey.vk == g_cfg.hotkey.vk) &&
                  (c2.hotkey.ctrl == g_cfg.hotkey.ctrl) && (c2.hotkey.shift == g_cfg.hotkey.shift) &&
                  (c2.copy_to_clipboard == g_cfg.copy_to_clipboard);
    report(L"settings_persist", cfg_ok);
    wprintf(L"     file hotkey=%ls | reloaded hotkey=%ls (vk 0x%02X vs 0x%02X)\n",
            hk_cfg.to_text().c_str(), c2.hotkey.to_text().c_str(),
            (unsigned)c2.hotkey.vk, (unsigned)g_cfg.hotkey.vk);
    wprintf(L"     save_dir=%ls name_format=%ls\n", c2.save_dir.c_str(), c2.name_format.c_str());

    wprintf(L"\nSELFTEST %ls (%d failures)\n", fails ? L"FAILED" : L"PASSED", fails);
    return fails;
}



static void do_capture(bool region_mode, const RECT* fixed_region = nullptr) {
    DIB img;
    if (region_mode) {
        RECT r;
        if (fixed_region) {                      // -rect x,y,w,h: skip the overlay (used by tests)
            r = *fixed_region;
        } else {
            Overlay ov;
            Selection sel = ov.run();
            if (!sel.ok || sel.w < 4 || sel.h < 4) return;      // cancelled
            r.left = sel.x; r.top = sel.y; r.right = sel.x + sel.w; r.bottom = sel.y + sel.h;
        }
        if (!capture_rect(r.left, r.top, r.right - r.left, r.bottom - r.top, img)) return;
    } else {
        POINT pt; GetCursorPos(&pt);
        HWND h = window_at_point(pt);
        if (!h) h = GetForegroundWindow();
        if (!h || !capture_window(h, img)) return;
    }

    // Output belongs to the editor now: Save writes the file and Copy fills the clipboard at the
    // moment of the click, both of them with the window still open (card #8).
    Editor ed;
    ed.set_output(save_output, copy_output);
    if (!ed.run(img)) notify(L"Capture discarded - nothing was saved.");
}

static BOOL CALLBACK enum_top(HWND h, LPARAM l) {
    if (!IsWindowVisible(h)) return TRUE;
    RECT r; GetWindowRect(h, &r);
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_CHILD) return TRUE;
    wchar_t cls[128]; GetClassNameW(h, cls, 128);
    if (cls[0] == 0) return TRUE;
    // Skip the desktop/shell scaffolding windows.
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"NotifyIconOverflowWindow") == 0)
        return TRUE;
    if (r.right - r.left < 20 || r.bottom - r.top < 20) return TRUE;

    // Prefer the topmost non-tool window under the cursor.
    *(HWND*)l = h;
    return TRUE;
}

static HotkeyHost g_hotkeys;

static void on_hotkey(int id) {
    if (id == HK_REGION)      do_capture(true);
    else if (id == HK_WINDOW) do_capture(false);
}

// Single argument parser shared by both entry points. MinGW's -municode startup calls
// wmain(argc, argv) for the console subsystem and wWinMain for the GUI subsystem, so we
// parse argv directly instead of re-parsing a reconstructed command line.
struct Args {
    bool region = true;        // default hotkey mode is region capture
    bool window = false;
    bool selftest = false;
    bool grab_now = false;     // -grab: fire one capture immediately, no hotkey needed
    bool open_file = false;
    bool has_fixed = false;    // -rect x,y,w,h: capture this region without showing the overlay
    RECT fixed{};              //          (the acceptance tests need a deterministic region)
    std::wstring path;
};

static Args parse_args(int argc, wchar_t** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        wchar_t* a_ = argv[i];
        if (_wcsicmp(a_, L"-window") == 0)   { a.region = false; a.window = true; }
        else if (_wcsicmp(a_, L"-region") == 0) { a.region = true; }
        else if (_wcsicmp(a_, L"-grab") == 0)   { a.grab_now = true; }
        else if (_wcsicmp(a_, L"-selftest") == 0) { a.selftest = true; }
        else if (_wcsicmp(a_, L"-rect") == 0 && i + 1 < argc) {
            int x, y, w, h;
            if (swscanf(argv[++i], L"%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 1 && h > 1) {
                a.fixed.left = x; a.fixed.top = y; a.fixed.right = x + w; a.fixed.bottom = y + h;
                a.has_fixed = true; a.region = true; a.grab_now = true;
            }
        }
        else if (a_[0] == 0) { /* empty argument */ }
        else { a.path = a_; a.open_file = true; }
    }
    return a;
}

// Try the configured hotkey; if another program owns it, fall back through a list of
// unlikely-to-collide combinations so the app is always usable out of the box.
static bool register_with_fallback(HotkeyHost& host, int id, Hotkey& hk,
                                   const wchar_t* label_for_report) {
    if (host.register_key(id, hk.modifiers(), hk.vk)) return true;

    static const Hotkey fallbacks[] = {
        {true, true, false, 'P'},   // Ctrl+Shift+P
        {true, true, false, VK_SNAPSHOT},  // Ctrl+Shift+PrintScreen
        {true, false, true, 'A'},   // Ctrl+Alt+A
        {true, false, true, 'S'},   // Ctrl+Alt+S
        {true, true, false, VK_F6}, // Ctrl+Shift+F6
    };
    for (const Hotkey& f : fallbacks) {
        if (f.vk == hk.vk && f.modifiers() == hk.modifiers()) continue;
        if (host.register_key(id, f.modifiers(), f.vk)) {
            hk = f;                    // adopt the working combination
            wchar_t m[256];
            wsprintfW(m, L"%ls: configured combination is taken by another program. Using %ls instead.",
                      label_for_report, f.to_text().c_str());
            OutputDebugStringW(m);
            return true;
        }
    }
    return false;
}

static int run_mode(const Args& a) {
    g_cfg.load();

    if (a.selftest) return run_selftest();

    if (a.open_file && !a.path.empty()) {
        MessageBoxW(nullptr, L"Opening existing files is not in this MVP build yet.\nUse the capture hotkey.",
                    L"ShotMvp", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Hotkeys live on their own hidden window so the editor's nested message loop cannot steal them.
    g_hotkeys.create(&on_hotkey);
    bool hk_ok1 = a.region ? register_with_fallback(g_hotkeys, HK_REGION, g_cfg.hotkey, L"Region hotkey") : false;
    bool hk_ok2 = register_with_fallback(g_hotkeys, HK_WINDOW, g_cfg.hotkey_window, L"Window hotkey");

    if (!hk_ok1 && !hk_ok2) {
        MessageBoxW(nullptr,
                    L"Could not register any capture hotkey (all candidates are used by other programs).\n"
                    L"Edit settings.ini next to ShotMvp.exe and restart.",
                    L"ShotMvp", MB_OK | MB_ICONWARNING);
    } else {
        // Persist whatever combination actually works so the UI and settings agree.
        g_cfg.save();
    }

    // One-shot modes: capture immediately, then exit. -grab picks the mode from -region/-window.
    if (a.grab_now || (a.window && !a.region)) {
        do_capture(a.window ? false : true, a.has_fixed ? &a.fixed : nullptr);
        g_hotkeys.unregister_all();
        return 0;
    }

    int rc = g_hotkeys.pump();
    g_hotkeys.unregister_all();
    return rc;
}

int wmain(int argc, wchar_t** argv) {
    enable_dpi_awareness();          // must run before any window/metrics call
    return run_mode(parse_args(argc, argv));
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdline, int show) {
    enable_dpi_awareness();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline ? cmdline : L"", &argc);
    Args a = parse_args(argc, argv);
    if (argv) LocalFree(argv);
    return run_mode(a);
}




