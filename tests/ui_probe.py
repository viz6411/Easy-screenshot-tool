"""Shared pixel and input helpers for the stage-1 acceptance tooling.

The editor paints everything through Direct2D, so the only honest check of what it drew is to
read pixels back off the screen and to drive real mouse/keyboard events. Everything here is
measurement or input - no production code lives in it.
"""
import ctypes, ctypes.wintypes as W, time, struct, zlib

u = ctypes.windll.user32
g = ctypes.windll.gdi32

# Surfaces as declared in theme.h (dark ramp). Keeping them here means the tests fail loudly if
# the production theme changes without the design tokens being revisited.
CANVAS = (0x1A, 0x1C, 0x20)
FLOATING = (0x21, 0x24, 0x29)
CONTROL = (0x36, 0x3D, 0x47)
CONTROL_HOVER = (0x42, 0x4A, 0x56)
ACCENT_FILL = (0x3E, 0x3F, 0x56)
PALETTE = [(0xE4, 0x2A, 0x31), (0x31, 0x9E, 0x14), (0xC8, 0x3C, 0xB4), (0x1F, 0x78, 0xD7),
           (0xE8, 0xB8, 0x00), (0x22, 0x22, 0x22), (0xFF, 0xFF, 0xFF)]
# Near-black is within 8 levels of the pill surface and near-white of the caption text, so the
# extremes need a tighter match plus a size sanity check or they match the wrong thing entirely.
SWATCH_TOL = [8, 8, 8, 8, 8, 3, 6]


def make_probe_dpi_aware():
    """The probe has to be per-monitor aware too, or Windows virtualises every coordinate."""
    try:
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            u.SetProcessDPIAware()


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def near(c, t, tol=8):
    return all(abs(a - b) <= tol for a, b in zip(c, t))


class DEVMODEW(ctypes.Structure):
    _fields_ = [("dmDeviceName", W.WCHAR * 32), ("dmSpecVersion", W.WORD), ("dmDriverVersion", W.WORD),
                ("dmSize", W.WORD), ("dmDriverExtra", W.WORD), ("dmFields", W.DWORD),
                ("dmPositionX", W.LONG), ("dmPositionY", W.LONG),
                ("dmDisplayOrientation", W.DWORD), ("dmDisplayFixedOutput", W.DWORD),
                ("dmColor", W.SHORT), ("dmDuplex", W.SHORT), ("dmYResolution", W.SHORT),
                ("dmTTOption", W.SHORT), ("dmCollate", W.SHORT), ("dmFormName", W.WCHAR * 32),
                ("dmLogPixels", W.WORD), ("dmBitsPerPel", W.DWORD),
                ("dmPelsWidth", W.DWORD), ("dmPelsHeight", W.DWORD),
                ("dmDisplayFlags", W.DWORD), ("dmDisplayFrequency", W.DWORD),
                ("dmICMMethod", W.DWORD), ("dmICMIntent", W.DWORD),
                ("dmMediaType", W.DWORD), ("dmDitherType", W.DWORD),
                ("dmReserved1", W.DWORD), ("dmReserved2", W.DWORD),
                ("dmPanningWidth", W.DWORD), ("dmPanningHeight", W.DWORD)]


def physical_resolution():
    dm = DEVMODEW(); dm.dmSize = ctypes.sizeof(dm)
    ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(dm))   # ENUM_CURRENT_SETTINGS
    return dm.dmPelsWidth, dm.dmPelsHeight


class Frame:
    def __init__(self, w, h, rgb, x=0, y=0):
        self.w, self.h, self.rgb, self.x, self.y = w, h, rgb, x, y

    def at(self, x, y):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return (0, 0, 0)
        i = (y * self.w + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]

    def screen(self, x, y):
        return self.x + int(x), self.y + int(y)

    def count(self, target, tol=8, x0=0, y0=0, x1=None, y1=None, step=1):
        x1 = self.w if x1 is None else x1
        y1 = self.h if y1 is None else y1
        n = 0
        for y in range(y0, y1, step):
            for x in range(x0, x1, step):
                if near(self.at(x, y), target, tol): n += 1
        return n

    def runs(self, y, match, x0=0, x1=None, min_len=1):
        """Contiguous runs on row y where match(colour) is true."""
        x1 = self.w if x1 is None else x1
        out, x = [], x0
        while x < x1:
            if match(self.at(x, y)):
                s = x
                while x < x1 and match(self.at(x, y)): x += 1
                if x - s >= min_len: out.append((s, x - 1, x - s))
            else:
                x += 1
        return out

    def col_runs(self, x, match, y0=0, y1=None, min_len=1):
        """Same vertically: contiguous runs down column x."""
        y1 = self.h if y1 is None else y1
        out, y = [], y0
        while y < y1:
            if match(self.at(x, y)):
                s = y
                while y < y1 and match(self.at(x, y)): y += 1
                if y - s >= min_len: out.append((s, y - 1, y - s))
            else:
                y += 1
        return out



class BIH(ctypes.Structure):
    _fields_ = [("biSize", W.DWORD), ("biWidth", W.LONG), ("biHeight", W.LONG),
                ("biPlanes", W.WORD), ("biBitCount", W.WORD), ("biCompression", W.DWORD),
                ("biSizeImage", W.DWORD), ("bxpm", W.LONG), ("bypm", W.LONG),
                ("biClrUsed", W.DWORD), ("biClrImportant", W.DWORD)]


class BI(ctypes.Structure):
    _fields_ = [("h", BIH), ("c", W.DWORD * 3)]


def grab_screen_rect(x, y, w, h):
    """Pixels straight off the screen - works no matter how the window renders."""
    sdc = u.GetDC(None)
    mdc = g.CreateCompatibleDC(sdc)
    bmp = g.CreateCompatibleBitmap(sdc, w, h)
    old = g.SelectObject(mdc, bmp)
    g.BitBlt(mdc, 0, 0, w, h, sdc, x, y, 0x00CC0020)      # SRCCOPY | CAPTUREBLT
    bi = BI(); bi.h.biSize = ctypes.sizeof(BIH)
    bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    got = g.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    g.SelectObject(mdc, old); g.DeleteDC(mdc); g.DeleteObject(bmp); u.ReleaseDC(None, sdc)
    if not got:
        return None
    raw = buf.raw
    out = bytearray(w * h * 3)
    for i in range(w * h):
        o = i * 4
        out[i * 3] = raw[o + 2]; out[i * 3 + 1] = raw[o + 1]; out[i * 3 + 2] = raw[o]
    return out


EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, W.HWND, W.LPARAM)


def find_window(class_prefix):
    res = []

    def cb(h, l):
        c = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(h, c, 256)
        if c.value.startswith(class_prefix) and u.IsWindowVisible(h):
            r = W.RECT(); u.GetWindowRect(h, ctypes.byref(r))
            res.append((h, c.value, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True

    u.EnumWindows(EnumProc(cb), 0)
    return res[0] if res else None


def wait_window(class_prefix, tries=30, delay=0.4):
    for _ in range(tries):
        w = find_window(class_prefix)
        if w: return w
        time.sleep(delay)
    return None


def client_origin(hwnd):
    """Screen position of the client area's (0,0). The editor hit-tests in client coordinates, so
    input has to be aimed through this or every click lands offset by the title bar and border."""
    pt = W.POINT(0, 0)
    u.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def capture_window(class_prefix):
    win = find_window(class_prefix)
    if not win: return None
    hwnd, cls, x, y, w, h = win
    rgb = grab_screen_rect(x, y, w, h)
    if rgb is None: return None
    fr = Frame(w, h, rgb, x, y)
    # Remember where the client area sits inside the window: everything the app paints lives
    # there, and the DWM frame border must not be mistaken for content.
    r = W.RECT(); u.GetClientRect(hwnd, ctypes.byref(r))
    ox, oy = client_origin(hwnd)
    fr.client = (ox - x, oy - y, r.right, r.bottom)
    return fr



# ------------------------------------------------------------ measurement -------------------
def find_pill(fr, max_row=220):
    """The pill is the widest run of the floating surface colour near the top. Its height comes
    from a column inside the pill's own left padding - scanning rows by colour would let the
    captured screenshot below it extend the box."""
    best = (0, 0, 0)
    for y in range(15, min(fr.h, max_row)):
        for s, e, ln in fr.runs(y, lambda c: near(c, FLOATING, 5), 1, fr.w - 1):
            if ln > best[0]: best = (ln, y, s)
    runlen, ry, rx0 = best
    if runlen < fr.w * 0.3:
        return None
    xs = [x for x in range(max(0, rx0 - 400), min(fr.w, rx0 + runlen + 400))
          if near(fr.at(x, ry), FLOATING, 5)]
    x0, x1 = min(xs), max(xs)
    probe = x0 + 14
    vert = fr.col_runs(probe, lambda c: near(c, FLOATING, 5), max(0, ry - 120), min(fr.h, ry + 240), 20)
    if not vert:
        return None
    y0, y1, _ = max(vert, key=lambda r: r[2])
    return {'x0': x0, 'x1': x1, 'y0': y0, 'y1': y1, 'cy': (y0 + y1) // 2,
            'w': x1 - x0 + 1, 'h': y1 - y0 + 1}


def control_runs(fr, pill):
    """Controls read cleanly on a row just inside the control band. Two traps: the pill has inner
    padding (s6) before the controls start, and every control carries an icon in its middle, so
    sampling too high lands in the padding and too low splits each control in two."""
    best, best_score = [], -1
    for row in (pill['y0'] + 14, pill['y0'] + 16, pill['y0'] + 18, pill['y0'] + 22):
        r = fr.runs(row, lambda c: near(c, CONTROL, 10) or near(c, CONTROL_HOVER, 10)
                    or near(c, ACCENT_FILL, 14), pill['x0'] + 1, pill['x1'], 24)
        good = sum(1 for s, e, ln in r if 40 <= ln <= 80)
        if good > best_score:
            best, best_score = r, good
    return best


def swatch_boxes(fr, pill):
    """Colour swatches, found by clustering matching pixels into x-groups. Clustering matters:
    the annotation colour is also used for the line-width samples further along the pill, so
    matching a colour anywhere in the pill would merge the swatch with those samples."""
    out = []
    for i, t in enumerate(PALETTE):
        tol = SWATCH_TOL[i]
        cols = {}
        for y in range(pill['y0'] + 5, pill['y1'] - 3):
            for x in range(pill['x0'] + 2, pill['x1'] - 1, 2):
                if near(fr.at(x, y), t, tol):
                    cols.setdefault(x, []).append(y)
        xs = sorted(cols)
        if not xs:
            out.append(None); continue
        clusters, cur = [], [xs[0]]
        for x in xs[1:]:
            if x - cur[-1] <= 6: cur.append(x)
            else: clusters.append(cur); cur = [x]
        clusters.append(cur)
        cl = max(clusters, key=lambda c: sum(len(cols[x]) for x in c))
        ys = [y for x in cl for y in cols[x]]
        x0, x1 = cl[0], cl[-1]
        w0, h0 = x1 - x0 + 1, max(ys) - min(ys) + 1
        if w0 > 70 or h0 > 70:
            out.append(None); continue
        out.append({'x0': x0, 'x1': x1, 'y0': min(ys), 'y1': max(ys), 'w': w0, 'h': h0,
                    'cx': (x0 + x1) // 2, 'cy': (min(ys) + max(ys)) // 2})
    return out


def dominant_state(fr, rect, tol=6):
    """Classify a control from what is painted inside it. Tolerances stay tight on purpose: the
    control fill (#363D47) and the active fill (#3E3F56) differ by 15 in blue, so a loose
    tolerance reports 'active' everywhere and the test proves nothing."""
    x0, y0, x1, y1 = rect
    counts = {'normal': 0, 'hover': 0, 'active': 0, 'disabled': 0}
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = fr.at(x, y)
            if near(c, CONTROL, tol): counts['normal'] += 1
            elif near(c, CONTROL_HOVER, tol): counts['hover'] += 1
            elif near(c, ACCENT_FILL, tol): counts['active'] += 1
            elif near(c, CANVAS, tol): counts['disabled'] += 1
    best = max(counts, key=lambda k: counts[k])
    return best if counts[best] >= 60 else 'unknown'


def outline_pixels(fr, pill, cx, ref=FLOATING):

    """Count pixels around a slot centre that read as an outline: brighter than the surface they
    sit on, but not a fill. This is how a near-black swatch is verified - its fill is the same
    brightness as the pill, so only the outline proves it is a control and not a hole."""
    target = tuple(min(255, int(ref[c] + (255 - ref[c]) * 0.14)) for c in range(3))   # borderStrong
    n = 0
    for y in range(pill['y0'] + 8, pill['y1'] - 7):
        for x in range(cx - 26, cx + 27):
            c = fr.at(x, y)
            if all(abs(a - b) <= 14 for a, b in zip(c, target)) and lum(c) > lum(ref) + 8:
                n += 1
    return n


def content_bbox(fr, pill, strip_top):
    """Locate the captured block inside the client area: columns that are not the flat editor
    canvas for almost the whole distance down. The window frame is excluded because it belongs to
    DWM, not to the app, and a dark screenshot can otherwise be mistaken for the canvas."""
    cx0, cy0, cw, ch = getattr(fr, 'client', (8, 0, fr.w - 16, fr.h))
    lo, hi = pill['y1'] + 3, min((strip_top - 3) if strip_top else cy0 + ch - 2, cy0 + ch - 3)
    band = hi - lo
    if band < 40:
        return None

    def not_canvas(c):
        return max(abs(a - b) for a, b in zip(c, CANVAS)) > 8

    good = [x for x in range(cx0 + 4, cx0 + cw - 4)
            if sum(1 for y in range(lo, hi, 2) if not_canvas(fr.at(x, y))) >= (band // 2) * 0.9]
    if len(good) < 2:
        return None
    x_left, x_right = good[0], good[-1]

    # The height is taken from several columns at once: one column can cross a flat patch of the
    # screenshot and under-report where the image really ends.
    starts, ends = [], []
    for i in range(9):
        x = x_left + (x_right - x_left) * i // 8
        for s, e, ln in fr.col_runs(x, not_canvas, lo, hi, 8):
            if ln >= band * 0.4:
                starts.append(s); ends.append(e)
    if not starts:
        return None
    y0, y1 = min(starts), max(ends)
    return {'x0': x_left, 'x1': x_right, 'y0': y0, 'y1': y1, 'w': x_right - x_left + 1, 'h': y1 - y0 + 1}


def strip_geometry(fr, pill):
    """The strip separator is a hairline spanning almost the whole window with the flat canvas on
    BOTH sides of it. The captured image also has a hairline outline and, when the screenshot is
    nearly as wide as the window, that outline looks like a separator - the canvas-above test is
    what tells them apart."""
    cx0, cy0, cw, ch = getattr(fr, 'client', (8, 0, fr.w - 16, fr.h))
    lo, hi = cx0 + 18, cx0 + cw - 18
    sep = None
    for y in range(max(pill['y1'] + 4, cy0 + ch - 150), cy0 + ch - 8):
        hits = 0
        for x in range(lo, hi, 4):
            above, c, below = fr.at(x, y - 4), fr.at(x, y), fr.at(x, y + 5)
            canvas_above = max(abs(a - b) for a, b in zip(above, CANVAS)) <= 7
            canvas_below = max(abs(a - b) for a, b in zip(below, CANVAS)) <= 7
            if canvas_above and canvas_below and lum(CANVAS) + 4 < lum(c) < lum(CANVAS) + 40 \
                    and max(c) - min(c) < 20:
                hits += 1
        if hits >= (hi - lo) // 4 * 0.85:
            sep = y; break
    start = sep + 1 if sep else max(pill['y1'] + 4, cy0 + ch - 150)
    text = [y for y in range(start, cy0 + ch - 2) if sum(1 for x in range(cx0, cx0 + cw, 3) if lum(fr.at(x, y)) > 110) > 12]
    return sep, text


# ------------------------------------------------------------ input -------------------------
def move(x, y, settle=0.3):
    u.SetCursorPos(int(x), int(y)); time.sleep(settle)


def click(x, y, settle=0.4):
    move(x, y)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.12)
    u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(settle)


def drag(x1, y1, x2, y2, steps=16, settle=0.5):
    move(x1, y1)
    u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.2)
    for i in range(1, steps + 1):
        u.SetCursorPos(int(x1 + (x2 - x1) * i / steps), int(y1 + (y2 - y1) * i / steps))
        time.sleep(0.035)
    u.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(settle)


def press_key(vk, settle=0.4):
    u.keybd_event(vk, 0, 0, 0); time.sleep(0.08)
    u.keybd_event(vk, 0, 2, 0); time.sleep(settle)


def save_png(path, w, h, rgb):
    out = bytearray()
    for y in range(h):
        out += b'\x00' + rgb[y * w * 3:(y + 1) * w * 3]
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(out), 6)) + chunk(b'IEND', b''))
    open(path, 'wb').write(png)
    return path


def clipboard_dib():
    """(width, height, bytes) of a CF_DIB/CF_DIBV5 on the clipboard, else None."""
    CF_DIB, CF_DIBV5 = 8, 17
    u.OpenClipboard.restype = W.BOOL
    u.IsClipboardFormatAvailable.restype = W.BOOL
    # Handles are pointers: without these restypes ctypes truncates them to 32 bits on a 64-bit
    # process and the block lock then silently fails.
    u.GetClipboardData.restype = ctypes.c_void_p
    k = ctypes.windll.kernel32
    k.GlobalLock.argtypes = [ctypes.c_void_p]; k.GlobalLock.restype = ctypes.c_void_p
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]; k.GlobalUnlock.restype = W.BOOL
    k.GlobalSize.argtypes = [ctypes.c_void_p]; k.GlobalSize.restype = ctypes.c_size_t
    for attempt in range(8):
        if u.OpenClipboard(None): break
        time.sleep(0.15)
    else:
        return None
    try:
        fmt = CF_DIBV5 if u.IsClipboardFormatAvailable(CF_DIBV5) else CF_DIB
        if not u.IsClipboardFormatAvailable(fmt): return None
        h = u.GetClipboardData(fmt)
        if not h: return None
        p = k.GlobalLock(h)
        if not p: return None
        size = k.GlobalSize(h)
        data = ctypes.string_at(p, size)
        k.GlobalUnlock(h)
        if size < 40: return None
        biw, bih = struct.unpack('<ii', data[4:12])
        return abs(biw), abs(bih), data
    finally:
        u.CloseClipboard()
