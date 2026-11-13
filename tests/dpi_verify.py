"""Verify ShotMvp is DPI aware and captures the full physical screen."""
import ctypes, ctypes.wintypes as W
import subprocess, time, sys, os, glob, struct

u = ctypes.windll.user32
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, W.HWND, W.LPARAM)


def find(cls):
    res = []

    def cb(h, l):
        c = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(h, c, 256)
        if c.value == cls and u.IsWindowVisible(h):
            r = W.RECT(); u.GetWindowRect(h, ctypes.byref(r))
            res.append((h, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True

    u.EnumWindows(EnumProc(cb), 0)
    return res


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
outdir = os.path.join(ROOT, 'test-output')
os.makedirs(outdir, exist_ok=True)

# Reference: what a DPI-aware process sees.
ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
ref_w = u.GetSystemMetrics(0); ref_h = u.GetSystemMetrics(1)
print("reference physical screen: %dx%d" % (ref_w, ref_h))

before = set(glob.glob(os.path.join(outdir, 'screenshot_*.png')))

# Run the app in region mode and check the overlay covers the WHOLE physical screen.
proc = subprocess.Popen([os.path.join(ROOT, 'build', 'ShotMvp.exe'), '-grab', '-region'])
time.sleep(3)
ov = find('ShotMvpOverlay')
if not ov:
    print("no overlay"); proc.kill(); sys.exit(1)
h, x, y, w, hh = ov[0]
print("overlay window: %dx%d at (%d,%d)" % (w, hh, x, y))
full = (w >= ref_w and hh >= ref_h)
print("OVERLAY COVERS FULL PHYSICAL SCREEN:", full)

# Drag a selection that reaches the bottom-right corner - the area that used to be cut off.
u.SetCursorPos(100, 100); time.sleep(0.3)
u.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.2)
steps = 18
for i in range(1, steps + 1):
    u.SetCursorPos(100 + (ref_w - 300 - 100) * i // steps, 100 + (ref_h - 300 - 100) * i // steps)
    time.sleep(0.04)
u.SetCursorPos(ref_w - 200, ref_h - 200); time.sleep(0.2)
u.mouse_event(0x0004, 0, 0, 0, 0)
print("dragged selection to near bottom-right corner (%d,%d)" % (ref_w - 200, ref_h - 200))

# Confirm the editor so it saves.
ed = None
for _ in range(20):
    time.sleep(0.4)
    e = find('ShotMvpEditor')
    if e:
        ed = e[0]; break
if ed:
    print("editor %dx%d" % (ed[3], ed[4]))
    time.sleep(1.0)
    u.keybd_event(0x11, 0, 0, 0); u.keybd_event(0x53, 0, 0, 0)
    u.keybd_event(0x53, 0, 2, 0); u.keybd_event(0x11, 0, 2, 0)
    time.sleep(2.0)

after = sorted(set(glob.glob(os.path.join(outdir, 'screenshot_*.png'))) - before)
for f in after:
    d = open(f, 'rb').read()
    ww, hh2 = struct.unpack_from('>II', d, 16)
    print("saved %s -> %dx%d (%d bytes)" % (os.path.basename(f), ww, hh2, len(d)))
    copy_to = os.path.join('.', 'shot_dpi_check.png')
    open(copy_to, 'wb').write(d)
    print("copied to", copy_to)

proc.terminate()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
