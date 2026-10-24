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

    [StructLayout(LayoutKind.Sequential)]
    internal struct InputConfig
    {
        public int mode, neutral, pointerActive, pointerX, pointerY;
        public float mouseSpeed;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Stats
    {
        public int state, focused;
        public double periodMs, p95Ms;
        public ulong frames, submitted, quadSubmitted, stereoSubmitted;
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
        [DllImport(Dll)] public static extern IntPtr prx_render_event_func();
        [DllImport(Dll)] public static extern void prx_set_init_texture(IntPtr texture);
        [DllImport(Dll)] public static extern void prx_set_frame(int slot, ref Frame frame);
        [DllImport(Dll)] public static extern void prx_capture([MarshalAs(UnmanagedType.LPWStr)] string dir);
        [DllImport(Dll)] public static extern int prx_locate([Out] Pose[] poses, [Out] Fov[] fovs);
        [DllImport(Dll)] public static extern void prx_get_stats(out Stats stats);
        [DllImport(Dll)] public static extern int prx_install_hooks();
        [DllImport(Dll)] public static extern uint prx_input_update(ref InputConfig cfg, [In] Controllers[] test, out Controllers result);
        [DllImport(Dll)] public static extern void prx_haptic_pulse(int hand, float amplitude, float seconds);
        [DllImport(Dll)] static extern int prx_poll_log(byte[] buffer, int capacity);

        static readonly byte[] logBuffer = new byte[1024];

        public static string PollLog()
        {
            if (prx_poll_log(logBuffer, logBuffer.Length) == 0) return null;
            int n = Array.IndexOf(logBuffer, (byte)0);
            return Encoding.UTF8.GetString(logBuffer, 0, n < 0 ? logBuffer.Length : n);
        }

        [DllImport("user32")] public static extern short GetAsyncKeyState(int vKey);
        [DllImport("user32")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32")] public static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    }
}
