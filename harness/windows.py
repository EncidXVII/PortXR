"""Keeps test windows off the user's main screen while a run is in progress.

Games and SteamVR open windows wherever they like (SteamVR's status window, the null HMD window, players that ignore
-monitor) and can take focus. A background thread moves every visible top-level window of the watched processes onto
the secondary monitor without activating it, and hands focus back to whatever the user had in front.
"""
import contextlib
import ctypes
import ctypes.wintypes as wt
import os
import threading

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE = 0x1, 0x4, 0x10
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
ENUM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def process_name(pid):
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(260)
        size = wt.DWORD(260)
        kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
        return os.path.basename(buf.value).lower()
    finally:
        kernel32.CloseHandle(h)


@contextlib.contextmanager
def keep_on(screen, names, return_focus=True):
    """screen: (x, y, w, h) target monitor; names: lowercase exe names whose windows get moved.
    return_focus=False for the input gate, which needs the game in front."""
    if screen is None:
        yield
        return
    user_window = user32.GetForegroundWindow()
    stop = threading.Event()

    def visit(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if process_name(pid.value) not in names:
            return True
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        x, y, w, h = screen
        if not (x <= r.left < x + w and y <= r.top < y + h):
            user32.SetWindowPos(hwnd, None, x + 16, y + 16, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
        return True

    def loop():
        callback = ENUM(visit)
        while not stop.wait(0.1):
            user32.EnumWindows(callback, 0)
            fg = user32.GetForegroundWindow()
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(fg, ctypes.byref(pid))
            if return_focus and user_window and fg != user_window and process_name(pid.value) in names:
                # A synthetic Alt press lifts the foreground lock so focus can be returned.
                user32.keybd_event(0x12, 0, 0, 0)
                user32.SetForegroundWindow(user_window)
                user32.keybd_event(0x12, 0, 2, 0)

    user32.SetProcessDPIAware()
    thread = threading.Thread(target=loop, daemon=True)
    thread.start()
    try:
        yield
    finally:
        stop.set()
        thread.join()
