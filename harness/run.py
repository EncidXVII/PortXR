"""Automated gate: runs a fixture under the SteamVR null driver and asserts stereo, UI, input, theater, menu."""
import argparse
import ctypes
import ctypes.wintypes as wt
import glob
import hashlib
import json
import math
import os
import shutil
import socket
import subprocess
import sys
import time

import numpy as np
from PIL import Image

import steamvr
import windows

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "harness", "out")
APP = os.path.join(ROOT, "dist", "Parallax.exe")
PORT = 47800
MENU_BIT, A_BIT = 64, 1
POST_FIXTURES = {"F2", "F5"}  # fixtures whose game camera has post-processing to carry over
user32 = ctypes.windll.user32


def placement(primary):
    """Game window args and null-HMD window rect. Unless told otherwise, both go to a secondary monitor so a run does
    not cover the main screen."""
    screen = None if primary else steamvr.secondary_monitor()
    if screen is None:
        return ["-screen-width", "1280", "-screen-height", "720"], None
    # Windows may scale the window by the display's DPI, so leave a wide margin before choosing 1280x720.
    small = screen[2] < 1800 or screen[3] < 1100
    size = ["-screen-width", "960", "-screen-height", "540"] if small else ["-screen-width", "1280", "-screen-height", "720"]
    x, y, w, h = screen
    # The null HMD window is DPI-scaled too; half the monitor keeps it inside.
    return size + ["-monitor", "2"], (x, y, w // 2, h // 2)


def test_processes(exe):
    """Executables whose windows a run may open: the game, its crash handler, SteamVR."""
    return {os.path.basename(exe).lower(), "unitycrashhandler64.exe"} | {p + ".exe" for p in steamvr.PROCESSES}


class Game:
    def __init__(self, exe, window_args):
        self.exe = exe
        self.window_args = window_args
        self.log = os.path.join(OUT, "fixture.log")
        self.proc = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(1.0)

    def start(self):
        if os.path.exists(self.log):
            os.remove(self.log)
        env = dict(os.environ, PARALLAX_TEST_PORT=str(PORT), FIXTURE_LOG=self.log, XR_RUNTIME_JSON=steamvr.runtime_json())
        self.proc = subprocess.Popen([self.exe, "-screen-fullscreen", "0"] + self.window_args, env=env)

    def cmd(self, line, tries=20):
        for _ in range(tries):
            self.sock.sendto(line.encode(), ("127.0.0.1", PORT))
            try:
                reply = self.sock.recv(65536).decode()
            except (socket.timeout, ConnectionResetError):
                if self.proc.poll() is not None:
                    raise RuntimeError(f"game exited with {self.proc.returncode}")
                continue
            if reply.startswith("error"):
                raise RuntimeError(f"{line}: {reply}")
            return reply
        raise RuntimeError(f"no reply to '{line}'")

    def stats(self):
        return json.loads(self.cmd("stats"))

    def wait(self, predicate, timeout, what):
        end = time.time() + timeout
        s = None
        while time.time() < end:
            try:
                s = self.stats()
            except RuntimeError:
                if self.proc.poll() is not None:
                    raise
                continue  # still loading
            if predicate(s):
                return s
            time.sleep(0.2)
        raise AssertionError(f"timed out waiting for {what}: {s}")

    def fixture(self):
        with open(self.log, encoding="utf-8") as f:
            return json.loads(f.readlines()[-1])

    def capture(self, name, files):
        d = os.path.join(OUT, name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        self.cmd(f"capture {d}")
        end = time.time() + 5
        paths = [os.path.join(d, f) for f in files]
        while time.time() < end and not all(os.path.exists(p) for p in paths):
            time.sleep(0.1)
        time.sleep(0.2)
        return [np.asarray(Image.open(p).convert("RGBA")).astype(np.int32) for p in paths]

    def hold(self, ctrl, seconds):
        self.cmd("ctrl " + ctrl)
        time.sleep(seconds)
        self.cmd("ctrl off")

    def focus(self):
        hwnds = []
        proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def cb(hwnd, _):
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.proc.pid and user32.IsWindowVisible(hwnd):
                hwnds.append(hwnd)
            return True

        user32.EnumWindows(proc(cb), 0)
        for h in hwnds:
            # A synthetic Alt press lifts the foreground lock for this process.
            user32.keybd_event(0x12, 0, 0, 0)
            user32.SetForegroundWindow(h)
            user32.keybd_event(0x12, 0, 2, 0)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.cmd("quit", tries=2)
                self.proc.wait(30)  # old-runtime (.NET 3.5) players take ~8 s to shut down
            except Exception:
                self.proc.kill()
                self.proc.wait()  # the next instance needs the test port free


def mask(img, rgb, tol=60):
    d = np.abs(img[..., :3] - np.array(rgb)).max(axis=2)
    return d < tol


def centroid(m):
    ys, xs = np.nonzero(m)
    if len(xs) == 0:
        raise AssertionError("marker not visible")
    return xs.mean(), ys.mean()


def check(name, cond, detail):
    print(("PASS " if cond else "FAIL ") + name + ": " + detail, flush=True)
    RESULTS[name] = {"pass": bool(cond), "detail": detail}


RESULTS = {}


def gate_stereo(g):
    left, right = g.capture("stereo", ["left.bmp", "right.bmp"])
    h, w = left.shape[:2]
    lx, _ = centroid(mask(left, (255, 255, 0)))
    rx, _ = centroid(mask(right, (255, 255, 0)))
    check("1 eye order", lx > rx + 3, f"near marker x left={lx:.1f} right={rx:.1f}")

    ok = True
    detail = []
    for eye, img in (("L", left), ("R", right)):
        cx, cy = centroid(mask(img, (255, 0, 0)))
        ok &= cx < w / 2 and cy < h / 2
        detail.append(f"{eye} red at ({cx / w:.2f},{cy / h:.2f})")
    ground = img[int(h * 0.9), w // 2, :3]
    ok &= bool(np.all(np.abs(ground - 128) < 20))
    detail.append(f"ground {ground.tolist()}")
    check("2 orientation+color", ok, ", ".join(detail))

    s = g.stats()
    up, down = math.tan(s["fov"][2]), math.tan(s["fov"][3])
    door = mask(left, (255, 0, 255))
    rows = np.nonzero(door[:, door.sum(axis=0).argmax()])[0]
    span = rows.max() - rows.min() + 1
    expected = h * (2.0 / 4.95) / (up - down)
    check("3 scale", abs(span - expected) / expected < 0.03, f"door {span}px expected {expected:.1f}px")


def gate_post(g):
    """URP fixture: bloom around the HDR glow sphere shows the eye cameras inherited the game's post-processing."""
    (left,) = g.capture("post", ["left.bmp"])
    ys, xs = np.nonzero(left[..., :3].min(axis=2) > 240)
    cx, cy, r = xs.mean(), ys.mean(), math.sqrt(len(xs) / math.pi)
    yy, xx = np.indices(left.shape[:2])
    d = np.hypot(xx - cx, yy - cy)
    ring = left[(d > 1.5 * r) & (d < 3 * r)][:, :3]
    lift = ring.mean() - np.array([26, 38, 77]).mean()
    check("11 post-processing", lift > 15, f"halo lifts the sky around the glow by {lift:.0f} levels")


def gate_tracking(g):
    base = g.stats()["eyeL"]
    g.cmd("head 0.1 0 0 0")
    time.sleep(0.4)
    moved = g.stats()["eyeL"]
    dx = moved[0] - base[0]
    g.cmd("recenter")
    time.sleep(0.4)
    back = g.stats()["eyeL"]
    g.cmd("head off")
    g.cmd("recenter")
    time.sleep(0.3)
    err = max(abs(a - b) for a, b in zip(back, base))
    check("4 tracking", abs(dx - 0.1) < 0.005 and err < 0.005, f"dx={dx:.4f} after recenter err={err:.4f}")


def gate_timing(g):
    a = g.stats()
    time.sleep(5)
    b = g.stats()
    frames = b["frames"] - a["frames"]
    gaps = frames - (b["submitted"] - a["submitted"])
    ok = b["p95Ms"] <= b["periodMs"] * 1.25 and abs(gaps) <= 2 and frames > 0
    check("5 timing", ok, f"p95 {b['p95Ms']}ms period {b['periodMs']}ms frames {frames} unsubmitted {gaps}")


def gate_ui(g):
    (quad,) = g.capture("ui", ["quad.bmp"])
    h, w = quad.shape[:2]
    white = (quad[..., :3].min(axis=2) > 200) & (quad[..., 3] > 128)
    top = white[: int(h * 0.2)].sum()
    rest = white[int(h * 0.2):].sum()
    blue = mask(quad, (51, 153, 255), 50).sum()
    check("6 ui", top > 500 and top > rest and blue > 2000, f"title pixels top={top} rest={rest} button={blue}")


def gate_input(g):
    g.focus()
    f0 = g.fixture()
    g.hold("0 0 1 0 0 0 0 0 0", 1.0)
    time.sleep(0.4)
    f1 = g.fixture()
    g.hold("0 0 0 1 0 0 0 0 0", 0.5)
    time.sleep(0.4)
    f2 = g.fixture()
    g.hold(f"{A_BIT} 0 0 0 0 0 0 0 0", 0.2)
    time.sleep(1.0)
    f3 = g.fixture()
    for line in ("pointer 0.5 0.8333 0", "pointer 0.5 0.8333 1", "pointer 0.5 0.8333 0"):
        g.cmd(line)
        time.sleep(0.3)
    g.cmd("pointer off")
    time.sleep(0.4)
    f4 = g.fixture()
    moved = f1["pos"][2] - f0["pos"][2]
    turned = f2["yaw"] - f1["yaw"]
    ok = moved > 1.0 and turned > 20 and f3["jumps"] > f2["jumps"] and f4["clicks"] > f3["clicks"]
    check("7 input", ok, f"forward {moved:.2f}m, turned {turned:.0f}deg, jumps {f2['jumps']}->{f3['jumps']}, clicks {f3['clicks']}->{f4['clicks']}")


def gate_theater(g):
    g.cmd("theater 1")
    time.sleep(0.5)
    a = g.stats()
    time.sleep(1.0)
    b = g.stats()
    (quad,) = g.capture("theater", ["quad.bmp"])
    stereo_stopped = b["stereo"] == a["stereo"] and b["quad"] > a["quad"]
    # The full game frame is opaque everywhere; the stereo-mode UI capture is transparent outside the UI.
    opaque = ((quad[..., 3] > 250) & (quad[..., :3].max(axis=2) > 10)).mean()
    g.cmd("theater 0")
    time.sleep(0.5)
    c = g.wait(lambda s: s["stereo"] > b["stereo"], 5, "stereo after theater")
    check("8 theater", stereo_stopped and opaque > 0.95 and c["theater"] == 0,
          f"stereo frozen {stereo_stopped}, game frame covers {opaque:.0%} of the screen")


def tap_menu(g, taps):
    for _ in range(taps):
        g.cmd(f"ctrl {MENU_BIT} 0 0 0 0 0 0 0 0")
        time.sleep(0.08)
        g.cmd("ctrl 0 0 0 0 0 0 0 0 0")
        time.sleep(0.08)
    g.cmd("ctrl off")
    time.sleep(0.5)


def gate_menu(g):
    tap_menu(g, 1)
    single = g.stats()["menu"]
    tap_menu(g, 2)
    opened = g.stats()["menu"]
    g.cmd("set WorldScale 1.5")
    tap_menu(g, 2)
    closed = g.stats()["menu"]
    check("9 menu", single == 0 and opened == 1 and closed == 0, f"single tap {single}, double tap {opened}, closed {closed}")


def gate_persist(g):
    value = g.cmd("get WorldScale")
    g.cmd("set WorldScale 1")
    check("9 menu persist", value.startswith("1.5"), f"WorldScale after restart {value}")


def gate_recovery(g):
    """Killing SteamVR mid-game: the game must keep running flat, then pick VR up again once the runtime is back
    (its next xrCreateInstance relaunches SteamVR)."""
    steamvr.stop()
    g.wait(lambda s: s["state"] != 3, 30, "session loss")
    s = g.wait(lambda s: s["state"] == 3, 180, "session after restart")
    time.sleep(2)
    after = g.stats()
    check("12 recovery", after["state"] == 3 and after["stereo"] > s["stereo"],
          f"stereo frames {s['stereo']} -> {after['stereo']} after the runtime restart")


def working_set(pid):
    out = subprocess.run(["tasklist", "/fi", f"PID eq {pid}", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    return int(out.split('","')[-1].strip().strip('"').replace(",", "").replace("\xa0", "").split()[0]) / 1024


def gate_soak(g, seconds, log_path):
    start_mem = working_set(g.proc.pid)
    end = time.time() + seconds
    while time.time() < end:
        g.hold("0 0 0.5 0.3 0 0 0 0 0", 2.0)
        time.sleep(1.0)
    mem = working_set(g.proc.pid) - start_mem
    with open(log_path, encoding="utf-8", errors="replace") as f:
        errors = [l.strip() for l in f if l.startswith(("[Error", "[Fatal"))]
    check("10 stability", mem < 20 and not errors, f"{seconds}s, memory +{mem:.1f} MB, errors {errors[:3]}")


def tree_hash(root):
    """Content hash of every file; multi-GB game assets are compared by size and mtime, which install never touches."""
    h = {}
    for p in glob.glob(os.path.join(root, "**", "*"), recursive=True):
        if os.path.isfile(p):
            st = os.stat(p)
            if st.st_size > 64 << 20:
                h[os.path.relpath(p, root)] = (st.st_size, st.st_mtime_ns)
                continue
            with open(p, "rb") as f:
                h[os.path.relpath(p, root)] = hashlib.sha256(f.read()).hexdigest()
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default="F1")
    ap.add_argument("--soak", type=int, default=30, help="stability run length in seconds")
    ap.add_argument("--input", action="store_true",
                    help="run the input gate: it focuses the game and drives the real mouse/keyboard for a few seconds")
    ap.add_argument("--primary", action="store_true", help="put test windows on the main screen")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    build = os.path.join(ROOT, "fixtures", args.fixture, "Build")
    exe = os.path.join(build, args.fixture + ".exe")
    window_args, hmd_window = placement(args.primary)

    before = tree_hash(build)
    subprocess.run([APP, "install", build], check=True)
    try:
        with steamvr.null_driver(hmd_window), windows.keep_on(hmd_window, test_processes(exe), not args.input):
            g = Game(exe, window_args)
            try:
                g.start()
                # IL2CPP first launch generates interop assemblies before plugins load.
                g.wait(lambda s: s["state"] == 3 and s["stereo"] > 10, 400, "stereo frames")
                gates = [gate_stereo, gate_tracking, gate_timing, gate_ui, gate_theater, gate_menu]
                if args.input:
                    gates.insert(4, gate_input)
                else:
                    print("SKIP 7 input: pass --input to run it (takes over mouse and keyboard)", flush=True)
                if args.fixture in POST_FIXTURES:
                    gates.insert(1, gate_post)
                for gate in gates:
                    try:
                        gate(g)
                    except Exception as ex:
                        check(gate.__name__, False, repr(ex))
                gate_soak(g, args.soak, os.path.join(build, "BepInEx", "LogOutput.log"))
                g.stop()
                g.start()
                g.wait(lambda s: s["state"] == 3, 120, "restart")
                gate_persist(g)
                gate_recovery(g)
            finally:
                g.stop()
    finally:
        subprocess.run([APP, "uninstall", build], check=True)
    check("install round trip", tree_hash(build) == before, "game folder byte-identical after uninstall")
    with open(os.path.join(OUT, "results.json"), "w") as f:
        json.dump(RESULTS, f, indent=2)
    sys.exit(0 if all(r["pass"] for r in RESULTS.values()) else 1)


if __name__ == "__main__":
    main()
