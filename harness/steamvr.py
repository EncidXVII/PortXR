"""SteamVR null-driver session: headless HMD for automated runs, user settings restored afterwards."""
import contextlib
import json
import os
import shutil
import subprocess
import time
import winreg

CHAPERONE = {
    "jsonid": "chaperone_info",
    "universes": [{
        "collision_bounds": [],
        "play_area": [2, 2],
        "seated": {"translation": [0, 0, 0], "yaw": 0},
        "standing": {"translation": [0, 0, 0], "yaw": 0},
        "time": "Thu Jan  1 00:00:00 2026",
        "universeID": universe,
    } for universe in (0, 1, 2)],
    "version": 5,
}
PROCESSES = ["vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup"]


def steam_root():
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
        return os.path.normpath(winreg.QueryValueEx(k, "SteamPath")[0])


def runtime_json():
    return os.path.join(steam_root(), "steamapps", "common", "SteamVR", "steamxr_win64.json")


def running():
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout.lower()
    return [p for p in PROCESSES if f'"{p}.exe"' in out]


def stop():
    for p in PROCESSES:
        subprocess.run(["taskkill", "/f", "/im", p + ".exe"], capture_output=True)
    for _ in range(50):
        if not running():
            return
        time.sleep(0.2)


@contextlib.contextmanager
def null_driver():
    if running():
        raise RuntimeError("SteamVR is running; close it so its settings can be swapped safely")
    path = os.path.join(steam_root(), "config", "steamvr.vrsettings")
    backup = path + ".parallax-backup"
    chaperone = os.path.join(steam_root(), "config", "chaperone_info.vrchap")
    chaperone_backup = chaperone + ".parallax-backup"
    had_chaperone = os.path.exists(chaperone)
    if had_chaperone:
        shutil.copy2(chaperone, chaperone_backup)
    shutil.copy2(path, backup)
    try:
        with open(path, encoding="utf-8") as f:
            settings = json.load(f)
        settings.setdefault("steamvr", {}).update({
            "forcedDriver": "null",
            "activateMultipleDrivers": True,
            "enableHomeApp": False,
            "requireHmd": True,
            "showMirrorView": False,
        })
        settings.setdefault("driver_null", {}).update({"enable": True, "displayFrequency": 90.0})
        # A motionless null HMD otherwise enters standby after seconds, which stops the session.
        settings.setdefault("power", {}).update({
            "pauseCompositorOnStandby": False,
            "turnOffScreensTimeout": 86400.0,
            "turnOffControllersTimeout": 0.0,
        })
        with open(path, "w", encoding="utf-8") as f:
            json.dump(settings, f, indent=3)
        # Without a calibrated play area SteamVR launches Room Setup as a scene app, which evicts the game.
        with open(chaperone, "w", encoding="utf-8") as f:
            json.dump(CHAPERONE, f, indent=3)
        yield
    finally:
        # SteamVR writes its settings on exit, so it must be gone before the original is put back.
        stop()
        shutil.copy2(backup, path)
        os.remove(backup)
        if had_chaperone:
            shutil.move(chaperone_backup, chaperone)
        elif os.path.exists(chaperone):
            os.remove(chaperone)
