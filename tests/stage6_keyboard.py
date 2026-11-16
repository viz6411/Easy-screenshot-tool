"""Card #26 acceptance (docs/WIN11-UI-GUIDE.md R16/R17): the pill is reachable by keyboard.

What is measured off the painted window:

  1  nothing is outlined before the keyboard asks for it - a mouse click never paints a focus ring;
  2  Tab moves one visible outline across the live controls, in the order they are painted, and wraps;
  3  the outline is a hollow contour the size of the control it names, and it clears 3:1 against
     both surfaces it sits between (WCAG 1.4.11, the bar R16 names);
  4  Shift+Tab reverses; Left/Right walk inside the group the outline already sits in and never
     leave it;
  5  a control drawn disabled (Undo with no history, Redo with nothing undone, «Сохранить» with no
     edit - the gate card #22 put there) never receives the outline;
  6  Enter and Space activate exactly the control the outline is on;
  7  an outline left on a control that has just become disabled is dropped, not left on a hole;
  8  the outline survives a window resize and the reflow that goes with it.

Everything is posted to the editor window: the machine's own mouse and keyboard stay free.

Run:  python tests/stage6_keyboard.py          exit code 0 = everything passed
"""
import ctypes, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import ui_probe as P

u = P.u
VK_TAB, VK_SHIFT, VK_RETURN, VK_SPACE = 0x09, 0x10, 0x0D, 0x20
VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN = 0x25, 0x27, 0x26, 0x28

REGION = (520, 420, 1120, 700)              # the captured region: 600x280 px of screen
OUT = os.path.join(ROOT, 'test-output')
DIR = os.path.join(OUT, 'keyboard')
INI = os.path.join(ROOT, 'build', 'settings.ini')
fails = []
proc = None

# theme.h: focusRing is white at 0.62 alpha. Over the pill surface (#212429) it lands near
# (171,172,174), over a control fill (#363D47) near (179,181,185), over the canvas recess (#1A1C20)
# near (168,169,170): a light neutral grey on all three. Nothing else in the pill is painted in that
# band - the accents are chromatic, the disabled ink is dark, the captions are #ABB3BE (19 levels
# between its red and its blue) and the full-strength glyph ink #ECEFF4 is brighter than the window.
LUM_BAND = (150, 218)
NEUTRAL = 14                                 # max(r,g,b) - min(r,g,b) of a neutral grey


def check(name, ok, detail=''):
    print('  %-56s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


# ------------------------------------------------------------------ settings / app ---------
def ini_backup():
    return open(INI, 'rb').read() if os.path.exists(INI) else None


def ini_set(values):
    gone = set(values)
    kept = [l for l in open(INI, 'rb').read().decode('utf-8', 'replace').splitlines()
            if '=' in l and l.split('=', 1)[0].strip() not in gone]
    open(INI, 'wb').write(('\n'.join(kept) + '\n'
                           + '\n'.join('%s=%s' % kv for kv in values.items()) + '\n').encode('utf-8'))


def start():
    global proc
    proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'),
                             '-grab', '-rect', '%d,%d,%d,%d' % REGION])
    time.sleep(2.2)


def stop():
    global proc
    hw = P.window_hwnd('ShotMvpEditor')
    if hw:
        u.PostMessageW(hw, 0x0010, 0, 0)                    # WM_CLOSE
        time.sleep(0.9)
    if proc:
        try:
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
        proc = None


# ------------------------------------------------------------------- measurements ----------
def focus_ink(c):
    return (max(c) - min(c)) <= NEUTRAL and LUM_BAND[0] <= P.lum(c) <= LUM_BAND[1]


def ctl_size(lay):
    """The side of a control square, from the chips that are squares: tools and properties."""
    ws = sorted(b[2] for b in lay['tools'] + lay['props'])
    return ws[len(ws) // 2] if ws else 0


def ink_clusters(fr, pill):
    """Every focus-coloured ink cluster in the pill as (x0, y0, w, h, ink_px, ink_inside_hole).

    Connected with a 3 px tolerance, which is what merges the ring's anti-aliased edges into one
    contour while keeping separate controls' ink apart (chips stand s2 = 8 px apart).
    """
    pts = set()
    for y in range(pill['y0'] + 2, pill['y1'] - 1):
        for x in range(pill['x0'] + 2, pill['x1'] - 1):
            if focus_ink(fr.at(x, y)):
                pts.add((x, y))
    seen, out = set(), []
    for seed in pts:
        if seed in seen:
            continue
        group, stack = [], [seed]
        seen.add(seed)
        while stack:
            px, py = stack.pop()
            group.append((px, py))
            for dy in range(-3, 4):
                for dx in range(-3, 4):
                    q = (px + dx, py + dy)
                    if q in pts and q not in seen:
                        seen.add(q)
                        stack.append(q)
        xs = [p[0] for p in group]
        ys = [p[1] for p in group]
        x0, y0, w, h = min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        inside = sum(1 for x, y in group
                     if x0 + 9 <= x <= x0 + w - 10 and y0 + 9 <= y <= y0 + h - 10)
        out.append((x0, y0, w, h, len(group), inside))
    return out


def outlines(fr, pill, n):
    """Clusters that are hollow contours the size of one control: the focus outline looks like that."""
    return [c for c in ink_clusters(fr, pill)
            if c[4] >= 200 and n - 8 <= c[2] <= n + 12 and n - 8 <= c[3] <= n + 12
            and c[5] <= c[4] * 0.12]


def masked_frame(fr):
    """The frame with the focus contour painted over, so measuring the controls is not confused by
    the outline itself: the ring is ink too, and left in the picture it widens the control box it
    belongs to (measured: a 57 px property chip reads 65 px wide, and its neighbour disappears).

    Only the focus-coloured pixels are hidden - the control fill, its hairline and its glyph are not
    that colour and stay where they are.
    """
    class Masked(P.Frame):
        def at(self, x, y):
            c = self.base.at(x, y)
            return P.FLOATING if focus_ink(c) else c

    m = Masked(fr.w, fr.h, fr.rgb, fr.x, fr.y)
    m.base = fr
    return m


def chip_list(fr, pill):
    """Every control of the pill in the order it is painted, each named `group:index`, positions
    measured from a frame in which the focus contour is masked out (see masked_frame)."""
    lay = P.pill_controls(masked_frame(fr), pill)
    n = ctl_size(lay)
    out = []
    for group in ('tools', 'swatches', 'props', 'history', 'output'):
        for i, b in enumerate(lay[group]):   # swatch boxes come as dicts, control boxes as tuples
            if b is None:
                continue
            cx = b['cx'] if group == 'swatches' else (b[0] + b[1]) // 2
            out.append({'name': '%s:%d' % (group, i), 'group': group, 'idx': i,
                        'cx': cx, 'box': b, 'n': n})
    out.sort(key=lambda c: c['cx'])
    return out, lay, n


def focused(fr, pill, chips, n):
    """Which control carries the outline: the single contour in the frame, matched by its centre."""
    found = outlines(fr, pill, n)
    if not found:
        return None, 'no focus contour in the pill'
    if len(found) > 1:
        return None, '%d contours at once: %s' % (len(found), [(c[0], c[1]) for c in found])
    x0, y0, w, h, ink, _ = found[0]
    cx, cy = x0 + w // 2, y0 + h // 2
    best = min(chips, key=lambda c: abs(c['cx'] - cx))
    if abs(best['cx'] - cx) > n * 0.6:
        return None, 'contour centre x %d is %d px from the nearest control x %d' % (
            cx, abs(best['cx'] - cx), best['cx'])
    return best, '%s contour %dx%d at (%d,%d), %d ink px' % (best['name'], w, h, cx, cy, ink)


def wcag(c1, c2):
    def lin(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    l1 = 0.2126 * lin(c1[0]) + 0.7152 * lin(c1[1]) + 0.0722 * lin(c1[2])
    l2 = 0.2126 * lin(c2[0]) + 0.7152 * lin(c2[1]) + 0.0722 * lin(c2[2])
    hi, lo = max(l1, l2), min(l1, l2)
    return (hi + 0.05) / (lo + 0.05)


def median(colours):
    if not colours:
        return None
    ch = [sorted(c[i] for c in colours)[len(colours) // 2] for i in (0, 1, 2)]
    return tuple(ch)


def ring_contrast(fr, bbox, n):
    """The outline's colour against the colours on both sides of it: WCAG 1.4.11 compares a focus
    indicator with what it is drawn on and with what surrounds it.

    Sampled along the left and right flanks only, and as a median: the top and bottom flanks would
    reach past the chip band, and a single pixel there is as likely to catch a glyph's
    anti-aliasing (which is light grey as well) as to catch the surface.
    """
    x0, y0, w, h = bbox
    x1, y1 = x0 + w - 1, y0 + h - 1
    ymid = range(y0 + h // 4, y1 - h // 4)
    ring, outer, inner = [], [], []
    for y in ymid:
        for band in range(1, 5):                 # the ring's own pixels, 1..4 px in from its edge
            for x in (x0 + band, x1 - band):
                c = fr.at(x, y)
                if focus_ink(c):
                    ring.append(c)
        for band in range(6, 11):                # just outside the outline: pill / recess surface
            for x in (x0 - band, x1 + band):
                c = fr.at(x, y)
                if not focus_ink(c):
                    outer.append(c)
        for band in range(6, 11):                # just inside it: the control's own surface
            for x in (x0 + band, x1 - band):
                c = fr.at(x, y)
                if not focus_ink(c):
                    inner.append(c)
    r, o, i = median(ring), median(outer), median(inner)
    if not (r and o and i) or len(ring) < 40:
        return None
    return min(wcag(r, o), wcag(r, i)), wcag(r, o), wcag(r, i), r, o, i


def draw_a_stroke(fr, pill):
    """An edit: what makes Undo live and «Сохранить» worth having (card #22 gates both)."""
    im = P.content_bbox(fr, pill, P.strip_geometry(fr, pill)[0])
    if not im:
        return False
    P.drag(*fr.screen(im['x0'] + 60, im['y0'] + 40), *fr.screen(im['x0'] + 240, im['y0'] + 110))
    time.sleep(0.6)
    return True


def press(vk, settle=0.4):
    P.press_key(vk, settle, P.window_hwnd('ShotMvpEditor'))


def tab(times=1, shift=False):
    hw = P.window_hwnd('ShotMvpEditor')
    if shift:
        u.PostMessageW(hw, P.WM_KEYDOWN, VK_SHIFT, P.key_lp(VK_SHIFT)); time.sleep(0.07)
    for _ in range(times):
        P.press_key(VK_TAB, 0.35, hw)
    if shift:
        u.PostMessageW(hw, P.WM_KEYUP, VK_SHIFT, P.key_lp(VK_SHIFT, True)); time.sleep(0.2)


def measure():
    """A fresh frame with its controls masked of the outline: (frame, pill, chips, layout, side)."""
    for _ in range(4):
        fr, pill = P.grab_pill(tries=4)
        if pill:
            chips, lay, n = chip_list(fr, pill)
            return fr, pill, chips, lay, n
        time.sleep(0.6)
    return None, None, [], {}, 0


def tab_until(name, tries=26):
    """Tab until the outline stands on one named control - the way a person keeps pressing it."""
    for _ in range(tries):
        tab()
        fr, pill, chips, lay, n = measure()
        if not pill:
            continue
        got, detail = focused(fr, pill, chips, n)
        if got and (got['name'] == name or got['group'] == name):
            return got, fr, pill, chips, n, detail
    return None, None, None, None, 0, 'never reached %s' % name


# --------------------------------------------------------------------------- run ------------
keep_ini = None
try:
    os.makedirs(DIR, exist_ok=True)
    keep_ini = ini_backup()
    ini_set({'save_ask': 0, 'save_dir': DIR, 'clipboard': 1,
             'hotkey': 'Ctrl+Shift+P', 'hotkey_window': 'Ctrl+Shift+W'})

    P.make_probe_dpi_aware()
    start()
    fr0, pill0, chips0, lay0, n0 = measure()
    if not pill0:
        print('the editor did not paint a pill'); sys.exit(1)

    counts = {g: len(lay0[g]) for g in ('tools', 'swatches', 'props', 'history', 'output')}
    check('every control of the pill is measurable for the keyboard walk',
          len(chips0) >= 19 and all(counts[g] > 0 for g in counts),
          '%d controls: tools %d, colours %d, properties %d, history %d, output %d, control %d px'
          % (len(chips0), counts['tools'], counts['swatches'], counts['props'],
             counts['history'], counts['output'], n0))

    # A ---------------------------------------------------------- nothing outlined by default
    got0, detail0 = focused(fr0, pill0, chips0, n0)
    check('nothing is outlined before the keyboard is used', got0 is None,
          'outline found on %s' % got0['name'] if got0 else detail0)

    P.click_chip(fr0, pill0, lay0['tools'][2])             # a real click on a tool
    time.sleep(0.5)
    fr1, pill1, chips1, lay1, n1 = measure()
    got1, detail1 = focused(fr1, pill1, chips1, n1)
    check('a mouse click does not paint a focus outline', got1 is None,
          'outline found on %s' % got1['name'] if got1 else detail1)
    # Park the pointer below the pill: the app draws the hovered control in its hover state, and a
    # pointer left on a chip would be read as that chip's state by the checks further down.
    P.move(*fr1.screen(pill1['x0'] + 6, pill1['y1'] + 30))

    # B --------------------------------------------------------- which controls are live at all
    live, blocked = [], []
    for c in chips0:
        hole = c['group'] in ('history', 'output') and P.outline_length(fr0, pill0, c['box']) == 0
        (blocked if hole else live).append(c)
    check('the disabled controls of a fresh editor are Undo, Redo and Save',
          [c['name'] for c in blocked] == ['history:0', 'history:1', 'output:1'],
          'blocked: %s' % ', '.join(c['name'] for c in blocked))

    # C --------------------------------------------------------- Tab walks them in paint order
    seen, contrasts, ok_walk = [], [], True
    for i in range(len(live)):
        tab()
        frk, pillk, chipsk, layk, nk = measure()
        gotk, detailk = focused(frk, pillk, chipsk, nk)
        if gotk is None:
            ok_walk = False
            seen.append('nothing(%s)' % detailk)
            continue
        seen.append(gotk['name'])
        found = outlines(frk, pillk, nk)
        rc = ring_contrast(frk, (found[0][0], found[0][1], found[0][2], found[0][3]), nk) if found else None
        if rc:
            contrasts.append((gotk['name'],) + rc)
    expected = [c['name'] for c in live]
    check('Tab walks every live control exactly once, in the order they are painted',
          ok_walk and seen == expected, '%s' % ' '.join(seen))

    worst = min(contrasts, key=lambda c: c[1]) if contrasts else None
    check('the outline clears 3:1 on both sides against what it sits on (WCAG 1.4.11)',
          len(contrasts) == len(live) and all(c[1] >= 3.0 for c in contrasts),
          '%d controls measured, worst: %s at %.2f:1 (outside %.2f:1, inside %.2f:1)'
          % (len(contrasts), worst[0], worst[1], worst[2], worst[3]) if worst else 'no outline measured')

    tab()                                                  # one more: the cycle has to close
    fr2, pill2, chips2, lay2, n2 = measure()
    got2, detail2 = focused(fr2, pill2, chips2, n2)
    check('Tab wraps back to the first live control',
          got2 is not None and got2['name'] == live[0]['name'],
          '%s' % (got2['name'] if got2 else detail2))

    visited_blocked = [name for name in seen if name in [c['name'] for c in blocked]]
    check('a control drawn disabled never receives the outline', not visited_blocked,
          'outline landed on: %s' % ', '.join(visited_blocked) if visited_blocked
          else '%s were never outlined' % ', '.join(c['name'] for c in blocked))

    # D --------------------------------------------------------- Shift+Tab reverses the walk
    # The focus sits on the first live control (the wrap check above), so Shift+Tab has to produce
    # the same order backwards, then wrap to the last live control.
    names, seen_rev, ok_rev = list(reversed(expected)), [], True
    for name in names:
        tab(1, shift=True)
        frs, pills, chipsS, lays, ns = measure()
        gots, details = focused(frs, pills, chipsS, ns)
        seen_rev.append(gots['name'] if gots else 'nothing(%s)' % details)
        if not gots or gots['name'] != name:
            ok_rev = False
            break
    check('Shift+Tab walks the whole pill backwards, one control at a time', ok_rev,
          '%d steps: %s' % (len(seen_rev), ' '.join(seen_rev)))

    tab(1, shift=True)                                     # backwards past the first control
    fr4, pill4, chips4, lay4, n4 = measure()
    got4, detail4 = focused(fr4, pill4, chips4, n4)
    check('Shift+Tab wraps from the first control to the last live one',
          got4 is not None and got4['name'] == expected[-1],
          '%s (expected %s)' % (got4['name'] if got4 else detail4, expected[-1]))

    # E --------------------------------------------------------- arrows walk inside a group
    gotT, frT, pillT, chipsT, nT, detailT = tab_until('tools:0')
    walked, detail_e, gotR = False, detailT, None
    if gotT:
        order = [c['name'] for c in live if c['group'] == 'tools']
        for step_to in (1, 2, 3):
            press(VK_RIGHT)
            frR, pillR, chipsR, layR, nR = measure()
            gotR, detail_e = focused(frR, pillR, chipsR, nR)
            if not gotR or gotR['name'] != order[step_to]:
                break
        walked = gotR is not None and gotR['name'] == order[3]
        for _ in range(4):                                 # past the end of the group
            press(VK_RIGHT)
        frR, pillR, chipsR, layR, nR = measure()
        gotR, detail_e = focused(frR, pillR, chipsR, nR)
        walked = walked and gotR is not None and gotR['name'] == order[-1]
        press(VK_LEFT)
        frR, pillR, chipsR, layR, nR = measure()
        gotL, detail_e = focused(frR, pillR, chipsR, nR)
        walked = walked and gotL is not None and gotL['name'] == order[-2]
    check('Left/Right walk inside the group the outline sits in and never leave it', walked,
          'tools walk: %s' % ' '.join(order) + ('; last seen %s' % (gotR['name'] if gotR else detail_e)))

    # F --------------------------------------------------------- Enter / Space activate it
    # tools:2 is the live tool (section A clicked it), so the Space test uses a different chip:
    # activating a tool must also hand the live state over from the one that had it.
    gotS, frS, pillS, chipsS, nS, detailS = tab_until('tools:0')
    if gotS is None:
        check('Space activates the tool the outline is on', False, detailS)
    else:
        states0 = {c['name']: P.dominant_state(masked_frame(frS), P.control_rect(pillS, c['box']),
                                                group='tool')
                   for c in chipsS if c['group'] == 'tools'}
        was = sorted(n for n, s in states0.items() if s == 'active')
        before = states0[gotS['name']]
        press(VK_SPACE, 0.6); time.sleep(0.4)
        frA, pillA, chipsA, layA, nA = measure()
        states1 = {c['name']: P.dominant_state(masked_frame(frA), P.control_rect(pillA, c['box']),
                                                group='tool')
                   for c in chipsA if c['group'] == 'tools'}
        after = states1.get(gotS['name'], 'missing')
        handed = all(states1.get(n) == 'normal' for n in was)
        check('Space activates the tool the outline is on (and the live tool hands over)',
              after == 'active' and before != 'active' and handed,
              '%s: %s -> %s, live tool %s -> %s'
              % (gotS['name'], before, after, was, [states1.get(n) for n in was]))

    draw_a_stroke(frA, pillA)                              # an edit makes Undo live
    gotB, frB, pillB, chipsB, nB, detailB = tab_until('history:0')
    if gotB is None:
        check('Undo becomes reachable by keyboard once an edit exists', False, detailB)
    else:
        check('Undo becomes reachable by keyboard once an edit exists', True,
              'the outline reached %s' % gotB['name'])
        press(VK_RETURN, 0.7); time.sleep(0.5)
        frC, pillC, chipsC, layC, nC = measure()
        gotC, detailC = focused(frC, pillC, chipsC, nC)
        undo_off = any(P.dominant_state(masked_frame(frC), P.control_rect(pillC, c['box']),
                                        group='history') == 'disabled'
                       for c in chipsC if c['group'] == 'history')
        check('Enter activates Undo: the edit is undone and Undo no longer holds the outline',
              gotC is None and undo_off, '%s / Undo drawn disabled: %s' % (detailC, undo_off))

    # G --------------------------------------------------------- keyboard-only Save
    files0 = set(os.listdir(DIR))
    draw_a_stroke(*measure()[:2])
    gotD, frD, pillD, chipsD, nD, detailD = tab_until('output:1')      # «Сохранить»
    if gotD is None:
        check('Save is reachable by keyboard once there is something to save', False, detailD)
    else:
        press(VK_RETURN, 0.9); time.sleep(1.4)
        new = sorted(set(os.listdir(DIR)) - files0)
        check('Enter on Save writes exactly one file', len(new) == 1, 'new files: %s' % new)

    # H --------------------------------------------------------- resize keeps the outline
    hw = P.window_hwnd('ShotMvpEditor')
    r = P.W.RECT(); u.GetWindowRect(hw, ctypes.byref(r))
    for size in ((1100, 900), (760, 780)):
        u.SetWindowPos(hw, 0, r.left, r.top, size[0], size[1], 0x0040)  # SWP_FRAMECHANGED
        time.sleep(0.9)
        frE, pillE, chipsE, layE, nE = measure()
        gotE, detailE = focused(frE, pillE, chipsE, nE)
        check('the outline survives the resize to %dx%d (the focus is not lost)' % size,
              gotE is not None, '%s' % detailE)

    stop()
finally:
    stop()
    if keep_ini:
        open(INI, 'wb').write(keep_ini)

print()
if fails:
    print('CARD #26 KEYBOARD-ACCESS ACCEPTANCE FAILED: %d' % len(fails))
    for f in fails:
        print('  - %s' % f)
    sys.exit(1)
print('CARD #26 KEYBOARD-ACCESS ACCEPTANCE PASSED')
