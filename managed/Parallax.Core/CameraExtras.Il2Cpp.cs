#if IL2CPP
using System;
using System.Collections.Generic;
using System.Linq;
using Il2CppInterop.Runtime;
using UnityEngine;
using IlFlags = Il2CppSystem.Reflection.BindingFlags;
using IlType = Il2CppSystem.Type;

namespace Parallax
{
    // IL2CPP build of CameraExtras: interop proxies expose game fields as properties over native memory, so the copy
    // goes through IL2CPP's own reflection. Nested serializable classes are shared rather than cloned (IL2CPP games
    // often strip Activator); temporal AA, the one setting with per-camera history, is still switched off.
    internal static class CameraExtras
    {
        static readonly string[] TypeNames =
        {
            "UnityEngine.Rendering.Universal.UniversalAdditionalCameraData",
            "UnityEngine.Rendering.HighDefinition.HDAdditionalCameraData",
            "UnityEngine.Rendering.PostProcessing.PostProcessLayer",
        };
        const IlFlags Fields = IlFlags.Instance | IlFlags.Public | IlFlags.NonPublic | IlFlags.DeclaredOnly;

        static IlType[] types;
        static readonly HashSet<string> logged = new HashSet<string>();
        static bool off;

        public static void Copy(Camera from, Camera eye)
        {
            if (off) return;
            try
            {
                types = types ?? FindTypes();
                foreach (var type in types)
                {
                    var src = from.GetComponent(type);
                    if (src == null) continue;
                    var dst = eye.GetComponent(type);
                    if (logged.Add(type.FullName)) Plugin.Log.LogInfo($"Eye cameras inherit {type.Name} from {from.name}");
                    if (dst == null)
                    {
                        var go = eye.gameObject;
                        bool active = go.activeSelf;
                        go.SetActive(false);
                        dst = go.AddComponent(type);
                        CopyFields(src, dst, type);
                        go.SetActive(active);
                        continue;
                    }
                    CopyFields(src, dst, type);
                }
            }
            catch (Exception ex)
            {
                off = true;
                Plugin.Log.LogWarning($"Camera settings copy is unavailable in this game ({ex.Message}); eyes render without the game's post-processing.");
            }
        }

        static IlType[] FindTypes() =>
            TypeNames.Select(name => AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetType(name, false)).FirstOrDefault(t => t != null))
                .Where(t => t != null).Select(Il2CppType.From).ToArray();

        static void CopyFields(Component src, Component dst, IlType type)
        {
            var serializeField = Il2CppType.Of<SerializeField>();
            var behaviour = Il2CppType.Of<MonoBehaviour>();
            for (var t = type; t != null && !t.Equals(behaviour); t = t.BaseType)
            {
                foreach (var f in t.GetFields(Fields))
                {
                    if (f.IsNotSerialized || f.IsLiteral || (!f.IsPublic && !f.IsDefined(serializeField, true))) continue;
                    if (f.FieldType.FullName.Contains("UnityEngine.Camera")) continue;  // camera stacks are not cloned
                    var value = f.GetValue(src);
                    if (f.FieldType.IsEnum && value != null && value.ToString().Contains("Temporal")) value = SafeAa(f.FieldType);
                    f.SetValue(dst, value);
                }
            }
        }

        // Enum members read as static fields: stripped corlibs often lack Enum.GetNames.
        static Il2CppSystem.Object SafeAa(IlType enumType)
        {
            var members = enumType.GetFields(IlFlags.Public | IlFlags.Static);
            var safe = members[0];
            foreach (var m in members) if (m.Name.Contains("Subpixel")) safe = m;
            return safe.GetValue(null);
        }
    }
}
#endif
