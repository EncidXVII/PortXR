using System;
using System.Linq;
using UnityEngine;
#if IL2CPP
using Il2CppInterop.Runtime;
#endif

namespace Parallax
{
    // Videos drawn on a camera's near or far plane (intros, cutscenes) only show through that camera, which the eyes
    // replace; while one plays, the view switches to the theater screen. VideoPlayer is found by name, so games
    // without the video module cost nothing; checked twice a second.
    internal static class VideoWatch
    {
        const int CameraFarPlane = 0, CameraNearPlane = 1;
        static Type type;
        static bool off, playing;
        static float nextCheck;

        public static bool Playing()
        {
            if (off || Time.unscaledTime < nextCheck) return playing && !off;
            nextCheck = Time.unscaledTime + 0.5f;
            try
            {
                type = type ?? AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetType("UnityEngine.Video.VideoPlayer", false))
                    .FirstOrDefault(t => t != null);
                if (type == null)
                {
                    off = true;
                    return false;
                }
                playing = false;
#if IL2CPP
                foreach (var o in UnityEngine.Object.FindObjectsOfType(Il2CppType.From(type)))
                    playing |= OnCameraPlane(Activator.CreateInstance(type, o.Pointer));
#else
                foreach (var o in UnityEngine.Object.FindObjectsOfType(type)) playing |= OnCameraPlane(o);
#endif
            }
            catch (Exception ex)
            {
                off = true;
                Plugin.Log.LogWarning($"Video detection is unavailable in this game ({ex.Message}); camera videos may stay hidden in stereo.");
            }
            return playing;
        }

        static bool OnCameraPlane(object player)
        {
            int mode = Convert.ToInt32(type.GetProperty("renderMode").GetValue(player, null));
            return (mode == CameraFarPlane || mode == CameraNearPlane) && (bool)type.GetProperty("isPlaying").GetValue(player, null)
                && (bool)type.GetProperty("isActiveAndEnabled").GetValue(player, null);
        }
    }
}
