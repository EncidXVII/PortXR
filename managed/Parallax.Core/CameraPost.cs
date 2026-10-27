using System;
using System.Linq;
using UnityEngine;
#if IL2CPP
using Il2CppInterop.Runtime;
#endif

namespace Parallax
{
    // Pipeline post-processing writes alpha 1 over the whole screen, so a hidden camera that still runs it turns the
    // see-through UI panel opaque. Off() switches it off on one camera (URP post and AA, the PPv2 layer's volumes and
    // AA) and returns the undo, or null when nothing was on. All are plain fields, cheap to flip back for the eyes'
    // settings copy. The PPv2 layer itself stays enabled: disabling it drops its effect bundles, which games query
    // through Camera.main (Risk of Rain 2's loading stopped at 99% on that). Members are reached by name: the plugin
    // never links pipeline assemblies. On IL2CPP the game's types are interop proxies, built around the component's pointer.
    internal static class CameraPost
    {
        static readonly string[] TypeNames =
        {
            "UnityEngine.Rendering.Universal.UniversalAdditionalCameraData",
            "UnityEngine.Rendering.PostProcessing.PostProcessLayer",
            "UnityEngine.Rendering.HighDefinition.HDAdditionalCameraData",
        };
        static readonly string[] LayerMembers = { "volumeLayer", "antialiasingMode" }, UrpMembers = { "renderPostProcessing", "antialiasing" };
        static Type[] types;
        static bool off;

        public static Action Off(Camera cam)
        {
            if (off) return null;
            Action undo = null;
            try
            {
                types = types ?? TypeNames.Select(n => AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetType(n, false))
                    .FirstOrDefault(t => t != null)).Where(t => t != null).ToArray();
                foreach (var type in types)
                {
                    bool layer = type.Name == "PostProcessLayer";
#if IL2CPP
                    var c = cam.GetComponent(Il2CppType.From(type));
                    object target = c == null ? null : Activator.CreateInstance(type, c.Pointer);
#else
                    object target = cam.GetComponent(type);
                    if ((Component)target == null) target = null;
#endif
                    if (target == null) continue;
                    // The PPv2 layer draws no volumes and no AA; URP keeps its camera data and loses post and AA. HDRP draws
                    // its sky whatever the culling mask, so its camera clears to transparent colour instead (Color = 1).
                    if (type.Name == "HDAdditionalCameraData")
                    {
                        undo += Set(cam, type, target, "clearColorMode", t => Enum.ToObject(t, 1));
                        undo += Set(cam, type, target, "backgroundColorHDR", t => Color.clear);
                        continue;
                    }
                    foreach (var name in layer ? LayerMembers : UrpMembers)
                        undo += Set(cam, type, target, name, t => t == typeof(bool) ? false : t == typeof(LayerMask) ? (object)(LayerMask)0 : Enum.ToObject(t, 0));
                }
            }
            catch (Exception ex)
            {
                off = true;
                Plugin.Log.LogWarning($"Post-processing switch is unavailable in this game ({ex.Message}); the UI panel may be opaque.");
            }
            return undo;
        }

        // A URP camera that draws overlay cameras in its stack (their output lands in its target, not on the screen).
        public static bool HasStack(Camera cam)
        {
            try
            {
                var type = types?.FirstOrDefault(t => t.Name == "UniversalAdditionalCameraData");
                if (type == null) return false;
#if IL2CPP
                var c = cam.GetComponent(Il2CppType.From(type));
                object target = c == null ? null : Activator.CreateInstance(type, c.Pointer);
#else
                object target = cam.GetComponent(type);
                if ((Component)target == null) target = null;
#endif
                var stack = target == null ? null : type.GetProperty("cameraStack")?.GetValue(target, null);
                var count = stack?.GetType().GetProperty("Count")?.GetValue(stack, null);
                return count is int n && n > 0;
            }
            catch (Exception)
            {
                return true;
            }
        }

        // Sets a property or field to `value(its type)`; returns the undo, or null when it already held that value.
        static Action Set(Camera cam, Type type, object target, string name, Func<Type, object> value)
        {
            var p = type.GetProperty(name);
            var f = p == null ? type.GetField(name) : null;
            if ((p == null || !p.CanWrite) && f == null) return null;
            var memberType = p != null ? p.PropertyType : f.FieldType;
            object was = p != null ? p.GetValue(target, null) : f.GetValue(target), want = value(memberType);
            if (Equals(was, want)) return null;
            Action<object> write = v =>
            {
                if (p != null) p.SetValue(target, v, null);
                else f.SetValue(target, v);
            };
            write(want);
            return () =>
            {
                if (cam != null) write(was);
            };
        }
    }
}
