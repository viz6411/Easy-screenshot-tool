"""Behavioural acceptance of the stage 0.2 cards: what the editor does, not what it looks like.

Run:  python stage2_behaviour.py            exit code 0 = everything passed

  F text     placing text is one click and typing on the keyboard; Enter keeps it, Esc drops it  (#6)
  G ink      the colour of the text and the three backdrops, measured on the pixels of the text   (#7)
  H history  crop is a step of the undo history: Ctrl+Z brings the image size and the annotations
             back, Ctrl+Y repeats the crop, and two strokes undo and redo in the order they were made (#5)
  I output   Save asks for a folder when none has ever been chosen, remembers the choice, and asks
             for the place and the name in the second output mode                                 (#8)
  J labels   another click while a label is being typed finishes it and places a new one where the
             mouse went: two labels with one tool, nothing typed jumps to a new point            (#21)

Everything is real input events and pixels read back off the screen. Text ink is always measured as
the change against a frame taken before the text, so a busy screenshot cannot be mistaken for ink,
and the text is placed where the screenshot holds the least of both backdrop colours.
"""
import ctypes, os, subprocess, sys, time

import ui_probe as P

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
P.make_probe_dpi_aware()

REGION = (520, 420, 1120, 700)
INI = os.path.join(ROOT, 'build', 'settings.ini')
MAGENTA = (0xC8, 0x3C, 0xB4)       # palette swatch 2: a colour this desktop capture does not contain
RED = (0xE4, 0x2A, 0x31)
GREEN = (0x31, 0x9E, 0x14)
LIGHT_BG = (0xEC, 0xEF, 0xF4)      # theme textPrimary: the 'light' backdrop (8.5)
DARK_BG = P.CANVAS                 # theme canvas: the 'dark' backdrop (8.5)
CHOSEN = r'C:\Users\viz\Desktop\Tester02'      # the folder typed into the picker in section I
fails = []


def check(name, ok, detail=''):
    print('  %-54s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


def ini_lines():
    if not os.path.exists(INI): return []
    return open(INI, 'rb').read().decode('utf-8-sig', errors='replace').splitlines()


def ini_value(key):
    for line in ini_lines():
        if line.strip().startswith(key + '='):
            return line.split('=', 1)[1].strip()
    return None


def write_ini(values, drop=()):
    """settings.ini as the app would have written it. Dropping save_dir entirely is the mode
    card #8 starts in: a folder has never been chosen."""
    gone = set(values) | set(drop)
    kept = [l for l in ini_lines() if "=" in l and l.split("=", 1)[0].strip() not in gone]
    open(INI, "wb").write(("# ShotMvp settings\n" + "\n".join(kept) + "\n"
                           + "\n".join("%s=%s" % kv for kv in values.items()) + "\n").encode("utf-8"))


proc = None


def start():
    """Restart the editor on the same captured region and park the pointer off the pill: no control
    is hovered, and nothing the previous session left open can be mistaken for state."""
    global proc
    if proc:
        proc.kill(); proc.wait(5)
    proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), '-grab',
                            '-rect', '%d,%d,%d,%d' % REGION])
    if not P.wait_window('ShotMvpEditor', tries=25):
        print('editor window never appeared'); sys.exit(1)
    time.sleep(1.4)
    cx0, cy0 = P.client_origin(P.find_window('ShotMvpEditor')[0])
    P.move(cx0 + 6, cy0 + 6)
    time.sleep(0.4)


def grab_layout():
    fr, pill = P.grab_pill()
    return fr, pill, P.pill_controls(fr, pill)


def changed(a, b, rect):
    """Pixels of the rect that differ between two frames: the honest form of "something was drawn"."""
    w = rect[2] - rect[0]
    n = 0
    for y in range(rect[1], rect[3]):
        ia, ib = (y * a.w + rect[0]) * 3, (y * b.w + rect[0]) * 3
        n += sum(1 for x, z in zip(a.rgb[ia:ia + w * 3], b.rgb[ib:ib + w * 3]) if x != z) // 3
    return n


def ink(fr, rect, colour, tol=25):
    return fr.count(colour, tol, rect[0], rect[1], rect[2], rect[3])


def dialog_title(dlg):
    buf = ctypes.create_unicode_buffer(160)
    P.u.GetWindowTextW(dlg[0], buf, 160)
    return buf.value


def accept_dialog(dlg, captions):
    """Presses a dialog's own button, found by what it says on it. Typed characters do not reach
    these dialogs (the shell's own fields do not take injected keys), and text set into them
    programmatically is not what the dialog then hands back - so the dialogs are driven only by their
    buttons, and everything they decided is read back from the app afterwards."""
    return bool(dlg) and P.press_dialog_button(dlg[0], captions)


def fresh_png(folder, marker, prefix):
    """Files the app wrote into that folder after `marker` (a time.time() taken before the Save).
    The name the file got and the folder it went to are read back, never assumed."""
    if not folder or not os.path.isdir(folder):
        return []
    return sorted(f for f in os.listdir(folder) if f.lower().startswith(prefix.lower())
                  and f.lower().endswith('.png') and os.path.getmtime(os.path.join(folder, f)) >= marker)


def wait_files(folder, marker, prefix, count=1, seconds=16):
    """A PNG the app is still writing: the file lands a few seconds after the dialog closes, so
    waiting for it is the only honest way to say whether a Save wrote anything at all."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        found = fresh_png(folder, marker, prefix)
        if len(found) >= count:
            return found
        time.sleep(0.5)
    return fresh_png(folder, marker, prefix)


def wait_dialog(cls, seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        dlg = P.find_window(cls)
        if dlg:
            return dlg
        time.sleep(0.4)
    return None


def strip_right_ink(frg, sep):
    """Ink in the right end of the status band. The hotkey hint is left-aligned there and a
    confirmation of Copy/Save is right-aligned (8.7), so this separates a confirmation from the hint
    that stands there all the time."""
    cx0, cy0, cw, ch = frg.client
    n = 0
    for y in range((sep or 0) + 5, cy0 + ch - 2):
        for x in range(cx0 + cw - 450, cx0 + cw - 6):
            if P.lum(frg.at(x, y)) > 110:
                n += 1
    return n


def dialog_gone(tries=25):
    for _ in range(tries):
        if not P.find_window('#32770'): return True
        time.sleep(0.4)
    return False


start()
fr, pill = P.grab_pill()
for _ in range(5):                      # a freshly opened window can still be painting
    if pill: break
    time.sleep(1.0)
    fr, pill = P.grab_pill()
if not pill:
    print('pill not detected - nothing to measure'); proc.kill(); sys.exit(1)

img = P.content_bbox(fr, pill, P.strip_geometry(fr, pill)[0])
if not img:
    print('captured image not found'); proc.kill(); sys.exit(1)
print('image %dx%d at (%d,%d), pill x %d..%d' % (img['w'], img['h'], img['x0'], img['y0'],
                                                 pill['x0'], pill['x1']))


def img_pt(frg, x, y):
    """A point in image coordinates, as screen coordinates: this is where the mouse has to go."""
    return frg.screen(img['x0'] + x, img['y0'] + y)


def area(x0, y0, x1, y1):
    """A rectangle in image coordinates converted to the frame the pixels are measured in."""
    return (img['x0'] + x0, img['y0'] + y0, img['x0'] + x1, img['y0'] + y1)


def pick_anchor(y0, y1):
    """A point whose surroundings hold the least of both backdrop colours, so a backdrop drawn there
    cannot be mistaken for the picture underneath it."""
    best, best_score = None, 1 << 30
    for y in (y0, y0 + 18):
        for x in (70, 110, 150):
            r = area(x - 10, y - 10, x + 320, y + 46)
            score = ink(fr, r, LIGHT_BG, 6) + ink(fr, r, DARK_BG, 6)
            if score < best_score:
                best, best_score = (x, y), score
    return best, best_score


ANCHOR_F, busy_f = pick_anchor(60, 78)
ANCHOR_G, busy_g = pick_anchor(150, 168)
RECT_F = area(ANCHOR_F[0] - 10, ANCHOR_F[1] - 10, ANCHOR_F[0] + 330, ANCHOR_F[1] + 48)
RECT_G = area(ANCHOR_G[0] - 10, ANCHOR_G[1] - 10, ANCHOR_G[0] + 330, ANCHOR_G[1] + 48)
print('text anchors: F at image %s, G at image %s (backdrop-coloured px around them: %d / %d)'
      % (ANCHOR_F, ANCHOR_G, busy_f, busy_g))


def place_text(x, y, text, swatch=2, backdrop=None):
    """The way the checklist types text: tool, ink colour, backdrop, one click on the image, keys."""
    fr, pill, lay = grab_layout()
    P.click_chip(fr, pill, lay['tools'][1])                       # the text tool
    fr, pill = P.grab_pill()
    sw = P.swatch_boxes(fr, pill)
    if sw[swatch]:
        P.click(*fr.screen(sw[swatch]['cx'], sw[swatch]['cy']))
    if backdrop is not None:
        fr, pill, lay = grab_layout()                             # this group is the backdrops now
        P.click_chip(fr, pill, lay['props'][backdrop])
    fr, pill = P.grab_pill()
    P.click(*img_pt(fr, x, y))
    P.type_text(text)
    P.press_key(0x0D, settle=0.8)                                 # Enter
    return P.grab_pill()[0]


print('\nF. text is placed by one click and typed on the keyboard (card #6)')
base = fr
fr1, pill1, lay1 = grab_layout()
P.click_chip(fr1, pill1, lay1['tools'][1])
fr2, pill2 = P.grab_pill()
sw = P.swatch_boxes(fr2, pill2)
if sw[2]:
    P.click(*fr2.screen(sw[2]['cx'], sw[2]['cy']))                # the ink of every text below is magenta
time.sleep(0.4)
fr2, pill2 = P.grab_pill()
WHOLE_IMG = area(0, 0, img['w'], img['h'])
check('choosing the text tool draws nothing on the image', changed(base, fr2, WHOLE_IMG) < 400,
      '%d pixels changed' % changed(base, fr2, WHOLE_IMG))
lay2 = P.pill_controls(fr2, pill2)
P.click(*img_pt(fr2, *ANCHOR_F))
fr3, pill3 = P.grab_pill()
check('the click alone starts no annotation', changed(base, fr3, WHOLE_IMG) < 400,
      '%d pixels changed' % changed(base, fr3, WHOLE_IMG))
P.type_text('Текст кликом')
fr4, pill4 = P.grab_pill()
check('typing shows the text with the mouse button up the whole time',
      ink(fr4, RECT_F, MAGENTA) >= 120, '%d pixels of the swatch colour, %d pixels of any change'
      % (ink(fr4, RECT_F, MAGENTA), changed(base, fr4, RECT_F)))
P.press_key(0x0D, settle=0.8)
fr5, pill5 = P.grab_pill()
kept = ink(fr5, RECT_F, MAGENTA)
fr6, pill6, lay6 = grab_layout()
P.click_chip(fr6, pill6, lay6['tools'][2])                        # switch to another tool
time.sleep(0.6)
fr7, pill7 = P.grab_pill()
check('Enter keeps the text and it is still there after switching tools',
      kept >= 120 and ink(fr7, RECT_F, MAGENTA) >= 120, 'kept %d, after the switch %d'
      % (kept, ink(fr7, RECT_F, MAGENTA)))

RECT_ESC = area(ANCHOR_F[0] + 40, ANCHOR_F[1] + 200, ANCHOR_F[0] + 430, ANCHOR_F[1] + 258)
fr8, pill8, lay8 = grab_layout()
P.click_chip(fr8, pill8, lay8['tools'][1])
time.sleep(0.4)
fr9, pill9 = P.grab_pill()
P.click(*img_pt(fr9, ANCHOR_F[0] + 40, ANCHOR_F[1] + 210))
P.type_text('Незафиксированный')
fr10, pill10 = P.grab_pill()
check('a second unfinished text shows up on the image', ink(fr10, RECT_ESC, MAGENTA) >= 120,
      '%d pixels' % ink(fr10, RECT_ESC, MAGENTA))
P.press_key(0x1B, settle=0.8)                                     # Esc closes the text, not the editor
fr11, pill11 = P.grab_pill()
check('Esc removes the unfinished text', ink(fr11, RECT_ESC, MAGENTA) < 120,
      '%d pixels left' % ink(fr11, RECT_ESC, MAGENTA))
check('Esc leaves the committed text where it was',
      abs(ink(fr11, RECT_F, MAGENTA) - kept) < 60, '%d vs %d' % (ink(fr11, RECT_F, MAGENTA), kept))
check('Esc leaves the editor open', bool(P.find_window('ShotMvpEditor')))
P.ctrl_key(0x43)                                                  # Ctrl+C
dib = P.clipboard_dib()
dib_mag = 0
if dib:
    w, h, buf = dib
    for i in range(len(buf) - 2):
        x, y, z = buf[i], buf[i + 1], buf[i + 2]
        if (abs(x - MAGENTA[0]) <= 30 and abs(y - MAGENTA[1]) <= 30 and abs(z - MAGENTA[2]) <= 30) or \
           (abs(z - MAGENTA[0]) <= 30 and abs(y - MAGENTA[1]) <= 30 and abs(x - MAGENTA[2]) <= 30):
            dib_mag += 1
check('the committed text reaches the clipboard bitmap', dib_mag >= 120,
      '%d pixels of the text colour in the %s bitmap (its exact place is closed by eye, item 17)'
      % (dib_mag, (dib[0], dib[1]) if dib else 'missing'))

print('\nG. the colour of the text and the three backdrops (card #7)')
for mode, word in enumerate(('Без фона', 'Светлая подложка', 'Тёмная подложка')):
    fr_a = P.grab_pill()[0]
    fr_b = place_text(ANCHOR_G[0], ANCHOR_G[1], word, swatch=2, backdrop=mode)
    d_light = ink(fr_b, RECT_G, LIGHT_BG, 6) - ink(fr_a, RECT_G, LIGHT_BG, 6)
    d_dark = ink(fr_b, RECT_G, DARK_BG, 6) - ink(fr_a, RECT_G, DARK_BG, 6)
    on = ink(fr_b, RECT_G, MAGENTA, 20)
    if mode == 0:
        check('backdrop "none" paints no rectangle around the text',
              d_light < 60 and d_dark < 60 and on >= 120, 'light +%d, dark +%d, ink %d' % (d_light, d_dark, on))
    if mode == 1:
        check('backdrop "light" fills the box behind the text', d_light >= 800 and on >= 120,
              'light +%d, ink %d' % (d_light, on))
    if mode == 2:
        check('backdrop "dark" fills a box of the other colour', d_dark >= 800 and on >= 120,
              'dark +%d, ink %d' % (d_dark, on))
    P.ctrl_key(0x5A)                                              # Ctrl+Z: the next one lands on a clean base
    time.sleep(0.8)

fr_a = P.grab_pill()[0]
fr_b = place_text(ANCHOR_G[0], ANCHOR_G[1], 'Другим цветом', swatch=0, backdrop=0)
check('picking another swatch changes the colour of the text',
      ink(fr_b, RECT_G, RED, 25) >= 120 and ink(fr_b, RECT_G, MAGENTA, 12) < 120,
      'red %d, magenta %d' % (ink(fr_b, RECT_G, RED, 25), ink(fr_b, RECT_G, MAGENTA, 12)))
P.ctrl_key(0x5A)
time.sleep(0.8)

print('\nH. crop is a step of the undo history (card #5)')
fr_h, pill_h, lay_h = grab_layout()
sw = P.swatch_boxes(fr_h, pill_h)
P.click(*fr_h.screen(sw[0]['cx'], sw[0]['cy']))                   # red
time.sleep(0.4)
fr_h, pill_h, lay_h = grab_layout()
P.click_chip(fr_h, pill_h, lay_h['tools'][2])                     # the rectangle tool
time.sleep(0.4)
fr_h, pill_h = P.grab_pill()
P.drag(*img_pt(fr_h, 80, 300), *img_pt(fr_h, 430, 470))
fr_g, pill_g, lay_g = grab_layout()
sw = P.swatch_boxes(fr_g, pill_g)
P.click(*fr_g.screen(sw[1]['cx'], sw[1]['cy']))                   # green
time.sleep(0.4)
fr_g, pill_g, lay_g = grab_layout()
P.click_chip(fr_g, pill_g, lay_g['tools'][3])                     # the ellipse tool
time.sleep(0.4)
fr_g, pill_g = P.grab_pill()
P.drag(*img_pt(fr_g, 560, 300), *img_pt(fr_g, 900, 470))
RED_SPOT, GREEN_SPOT = area(70, 290, 445, 480), area(550, 290, 915, 480)
fr_s, pill_s = P.grab_pill()
n_red, n_green = ink(fr_s, RED_SPOT, RED), ink(fr_s, GREEN_SPOT, GREEN)
check('two annotations in two colours stand on the image', n_red >= 300 and n_green >= 300,
      'red %d, green %d' % (n_red, n_green))
P.ctrl_key(0x5A)
fr_u1, _ = P.grab_pill()
check('Ctrl+Z removed the last annotation only', ink(fr_u1, GREEN_SPOT, GREEN) < 300 and
      ink(fr_u1, RED_SPOT, RED) >= 300, 'green %d, red %d' % (ink(fr_u1, GREEN_SPOT, GREEN),
                                                              ink(fr_u1, RED_SPOT, RED)))
P.ctrl_key(0x5A)
fr_u2, _ = P.grab_pill()
check('a second Ctrl+Z removed the first annotation too', ink(fr_u2, RED_SPOT, RED) < 300,
      'red %d' % ink(fr_u2, RED_SPOT, RED))
P.ctrl_key(0x59)
fr_r1, _ = P.grab_pill()
check('Ctrl+Y repeated the first annotation, not the second',
      ink(fr_r1, RED_SPOT, RED) >= 300 and ink(fr_r1, GREEN_SPOT, GREEN) < 300,
      'red %d, green %d' % (ink(fr_r1, RED_SPOT, RED), ink(fr_r1, GREEN_SPOT, GREEN)))
P.ctrl_key(0x59)
fr_r2, _ = P.grab_pill()
check('a second Ctrl+Y repeated the second annotation', ink(fr_r2, GREEN_SPOT, GREEN) >= 300,
      'green %d' % ink(fr_r2, GREEN_SPOT, GREEN))

fr_c, pill_c, lay_c = grab_layout()
size0 = (img['x1'] - img['x0'] + 1, img['y1'] - img['y0'] + 1)
P.click_chip(fr_c, pill_c, lay_c['tools'][5])                     # the crop tool
time.sleep(0.5)
crop_from, crop_to = (60, 40), (980, 520)
P.drag(*img_pt(P.grab_pill()[0], *crop_from), *img_pt(P.grab_pill()[0], *crop_to))
time.sleep(0.8)
BAND = (0, pill['y1'] + 3, fr.w, P.strip_geometry(fr, pill)[0] - 3)
fr_crop, pill_crop = P.grab_pill()
cbox = P.content_bbox(fr_crop, pill_crop, P.strip_geometry(fr_crop, pill_crop)[0])
cut = cbox and (cbox['x1'] - cbox['x0'] + 1, cbox['y1'] - cbox['y0'] + 1)
check('crop cut the image down to the region drawn on it',
      cut and abs(cut[0] - (crop_to[0] - crop_from[0])) <= 20 and abs(cut[1] - (crop_to[1] - crop_from[1])) <= 20,
      'image %s -> %s' % (size0, cut))
P.ctrl_key(0x5A)
fr_cu, pill_cu = P.grab_pill()
cbox = P.content_bbox(fr_cu, pill_cu, P.strip_geometry(fr_cu, pill_cu)[0])
back = cbox and (cbox['x1'] - cbox['x0'] + 1, cbox['y1'] - cbox['y0'] + 1)
red_back, green_back = ink(fr_cu, BAND, RED), ink(fr_cu, BAND, GREEN)
check('Ctrl+Z undid the crop: the image is its original size again',
      back and abs(back[0] - size0[0]) <= 14 and abs(back[1] - size0[1]) <= 14, 'image %s' % (back,))
check('Ctrl+Z after a crop lost no annotation', back and red_back >= 300 and green_back >= 300,
      'red %d, green %d' % (red_back, green_back))
P.ctrl_key(0x59)
fr_cr, pill_cr = P.grab_pill()
cbox = P.content_bbox(fr_cr, pill_cr, P.strip_geometry(fr_cr, pill_cr)[0])
again = cbox and (cbox['x1'] - cbox['x0'] + 1, cbox['y1'] - cbox['y0'] + 1)
check('Ctrl+Y repeated the crop on the same region with the annotation inside it intact',
      again and cut and abs(again[0] - cut[0]) <= 8 and abs(again[1] - cut[1]) <= 8
      and ink(fr_cr, BAND, RED) >= 300 and ink(fr_cr, BAND, GREEN) >= 300,
      'image %s, red %d, green %d' % (again, ink(fr_cr, BAND, RED), ink(fr_cr, BAND, GREEN)))

print('\nI. Save asks for a folder once and then keeps it (card #8)')
original_ini = open(INI, 'rb').read()
created = []


def keep(files, folder):
    global created
    created += [os.path.join(folder, f) for f in files]
    return files


def edit_something():
    """Card #22 made "Сохранить" a live control only once the document differs from the captured
    frame (both by mouse and by Ctrl+S - Editor::do_action 3 refuses without an edit). Every case
    below saves, so each one first draws a stroke, exactly the way sections F-H do. The assertions
    themselves are unchanged."""
    fr, pill = P.grab_pill()
    im = P.content_bbox(fr, pill, P.strip_geometry(fr, pill)[0])
    P.drag(*fr.screen(im['x0'] + 40, im['y0'] + 40), *fr.screen(im['x0'] + 240, im['y0'] + 120))
    time.sleep(0.5)


try:
    os.makedirs(CHOSEN, exist_ok=True)
    common = {'clipboard': 1, 'hotkey': 'Ctrl+Shift+P', 'hotkey_window': 'Ctrl+Shift+W',
              'name_format': 'screenshot_%Y-%m-%d_%H-%M-%S'}

    write_ini(dict(common, save_ask=0, save_dir=CHOSEN))
    start()
    time.sleep(1.2)
    edit_something()
    mark = time.time()
    P.ctrl_key(0x53)
    time.sleep(1.5)
    asked = P.find_window('#32770')
    files = keep(wait_files(CHOSEN, mark, 'screenshot_'), CHOSEN)
    check('the default output mode saves without asking for anything', not asked and bool(files),
          'dialog %s, files %s' % (bool(asked), files[:1]))
    if asked:
        accept_dialog(asked, ('cancel', 'отмена', 'отменить'))
        dialog_gone()

    write_ini(dict(common, save_ask=0), drop=('save_dir',))          # no save_dir line at all
    start()
    time.sleep(1.2)
    edit_something()
    fr_no, pill_no = P.grab_pill()
    ink_before = strip_right_ink(fr_no, P.strip_geometry(fr_no, pill_no)[0])
    mark = time.time()
    P.ctrl_key(0x53)
    dlg = wait_dialog('#32770')
    check('with no folder ever chosen, Save opens the system folder picker', bool(dlg),
          'dialog %r' % (dialog_title(dlg) if dlg else None))
    if dlg:
        accepted_cancel = accept_dialog(dlg, ('cancel', 'отмена', 'отменить'))
        closed_cancel = dialog_gone()
        check('the picker was closed by its own button and the editor is still standing',
              accepted_cancel and closed_cancel and bool(P.find_window('ShotMvpEditor')),
              'button pressed %s, dialog closed %s' % (accepted_cancel, closed_cancel))
        fr_no2, pill_no2 = P.grab_pill()
        ink_after = strip_right_ink(fr_no2, P.strip_geometry(fr_no2, pill_no2)[0])
        check('cancelling the picker writes no file and announces it at the right end of the strip',
              ink_after >= 150 and ink_before < 150 and not fresh_png(CHOSEN, mark, 'screenshot_'),
              'ink at the right end of the strip %d -> %d, nothing written %s'
              % (ink_before, ink_after, not fresh_png(CHOSEN, mark, 'screenshot_')))

        P.ctrl_key(0x53)
        dlg2 = wait_dialog('#32770')
        mark = time.time()
        accepted = accept_dialog(dlg2, ('ok',))
        closed = dialog_gone()
        folder = ini_value('save_dir')
        files = keep(wait_files(folder, mark, 'screenshot_'), folder or '')
        check('the folder the picker accepted is where the file landed', accepted and closed and bool(files),
              'the app remembered save_dir=%s and the file is there: %s' % (folder, files[:1]))
        check('the chosen folder is remembered in the settings', bool(folder), 'save_dir=%s' % folder)

        mark = time.time()
        P.ctrl_key(0x53); time.sleep(1.5)
        P.ctrl_key(0x53); time.sleep(1.5)
        more = keep(wait_files(folder, mark, 'screenshot_', 2, 24), folder or '')
        check('the next two Saves ask nothing and write two more files into the same folder',
              not P.find_window('#32770') and len(more) >= 2, 'dialog %s, files %s'
              % (bool(P.find_window('#32770')), more[:2]))

    # The second output mode. The name the dialog is opened with comes from name_format, so the file
    # it writes can be recognised by that name without driving the shell's own fields.
    write_ini(dict(common, save_ask=1, save_dir=CHOSEN, name_format='tester02_%Y-%m-%d_%H-%M-%S'))
    start()
    time.sleep(1.2)
    edit_something()
    mark = time.time()
    P.ctrl_key(0x53)
    dlg3 = wait_dialog('#32770')
    buttons = [p[2] for p in (P.dialog_parts(dlg3[0]) if dlg3 else []) if p[1] == 'Button']
    check('the save_ask mode opens a window asking for the place and the name', bool(dlg3),
          'dialog %r, buttons %s' % (dialog_title(dlg3) if dlg3 else None, buttons[:4]))
    saved = accept_dialog(dlg3, ('save', 'сохранить'))
    closed_save = dialog_gone()
    files = keep(wait_files(CHOSEN, mark, 'tester02_'), CHOSEN)
    check('confirming that window writes the file it offered there', saved and closed_save and bool(files),
          'the file the dialog offered is in %s: %s' % (CHOSEN, files[:1]))

    mark = time.time()
    P.ctrl_key(0x53)
    asked_again = wait_dialog('#32770')
    check('every Save in that mode asks again instead of writing behind the user', bool(asked_again),
          'dialog %r' % (dialog_title(asked_again) if asked_again else None))
    if asked_again:
        accept_dialog(asked_again, ('cancel', 'отмена', 'отменить'))
        dialog_gone()
    extra = wait_files(CHOSEN, mark, 'tester02_', 1, 8)
    check('cancelling that window writes nothing new', not extra,
          'nothing new in %s since the cancel: %s' % (CHOSEN, extra[:1]))
    check('the editor is still open at the end of both output modes', bool(P.find_window('ShotMvpEditor')))
finally:
    if proc:
        proc.kill(); proc.wait(5); proc = None
    open(INI, 'wb').write(original_ini)
    for path in created:
        if os.path.exists(path):
            os.remove(path)
    for f in os.listdir(CHOSEN) if os.path.isdir(CHOSEN) else []:
        os.remove(os.path.join(CHOSEN, f))
    try:
        os.rmdir(CHOSEN)
    except OSError:
        print('   (left %s standing: it holds something this test did not write)' % CHOSEN)
    print('settings.ini restored and the files this test wrote are removed')

print('\nJ. another click while a label is being typed places a new label (card #21)')
start()
frA, pillA, layA = grab_layout()
P.click_chip(frA, pillA, layA['tools'][1])                       # the text tool, and it stays the tool
frA, pillA = P.grab_pill()
sw = P.swatch_boxes(frA, pillA)
if sw[2]:
    P.click(*frA.screen(sw[2]['cx'], sw[2]['cy']))              # magenta ink, as in section F
time.sleep(0.4)
P.click(*img_pt(frA, *ANCHOR_F))
P.type_text('Первая подпись')
frB, pillB = P.grab_pill()
first = ink(frB, RECT_F, MAGENTA)
check('the first label is on the image while it is being typed', first >= 120, '%d pixels' % first)

P.click(*img_pt(frB, *ANCHOR_G))                                # a second click, no Enter, no tool change
frC, pillC = P.grab_pill()
left = ink(frC, RECT_G, MAGENTA)
still = ink(frC, RECT_F, MAGENTA)
check('the second click left the first label exactly where it stood',
      still >= 120 and abs(still - first) < 60, '%d pixels where the first label was (%d before)' % (still, first))
check('the second click opened an empty label at the point that was clicked', left < 120,
      '%d pixels at the new point before anything is typed there' % left)

P.type_text('Вторая подпись')
frD, pillD = P.grab_pill()
check('two labels stand on the image at the same time, with one tool never left',
      ink(frD, RECT_G, MAGENTA) >= 120 and abs(ink(frD, RECT_F, MAGENTA) - first) < 60,
      'the new label %d pixels, the first one still %d' % (ink(frD, RECT_G, MAGENTA), ink(frD, RECT_F, MAGENTA)))
P.press_key(0x0D, settle=0.8)                                   # Enter commits the second one
P.ctrl_key(0x5A)                                                # Ctrl+Z: the last placed label goes back
frE, pillE = P.grab_pill()
check('Ctrl+Z took back the label placed last and left the first one standing',
      ink(frE, RECT_G, MAGENTA) < 120 and ink(frE, RECT_F, MAGENTA) >= 120,
      'second label %d pixels left, first label %d' % (ink(frE, RECT_G, MAGENTA), ink(frE, RECT_F, MAGENTA)))
if proc:
    proc.kill(); proc.wait(5); proc = None

print('\n%s' % ('STAGE 0.2 BEHAVIOUR ACCEPTANCE PASSED' if not fails else 'FAILURES: ' + ', '.join(fails)))
sys.exit(1 if fails else 0)
