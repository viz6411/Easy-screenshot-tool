"""Stage 0.2 acceptance of the pill and the output actions: drive the real editor with the real
mouse and check what it really does.

Run:  python stage1_states.py            exit code 0 = everything passed

  A layout   the pill carries all five control groups, the chips are one size, nothing spills out
  B groups   each group rests on its own surface (card #19) and each control says what it does
  C parity   clicking where a control is painted activates that control and nothing else
  D strip    the separator IS the top edge of the status strip, with the agreed gap above it
  E output   Save writes a file and Copy fills the clipboard, and neither of them closes the
             editor (card #8) - the confirmation appears in the strip instead

The app is started with -rect so the captured region is known in advance; everything after that
is real input events and pixels read back off the screen. Control positions come from what was
painted (ui_probe.pill_controls: separators split the pill into its five groups, and every group
is measured on its own terms), not from an assumed pitch - docs/TOOLBAR-SPEC.md 8.8.
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
    """The folder the app writes to, read from the same settings.ini the app reads. No save_dir=
    line means the user has never chosen one - that mode is stage2_behaviour.py's job."""
    ini = os.path.join(ROOT, 'build', 'settings.ini')
    if not os.path.exists(ini):
        return None
    text = open(ini, 'rb').read().decode('utf-8-sig', errors='replace')
    for line in text.splitlines():
        if line.strip().startswith('save_dir='):
            return line.split('=', 1)[1].strip()
    return None


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
known_png = lambda: set(f for f in os.listdir(SAVE_DIR) if f.lower().endswith('.png')) \
    if SAVE_DIR and os.path.isdir(SAVE_DIR) else set()
before = known_png()

proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), '-grab', '-rect', '%d,%d,%d,%d' % REGION])
ed = P.wait_window('ShotMvpEditor', tries=25)
if not ed:
    print('editor window never appeared'); proc.kill(); sys.exit(1)
hwnd, cls, wx, wy, ww, wh = ed
cx0, cy0 = P.client_origin(hwnd)
print('editor window %dx%d at (%d,%d), client at (%d,%d)' % (ww, wh, wx, wy, cx0, cy0))
time.sleep(1.4)
P.move(cx0 + 6, cy0 + 6)          # the pointer the previous run left on a control would be read as a hover
time.sleep(0.4)


def state_of(fr, pill, box, group):
    """Classified from the chip square only: the caption band below it carries text, not a surface."""
    if not box: return 'missing'
    return P.dominant_state(fr, P.control_rect(pill, box), group=group)


def fill_of(fr, pill, box):
    if not box: return 'missing'
    return P.dominant_fill(fr, P.control_rect(pill, box))


fr, pill = P.grab_pill()
for _ in range(5):                      # a freshly opened window can still be painting
    if pill:
        break
    time.sleep(1.0)
    fr, pill = P.grab_pill()
if not pill:
    print('pill not detected - nothing to measure'); proc.terminate(); sys.exit(1)

lay = P.pill_controls(fr, pill)
sep, text_rows = P.strip_geometry(fr, pill)
img = P.content_bbox(fr, pill, sep)
separators = lay['separators']
tools, props, history, output = lay['tools'], lay['props'], lay['history'], lay['output']
sw = [s for s in lay['swatches'] if s]
client = getattr(fr, 'client', (0, 0, fr.w, fr.h))
right_edge, bottom_edge = client[0] + client[2], client[1] + client[3]

print('pill   x %d..%d y %d..%d  (w=%d h=%d)' % (pill['x0'], pill['x1'], pill['y0'], pill['y1'],
                                                 pill['w'], pill['h']))
print('groups %d tools, %d swatches, %d properties, %d history, %d output, separated at x=%s'
      % (len(tools), len(sw), len(props), len(history), len(output), separators))
print('image  %s' % (img and '%dx%d at (%d,%d)' % (img['w'], img['h'], img['x0'], img['y0'])))
print('strip  separator y=%s, text rows %s' % (sep, text_rows[:1] + text_rows[-1:]))

print('\nA. layout of the pill')
check('the five groups are separated by hairlines', len(separators) == 4, 'separators at x=%s' % separators)
check('all five groups are on the pill',
      len(tools) == 6 and len(sw) == 7 and len(props) == 3 and len(history) == 2 and len(output) == 2,
      '%d/%d/%d/%d/%d' % (len(tools), len(sw), len(props), len(history), len(output)))
boxes = tools + props + history + output
check('every control is one size of chip', boxes and max(b[2] for b in boxes) - min(b[2] for b in boxes) <= 8,
      'widths %s' % [b[2] for b in boxes])
check('nothing spills out of the pill', boxes and boxes[-1][1] < pill['x1'] - 8,
      'last control ends at %d, pill inner edge %d' % (boxes and boxes[-1][1], pill['x1'] - 21))
if img:
    check('captured region is %dx%d physical px' % (REGION[2], REGION[3]),
          abs(img['w'] - REGION[2]) <= 14 and abs(img['h'] - REGION[3]) <= 14,
          'measured %dx%d' % (img['w'], img['h']))

print('\nB. what each control says it is doing')
st = [state_of(fr, pill, b, 'tool') for b in tools]
ws = [state_of(fr, pill, b, 'tool') for b in props]
his = [state_of(fr, pill, b, 'history') for b in history]
out = [state_of(fr, pill, b, 'output') for b in output]
print('   tools      :', st)
print('   properties :', ws)
print('   history    :', his)
print('   output     :', out)
check('the default tool is marked active', st[0] == 'active', 'slot 1 = %s' % st[0])
check('exactly one tool is active at start', st.count('active') == 1, str(st))
check('one line width is active', ws.count('active') == 1, str(ws))
check('undo/redo read as recessed at start', his == ['disabled', 'disabled'], str(his))
check('copy/save read as available', out == ['normal', 'normal'], str(out))
# A recessed control and a disabled control are both the canvas colour; what tells them apart is
# the outline around the niche (8.2), measured against the surface the outline sits on (8.8).
niche = [P.outline_length(fr, pill, b) for b in output]
holes = [P.outline_length(fr, pill, b) for b in history]
print('   outline hairline : output %s px, history %s px' % (niche, holes))
check('the output niches are drawn as niches, the disabled history controls are not',
      min(niche) >= 25 and max(holes) <= 8, 'output %s vs history %s' % (niche, holes))

print('\nC. clicking where a control is painted activates that control')
P.click_chip(fr, pill, tools[2])
time.sleep(0.7)
fr2, pill2 = P.grab_pill()
lay2 = P.pill_controls(fr2, pill2)
st2 = [state_of(fr2, pill2, b, 'tool') for b in lay2['tools']]
print('   tools after click:', st2)
check('the clicked control became active', st2[2] == 'active', 'rect slot = %s' % st2[2])
check('the control that was active released it', st2[0] == 'normal', 'arrow slot = %s' % st2[0])
check('no neighbour was activated', st2[1] == 'normal' and st2[3] == 'normal', str(st2[1:4]))
P.save_png(os.path.join(ROOT, 'build', 'stage1_active.png'), fr2.w, fr2.h, fr2.rgb)

mark = lay2['tools'][4]
# The posted move is one message in a queue the owner's own mouse also writes to, so a single move
# is not always the last thing the app saw when the frame is taken: re-post and re-measure.
for attempt in (1, 2, 3):
    P.move(*fr2.screen((mark[0] + mark[1]) // 2, pill2['y0'] + 38), settle=0.5 + 0.3 * attempt)
    fr3, pill3 = P.grab_pill()
    lay3 = P.pill_controls(fr3, pill3)
    st3 = [state_of(fr3, pill3, b, 'tool') for b in lay3['tools']]
    if st3[4] == 'hover':
        break
check('hovering a control lifts its fill', st3[4] == 'hover',
      'marker slot = %s after %d posted move%s' % (st3[4], attempt, 's' if attempt > 1 else ''))
check('hovering did not activate anything', st3.count('active') == 1, str(st3))
P.save_png(os.path.join(ROOT, 'build', 'stage1_hover.png'), fr3.w, fr3.h, fr3.rgb)
if img:                                 # park the pointer on the image: no control is hovered now
    P.move(*fr2.screen(img['x0'] + 60, img['y0'] + 60), settle=0.4)

print('\nD. the status strip')
check('the strip has its own separator row', sep is not None, 'separator row y=%s' % sep)
check('the strip shows text', bool(text_rows),
      'text rows %d..%d' % (min(text_rows), max(text_rows)) if text_rows else 'none')
if sep is not None and text_rows:
    check('strip text sits below the separator', min(text_rows) > sep,
          'separator %d, first text row %d' % (sep, min(text_rows)))
    # Card #9: the separator IS the top edge of the strip, not a line floating above it. From the
    # separator down to the bottom of the client area the strip band is FOOT_H tall (47 px at 175
    # %); while the line was drawn s8 above the strip the same measurement read 62 px.
    check('the separator is the top edge of the strip', 45 <= bottom_edge - sep <= 49,
          'client bottom %d, separator %d, strip band %d px' % (bottom_edge, sep, bottom_edge - sep))
if sep is not None and img:
    # The dark gap under the image is one constant (PAD_BOTTOM = s16 = 28 px). The capture's own
    # frame hairline straddles the last row of pixels, so a pixel-to-pixel reading is 1 px short.
    gap = sep - img['y1']
    check('a whole line of strip text fits between the image and the strip',
          gap >= 24, 'gap %d px (28 wanted; 6-7 px was the old value)' % gap)

print('\nE. draw, then Save and Copy without closing the editor')
if not img:
    check('captured image located', False, 'image block not found')
else:
    P.drag(*fr.screen(img['x0'] + 120, img['y0'] + 120), *fr.screen(img['x0'] + 520, img['y0'] + 420))
    time.sleep(0.6)
    fr4, pill4 = P.grab_pill()
    block = bytearray()
    for y in range(img['y0'] + 4, img['y1'] - 3):
        for x in range(img['x0'] + 4, img['x1'] - 3):
            block += bytes(fr4.at(x, y))
    drawn = count_red(block, 200)
    check('a drawn annotation appears on the captured image', drawn >= 200, '%d red pixels' % drawn)

    lay4 = P.pill_controls(fr4, pill4)
    his4 = [state_of(fr4, pill4, b, 'history') for b in lay4['history']]
    print('   history after a stroke:', his4)
    check('undo became a live control after the stroke', his4[0] == 'normal', str(his4))

    # Card #19 / checklist item 5: three groups read as three surfaces, not as three drawn lines.
    surfaces = {'tools': fill_of(fr4, pill4, lay4['tools'][1]),
                'history': fill_of(fr4, pill4, lay4['history'][0]),
                'output': fill_of(fr4, pill4, lay4['output'][0])}
    print('   group surfaces:', surfaces)
    check('tools, history and output rest on three different surfaces',
          len(set(surfaces.values())) == 3, str(surfaces))

    span0 = P.strip_text_span(fr4, P.strip_geometry(fr4, pill4)[0])
    print('   strip text before output: %s' % (span0 and 'x %d..%d' % (span0[0], span0[1])))

    # Copy first: no file, and the editor must stay open (checklist item 22).
    P.click_chip(fr4, pill4, lay4['output'][0])
    time.sleep(1.0)
    check('"Копировать" left the editor open', bool(P.find_window('ShotMvpEditor')),
          'save folder in settings.ini: %s' % SAVE_DIR)
    new_after_copy = sorted(known_png() - before)
    check('"Копировать" created no file in the save folder', not new_after_copy, '%s' % new_after_copy[:2])
    dib = P.clipboard_dib()
    check('"Копировать" put a bitmap of the captured size on the clipboard',
          dib is not None and (dib[0], dib[1]) == (REGION[2], REGION[3]),
          'clipboard %s' % ((dib[0], dib[1]) if dib else None,))
    if dib:
        n = count_red(dib[2], 300)
        check('the drawn annotation is baked into the clipboard bitmap', n >= 300,
              '%d red pixels in the clipboard bitmap' % n)
    fr5, pill5 = P.grab_pill()
    span = P.strip_text_span(fr5, P.strip_geometry(fr5, pill5)[0])
    check('"Копировать" confirmed itself in the status strip', span is not None and span[1] > right_edge - 60,
          'strip text x %s..%d, window inner edge %d' % (span and span[0], span and span[1], right_edge))

    # Save: the file appears while the editor keeps standing (checklist item 23, default mode).
    fr6, pill6 = P.grab_pill()
    lay6 = P.pill_controls(fr6, pill6)
    P.click_chip(fr6, pill6, lay6['output'][1])
    time.sleep(1.5)
    check('"Сохранить" left the editor open', bool(P.find_window('ShotMvpEditor')),
          'editor window after Save: present')
    new = sorted(known_png() - before)
    check('"Сохранить" produced a file', bool(new), 'in %s: %s' % (SAVE_DIR, new[:1]))
    if new:
        path = os.path.join(SAVE_DIR, new[0])
        dims = png_size(path)
        check('saved file matches the captured region', dims == (REGION[2], REGION[3]), 'PNG %s' % (dims,))
        full = os.path.abspath(path)
        if full.startswith(os.path.abspath(SAVE_DIR)) and full.lower().endswith('.png'):
            os.remove(full)
            print('   (removed the test artifact: %s)' % os.path.basename(full))
    fr7, pill7 = P.grab_pill()
    span = P.strip_text_span(fr7, P.strip_geometry(fr7, pill7)[0])
    check('"Сохранить" confirmed itself in the status strip', span is not None and span[1] > right_edge - 60,
          'strip text x %s..%d' % (span and span[0], span and span[1]))

if not P.find_window('ShotMvpEditor'):
    check('the editor is still open at the end of the run', False, 'the window was gone before Esc')
else:
    P.press_key(0x1B)
    time.sleep(1.0)
    proc.kill()

print('\n%s' % ('STAGE 0.2 PANEL ACCEPTANCE PASSED' if not fails else 'FAILURES: ' + ', '.join(fails)))
sys.exit(1 if fails else 0)
