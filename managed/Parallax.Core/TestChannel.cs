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
        Vector3? headPos;
        float headYaw;

        TestChannel(int port) { udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, port)); }

        public static TestChannel FromEnvironment()
        {
            if (!int.TryParse(Environment.GetEnvironmentVariable("PARALLAX_TEST_PORT"), out int port)) return null;
            Plugin.Log.LogInfo("Test channel on UDP " + port);
            return new TestChannel(port);
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
                case "recenter":
                    rig.RequestRecenter();
                    return "ok";
                case "theater":
                    Settings.Theater.Value = a[1] == "1";
                    return "ok";
                case "menu":
                    if (rig.Menu.Open != (a[1] == "1")) rig.Menu.Toggle();
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
                case "quit":
                    Application.Quit();
                    return "ok";
            }
            return "error unknown command";
        }

        static BepInEx.Configuration.ConfigEntryBase Entry(string key) =>
            Settings.File[Settings.File.Keys.First(d => d.Key == key)];

        static string V(Vector3 v) => FormattableString.Invariant($"[{v.x:0.####},{v.y:0.####},{v.z:0.####}]");

        static string P(Pose p) => FormattableString.Invariant($"[{p.px:0.####},{p.py:0.####},{p.pz:0.####},{p.qx:0.####},{p.qy:0.####},{p.qz:0.####},{p.qw:0.####}]");

        static string Stats(Rig rig)
        {
            var s = rig.GetStats();
            var f = rig.Fovs[0];
            var cam = rig.GameCam;
            return FormattableString.Invariant($$"""
                {"state":{{s.state}},"focused":{{s.focused}},"periodMs":{{s.periodMs:0.###}},"p95Ms":{{s.p95Ms:0.###}},
                "frames":{{s.frames}},"submitted":{{s.submitted}},"stereo":{{s.stereoSubmitted}},"quad":{{s.quadSubmitted}},
                "theater":{{(Settings.Theater.Value ? 1 : 0)}},"menu":{{(rig.Menu.Open ? 1 : 0)}},"pointer":{{(rig.PointerActive ? 1 : 0)}},
                "eyeL":{{V(rig.Eyes[0].transform.position)}},"eyeR":{{V(rig.Eyes[1].transform.position)}},
                "cam":"{{(cam != null ? cam.name : "")}}","camMask":{{(cam != null ? cam.cullingMask : 0)}},"camPos":{{(cam != null ? V(cam.transform.position) : "null")}},
                "fov":[{{f.left:0.#####}},{{f.right:0.#####}},{{f.up:0.#####}},{{f.down:0.#####}}],
                "eyeSize":[{{rig.EyeSize.x}},{{rig.EyeSize.y}}],"screen":[{{Screen.width}},{{Screen.height}}],
                "viewL":{{P(rig.Views[0])}},"viewR":{{P(rig.Views[1])}}}
                """);
        }
    }
}
