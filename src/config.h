// config.h - settings persisted next to the exe (simple, human-readable)
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cwchar>

struct Hotkey {
    bool ctrl = true, shift = true, alt = false;
    int vk = 'C';

    std::wstring to_text() const {
        std::wstring s;
        if (ctrl)  s += L"Ctrl+";
        if (alt)   s += L"Alt+";
        if (shift) s += L"Shift+";

        // GetKeyNameTextW wants a scan code in the low word, extended flag in bit 24.
        UINT vs = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        LONG param = (LONG)(vs << 16);
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) param |= (1 << 24);   // extended keys
        wchar_t buf[32] = {};
        int n = GetKeyNameTextW(param, buf, 32);
        if (n > 0 && buf[0]) { s += buf; return s; }

        // Fallbacks for keys MapVirtualKey cannot name.
        switch (vk) {
            case VK_SNAPSHOT: s += L"PrintScreen"; break;
            case VK_SPACE:    s += L"Space";       break;
            case VK_INSERT:   s += L"Insert";      break;
            case VK_DELETE:   s += L"Delete";      break;
            case VK_HOME:     s += L"Home";        break;
            case VK_END:      s += L"End";         break;
            case VK_PRIOR:    s += L"PgUp";        break;
            case VK_NEXT:     s += L"PgDn";        break;
            case VK_LWIN:     s += L"Win";         break;
            default: { wchar_t t[8]; wsprintfW(t, L"0x%02X", vk); s += t; }
        }
        return s;
    }

    UINT modifiers() const {
        UINT m = 0;
        if (ctrl)  m |= MOD_CONTROL;
        if (alt)   m |= MOD_ALT;
        if (shift) m |= MOD_SHIFT;
        if (vk >= VK_LWIN && vk <= VK_RWIN) m |= MOD_WIN;
        return m;
    }
};

struct Config {
    Hotkey hotkey;                 // region capture (default Ctrl+Shift+C, as requested)
    Hotkey hotkey_window{true, true, false, 'W'};   // active-window capture
    bool   copy_to_clipboard = true;
    std::wstring save_dir;         // empty -> Documents\Screenshots
    std::wstring name_format = L"screenshot_%Y-%m-%d_%H-%M-%S";
    bool   save_ask = false;       // 1 = ask for the place and name on every Save
    // default_save_dir() fills save_dir silently, so "the user has never picked a folder" cannot be
    // read from save_dir.empty(); this flag is set only when save_dir= was really in the file (8.7).
    bool   save_dir_chosen = false;

    static std::wstring path() {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\'); if (slash) *slash = 0;
        return std::wstring(exe) + L"\\settings.ini";
    }

    void load() {
        std::wstring p = path();
        FILE* f = _wfopen(p.c_str(), L"r, ccs=UTF-8");
        if (!f) { default_save_dir(); return; }
        wchar_t line[512];
        while (fgetws(line, 512, f)) {
            // Strip a UTF-8 BOM if present (ccs=UTF-8 writes one; it would corrupt the first key).
            wchar_t* p0 = line;
            if (p0[0] == L'\xFEFF') ++p0;
            while (*p0 && (p0[0] == L'\r' || p0[0] == L'\n')) ++p0;
            while (*p0 == L' ' || *p0 == L'\t') ++p0;
            wchar_t* eq = wcschr(p0, L'=');
            if (!eq || p0[0] == L'#') continue;
            *eq = 0;
            std::wstring key = p0;
            std::wstring val = eq + 1;
            while (!val.empty() && (val.back() == L'\r' || val.back() == L'\n')) val.pop_back();

            if (key == L"hotkey")           parse_hotkey(val, hotkey);
            else if (key == L"hotkey_window") parse_hotkey(val, hotkey_window);
            else if (key == L"clipboard")   copy_to_clipboard = (val == L"1");
            else if (key == L"save_dir")    { save_dir = val; save_dir_chosen = !val.empty(); }
            else if (key == L"name_format") name_format = val;
            else if (key == L"save_ask")    save_ask = (val == L"1");
        }
        fclose(f);
        default_save_dir();
    }

    void save() const {
        std::wstring p = path();
        FILE* f = _wfopen(p.c_str(), L"w, ccs=UTF-8");
        if (!f) return;
        fwprintf(f, L"# ShotMvp settings\n");
        fwprintf(f, L"hotkey=%ls\n", hotkey.to_text().c_str());
        fwprintf(f, L"hotkey_window=%ls\n", hotkey_window.to_text().c_str());
        fwprintf(f, L"clipboard=%d\n", copy_to_clipboard ? 1 : 0);
        fwprintf(f, L"save_dir=%ls\n", save_dir.c_str());
        fwprintf(f, L"save_ask=%d\n", save_ask ? 1 : 0);
        fwprintf(f, L"name_format=%ls\n", name_format.c_str());
        fclose(f);
    }

    void default_save_dir() {
        if (!save_dir.empty()) return;
        wchar_t dir[MAX_PATH] = {};
        if (SHGetFolderPathW_(dir)) save_dir = dir;
        else { GetModuleFileNameW(nullptr, dir, MAX_PATH); wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) { *sl = 0; save_dir = dir; } }
    }

private:
    static bool SHGetFolderPathW_(wchar_t* out) {
        HMODULE sh = LoadLibraryW(L"shell32.dll");
        if (!sh) return false;
        typedef HRESULT(WINAPI* Fn)(int, HANDLE, HANDLE, DWORD, wchar_t*);
        Fn fn = (Fn)GetProcAddress(sh, "SHGetFolderPathW");
        if (!fn) return false;
        // CS_PERSONAL = 0x0005 -> Documents
        return fn(0x0005, nullptr, nullptr, 0, out) == S_OK && out[0];
    }

    static void parse_hotkey(const std::wstring& v, Hotkey& hk) {
        hk.ctrl = hk.shift = hk.alt = false;
        size_t i = 0;
        while (true) {
            size_t plus = v.find(L'+', i);
            std::wstring part = (plus == std::wstring::npos) ? v.substr(i) : v.substr(i, plus - i);
            if (part.empty()) break;
            if (_wcsicmp(part.c_str(), L"Ctrl") == 0)  hk.ctrl = true;
            else if (_wcsicmp(part.c_str(), L"Shift") == 0) hk.shift = true;
            else if (_wcsicmp(part.c_str(), L"Alt") == 0)   hk.alt = true;
            else {
                // Virtual-key codes for letters and digits ARE their uppercase ASCII values, so use
                // that mapping directly. VkKeyScanW is unreliable here: it returns -1 for 'P' on this
                // machine's keyboard layout, which silently left the hotkey at its default ('C') and
                // made every save/reload round-trip fail.
                wchar_t up = (wchar_t)toupper((int)part[0]);
                if ((up >= L'A' && up <= L'Z') || (up >= L'0' && up <= L'9')) {
                    hk.vk = (int)up;
                } else {
                    int vk = VkKeyScanW(up);
                    if (vk != -1) hk.vk = vk & 0xFF;
                }
                if (_wcsicmp(part.c_str(), L"PrintScreen") == 0) hk.vk = VK_SNAPSHOT;
                if (part.size() > 1 && part[0] == '0' && part[1] == 'x')
                    hk.vk = (int)wcstoul(part.c_str(), nullptr, 16);
            }
            if (plus == std::wstring::npos) break;
            i = plus + 1;
        }
    }
};


