"""Acceptance of cards #16 (open an existing file) and #17 (a saved PNG is a compressed PNG).

Run:  python stage3_open_png.py            exit code 0 = everything passed

  A open    the file named on the command line is the picture the editor shows: a PNG written by
            PIL at a known size with known colours appears in the window at that size and with
            those colours, measured as pixels of the window rather than as a number the app claims.
  B output  what the editor saves out of that file is the same picture: the same dimensions and
            every pixel equal to the one that went in. The comparison decodes with PIL, a codec the
            product does not know about, so a channel swapped on the way out is caught.
  C weight  a capture of the whole physical screen saves as a file clearly lighter than the raw
            pixels it is made of. Card #17: such a frame used to weigh about 12 MB because the
            writer stored the pixels instead of compressing them.

Input is posted to the app's window only (ui_probe), so the machine this runs on keeps working.
"""
import os, subprocess, sys, time

import ui_probe as P
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
P.make_probe_dpi_aware()

EXE = os.path.join(ROOT, 'build', 'ShotMvp.exe')
INI = os.path.join(ROOT, 'build', 'settings.ini')
DIR = os.path.join(ROOT, 'build', 'openpng')
SRC = os.path.join(DIR, 'source.png')
W_PX, H_PX = 400, 300                     # the file opened in section A
RED, BLUE, GREEN = (255, 0, 0), (0, 0, 255), (0, 255, 0)
PATCH = (60, 40)                          # each colour block in that file
fails = []
proc = None


def check(name, ok, detail=''):
    print('  %-54s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


def write_source_png():
    """Known pixels: three solid blocks on a background none of them belongs to."""
    im = Image.new('RGB', (W_PX, H_PX), (12, 30, 60))
    for colour, x, y in ((RED, 0, 0), (BLUE, 200, 0), (GREEN, 0, 120)):
        for yy in range(y, y + PATCH[1]):
            for xx in range(x, x + PATCH[0]):
                im.putpixel((xx, yy), colour)
    im.save(SRC, 'PNG')
    return im


def ini_backup():
    return open(INI, 'rb').read() if os.path.exists(INI) else None


def ini_set_save_dir(folder):
    """settings.ini pointing at the test folder, save_ask off: the Save button then writes a file
    without a dialog the harness would have to drive."""
    keep = [l for l in (ini_backup() or b'').decode('utf-8-sig').splitlines()
            if l.split('=')[0].strip() not in ('save_dir', 'save_ask')]
    open(INI, 'wb').write(('\n'.join(keep) + '\nsave_dir=%s\nsave_ask=0\n' % folder).encode('utf-8'))


def start(args):
    global proc
    stop()
    proc = subprocess.Popen([EXE] + args)
    if not P.wait_window('ShotMvpEditor', tries=25):
        print('editor window never appeared'); sys.exit(1)
    time.sleep(1.4)


def stop():
    global proc
    if proc:
        proc.kill(); proc.wait(5); proc = None


def saved_since(marker, folder=DIR):
    got = [f for f in os.listdir(folder)
           if f.lower().endswith('.png') and os.path.getmtime(os.path.join(folder, f)) >= marker]
    if not got:
        for _ in range(20):
            time.sleep(0.5)
            got = [f for f in os.listdir(folder)
                   if f.lower().endswith('.png') and os.path.getmtime(os.path.join(folder, f)) >= marker]
            if got: break
    return os.path.join(folder, got[0]) if got else None


def layout():
    for _ in range(5):
        fr, pill = P.grab_pill()
        if pill:
            return fr, pill
        time.sleep(1.0)
    return None, None


# ------------------------------------------------------------------ setup -----------------
os.makedirs(DIR, exist_ok=True)
for f in os.listdir(DIR):
    os.remove(os.path.join(DIR, f))
backup = ini_backup()
try:
    ini_set_save_dir(DIR)

    # A ---------------------------------------------------------------- open the file we wrote
    source = write_source_png()
    start([SRC])
    fr, pill = layout()
    if not pill:
        print('the editor window did not paint a pill - nothing to measure'); sys.exit(1)
    img = P.content_bbox(fr, pill, P.strip_geometry(fr, pill)[0])
    if not img:
        print('the opened picture was not found in the window'); stop(); sys.exit(1)
    print('opened file %dx%d, editor shows %dx%d at (%d,%d)'
          % (W_PX, H_PX, img['w'], img['h'], img['x0'], img['y0']))
    check('the opened file is shown at its own size',
          abs(img['w'] - W_PX) <= 4 and abs(img['h'] - H_PX) <= 4,
          'window measures %dx%d for a %dx%d file' % (img['w'], img['h'], W_PX, H_PX))

    def block(colour, x, y):
        return fr.count(colour, 8, img['x0'] + x, img['y0'] + y,
                        img['x0'] + x + PATCH[0], img['y0'] + y + PATCH[1])

    red, blue, green = block(RED, 0, 0), block(BLUE, 200, 0), block(GREEN, 0, 120)
    check('the opened file keeps the colours it was written with',
          red >= 1500 and blue >= 1500 and green >= 1500,
          'red %d, blue %d, green %d pixels of their %d px blocks'
          % (red, blue, green, PATCH[0] * PATCH[1]))
    check('the picture is not painted with its channels swapped',
          block(BLUE, 0, 0) < 200 and block(RED, 200, 0) < 200,
          'blue in the red block: %d, red in the blue block: %d'
          % (block(BLUE, 0, 0), block(RED, 200, 0)))

    # B ---------------------------------------------------------------- save it back out
    marker = time.time()
    P.ctrl_key(0x53)                                            # Ctrl+S
    out = saved_since(marker)
    if not out:
        print('Ctrl+S produced no file in %s' % DIR); stop(); sys.exit(1)
    saved = Image.open(out).convert('RGB')
    check('the saved file has the dimensions of the opened one', saved.size == (W_PX, H_PX),
          '%s -> %s %s' % (os.path.basename(out), saved.size[0], saved.size[1]))
    same = list(source.getdata()) == list(saved.getdata())
    diff = sum(1 for a, b in zip(source.getdata(), saved.getdata()) if a != b)
    check('every pixel survives open-and-save', same, '%d of %d pixels differ' % (diff, W_PX * H_PX))
    stop()

    # C ---------------------------------------------------------------- a whole screen out
    sw, sh = P.physical_resolution()
    start(['-grab', '-rect', '0,0,%d,%d' % (sw, sh)])
    layout()                                                    # wait until the window has painted
    marker = time.time()
    P.ctrl_key(0x53)
    out = saved_since(marker)
    if not out:
        print('the full-screen capture produced no file'); stop(); sys.exit(1)
    size = os.path.getsize(out)
    raw = sw * sh * 4
    check('a %dx%d capture saves smaller than 12 MB' % (sw, sh), size < 12 * 1024 * 1024,
          '%s = %.1f MB, raw pixels would be %.1f MB'
          % (os.path.basename(out), size / 1048576.0, raw / 1048576.0))
    check('the saved file is lighter than the pixels it is made of', size < raw,
          '%.1f MB against %.1f MB of pixels' % (size / 1048576.0, raw / 1048576.0))
    dims = Image.open(out).size
    check('the full-screen save is the full screen', dims == (sw, sh), '%s -> %s' % (dims, (sw, sh)))
    stop()
finally:
    if backup is not None:
        open(INI, 'wb').write(backup)
    if proc:
        proc.kill()

print('\n%s' % ('STAGE 0.3 OPEN/OUTPUT ACCEPTANCE PASSED' if not fails else 'FAILURES: ' + ', '.join(fails)))
sys.exit(1 if fails else 0)
