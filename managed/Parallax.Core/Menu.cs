using System.Collections.Generic;
using BepInEx.Configuration;
using UnityEngine;

namespace Parallax
{
    // Drawn with IMGUI onto the screen, so it reaches the headset through the UI capture and the laser pointer.
    // Only engine members that survive IL2CPP stripping in typical games are used: no texture creation outside an
    // Optional guard, no style copies, layout options always passed as arrays (Il2CppInterop has no params).
    // Look: "Night Lime" (black, one lime accent, hairlines, mono labels), the same tokens as the desktop app.
    internal class Menu
    {
        static readonly string[] Tabs = { "Quick", "View", "Controls", "Game", "Diagnostics" };
        const int QuickTab = 0, ViewTab = 1, ControlsTab = 2, GameTab = 3;
        const float RefHeight = 1080f, Width = 1100f, Height = 760f, Row = 64f, Pad = 40f;
        const int ThumbW = 320, ThumbH = 180, History = 120;
        static readonly Color Bg = new Color(0.039f, 0.039f, 0.039f, 0.97f), Surface = new Color(0.071f, 0.071f, 0.071f);
        static readonly Color Raised = new Color(0.10f, 0.10f, 0.10f), Line = new Color(0.15f, 0.15f, 0.15f);
        // Edge: outlines of things you can press (3:1 on the panel); Line stays for decorative dividers.
        static readonly Color Edge = new Color(0.369f, 0.369f, 0.369f);
        static readonly Color Ink = new Color(0.95f, 0.95f, 0.95f), Ink2 = new Color(0.64f, 0.64f, 0.64f), Ink3 = new Color(0.502f, 0.502f, 0.502f);
        static readonly Color Lime = new Color(0.776f, 1f, 0.102f), OnLime = new Color(0.039f, 0.039f, 0.039f);
        static readonly Color Danger = new Color(1f, 0.353f, 0.235f);

        readonly Rig rig;
        internal bool Open;
        bool welcome;  // first-run card instead of the tabs
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

        readonly Optional fonts = new Optional("Menu fonts");
        GUIStyle cell, cellOn, label, muted, eyebrow, eyebrowWord, display, value, data, warn, tabStyle, tabOn;

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
                welcome = false;
                Cursor.lockState = savedLock;
                Cursor.visible = savedVisible;
            }
            Native.prx_haptic_pulse(0, 0.4f, 0.03f);
        }

        // -1 is the first-run card (recenter, check the scale, learn the controls).
        public void Show(int page)
        {
            if (!Open) Toggle();
            welcome = page < 0;
            if (!welcome) Select(page);
        }

        public void Update()
        {
            frameMs[historyAt] = Time.unscaledDeltaTime * 1000f;
            historyAt = (historyAt + 1) % History;
            bool previewing = Open && !welcome && tab == GameTab;
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
            Fill(area, Bg);
            Outline(area, Line);
            GUILayout.BeginArea(new Rect(area.x + Pad, area.y + 28, Width - 2 * Pad, Height - 56));
            if (welcome) DrawWelcome();
            else DrawTabs();
            GUILayout.EndArea();
            GUI.matrix = Matrix4x4.identity;

            // A light tick when the pointer moves onto another control.
            if (Event.current.type == EventType.Repaint)
            {
                if (hovered != 0 && hovered != lastHovered) Native.prx_haptic_pulse(1, 0.15f, 0.01f);
                lastHovered = hovered;
                hovered = 0;
            }
        }

        void DrawTabs()
        {
            // Tabs: words with a lime underline under the active one; Close and Reset stay quiet on the right.
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            for (int i = 0; i < Tabs.Length; i++)
            {
                bool on = i == tab;
                if (GUILayout.Button(Tabs[i].ToUpperInvariant(), on ? tabOn : tabStyle, new[] { GUILayout.Height(56) })) Select(i);
                var r = GUILayoutUtility.GetLastRect();
                if (Event.current.type == EventType.Repaint)
                {
                    if (on) Fill(new Rect(r.x, r.yMax - 3, r.width, 3), Lime);
                    Hover(r);
                }
                GUILayout.Space(24);
            }
            GUILayout.FlexibleSpace();
            if (tab != GameTab && Button("Reset", false, 140)) ResetTab();
            GUILayout.Space(8);
            if (Button("Close", false, 140)) Toggle();
            GUILayout.EndHorizontal();
            var line = GUILayoutUtility.GetRect(1, 1, new[] { GUILayout.ExpandWidth(true), GUILayout.Height(1) });
            Fill(line, Line);
            GUILayout.Space(24);

            if (tab == QuickTab) DrawQuick();
            else if (tab == ViewTab) DrawView();
            else if (tab == ControlsTab) DrawControls();
            else if (tab == GameTab) DrawGame();
            else DrawDiagnostics(rig.GetStats());
        }

        void DrawWelcome()
        {
            Eyebrow("00", "WELCOME");
            GUILayout.Label("YOU ARE IN VR.", display, new[] { GUILayout.Height(72) });
            GUILayout.Space(8);
            GUILayout.Label("Three quick checks, then play. This card shows once; the menu is always a double-tap away.", muted, new GUILayoutOption[0]);
            GUILayout.Space(24);

            Step("01", "Stand or sit the way you will play, then set your view straight ahead.");
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Space(72);
            if (Button("Recenter", false, 300)) rig.RequestRecenter();
            GUILayout.EndHorizontal();
            GUILayout.Space(16);

            Step("02", "Doors and people should look life-size. Too small or too big? Adjust the world scale.");
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Space(72);
            StepperControls(Settings.WorldScale, 0.1f, 0.5f, 3f, "{0:0.0}x");
            GUILayout.EndHorizontal();
            GUILayout.Space(16);

            Step("03", "Left stick moves, right stick looks, triggers click. Double-tap the left menu button for this menu.");
            GUILayout.Space(24);

            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Space(72);
            if (Primary("Start playing", 360))
            {
                Settings.FirstRun.Value = false;
                Toggle();
            }
            GUILayout.Space(8);
            if (Button("Show all settings", false, 360))
            {
                Settings.FirstRun.Value = false;
                welcome = false;
            }
            GUILayout.EndHorizontal();
        }

        void Step(string number, string text)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Label(number, eyebrowWord, new[] { GUILayout.Width(72), GUILayout.Height(48) });
            GUILayout.Label(text, label, new GUILayoutOption[0]);
            GUILayout.EndHorizontal();
            GUILayout.Space(8);
        }

        void Select(int i)
        {
            tab = i;
            picking = -1;
            scroll = Vector2.zero;
        }

        // Back to defaults for the settings this tab shows.
        void ResetTab()
        {
            ConfigEntryBase[] entries;
            if (tab == QuickTab) entries = new ConfigEntryBase[] { Settings.Theater, Settings.WorldScale, Settings.Turning, Settings.Vignette, Settings.Input };
            else if (tab == ViewTab)
                entries = new ConfigEntryBase[] { Settings.Height, Settings.RenderScale, Settings.AutoRenderScale, Settings.UiDistance, Settings.UiWidth, Settings.Pointer, Settings.WristMenu };
            else if (tab == ControlsTab) entries = Bindings.Entries;
            else return;
            foreach (var e in entries) e.BoxedValue = e.DefaultValue;
            picking = -1;
            if (tab == ViewTab) rig.ApplyRenderScale();
        }

        void DrawQuick()
        {
            bool theater = Settings.Theater.Value;
            Choice("View", "Stereo", "Theater", ref theater);
            Settings.Theater.Value = theater;

            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label("Position", label, 360);
            if (Button("Recenter", false, 300)) rig.RequestRecenter();
            GUILayout.EndHorizontal();
            GUILayout.Space(12);

            Stepper("World scale", Settings.WorldScale, 0.1f, 0.5f, 3f, "{0:0.0}x");
            Options("Turning", Settings.Turning, new[] { TurnMode.Smooth, TurnMode.Snap15, TurnMode.Snap30, TurnMode.Snap45 },
                new[] { "Smooth", "Snap 15°", "Snap 30°", "Snap 45°" }, null);
            Options("Comfort vignette", Settings.Vignette, new[] { VignetteLevel.Off, VignetteLevel.Low, VignetteLevel.High },
                new[] { "Off", "Low", "High" }, VignetteLevel.Off);

            bool kbm = Settings.Input.Value == InputMode.KeyboardMouse;
            Choice("Controls", "Gamepad", "Keyboard & mouse", ref kbm);
            Settings.Input.Value = kbm ? InputMode.KeyboardMouse : InputMode.Gamepad;
        }

        void DrawView()
        {
            Stepper("Height", Settings.Height, 0.05f, -1f, 1f, "{0:+0.00;-0.00;0.00} m");
            float scale = Settings.RenderScale.Value;
            Stepper("Resolution", Settings.RenderScale, 0.1f, 0.5f, 2f, "{0:0.0}x");
            if (Settings.RenderScale.Value != scale) rig.ApplyRenderScale();
            Toggle("Auto resolution", Settings.AutoRenderScale);
            Stepper("UI distance", Settings.UiDistance, 0.1f, 0.5f, 3f, "{0:0.0} m");
            Stepper("UI width", Settings.UiWidth, 0.1f, 0.5f, 4f, "{0:0.0} m");
            bool menuOnly = Settings.Pointer.Value == PointerMode.MenuOnly;
            Choice("Laser pointer", "With game cursor", "Menu only", ref menuOnly);
            Settings.Pointer.Value = menuOnly ? PointerMode.MenuOnly : PointerMode.Auto;
            Toggle("Wrist glance menu", Settings.WristMenu);
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
            BindingColumn("01", "Left hand", Bindings.LeftHand);
            GUILayout.Space(40);
            BindingColumn("02", "Right hand", Bindings.RightHand);
            GUILayout.EndHorizontal();
            var conflict = Bindings.Conflict();
            if (conflict != null) Label(conflict, warn, 0);
        }

        void BindingColumn(string number, string name, int[] inputs)
        {
            GUILayout.BeginVertical(new GUILayoutOption[0]);
            Eyebrow(number, name.ToUpperInvariant());
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
            scroll = GUILayout.BeginScrollView(scroll, new[] { GUILayout.Height(Height - 280) });
            bool automatic = Settings.CameraName.Value.Length == 0;
            if (Button("Automatic", automatic, ThumbW)) PickCamera("");
            GUILayout.Space(16);
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
                    else Fill(rect, Surface);
                    Outline(rect, cam == rig.GameCam ? Lime : Line);
                    // Lime fill only for a camera the player picked; the automatic pick gets the lime outline above.
                    if (Button(cam.name, !automatic && cam == rig.GameCam, ThumbW)) PickCamera(cam.name);
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

        void DrawDiagnostics(Stats s)
        {
            DrawGraph((float)s.periodMs);
            GUILayout.Space(16);
            InfoRow("Headset", $"{(XrState)s.state}, {1000.0 / System.Math.Max(0.001, s.periodMs):0} Hz");
            InfoRow("Frame time p95", $"{s.p95Ms:0.0} ms of {s.periodMs:0.0} ms");
            InfoRow("Eye resolution", $"{rig.EyeSize.x} x {rig.EyeSize.y}");

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

        // Bars of recent frame intervals against the headset's frame budget: lime within it, danger over it.
        void DrawGraph(float budgetMs)
        {
            var rect = GUILayoutUtility.GetRect(Width - 2 * Pad, 160, new[] { GUILayout.Height(160) });
            if (Event.current.type != EventType.Repaint) return;
            float top = Mathf.Max(budgetMs * 1.5f, 1f), bar = rect.width / History;
            Fill(rect, Surface);
            Outline(rect, Line);
            for (int i = 0; i < History; i++)
            {
                float ms = frameMs[(historyAt + i) % History], h = Mathf.Min(ms / top, 1f) * rect.height;
                Fill(new Rect(rect.x + i * bar, rect.yMax - h, Mathf.Max(1, bar - 2), h), ms > budgetMs * 1.1f ? Danger : Lime);
            }
            float budgetY = rect.yMax - budgetMs / top * rect.height;
            Fill(new Rect(rect.x, budgetY, rect.width, 1), Ink2);
            GUI.Label(new Rect(rect.x + 12, budgetY - 30, 400, 28), $"HEADSET BUDGET {budgetMs:0.0} MS", eyebrow);
        }

        void Warning(string text, string fix, System.Action action)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(text, warn, 0);
            GUILayout.FlexibleSpace();
            if (Button(fix, false, 260)) action();
            GUILayout.EndHorizontal();
        }

        void InfoRow(string name, string text)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Label(name, muted, new[] { GUILayout.Width(360), GUILayout.Height(48) });
            GUILayout.Label(text, data, new[] { GUILayout.Height(48) });
            GUILayout.EndHorizontal();
        }

        void Toggle(string name, ConfigEntry<bool> entry)
        {
            bool on = entry.Value;
            Choice(name, "Off", "On", ref on, true);
            entry.Value = on;
        }

        // Segmented control: two cells side by side, the selected one lime.
        void Choice(string name, string off, string on, ref bool v, bool offIsNeutral = false)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 360);
            if (Button(off, !v && !offIsNeutral, 300)) v = false;
            if (!v && offIsNeutral && Event.current.type == EventType.Repaint) Outline(GUILayoutUtility.GetLastRect(), Ink);
            if (Button(on, v, 300)) v = true;
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        // Segmented control over an enum setting, cells sharing the 600 px value column; `neutral` (an Off) is not lime.
        void Options<T>(string name, ConfigEntry<T> entry, T[] values, string[] labels, T? neutral) where T : struct, System.Enum
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 360);
            float width = 600f / values.Length;
            for (int i = 0; i < values.Length; i++)
            {
                bool on = entry.Value.Equals(values[i]), lit = on && !(neutral.HasValue && neutral.Value.Equals(values[i]));
                if (Button(labels[i], lit, width)) entry.Value = values[i];
                if (on && !lit && Event.current.type == EventType.Repaint) Outline(GUILayoutUtility.GetLastRect(), Ink);
            }
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        void Stepper(string name, ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            Label(name, label, 360);
            StepperControls(entry, step, min, max, format);
            GUILayout.EndHorizontal();
            GUILayout.Space(12);
        }

        void StepperControls(ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            // Rounded so repeated steps land on clean values.
            if (Button("−", false, 96)) entry.Value = Mathf.Round(Mathf.Max(min, entry.Value - step) * 100) / 100;
            Label(string.Format(format, entry.Value), value, 200);
            if (Button("+", false, 96)) entry.Value = Mathf.Round(Mathf.Min(max, entry.Value + step) * 100) / 100;
        }

        // Secondary cell: hairline outline, raised on hover; `on` fills it lime (selected state).
        bool Button(string text, bool on, float width)
        {
            GUI.backgroundColor = on ? Lime : Surface;
            bool clicked = GUILayout.Button(text, on ? cellOn : cell, Size(width));
            GUI.backgroundColor = Color.white;
            var r = GUILayoutUtility.GetLastRect();
            if (Event.current.type == EventType.Repaint)
            {
                bool over = r.Contains(Event.current.mousePosition);
                if (!on) Outline(r, over ? Ink : Edge);
                Hover(r);
            }
            if (clicked) Native.prx_haptic_pulse(1, 0.35f, 0.02f);
            return clicked;
        }

        // The one primary action of a view: lime fill with an arrow.
        bool Primary(string text, float width) => Button(text.ToUpperInvariant() + "  →", true, width);

        void Hover(Rect r)
        {
            if (r.Contains(Event.current.mousePosition)) hovered = r.x * 7919f + r.y + 1;
        }

        void Eyebrow(string number, string word)
        {
            GUILayout.BeginHorizontal(new GUILayoutOption[0]);
            GUILayout.Label(number + "  //  ", eyebrow, new[] { GUILayout.Height(32), GUILayout.ExpandWidth(false) });
            GUILayout.Label(word, eyebrowWord, new[] { GUILayout.Height(32), GUILayout.ExpandWidth(false) });
            GUILayout.EndHorizontal();
        }

        void Label(string text, GUIStyle style, float width) => GUILayout.Label(text, style, Size(width));

        static GUILayoutOption[] Size(float width) =>
            width > 0 ? new[] { GUILayout.Height(Row), GUILayout.Width(width) } : new[] { GUILayout.Height(Row) };

        static void Fill(Rect r, Color c)
        {
            var saved = GUI.color;
            GUI.color = c;
            GUI.DrawTexture(r, Texture2D.whiteTexture);
            GUI.color = saved;
        }

        static void Outline(Rect r, Color c)
        {
            Fill(new Rect(r.x, r.y, r.width, 1), c);
            Fill(new Rect(r.x, r.yMax - 1, r.width, 1), c);
            Fill(new Rect(r.x, r.y, 1, r.height), c);
            Fill(new Rect(r.xMax - 1, r.y, 1, r.height), c);
        }

        void EnsureStyles()
        {
            if (cell != null) return;
            Font displayFont = null, monoFont = null;
            // OS fonts every Windows has: Impact (heavy condensed, like the app's Anton) for headlines, Consolas for
            // labels and numbers; the IMGUI default otherwise.
            fonts.Run(() =>
            {
                displayFont = Font.CreateDynamicFontFromOSFont("Impact", 60);
                monoFont = Font.CreateDynamicFontFromOSFont("Consolas", 22);
            });
            cell = Style(TextAnchor.MiddleCenter, Ink, 26, null, true);
            cellOn = Style(TextAnchor.MiddleCenter, OnLime, 26, null, true);
            label = Style(TextAnchor.MiddleLeft, Ink, 28, null, false);
            muted = Style(TextAnchor.MiddleLeft, Ink2, 26, null, false);
            warn = Style(TextAnchor.MiddleLeft, Danger, 26, null, false);
            label.wordWrap = muted.wordWrap = warn.wordWrap = true;
            value = Style(TextAnchor.MiddleCenter, Ink, 28, monoFont, false);
            data = Style(TextAnchor.MiddleLeft, Ink, 26, monoFont, false);
            display = Style(TextAnchor.MiddleLeft, Ink, 60, displayFont, false);
            eyebrow = Style(TextAnchor.MiddleLeft, Ink3, 24, monoFont, false);
            eyebrowWord = Style(TextAnchor.MiddleLeft, Lime, 24, monoFont, false);
            tabStyle = Style(TextAnchor.MiddleLeft, Ink3, 24, monoFont, false);
            tabStyle.hover.textColor = Ink;
            tabOn = Style(TextAnchor.MiddleLeft, Ink, 24, monoFont, false);
        }

        // Filled styles use the built-in white texture, tinted by GUI.backgroundColor when drawn (see Button).
        static GUIStyle Style(TextAnchor align, Color text, int size, Font font, bool filled)
        {
            var s = new GUIStyle { fontSize = size, alignment = align, font = font };
            s.normal.textColor = s.hover.textColor = s.active.textColor = text;
            if (filled) s.normal.background = s.hover.background = s.active.background = Texture2D.whiteTexture;
            return s;
        }
    }
}
