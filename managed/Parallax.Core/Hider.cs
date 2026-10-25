using System.Collections.Generic;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Parallax
{
    // Objects the player hid from the Game tab (a helmet, a body, a screen overlay mesh in front of the camera):
    // aim the right controller at one and pull the trigger. Stored per game as hierarchy paths and hidden again on
    // every scene where they turn up.
    internal sealed class Hider
    {
        readonly List<Renderer> hidden = new List<Renderer>();
        readonly Optional find = new Optional("Hiding objects");
        Renderer[] renderers = new Renderer[0];
        float nextScan, nextApply;
        bool triggerWas, logged;

        internal bool Active { get; private set; }
        internal Renderer Target { get; private set; }
        internal float TargetDistance { get; private set; }
        internal int Count => Paths().Count;
        internal int Scanned => renderers.Length;

        internal void Start()
        {
            Active = true;
            triggerWas = true;  // the trigger that chose the tool must be let go first
            nextScan = 0;
        }

        internal void Stop()
        {
            Active = false;
            Target = null;
        }

        // Nearest enabled renderer whose bounds the ray (game world) crosses.
        internal void Aim(Ray ray, bool trigger, bool cancel)
        {
            if (cancel)
            {
                Stop();
                return;
            }
            if (Time.unscaledTime >= nextScan)
            {
                nextScan = Time.unscaledTime + 1f;
                int before = renderers.Length;
                find.Run(Scan);
                if (renderers.Length != before || !logged) Plugin.Log.LogInfo($"Hide objects: {renderers.Length} renderers in view");
                logged = true;
            }
            Target = null;
            TargetDistance = float.MaxValue;
            foreach (var r in renderers)
            {
                if (r == null || !r.enabled || !r.gameObject.activeInHierarchy) continue;
                if (Hit(r.bounds, ray, out float d) && d < TargetDistance)
                {
                    Target = r;
                    TargetDistance = d;
                }
            }
            if (trigger && !triggerWas && Target != null)
            {
                var list = Paths();
                list.Add(PathOf(Target.transform));
                Settings.Hidden.Value = string.Join(";", list.ToArray());
                Hide(Target);
                Plugin.Log.LogInfo("Hid " + list[list.Count - 1]);
                Native.prx_haptic_pulse(1, 0.4f, 0.04f);
            }
            triggerWas = trigger;
        }

        // Re-hides stored objects every couple of seconds: scenes load and games re-enable renderers.
        internal void Apply()
        {
            if (Time.unscaledTime < nextApply || Settings.Hidden.Value.Length == 0) return;
            nextApply = Time.unscaledTime + 2f;
            foreach (var path in Paths())
            {
                var go = GameObject.Find(path);
                var r = go != null ? go.GetComponent<Renderer>() : null;
                if (r != null && r.enabled) Hide(r);
            }
        }

        internal void ShowAll()
        {
            foreach (var r in hidden)
                if (r != null) r.enabled = true;
            hidden.Clear();
            Settings.Hidden.Value = "";
        }

        // Every renderer under the loaded scenes' roots. FindObjectsOfType<Renderer> comes back empty on IL2CPP.
        void Scan()
        {
            var list = new List<Renderer>();
            for (int i = 0; i < SceneManager.sceneCount; i++)
            {
                var scene = SceneManager.GetSceneAt(i);
                if (!scene.isLoaded) continue;
                foreach (var root in scene.GetRootGameObjects())
                {
#if IL2CPP
                    // The generic overload fails in Il2CppInterop (its array type is not an IL2CPP reference type).
                    foreach (var c in root.GetComponentsInChildren(Il2CppInterop.Runtime.Il2CppType.Of<Renderer>()))
                        list.Add(c.Cast<Renderer>());
#else
                    list.AddRange(root.GetComponentsInChildren<Renderer>());
#endif
                }
            }
            renderers = list.ToArray();
        }

        // Ray against an axis-aligned box (slab test); plain math, since IL2CPP games may strip Bounds.IntersectRay.
        static bool Hit(Bounds b, Ray ray, out float distance)
        {
            Vector3 c = b.center, e = b.extents, o = ray.origin, d = ray.direction;
            float[] lo = { c.x - e.x, c.y - e.y, c.z - e.z }, hi = { c.x + e.x, c.y + e.y, c.z + e.z };
            float[] os = { o.x, o.y, o.z }, ds = { d.x, d.y, d.z };
            float near = 0f, far = float.MaxValue;
            distance = 0;
            for (int i = 0; i < 3; i++)
            {
                if (Mathf.Abs(ds[i]) < 1e-8f)
                {
                    if (os[i] < lo[i] || os[i] > hi[i]) return false;
                    continue;
                }
                float t1 = (lo[i] - os[i]) / ds[i], t2 = (hi[i] - os[i]) / ds[i];
                near = Mathf.Max(near, Mathf.Min(t1, t2));
                far = Mathf.Min(far, Mathf.Max(t1, t2));
            }
            distance = near;
            return near <= far;
        }

        void Hide(Renderer r)
        {
            r.enabled = false;
            hidden.Add(r);
        }

        static List<string> Paths()
        {
            var list = new List<string>();
            foreach (var p in Settings.Hidden.Value.Split(';'))
                if (p.Length > 0 && !list.Contains(p)) list.Add(p);
            return list;
        }

        static string PathOf(Transform t)
        {
            var path = t.name;
            for (var p = t.parent; p != null; p = p.parent) path = p.name + "/" + path;
            return "/" + path;
        }
    }
}
