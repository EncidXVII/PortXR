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
PROCESSES = ["vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup", "steamvr_room_setup"]


def steam_root():
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
        return os.path.normpath(winreg.QueryValueEx(k, "SteamPath")[0])


def runtime_json():
    return os.path.join(steam_root(), "steamapps", "common", "SteamVR", "steamxr_win64.json")


def secondary_monitor():
    """Physical-pixel rect (x, y, w, h) of the first non-primary monitor, or None; test windows go there."""
    import ctypes
    import ctypes.wintypes as wt
    user32 = ctypes.windll.user32
    user32.SetProcessDPIAware()
    rects = []

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]

    def cb(hmon, hdc, rect, data):
        info = MONITORINFO(cbSize=ctypes.sizeof(MONITORINFO))
        user32.GetMonitorInfoW(hmon, ctypes.byref(info))
        if not info.dwFlags & 1:  # MONITORINFOF_PRIMARY
            r = info.rcWork
            rects.append((r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True

    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)
    user32.EnumDisplayMonitors(None, None, proc(cb), 0)
    return rects[0] if rects else None


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
def null_driver(window=None):
    """window: (x, y, w, h) for the null HMD's desktop window, e.g. secondary_monitor()."""
    if running():
        raise RuntimeError("SteamVR is running; close it so its settings can be swapped safely")
    path = os.path.join(steam_root(), "config", "steamvr.vrsettings")
    backup = path + ".parallax-backup"
    chaperone = os.path.join(steam_root(), "config", "chaperone_info.vrchap")
    chaperone_backup = chaperone + ".parallax-backup"
    if os.path.exists(backup):
        # A previous run was killed before it could clean up: its backups are the user's real files.
        shutil.copy2(backup, path)
        if os.path.exists(chaperone_backup):
            shutil.move(chaperone_backup, chaperone)
        elif os.path.exists(chaperone):
            os.remove(chaperone)
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
        if window:
            x, y, w, h = window
            settings["driver_null"].update({"windowX": x, "windowY": y, "windowWidth": w, "windowHeight": h})
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
        # SteamVR writes its settings on exit, so it must be gone before the original is put back. A relaunch the
        # game or vrstartup had already begun can appear a moment later, so it must stay gone for a few seconds.
        for _ in range(5):
            stop()
            time.sleep(3)
            if not running():
                break
        shutil.copy2(backup, path)
        os.remove(backup)
        if had_chaperone:
            shutil.move(chaperone_backup, chaperone)
        elif os.path.exists(chaperone):
            os.remove(chaperone)
