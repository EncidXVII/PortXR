// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using MoonSharp.Interpreter;
using UnityEngine;
#if IL2CPP
using Il2CppInterop.Runtime;
#endif

namespace PortXR
{
    // Per-game Lua scripts, for what the automatic handling gets wrong in one game: pick a camera, hide or move an
    // object, flip a game setting, nudge the eyes. They live in %APPDATA%\PortXR\scripts\<game exe name>\*.lua (the
    // app imports them there), so they survive an uninstall, and reload when a file changes. Sandboxed: no files, no
    // OS, only the `portxr` table (see SCRIPTING.md). A script may define on_frame(dt), on_scene(name) and
    // on_camera(name); one that fails is switched off with one log line.
    internal sealed class Scripts
    {
        public static string Folder => Path.Combine(Path.Combine(Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "PortXR"), "scripts"),
            System.Diagnostics.Process.GetCurrentProcess().ProcessName);

        readonly Rig rig;
        readonly List<Loaded> scripts = new List<Loaded>();
        string stamp = "";
        float nextCheck;
        string scene, camera;

        sealed class Loaded
        {
            public string Name;
            public Script Lua;
            public MoonSharp.Interpreter.Coroutine Runner;
            public bool Failed;
        }

        // A script's code runs inside one coroutine kept for the script, which MoonSharp hands back every so many
        // instructions: that is where a script running over a second (an endless loop) is stopped, so it can't freeze
        // the game. One coroutine per script and not per call, because each new one allocates its own stacks.
        const string RunnerSource = "local yield = coroutine.yield return function() local f, a = yield() while true do f(a) f, a = yield() end end";

        static MoonSharp.Interpreter.Coroutine Runner(Script lua)
        {
            var co = lua.CreateCoroutine(lua.DoString(RunnerSource)).Coroutine;
            co.AutoYieldCounter = 10000;
            co.Resume();
            return co;
        }

        static void Run(Loaded s, DynValue function, DynValue arg)
        {
            var clock = System.Diagnostics.Stopwatch.StartNew();
            s.Runner.Resume(function, arg);
            while (s.Runner.State == CoroutineState.ForceSuspended)
            {
                if (clock.ElapsedMilliseconds > 1000) throw new ScriptRuntimeException("ran for over a second (an endless loop?) and was stopped");
                s.Runner.Resume();
            }
        }

        // Eye offset a script set, metres in the player's frame (x right, y up, z forward).
        public Vector3 EyeOffset;
        // Part of the game's UI that moves with the crosshair (Crosshair: Game's): width and height as fractions of
        // the screen; zero keeps the default.
        public Vector2 CrosshairArea;

        public Scripts(Rig rig) { this.rig = rig; }

        public void Update()
        {
            if (Time.unscaledTime >= nextCheck)
            {
                nextCheck = Time.unscaledTime + 2f;
                Reload();
            }
            if (scripts.Count == 0) return;
            string nowScene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
            if (nowScene != scene)
            {
                scene = nowScene;
                Call("on_scene", DynValue.NewString(scene));
            }
            string nowCamera = rig.GameCam != null ? rig.GameCam.name : null;
            if (nowCamera != camera)
            {
                camera = nowCamera;
                if (camera != null) Call("on_camera", DynValue.NewString(camera));
            }
            Call("on_frame", DynValue.NewNumber(Time.unscaledDeltaTime));
        }

        void Reload()
        {
            string[] files;
            try
            {
                files = Directory.Exists(Folder) ? Directory.GetFiles(Folder, "*.lua").OrderBy(f => f).ToArray() : new string[0];
            }
            catch (Exception)
            {
                return;
            }
            string now = string.Join("|", files.Select(f => f + File.GetLastWriteTimeUtc(f).Ticks).ToArray());
            if (now == stamp) return;
            stamp = now;
            scripts.Clear();
            EyeOffset = Vector3.zero;
            CrosshairArea = Vector2.zero;
            scene = camera = null;
            foreach (var file in files)
            {
                var s = new Loaded { Name = Path.GetFileName(file), Lua = new Script(CoreModules.Preset_SoftSandbox) };
                s.Lua.Globals["portxr"] = Api(s.Lua);
                try
                {
                    s.Runner = Runner(s.Lua);
                    Run(s, s.Lua.LoadString(File.ReadAllText(file), null, s.Name), DynValue.Nil);
                    scripts.Add(s);
                    Plugin.Log.LogInfo($"Script {s.Name} loaded");
                }
                catch (Exception ex)
                {
                    Plugin.Log.LogWarning($"Script {s.Name} failed to load: {Message(ex)}");
                }
            }
        }

        void Call(string function, DynValue arg)
        {
            foreach (var s in scripts)
            {
                if (s.Failed) continue;
                var f = s.Lua.Globals.Get(function);
                if (f.Type != DataType.Function) continue;
                try
                {
                    Run(s, f, arg);
                }
                catch (Exception ex)
                {
                    s.Failed = true;
                    Plugin.Log.LogWarning($"Script {s.Name} stopped in {function}: {Message(ex)}");
                }
            }
        }

        static string Message(Exception ex) => ex is InterpreterException ie && ie.DecoratedMessage != null ? ie.DecoratedMessage : ex.Message;

        Table Api(Script lua)
        {
            var t = new Table(lua);
            void Add(string name, Func<CallbackArguments, DynValue> f) => t[name] = DynValue.NewCallback((ctx, a) => f(a));
            Add("log", a =>
            {
                Plugin.Log.LogInfo("[script] " + a.AsStringUsingMeta(null, 0, "log"));
                return DynValue.Nil;
            });
            Add("get", a => DynValue.NewString(Setting(a[0].String).GetSerializedValue()));
            Add("set", a =>
            {
                Setting(a[0].String).SetSerializedValue(a[1].ToPrintString());
                return DynValue.Nil;
            });
            Add("time", a => DynValue.NewNumber(Time.unscaledTime));
            Add("scene", a => DynValue.NewString(UnityEngine.SceneManagement.SceneManager.GetActiveScene().name));
            Add("camera", a => rig.GameCam != null ? DynValue.NewString(rig.GameCam.name) : DynValue.Nil);
            Add("key", a => DynValue.NewBoolean(Native.prx_key_down((int)a[0].Number) != 0));
            Add("find", a => DynValue.NewBoolean(GameObject.Find(a[0].String) != null));
            Add("active", a =>
            {
                var go = Object(a[0].String);
                if (a.Count > 1) go.SetActive(a[1].CastToBool());
                return DynValue.NewBoolean(go.activeSelf);
            });
            Add("position", a => Triple(Object(a[0].String).transform.position));
            Add("set_position", a =>
            {
                Object(a[0].String).transform.position = new Vector3((float)a[1].Number, (float)a[2].Number, (float)a[3].Number);
                return DynValue.Nil;
            });
            Add("rotation", a => Triple(Object(a[0].String).transform.eulerAngles));
            Add("set_rotation", a =>
            {
                Object(a[0].String).transform.eulerAngles = new Vector3((float)a[1].Number, (float)a[2].Number, (float)a[3].Number);
                return DynValue.Nil;
            });
            // Renderers under the object stop drawing; the object itself keeps running (unlike active(path, false)).
            Add("hide", a =>
            {
                foreach (var r in Object(a[0].String).GetComponentsInChildren<Renderer>(true)) r.enabled = a.Count < 2 || !a[1].CastToBool();
                return DynValue.Nil;
            });
            Add("eye_offset", a =>
            {
                EyeOffset = new Vector3((float)a[0].Number, (float)a[1].Number, (float)a[2].Number);
                return DynValue.Nil;
            });
            Add("crosshair_area", a =>
            {
                CrosshairArea = new Vector2(Mathf.Clamp((float)a[0].Number, 0.02f, 1f), Mathf.Clamp((float)a[1].Number, 0.02f, 1f));
                return DynValue.Nil;
            });
            Add("enabled", a => Member(a[0].String, a[1].String, "enabled", a.Count > 2 ? a[2] : null));
            Add("field", a => Member(a[0].String, a[1].String, a[2].String, a.Count > 3 ? a[3] : null));
            return t;
        }

        static BepInEx.Configuration.ConfigEntryBase Setting(string key)
        {
            var def = Settings.File.Keys.FirstOrDefault(d => d.Key == key);
            if (def == null) throw new ScriptRuntimeException("no PortXR setting named " + key);
            return Settings.File[def];
        }

        static GameObject Object(string path)
        {
            var go = GameObject.Find(path);
            if (go == null) throw new ScriptRuntimeException("no active object at " + path);
            return go;
        }

        static DynValue Triple(Vector3 v) => DynValue.NewTuple(DynValue.NewNumber(v.x), DynValue.NewNumber(v.y), DynValue.NewNumber(v.z));

        static readonly Dictionary<string, Type> types = new Dictionary<string, Type>();

        // One member (field or property) of a component on the object, by type name ("Light", "PlayerController"):
        // read, or written when a value is given. Numbers, booleans, strings and enums (by number or name).
        static DynValue Member(string path, string typeName, string member, DynValue value)
        {
            var go = Object(path);
            if (!types.TryGetValue(typeName, out var type))
            {
                type = AppDomain.CurrentDomain.GetAssemblies().SelectMany(Types).FirstOrDefault(t => t.Name == typeName || t.FullName == typeName);
                types[typeName] = type;
            }
            if (type == null) throw new ScriptRuntimeException("no type named " + typeName);
#if IL2CPP
            var c = go.GetComponent(Il2CppType.From(type));
            object target = c == null ? null : Activator.CreateInstance(type, c.Pointer);
#else
            object target = go.GetComponent(type);
            if ((Component)target == null) target = null;
#endif
            if (target == null) throw new ScriptRuntimeException($"{path} has no {typeName}");
            var p = type.GetProperty(member);
            var f = p == null ? type.GetField(member) : null;
            if (p == null && f == null) throw new ScriptRuntimeException($"{typeName} has no member {member}");
            var memberType = p != null ? p.PropertyType : f.FieldType;
            if (value != null)
            {
                object v = memberType == typeof(bool) ? value.CastToBool()
                    : memberType == typeof(string) ? value.ToPrintString()
                    : memberType.IsEnum ? (value.Type == DataType.Number ? Enum.ToObject(memberType, (int)value.Number) : Enum.Parse(memberType, value.String))
                    : Convert.ChangeType(value.Number, memberType);
                if (p != null) p.SetValue(target, v, null);
                else f.SetValue(target, v);
            }
            object now = p != null ? p.GetValue(target, null) : f.GetValue(target);
            return now is bool b ? DynValue.NewBoolean(b)
                : now is string s ? DynValue.NewString(s)
                : now is IConvertible && !(now is Enum) ? DynValue.NewNumber(Convert.ToDouble(now))
                : DynValue.NewString(now?.ToString() ?? "nil");
        }

        static IEnumerable<Type> Types(System.Reflection.Assembly a)
        {
            try
            {
                return a.GetTypes();
            }
            catch (System.Reflection.ReflectionTypeLoadException ex)
            {
                return ex.Types.Where(t => t != null);
            }
            catch (Exception)
            {
                return new Type[0];
            }
        }
    }
}
