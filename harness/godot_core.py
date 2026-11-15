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

    def cmd(self, text):
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
        f.write("[Controls]\nMode = Gamepad\nHeadAim = Off\nAimWith = Head\n\n[Comfort]\nTurning = Smooth\n")
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
                ch.cmd('controllers {"rt": 1.0}')
                time.sleep(0.3)
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
