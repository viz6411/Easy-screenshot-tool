"""Card #24 acceptance: the editor's layout must survive any window size.

What the owner reported (2026-11-16): "any change of the window size breaks everything, the
elements are placed crooked". This harness turns that into measurable invariants and walks a
matrix of window sizes, from the biggest to the floor the window itself enforces.

The picture is opened from a written file (not grabbed off the desktop): the desktop behind the
editor contains grey panels and window borders that look exactly like the editor's own control
surfaces, and measuring "is any control drawn outside the toolbar" against that background measures
the desktop instead of the app. A known picture with known colours removes that ambiguity.

Invariants (docs/WIN11-UI-GUIDE.md 20), all coordinates are client coordinates:
  I1  the pill is inside the client area - nothing of the toolbar is cut off or pushed out;
  I2  the status strip is below the toolbar, inside the client, with its separator and its text;
  I3  every control chip the picture shows lives inside the pill;
  I4  the reflow does not lose or duplicate a control: the same number of chips as at opening;
  I5  the chip that is drawn is the chip that reacts (hit-test follows the drawing);
  I6  the window refuses to shrink below a size the layout can still draw, and the layout is
      intact at that floor.

Input is posted to the editor window and sizes are set with SetWindowPos: the machine's mouse and
keyboard stay free (the owner works at this PC).
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import ui_probe as P
from PIL import Image

u = P.u
W = P.W
OUT = os.path.join(ROOT, 'test-output')
os.makedirs(OUT, exist_ok=True)
IMG = os.path.join(OUT, 'layout-source.png')
IMG_W, IMG_H = 1280, 860            # big enough that the layout has to clip it, not centre it
fails = []
proc = None
baseline = None                     # chip count measured at the size the app opens at

SIZES = [                           # outer window sizes: wide, mid, narrow, low, tall, extreme
    (2128, 1010), (2500, 1200), (1800, 1100), (1400, 1000), (1100, 900),
    (900, 820), (760, 780), (2400, 560), (700, 1300), (1200, 640),
]

# Control surfaces as painted (theme.h). ACCENT first: the active fill and the resting fill are
# 15 levels apart, so the order decides which one a pixel is allowed to be.
FILLS = [((0x3E, 0x3F, 0x56), 'active'), ((0x4C, 0x55, 0x61), 'pressed'),
         ((0x42, 0x4A, 0x56), 'hover'), ((0x36, 0x3D, 0x47), 'resting')]
FILL_TOL = 5
MIN_CHIP = 30                       # a control square is ctlH (32 px) scaled with DPI


def fill_class(c):
    for col, name in FILLS:
        if P.near(c, col, FILL_TOL):
            return name
    return None


def check(name, ok, detail=''):
    print('  %-54s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


def client(hw):
    r = W.RECT()
    u.GetClientRect(hw, P.ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top



def chip_boxes(fr, pill):
    """Control chips: a run of a control surface at least MIN_CHIP wide whose vertical extent is at
    least MIN_CHIP tall (the glyph drawn inside the chip is part of the chip, so the extent is
    measured as "not the pill surface", not "the same grey"). Captions and separators cannot start a
    box: they are text/lines, not a 30 px wide fill."""
    x0, y0, x1, y1 = pill['x0'] + 3, pill['y0'] + 3, pill['x1'] - 2, pill['y1'] - 2
    boxes = []
    y = y0
    while y < y1:
        for (s, e, ln) in fr.runs(y, lambda c: fill_class(c), x0, x1, MIN_CHIP):
            mid = (s + e) // 2
            top = y
            while top - 1 >= y0 and not P.near(fr.at(mid, top - 1), P.FLOATING, 7):
                top -= 1
            bot = y
            while bot + 1 < y1 and not P.near(fr.at(mid, bot + 1), P.FLOATING, 7):
                bot += 1
            h = bot - top + 1
            if h >= MIN_CHIP and h <= 1.8 * ln:
                boxes.append([s, e, top, bot])
        y += 2
    merged = []                     # the same chip is found on several scan rows
    for b in boxes:
        for m in merged:
            if not (b[1] < m[0] or b[0] > m[1] or b[3] < m[2] or b[2] > m[3]):
                m[0] = min(m[0], b[0]); m[1] = max(m[1], b[1])
                m[2] = min(m[2], b[2]); m[3] = max(m[3], b[3])
                break
        else:
            merged.append(b)
    return sorted(merged, key=lambda b: (b[2], b[0]))


def chip_fill(fr, box):
    """The fill a chip is painted with: the class covering most of its square."""
    votes = {}
    for y in range(box[2] + 4, box[3] - 3, 3):
        for x in range(box[0] + 4, box[1] - 3, 3):
            k = fill_class(fr.at(x, y))
            if k:
                votes[k] = votes.get(k, 0) + 1
    if not votes:
        return None
    return max(votes.items(), key=lambda kv: kv[1])[0]


def measure(tag, hw):
    """One size: return (frame, pill, chips) or None when the app drew nothing measurable."""
    fr, pill = P.grab_pill(tries=6)
    if not pill:
        check('%s: pill found' % tag, False, 'no pill surface at the top of the client')
        return None
    cw, ch = client(hw)
    cx0, cy0 = fr.client[0], fr.client[1]
    check('%s: pill inside the client (I1)' % tag,
          pill['x0'] >= cx0 and pill['y0'] >= cy0 and pill['x1'] <= cx0 + cw - 1
          and pill['y1'] <= cy0 + ch - 1,
          'pill x %d..%d y %d..%d in client %dx%d at %d,%d'
          % (pill['x0'] - cx0, pill['x1'] - cx0, pill['y0'] - cy0, pill['y1'] - cy0, cw, ch, cx0, cy0))

    sep, text = P.strip_geometry(fr, pill)
    ok_strip = sep is not None and sep > pill['y1'] and sep < cy0 + ch - 1 and bool(text)
    check('%s: status strip below the pill (I2)' % tag, ok_strip,
          'separator y %s (client y %s), pill bottom %d, strip text rows %d, client bottom %d'
          % (sep, None if sep is None else sep - cy0, pill['y1'] - cy0, len(text), ch))

    top_area = {'x0': cx0, 'y0': cy0, 'x1': cx0 + cw - 1, 'y1': cy0 + min(ch, 460) - 1}
    all_chips = chip_boxes(fr, top_area)
    outside = [b for b in all_chips if not (pill['x0'] <= b[0] and b[1] <= pill['x1']
                                            and pill['y0'] <= b[2] and b[3] <= pill['y1'])]
    chips = [b for b in all_chips if b not in outside]
    spill = [b for b in all_chips if b[0] < cx0 or b[1] > cx0 + cw - 1 or b[2] < cy0 or b[3] > cy0 + ch - 1]
    check('%s: every control inside the pill (I3)' % tag, not outside and not spill,
          '%d chip(s) outside the pill, %d outside the client, %d chips total'
          % (len(outside), len(spill), len(chips)))
    check('%s: controls found (I4)' % tag, len(chips) >= 6,
          '%d control chips: %s' % (len(chips), [chip_fill(fr, b) for b in chips]))
    return fr, pill, chips


def activate(fr, pill, chips):
    """I5: aim at a drawn chip and check that this exact chip is the one that changed.

    The target is the rightmost control chip, i.e. the line-width group: activating a tool would
    swap the property group and change the number of controls, which is what the next measurement
    compares.
    """
    targets = [b for b in chips if chip_fill(fr, b) != 'active']
    if not targets:
        return False, 'no idle chip to click', None
    b = targets[-1]
    cx, cy = (b[0] + b[1]) // 2, (b[2] + b[3]) // 2
    P.click(*fr.screen(cx, cy))
    time.sleep(0.4)
    fr2, pill2 = P.grab_pill(tries=6)
    if not pill2:
        return False, 'the pill vanished after the click', None
    hits = [q for q in chip_boxes(fr2, pill2) if q[0] - 4 <= cx <= q[1] + 4 and q[2] - 4 <= cy <= q[3] + 4]
    if len(hits) != 1:
        return False, 'the point %d,%d is covered by %d chip box(es)' % (cx, cy, len(hits)), fr2
    before, after = chip_fill(fr, b), chip_fill(fr2, hits[0])
    return after == 'active', 'chip fill %s -> %s at %d,%d' % (before, after, cx, cy), fr2


def write_source():
    """Known pixels: magenta/cyan blocks, colours the app never paints for a control."""
    im = Image.new('RGB', (IMG_W, IMG_H), (255, 0, 255))
    for y in range(0, IMG_H, 120):
        for x in range(0, IMG_W, 160):
            for yy in range(y, min(y + 60, IMG_H)):
                for xx in range(x, min(x + 80, IMG_W)):
                    im.putpixel((xx, yy), (0, 220, 255))
    im.save(IMG, 'PNG')
    return IMG


try:
    P.make_probe_dpi_aware()
    proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), write_source()])
    if not P.wait_window('ShotMvpEditor', tries=30):
        print('editor window never appeared'); sys.exit(1)
    time.sleep(1.8)
    hw = P.find_window('ShotMvpEditor')[0]

    r = W.RECT()
    u.GetWindowRect(hw, P.ctypes.byref(r))
    print('as opened: window %dx%d, client %dx%d' % (r.right - r.left, r.bottom - r.top, *client(hw)))
    res = measure('default', hw)
    if res:
        baseline = len(res[2])
        P.crop_up(res[0], res[1]['x0'], res[1]['y0'], res[1]['x1'] + 1, res[1]['y1'] + 1,
                  os.path.join(OUT, 'stage5-opened-pill.png'), 1)
    if not baseline:
        print('the app did not draw a measurable toolbar at its own opening size'); sys.exit(1)
    print('baseline chips at opening size: %d' % baseline)

    for (ww, wh) in SIZES:
        u.SetWindowPos(hw, 0, r.left, r.top, ww, wh, 0x0040)     # SWP_FRAMECHANGED
        time.sleep(0.7)
        u.GetWindowRect(hw, P.ctypes.byref(r))
        tag = '%dx%d' % (r.right - r.left, r.bottom - r.top)
        res = measure(tag, hw)
        if not res:
            continue
        fr, pill, chips = res
        check('%s: same controls as at opening (I4)' % tag, len(chips) == baseline,
              '%d chips now, %d at opening' % (len(chips), baseline))
        ok, detail, fr2 = activate(fr, pill, chips)
        check('%s: click activates the drawn chip (I5)' % tag, ok, detail)
        if tag in ('1100x900', '760x780') and fr2:
            P.crop_up(fr2, pill['x0'], pill['y0'], pill['x1'] + 1, pill['y1'] + 1,
                      os.path.join(OUT, 'stage5-pill-%s.png' % tag), 1)

    # I6: below the floor the window must refuse the size, and what remains must still be drawable.
    for (ww, wh) in [(380, 420), (300, 300)]:
        u.SetWindowPos(hw, 0, r.left, r.top, ww, wh, 0x0040)
        time.sleep(0.7)
        u.GetWindowRect(hw, P.ctypes.byref(r))
        got = (r.right - r.left, r.bottom - r.top)
        cw, ch = client(hw)
        refused = got[0] > ww or got[1] > wh
        print('   asked for %dx%d, Windows kept %dx%d (client %dx%d)' % (ww, wh, got[0], got[1], cw, ch))
        check('floor %dx%d is refused by the window (I6)' % (ww, wh), refused,
              'window kept %dx%d, client %dx%d' % (got[0], got[1], cw, ch))
        res = measure('floor %dx%d' % got, hw)
        if res:
            fr, pill, chips = res
            check('floor %dx%d keeps the same controls (I4/I6)' % got, len(chips) == baseline,
                  '%d chips now, %d at opening' % (len(chips), baseline))
            # The floor is the size the layout demands, not an arbitrary system minimum: at it the
            # toolbar uses nearly all the width it is given (WM_GETMINMAXINFO: pill + s24 + margins).
            check('floor %dx%d is the content floor (I6)' % got, pill['w'] >= client(hw)[0] - 120,
                  'pill %d px in client %d px' % (pill['w'], client(hw)[0]))

    print('\n%s' % ('LAYOUT RESIZE MATRIX PASSED' if not fails else 'FAILURES: %d - %s'
                    % (len(fails), fails[:6])))
finally:
    if proc:
        try:
            proc.kill(); proc.wait(5)
        except Exception:
            pass
    try:
        hw = P.find_window('ShotMvpEditor')
        if hw: u.PostMessageW(hw[0], 0x0010, 0, 0)
    except Exception:
        pass
sys.exit(1 if fails else 0)
