# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

"""Draws the VR menu without a headset, straight from the core: every tab and the welcome card as PNGs.

    python harness/menu_shots.py OUT_DIR [--long]

--long fills the menu with the longest names real games bring (game, cameras), for checking that nothing runs out of
its box. The README's picture is OUT_DIR/vr-menu-0.png (the Quick tab).
"""
import ctypes
import os
import struct
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Host(ctypes.Structure):
    _fields_ = [("product", ctypes.c_char_p), ("gameDir", ctypes.c_wchar_p), ("cameras", ctypes.c_char_p),
                ("currentCamera", ctypes.c_char_p), ("hideObjects", ctypes.c_int32), ("hidden", ctypes.c_int32),
                ("aimExtras", ctypes.c_int32), ("uiBlack", ctypes.c_int32), ("eyeWidth", ctypes.c_int32),
                ("eyeHeight", ctypes.c_int32)]


def png(path, w, h, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    out = sys.argv[1]
    long = "--long" in sys.argv
    os.makedirs(out, exist_ok=True)
    core = ctypes.CDLL(os.path.join(ROOT, "build", "native", "Release", "portxr_xr.dll"))
    core.prx_menu_render.restype = ctypes.POINTER(ctypes.c_uint8)
    core.prx_menu_render.argtypes = [ctypes.c_float, ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)]
    core.prx_menu_settings(ctypes.c_wchar_p(os.path.join(tempfile.mkdtemp(), "game.cfg")))
    if long:
        cameras = b"MainCamera\nCinemachineBrain_PlayerFollowCamera_ThirdPerson_Shoulder_Right\nUI Camera (Overlay, Screen Space)"
        host = Host(b"The Legend of an Exceptionally Long Game Title: Definitive Remastered Edition", r"C:\Games\Long",
                    cameras, b"CinemachineBrain_PlayerFollowCamera_ThirdPerson_Shoulder_Right", 1, 128, 1, 1, 3840, 3744)
    else:
        host = Host(b"Outer Wilds", r"C:\Games\Outer Wilds", b"MainCamera\nMapCamera", b"MainCamera", 1, 3, 1, 1, 2468, 2740)
    core.prx_menu_host(ctypes.byref(host))
    w, h = ctypes.c_int32(), ctypes.c_int32()
    for page in [-1, 0, 1, 2, 3, 4, 5]:
        core.prx_menu_show(page, -1)
        for _ in range(90):  # past the opening and tab motion
            px = core.prx_menu_render(1 / 60, ctypes.byref(w), ctypes.byref(h))
        if not px or w.value <= 0:
            print("page", page, "drew nothing")
            continue
        n = w.value * h.value * 4
        rgba = bytes(ctypes.cast(px, ctypes.POINTER(ctypes.c_uint8 * n)).contents)
        name = "vr-menu-welcome.png" if page < 0 else f"vr-menu-{page}.png"
        png(os.path.join(out, name), w.value, h.value, rgba)
        print(name, w.value, h.value)
        if page == -1:
            core.prx_menu_show(-2, -1)  # close the card before the tabs


main()
