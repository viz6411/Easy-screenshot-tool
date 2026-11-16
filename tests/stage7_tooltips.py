"""Card #27 acceptance (docs/WIN11-UI-GUIDE.md R18): every control of the pill names itself.

What is measured, off the real windows the app creates:

  1  nothing is tip-painted before the user asks - no tooltip window exists at opening;
  2  the tip appears after the Fluent delay, not immediately;
  3  it carries the command name and, where the command has one, its hotkey;
  4  it does not cover the control it describes and stays inside the editor window;
  5  it disappears when the pointer leaves the control;
  6  a control drawn disabled (Undo/Redo with nothing to undo, «Сохранить» with nothing edited -
     card #22) gets no tip at all, and a tip dies together with the state it described;
  7  a control reached by Tab gets the same tip without any hover.

The tooltip is comctl32's own window (class tooltips_class32), owned by the editor window: it is
read through WM_GETTEXT and GetWindowRect, so every number below is the real window the user sees.
The machine's own mouse and keyboard are never touched - everything is posted to the editor.

Run:  python tests/stage7_tooltips.py          exit code 0 = everything passed
"""
import ctypes, os, subprocess, sys, time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import ui_probe as P

u = P.u
W = P.W
VK_TAB, VK_SHIFT = 0x09, 0x10
VK_ONE, VK_TWO = 0x31, 0x32
VK_S = 0x53
GW_OWNER, GA_PARENT = 4, 3
WM_GETTEXT = 0x000D

REGION = (520, 420, 1120, 700)
OUT = os.path.join(ROOT, 'test-output')
DIR = os.path.join(OUT, 'tooltips')
INI = os.path.join(ROOT, 'build', 'settings.ini')
EARLY = 0.15          # well inside the 400 ms delay: no tip may exist yet
LATE = 0.95           # past it: the tip must exist
fails = []
proc = None

EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)


def check(name, ok, detail=''):
    print('  %-58s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
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
def tips(hw):
    """The tooltip windows the editor owns that are actually showing text: (text, rect).

    comctl32 keeps a hidden control window next to the visible balloon, and the balloon is the
    window the user sees - so a tip is counted only when it is visible and has text.
    """
    out = []

    def cb(h, _):
        cls = ctypes.create_unicode_buffer(128)
        u.GetClassNameW(h, cls, 128)
        if cls.value != 'tooltips_class32':
            return True
        if not u.IsWindowVisible(h):
            return True
        if u.GetWindow(h, GW_OWNER) != hw and u.GetAncestor(h, GA_PARENT) != hw:
            return True
        buf = ctypes.create_unicode_buffer(512)
        u.SendMessageW(h, WM_GETTEXT, 511, ctypes.cast(ctypes.byref(buf), ctypes.c_void_p))
        r = W.RECT(); u.GetWindowRect(h, ctypes.byref(r))
        out.append((buf.value, (r.left, r.top, r.right, r.bottom)))
        return True

    u.EnumWindows(EnumProc(cb), 0)
    return out


def one_tip(hw):
    t = tips(hw)
    return t[0] if len(t) == 1 else None


def box_span(box):
    """(left, right) of a measured control: control boxes come as tuples, colour swatches as dicts."""
    return (box['x0'], box['x1']) if isinstance(box, dict) else (box[0], box[1])


def chip_screen(fr, pill, box):
    """The chip as screen coordinates, read the same way the harness aims a click at it."""
    x0, y0 = fr.screen(box_span(box)[0], pill['y0'] + P.CHIP_TOP)
    x1, y1 = fr.screen(box_span(box)[1], pill['y0'] + P.CHIP_BOTTOM)
    return (x0, y0, x1, y1)


def hover(fr, pill, box, wait=LATE):
    P.move(*fr.screen((box_span(box)[0] + box_span(box)[1]) // 2, pill['y0'] + 38), settle=0.05)
    time.sleep(wait)


def park(fr, pill):
    """Move the pointer off the pill, then off the picture: no control is hovered any more."""
    P.move(*fr.screen(pill['x0'] + 4, pill['y1'] + 70), settle=0.45)


def overlap(a, b):
    return not (a[2] <= b[0] or a[0] >= b[2] or a[3] <= b[1] or a[1] >= b[3])


def draw_a_stroke(fr, pill):
    im = P.content_bbox(fr, pill, P.strip_geometry(fr, pill)[0])
    if not im:
        return False
    P.drag(*fr.screen(im['x0'] + 60, im['y0'] + 40), *fr.screen(im['x0'] + 240, im['y0'] + 110))
    time.sleep(0.6)
    return True


def measure():
    for _ in range(4):
        fr, pill = P.grab_pill(tries=4)
        if pill:
            return fr, pill, P.pill_controls(fr, pill)
        time.sleep(0.6)
    return None, None, None


# --------------------------------------------------------------------------- run ------------
keep_ini = None
try:
    os.makedirs(DIR, exist_ok=True)
    keep_ini = ini_backup()
    ini_set({'save_ask': 0, 'save_dir': DIR, 'clipboard': 1,
             'hotkey': 'Ctrl+Shift+P', 'hotkey_window': 'Ctrl+Shift+W'})

    P.make_probe_dpi_aware()
    start()
    fr0, pill0, lay0 = measure()
    if not pill0:
        print('the editor did not paint a pill'); sys.exit(1)
    hw = P.window_hwnd('ShotMvpEditor')
    wr = W.RECT(); u.GetWindowRect(hw, ctypes.byref(wr))
    win = (wr.left, wr.top, wr.right, wr.bottom)

    # A ---------------------------------------------------------- nothing is tip-painted at rest
    time.sleep(0.6)
    check('no tooltip exists before anything is hovered', not tips(hw), '%s' % tips(hw))

    # B ---------------------------------------------------------- the delay comes first
    hover(fr0, pill0, lay0['tools'][1], wait=EARLY)
    check('the tooltip does not appear before the Fluent delay', not tips(hw), '%s' % tips(hw))
    time.sleep(LATE - EARLY)
    t = one_tip(hw)
    check('the tooltip appears once the pointer has rested on the control', t is not None,
          '%s' % (t,) if t else 'no tooltip window after %.2f s' % LATE)

    # C ---------------------------------------------------------- what it says
    check('the tooltip names the command and its hotkey', t is not None and t[0] == 'Текст (2)',
          'text %r' % (t[0] if t else ''))

    # D ---------------------------------------------------------- where it stands
    if t:
        chip = chip_screen(fr0, pill0, lay0['tools'][1])
        check('the tooltip does not cover the control it describes', not overlap(t[1], chip),
              'tip %s chip %s' % (t[1], chip))
        check('the tooltip stays inside the editor window',
              t[1][0] >= win[0] and t[1][2] <= win[2] and t[1][1] >= win[1] and t[1][3] <= win[3],
              'tip %s window %s' % (t[1], win))
    else:
        check('the tooltip does not cover the control it describes', False, 'no tooltip measured')
        check('the tooltip stays inside the editor window', False, 'no tooltip measured')

    park(fr0, pill0)
    check('the tooltip goes away when the pointer leaves the control', not tips(hw), '%s' % tips(hw))

    # E ---------------------------------------------------------- every tool names its key 1-6
    names = ['Стрелка (1)', 'Текст (2)', 'Прямоугольник (3)', 'Овал (4)', 'Маркер (5)', 'Кадрирование (6)']
    said, ok_tools = [], True
    for i, box in enumerate(lay0['tools']):
        hover(fr0, pill0, box)
        t = one_tip(hw)
        said.append(t[0] if t else 'nothing')
        if not t or t[0] != names[i]:
            ok_tools = False
        park(fr0, pill0)
    check('every tool control says its name and its key 1-6', ok_tools, '%s' % ' | '.join(said))

    # F ---------------------------------------------------------- colours, widths, backdrops
    hover(fr0, pill0, lay0['swatches'][4])
    t = one_tip(hw)
    check('a colour control says which colour it is', t is not None and t[0] == 'Цвет: жёлтый',
          'text %r' % (t[0] if t else ''))
    park(fr0, pill0)

    hover(fr0, pill0, lay0['props'][1])
    t = one_tip(hw)
    check('a line-width control says the width it stands for',
          t is not None and t[0] == 'Толщина: 4 px', 'text %r' % (t[0] if t else ''))
    park(fr0, pill0)

    P.press_key(VK_TWO, 0.4, hw)                              # the text tool carries backdrops
    time.sleep(0.5)
    fr1, pill1, lay1 = measure()
    hover(fr1, pill1, lay1['props'][1])
    t = one_tip(hw)
    check('with the text tool the same cells say what backdrop they mean',
          t is not None and t[0] == 'Фон: светлая подложка', 'text %r' % (t[0] if t else ''))
    park(fr1, pill1)
    P.press_key(VK_ONE, 0.4, hw)
    time.sleep(0.4)

    # G ---------------------------------------------------------- a disabled control gets no tip
    fr2, pill2, lay2 = measure()
    for label, box in (('Undo', lay2['history'][0]), ('Redo', lay2['history'][1]),
                       ('Save', lay2['output'][1])):
        hover(fr2, pill2, box)
        check('the disabled %s control gets no tooltip' % label, not tips(hw), '%s' % tips(hw))
        park(fr2, pill2)

    # H ---------------------------------------------------------- keyboard: the tip follows Tab
    P.press_key(VK_TAB, 0.3, hw)
    time.sleep(0.35)
    t = one_tip(hw)
    check('Tab gives the control the same tooltip, without any hover',
          t is not None and t[0] == 'Стрелка (1)', 'text %r' % (t[0] if t else ''))
    u.PostMessageW(hw, P.WM_KEYDOWN, VK_SHIFT, P.key_lp(VK_SHIFT)); time.sleep(0.07)
    P.press_key(VK_TAB, 0.3, hw)
    u.PostMessageW(hw, P.WM_KEYUP, VK_SHIFT, P.key_lp(VK_SHIFT, True))
    time.sleep(0.35)
    t2 = one_tip(hw)
    check('Shift+Tab gives it to the control Tab wrapped back to',
          t2 is not None and t2[0] == 'Копировать (Ctrl+C)', 'text %r' % (t2[0] if t2 else ''))

    # I ---------------------------------------------------------- a tip follows the state it describes
    files0 = set(os.listdir(DIR))
    fr3, pill3, lay3 = measure()
    draw_a_stroke(fr3, pill3)
    fr4, pill4, lay4 = measure()
    hover(fr4, pill4, lay4['output'][1])
    t = one_tip(hw)
    check('«Сохранить» gets its tip as soon as there is something to save',
          t is not None and t[0] == 'Сохранить (Ctrl+S)', 'text %r' % (t[0] if t else ''))
    P.ctrl_key(0x53, 0.8)                                     # Ctrl+S saves it away
    time.sleep(0.6)
    new = sorted(set(os.listdir(DIR)) - files0)
    check('and the save itself happened (the tip did not replace it)', len(new) == 1, 'new files: %s' % new)
    t = one_tip(hw)
    check('the tip stays while that control is still live (card #22: a saved document still '
          'differs from the captured frame, so Save is still a command)',
          t is not None and t[0] == 'Сохранить (Ctrl+S)', 'text %r' % (t[0] if t else ''))
    park(fr4, pill4)

    fr5, pill5, lay5 = measure()
    hover(fr5, pill5, lay5['history'][0])
    t = one_tip(hw)
    check('«Отменить» names itself and its key while there is something to undo',
          t is not None and t[0] == 'Отменить (Ctrl+Z)', 'text %r' % (t[0] if t else ''))
    P.ctrl_key(0x5A, 0.8)                                     # Ctrl+Z: the last edit goes back
    time.sleep(0.6)
    check('the tooltip dies together with the state it described', not tips(hw), '%s' % tips(hw))

    stop()
finally:
    stop()
    if keep_ini:
        open(INI, 'wb').write(keep_ini)

print()
if fails:
    print('CARD #27 TOOLTIP ACCEPTANCE FAILED: %d' % len(fails))
    for f in fails:
        print('  - %s' % f)
    sys.exit(1)
print('CARD #27 TOOLTIP ACCEPTANCE PASSED')
