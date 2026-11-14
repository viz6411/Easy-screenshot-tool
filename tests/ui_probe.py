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
CONTROL_PRESSED = (0x4C, 0x55, 0x61)
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


def dib_from_dc(hdc, bmp, w, h):
    """Bitmap -> top-down RGB bytes through GetDIBits. GetBitmapBits would hand back bottom-up BGRA,
    which reads as a vertically flipped image with red and blue swapped."""
    bi = BI(); bi.h.biSize = ctypes.sizeof(BIH)
    bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    if not g.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0):
        return None
    raw = buf.raw
    out = bytearray(w * h * 3)
    for i in range(w * h):
        o = i * 4
        out[i * 3] = raw[o + 2]; out[i * 3 + 1] = raw[o + 1]; out[i * 3 + 2] = raw[o]
    return out


def grab_screen_rect(x, y, w, h):
    """Pixels straight off the screen - works no matter how the window renders."""
    sdc = u.GetDC(None)
    mdc = g.CreateCompatibleDC(sdc)
    bmp = g.CreateCompatibleBitmap(sdc, w, h)
    old = g.SelectObject(mdc, bmp)
    g.BitBlt(mdc, 0, 0, w, h, sdc, x, y, 0x00CC0020)      # SRCCOPY | CAPTUREBLT
    out = dib_from_dc(mdc, bmp, w, h)
    g.SelectObject(mdc, old); g.DeleteDC(mdc); g.DeleteObject(bmp); u.ReleaseDC(None, sdc)
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


def window_rgb(hwnd):
    """One window's own pixels through PrintWindow (PW_RENDERFULLCONTENT).

    A screen BitBlt of the window's rectangle blends whatever overlaps it - and the window under
    test shares the screen with the owner's work. PrintWindow returns the window's own surface
    (proven on this machine by the stage-2 picker frames, docs/TOOLBAR-SPEC.md 8.8).
    """
    r = W.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    sdc = u.GetDC(None)
    mdc = g.CreateCompatibleDC(sdc)
    bmp = g.CreateCompatibleBitmap(sdc, w, h)
    old = g.SelectObject(mdc, bmp)
    u.PrintWindow(hwnd, mdc, 2)
    rgb = dib_from_dc(mdc, bmp, w, h)
    g.SelectObject(mdc, old); g.DeleteObject(bmp); g.DeleteDC(mdc); u.ReleaseDC(None, sdc)
    if rgb is None:
        return None
    return r.left, r.top, w, h, rgb


def capture_window(class_prefix):
    win = find_window(class_prefix)
    if not win: return None
    hwnd = win[0]
    if u.IsIconic(hwnd):
        # SW_RESTORE (9): the window belongs to a desktop somebody is actually using, and whoever
        # is at the keyboard can minimize the app mid-run. A minimized window sits at -32000,-32000
        # and has no pixels to read.
        u.ShowWindow(hwnd, 9); time.sleep(0.6)
    part = window_rgb(hwnd)
    if not part: return None
    x, y, w, h, rgb = part
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


CHIP_TOP, CHIP_BOTTOM = 8, 70      # rows of the chip square inside the pill; the caption band is
CAPTION_TOP = 74                    # further down and is not part of a control's surface


def pill_ink(c):
    """Anything on the pill that is not the pill surface: a control fill, a recess or an outline."""
    return not near(c, FLOATING, 7)


def group_separators(fr, pill):
    """x of the hairlines between the groups (8.1: four separators between five groups).

    A separator is the only thing in the pill one pixel wide and a whole cell tall (88 px at
    175 %): a control outline is ctlH = 56 px tall, a swatch square 38 px. Height is what tells
    them apart, so this is a measurement of the separation itself - the thing card #19 replaced
    the drawn lines with.
    shortcut: reads the one-row signed layout (cell 88 px); the wrapped layout of level 2/3 needs
    the same scan per row, which is only reachable on a client narrower than 1121 px at 175 %.
    """
    out = []
    for x in range(pill['x0'] + 3, pill['x1'] - 2):
        if any(ln >= 80 for _, _, ln in fr.col_runs(x, pill_ink, pill['y0'] + CHIP_TOP,
                                                    pill['y0'] + CHIP_BOTTOM + 20, 80)):
            out.append(x)
    return out


def chip_boxes_in(fr, x0, x1, y0, y1):
    """Control boxes inside one group span, read as ink across the whole chip band.

    A filled chip (tools, properties), a canvas niche (the output group) and an outline-only chip
    (a history control at rest) all leave a horizontal ink run in this band - but an outline-only
    chip leaves it at its top and bottom border rows only, so the rows are merged rather than
    sampled at one height. Nothing else in the band is 34 px or wider: a separator is 1 px and
    the captions start below the band.
    """
    spans = sorted((s, e) for y in range(y0, y1) for s, e, ln in fr.runs(y, pill_ink, x0, x1, 34))
    out = []
    for s, e in spans:
        if out and s - out[-1][1] <= 2:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([s, e])
    return [(s, e, e - s + 1) for s, e in out]


def pill_controls(fr, pill):
    """The five groups of the pill, each inside its own pair of separators.

    Replaces control_runs(), which sampled one row of filled control surface and so saw only the
    tool chips and the property chips: the history chips sit on the pill surface (bright outline,
    no fill), the output chips sit in a canvas recess, and the colour swatches are neither - the
    old harness could not find «Сохранить» and took its position from an assumed pitch
    (docs/TOOLBAR-SPEC.md 8.8). Here the separators split the pill into the five groups and each
    group is measured on its own terms; the colour group is measured by colour.
    """
    seps = group_separators(fr, pill)
    edges = [pill['x0'] + 2] + seps + [pill['x1'] - 1]
    y0, y1 = pill['y0'] + CHIP_TOP, pill['y0'] + CHIP_BOTTOM
    parts = [chip_boxes_in(fr, edges[i] + 1, edges[i + 1], y0, y1) for i in range(len(edges) - 1)]
    parts += [[], [], [], [], []]
    return {'separators': seps, 'tools': parts[0], 'swatches': swatch_boxes(fr, pill),
            'props': parts[2], 'history': parts[3], 'output': parts[4]}


def control_rect(pill, box):
    """A square in the middle of one control's chip: what is painted there is the control's state,
    and the caption band under the chip is text, not a surface."""
    cx = (box[0] + box[1]) // 2
    return (cx - 20, pill['y0'] + 38 - 20, cx + 20, pill['y0'] + 38 + 20)


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


# What each group's resting surface is, from docs/TOOLBAR-SPEC.md 8.2. The old classifier read
# every canvas-coloured pixel as 'disabled', which is right for a tool chip (a disabled tool is a
# canvas hole) and wrong for the output group, whose resting control IS a canvas recess - so
# «Сохранить» came back as unavailable.
SURFACES = {
    'tool':   [(CONTROL, 'normal'), (CONTROL_HOVER, 'hover'), (ACCENT_FILL, 'active'),
               (CANVAS, 'disabled')],
    'history': [(FLOATING, 'normal'), (CONTROL, 'hover'), (CANVAS, 'disabled')],
    'output': [(CANVAS, 'normal'), (CONTROL, 'hover'), (CONTROL_HOVER, 'hover')],
}


def dominant_state(fr, rect, tol=6, group='tool'):
    """Classify one control from what is painted inside it, knowing which surface that control's
    group rests on. Tolerances stay tight on purpose: the control fill (#363D47) and the active
    fill (#3E3F56) differ by 15 in blue, so a loose tolerance reports 'active' everywhere and the
    test proves nothing. Same reason for the ordering: the pill surface (#212429) and the canvas
    hole (#1A1C20) are 7-9 levels apart, and a loose tolerance reads a recessed control as resting.
    """
    x0, y0, x1, y1 = rect
    counts = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = fr.at(x, y)
            for target, name in SURFACES[group]:
                if near(c, target, tol):
                    counts[name] = counts.get(name, 0) + 1
                    break
    if not counts:
        return 'unknown'
    best = max(counts, key=lambda k: counts[k])
    return best if counts[best] >= 60 else 'unknown'


def outline_length(fr, pill, box):
    """Longest unbroken hairline running down the chip band along either side of a control.

    A control drawn with a border (a tool chip, a recess of the output group) carries a bright
    hairline hugging its edge for the whole height of the chip; a hole punched in the pill (a
    disabled history control) carries none. The band is searched within 4 px of each measured edge
    because controls are centred from a float pitch and can land half a pixel either way. Measuring
    the edge rather than the interior is the point: the interior of a recess and of a hole is the
    same canvas colour plus a glyph, and the earlier version of this measurement was counting the
    glyph's anti-aliasing.
    """
    y0, y1 = pill['y0'] + CHIP_TOP, pill['y0'] + CHIP_BOTTOM
    bright = lambda c: lum(c) > lum(FLOATING) + 5
    best = 0
    for lo, hi in ((box[0] - 4, box[0] + 5), (box[1] - 4, box[1] + 5)):
        for x in range(lo, hi):
            for s, e, ln in fr.col_runs(x, bright, y0, y1, 3):
                best = max(best, ln)
    return best


def content_bbox(fr, pill, strip_top):
    """Locate the captured block inside the client area: columns that hold a solid run of
    screenshot for at least half the distance down. The window frame is excluded because it belongs
    to DWM, not to the app, and a dark screenshot can otherwise be mistaken for the canvas. Half the
    band is what lets a cropped image (shorter than the band, centred in it) be measured too."""
    cx0, cy0, cw, ch = getattr(fr, 'client', (8, 0, fr.w - 16, fr.h))
    lo, hi = pill['y1'] + 3, min((strip_top - 3) if strip_top else cy0 + ch - 2, cy0 + ch - 3)
    band = hi - lo
    if band < 40:
        return None

    def not_canvas(c):
        return max(abs(a - b) for a, b in zip(c, CANVAS)) > 8

    good = [x for x in range(cx0 + 4, cx0 + cw - 4)
            if sum(1 for y in range(lo, hi, 2) if not_canvas(fr.at(x, y))) >= (band // 2) * 0.5]
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


def dominant_fill(fr, rect, tol=6):
    """Which surface a control area is painted with: 'control' (a raised chip), 'canvas' (a
    recess or a hole), 'floating' (the pill surface itself, i.e. no chip at all) or 'accent' (the
    active tool). Card #19 asks the five groups to read as different surfaces, so this is the
    measurement that closes it - dominant_state() answers what a control is doing, not what it is
    made of."""
    x0, y0, x1, y1 = rect
    counts = {'control': 0, 'canvas': 0, 'floating': 0, 'accent': 0}
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = fr.at(x, y)
            if near(c, CONTROL, tol) or near(c, CONTROL_HOVER, tol) or near(c, CONTROL_PRESSED, tol):
                counts['control'] += 1
            elif near(c, CANVAS, tol):
                counts['canvas'] += 1
            elif near(c, FLOATING, tol):
                counts['floating'] += 1
            elif near(c, ACCENT_FILL, 14):
                counts['accent'] += 1
    return max(counts, key=lambda k: counts[k])


def strip_text_span(fr, sep, bottom=None):
    """(x0, x1, ink) - how far the text of the status band reaches. Card #8 puts the confirmation
    of Save/Copy at the right edge of that band («Сохранено: C:\\...»), so this tells a
    confirmation apart from the hotkey hint that is already sitting on the left of the same line.
    """
    bottom = bottom if bottom is not None else fr.h - 3
    x0, x1, ink = None, None, 0
    for y in range((sep or 0) + 5, bottom):
        for x in range(1, fr.w - 2):
            if lum(fr.at(x, y)) > 110:
                ink += 1
                x0 = x if x0 is None else min(x0, x)
                x1 = x if x1 is None else max(x1, x)
    return (x0, x1, ink) if ink else None


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
# Everything here posts messages to the window under test. It never moves the pointer, never clicks
# and never presses a key globally: the machine these checks run on is the owner's working machine,
# and physical injection used to steal its mouse and keyboard for the whole run (card #12).
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0200, 0x0201, 0x0202
WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x0100, 0x0101, 0x0102
VK_SHIFT, VK_CONTROL = 0x10, 0x11


def window_hwnd(class_prefix='ShotMvpEditor', hwnd=None):
    w = find_window(class_prefix)
    return hwnd or (w[0] if w else None)


def lp(x, y):
    """LPARAM of a mouse message in the window's client coordinates."""
    return ((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)


def key_lp(vk, up=False):
    return 1 | (u.MapVirtualKeyW(vk, 0) << 16) | ((1 << 30) if up else 0) | ((1 << 31) if up else 0)


def to_client(hwnd, x, y):
    """Screen point -> client point: hit-testing happens in client coordinates, and ScreenToClient
    is what keeps the click aimed correctly through the title bar and the frame."""
    pt = W.POINT(int(x), int(y)); u.ScreenToClient(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def move(x, y, settle=0.3, hwnd=None):
    h = window_hwnd('ShotMvpEditor', hwnd)
    if not h: return
    u.PostMessageW(h, WM_MOUSEMOVE, 0, lp(*to_client(h, x, y))); time.sleep(settle)


def click(x, y, settle=0.4, hwnd=None):
    h = window_hwnd('ShotMvpEditor', hwnd)
    if not h: return
    cx, cy = to_client(h, x, y)
    u.PostMessageW(h, WM_MOUSEMOVE, 0, lp(cx, cy)); time.sleep(0.12)
    u.PostMessageW(h, WM_LBUTTONDOWN, 1, lp(cx, cy)); time.sleep(0.12)
    u.PostMessageW(h, WM_LBUTTONUP, 0, lp(cx, cy)); time.sleep(settle)


def drag(x1, y1, x2, y2, steps=16, settle=0.5, hwnd=None):
    h = window_hwnd('ShotMvpEditor', hwnd)
    if not h: return
    move(x1, y1, 0.15, h)
    u.PostMessageW(h, WM_LBUTTONDOWN, 1, lp(*to_client(h, x1, y1))); time.sleep(0.2)
    for i in range(1, steps + 1):
        move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 0.035, h)
    u.PostMessageW(h, WM_LBUTTONUP, 0, lp(*to_client(h, x2, y2))); time.sleep(settle)


def press_key(vk, settle=0.4, hwnd=None):
    """Posted to the window, with the scan code in lParam so the app's own TranslateMessage path
    is not relied on. Ctrl combinations need the app to see VK_CONTROL as its own message too -
    see Editor::on_key (card #21)."""
    h = window_hwnd('ShotMvpEditor', hwnd)
    if not h: return
    u.PostMessageW(h, WM_KEYDOWN, vk, key_lp(vk)); time.sleep(0.08)
    u.PostMessageW(h, WM_KEYUP, vk, key_lp(vk, True)); time.sleep(settle)


def ctrl_key(vk, settle=0.6, shift=False, hwnd=None):
    h = window_hwnd('ShotMvpEditor', hwnd)
    if not h: return
    if shift:
        u.PostMessageW(h, WM_KEYDOWN, VK_SHIFT, key_lp(VK_SHIFT)); time.sleep(0.05)
    u.PostMessageW(h, WM_KEYDOWN, VK_CONTROL, key_lp(VK_CONTROL)); time.sleep(0.06)
    press_key(vk, 0.5, h)
    u.PostMessageW(h, WM_KEYUP, VK_CONTROL, key_lp(VK_CONTROL, True))
    if shift:
        u.PostMessageW(h, WM_KEYUP, VK_SHIFT, key_lp(VK_SHIFT, True))
    time.sleep(settle)


def physical_key(vk, ctrl=False, shift=False, settle=0.6):
    """The one thing that genuinely cannot be posted: a capture hotkey the app registers globally
    with RegisterHotKey is fired by the system only for a real keystroke. Used for that and nothing
    else, so the run that touches the owner's keyboard is a single keystroke, not a whole session."""
    if ctrl: u.keybd_event(VK_CONTROL, u.MapVirtualKeyW(VK_CONTROL, 0), 0, 0)
    if shift: u.keybd_event(VK_SHIFT, u.MapVirtualKeyW(VK_SHIFT, 0), 0, 0)
    sc = u.MapVirtualKeyW(vk, 0)
    u.keybd_event(vk, sc, 0, 0); time.sleep(0.08)
    u.keybd_event(vk, sc, 2, 0)
    if shift: u.keybd_event(VK_SHIFT, u.MapVirtualKeyW(VK_SHIFT, 0), 2, 0)
    if ctrl: u.keybd_event(VK_CONTROL, u.MapVirtualKeyW(VK_CONTROL, 0), 2, 0)
    time.sleep(settle)


WM_CHAR = 0x0102
u.VkKeyScanW.restype = ctypes.c_short


def grab_pill():
    """A fresh screenshot of the editor together with the pill found in it - the two are always
    wanted together, and a window that is gone or still painting gives (None, None)."""
    fr = capture_window('ShotMvpEditor')
    return fr, (find_pill(fr) if fr else None)


def click_chip(fr, pill, box):
    """Clicks the middle of the chip square of one measured control."""
    click(*fr.screen((box[0] + box[1]) // 2, pill['y0'] + 38))


def type_text(s, hwnd=None):
    """Each character posted as the WM_CHAR a keyboard would produce - layout-independent, so
    Cyrillic needs no keyboard at all (it used to need a layout lookup and a fallback)."""
    h = window_hwnd('ShotMvpEditor', hwnd)
    if not h: return
    for ch in s:
        u.PostMessageW(h, WM_CHAR, ord(ch), 1 | (1 << 24)); time.sleep(0.03)
    time.sleep(0.4)


def dialog_parts(dlg):
    """[(hwnd, class, caption, rect)] of everything inside a shell dialog, to the depth the standard
    dialogs put their controls at. Used to drive a dialog by its own controls instead of by guessed
    screen coordinates."""
    out = []

    def walk(parent, depth):
        child = None
        while True:
            child = u.FindWindowExW(parent, child, None, None)
            if not child: break
            cls = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(child, cls, 64)
            cap = ctypes.create_unicode_buffer(128)
            u.GetWindowTextW(child, cap, 128)
            r = W.RECT(); u.GetWindowRect(child, ctypes.byref(r))
            out.append((child, cls.value, cap.value, (r.left, r.top, r.right, r.bottom)))
            if depth < 3:
                walk(child, depth + 1)

    walk(dlg, 0)
    return out


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


def crop_up(fr, x0, y0, x1, y1, path, up=1):
    """A part of a frame, enlarged by an integer factor, written as a PNG. Acceptance frames have
    to be readable without a second viewer, and a glyph has to be seen on the real panel, not only
    on the dev's icon sheet."""
    w, h = x1 - x0, y1 - y0
    if up <= 1:
        rows = b''.join(fr.rgb[(y * fr.w + x0) * 3:(y * fr.w + x1) * 3] for y in range(y0, y1))
        return save_png(path, w, h, rows)
    out = bytearray()
    for y in range(y0, y1):
        row = bytearray()
        base = (y * fr.w + x0) * 3
        for x in range(w):
            row += fr.rgb[base + x * 3:base + x * 3 + 3] * up
        out += row * up
    return save_png(path, w * up, h * up, bytes(out))


def dib_to_png(data, path):
    """The CF_DIB bytes as a viewable PNG - what a paste into a chat or an image editor receives."""
    hsize, w, hh, _, bits, comp = struct.unpack('<IiiHHI', data[:20])
    h = abs(hh)
    if bits not in (24, 32) or comp not in (0, 3):
        return None
    stride = (w * bits + 31) // 32 * 4
    out = bytearray()
    for y in range(h):
        row = data[hsize + (y if hh < 0 else h - 1 - y) * stride:]
        per = 4 if bits == 32 else 3
        out += b''.join(row[x * per:x * per + 3][::-1] for x in range(w))
    return save_png(path, w, h, bytes(out))


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
