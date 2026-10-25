using System;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using UnityEngine;

namespace Parallax
{
    // Harness-only control surface; exists only when PARALLAX_TEST_PORT is set.
    internal class TestChannel
    {
        readonly UdpClient udp;
        internal Controllers[] ControllerOverride;
        internal Vector2? PointerOverride;
        // Right controller at this offset from the play origin, aimed at the panel or at AimTarget (also from the origin).
        internal Vector3? AimOffset, AimTarget;
        internal string AimObject;  // or at this game object, wherever it is in the game world
        Vector3? headPos;
        float headYaw;
        int busyMs;  // simulated CPU-bound game: main-thread stall per frame

        TestChannel(int port) { udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, port)); }

        // Harness file next to the plugin: the UDP port, then optionally an OpenXR runtime manifest. Games that relaunch
        // themselves through Steam lose the harness's environment, so both can come from here instead.
        static string[] TestFile()
        {
            string path = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(typeof(TestChannel).Assembly.Location), "test-port.txt");
            return System.IO.File.Exists(path) ? System.IO.File.ReadAllLines(path) : new string[0];
        }

        public static void ApplyRuntimeOverride()
        {
            var lines = TestFile();
            if (lines.Length > 1 && lines[1].Trim().Length > 0) Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", lines[1].Trim());
        }

        public static TestChannel FromEnvironment()
        {
            string text = Environment.GetEnvironmentVariable("PARALLAX_TEST_PORT");
            if (string.IsNullOrEmpty(text) && TestFile().Length > 0) text = TestFile()[0].Trim();
            if (!int.TryParse(text, out int port)) return null;
            try
            {
                var channel = new TestChannel(port);
                Plugin.Log.LogInfo("Test channel on UDP " + port);
                return channel;
            }
            catch (SocketException ex)
            {
                Plugin.Log.LogError($"Test channel port {port} unavailable ({ex.Message}); VR runs without it.");
                return null;
            }
        }

        public void Poll(Rig rig)
        {
            while (udp.Available > 0)
            {
                IPEndPoint from = null;
                var line = Encoding.UTF8.GetString(udp.Receive(ref from)).Trim();
                string reply;
                try { reply = Handle(rig, line); }
                catch (Exception ex) { reply = "error " + ex.Message; }
                var bytes = Encoding.UTF8.GetBytes(reply);
                udp.Send(bytes, bytes.Length, from);
            }
            if (busyMs > 0) System.Threading.Thread.Sleep(busyMs);
        }

        public void ApplyHead(Pose[] views)
        {
            if (headPos == null) return;
            var center = (Xr.Position(views[0]) + Xr.Position(views[1])) * 0.5f;
            var undo = Quaternion.Inverse(Xr.Rotation(views[0]));
            var target = Quaternion.Euler(0, headYaw, 0);
            for (int e = 0; e < 2; e++)
                views[e] = Xr.ToPose(headPos.Value + target * (undo * (Xr.Position(views[e]) - center)),
                    target * undo * Xr.Rotation(views[e]));
        }

        static float F(string s) => float.Parse(s, CultureInfo.InvariantCulture);

        string Handle(Rig rig, string line)
        {
            var a = line.Split(' ');
            switch (a[0])
            {
                case "head":
                    if (a[1] == "off") headPos = null;
                    else
                    {
                        headPos = new Vector3(F(a[1]), F(a[2]), F(a[3]));
                        headYaw = F(a[4]);
                    }
                    return "ok";
                case "ctrl":
                    if (a[1] == "off") ControllerOverride = null;
                    else
                        ControllerOverride = new[]
                        {
                            new Controllers
                            {
                                buttons = (Buttons)uint.Parse(a[1]),
                                lx = F(a[2]), ly = F(a[3]), rx = F(a[4]), ry = F(a[5]),
                                lTrigger = F(a[6]), rTrigger = F(a[7]), lGrip = F(a[8]), rGrip = F(a[9]),
                            },
                        };
                    return "ok";
                case "pointer":
                    if (a[1] == "off")
                    {
                        PointerOverride = null;
                        ControllerOverride = null;
                    }
                    else
                    {
                        PointerOverride = new Vector2(F(a[1]), F(a[2]));
                        ControllerOverride = ControllerOverride ?? new Controllers[1];
                        ControllerOverride[0].rTrigger = a[3] == "1" ? 1 : 0;
                    }
                    return "ok";
                case "aim":
                    AimOffset = a[1] == "off" ? (Vector3?)null : new Vector3(F(a[1]), F(a[2]), F(a[3]));
                    AimObject = a.Length > 4 && a[4].StartsWith("@") ? a[4].Substring(1) : null;
                    AimTarget = a.Length > 6 ? new Vector3(F(a[4]), F(a[5]), F(a[6])) : (Vector3?)null;
                    return "ok";
                case "hider":
                    if (a[1] == "1") rig.Hider.Start();
                    else if (a[1] == "show") rig.Hider.ShowAll();
                    else rig.Hider.Stop();
                    return "ok";
                case "recenter":
                    rig.RequestRecenter();
                    return "ok";
                case "busy":
                    busyMs = int.Parse(a[1]);
                    return "ok";
                case "theater":
                    Settings.Theater.Value = a[1] == "1";
                    return "ok";
                case "menu":
                    if (rig.Menu.Open != (a[1] == "1")) rig.Menu.Toggle();
                    return "ok";
                case "tab":
                    rig.Menu.Show(int.Parse(a[1]));
                    return "ok";
                case "set":
                    Entry(a[1]).SetSerializedValue(a[2]);
                    return "ok";
                case "get":
                    return Entry(a[1]).GetSerializedValue();
                case "capture":
                    Native.prx_capture(line.Substring(8));
                    return "ok";
                case "stats":
                    return Stats(rig);
#if !IL2CPP
                case "videos":
                    var vp = Type.GetType("UnityEngine.Video.VideoPlayer, UnityEngine.VideoModule");
                    if (vp == null) return "no video module";
                    string Get(object o, string n) => vp.GetProperty(n)?.GetValue(o, null)?.ToString() ?? "null";
                    return string.Join("\n", UnityEngine.Object.FindObjectsOfType(vp).Select(o =>
                        $"{((Component)o).name} mode {Get(o, "renderMode")} cam {Get(o, "targetCamera")} rt {Get(o, "targetTexture")} " +
                        $"playing {Get(o, "isPlaying")} prepared {Get(o, "isPrepared")} frame {Get(o, "frame")} enabled {((Behaviour)o).isActiveAndEnabled}").ToArray());
#endif
                case "cams":
                    return string.Join("\n", Camera.allCameras.Select(c =>
                        $"{c.name} depth {c.depth} mask {c.cullingMask:X} clear {c.clearFlags} bg {c.backgroundColor} ortho {c.orthographic} " +
                        $"rect {c.rect} rt {(c.targetTexture != null ? $"{c.targetTexture.width}x{c.targetTexture.height} {c.targetTexture.format}" : "-")} " +
                        $"pos {V(c.transform.position)} rot {V(c.transform.eulerAngles)} parent {(c.transform.parent != null ? c.transform.parent.name : "-")}").ToArray());
                case "quit":
                    Application.Quit();
                    return "ok";
            }
            return "error unknown command";
        }

        static BepInEx.Configuration.ConfigEntryBase Entry(string key) =>
            Settings.File[Settings.File.Keys.First(d => d.Key == key)];

        static string V(Vector3 v) => $"[{v.x:0.####},{v.y:0.####},{v.z:0.####}]";

        static string P(Pose p) => $"[{p.px:0.####},{p.py:0.####},{p.pz:0.####},{p.qx:0.####},{p.qy:0.####},{p.qz:0.####},{p.qw:0.####}]";

        // .NET 3.5 has no FormattableString, so the JSON is formatted under a temporarily invariant culture.
        static string Stats(Rig rig)
        {
            var thread = System.Threading.Thread.CurrentThread;
            var culture = thread.CurrentCulture;
            thread.CurrentCulture = CultureInfo.InvariantCulture;
            try { return StatsJson(rig); }
            finally { thread.CurrentCulture = culture; }
        }

        static string StatsJson(Rig rig)
        {
            var s = rig.GetStats();
            var f = rig.Fovs[0];
            var cam = rig.GameCam;
            return $$"""
                {"state":{{s.state}},"focused":{{s.focused}},"periodMs":{{s.periodMs:0.###}},"p95Ms":{{s.p95Ms:0.###}},
                "frames":{{s.frames}},"submitted":{{s.submitted}},"stereo":{{s.stereoSubmitted}},"quad":{{s.quadSubmitted}},"curved":{{s.curvedSubmitted}},"curvedSupported":{{s.curvedSupported}},
                "theater":{{(Settings.Theater.Value ? 1 : 0)}},"vignette":{{rig.VignetteNow:0.###}},"menu":{{(rig.Menu.Open ? 1 : 0)}},"pointer":{{(rig.PointerActive ? 1 : 0)}},"hider":{{(rig.Hider.Active ? 1 : 0)}},"hideTarget":"{{(rig.Hider.Target != null ? rig.Hider.Target.name : "")}}","aimValid":{{rig.AimValid}},"scanned":{{rig.Hider.Scanned}},"aim":{{(rig.Aiming ? 1 : 0)}},"smoothing":{{(rig.StepSmoothing ? 1 : 0)}},"orbit":{{rig.OrbitRadius:0.###}},"body":{{rig.BodyYaw:0.#}},
                "eyeL":{{V(rig.Eyes[0].transform.position)}},"eyeR":{{V(rig.Eyes[1].transform.position)}},
                "cam":"{{(cam != null ? cam.name : "")}}","camMask":{{(cam != null ? cam.cullingMask : 0)}},"camPos":{{(cam != null ? V(cam.transform.position) : "null")}},
                "fov":[{{f.left:0.#####}},{{f.right:0.#####}},{{f.up:0.#####}},{{f.down:0.#####}}],
                "eyeSize":[{{rig.EyeSize.x}},{{rig.EyeSize.y}}],"screen":[{{Screen.width}},{{Screen.height}}],
                "viewL":{{P(rig.Views[0])}},"viewR":{{P(rig.Views[1])}}}
                """;
        }
    }
}
