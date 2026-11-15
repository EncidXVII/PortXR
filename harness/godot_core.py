"""Godot on the shared VR core: fixture G1 with the Godot adapter (godot/core) and parallax_godot.dll, on Direct3D 12,
SteamVR null driver. Checks what the compositor shows the test headset and the shared behaviour through the adapter's
test channel. No real keyboard or mouse input: controllers are faked in the core and the gamepad goes into the game.

    python harness/godot_core.py [--secondary]
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

import run
import steamvr
import windows
from godot import GODOT, G1, ROOT, headset_view

OUT = os.path.join(run.OUT, "g1core")
PACK = os.path.join(OUT, "ParallaxVR")
PORT = 47812
results = []


def check(name, ok, detail):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)


class Channel:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(1.0)

    def drain(self):
        # A reply to a resent command can arrive late and would be read as the answer to the next one.
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recv(65536)
        except (BlockingIOError, ConnectionResetError, OSError):
            pass
        finally:
            self.sock.settimeout(1.0)

    def cmd(self, text):
        self.drain()
        for _ in range(5):
            self.sock.sendto(text.encode(), ("127.0.0.1", PORT))
            try:
                return json.loads(self.sock.recvfrom(65536)[0])
            except (socket.timeout, ConnectionResetError, ValueError):
                time.sleep(0.5)
        raise RuntimeError(f"no reply to {text}")

    def wait(self, test, seconds, what):
        until = time.time() + seconds
        while time.time() < until:
            try:
                s = self.cmd("stats")
                if test(s):
                    return s
            except RuntimeError:
                pass
            time.sleep(0.5)
        raise RuntimeError(f"timed out waiting for {what}")


def angle(a, b):
    return (b - a + 180) % 360 - 180


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secondary", action="store_true")
    args = ap.parse_args()
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(PACK)
    build = os.path.join(ROOT, "build", "native", "Release")
    for f in ("parallax_godot.dll", "openxr_loader.dll"):
        shutil.copy2(os.path.join(build, f), PACK)
    for f in ("parallax.gd", "parallax.gdextension"):
        shutil.copy2(os.path.join(ROOT, "godot", "core", f), PACK)
    # This game's settings, as the app writes them on Add VR; the tests change them through the core.
    with open(os.path.join(PACK, "parallax.cfg"), "w") as f:
        f.write("[Controls]\nMode = Gamepad\nHeadAim = Off\nAimWith = Head\n\n[Comfort]\nTurning = Smooth\n\n[Menu]\nFirstRun = false\n")
    if not os.path.isdir(os.path.join(G1, ".godot")):
        subprocess.run([GODOT, "--headless", "--path", G1, "--editor", "--quit"], capture_output=True, timeout=300)
    with open(os.path.join(G1, "override.cfg"), "w") as f:
        f.write('[rendering]\n\nrendering_device/driver.windows="d3d12"\n\n'
                f'[autoload]\n\nParallaxVR="*{os.path.join(PACK, "parallax.gd").replace(os.sep, "/")}"\n')
    screen = steamvr.secondary_monitor() if args.secondary else None
    _, hmd = run.placement(secondary=args.secondary)
    x, y = (screen[0] + 40, screen[1] + 40) if screen else (40, 40)
    env = dict(os.environ, XR_RUNTIME_JSON=steamvr.runtime_json(), PARALLAX_TEST_PORT=str(PORT),
               PARALLAX_LOG_FILE=os.path.join(OUT, "core.log"))
    log = os.path.join(OUT, "stdout.log")
    names = {os.path.basename(GODOT).lower()} | {p + ".exe" for p in steamvr.PROCESSES}
    ch = Channel()
    try:
        with steamvr.null_driver(hmd, {"dashboard": {"enableDashboard": False}}), windows.keep_on(screen, names), open(log, "w") as out:
            game = subprocess.Popen([GODOT, "--path", G1, "--windowed", "--position", f"{x},{y}"] + (["--screen", "1"] if screen else []),
                                    stdout=out, stderr=subprocess.STDOUT, env=env)
            try:
                s = ch.wait(lambda s: s["state"] == 3 and s["camera"].endswith("Player") and s["frames"] > 200, 90, "the core running")
                check("core running", True, f"session running, camera {s['camera']}")
                time.sleep(3)
                img = headset_view(os.path.join(OUT, "headset.png"))
                if img is None:
                    check("headset", False, "no Headset Window")
                else:
                    r, g, b = img[..., 0], img[..., 1], img[..., 2]
                    red = float(((r > 180) & (g < 70) & (b < 70)).mean())
                    green = float(((g > 180) & (r < 70) & (b < 70)).mean())
                    yellow = int(((r > 180) & (g > 180) & (b < 80)).sum())
                    check("world", red > 0.003 and green > 0.003, f"red box {red:.1%}, green box {green:.1%}: eyes placed by the core")
                    check("ui panel", yellow > 0, f"crosshair pixels {yellow}: the game's UI on the core's panel")

                # Laser pointer: the fake right hand points at G1's PLAY button on the UI panel; the trigger clicks it
                # through Godot's own input (the real cursor is not touched).
                ch.cmd('controllers {"rt": 0.0}')
                ch.cmd("point 0.5 0.667")
                s = ch.wait(lambda s: s["pointer"][0] >= 0, 5, "the pointer on the panel")
                time.sleep(0.5)  # the pointer eases onto the button
                ch.cmd('controllers {"rt": 1.0}')
                time.sleep(0.5)
                ch.cmd('controllers {"rt": 0.0}')
                s = ch.wait(lambda s: s["game"].get("clicks", 0) >= 1, 5, "PLAY clicked by the laser")
                check("laser pointer", s["game"].get("captured", False), f"the laser at {s['pointer']} clicked PLAY")
                ch.cmd("point -1 0")
                ch.cmd('controllers {}')

                # Gamepad mode: the core's emulated pad reaches the game as pad 0.
                ch.cmd('controllers {"buttons": 1}')
                s = ch.wait(lambda s: 0 in s["game"].get("buttons", []), 5, "pad A in the game")
                ch.cmd('controllers {}')
                check("gamepad", True, f"A on the right controller reached the game as pad button A ({s['game']['buttons']})")

                # Head aim: the core steers the game camera back to where the head looks (G1 is playing now: PLAY
                # captured the mouse, and it looks with the stick only then, as first-person games do).
                ch.cmd("set Controls/HeadAim On")
                s = ch.wait(lambda s: s["aiming"], 10, "aiming")
                start = s["heading"]
                ch.cmd("turn 40")
                turned = ch.cmd("stats")["heading"]
                s = ch.wait(lambda s: abs(angle(s["heading"], start)) < 3, 8, "aim back on the head")
                check("head aim", abs(angle(turned, start)) > 5, f"camera turned {angle(start, turned):+.0f}, steered back to {angle(start, s['heading']):+.1f} deg of the head")

                # Snap turning: a flick of the right stick turns the body by 30 degrees (aim on: the body turns, aim follows).
                ch.cmd("set Comfort/Turning Snap30")
                before = ch.cmd("stats")["heading"]
                ch.cmd('controllers {"rx": 1.0}')
                time.sleep(0.4)
                ch.cmd('controllers {}')
                s = ch.wait(lambda s: abs(abs(angle(before, s["heading"])) - 30) < 3, 6, "a 30 degree snap")
                check("snap turn", True, f"one flick turned the camera {angle(before, s['heading']):+.1f} deg")

                # The VR menu, drawn by the core: opens over the panel, the laser picks Theater in Quick's Display row
                # (written to this game's settings), captured as the headset gets it.
                ch.cmd("menu")
                s = ch.wait(lambda s: s["menu_open"], 5, "the menu open")
                time.sleep(0.5)
                cap = os.path.join(OUT, "menu")
                os.makedirs(cap, exist_ok=True)
                ch.cmd("capture " + cap)
                time.sleep(1)
                from run import load_bmp
                shot = os.path.join(cap, "menu.bmp")
                if os.path.exists(shot):
                    m = load_bmp(shot)
                    dark = float(((m[..., 3] > 200) & (m[..., :3].max(axis=2) < 30)).mean())
                    ink = int((m[..., :3].min(axis=2) > 200).sum())
                    check("menu drawn", dark > 0.5 and ink > 2000, f"menu layer {m.shape[1]}x{m.shape[0]}: {dark:.0%} panel, {ink} ink pixels of text")
                else:
                    check("menu drawn", False, "no menu.bmp captured")
                ch.cmd('controllers {"rt": 0.0}')
                ch.cmd("point 0.823 0.182")
                time.sleep(0.4)
                ch.cmd('controllers {"rt": 1.0}')
                time.sleep(0.3)
                ch.cmd('controllers {"rt": 0.0}')
                time.sleep(0.5)
                v = ch.cmd("setting View/Display")["value"]
                check("menu click", v == "Theater", f"the laser on Display > Theater set View/Display = {v!r} in the game's settings")
                ch.cmd("set View/Display Stereo")
                # Every tab, clicked by the laser and captured as the headset gets it (tab words on the tab row).
                tabs = {"quick": 0.075, "view": 0.157, "controls": 0.264, "game": 0.373, "library": 0.47, "diagnostics": 0.61}
                drawn = []
                for name, u in tabs.items():
                    ch.cmd(f"point {u} 0.076")
                    time.sleep(0.3)
                    ch.cmd('controllers {"rt": 1.0}')
                    time.sleep(0.2)
                    ch.cmd('controllers {"rt": 0.0}')
                    ch.cmd(f"point 0.5 0.98")  # off the controls, so no hover ring in the picture
                    time.sleep(0.8)
                    ch.cmd("capture " + cap)
                    time.sleep(0.8)
                    if os.path.exists(shot):
                        m = load_bmp(shot)
                        from PIL import Image
                        Image.fromarray(m[..., :3].astype("uint8")).save(os.path.join(OUT, f"menu-{name}.png"))
                        drawn.append(int((m[..., :3].min(axis=2) > 200).sum()))
                        os.remove(shot)
                check("menu tabs", len(drawn) == 6 and min(drawn) > 500, f"six tabs captured, ink pixels {drawn}")
                ch.cmd("point -1 0")
                ch.cmd('controllers {}')
                ch.cmd("menu")

                def colours(img):
                    r, g, b = img[..., 0], img[..., 1], img[..., 2]
                    return float(((r > 180) & (g < 70) & (b < 70)).mean()), float(((g > 180) & (r < 70) & (b < 70)).mean())

                # Theater: the game draws its flat picture, the core shows it on the (curved where it can) screen.
                ch.cmd("set View/Display Theater")
                ch.wait(lambda s: s["mode"] == 2, 5, "Theater")
                time.sleep(2)
                img = headset_view(os.path.join(OUT, "theater.png"))
                red, green = colours(img) if img is not None else (0, 0)
                check("theater", red > 0.001 and green > 0.001, f"the flat game on the screen: red {red:.2%}, green {green:.2%} of the view")

                # Parallax: two window cameras into the game, a little apart, on the screen.
                ch.cmd("set View/Display Parallax")
                ch.wait(lambda s: s["mode"] == 1, 5, "Parallax")
                time.sleep(2)
                img = headset_view(os.path.join(OUT, "parallax.png"))
                red, green = colours(img) if img is not None else (0, 0)
                ch.cmd("capture " + cap)
                time.sleep(1)
                wl, wr = os.path.join(cap, "windowL.bmp"), os.path.join(cap, "windowR.bmp")
                differ = 0.0
                if os.path.exists(wl) and os.path.exists(wr):
                    a_, b_ = load_bmp(wl), load_bmp(wr)
                    differ = float(abs(a_[..., :3].astype(int) - b_[..., :3].astype(int)).mean())
                check("parallax", red > 0.001 and green > 0.001 and differ > 0.5,
                      f"the window on the screen: red {red:.2%}, green {green:.2%}; the eyes' windows differ by {differ:.2f}")

                # Screen handles: Theater, the menu's Screen set to Unlocked, the bar dragged to the right, then RESET.
                ch.cmd("set View/Display Theater")
                ch.wait(lambda s: s["mode"] == 2, 5, "Theater")
                ch.cmd("menu")
                ch.wait(lambda s: s["menu_open"], 5, "the menu open")
                ch.cmd('controllers {"rt": 0.0}')
                ch.cmd("point 0.075 0.076")  # the Quick tab: the menu reopens on the tab it was left on
                time.sleep(0.4)
                ch.cmd('controllers {"rt": 1.0}')
                time.sleep(0.2)
                ch.cmd('controllers {"rt": 0.0}')
                time.sleep(0.3)
                ch.cmd("point 0.778 0.273")
                time.sleep(0.4)
                ch.cmd('controllers {"rt": 1.0}')
                time.sleep(0.2)
                ch.cmd('controllers {"rt": 0.0}')
                time.sleep(0.5)
                ch.cmd("capture " + cap)
                time.sleep(1)
                if os.path.exists(shot):
                    from PIL import Image
                    Image.fromarray(load_bmp(shot)[..., :3].astype("uint8")).save(os.path.join(OUT, "menu-theater.png"))
                hb = os.path.join(cap, "handles.bmp")
                shown = os.path.exists(hb) and int((load_bmp(hb)[..., 3] > 50).sum()) > 500
                ch.cmd("point_screen 0.5 0.967")
                time.sleep(0.4)
                ch.cmd('controllers {"rt": 1.0}')
                for u in (0.55, 0.6, 0.65, 0.7):
                    time.sleep(0.15)
                    ch.cmd(f"point_screen {u} 0.967")
                time.sleep(0.3)
                ch.cmd('controllers {"rt": 0.0}')
                time.sleep(0.5)
                moved = ch.cmd("setting View/ScreenYaw")["value"]
                check("screen handles", shown and moved not in ("", "0", "0.0"),
                      f"handles {'drawn' if shown else 'missing'}; dragging the bar turned the screen to ScreenYaw = {moved}")
                ch.cmd("point_screen 0.69 0.967")  # RESET, right of the bar (16:9)
                time.sleep(0.4)
                ch.cmd('controllers {"rt": 1.0}')
                time.sleep(0.2)
                ch.cmd('controllers {"rt": 0.0}')
                time.sleep(0.5)
                back = ch.cmd("setting View/ScreenYaw")["value"]
                check("screen reset", back in ("0", "0.0"), f"RESET put ScreenYaw back to {back}")
                ch.cmd("point -1 0")
                ch.cmd('controllers {}')
                ch.cmd("menu")
                ch.cmd("set View/Display Stereo")
            finally:
                game.terminate()
                game.wait(20)
    except RuntimeError as e:
        check("run", False, str(e))
    finally:
        os.remove(os.path.join(G1, "override.cfg"))
    print("\n".join(l for l in open(log, encoding="utf-8", errors="replace").read().splitlines() if "SCRIPT ERROR" in l or "Crash" in l)[:2000])
    print(f"{sum(results)}/{len(results)} passed", flush=True)
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()
