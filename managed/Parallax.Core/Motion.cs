using System.Runtime.InteropServices;
using UnityEngine;

namespace Parallax
{
    // UI motion, as in DesktopVR: presses answer at once and ease back, hovers ease, things that travel (the tab bar, a
    // segment's choice) take an expo-out curve, the menu and the watch fade. All of it follows Windows' "Animation
    // effects" setting (Settings > Accessibility > Visual effects): off keeps every state change and drops the movement.
    internal static class Motion
    {
        internal const float Fast = 0.16f, Slow = 0.28f;

        [DllImport("user32", EntryPoint = "SystemParametersInfoW")]
        static extern int SystemParametersInfo(uint action, uint param, ref int value, uint ini);

        static int on = -1;

        internal static bool On
        {
            get
            {
                if (on < 0)
                {
                    int v = 1;
                    try
                    {
                        // SPI_GETCLIENTAREAANIMATION; on when the query fails.
                        on = SystemParametersInfo(0x1042, 0, ref v, 0) == 0 || v != 0 ? 1 : 0;
                    }
                    catch (System.Exception)
                    {
                        on = 1;
                    }
                }
                return on == 1;
            }
        }

        // Moves `v` toward `target` with time constant `tau` (seconds): fast at first, settling without overshoot.
        internal static float Follow(float v, float target, float tau, float dt)
        {
            if (!On || tau <= 0) return target;
            v += (target - v) * Mathf.Min(1f, dt / tau);
            return Mathf.Abs(target - v) < 0.002f ? target : v;
        }

        // 0..1 over `duration` from `start` (unscaled time), shaped by expo-out (cubic-bezier(0.23, 1, 0.32, 1)'s feel).
        internal static float Out(float start, float duration)
        {
            if (!On || duration <= 0) return 1f;
            float t = Mathf.Clamp01((Time.unscaledTime - start) / duration);
            return 1f - (1f - t) * (1f - t) * (1f - t) * (1f - t);
        }

        internal static float Smooth(float a, float b, float x)
        {
            float t = Mathf.Clamp01((x - a) / (b - a));
            return t * t * (3f - 2f * t);
        }
    }
}
