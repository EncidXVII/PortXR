"""Godot VR gate: fixture G1 (Godot 4.5, fixtures/G1) run with Parallax's Godot install (override.cfg + the script) on
the SteamVR null driver, checked in what the compositor shows the test headset.

    python harness/godot.py [--secondary]

Needs the Godot 4.5 editor (GODOT, default E:\\Godot\\Godot_v4.5-stable_win64_console.exe): G1 runs as a project, so no
export templates. The runtime is woken first (ParallaxTheater.exe --wake), as the app does before a Godot game.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

import numpy as np

import run
import steamvr
import windows

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GODOT = os.environ.get("GODOT", r"E:\Godot\Godot_v4.5-stable_win64_console.exe")
G1 = os.path.join(ROOT, "fixtures", "G1")
SCRIPT = os.path.join(ROOT, "godot", "parallax_vr.gd")
HOST = os.path.join(ROOT, "build", "native", "Release", "ParallaxTheater.exe")
OUT = os.path.join(run.OUT, "g1")
results = []


def check(name, ok, detail):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


def headset_view(path):
    """RGB of SteamVR's null-HMD 'Headset Window' (PrintWindow, full content): what the compositor shows."""
    u, g = ctypes.windll.user32, ctypes.windll.gdi32
    u.SetProcessDPIAware()
    found = []

    def visit(h, _):
        pid = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if windows.process_name(pid.value) in ("vrcompositor.exe", "vrserver.exe") and u.IsWindowVisible(h):
            r = wt.RECT()
            u.GetClientRect(h, ctypes.byref(r))
            found.append((r.right * r.bottom, h, r.right, r.bottom))
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)(visit), 0)
    if not found:
        return None
    _, h, w, hh = max(found)
    dc = u.GetDC(h)
    mem = g.CreateCompatibleDC(dc)
    bmp = g.CreateCompatibleBitmap(dc, w, hh)
    g.SelectObject(mem, bmp)
    u.PrintWindow(h, mem, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * hh * 4)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("x", wt.LONG), ("y", wt.LONG), ("used", wt.DWORD), ("important", wt.DWORD)]

    g.GetDIBits(mem, bmp, 0, hh, buf, ctypes.byref(BIH(ctypes.sizeof(BIH), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)), 0)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(h, dc)
    img = np.frombuffer(buf, np.uint8).reshape(hh, w, 4)[..., [2, 1, 0]].copy()
    from PIL import Image
    Image.fromarray(img).save(path)
    return img.astype(int)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secondary", action="store_true", help="put test windows on a second monitor")
    args = ap.parse_args()
    if not os.path.exists(GODOT):
        sys.exit(f"Godot 4.5 editor not found at {GODOT} (set GODOT)")
    os.makedirs(OUT, exist_ok=True)
    if not os.path.isdir(os.path.join(G1, ".godot")):
        subprocess.run([GODOT, "--headless", "--path", G1, "--editor", "--quit"], capture_output=True, timeout=300)
    # The same override.cfg the app writes (installer.rs, install_godot).
    with open(os.path.join(G1, "override.cfg"), "w") as f:
        f.write('\n[xr]\n\nopenxr/enabled=true\nshaders/enabled=true\n\n[rendering]\n\nrendering_device/driver.windows="vulkan"\n\n'
                f'[autoload]\n\nParallaxVR="*{SCRIPT.replace(os.sep, "/")}"\n')
    screen = steamvr.secondary_monitor() if args.secondary else None
    _, hmd = run.placement(secondary=args.secondary)
    x, y = (screen[0] + 40, screen[1] + 40) if screen else (40, 40)
    env = dict(os.environ, XR_RUNTIME_JSON=steamvr.runtime_json())
    log = os.path.join(OUT, "stdout.log")
    names = {os.path.basename(GODOT).lower(), "parallaxtheater.exe"} | {p + ".exe" for p in steamvr.PROCESSES}
    try:
        # The dashboard opens over a session the null driver gives no controllers to close it with.
        with steamvr.null_driver(hmd, {"dashboard": {"enableDashboard": False}}), windows.keep_on(screen, names):
            t = time.time()
            woke = subprocess.run([HOST, "--wake"], env=env, timeout=90).returncode
            check("G1 wake", woke == 0, f"runtime ready in {time.time() - t:.1f} s")
            with open(log, "w") as out:
                game = subprocess.Popen([GODOT, "--path", G1, "--windowed", "--position", f"{x},{y}"]
                                        + (["--screen", "1"] if screen else []), stdout=out, stderr=subprocess.STDOUT, env=env)
                try:
                    time.sleep(20)
                    img = headset_view(os.path.join(OUT, "headset.png"))
                finally:
                    game.terminate()
                    game.wait(20)
    finally:
        os.remove(os.path.join(G1, "override.cfg"))
    text = open(log, encoding="utf-8", errors="replace").read()
    check("G1 openxr", "Parallax: OpenXR on" in text, "the game's own OpenXR started through override.cfg")
    check("G1 camera", "camera /root/Main/Player" in text, "the headset follows the game's player camera")
    if img is None:
        check("G1 headset", False, "no Headset Window")
    else:
        r, g, b = img[..., 0], img[..., 1], img[..., 2]
        red = float(((r > 180) & (g < 70) & (b < 70)).mean())
        green = float(((g > 180) & (r < 70) & (b < 70)).mean())
        yellow = int(((r > 180) & (g > 180) & (b < 80)).sum())
        check("G1 world", red > 0.003 and green > 0.003, f"red box {red:.1%}, green box {green:.1%} of the view")
        check("G1 ui panel", yellow > 0, f"crosshair pixels {yellow} (the game's HUD on the panel)")
    print(f"{sum(results)}/{len(results)} passed", flush=True)
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
