#if !IL2CPP
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using UnityEngine;

namespace Parallax
{
    // Camera.CopyFrom skips components, so render-pipeline settings (post-processing, renderer, AA) are copied
    // field by field from the game camera to the eye cameras. Types are found by name: the plugin never links
    // against pipeline assemblies, which differ per game.
    internal static class CameraExtras
    {
        internal static readonly string[] TypeNames =
        {
            "UnityEngine.Rendering.Universal.UniversalAdditionalCameraData",
            "UnityEngine.Rendering.HighDefinition.HDAdditionalCameraData",
            "UnityEngine.Rendering.PostProcessing.PostProcessLayer",
        };
        const BindingFlags Fields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

        static Type[] types;
        static readonly HashSet<Type> logged = new HashSet<Type>();

        public static void Copy(Camera from, Camera eye)
        {
            types = types ?? FindTypes();
            foreach (var type in types)
            {
                var src = from.GetComponent(type);
                if (src == null) continue;
                var dst = eye.GetComponent(type);
                if (logged.Add(type)) Plugin.Log.LogInfo($"Eye cameras inherit {type.Name} from {from.name}");
                if (dst == null)
                {
                    // Added while inactive so OnEnable sees copied settings, not defaults (PPv2 needs its resources).
                    var go = eye.gameObject;
                    bool active = go.activeSelf;
                    go.SetActive(false);
                    dst = go.AddComponent(type);
                    CopyFields(src, dst, type);
                    AvoidTemporalAa(dst, type);
                    go.SetActive(active);
                    continue;
                }
                CopyFields(src, dst, type);
                AvoidTemporalAa(dst, type);
            }
        }

        internal static Type[] FindTypes() =>
            TypeNames.Select(name => AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetType(name, false)).FirstOrDefault(t => t != null))
                .Where(t => t != null).ToArray();

        // Unity serialization rules: public or [SerializeField], not [NonSerialized]. Plain serializable classes are
        // cloned, not shared, because they can hold per-camera history (e.g. temporal AA state).
        static void CopyFields(object src, object dst, Type type)
        {
            for (var t = type; t != null && t != typeof(MonoBehaviour) && t != typeof(object); t = t.BaseType)
            {
                foreach (var f in t.GetFields(Fields))
                {
                    if (f.IsNotSerialized || f.IsLiteral || (!f.IsPublic && !f.IsDefined(typeof(SerializeField), true))) continue;
                    if (ReferencesCameras(f.FieldType)) continue;  // camera stacks are not cloned
                    f.SetValue(dst, Clone(f.GetValue(src), f.FieldType));
                }
            }
        }

        static object Clone(object value, Type type)
        {
            if (value == null || type.IsValueType || type == typeof(string) || typeof(UnityEngine.Object).IsAssignableFrom(type)) return value;
            if (!type.IsDefined(typeof(SerializableAttribute), false) || type.IsArray || type.GetConstructor(Type.EmptyTypes) == null) return value;
            var copy = Activator.CreateInstance(type);
            CopyFields(value, copy, type);
            return copy;
        }

        static bool ReferencesCameras(Type t) =>
            typeof(Camera).IsAssignableFrom(t) || t == typeof(Camera[]) || t == typeof(List<Camera>);

        // Temporal AA reprojects with a per-camera history; in stereo it smears, so swap it for SMAA (or none).
        static void AvoidTemporalAa(object component, Type type)
        {
            for (var t = type; t != null && t != typeof(MonoBehaviour); t = t.BaseType)
            {
                foreach (var f in t.GetFields(Fields))
                {
                    if (!f.FieldType.IsEnum || !f.GetValue(component).ToString().Contains("Temporal")) continue;
                    var names = Enum.GetNames(f.FieldType);
                    var safe = names.FirstOrDefault(n => n.Contains("Subpixel")) ?? names[0];
                    f.SetValue(component, Enum.Parse(f.FieldType, safe));
                }
            }
        }
    }
}
#endif
