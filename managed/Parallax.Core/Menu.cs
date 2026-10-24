using UnityEngine;

namespace Parallax
{
    // Drawn with IMGUI onto the screen, so it reaches the headset through the UI capture and the laser pointer.
    // Only engine members that survive IL2CPP stripping in typical games are used: no texture creation, no style
    // copies, layout options always passed as arrays (Il2CppInterop has no params).
    internal class Menu
    {
        static readonly string[] Tabs = { "Quick", "Game", "Diagnostics" };
        const float RefHeight = 1080f, Width = 1100f, Height = 720f, Row = 64f;
        // Warm grays plus one accent; flat fills instead of borders.
        static readonly Color PanelFill = new Color(0.12f, 0.11f, 0.10f, 0.96f), Fill = new Color(0.20f, 0.19f, 0.18f);
        static readonly Color Accent = new Color(0.91f, 0.51f, 0.23f), Ink = new Color(0.93f, 0.91f, 0.89f);
        static readonly Color Muted = new Color(0.64f, 0.61f, 0.58f), Dark = new Color(0.12f, 0.11f, 0.10f);

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
            GUI.backgroundColor = PanelFill;
            GUI.Box(area, GUIContent.none, panel);
            GUILayout.BeginArea(new Rect(area.x + 32, area.y + 24, Width - 64, Height - 48));
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            for (int i = 0; i < Tabs.Length; i++)
            {
                if (Button(Tabs[i], i == tab, 220)) tab = i;
                GUILayout.Space(8);
            }
            GUILayout.FlexibleSpace();
            if (Button("Close", false, 160)) Toggle();
            GUILayout.EndHorizontal();
            GUILayout.Space(24);
            if (tab == 0) DrawQuick();
            else if (tab == 1) DrawGame();
            else DrawDiagnostics();
            GUILayout.EndArea();
            GUI.matrix = Matrix4x4.identity;
            GUI.backgroundColor = Color.white;
        }

        void DrawQuick()
        {
            bool theater = Settings.Theater.Value;
            Choice("View", "Stereo", "Theater", ref theater);
            Settings.Theater.Value = theater;

            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label("Position", label, 300);
            if (Button("Recenter", false, 300)) rig.RequestRecenter();
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
            Label("Head camera", label, 0);
            scroll = GUILayout.BeginScrollView(scroll, new[] { GUILayout.Height(Height - 260) });
            if (Button("Automatic", Settings.CameraName.Value.Length == 0, 0)) PickCamera("");
            foreach (var cam in Camera.allCameras)
            {
                if (rig.IsEye(cam) || cam.targetTexture != null) continue;
                if (Button(cam.name, cam == rig.GameCam, 0)) PickCamera(cam.name);
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
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Label(name, muted, new[] { GUILayout.Width(360), GUILayout.Height(48) });
            GUILayout.Label(value, label, new[] { GUILayout.Height(48) });
            GUILayout.EndHorizontal();
        }

        void Choice(string name, string off, string on, ref bool value)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 300);
            if (Button(off, !value, 300)) value = false;
            GUILayout.Space(8);
            if (Button(on, value, 300)) value = true;
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        void Stepper(string name, BepInEx.Configuration.ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 300);
            if (Button("-", false, 96)) entry.Value = Mathf.Max(min, entry.Value - step);
            Label(string.Format(format, entry.Value), title, 200);
            if (Button("+", false, 96)) entry.Value = Mathf.Min(max, entry.Value + step);
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        // width 0 = stretch.
        bool Button(string text, bool on, float width)
        {
            GUI.backgroundColor = on ? Accent : Fill;
            bool clicked = GUILayout.Button(text, on ? selected : button, Size(width));
            GUI.backgroundColor = Color.white;
            return clicked;
        }

        void Label(string text, GUIStyle style, float width) => GUILayout.Label(text, style, Size(width));

        static GUILayoutOption[] Size(float width) =>
            width > 0 ? new[] { GUILayout.Height(Row), GUILayout.Width(width) } : new[] { GUILayout.Height(Row) };

        void EnsureStyles()
        {
            if (panel != null) return;
            panel = Style(TextAnchor.UpperLeft, Ink, Ink, false, true);
            button = Style(TextAnchor.MiddleCenter, Ink, Accent, false, true);
            selected = Style(TextAnchor.MiddleCenter, Dark, Dark, true, true);
            label = Style(TextAnchor.MiddleLeft, Ink, Ink, false, false);
            muted = Style(TextAnchor.MiddleLeft, Muted, Muted, false, false);
            title = Style(TextAnchor.MiddleCenter, Ink, Ink, true, false);
        }

        // Fills use the built-in white texture tinted by GUI.backgroundColor; hover feedback is the text colour.
        static GUIStyle Style(TextAnchor align, Color text, Color hoverText, bool bold, bool filled)
        {
            var s = new GUIStyle { fontSize = 28, alignment = align, fontStyle = bold ? FontStyle.Bold : FontStyle.Normal };
            s.normal.textColor = text;
            s.hover.textColor = s.active.textColor = hoverText;
            if (filled) s.normal.background = s.hover.background = s.active.background = Texture2D.whiteTexture;
            return s;
        }
    }
}
