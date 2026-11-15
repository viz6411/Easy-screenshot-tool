"""Verify ShotMvp is per-monitor DPI aware: Windows itself reports the running window as
per-monitor aware, the overlay covers the whole physical screen, and a selection dragged into the
bottom-right corner saves a file measured in physical pixels.

Run:  python dpi_verify.py            exit code 0 = everything passed

The manifest that makes this happen is compiled by windres (src/app.rc + src/app.manifest), which
is what card #18 replaced: it used to be patched into the bytes of the already built exe.

Input is posted to the app's own windows only - the machine this runs on is somebody's desk.
"""
import ctypes, os, subprocess, sys, time

import ui_probe as P

u = P.u
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
OUTDIR = os.path.join(ROOT, 'test-output')
INI = os.path.join(ROOT, 'build', 'settings.ini')
os.makedirs(OUTDIR, exist_ok=True)

fails = []
proc = None

u.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p


def check(name, ok, detail=''):
    print('  %-52s %-4s %s' % (name, 'PASS' if ok else 'FAIL', detail))
    if not ok:
        fails.append(name)


def ini_backup():
    return open(INI, 'rb').read() if os.path.exists(INI) else None


def ini_save_dir(folder):
    keep = [l for l in (ini_backup() or b'').decode('utf-8-sig').splitlines()
            if l.split('=')[0].strip() not in ('save_dir', 'save_ask')]
    open(INI, 'wb').write(('\n'.join(keep) + '\nsave_dir=%s\nsave_ask=0\n' % folder).encode('utf-8'))


P.make_probe_dpi_aware()
ref_w, ref_h = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
print('physical screen this check measures against: %dx%d' % (ref_w, ref_h))

before = set(os.listdir(OUTDIR))
backup = ini_backup()
try:
    ini_save_dir(OUTDIR)
    proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), '-grab', '-region'])

    ovh = None
    for _ in range(20):
        w = P.find_window('ShotMvpOverlay')
        if w:
            ovh = w[0]; break
        time.sleep(0.4)
    if not ovh:
        print('no overlay window'); sys.exit(1)
    r = P.W.RECT(); u.GetWindowRect(ovh, P.ctypes.byref(r))
    ow, oh = r.right - r.left, r.bottom - r.top
    print('overlay window: %dx%d at (%d,%d)' % (ow, oh, r.left, r.top))
    check('the overlay covers the whole physical screen', ow >= ref_w and oh >= ref_h,
          '%dx%d against %dx%d' % (ow, oh, ref_w, ref_h))

    # A selection reaching the bottom-right corner - the area that used to be cut off.
    x0, y0, x1, y1 = 100, 100, ref_w - 200, ref_h - 200
    P.drag(x0, y0, x1, y1, steps=18, settle=1.0, hwnd=ovh)

    ed = None
    for _ in range(25):
        w = P.find_window('ShotMvpEditor')
        if w:
            ed = w[0]; break
        time.sleep(0.4)
    if not ed:
        print('the editor never opened after the selection'); sys.exit(1)

    # The proof that the compiled manifest took effect: Windows itself says this window (another
    # process's window, so an opaque context handle) is per-monitor V2 aware. Comparing contexts is
    # what AreDpiAwarenessContextsEqual is for; DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is -4.
    ctx = u.GetWindowDpiAwarenessContext(ed)
    u.AreDpiAwarenessContextsEqual.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    u.AreDpiAwarenessContextsEqual.restype = ctypes.c_int
    per_monitor_v2 = u.AreDpiAwarenessContextsEqual(ctx, ctypes.c_void_p(-4))
    check('Windows reports the editor window as per-monitor V2 DPI aware', bool(per_monitor_v2),
          'DPI awareness context of the editor window = %s' % ctx)

    marker = time.time()
    P.ctrl_key(0x53, hwnd=ed)
    saved = None
    for _ in range(30):
        new = [f for f in os.listdir(OUTDIR) if f not in before and f.lower().endswith('.png')]
        if new:
            saved = os.path.join(OUTDIR, new[0]); break
        time.sleep(0.5)
    if not saved:
        print('Ctrl+S wrote nothing into %s' % OUTDIR); sys.exit(1)

    data = open(saved, 'rb').read()
    w_px, h_px = P.struct.unpack_from('>II', data, 16)
    want_w, want_h = x1 - x0, y1 - y0
    print('saved %s = %d bytes' % (os.path.basename(saved), len(data)))
    check('the dragged corner selection saves the area it covered',
          abs(w_px - want_w) <= 4 and abs(h_px - want_h) <= 4,
          'file %dx%d for the region %dx%d' % (w_px, h_px, want_w, want_h))
    check('the saved file is a real PNG', data[:8] == b'\x89PNG\r\n\x1a\n' and len(data) < w_px * h_px * 4,
          '%d bytes for %dx%d pixels' % (len(data), w_px, h_px))
finally:
    if backup is not None:
        open(INI, 'wb').write(backup)
    if proc:
        proc.kill()

for f in os.listdir(OUTDIR):
    if f not in before:
        os.remove(os.path.join(OUTDIR, f))

print('\n%s' % ('DPI VERIFICATION PASSED' if not fails else 'FAILURES: ' + ', '.join(fails)))
sys.exit(1 if fails else 0)
