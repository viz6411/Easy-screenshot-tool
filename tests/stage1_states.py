"""Stage-1 acceptance: drive the real editor with the real mouse and check what it really does.

Run:  python stage1_states.py            exit code 0 = everything passed

  A geometry   pill centred, controls one width on one pitch, swatches square, nothing outside
  B states     what is painted in each control matches what that control should be doing
  C parity     clicking where a control is painted activates that control and nothing else
  D strip      the status strip has its own separator row, text below it, clear of the image
  E end-to-end draw something, press Save, get a file of the right size with the strokes in it

The app is started with -rect so the captured region is known in advance; everything after that
is real input events and pixels read back off the screen.
"""
import os, struct, subprocess, sys, time

import ui_probe as P

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
P.make_probe_dpi_aware()

REGION = (520, 420, 1120, 700)
RED = (0xE4, 0x2A, 0x31)
fails = []


def check(name, ok, detail=''):
    print('  %-48s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


def save_dir_from_ini():
    ini = os.path.join(ROOT, 'build', 'settings.ini')
    if not os.path.exists(ini):
        return os.path.expanduser('~\\Desktop')
    text = open(ini, 'rb').read().decode('utf-8-sig', errors='replace')
    for line in text.splitlines():
        if line.strip().startswith('save_dir='):
            return line.split('=', 1)[1].strip()
    return os.path.expanduser('~\\Desktop')


def png_size(path):
    head = open(path, 'rb').read(33)
    if head[:8] != b'\x89PNG\r\n\x1a\n': return None
    return struct.unpack('>II', head[16:24])


def count_red(buf, need):
    """Pixels close to the annotation red. Byte order is unknown here (the clipboard hands over
    BGRA, the screen grab is RGB), so both orders are accepted."""
    n = 0
    r, g, b = RED
    for i in range(len(buf) - 2):
        x, y, z = buf[i], buf[i + 1], buf[i + 2]
        if (abs(x - r) <= 25 and abs(y - g) <= 25 and abs(z - b) <= 30) or \
           (abs(x - b) <= 30 and abs(y - g) <= 25 and abs(z - r) <= 25):
            n += 1
            if n >= need: return n
    return n


SAVE_DIR = save_dir_from_ini()
before = set(os.listdir(SAVE_DIR)) if os.path.isdir(SAVE_DIR) else set()

proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), '-grab', '-rect', '%d,%d,%d,%d' % REGION])
ed = P.wait_window('ShotMvpEditor', tries=25)
if not ed:
    print('editor window never appeared'); proc.kill(); sys.exit(1)
hwnd, cls, wx, wy, ww, wh = ed
cx0, cy0 = P.client_origin(hwnd)
print('editor window %dx%d at (%d,%d), client at (%d,%d)' % (ww, wh, wx, wy, cx0, cy0))
time.sleep(1.4)

fr = P.capture_window('ShotMvpEditor')
pill = P.find_pill(fr)
if not pill:
    print('pill not detected - nothing to measure'); proc.terminate(); sys.exit(1)
sep, text_rows = P.strip_geometry(fr, pill)
runs = P.control_runs(fr, pill)
sw = P.swatch_boxes(fr, pill)
img = P.content_bbox(fr, pill, sep)

print('pill   x %d..%d y %d..%d  (w=%d h=%d)' % (pill['x0'], pill['x1'], pill['y0'], pill['y1'], pill['w'], pill['h']))
print('fills  %d runs: %s' % (len(runs), [(r[0], r[1], r[2]) for r in runs]))
print('swatch %s' % [(s['x0'], s['x1']) if s else None for s in sw])
print('image  %s' % (img and '%dx%d at (%d,%d)' % (img['w'], img['h'], img['x0'], img['y0'])))
print('strip  separator y=%s, text rows %s' % (sep, text_rows[:1] + text_rows[-1:]))

# Slot positions come from what was painted, not from an assumed scale: control-fill runs give
# the tools, the line widths and the enabled actions; the swatch squares give the colour slots.
pitches = [runs[i + 1][0] - runs[i][0] for i in range(len(runs) - 1)] if len(runs) > 1 else []
unit = min([p for p in pitches if p < 75], default=0)
centres = [(r[0] + r[1]) // 2 for r in runs]
tool_c, width_c = centres[:6], centres[6:9]
copy_c, save_c = (centres[9], centres[10]) if len(centres) >= 11 else (0, 0)
undo_c, redo_c = copy_c - 2 * unit, copy_c - unit
sw_c = [s['cx'] for s in sw if s]
print('pitch %d px; %d tools, %d swatches, %d widths, %d action fills (undo/redo are painted '
      'recessed: there is nothing to undo yet)'
      % (unit, len(tool_c), len(sw_c), len(width_c), max(0, len(centres) - 9)))


def state_at(cx, frame):
    if not cx: return 'missing'
    return P.dominant_state(frame, (cx - 24, pill['y0'] + 16, cx + 24, pill['y1'] - 14))


print('\nA. geometry')
check('pill is centred in the window', abs(pill['x0'] - (fr.w - 1 - pill['x1'])) < 12,
      'left margin %d, right margin %d' % (pill['x0'], fr.w - 1 - pill['x1']))
check('controls share one width', runs and max(r[2] for r in runs) - min(r[2] for r in runs) <= 8,
      'widths %s' % [r[2] for r in runs])
adj = [p for p in pitches if p < 75]
check('neighbouring controls sit on one pitch', bool(adj) and max(adj) - min(adj) <= 5, 'pitches %s' % adj)
squares = [s for s in sw if s and abs(s['w'] - s['h']) <= 4]
check('colour swatches are square', len(squares) >= 6,
      '%d of 7 square: %s' % (len(squares), [s and (s['w'], s['h']) for s in sw]))
check('no control spills out of the pill', runs and runs[-1][1] < pill['x1'] - 8,
      'last control fill ends at %d, pill inner edge %d' % (runs[-1][1], pill['x1'] - 21))
if img:
    check('captured region is %dx%d physical px' % (REGION[2], REGION[3]),
          abs(img['w'] - REGION[2]) <= 14 and abs(img['h'] - REGION[3]) <= 14,
          'measured %dx%d' % (img['w'], img['h']))

print('\nB. what each control says it is doing')
st = [state_at(c, fr) for c in tool_c]
ws = [state_at(c, fr) for c in width_c]
acts = [state_at(c, fr) for c in (undo_c, redo_c, copy_c, save_c)]
print('   tools   :', st)
print('   widths  :', ws)
print('   actions :', acts)
check('the default tool is marked active', st[0] == 'active', 'slot 1 = %s' % st[0])
check('exactly one tool is active at start', st.count('active') == 1, str(st))
check('one line width is active', ws.count('active') == 1, str(ws))
check('undo/redo read as recessed at start', acts[0] == 'disabled' and acts[1] == 'disabled',
      'undo=%s redo=%s' % (acts[0], acts[1]))
check('copy/save read as enabled', acts[2] == 'normal' and acts[3] == 'normal', str(acts[2:]))

print('\nC. clicking where a control is painted activates that control')
P.click(*fr.screen(tool_c[2], pill['cy']))
time.sleep(0.7)
fr2 = P.capture_window('ShotMvpEditor')
st2 = [state_at(c, fr2) for c in tool_c]
print('   tools after click:', st2)
check('the clicked control became active', st2[2] == 'active', 'rect slot = %s' % st2[2])
check('the control that was active released it', st2[0] == 'normal', 'arrow slot = %s' % st2[0])
check('no neighbour was activated', st2[1] == 'normal' and st2[3] == 'normal', str(st2[1:4]))
P.save_png('stage1_active.png', fr2.w, fr2.h, fr2.rgb)

P.move(*fr.screen(tool_c[4], pill['cy']), settle=0.5)
fr3 = P.capture_window('ShotMvpEditor')
st3 = [state_at(c, fr3) for c in tool_c]
check('hovering a control lifts its fill', st3[4] == 'hover', 'marker slot = %s' % st3[4])
check('hovering did not activate anything', st3.count('active') == 1, str(st3))
P.save_png('stage1_hover.png', fr3.w, fr3.h, fr3.rgb)

print('\nD. status strip')
check('strip has its own separator row', sep is not None, 'separator row y=%s' % sep)
check('strip shows text', bool(text_rows),
      'text rows %d..%d' % (min(text_rows), max(text_rows)) if text_rows else 'none')
if sep is not None and text_rows:
    check('strip text sits below the separator', min(text_rows) > sep,
          'separator %d, first text row %d' % (sep, min(text_rows)))
if sep is not None and img:
    check('captured image does not touch the strip', sep - img['y1'] >= 6, 'gap %d px' % (sep - img['y1']))

print('\nE. draw, save, and what comes out')
if img:
    a = fr.screen(img['x0'] + 120, img['y0'] + 120)
    b = fr.screen(img['x0'] + 520, img['y0'] + 420)
    P.drag(a[0], a[1], b[0], b[1])
    time.sleep(0.6)
    fr4 = P.capture_window('ShotMvpEditor')
    if fr4:
        block = bytearray()
        for y in range(img['y0'] + 4, img['y1'] - 3):
            for x in range(img['x0'] + 4, img['x1'] - 3, 1):
                block += bytes(fr4.at(x, y))
        drawn = count_red(block, 200)
        check('a drawn annotation appears on the captured image', drawn >= 200, '%d red pixels' % drawn)
else:
    check('captured image located', False, 'image block not found')

if save_c:
    P.click(*fr.screen(save_c, pill['cy']))
    for _ in range(20):
        time.sleep(0.4)
        if not P.find_window('ShotMvpEditor'): break
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
    new = [f for f in (set(os.listdir(SAVE_DIR)) - before) if f.lower().endswith('.png')]
    check('Save produced a file', bool(new), 'in %s: %s' % (SAVE_DIR, new[:1]))
    if new:
        path = os.path.join(SAVE_DIR, new[0])
        dims = png_size(path)
        check('saved file matches the captured region', dims == (REGION[2], REGION[3]), 'PNG %s' % (dims,))
        dib = P.clipboard_dib()
        check('clipboard holds a bitmap of the same size',
              dib is not None and (dib[0], dib[1]) == (REGION[2], REGION[3]),
              'clipboard %s' % ((dib[0], dib[1]) if dib else None,))
        if dib:
            n = count_red(dib[2], 300)
            check('annotations are baked into the output', n >= 300, '%d red pixels in the clipboard bitmap' % n)
        full = os.path.abspath(path)
        if full.startswith(os.path.abspath(SAVE_DIR)) and full.lower().endswith('.png'):
            os.remove(full)
            print('   (removed the test artifact: %s)' % os.path.basename(full))
else:
    check('save control detected', False, 'no save slot found')
    P.press_key(0x1B)
    time.sleep(1.0)
    proc.kill()

print('\n%s' % ('STAGE 1 ACCEPTANCE PASSED' if not fails else 'FAILURES: ' + ', '.join(fails)))
sys.exit(1 if fails else 0)
