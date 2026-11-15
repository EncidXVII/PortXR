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
POST_FIXTURES = {"F2", "F5", "F7", "F10", "F11"}  # fixtures whose game camera has post-processing to carry over
PAD_FIXTURES = {"F6"}  # fixtures that read the Input System package
user32 = ctypes.windll.user32


def placement(secondary):
    """Game window args and null-HMD window rect. With secondary, both go to a second monitor so a run does not cover
    the main screen."""
    screen = steamvr.secondary_monitor() if secondary else None
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

    def drain(self):
        # Replies to a resent command can arrive late and would be read as the answer to the next one.
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recv(65536)
        except OSError:  # nothing waiting (or, before the first send, not bound yet)
            pass
        finally:
            self.sock.settimeout(1.0)

    def cmd(self, line, tries=20):
        self.drain()
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
        # A late reply to an earlier, resent command can arrive first; stats replies are always JSON.
        for _ in range(5):
            reply = self.cmd("stats")
            if reply.startswith("{"):
                return json.loads(reply)
        raise RuntimeError(f"no stats reply, last got '{reply}'")

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
        return [load_bmp(p) for p in paths]

    def hold(self, ctrl, seconds):
        self.cmd("ctrl " + ctrl)
        time.sleep(seconds)
        self.cmd("ctrl off")

    def windows(self):
        hwnds = []
        proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def cb(hwnd, _):
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == self.proc.pid and user32.IsWindowVisible(hwnd):
                hwnds.append(hwnd)
            return True

        user32.EnumWindows(proc(cb), 0)
        return hwnds

    def focus(self):
        for h in self.windows():
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


def load_bmp(path):
    """RGBA from the capture's 32-bit BMP. PIL reads BI_RGB 32-bit as RGB and drops the alpha the UI gates need."""
    b = open(path, "rb").read()
    off = int.from_bytes(b[10:14], "little")
    w, h = int.from_bytes(b[18:22], "little", signed=True), int.from_bytes(b[22:26], "little", signed=True)
    img = np.frombuffer(b, np.uint8, abs(w * h) * 4, off).reshape(abs(h), w, 4)[..., [2, 1, 0, 3]]
    return (img if h < 0 else img[::-1]).astype(np.int32)


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


def gate_mirror(g):
    """The desktop window mirrors the left eye: mostly the world's sky and ground, not only the UI. Crops differ per
    path (managed blit, native Present, HDRP's scaling), so colours are compared, not pixels."""
    (left,) = g.capture("mirror", ["left.bmp"])
    hwnd = next(h for h in g.windows() if window_class(h) == "UnityWndClass")
    window = print_window(hwnd).astype(int)
    Image.fromarray(window.astype(np.uint8)).save(os.path.join(OUT, "mirror", "window.png"))
    eh, ew = left.shape[:2]
    sky, ground = left[eh // 20, ew // 10, :3], left[eh * 19 // 20, ew // 2, :3]
    near = lambda c: np.all(np.abs(window - c) < 24, axis=2)
    share = float((near(sky) | near(ground)).mean())
    check("1b desktop mirror", share > 0.5, f"{share:.0%} of the window is the eye's sky or ground")


def window_class(hwnd):
    name = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(hwnd, name, 64)
    return name.value


def client_size(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right, r.bottom


def print_window(hwnd):
    """The window's own content, even under other windows (PW_RENDERFULLCONTENT reads DirectX windows too)."""
    user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    gdi32 = ctypes.windll.gdi32
    w, h = client_size(hwnd)
    wdc = user32.GetDC(hwnd)
    dc = gdi32.CreateCompatibleDC(wdc)
    bmp = gdi32.CreateCompatibleBitmap(wdc, w, h)
    gdi32.SelectObject(dc, bmp)
    user32.PrintWindow(hwnd, dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    header = (ctypes.c_uint32 * 10)(40, w, (-h) & 0xFFFFFFFF, 1 | (32 << 16), 0, 0, 0, 0, 0, 0)
    buf = (ctypes.c_ubyte * (w * h * 4))()
    gdi32.GetDIBits(dc, bmp, 0, h, buf, header, 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(hwnd, wdc)
    return np.frombuffer(buf, np.uint8).reshape(h, w, 4)[:, :, 2::-1].copy()


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
    # The world shows through: post-processing on the hidden game camera would make the whole panel opaque.
    opaque = (quad[..., 3] > 128).mean()
    check("6b ui see-through", opaque < 0.3, f"{opaque:.0%} of the panel opaque")
    # The 50% white square left of middle keeps its opacity (alpha about 128) and its premultiplied colour (half white:
    # 128 on a plain swapchain, 188 on an sRGB one). Unity 2018's UI shader stores alpha squared (25%), and keying a
    # panel the game left see-through dims it more; too much (71%) is wrong as well. Holds through the off-screen
    # camera too (F5, F7: their post drew over the UI).
    square = quad[int(h * 0.45):int(h * 0.55), int(w * 0.05):int(w * 0.1)]
    a, c = float(np.median(square[..., 3])), float(np.median(square[..., :3]))
    check("6c translucent ui", 108 < a < 150 and (110 < c < 146 or 172 < c < 204), f"alpha {a:.0f}, colour {c:.0f} (alpha about 128 wanted)")


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


def gate_pointer(g):
    """Laser pointer on the game's button with the game window not in front: the click goes through the game's
    EventSystem, not the OS mouse, so it lands anyway and never touches the real cursor."""
    f0 = g.fixture()
    for line in ("pointer 0.5 0.8333 0", "pointer 0.5 0.8333 1", "pointer 0.5 0.8333 0"):
        g.cmd(line)
        time.sleep(0.3)
    g.cmd("pointer off")
    time.sleep(0.4)
    f1 = g.fixture()
    check("7b pointer (unfocused)", f1["clicks"] > f0["clicks"], f"clicks {f0['clicks']}->{f1['clicks']}")


def gate_pad(g):
    """Gamepad mode on an Input System game: our virtual pad moves, turns and jumps the player. No real input is
    touched, so this runs unattended."""
    g.cmd("set Mode Gamepad")
    try:
        time.sleep(0.5)
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
    finally:
        g.cmd("set Mode KeyboardMouse")
    moved = f1["pos"][2] - f0["pos"][2]
    turned = f2["yaw"] - f1["yaw"]
    ok = f1["pad"] == "ParallaxPad" and moved > 1.0 and turned > 20 and f3["jumps"] > f2["jumps"]
    check("7 gamepad (Input System)", ok,
          f"pad {f1['pad']!r}, forward {moved:.2f}m, turned {turned:.0f}deg, jumps {f2['jumps']}->{f3['jumps']}")


def gate_vignette(g):
    """Comfort vignette: with the left stick held, the eye image's corners darken while its centre stays."""
    def patches(img):
        h, w = img.shape[:2]
        corners = (img[int(h * 0.88):, : int(w * 0.12), :3].mean() + img[int(h * 0.88):, int(w * 0.88):, :3].mean()) / 2
        return corners, img[int(h * 0.4):int(h * 0.6), int(w * 0.4):int(w * 0.6), :3].mean()

    g.cmd("set Vignette High")
    try:
        time.sleep(1.0)
        (rest,) = g.capture("vignette-rest", ["left.bmp"])
        g.cmd("ctrl 0 0 1 0 0 0 0 0 0")
        time.sleep(1.0)
        strength = g.stats()["vignette"]
        (moving,) = g.capture("vignette-moving", ["left.bmp"])
    finally:
        g.cmd("ctrl off")
        g.cmd("set Vignette Off")
    (rc, rm), (mc, mm) = patches(rest), patches(moving)
    ok = strength > 0.9 and mc < rc * 0.5 and abs(mm - rm) < rm * 0.15
    check("13 comfort vignette", ok, f"strength {strength}, corners {rc:.0f} -> {mc:.0f}, centre {rm:.0f} -> {mm:.0f}")


def gate_snap(g):
    """Snap turn on an Input System game in gamepad mode: one flick of the right stick turns the player 30 degrees."""
    g.cmd("set Mode Gamepad")
    g.cmd("set Turning Snap30")
    try:
        time.sleep(0.5)
        f0 = g.fixture()
        g.hold("0 0 0 1 0 0 0 0 0", 0.3)
        time.sleep(1.0)
        f1 = g.fixture()
    finally:
        g.cmd("set Turning Smooth")
        g.cmd("set Mode KeyboardMouse")
    turned = (f1["yaw"] - f0["yaw"] + 180) % 360 - 180
    check("14 snap turn", abs(abs(turned) - 30) < 5, f"one flick turned {turned:.1f}deg (want 30)")


def gate_aim(g):
    """Head aim on an Input System game in gamepad mode: turning the head 40 degrees turns the game camera with it, and
    the crosshair sits mid-view. Hand aim: the right controller pointing 30 degrees right turns the camera there while
    the head looks ahead, and the crosshair moves right in the view."""
    g.cmd("set Mode Gamepad")
    g.cmd("set HeadAim On")
    try:
        time.sleep(0.5)
        f0 = g.fixture()
        g.cmd("head 0 1.6 0 40")
        time.sleep(2.0)
        f1 = g.fixture()
        s = g.stats()
        g.cmd("head 0 1.6 0 0")
        g.cmd("set AimWith Hand")
        g.cmd("set Crosshair Lime")
        g.cmd("viewmodel on")
        g.cmd(f"aim 0.2 -0.3 0.1 {0.2 + 3 * math.sin(math.radians(30)):.3f} -0.3 {0.1 + 3 * math.cos(math.radians(30)):.3f}")
        time.sleep(2.0)
        f2 = g.fixture()
        h = g.stats()
        (eye,) = g.capture("aim", ["left.bmp"])
        g.cmd("set Crosshair Game")
        time.sleep(0.3)
        p = g.stats()
        try:
            quad, mid = g.capture("aim-split", ["quad.bmp", "patch.bmp"])
        except FileNotFoundError:
            quad = mid = None
    finally:
        g.cmd("aim off")
        g.cmd("head off")
        g.cmd("viewmodel off")
        g.cmd("set Crosshair Game")
        g.cmd("set AimWith Head")
        g.cmd("set HeadAim Auto")
        g.cmd("set Mode KeyboardMouse")
    turned = (f1["yaw"] - f0["yaw"] + 180) % 360 - 180
    u, v, r = s["reticle"]
    check("17 head aim", s["aim"] == 1 and abs(turned - 40) < 5 and r > 0 and abs(u - 0.5) < 0.12 and abs(v - 0.5) < 0.12,
          f"aim {s['aim']}, head turned 40, game turned {turned:.1f}deg, crosshair at {u:.2f} {v:.2f} r {r:.4f}")
    hand = (f2["yaw"] - f0["yaw"] + 180) % 360 - 180
    # Lime pixels in a box around the crosshair in the eye image; then the game's own crosshair moved there instead.
    hu, hv, hr = h["reticle"]
    ih, iw = eye.shape[:2]
    x, y, rad = int(hu * iw), int(hv * ih), max(4, int(hr * ih * 2))
    ring = int(mask(eye[y - rad:y + rad, x - rad:x + rad], (198, 255, 26), 45).sum())
    check("17b hand aim", h["aim"] == 1 and abs(hand - 30) < 5 and hu > u + 0.1 and ring > 10 and p["patch"] == 1,
          f"controller at 30, game turned {hand:.1f}deg, crosshair at {hu:.2f} (head aim {u:.2f}), {ring} lime px, game's moved {p['patch']}")
    check("17c weapon in hand", h["held"] == 1, f"viewmodel parts moved to the hand: {h['held']}")
    # The game's middle leaves the panel as a hole and is drawn into a texture of its own, each a whole image (runtimes
    # differ on sub-rectangles of one image).
    if quad is None:
        check("17d crosshair layer", False, "no patch.bmp: the middle was not copied to its own layer")
    else:
        qh, qw = quad.shape[:2]
        hole = float(quad[qh // 2 - 4:qh // 2 + 4, qw // 2 - 4:qw // 2 + 4, 3].max())
        share = mid.shape[1] / qw
        check("17d crosshair layer", hole == 0 and abs(share - 0.18) < 0.02,
              f"panel middle alpha {hole:.0f}, own layer {mid.shape[1]}x{mid.shape[0]} ({share:.2f} of the panel width)")


def gate_theater(g):
    g.cmd("theater 1")
    time.sleep(0.5)
    a = g.stats()
    time.sleep(1.0)
    b = g.stats()
    (quad,) = g.capture("theater", ["quad.bmp"])
    stereo_stopped = b["stereo"] == a["stereo"] and b["quad"] > a["quad"]
    # SteamVR has no curved layers, so this checks the flat fallback there; other runtimes get the curve.
    curved = b["curved"] - a["curved"] == (b["quad"] - a["quad"] if b["curvedSupported"] else 0)
    # The full game frame is opaque everywhere; the stereo-mode UI capture is transparent outside the UI.
    opaque = ((quad[..., 3] > 250) & (quad[..., :3].max(axis=2) > 10)).mean()
    g.cmd("theater 0")
    time.sleep(0.5)
    c = g.wait(lambda s: s["stereo"] > b["stereo"], 5, "stereo after theater")
    check("8 theater", stereo_stopped and curved and opaque > 0.95 and c["theater"] == 0,
          f"stereo frozen {stereo_stopped}, {'curved' if b['curvedSupported'] else 'flat'} screen ok {curved}, game frame covers {opaque:.0%} of the screen")


def gate_screen(g):
    """Screen handles: with the laser on (VR menu open), a corner resizes the theater screen, RESET puts it back, the
    bar under it drags it around the player while the stick pushes it away, and the result is saved for the game.
    Game input pauses on a handle."""
    g.cmd("theater 1")
    g.cmd("menu 1")
    time.sleep(0.5)
    sw, sh = g.stats()["screen"]

    def at(u, v, width=3.2):  # a point on the unmoved screen, 3 m ahead
        return f"{(u - 0.5) * width:.3f} {(0.5 - v) * width * sh / sw:.3f} 3"

    def pull(on):
        g.cmd(f"ctrl 0 0 0 0 0 0 {1 if on else 0} 0 0")
        time.sleep(0.3)

    try:
        # Locked (the default): no handles under the laser, so playing never brings them up.
        g.cmd(f"aim 0.2 -0.3 0.1 {at(0.98, 0.03)}")
        time.sleep(0.8)
        locked = g.stats()["handle"]
        g.cmd("unlock 1")
        corner = g.wait(lambda s: s["handle"] == 2, 3, "corner handle")
        time.sleep(0.3)
        (overlay,) = g.capture("screen-handles", ["handles.bmp"])
        pull(True)
        g.cmd(f"aim 0.2 -0.3 0.1 {at(1.25, -0.25)}")
        time.sleep(0.5)
        pull(False)
        sized = g.stats()
        g.cmd(f"aim 0.2 -0.3 0.1 {at(0.5 + 0.26 * sh / sw, 0.9675, sized['screenPose'][3])}")
        g.wait(lambda s: s["handle"] == 3, 3, "reset handle")
        pull(True)
        pull(False)
        reset = g.stats()
        g.cmd(f"aim 0.2 -0.3 0.1 {at(0.5, 0.9675)}")
        g.wait(lambda s: s["handle"] == 1, 3, "bar handle")
        pull(True)
        g.cmd("aim 0.2 -0.3 0.1 1.8 0.3 3")
        time.sleep(0.5)
        g.cmd("ctrl 0 0 0 0 1 0 1 0 0")
        time.sleep(0.6)
        moved = g.stats()
        pull(False)
        done = g.stats()
        saved = float(g.cmd("get ScreenYaw"))
    finally:
        g.cmd("ctrl off")
        g.cmd("aim off")
        g.cmd("menu 0")
        for key, value in (("ScreenYaw", "0"), ("ScreenPitch", "0"), ("ScreenDistance", "3"), ("ScreenWidth", "3.2")):
            g.cmd(f"set {key} {value}")
        g.cmd("theater 0")
    # The lime corner bracket in the screen's top-right.
    n = max(3, int(overlay.shape[0] * 0.1))
    region = overlay[:n, -n:]
    lime = ((region[..., 1] > 200) & (region[..., 0] > 150) & (region[..., 2] < 90)).sum()
    width, back = sized["screenPose"][3], reset["screenPose"][3]
    yaw, pitch, dist = moved["screenPose"][0], moved["screenPose"][1], moved["screenPose"][2]
    ok = (lime > 20 and corner["adjust"] == 0 and width > 3.6 and abs(back - 3.2) < 0.05 and yaw > 20 and pitch > 5 and dist > 3.3
          and moved["adjust"] == 1 and done["adjust"] == 0 and abs(saved - done["screenPose"][0]) < 0.2)
    check("19 screen handles", ok, f"corner lit {lime} px, resized to {width:.1f} m, reset to {back:.1f} m, dragged to yaw {yaw:.0f} "
          f"pitch {pitch:.0f}, stick to {dist:.1f} m, released {done['adjust'] == 0}, saved yaw {saved}")
    check("19b screen locked", locked == 0, f"handle under the laser while locked: {locked}")


def gate_window(g):
    """Parallax mode: the game in 3D inside the screen. Each eye gets its own window image; the two differ by the
    stereo offset, the VR stereo layer stops, and leaning sideways leaves the view alone (the game's camera alone
    steers it). Stereo resumes after."""
    g.cmd("set Display Parallax")
    try:
        a = g.wait(lambda s: s["window"] == 1, 5, "window mode")
        time.sleep(1.0)
        b = g.stats()
        left, right = g.capture("window", ["windowL.bmp", "windowR.bmp"])
        g.cmd("head -0.25 1.6 0 0")
        time.sleep(0.5)
        (leaned,) = g.capture("window-leaned", ["windowL.bmp"])
        g.cmd("head off")
    finally:
        g.cmd("set Display Stereo")
    c = g.wait(lambda s: s["stereo"] > b["stereo"] and s["window"] == 0, 5, "stereo after window")
    filled = min((img[..., :3].max(axis=2) > 10).mean() for img in (left, right))
    differ = np.abs(left[..., :3].astype(int) - right[..., :3].astype(int)).mean()
    lean = np.abs(left[..., :3].astype(int) - leaned[..., :3].astype(int)).mean()
    vr_stopped = b["stereo"] == a["stereo"] or b["stereo"] - a["stereo"] <= 2
    check("18 parallax window", filled > 0.9 and 0.2 < differ and lean < differ / 2 and vr_stopped and b["focus"] > 0,
          f"filled {filled:.0%}, eyes differ {differ:.2f}, leaning changes {lean:.2f}, VR layer stopped {vr_stopped}, focus {b['focus']}")


def gate_render_scale(g):
    """A stalled main thread misses frames: auto render scale tries one lower resolution, sees no gain (CPU-bound)
    and puts it back."""
    base = g.stats()["eyeSize"]
    g.cmd("busy 25")
    try:
        low = g.wait(lambda s: s["eyeSize"][0] < base[0], 15, "lower render scale")["eyeSize"]
        back = g.wait(lambda s: s["eyeSize"] == base, 15, "render scale restored")["eyeSize"]
    finally:
        g.cmd("busy 0")
    check("5b render scale", low[0] < base[0] and back == base, f"eye width {base[0]} -> {low[0]} -> {back[0]} (CPU-bound, undone)")


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
    time.sleep(0.5)
    (quad,) = g.capture("menu", ["menu.bmp"])
    # One capture per tab for review (harness/out/menu-tab*.bmp): the menu is the core's own layer.
    for t in [*range(6), -1]:
        g.cmd(f"tab {t}")
        time.sleep(0.4)
        g.capture(f"menu-tab{t}", ["menu.bmp"])
    g.cmd("tab 0")
    h, w = quad.shape[:2]
    # The menu's own background is solid.
    panel = int(np.median(quad[int(h * 0.3):int(h * 0.8), int(w * 0.3):int(w * 0.7), 3]))
    g.cmd("set WorldScale 1.5")
    g.cmd("set A F")  # rebind: right A presses F
    tap_menu(g, 2)
    closed = g.stats()["menu"]
    check("9 menu", single == 0 and opened == 1 and closed == 0, f"single tap {single}, double tap {opened}, closed {closed}")
    check("9b menu solid", panel > 200, f"menu panel alpha {panel}")


def gate_beam(g):
    """Laser beam: with the menu open and the right controller aimed at the panel, a lime beam is drawn into the eye
    image (and only then)."""
    def lime(img):
        return int(mask(img, (198, 255, 26), 45).sum())
    # The theme is one file for every game and the app; the run puts back whatever the player had.
    theme = os.path.join(os.environ["APPDATA"], "Parallax", "theme.txt")
    kept = open(theme, encoding="utf-8").read() if os.path.exists(theme) else None
    g.cmd("menu 1")
    try:
        if kept is not None:
            os.remove(theme)
            time.sleep(3.5)  # the plugin reads the file every 3 s
        time.sleep(0.3)
        (before,) = g.capture("beam-off", ["left.bmp"])
        g.cmd("aim 0.25 -0.35 0.1")
        time.sleep(0.3)
        (on,) = g.capture("beam-on", ["left.bmp"])
        hovering = g.stats().get("pointer")
        # Another theme recolours the beam: the accent reaches the shader.
        with open(theme, "w", encoding="utf-8") as fh:
            fh.write("Sky")
        time.sleep(3.5)
        (sky,) = g.capture("beam-sky", ["left.bmp"])
    finally:
        g.cmd("aim off")
        g.cmd("menu 0")
        if kept is None:
            if os.path.exists(theme):
                os.remove(theme)
        else:
            with open(theme, "w", encoding="utf-8") as fh:
                fh.write(kept)
        time.sleep(3.5)  # later gates (the lit watch) see the player's theme again, not Sky
    check("15 laser beam", lime(on) > lime(before) + 1000,
          f"lime pixels {lime(before)} -> {lime(on)}, pointer {hovering}")
    blue = int(mask(sky, (92, 200, 255), 45).sum())
    check("15b theme colour", blue > 1000 and lime(sky) < lime(on) // 4, f"sky pixels {blue}, lime {lime(sky)}")


def gate_wrist(g):
    """Wrist glance: the left hand raised with the back of the wrist to the eyes lights the watch, and frames keep
    coming meanwhile (a stripped math call once froze them on IL2CPP games)."""
    g.cmd("wrist 1")
    try:
        time.sleep(0.5)
        a = g.stats()
        time.sleep(1.0)
        b = g.stats()
        (eye,) = g.capture("wrist", ["left.bmp"])
    finally:
        g.cmd("wrist 0")
    lime = int(mask(eye, (198, 255, 26), 45).sum())
    check("20 wrist glance", b["watch"] == 1 and b["glance"] == 1 and b["stereo"] > a["stereo"] + 30 and lime > 200,
          f"watch {b['watch']}, glance {b['glance']}, frames {a['stereo']} -> {b['stereo']}, watch pixels {lime}")


def gate_hide(g):
    """Hide-object tool: aimed at the magenta door, the trigger hides it (gone from the eye image when it was in view);
    Show all brings it back."""
    def door():
        (left,) = g.capture("hide", ["left.bmp"])
        return int(mask(left, (255, 0, 255), 60).sum())
    g.cmd("set WorldScale 1")  # the target below is in metres of the game world; the menu gate left 1.5
    time.sleep(0.3)
    before = door()
    g.cmd("hider 1")
    try:
        g.cmd("aim 0.2 -0.3 0.1 @Door")
        time.sleep(0.3)
        st = g.stats()
        aimed = (st.get("hider"), st.get("aimValid"), st.get("scanned"), st.get("hideTarget"))
        g.cmd("ctrl 0 0 0 0 0 0 1 0 0")
        time.sleep(0.3)
        g.cmd("ctrl off")
        time.sleep(0.3)
        gone = door()
    finally:
        g.cmd("aim off")
        g.cmd("hider 0")
    g.cmd("hider show")
    time.sleep(0.3)
    back = door()
    g.cmd("set WorldScale 1.5")
    # Earlier gates may have walked or turned the player; the pixels only count when the door was in view.
    seen = before > 2000
    check("16 hide objects", aimed[3] == "Door" and (not seen or (gone < before * 0.05 and back > before * 0.9)),
          f"door pixels {before} -> hidden {gone} -> shown {back}; tool, beam, renderers, target {aimed}")


def gate_persist(g):
    value, bound = g.cmd("get WorldScale"), g.cmd("get A")
    g.cmd("set WorldScale 1")
    g.cmd("set A Space")
    check("9 menu persist", value.startswith("1.5") and bound == "F", f"WorldScale after restart {value}, A bound to {bound}")


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
    ap.add_argument("--secondary", action="store_true", help="put test windows on a second monitor, off the main screen")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    build = os.path.join(ROOT, "fixtures", args.fixture, "Build")
    exe = os.path.join(build, args.fixture + ".exe")
    window_args, hmd_window = placement(args.secondary)

    before = tree_hash(build)
    subprocess.run([APP, "install", build], check=True)
    try:
        with steamvr.null_driver(hmd_window), windows.keep_on(hmd_window, test_processes(exe), not args.input):
            g = Game(exe, window_args)
            try:
                g.start()
                # IL2CPP first launch generates interop assemblies before plugins load.
                g.wait(lambda s: s["state"] == 3 and s["stereo"] > 10, 400, "stereo frames")
                gates = [gate_stereo, gate_mirror, gate_tracking, gate_timing, gate_ui, gate_pointer, gate_theater, gate_screen, gate_window, gate_menu, gate_render_scale]
                if args.input:
                    gates.insert(5, gate_input)
                else:
                    print("SKIP 7 input: pass --input to run it (takes over mouse and keyboard)", flush=True)
                if args.fixture in PAD_FIXTURES:
                    gates.insert(4, gate_pad)
                gates.append(gate_vignette)
                gates.append(gate_beam)
                gates.append(gate_wrist)
                gates.append(gate_hide)
                if args.fixture in PAD_FIXTURES:
                    gates.append(gate_aim)
                    gates.append(gate_snap)  # last: they turn the player away from the door
                if args.fixture in POST_FIXTURES:
                    gates.insert(1, gate_post)
                if os.environ.get("GATES"):  # a quick run of some gates while developing: GATES=gate_aim,gate_beam
                    gates = [x for x in gates if x.__name__ in os.environ["GATES"].split(",")]
                for gate in gates:
                    try:
                        gate(g)
                    except Exception as ex:
                        check(gate.__name__, False, repr(ex) + " at line " + str(ex.__traceback__.tb_next.tb_lineno if ex.__traceback__.tb_next else "?"))
                gate_soak(g, args.soak, os.path.join(build, "BepInEx", "LogOutput.log"))
                g.stop()
                g.start()
                g.wait(lambda s: s["state"] == 3, 120, "restart")
                gate_persist(g)
                gate_recovery(g)
            finally:
                g.stop()
    finally:
        log = os.path.join(build, "BepInEx", "LogOutput.log")
        if os.path.exists(log):
            shutil.copy2(log, os.path.join(OUT, f"LogOutput-{args.fixture}.log"))  # uninstall deletes it
        subprocess.run([APP, "uninstall", build], check=True)
    check("install round trip", tree_hash(build) == before, "game folder byte-identical after uninstall")
    with open(os.path.join(OUT, "results.json"), "w") as f:
        json.dump(RESULTS, f, indent=2)
    sys.exit(0 if all(r["pass"] for r in RESULTS.values()) else 1)


if __name__ == "__main__":
    main()
