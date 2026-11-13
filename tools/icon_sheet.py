"""Magnified sheet of the toolbar icons, taken from the running editor.

The point of this tool is legibility, not geometry: a 39-unit icon is far too small to judge
in a screenshot, so every control is cropped out of the real window and magnified. It is the
evidence used for the icon cards, and it is also how a wrong glyph direction gets caught.

Run:  python tools/icon_sheet.py [path/to/output.png]
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
os.chdir(ROOT)

import ui_probe as P

# Without this the probe sees a virtualized screen and the pill is not where the pixels are.
P.make_probe_dpi_aware()

EXE = os.path.join(ROOT, 'build', 'ShotMvp.exe')
REGION = (520, 420, 1120, 700)
SCALE = 6
GAP = 12
PER_ROW = 6


def crop(frame, x0, y0, x1, y1):
    w, h = x1 - x0 + 1, y1 - y0 + 1
    out = bytearray(w * h * 3)
    for row in range(h):
        src = ((y0 + row) * frame.w + x0) * 3
        out[row * w * 3:(row + 1) * w * 3] = frame.rgb[src:src + w * 3]
    return w, h, out


def magnify(w, h, rgb, s):
    ow, oh = w * s, h * s
    out = bytearray(ow * oh * 3)
    for row in range(oh):
        src_row = (row // s) * w * 3
        dst_row = row * ow * 3
        for col in range(ow):
            i = src_row + (col // s) * 3
            j = dst_row + col * 3
            out[j] = rgb[i]; out[j + 1] = rgb[i + 1]; out[j + 2] = rgb[i + 2]
    return ow, oh, out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'icon-sheet.png')
    if not os.path.exists(EXE):
        print('build/ShotMvp.exe is missing - build first'); return 1

    proc = subprocess.Popen([EXE, '-grab', '-rect', '%d,%d,%d,%d' % REGION])
    try:
        ed = P.wait_window('ShotMvpEditor', tries=25)
        if not ed:
            print('editor window never appeared'); return 1
        time.sleep(1.2)

        # Draw one stroke so undo and redo become enabled: a disabled control is painted as a
        # recess and its icon would not show up in the sheet.
        P.move(900, 800); P.drag(820, 720, 1240, 980, steps=8)
        time.sleep(0.4)

        fr = P.capture_window('ShotMvpEditor')
        pill = P.find_pill(fr)
        if not pill:
            print('pill not found'); return 1
        runs = P.control_runs(fr, pill)
        if not runs:
            print('no controls found in the pill'); return 1

        top, bottom = pill['y0'] + 6, pill['y1'] - 5
        tiles = [magnify(*(crop(fr, r[0], top, r[1], bottom)), SCALE) for r in runs]

        cw = max(t[0] for t in tiles)
        ch = max(t[1] for t in tiles)
        rows = (len(tiles) + PER_ROW - 1) // PER_ROW
        W = PER_ROW * cw + (PER_ROW + 1) * GAP
        H = rows * ch + (rows + 1) * GAP
        sheet = bytearray([0x18, 0x1B, 0x20]) * (W * H * 3)
        for n, (tw, th, rgb) in enumerate(tiles):
            row, col = n // PER_ROW, n % PER_ROW
            ox = GAP + col * (cw + GAP)
            oy = GAP + row * (ch + GAP)
            for y in range(th):
                dst = (oy + y) * W * 3 + ox * 3
                src = y * tw * 3
                sheet[dst:dst + tw * 3] = rgb[src:src + tw * 3]

        out_dir = os.path.dirname(os.path.abspath(out_path))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        P.save_png(out_path, W, H, sheet)
        print('sheet: %s (%dx%d) from %d controls' % (out_path, W, H, len(tiles)))
        print('order: %s' % ', '.join('x%d w%d' % (r[0], r[2]) for r in runs))
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == '__main__':
    sys.exit(main())
