using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Parallax
{
    // Mirrors native/src/common.h field for field.
    [StructLayout(LayoutKind.Sequential)]
    internal struct Pose { public float px, py, pz, qx, qy, qz, qw; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Fov { public float left, right, up, down; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Layer { public IntPtr texture; public int flipY; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Frame
    {
        public int stereo;
        public Layer eyeL, eyeR;
        public Pose poseL, poseR;
        public Fov fovL, fovR;
        public int quad;
        public int quadFromBackbuffer;
        public Layer quadLayer;
        public Pose quadPose;
        public float quadWidth, quadHeight;
        public int quadAlpha;
        public int mirror;
        public float quadRadius;
        public float vignette;
        public int beam;
        public float fromX, fromY, fromZ, toX, toY, toZ;
        public int watch;
        public float watchLit;
        public float watchX, watchY, watchZ, watchRadius, watchAlpha;
        public int uiKey, scene;
        public float keepX0, keepY0, keepX1, keepY1;
        public int window;
        public Layer windowL, windowR;
        public float aimLX, aimLY, aimLR, aimRX, aimRY, aimRR;
        public float patchW, patchH;
        public int patchShown;
        public Pose aimPose;
        public float aimW, aimH;
        public float accentR, accentG, accentB;
        public int passthrough;
    }

    [Flags]
    internal enum Buttons : uint { A = 1, B = 2, X = 4, Y = 8, LStick = 16, RStick = 32, Menu = 64 }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Controllers
    {
        public Buttons buttons;
        public float lx, ly, rx, ry;
        public float lTrigger, rTrigger, lGrip, rGrip;
        public Pose aimL, aimR;
        public int aimValidL, aimValidR;
    }

    // XINPUT_GAMEPAD layout.
    [StructLayout(LayoutKind.Sequential)]
    internal struct Pad
    {
        public ushort buttons;
        public byte leftTrigger, rightTrigger;
        public short lx, ly, rx, ry;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct InputConfig
    {
        public int mode, neutral, pointerActive, pointerX, pointerY;
        public float mouseSpeed;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 12)] public int[] binds;
        public int snap;
        public float turnX;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10)] public int[] twice;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10)] public int[] held;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10)] public int[] combo;
        public int aim;
        public float turnY;
        public int pointerClick;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Stats
    {
        public int state, focused;
        public double periodMs, p95Ms;
        public ulong frames, submitted, quadSubmitted, stereoSubmitted, curvedSubmitted;
        public int curvedSupported;
    }

    // The core's frame (native/src/bridge.cpp PrxGame and PrxRig): what Unity knows about the game this frame, and the
    // player's rig back, shared with every engine. OpenXR handedness: Xr mirrors Z.
    [StructLayout(LayoutKind.Sequential)]
    internal struct Game
    {
        public int hasCamera;
        public Pose camera;
        [MarshalAs(UnmanagedType.LPStr)] public string cameraKey;
        public int cursorFree, padGame, carried, tests;
        public float uiAspect, fovY, nearClip, focus;
        public float upX, upY, upZ, sceneScale, offsetX, offsetY, offsetZ;
        public int worldPointer;
        public float worldReach;
        public int screenW, screenH, pointerClick;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RigOut
    {
        public Pose eyeL, eyeR, viewL, viewR;
        public Fov fovL, fovR;
        public int mode, aiming, events, menuOpen;
        public int pointer, pointerDown;
        public float pointerU, pointerV;
        public int quit, renderScale, cameraPicked, hideObjects, showHidden;
        public float windowOffsetL, windowOffsetR;
        public Fov windowFovL, windowFovR;
        public int handAiming;
        public Pose handAim;
        public Pose origin;
        public float bodyX, bodyY, bodyZ, bodyW, baseX, baseY, baseZ, scale;
        public Controllers controllers;
        public int handles, handle, grab;
        public float screenYaw, screenPitch, screenDistance, screenWidth;
        public float vignette, focus, bodyYaw, orbitRadius;
        public int smoothing, glancing, watch, beam;
    }

    internal enum XrState { Failed = -1, None = 0, Instance = 1, Session = 2, Running = 3 }

    internal static class Native
    {
        const string Dll = "parallax_xr";
        public const int EvInit = 1, EvBegin = 2, EvSubmit = 3, EvShutdown = 4, EvCaptureBackbuffer = 5;
        public const uint EvMenuToggle = 1;

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibrary(string path);

        // Mono only probes the game's plugin folders; loading by full path first makes the DllImports bind to it.
        public static bool Load(string dir) => LoadLibrary(Path.Combine(dir, Dll + ".dll")) != IntPtr.Zero;

        [DllImport(Dll)] public static extern int prx_init([MarshalAs(UnmanagedType.LPStr)] string appName, int api, out int width, out int height);
        [DllImport(Dll)] public static extern int prx_state();
        [DllImport(Dll)] public static extern int prx_ui_keyed();
        [DllImport(Dll)] public static extern int prx_passthrough();
        [DllImport(Dll)] public static extern IntPtr prx_render_event_func();
        [DllImport(Dll)] public static extern void prx_set_init_texture(IntPtr texture);
        [DllImport(Dll)] public static extern void prx_set_frame(int slot, ref Frame frame);
        [DllImport(Dll)] public static extern void prx_capture([MarshalAs(UnmanagedType.LPWStr)] string dir);
        [DllImport(Dll)] public static extern void prx_get_stats(out Stats stats);
        [DllImport(Dll)] public static extern int prx_install_hooks();
        [DllImport(Dll)] public static extern uint prx_input_update(ref InputConfig cfg, [In] Controllers[] test, out Controllers result);
        [DllImport(Dll)] public static extern int prx_pad_state(out Pad pad);
        [DllImport(Dll)] public static extern int prx_key_down(int vk);
        [DllImport(Dll)] public static extern int prx_game_in_front();
        [DllImport(Dll)] public static extern void prx_haptic_pulse(int hand, float amplitude, float seconds);
        [DllImport(Dll)] static extern int prx_poll_log(byte[] buffer, int capacity);
        [DllImport(Dll)] public static extern int prx_bridge_frame(ref Game game, out RigOut rig);
        [DllImport(Dll)] public static extern void prx_bridge_layers(ref Frame frame, float uiAspect);
        [DllImport(Dll)] public static extern void prx_bridge_recenter();
        [DllImport(Dll)] public static extern void prx_bridge_test([In] Controllers[] controllers, float u, float v, int wrist, float[] head);
        [DllImport(Dll)] public static extern void prx_settings_touch();

        static readonly byte[] logBuffer = new byte[1024];

        public static string PollLog()
        {
            if (prx_poll_log(logBuffer, logBuffer.Length) == 0) return null;
            int n = Array.IndexOf(logBuffer, (byte)0);
            return Encoding.UTF8.GetString(logBuffer, 0, n < 0 ? logBuffer.Length : n);
        }

    }
}
