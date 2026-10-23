using UnityEngine;

namespace Parallax
{
    // Drawn with IMGUI onto the screen, so it reaches the headset through the UI capture and the laser pointer.
    internal class Menu
    {
        static readonly string[] Tabs = { "Quick", "Game", "Diagnostics" };
        const float RefHeight = 1080f, Width = 1100f, Height = 720f, Row = 64f;

        readonly Rig rig;
        internal bool Open;
        int tab;
        CursorLockMode savedLock;
        bool savedVisible;
        Vector2 scroll;

        GUIStyle panel, button, selected, label, muted, title;

        public Menu(Rig rig) { this.rig = rig; }

        public void Toggle()
        {
            Open = !Open;
            if (Open)
            {
                savedLock = Cursor.lockState;
                savedVisible = Cursor.visible;
            }
            else
            {
                Cursor.lockState = savedLock;
                Cursor.visible = savedVisible;
            }
            Native.prx_haptic_pulse(0, 0.4f, 0.03f);
        }

        // Games re-lock the cursor every frame; keep it free while the menu is up.
        public void Update()
        {
            if (!Open) return;
            Cursor.lockState = CursorLockMode.None;
            Cursor.visible = true;
        }

        public void Draw()
        {
            if (!Open) return;
            EnsureStyles();
            float k = Screen.height / RefHeight;
            GUI.matrix = Matrix4x4.TRS(Vector3.zero, Quaternion.identity, new Vector3(k, k, 1));
            var area = new Rect((Screen.width / k - Width) / 2, (RefHeight - Height) / 2, Width, Height);
            GUI.Box(area, GUIContent.none, panel);
            GUILayout.BeginArea(new Rect(area.x + 32, area.y + 24, Width - 64, Height - 48));
            GUILayout.BeginHorizontal();
            for (int i = 0; i < Tabs.Length; i++)
            {
                if (GUILayout.Button(Tabs[i], i == tab ? selected : button, GUILayout.Height(Row), GUILayout.Width(220))) tab = i;
                GUILayout.Space(8);
            }
            GUILayout.FlexibleSpace();
            if (GUILayout.Button("Close", button, GUILayout.Height(Row), GUILayout.Width(160))) Toggle();
            GUILayout.EndHorizontal();
            GUILayout.Space(24);
            if (tab == 0) DrawQuick();
            else if (tab == 1) DrawGame();
            else DrawDiagnostics();
            GUILayout.EndArea();
            GUI.matrix = Matrix4x4.identity;
        }

        void DrawQuick()
        {
            bool theater = Settings.Theater.Value;
            Choice("View", "Stereo", "Theater", ref theater);
            Settings.Theater.Value = theater;

            GUILayout.BeginHorizontal();
            GUILayout.Label("Position", label, GUILayout.Width(300), GUILayout.Height(Row));
            if (GUILayout.Button("Recenter", button, GUILayout.Height(Row), GUILayout.Width(300))) rig.RequestRecenter();
            GUILayout.EndHorizontal();
            GUILayout.Space(12);

            Stepper("Height", Settings.Height, 0.05f, -1f, 1f, "{0:+0.00;-0.00;0.00} m");
            Stepper("World scale", Settings.WorldScale, 0.1f, 0.5f, 3f, "{0:0.0}x");

            bool kbm = Settings.Input.Value == InputMode.KeyboardMouse;
            Choice("Controls", "Gamepad", "Keyboard & mouse", ref kbm);
            Settings.Input.Value = kbm ? InputMode.KeyboardMouse : InputMode.Gamepad;
        }

        void DrawGame()
        {
            GUILayout.Label("Head camera", label, GUILayout.Height(Row));
            scroll = GUILayout.BeginScrollView(scroll, GUILayout.Height(Height - 260));
            if (GUILayout.Button("Automatic", Settings.CameraName.Value.Length == 0 ? selected : button, GUILayout.Height(Row)))
                PickCamera("");
            foreach (var cam in Camera.allCameras)
            {
                if (rig.IsEye(cam) || cam.targetTexture != null) continue;
                var style = cam == rig.GameCam ? selected : button;
                if (GUILayout.Button(cam.name, style, GUILayout.Height(Row))) PickCamera(cam.name);
            }
            GUILayout.EndScrollView();
        }

        void PickCamera(string name)
        {
            Settings.CameraName.Value = name;
            rig.ReleaseCamera();
        }

        void DrawDiagnostics()
        {
            var s = rig.GetStats();
            Line("Runtime", ((XrState)s.state) + (s.focused != 0 ? ", focused" : ""));
            Line("Refresh", $"{1000.0 / System.Math.Max(0.001, s.periodMs):0} Hz");
            Line("Frame time p95", $"{s.p95Ms:0.0} ms");
            Line("Frames submitted", $"{s.submitted} of {s.frames}");
            Line("Eye resolution", $"{rig.EyeSize.x} x {rig.EyeSize.y}");
            Line("Head camera", rig.GameCam != null ? rig.GameCam.name : "none");
        }

        void Line(string name, string value)
        {
            GUILayout.BeginHorizontal();
            GUILayout.Label(name, muted, GUILayout.Width(360), GUILayout.Height(48));
            GUILayout.Label(value, label, GUILayout.Height(48));
            GUILayout.EndHorizontal();
        }

        void Choice(string name, string off, string on, ref bool value)
        {
            GUILayout.BeginHorizontal();
            GUILayout.Label(name, label, GUILayout.Width(300), GUILayout.Height(Row));
            if (GUILayout.Button(off, value ? button : selected, GUILayout.Height(Row), GUILayout.Width(300))) value = false;
            GUILayout.Space(8);
            if (GUILayout.Button(on, value ? selected : button, GUILayout.Height(Row), GUILayout.Width(300))) value = true;
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        void Stepper(string name, BepInEx.Configuration.ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            GUILayout.BeginHorizontal();
            GUILayout.Label(name, label, GUILayout.Width(300), GUILayout.Height(Row));
            if (GUILayout.Button("-", button, GUILayout.Height(Row), GUILayout.Width(96))) entry.Value = Mathf.Max(min, entry.Value - step);
            GUILayout.Label(string.Format(format, entry.Value), title, GUILayout.Width(200), GUILayout.Height(Row));
            if (GUILayout.Button("+", button, GUILayout.Height(Row), GUILayout.Width(96))) entry.Value = Mathf.Min(max, entry.Value + step);
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        // Warm grays plus one accent; flat fills instead of borders.
        void EnsureStyles()
        {
            if (panel != null) return;
            var text = new Color(0.93f, 0.91f, 0.89f);
            panel = new GUIStyle { normal = { background = Fill(new Color(0.12f, 0.11f, 0.10f, 0.96f)) } };
            button = new GUIStyle
            {
                fontSize = 28,
                alignment = TextAnchor.MiddleCenter,
                normal = { background = Fill(new Color(0.20f, 0.19f, 0.18f)), textColor = text },
                hover = { background = Fill(new Color(0.28f, 0.26f, 0.24f)), textColor = text },
                active = { background = Fill(new Color(0.34f, 0.31f, 0.29f)), textColor = text },
            };
            selected = new GUIStyle(button)
            {
                fontStyle = FontStyle.Bold,
                normal = { background = Fill(new Color(0.91f, 0.51f, 0.23f)), textColor = new Color(0.12f, 0.11f, 0.10f) },
                hover = { background = Fill(new Color(0.95f, 0.58f, 0.31f)), textColor = new Color(0.12f, 0.11f, 0.10f) },
            };
            label = new GUIStyle { fontSize = 28, alignment = TextAnchor.MiddleLeft, normal = { textColor = text } };
            muted = new GUIStyle(label) { normal = { textColor = new Color(0.64f, 0.61f, 0.58f) } };
            title = new GUIStyle(label) { fontStyle = FontStyle.Bold, alignment = TextAnchor.MiddleCenter };
        }

        static Texture2D Fill(Color c)
        {
            var t = new Texture2D(1, 1);
            t.SetPixel(0, 0, c);
            t.Apply();
            return t;
        }
    }
}
