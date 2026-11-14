"""Acceptance shots for stage 0.2 (docs/TOOLBAR-SPEC.md §8.8).

Every stage2-*.png the spec asks for is taken here from the real editor on the real screen: the
whole window, magnified crops of the pill and its groups, the marker highlight, the three text
backdrops, the status strip and the two output actions. They are evidence for reading the result
by eye; they do not replace the manual checklist run (CONTRIBUTING §5).

Run on Windows, where the app can run:
    python tools/stage2_shots.py [out-dir]        default: build/stage2
    python tools/stage2_shots.py build/stage2 no-save-dir    also shoots the folder picker
"""
import ctypes, ctypes.wintypes as W, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
os.chdir(ROOT)

import ui_probe as P
from icon_sheet import chip_boxes as P_CHIP_BOXES

P.make_probe_dpi_aware()
u = P.u

EXE = os.path.join(ROOT, 'build', 'ShotMvp.exe')
INI = os.path.join(ROOT, 'build', 'settings.ini')
REGION = (520, 420, 1120, 700)
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'stage2')
PICK_MODE = 'no-save-dir' in sys.argv[2:]

VK_CONTROL, VK_RETURN, VK_ESCAPE = 0x11, 0x0D, 0x1B


def ctrl(vk):
    """Posted Ctrl+<key> to the editor window - ui_probe drives every control the same way, so the
    shot harness does not touch the machine's keyboard either."""
    P.ctrl_key(vk, settle=0.8)


WM_CHAR = 0x0102
VK_SHIFT = 0x10
u.VkKeyScanW.restype = ctypes.c_short


def type_text(s):
    P.type_text(s)



def window_bits(hwnd):
    """The window's own pixels - the shared PrintWindow reader in tests/ui_probe.py (window_rgb),
    kept under this name because the picker shot is assembled from window surfaces."""
    return P.window_rgb(hwnd)


def compose(base, x, y, part):
    """Writes another window's bits into the base frame (base = w, h, rgb) at screen position x,y."""
    bw, bh, brgb = base
    pw, ph, prgb = part[2], part[3], part[4]
    x0, x1 = max(0, x), min(bw, x + pw)
    y0, y1 = max(0, y), min(bh, y + ph)
    for dy in range(y0, y1):
        n = (x1 - x0) * 3
        if n <= 0:
            break
        src = ((dy - y) * pw + (x0 - x)) * 3
        dst = (dy * bw + x0) * 3
        brgb[dst:dst + n] = prgb[src:src + n]
    return base


def crop(fr, x0, y0, x1, y1, s=1):
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(fr.w - 1, x1), min(fr.h - 1, y1)
    w, h = (x1 - x0 + 1) * s, (y1 - y0 + 1) * s
    out = bytearray(w * h * 3)
    for r in range(h):
        src = ((y0 + r // s) * fr.w + x0) * 3
        row = fr.rgb[src:src + (x1 - x0 + 1) * 3]
        line = bytearray()
        for i in range(x1 - x0 + 1):
            line += row[i * 3:(i + 1) * 3] * s
        out[r * w * 3:(r + 1) * w * 3] = line
    return w, h, out


def save(name, part):
    w, h, rgb = part
    os.makedirs(OUT, exist_ok=True)
    P.save_png(os.path.join(OUT, name), w, h, rgb)
    print('%-28s %dx%d' % (name, w, h))


def chip_boxes(fr, pill):
    """Shared with tools/icon_sheet.py: control boxes along the chip line."""
    return P_CHIP_BOXES(fr, pill)


def pill_groups(fr, pill, boxes):
    """Splits the measured chips into (tools, the rest) using the colour swatches as the anchor.

    Index arithmetic over the whole run list breaks when a tool change leaves one chip
    unmeasurable; the swatch row sits between the tools and the property controls in every tool.
    The tail is the three property controls, then history, then output.
    """
    sw = [b for b in P.swatch_boxes(fr, pill) if b]
    right = max(b['x1'] for b in sw) if sw else pill['x1']
    return ([b for b in boxes if b[0] <= right + 20], [b for b in boxes if b[0] > right + 20])


def groups():
    """Current pill state: frame, pill, chips, chip top row, (tools, props+history+output)."""
    fr = P.capture_window('ShotMvpEditor')
    pill = P.find_pill(fr)
    boxes, top = chip_boxes(fr, pill)
    return fr, pill, boxes, top, pill_groups(fr, pill, boxes)


def strip_top(fr, pill):
    """The separator row of the status strip - measured by the harness helper that already
    knows what a separator is: a hairline with flat canvas on both sides of it."""
    sep, _text = P.strip_geometry(fr, pill)
    return sep or fr.h - 60


def bottom_crop(fr, pill, above=70):
    top = strip_top(fr, pill)
    return crop(fr, 0, top - above, fr.w - 1, fr.h - 1, 2)


def caption_left(fr, pill, x_chip):
    """Left edge of the group caption standing left of x_chip, or None when the group has none.

    Group captions are left-aligned to their group, so a chip-based crop slices the word in half.
    The caption band is the strip between the chip bottoms and the pill bottom; anything brighter
    than the pill there is ink.
    """
    best = None
    for y in range(pill['y1'] - 34, pill['y1'] - 4):
        for s, e, ln in fr.runs(y, lambda c: max(abs(a - b) for a, b in zip(c, P.CANVAS)) > 30,
                                max(0, x_chip - 240), x_chip, 6):
            best = s if best is None else min(best, s)
    return best


def stack(parts, gap=24, bg=b'\x18\x1b\x20'):
    """Crops stacked vertically into one PNG (one artifact file per spec line).

    Left-aligned on purpose: the history and output groups are far narrower than the tools group,
    and centring them buries a 120 px crop in a 1750 px band. bg is three bytes, so it is repeated
    once per pixel - multiplying it by a byte count writes three rows where one belongs.
    """
    w = max(p[0] for p in parts)
    h = sum(p[1] for p in parts) + gap * (len(parts) - 1)
    sheet = bytearray()
    for i, (pw, ph, rgb) in enumerate(parts):
        if i:
            sheet += bg * (w * gap)
        for y in range(ph):
            src = y * pw * 3
            sheet += rgb[src:src + pw * 3] + bg * (w - pw)
    assert len(sheet) == w * h * 3, 'stack built %d bytes for a %dx%d sheet' % (len(sheet), w, h)
    return w, h, sheet


def main():
    if not os.path.exists(EXE):
        print('build/ShotMvp.exe is missing - build first'); return 1
    os.makedirs(OUT, exist_ok=True)

    # The folder picker only appears when no folder has ever been chosen, i.e. when settings.ini
    # has no save_dir= line: shoot that state by temporarily rewriting the file.
    backup = None
    if PICK_MODE and os.path.exists(INI):
        backup = INI + '.bak'
        os.replace(INI, backup)
        with open(backup, 'r', encoding='utf-8-sig') as f:
            keep = [l for l in f if not l.lstrip().startswith('save_dir=')]
        with open(INI, 'w', encoding='utf-8') as f:
            f.write(''.join(keep))

    proc = subprocess.Popen([EXE, '-grab', '-rect', '%d,%d,%d,%d' % REGION])
    try:
        if not P.wait_window('ShotMvpEditor', tries=25):
            print('editor window never appeared'); return 1
        time.sleep(1.5)
        hwnd = P.find_window('ShotMvpEditor')[0]
        wr = W.RECT(); u.GetWindowRect(hwnd, ctypes.byref(wr))

        fr = P.capture_window('ShotMvpEditor')
        pill = P.find_pill(fr)
        for _ in range(5):                      # a freshly opened window can still be painting
            if pill:
                break
            time.sleep(1.0)
            fr = P.capture_window('ShotMvpEditor')
            pill = P.find_pill(fr)
        if not pill:
            print('pill not found'); return 1
        boxes, chip_top = chip_boxes(fr, pill)
        tools, tail = pill_groups(fr, pill, boxes)
        print('pill', pill)
        print('chips', len(boxes), 'top', chip_top, [b for b in boxes])
        print('groups: %d tool chips, then %d (properties, history, output)' % (len(tools), len(tail)))

        # One stroke and its undo: the canvas is clean again for the whole-window shot, while the
        # history controls show one enabled (redo) and one recessed (undo) - what 8.8 asks to see.
        P.drag(*fr.screen(760, 320), *fr.screen(1080, 360), steps=8)
        time.sleep(0.4)
        ctrl(ord('Z'))
        old_x0 = pill['x0']
        fr = P.capture_window('ShotMvpEditor') or fr
        pill = P.find_pill(fr) or pill
        dx = pill['x0'] - old_x0                # the chips were measured before the undo

        def span(group, margin=6):
            """A group box: the whole cells of one group, clamped to the pill.

            A cell is wider than its chip (the caption lives inside the cell), so the box reaches
            half a pitch past the outer chips - half a pitch is exactly the empty half of the
            outermost cells, and a caption can never stick out of it. The vertical box is the same
            for every group: chips and captions share one line band across the whole pill.
            """
            pitch = (group[-1][0] - group[0][0]) / (len(group) - 1) if len(group) > 1 else 56
            pad = int(pitch / 2)
            return (max(pill['x0'] - margin, group[0][0] - pad + dx), pill['y0'] - margin,
                    min(pill['x1'] + margin, group[-1][1] + pad + dx), pill['y1'] + margin)

        save('stage2-pill.png', (fr.w, fr.h, fr.rgb))
        save('stage2-pill-2x.png', crop(fr, pill['x0'] - 2, pill['y0'] - 2,
                                        pill['x1'] + 2, pill['y1'] + 30, 2))
        if len(tools) >= 6 and len(tail) >= 7:
            parts = [crop(fr, *span(tools), 2),
                     crop(fr, *span(tail[3:5]), 2),
                     crop(fr, *span(tail[5:7]), 2)]
            print('group crops: %s' % ' '.join('%dx%d' % (p[0], p[1]) for p in parts))
            save('stage2-groups.png', stack(parts))
        sep = strip_top(fr, pill)
        bbox = P.content_bbox(fr, pill, sep)
        if bbox:
            print('separator y=%d, image bottom y=%d, gap=%d px (8.6 wants 28)'
                  % (sep, bbox['y1'], sep - bbox['y1']))
        save('stage2-strip.png', bottom_crop(fr, pill))

        # Marker highlight: chosen from the pill rather than a key, and with the widest stroke so
        # the band reads in the shot.
        click_chip(fr, chip_top, tools[4])
        time.sleep(0.4)
        mr, mpill, _, mtop, (_, mtail) = groups()
        click_chip(mr, mtop, mtail[2])
        P.drag(*mr.screen(760, 700), *mr.screen(1250, 745), steps=10)
        time.sleep(0.4)
        save('stage2-marker.png', crop(P.capture_window('ShotMvpEditor'), 660, 640, 1400, 840, 2))

        # Text: click, type, Enter (card #6), with the backdrop chosen from the pill (card #7).
        draw_text('Без подложки', 0, 300)
        draw_text('Светлая подложка', 1, 360)
        draw_text('Синий на светлой', 1, 420, swatch=3)
        draw_text('Тёмная подложка', 2, 480)
        fr, pill, boxes, top, (t2, props) = groups()
        save('stage2-text-bg.png', crop(fr, 560, 250, 1500, 560, 2))
        cap = caption_left(fr, pill, props[0][0])
        left = props[0][0] - 10 if cap is None else min(props[0][0] - 10, cap - 10)
        save('stage2-text-bg-controls.png', crop(fr, left, top - 8, props[2][1] + 10,
                                                 pill['y1'] + 6, 2))


        if PICK_MODE:
            ctrl(ord('S'))
            time.sleep(3.0)                      # the shell dialog paints slowly, a shorter wait
            ed = P.find_window('ShotMvpEditor')  # catches it half-drawn
            dlg = P.find_window('#32770')        # the class of every standard shell dialog
            base = window_bits(ed[0]) if ed else None
            if base and dlg:
                part = window_bits(dlg[0])
                if part:
                    base = compose((base[2], base[3], base[4]), part[0] - ed[2], part[1] - ed[3], part)
            if base:
                save('stage2-save-first.png', base)
            P.press_key(VK_ESCAPE, settle=1.0)
            fr = P.capture_window('ShotMvpEditor')
            if fr:
                save('stage2-save-cancelled.png', (fr.w, fr.h, fr.rgb))
        else:
            ctrl(ord('S'))
            fr = P.capture_window('ShotMvpEditor')
            save('stage2-save-after.png', (fr.w, fr.h, fr.rgb))
            ctrl(ord('C'))
            fr = P.capture_window('ShotMvpEditor')
            save('stage2-copy.png', (fr.w, fr.h, fr.rgb))
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        if backup:
            if os.path.exists(INI):
                os.remove(INI)
            os.replace(backup, INI)
            print('settings.ini restored')


def click_chip(fr, top, box):
    """Clicks the middle of one measured chip."""
    P.click(*fr.screen((box[0] + box[1]) // 2, top + 24))


def draw_text(text, backdrop, img_y, swatch=None):
    """Places one text the way the checklist describes it: pick the backdrop (and optionally the
    ink colour) in the pill, click on the image, type, Enter."""
    fr, pill, _, top, (tools, _) = groups()
    click_chip(fr, top, tools[1])                        # the text tool
    time.sleep(0.4)
    fr, pill, _, top, (_, tail) = groups()
    if len(tail) >= 3:
        click_chip(fr, top, tail[backdrop])
    if swatch is not None:
        sw = P.swatch_boxes(fr, pill)
        if sw[swatch]:
            P.click(*fr.screen(sw[swatch]['cx'], sw[swatch]['cy']))
    time.sleep(0.3)
    fr = P.capture_window('ShotMvpEditor')
    P.click(*fr.screen(620, img_y))
    type_text(text)
    P.press_key(VK_RETURN, settle=0.8)


if __name__ == '__main__':
    sys.exit(main())
