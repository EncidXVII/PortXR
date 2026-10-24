using System.Collections.Generic;
using UnityEngine;

namespace Parallax
{
    // Drawn with IMGUI onto the screen, so it reaches the headset through the UI capture and the laser pointer.
    // Only engine members that survive IL2CPP stripping in typical games are used: no texture creation outside an
    // Optional guard, no style copies, layout options always passed as arrays (Il2CppInterop has no params).
    internal class Menu
    {
        static readonly string[] Tabs = { "Quick", "View", "Controls", "Game", "Diagnostics" };
        const int QuickTab = 0, ViewTab = 1, ControlsTab = 2, GameTab = 3;
        const float RefHeight = 1080f, Width = 1100f, Height = 720f, Row = 64f;
        const int ThumbW = 320, ThumbH = 180, History = 120;
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
        int picking = -1;  // binding being changed in the Controls tab
        float hovered, lastHovered;

        // Game tab: one hidden camera renders a preview of one candidate per frame.
        readonly Optional previews = new Optional("Camera previews");
        readonly Dictionary<Camera, RenderTexture> thumbs = new Dictionary<Camera, RenderTexture>();
        readonly List<Camera> candidates = new List<Camera>();
        Camera thumbCam;
        int nextThumb;

        // Diagnostics: frame intervals in milliseconds, oldest first after `historyAt`.
        readonly float[] frameMs = new float[History];
        int historyAt;

        GUIStyle panel, button, selected, label, muted, title, warn;

        public Menu(Rig rig) { this.rig = rig; }

        public void Toggle()
        {
            Open = !Open;
            picking = -1;
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

        public void Update()
        {
            frameMs[historyAt] = Time.unscaledDeltaTime * 1000f;
            historyAt = (historyAt + 1) % History;
            bool previewing = Open && tab == GameTab;
            if (thumbCam != null && !previewing)
            {
                thumbCam.enabled = false;
                foreach (var rt in thumbs.Values) Object.Destroy(rt);
                thumbs.Clear();
            }
            if (!Open) return;
            // Games re-lock the cursor every frame; keep it free while the menu is up.
            Cursor.lockState = CursorLockMode.None;
            Cursor.visible = true;
            candidates.Clear();
            foreach (var cam in Camera.allCameras)
                if (!rig.IsEye(cam) && cam.targetTexture == null) candidates.Add(cam);
            if (previewing) previews.Run(RenderNextPreview);
        }

        void RenderNextPreview()
        {
            if (thumbCam == null)
            {
                var go = new GameObject("Parallax Camera Preview");
                Object.DontDestroyOnLoad(go);
                thumbCam = go.AddComponent<Camera>();
            }
            if (candidates.Count == 0)
            {
                thumbCam.enabled = false;
                return;
            }
            var cam = candidates[nextThumb++ % candidates.Count];
            if (!thumbs.TryGetValue(cam, out var rt)) thumbs[cam] = rt = new RenderTexture(ThumbW, ThumbH, 16);
            thumbCam.CopyFrom(cam);
            // The head camera renders nothing itself while the eyes take over; preview it with its own settings.
            if (cam == rig.GameCam) rig.RestoreForPreview(thumbCam);
            thumbCam.targetTexture = rt;
            thumbCam.enabled = true;
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
                if (Button(Tabs[i], i == tab, 172)) Select(i);
                GUILayout.Space(8);
            }
            GUILayout.FlexibleSpace();
            if (Button("Close", false, 120)) Toggle();
            GUILayout.EndHorizontal();
            GUILayout.Space(24);
            if (tab == QuickTab) DrawQuick();
            else if (tab == ViewTab) DrawView();
            else if (tab == ControlsTab) DrawControls();
            else if (tab == GameTab) DrawGame();
            else DrawDiagnostics();
            GUILayout.EndArea();
            GUI.matrix = Matrix4x4.identity;
            GUI.backgroundColor = Color.white;

            // A light tick when the pointer moves onto another control.
            if (Event.current.type == EventType.Repaint)
            {
                if (hovered != 0 && hovered != lastHovered) Native.prx_haptic_pulse(1, 0.15f, 0.01f);
                lastHovered = hovered;
                hovered = 0;
            }
        }

        void Select(int i)
        {
            tab = i;
            picking = -1;
            scroll = Vector2.zero;
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

        void DrawView()
        {
            float scale = Settings.RenderScale.Value;
            Stepper("Resolution", Settings.RenderScale, 0.1f, 0.5f, 2f, "{0:0.0}x");
            if (Settings.RenderScale.Value != scale) rig.ApplyRenderScale();
            Toggle("Auto resolution", Settings.AutoRenderScale);
            Stepper("UI distance", Settings.UiDistance, 0.1f, 0.5f, 3f, "{0:0.0} m");
            Stepper("UI width", Settings.UiWidth, 0.1f, 0.5f, 4f, "{0:0.0} m");
            bool menuOnly = Settings.Pointer.Value == PointerMode.MenuOnly;
            Choice("Laser pointer", "With game cursor", "Menu only", ref menuOnly);
            Settings.Pointer.Value = menuOnly ? PointerMode.MenuOnly : PointerMode.Auto;
            Toggle("Wrist glance + trigger opens menu", Settings.WristMenu);
        }

        void DrawControls()
        {
            if (Settings.Input.Value == InputMode.Gamepad)
            {
                Label("Gamepad mode: the controllers act as an Xbox pad. Sticks, triggers, A B X Y, grips as", muted, 0);
                Label("shoulder buttons, stick clicks; menu tap is Start. Switch to Keyboard & mouse in Quick to rebind.", muted, 0);
                return;
            }
            if (picking >= 0)
            {
                DrawKeyPicker();
                return;
            }
            Label("Left stick moves (W A S D), right stick looks. Pick a button to change what it presses.", muted, 0);
            GUILayout.Space(8);
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            BindingColumn("Left hand", Bindings.LeftHand);
            GUILayout.Space(32);
            BindingColumn("Right hand", Bindings.RightHand);
            GUILayout.EndHorizontal();
            var conflict = Bindings.Conflict();
            if (conflict != null) Label(conflict, warn, 0);
        }

        void BindingColumn(string name, int[] inputs)
        {
            GUILayout.BeginVertical(new GUILayoutOption[0]);
            Label(name, muted, 0);
            foreach (int i in inputs)
            {
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                Label(Bindings.Labels[i], label, 220);
                if (Button(Bindings.Entries[i].Value, false, 260)) picking = i;
                GUILayout.EndHorizontal();
            }
            GUILayout.EndVertical();
        }

        void DrawKeyPicker()
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label($"{Bindings.Hand(picking)} {Bindings.Labels[picking]} presses:", label, 0);
            GUILayout.FlexibleSpace();
            if (Button("Cancel", false, 160)) picking = -1;
            GUILayout.EndHorizontal();
            GUILayout.Space(8);
            const int columns = 6;
            var names = Bindings.KeyNames;
            for (int start = 0; start < names.Length && picking >= 0; start += columns)
            {
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                for (int j = start; j < Mathf.Min(start + columns, names.Length); j++)
                {
                    if (Button(names[j], names[j] == Bindings.Entries[picking].Value, 164))
                    {
                        Bindings.Entries[picking].Value = names[j];
                        picking = -1;
                        break;
                    }
                    GUILayout.Space(4);
                }
                GUILayout.EndHorizontal();
                GUILayout.Space(4);
            }
        }

        void DrawGame()
        {
            Label("Head camera: the game camera your view follows.", muted, 0);
            scroll = GUILayout.BeginScrollView(scroll, new[] { GUILayout.Height(Height - 200) });
            bool automatic = Settings.CameraName.Value.Length == 0;
            if (Button(automatic ? "Automatic (on)" : "Automatic", automatic, 0)) PickCamera("");
            GUILayout.Space(8);
            const int columns = 3;
            for (int start = 0; start < candidates.Count; start += columns)
            {
                GUILayout.BeginHorizontal(new GUILayoutOption[0]);
                for (int j = start; j < Mathf.Min(start + columns, candidates.Count); j++)
                {
                    var cam = candidates[j];
                    GUILayout.BeginVertical(new[] { GUILayout.Width(ThumbW) });
                    var rect = GUILayoutUtility.GetRect(ThumbW, ThumbH, new[] { GUILayout.Width(ThumbW), GUILayout.Height(ThumbH) });
                    if (thumbs.TryGetValue(cam, out var rt)) GUI.DrawTexture(rect, rt);
                    else
                    {
                        GUI.backgroundColor = Fill;
                        GUI.Box(rect, GUIContent.none, panel);
                    }
                    if (Button(cam.name, cam == rig.GameCam, ThumbW)) PickCamera(cam.name);
                    GUILayout.EndVertical();
                    GUILayout.Space(16);
                }
                GUILayout.EndHorizontal();
                GUILayout.Space(16);
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
            DrawGraph((float)s.periodMs);
            GUILayout.Space(16);
            Line("Runtime", ((XrState)s.state) + (s.focused != 0 ? ", focused" : "") + $", {1000.0 / System.Math.Max(0.001, s.periodMs):0} Hz");
            Line("Frame time p95", $"{s.p95Ms:0.0} ms of {s.periodMs:0.0} ms");
            Line("Eye resolution", $"{rig.EyeSize.x} x {rig.EyeSize.y}");
            Line("Head camera", rig.GameCam != null ? rig.GameCam.name : "none");

            // Problems with a one-click fix.
            if (s.state == (int)XrState.Running && s.p95Ms > s.periodMs * 1.1)
                Warning("The game misses the headset's frame rate.", "Lower resolution", () =>
                {
                    Settings.RenderScale.Value = Mathf.Max(0.5f, Settings.RenderScale.Value - 0.1f);
                    rig.ApplyRenderScale();
                });
            if (rig.GameCam == null && !Settings.Theater.Value)
                Warning("No game camera found, so the game shows on the theater screen.", "Pick camera", () => Select(GameTab));
            if (Bindings.Conflict() is string conflict)
                Warning(conflict, "Fix bindings", () => Select(ControlsTab));
        }

        // Bars of recent frame intervals against the headset's frame budget (the accent line).
        void DrawGraph(float budgetMs)
        {
            var rect = GUILayoutUtility.GetRect(Width - 64, 160, new[] { GUILayout.Height(160) });
            if (Event.current.type != EventType.Repaint) return;
            float top = Mathf.Max(budgetMs * 2, 1f), bar = rect.width / History;
            GUI.color = Fill;
            GUI.DrawTexture(rect, Texture2D.whiteTexture);
            for (int i = 0; i < History; i++)
            {
                float ms = frameMs[(historyAt + i) % History], h = Mathf.Min(ms / top, 1f) * rect.height;
                GUI.color = ms > budgetMs * 1.1f ? Accent : Muted;
                GUI.DrawTexture(new Rect(rect.x + i * bar, rect.yMax - h, Mathf.Max(1, bar - 1), h), Texture2D.whiteTexture);
            }
            GUI.color = Accent;
            GUI.DrawTexture(new Rect(rect.x, rect.yMax - budgetMs / top * rect.height, rect.width, 2), Texture2D.whiteTexture);
            GUI.color = Color.white;
        }

        void Warning(string text, string fix, System.Action action)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(text, warn, 0);
            GUILayout.FlexibleSpace();
            if (Button(fix, false, 260)) action();
            GUILayout.EndHorizontal();
        }

        void Line(string name, string value)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Label(name, muted, new[] { GUILayout.Width(360), GUILayout.Height(48) });
            GUILayout.Label(value, label, new[] { GUILayout.Height(48) });
            GUILayout.EndHorizontal();
        }

        void Toggle(string name, BepInEx.Configuration.ConfigEntry<bool> entry)
        {
            bool on = entry.Value;
            Choice(name, "Off", "On", ref on);
            entry.Value = on;
        }

        void Choice(string name, string off, string on, ref bool value)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 360);
            if (Button(off, !value, 300)) value = false;
            GUILayout.Space(8);
            if (Button(on, value, 300)) value = true;
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        void Stepper(string name, BepInEx.Configuration.ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 360);
            // Rounded so repeated steps land on clean values.
            if (Button("-", false, 96)) entry.Value = Mathf.Round(Mathf.Max(min, entry.Value - step) * 100) / 100;
            Label(string.Format(format, entry.Value), title, 200);
            if (Button("+", false, 96)) entry.Value = Mathf.Round(Mathf.Min(max, entry.Value + step) * 100) / 100;
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        // width 0 = stretch.
        bool Button(string text, bool on, float width)
        {
            GUI.backgroundColor = on ? Accent : Fill;
            bool clicked = GUILayout.Button(text, on ? selected : button, Size(width));
            GUI.backgroundColor = Color.white;
            if (Event.current.type == EventType.Repaint)
            {
                var r = GUILayoutUtility.GetLastRect();
                if (r.Contains(Event.current.mousePosition)) hovered = r.x * 7919f + r.y + 1;
            }
            if (clicked) Native.prx_haptic_pulse(1, 0.35f, 0.02f);
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
            warn = Style(TextAnchor.MiddleLeft, Accent, Accent, true, false);
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
