"""Smoke-runs installed games under the null driver and records a tier guess in compat/results.csv.

A: stereo frames with a non-black image; C: only the theater/UI layer reached the headset; Fail: no session or crash.
B needs a human (quirks/profiles), so the harness never guesses it.
"""
import argparse
import csv
import os
import re
import subprocess
import time

import numpy as np

import run
import steamvr
import windows

RESULTS = os.path.join(run.ROOT, "compat", "results.csv")


def app_id(game_dir):
    steamapps = os.path.dirname(os.path.dirname(game_dir))
    for name in os.listdir(steamapps):
        if name.startswith("appmanifest_"):
            text = open(os.path.join(steamapps, name), encoding="utf-8", errors="replace").read()
            if re.search(r'"installdir"\s+"%s"' % re.escape(os.path.basename(game_dir)), text, re.I):
                return re.search(r'"appid"\s+"(\d+)"', text).group(1)
    return None


def find_exe(game_dir):
    for name in os.listdir(game_dir):
        stem, ext = os.path.splitext(name)
        if ext.lower() == ".exe" and os.path.isdir(os.path.join(game_dir, stem + "_Data")):
            return os.path.join(game_dir, name)
    raise RuntimeError("no Unity exe")


def smoke(game_dir, seconds):
    exe = find_exe(game_dir)
    # Without steam_appid.txt Steam games relaunch through the client and lose the harness environment.
    appid_file = os.path.join(game_dir, "steam_appid.txt")
    added_appid = not os.path.exists(appid_file)
    before = run.tree_hash(game_dir)
    subprocess.run([run.APP, "install", game_dir], check=True)
    if added_appid:
        with open(appid_file, "w") as f:
            f.write(app_id(game_dir) or "")
    tier, detail = "Fail", ""
    try:
        window_args, hmd_window = run.placement(primary=False)
        with steamvr.null_driver(hmd_window), windows.keep_on(hmd_window, run.test_processes(exe)):
            g = run.Game(exe, window_args)
            g.log = os.path.join(run.OUT, "compat_fixture.log")
            g.start()
            try:
                s = g.wait(lambda s: s["state"] == 3 and s["quad"] > 30, 180, "frames")
                time.sleep(seconds)
                s = g.stats()
                if s["stereo"] > 30:
                    (left,) = g.capture("compat", ["left.bmp"])
                    lit = left[..., :3].mean()
                    tier = "A" if lit > 8 and left[..., :3].std() > 4 else "C"
                    detail = f"stereo {s['stereo']} frames, camera '{s['cam']}', mean {lit:.0f}"
                else:
                    tier, detail = "C", f"no stereo (camera '{s['cam']}'), quad {s['quad']} frames"
            except Exception as ex:
                detail = repr(ex)[:200]
            finally:
                g.stop()
    finally:
        if added_appid and os.path.exists(appid_file):
            os.remove(appid_file)
        subprocess.run([run.APP, "uninstall", game_dir], check=True)
    restored = run.tree_hash(game_dir) == before
    return tier, detail, restored


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("games", nargs="+", help="game folders")
    ap.add_argument("--seconds", type=int, default=20)
    args = ap.parse_args()
    os.makedirs(run.OUT, exist_ok=True)
    rows = []
    for d in args.games:
        tier, detail, restored = smoke(d, args.seconds)
        print(f"{tier:4} {os.path.basename(d)}: {detail}; folder restored {restored}", flush=True)
        rows.append([os.path.basename(d), tier, detail, restored])
    with open(RESULTS, "a", newline="") as f:
        csv.writer(f).writerows(rows)


if __name__ == "__main__":
    main()
