"""Card #22 acceptance: unsaved edits are announced, and closing asks instead of deciding.

What the owner approved for this card:
  * "Сохранить" is inactive while the document is exactly the captured frame (no edits at all) -
    drawn as a hole and unreachable by Ctrl+S too;
  * the output group carries its own caption saying whether the edits are unsaved, and the pill
    does NOT resize when that state flips (a pill that changes width re-flows controls under the
    pointer);
  * closing with unsaved edits opens a dialog with three answers - «Сохранить», «Не сохранять»,
    «Отмена» - and each answer does what it says: saves and closes, closes without writing, or
    keeps the editor exactly as it was;
  * closing with nothing unsaved asks nothing.

Dialogs are driven through their own buttons (BM_CLICK, ui_probe.press_dialog_button); every mouse
and keystroke in this file is posted to the editor window, so the owner's machine keeps working.
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import ui_probe as P

u = P.u
OUT = os.path.join(ROOT, 'test-output')
DIR = os.path.join(OUT, 'unsaved')
INI = os.path.join(ROOT, 'build', 'settings.ini')
REGION = (700, 500, 1120, 700)
fails = []
proc = None
esc_saved = []


def check(name, ok, detail=''):
    print('  %-56s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


def ini_lines():
    if not os.path.exists(INI): return []
    return open(INI, 'rb').read().decode('utf-8-sig', errors='replace').splitlines()


def write_test_ini():
    """Saving in this harness must not open a folder picker: the file has to land where the harness
    can see it, and the real settings.ini is put back in `finally`."""
    gone = {'save_ask', 'save_dir'}
    kept = [l for l in ini_lines() if '=' in l and l.split('=', 1)[0].strip() not in gone]
    open(INI, 'wb').write(("\n".join(kept) + "\nsave_ask=0\nsave_dir=" + DIR + "\n").encode('utf-8'))


def start():
    global proc
    if proc:
        try:
            proc.kill(); proc.wait(5)
        except Exception:
            pass
    proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), '-grab',
                            '-rect', '%d,%d,%d,%d' % REGION])
    if not P.wait_window('ShotMvpEditor', tries=25):
        return None
    time.sleep(1.6)
    return P.find_window('ShotMvpEditor')[0]


def close_app():
    global proc
    if proc:
        try:
            proc.kill(); proc.wait(5)
        except Exception:
            pass
        proc = None


def new_stroke():
    """One arrow on the picture: the document is no longer the captured frame."""
    fr, pill = P.grab_pill()
    im = P.content_bbox(fr, pill, P.strip_geometry(fr, pill)[0])
    P.drag(*fr.screen(im['x0'] + 60, im['y0'] + 60), *fr.screen(im['x0'] + 320, im['y0'] + 200))
    time.sleep(0.5)
    return P.grab_pill()


def save_state(fr, pill):
    """(outline of «Сохранить», outline of «Копировать») - a hole has no hairline on its edges."""
    lay = P.pill_controls(fr, pill)
    out = lay['output']
    if len(out) < 2:
        return None, None, lay
    return P.outline_length(fr, pill, out[1]), P.outline_length(fr, pill, out[0]), lay


def caption_cols(fr, pill, lay):
    """The lit columns of the output group's own caption band: the columns of the pill that carry
    text ink right of the last group separator, below the chip band."""
    seps = lay['separators'] or [pill['x0']]
    x0 = seps[-1] + 6
    y0, y1 = pill['y0'] + P.CHIP_BOTTOM + 2, pill['y1'] - 2
    cols = []
    for x in range(x0, pill['x1'] - 1):
        if any(P.lum(fr.at(x, y)) > 110 for y in range(y0, y1)):
            cols.append(x)
    return cols


def dialog():
    w = P.find_window('#32770')
    if not w:
        return None, []
    return w[0], [p[2] for p in P.dialog_parts(w[0]) if p[1] == 'Button']


def dialog_gone(tries=20):
    for _ in range(tries):
        if not P.find_window('#32770'): return True
        time.sleep(0.3)
    return False


def fresh_files(since, prefix='screenshot_'):
    if not os.path.isdir(DIR): return []
    return [f for f in os.listdir(DIR)
            if f.startswith(prefix) and f.endswith('.png') and os.path.getmtime(os.path.join(DIR, f)) >= since]


try:
    os.makedirs(DIR, exist_ok=True)
    for f in os.listdir(DIR):
        os.remove(os.path.join(DIR, f))
    ini_backup = open(INI, 'rb').read() if os.path.exists(INI) else None
    write_test_ini()
    P.make_probe_dpi_aware()

    print('A. nothing edited: no Save, and closing asks nothing')
    hw = start()
    if not hw: print('editor window never appeared'); sys.exit(1)
    fr, pill = P.grab_pill()
    hole, live, lay = save_state(fr, pill)
    check('Сохранить is a hole while nothing has been edited', hole == 0 and live > 0,
          'Save outline %d px, Copy outline %d px' % (hole if hole is not None else -1, live or 0))
    clean_cols = caption_cols(fr, pill, lay)
    check('the output group carries a caption', len(clean_cols) > 20, '%d caption columns' % len(clean_cols))
    P.press_key(0x1B)                                     # Esc on a clean editor
    time.sleep(1.2)
    dlg_h, caps = dialog()
    check('Esc with nothing unsaved asks nothing', dlg_h is None, 'dialog %s' % (caps[:3],))
    check('and closes the editor', P.find_window('ShotMvpEditor') is None,
          'editor %s' % bool(P.find_window('ShotMvpEditor')))
    close_app()

    print('\nB. an edit makes Save live and moves the caption, without moving the pill')
    hw = start()
    fr0, pill0 = P.grab_pill()
    lay0 = P.pill_controls(fr0, pill0)
    cols_clean = caption_cols(fr0, pill0, lay0)
    fr1, pill1 = new_stroke()
    outline_save, outline_copy, lay1 = save_state(fr1, pill1)
    check('Сохранить became a live control after the stroke', (outline_save or 0) > 20,
          'Save outline %d px (was %d px)' % (outline_save or 0, hole or 0))
    cols_dirty = caption_cols(fr1, pill1, lay1)
    moved = len(set(cols_clean) ^ set(cols_dirty))
    check('the caption of the output group changed with the state', moved > 30,
          '%d caption columns differ (%d clean, %d dirty)' % (moved, len(cols_clean), len(cols_dirty)))
    check('the pill did not resize when the state flipped',
          pill0['w'] == pill1['w'] and pill0['h'] == pill1['h'],
          'pill %dx%d -> %dx%d' % (pill0['w'], pill0['h'], pill1['w'], pill1['h']))
    P.crop_up(fr1, lay1['separators'][-1] - 4, pill1['y0'], pill1['x1'] + 1, pill1['y1'] + 1,
              os.path.join(OUT, 'stage4-caption-dirty.png'), 2)

    print('\nC. «Отмена» keeps everything')
    P.press_key(0x1B)
    time.sleep(1.2)
    dlg_h, caps = dialog()
    names = ' '.join(caps).lower()
    check('Esc with unsaved edits asks, in the product\'s own words',
          dlg_h is not None and 'сохранить' in names and 'не сохранять' in names and 'отмена' in names,
          'buttons %s' % caps[:3])
    ok = P.press_dialog_button(dlg_h, ('отмена', 'cancel'))
    time.sleep(0.8)
    kept = dialog_gone() and bool(P.find_window('ShotMvpEditor'))
    fr2, pill2 = P.grab_pill()
    im2 = P.content_bbox(fr2, pill2, P.strip_geometry(fr2, pill2)[0])
    still = fr2.count((0xE4, 0x2A, 0x31), 30, im2['x0'], im2['y0'], im2['x1'] + 1, im2['y1'] + 1)
    check('«Отмена» closed the dialog and left the editor standing', ok and kept,
          'button %s, dialog closed and editor alive %s' % (ok, kept))
    check('the unfinished edit is still on the picture', still > 300, '%d px of the drawing colour' % still)
    check('«Отмена» wrote no file', not fresh_files(0), fresh_files(0)[:1])

    print('\nD. «Не сохранять» closes without writing')
    mark = time.time()
    P.press_key(0x1B)
    time.sleep(1.2)
    dlg_h, caps = dialog()
    ok = P.press_dialog_button(dlg_h, ('не сохранять',))
    time.sleep(1.0)
    gone = dialog_gone() and P.find_window('ShotMvpEditor') is None
    time.sleep(2.0)
    check('«Не сохранять» closed the dialog and the editor', ok and gone,
          'button %s, both gone %s' % (ok, gone))
    check('«Не сохранять» wrote no file', not fresh_files(mark), fresh_files(mark)[:1])
    close_app()

    print('\nE. «Сохранить» writes the file and closes')
    hw = start()
    new_stroke()
    mark = time.time()
    P.press_key(0x1B)
    time.sleep(1.2)
    dlg_h, caps = dialog()
    ok = P.press_dialog_button(dlg_h, ('сохранить',))
    time.sleep(1.5)
    gone = dialog_gone() and P.find_window('ShotMvpEditor') is None
    files = fresh_files(mark)
    for _ in range(12):
        if files: break
        time.sleep(0.5); files = fresh_files(mark)
    check('«Сохранить» closed the dialog and the editor', ok and gone,
          'button %s, both gone %s' % (ok, gone))
    check('«Сохранить» wrote the file it promised', bool(files), 'in %s: %s' % (DIR, files[:1]))
    if files:
        d = open(os.path.join(DIR, files[0]), 'rb').read()
        w, h = (0, 0)
        try:
            import struct
            w, h = struct.unpack('>II', d[16:24])
        except Exception:
            pass
        check('the saved file is the edited picture, not the raw capture',
              w == REGION[2] and h == REGION[3] and len(d) > 4096, 'PNG %dx%d, %d bytes' % (w, h, len(d)))
    close_app()

    print('\nF. Copying clears the "unsaved" sign but keeps Save live')
    hw = start()
    fr0, pill0 = P.grab_pill()
    cols_clean = caption_cols(fr0, pill0, P.pill_controls(fr0, pill0))
    fr1, pill1 = new_stroke()
    cols_dirty = caption_cols(fr1, pill1, P.pill_controls(fr1, pill1))
    P.ctrl_key(0x43)                                       # Ctrl+C
    time.sleep(1.0)
    fr2, pill2 = P.grab_pill()
    lay2 = P.pill_controls(fr2, pill2)
    cols_after = caption_cols(fr2, pill2, lay2)
    back = len(set(cols_clean) ^ set(cols_after))
    outline_save, _, _ = save_state(fr2, pill2)
    check('after Copy the caption is the clean one again', back <= 12 and abs(len(cols_after) - len(cols_clean)) <= 6,
          '%d caption columns differ from the clean frame' % back)
    check('Save is still live, the document is still edited', (outline_save or 0) > 20,
          'Save outline %d px' % (outline_save or 0))
    P.press_key(0x1B)
    time.sleep(1.0)
    check('and closing after a Copy asks nothing (nothing is unsaved)', P.find_window('#32770') is None,
          'dialog %s' % bool(P.find_window('#32770')))
    close_app()

    print('\n%s' % ('CARD #22 UNSAVED-EDITS ACCEPTANCE PASSED' if not fails
                    else 'FAILURES: %d - %s' % (len(fails), fails)))
finally:
    close_app()
    try:
        hw = P.find_window('ShotMvpEditor')
        if hw: u.PostMessageW(hw[0], 0x0100, 0, 0)      # WM_CLOSE, not WM_DESTROY
    except Exception:
        pass
    if ini_backup:
        open(INI, 'wb').write(ini_backup)
sys.exit(1 if fails else 0)
