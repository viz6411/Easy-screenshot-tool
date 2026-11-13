// png_out.h - minimal PNG writer (own stored-block deflate; no zlib dependency)
#pragma once
#include <windows.h>
#include <vector>
#include <cstring>

namespace png {

inline uint32_t crc32_of(const uint8_t* d, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline void put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16));
    o.push_back((uint8_t)(v >> 8));  o.push_back((uint8_t)v);
}

inline void chunk(std::vector<uint8_t>& o, const char* type, const uint8_t* data, size_t n) {
    put32(o, (uint32_t)n);
    size_t start = o.size();
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)type[i]);
    if (n) o.insert(o.end(), data, data + n);
    uint32_t c = crc32_of(&o[start], 4 + n);
    put32(o, c);
}

// Deflate using stored (uncompressed) blocks: reliable and dependency-free.
inline void deflate_stored(std::vector<uint8_t>& out, const uint8_t* src, size_t n) {
    // zlib header: CM=8 (deflate), CINFO=7 (32K window), FLEVEL=0
    out.push_back(0x78); out.push_back(0x01);
    size_t pos = 0;
    const size_t MAXBLOCK = 65535;
    if (n == 0) {                       // single empty final block
        out.push_back(1); put32(out, 0); out.push_back(0xFF); out.push_back(0xFF);
        out.push_back(0x00); out.push_back(0x00);   // placeholder: no LEN/NLEN for empty
        out.resize(out.size() - 2);
    }
    while (pos < n) {
        size_t len = (std::min)(MAXBLOCK, n - pos);
        bool last = (pos + len >= n);
        out.push_back(last ? 1 : 0);
        out.push_back((uint8_t)(len & 0xFF));  out.push_back((uint8_t)((len >> 8) & 0xFF));
        uint16_t nlen = (uint16_t)~(uint16_t)len;
        out.push_back((uint8_t)(nlen & 0xFF)); out.push_back((uint8_t)((nlen >> 8) & 0xFF));
        out.insert(out.end(), src + pos, src + pos + len);
        pos += len;
    }
    // adler32
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) { a = (a + src[i]) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    put32(out, adler);
}

// rgba: width*height*4 bytes, row-major, top-down.
inline bool write(const wchar_t* path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);                                  // filter: None
        const uint8_t* row = &rgba[(size_t)y * w * 4];
        raw.insert(raw.end(), row, row + (size_t)w * 4);
    }
    std::vector<uint8_t> idat;
    deflate_stored(idat, raw.data(), raw.size());

    std::vector<uint8_t> f;
    const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    f.insert(f.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type RGBA
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", idat.data(), idat.size());
    chunk(f, "IEND", nullptr, 0);

    HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    while (off < f.size()) {
        DWORD written = 0;
        DWORD toWrite = (DWORD)(std::min)(size_t(1u << 20), f.size() - off);
        if (!WriteFile(hf, &f[off], toWrite, &written, nullptr) || written == 0) {
            CloseHandle(hf); return false;
        }
        off += written;
    }
    CloseHandle(hf);
    return true;
}

} // namespace png
