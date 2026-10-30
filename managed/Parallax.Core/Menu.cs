using System.Collections.Generic;
using System.Linq;
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
        const float RefHeight = 1080f, Width = 1100f, Height = 800f, Row = 64f, Pad = 48f;
        const int ThumbW = 304, ThumbH = 171, History = 120;
        static readonly Color Bg = new Color(0.039f, 0.039f, 0.039f, 0.97f), Surface = new Color(0.071f, 0.071f, 0.071f);
        static readonly Color Raised = new Color(0.10f, 0.10f, 0.10f), Line = new Color(0.15f, 0.15f, 0.15f), HoverFill = new Color(0.19f, 0.19f, 0.19f);
        // Edge: outlines of things you can press (3:1 on the panel); Line stays for decorative dividers.
        static readonly Color Edge = new Color(0.369f, 0.369f, 0.369f);
        static readonly Color Ink = new Color(0.95f, 0.95f, 0.95f), Ink2 = new Color(0.64f, 0.64f, 0.64f), Ink3 = new Color(0.502f, 0.502f, 0.502f);
        static readonly Color Lime = new Color(0.776f, 1f, 0.102f), OnLime = new Color(0.039f, 0.039f, 0.039f);
        static readonly Color Danger = new Color(1f, 0.353f, 0.235f);
        // Type scale (px at 1080 reference height); the app's scale times ~2 for reading at panel distance.
        const int Display = 60, Body = 28, Small = 24;

        readonly Rig rig;
        internal bool Open;
        bool welcome;  // first-run card instead of the tabs
        int tab;
        CursorLockMode savedLock;
        bool savedVisible;
        int picking = -1;  // binding being changed in the Controls tab
        int gesture;       // which gesture's bindings the Controls tab shows
        float hovered, lastHovered;
        // The laser pointer, read from the controller rather than the OS mouse, so the menu has it first whatever window
        // is in front: screen pixels from the top-left, and a trigger pull not yet handled by a repaint. The GUI matrix's
        // offset and scale map it into the menu's coordinates.
        Vector2 pointer, origin;
        float scale = 1;
        bool pointing, triggerDown, press;

        // Game tab: one hidden camera renders a preview of one candidate per frame.
        readonly Optional previews = new Optional("Camera previews");
        readonly Dictionary<int, RenderTexture> thumbs = new Dictionary<int, RenderTexture>();
        readonly List<Camera> candidates = new List<Camera>();
        Camera thumbCam;
        int nextThumb;

        // Diagnostics: frame intervals in milliseconds, oldest first after `historyAt`.
        readonly float[] frameMs = new float[History];
        int historyAt;

        readonly Optional fonts = new Optional("Menu fonts");
        GUIStyle imageStyle;
        readonly MenuLayout ui = new MenuLayout();
        readonly Dictionary<string, GUIContent> contents = new Dictionary<string, GUIContent>();
        readonly Dictionary<int, GUIContent> images = new Dictionary<int, GUIContent>();
        int camPage;
        GUIStyle cell, cellOn, label, muted, eyebrow, eyebrowWord, display, value, data, warn, tabStyle, tabOn;

        // Where the menu was last drawn, in screen pixels from the top-left.
        internal Rect ScreenRect;
        Rect hoverRect;

        public Menu(Rig rig) { this.rig = rig; }

        public void Toggle()
        {
            Open = !Open;
            picking = -1;
            press = false;
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

        internal void Point(bool active, Vector2 at, float trigger)
        {
            pointing = active;
            pointer = at;
            bool held = triggerDown ? trigger > 0.4f : trigger > 0.6f;
            if (active && held && !triggerDown && Open) press = true;
            triggerDown = held;
        }

        void Matrix(Vector2 at, float k)
        {
            GUI.matrix = Matrix4x4.TRS(at, Quaternion.identity, new Vector3(k, k, 1));
            origin = at;
            scale = k;
        }

        // Where the pointer is in the current GUI coordinates; the OS mouse when no controller points at the panel.
        Vector2 Mouse => pointing ? (pointer - origin) / scale : Event.current.mousePosition;

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
                if (!rig.IsEye(cam) && Rig.Candidate(cam)) candidates.Add(cam);
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
            if (!thumbs.TryGetValue(cam.GetInstanceID(), out var rt)) thumbs[cam.GetInstanceID()] = rt = new RenderTexture(ThumbW, ThumbH, 16);
            thumbCam.CopyFrom(cam);
            // The head camera renders nothing itself while the eyes take over; preview it with its own settings.
            if (cam == rig.GameCam) rig.RestoreForPreview(thumbCam);
            thumbCam.targetTexture = rt;
            thumbCam.enabled = true;
        }

        public void Draw()
        {
            // Only the events the menu acts on: drawing (which also takes the laser pointer's clicks) and a desktop
            // mouse click.
            var type = Event.current.type;
            if (!Open || (type != EventType.Repaint && (type != EventType.MouseDown || pointing))) return;
            EnsureStyles();
            float k = Screen.height / RefHeight;
            Matrix(Vector2.zero, k);
            var area = new Rect((Screen.width / k - Width) / 2, (RefHeight - Height) / 2, Width, Height);
            ScreenRect = new Rect(area.x * k, area.y * k, area.width * k, area.height * k);
            Fill(area, Bg);
            Outline(area, Line);
            // The content's origin moved by the matrix, so the layout starts at (0, 0).
            Matrix(new Vector2((area.x + Pad) * k, (area.y + 32) * k), k);
            ui.Run(Width - 2 * Pad, () =>
            {
                if (welcome) DrawWelcome();
                else DrawTabs();
                // In a headset a hairline is lost: the control under the laser gets a lighter fill and a 2 px ring.
                if (hoverRect.width > 0 && Repainting) Outline(hoverRect, Ink, 2);
                hoverRect = default;
            });
            Matrix(Vector2.zero, 1);
            if (contents.Count > 512) contents.Clear();  // changing numbers (diagnostics) make new strings every frame

            // A light tick when the pointer moves onto another control.
            if (Event.current.type == EventType.Repaint)
            {
                if (hovered != 0 && hovered != lastHovered) Native.prx_haptic_pulse(1, 0.15f, 0.01f);
                lastHovered = hovered;
                hovered = 0;
                press = false;
            }
        }

        void DrawTabs()
        {
            // Tabs: words with a lime underline under the active one; Close and Reset stay quiet on the right.
            ui.BeginRow();
            for (int i = 0; i < Tabs.Length; i++)
            {
                bool on = i == tab;
                string word = Tabs[i].ToUpperInvariant();
                var style = on ? tabOn : tabStyle;
                var r = ui.Take(Natural(word, style), Row);
                Text(r, word, style, true);
                if (Clicked(r)) Select(i);
                if (Repainting)
                {
                    if (on) Fill(new Rect(r.x, r.yMax - 3, r.width, 3), Lime);
                    Hover(r);
                }
                ui.Gap(24);
            }
            ui.Flex();
            // Only tabs with settings have something to reset.
            if (tab <= ControlsTab && Button("Reset", false, 128)) ResetTab();
            ui.Gap(8);
            if (Button("Close", false, 128)) Toggle();
            ui.Gap(2);  // keeps the right outline inside the area's clip
            ui.EndRow();
            Fill(ui.Take(0, 1), Line);
            ui.Gap(24);

            if (tab == QuickTab) DrawQuick();
            else if (tab == ViewTab) DrawView();
            else if (tab == ControlsTab) DrawControls();
            else if (tab == GameTab) DrawGame();
            else DrawDiagnostics(rig.GetStats());
        }

        void DrawWelcome()
        {
            Label("YOU ARE IN VR.", display, 0, 72);
            ui.Gap(8);
            Label("Three quick checks, then play. The menu is always a double-tap away.", muted, 0, 48);
            ui.Gap(24);

            Step("01", "Stand or sit the way you will play, then set your view straight ahead.");
            ui.BeginRow();
            ui.Gap(72);
            if (Button("Recenter", false, 300)) rig.RequestRecenter();
            ui.EndRow();
            ui.Gap(16);

            Step("02", "Doors and people should look life-size. Too small or too big? Adjust the world scale.");
            ui.BeginRow();
            ui.Gap(72);
            StepperControls(Settings.WorldScale, 0.1f, 0.5f, 3f, "{0:0.0}×");
            ui.EndRow();
            ui.Gap(16);

            Step("03", "Left stick moves, right stick looks, triggers click. Double-tap the left menu button for this menu.");
            ui.Gap(24);

            ui.BeginRow();
            ui.Gap(72);
            if (Primary("Start playing", 360))
            {
                Settings.FirstRun.Value = false;
                Toggle();
            }
            ui.Gap(8);
            if (Button("Show all settings", false, 360))
            {
                Settings.FirstRun.Value = false;
                welcome = false;
            }
            ui.EndRow();
        }

        void Step(string number, string text)
        {
            ui.BeginRow();
            Label(number, eyebrowWord, 72, 48);
            Text(ui.Take(0, 48), text, label);
            ui.EndRow();
            ui.Gap(8);
        }

        void Select(int i)
        {
            tab = i;
            picking = -1;
            camPage = 0;
        }

        // The player's settings for all games, kept by Parallax.exe in the same "[Section]" / "Key = Value" format;
        // empty when the app never saved any.
        static Dictionary<string, string> AllGamesValues()
        {
            var values = new Dictionary<string, string>();
            string section = "";
            string path = System.IO.Path.Combine(System.Environment.GetFolderPath(System.Environment.SpecialFolder.ApplicationData), "Parallax\\defaults.cfg");
            try
            {
                foreach (var raw in System.IO.File.ReadAllLines(path))
                {
                    string line = raw.Trim();
                    int eq = line.IndexOf('=');
                    if (line.StartsWith("[") && line.EndsWith("]")) section = line.Substring(1, line.Length - 2);
                    else if (eq > 0 && !line.StartsWith("#")) values[section + "/" + line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
                }
            }
            catch (System.Exception) { }
            return values;
        }

        // Back to the player's all-games settings (else the plugin's defaults) for the settings this tab shows.
        void ResetTab()
        {
            ConfigEntryBase[] entries;
            if (tab == QuickTab) entries = new ConfigEntryBase[] { Settings.Display, Settings.Depth, Settings.ScreenYaw, Settings.ScreenPitch, Settings.ScreenDistance, Settings.ScreenWidth, Settings.WorldScale, Settings.Turning, Settings.Vignette, Settings.Input, Settings.HeadAim };
            else if (tab == ViewTab)
                entries = new ConfigEntryBase[] { Settings.Height, Settings.RenderScale, Settings.AutoRenderScale, Settings.UiDistance, Settings.UiWidth, Settings.Pointer, Settings.WristMenu, Settings.UiBlack };
            else if (tab == ControlsTab) entries = Bindings.Entries.SelectMany(g => g).Where(e => e != null).ToArray();
            else return;
            var allGames = AllGamesValues();
            foreach (var e in entries)
            {
                if (allGames.TryGetValue(e.Definition.Section + "/" + e.Definition.Key, out var v)) e.SetSerializedValue(v);
                else e.BoxedValue = e.DefaultValue;
            }
            picking = -1;
            if (tab == ViewTab) rig.ApplyRenderScale();
        }

        void DrawQuick()
        {
            Options("Display", Settings.Display, new[] { DisplayMode.Stereo, DisplayMode.Parallax, DisplayMode.Theater },
                new[] { "Stereo", "Parallax", "Theater" }, null);
            if (Settings.Display.Value == DisplayMode.Parallax) Stepper("3D depth", Settings.Depth, 0.25f, 0f, 3f, "{0:0.00}×");
            if (Settings.Display.Value != DisplayMode.Stereo)
            {
                ui.BeginRow();
                Label("Screen", label, 360);
                if (Button("Move & resize", false, 300))
                {
                    Toggle();
                    rig.AdjustScreen(true);
                }
                if (Button("Reset", false, 200)) rig.ResetScreen();
                ui.EndRow();
                ui.Gap(12);
            }

            ui.BeginRow();
            Label("Position", label, 360);
            if (Button("Recenter", false, 300)) rig.RequestRecenter();
            ui.EndRow();
            ui.Gap(12);

            Stepper("World scale", Settings.WorldScale, 0.1f, 0.5f, 3f, "{0:0.0}×");
            Options("Turning", Settings.Turning, new[] { TurnMode.Smooth, TurnMode.Snap15, TurnMode.Snap30, TurnMode.Snap45 },
                new[] { "Smooth", "Snap 15°", "Snap 30°", "Snap 45°" }, null);
            Options("Comfort vignette", Settings.Vignette, new[] { VignetteLevel.Off, VignetteLevel.Low, VignetteLevel.High },
                new[] { "Off", "Low", "High" }, VignetteLevel.Off);

            // Same order as the app.
            Options("Controls", Settings.Input, new[] { InputMode.Auto, InputMode.Gamepad, InputMode.KeyboardMouse, InputMode.Off },
                new[] { "Auto", "Gamepad", "Keyboard & mouse", "Off" }, InputMode.Off);
            Options("Head aim", Settings.HeadAim, new[] { AimMode.Auto, AimMode.On, AimMode.Off }, new[] { "Auto", "On", "Off" }, AimMode.Off);
        }

        void DrawView()
        {
            Stepper("Height", Settings.Height, 0.05f, -1f, 1f, "{0:+0.00;-0.00;0.00} m");
            float scale = Settings.RenderScale.Value;
            Stepper("Resolution", Settings.RenderScale, 0.1f, 0.5f, 2f, "{0:0.0}×");
            if (Settings.RenderScale.Value != scale) rig.ApplyRenderScale();
            Toggle("Auto resolution", Settings.AutoRenderScale);
            Stepper("UI distance", Settings.UiDistance, 0.1f, 0.5f, 3f, "{0:0.0} m");
            Stepper("UI width", Settings.UiWidth, 0.1f, 0.5f, 4f, "{0:0.0} m");
            bool menuOnly = Settings.Pointer.Value == PointerMode.MenuOnly;
            Choice("Laser pointer", "With game cursor", "Menu only", ref menuOnly);
            Settings.Pointer.Value = menuOnly ? PointerMode.MenuOnly : PointerMode.Auto;
            Toggle("Wrist glance menu", Settings.WristMenu);
            Options("UI black", Settings.UiBlack, new[] { UiBackground.Auto, UiBackground.SeeThrough, UiBackground.Keep },
                new[] { "Auto", "See-through", "Keep" }, null);
        }

        void DrawControls()
        {
            if (Settings.InputNow == InputMode.Off)
            {
                Label("Controls are off: the controllers only work this menu and the laser pointer. Play with your own", muted, 0);
                Label("keyboard, mouse or gamepad. Switch Controls in Quick to use the controllers in the game.", muted, 0);
                return;
            }
            if (Settings.InputNow == InputMode.Gamepad)
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
            ui.BeginRow();
            for (int g = 0; g < Bindings.Gestures.Length; g++)
            {
                if (Button(Bindings.Gestures[g], g == gesture, 220)) gesture = g;
                ui.Gap(4);
            }
            ui.EndRow();
            Label(gesture == Bindings.Press ? "Left stick moves (W A S D), right stick looks. Pick a button to change it."
                : gesture == Bindings.Double ? "Two quick presses. The first press still does its own key."
                : gesture == Bindings.Hold ? "Held for half a second. A short press then taps its own key on release."
                : "Pressed while the same hand's grip is held. The grip's own key lets go meanwhile.", muted, 0);
            ui.BeginRow();
            BindingColumn("01", "Left hand", Bindings.LeftHand);
            ui.Gap(40);
            BindingColumn("02", "Right hand", Bindings.RightHand);
            ui.EndRow();
            var conflict = Bindings.Conflict();
            if (conflict != null) Label(conflict, warn, 0);
        }

        void BindingColumn(string number, string name, int[] inputs)
        {
            ui.BeginColumn();
            Eyebrow(number, name.ToUpperInvariant());
            foreach (int i in inputs)
            {
                var entry = Bindings.Entries[gesture][i];
                if (entry == null) continue;
                ui.BeginRow();
                Label(Bindings.Name(gesture, i), label, 260);
                if (Button(entry.Value, false, 220)) picking = i;
                ui.EndRow();
            }
            ui.EndColumn();
        }

        void DrawKeyPicker()
        {
            ui.BeginRow();
            var picked = Bindings.Entries[gesture][picking];
            Label($"{Bindings.Hand(picking)} {Bindings.Name(gesture, picking)} presses:", label, 0);
            ui.Flex();
            if (Button("Cancel", false, 160)) picking = -1;
            ui.EndRow();
            ui.Gap(8);
            const int columns = 6;
            var names = Bindings.KeyNames;
            for (int start = 0; start < names.Length && picking >= 0; start += columns)
            {
                ui.BeginRow();
                for (int j = start; j < Mathf.Min(start + columns, names.Length); j++)
                {
                    if (Button(names[j], names[j] == picked.Value, 160))
                    {
                        picked.Value = names[j];
                        picking = -1;
                        break;
                    }
                    ui.Gap(4);
                }
                ui.EndRow();
                ui.Gap(4);
            }
        }

        void DrawGame()
        {
            ui.BeginRow();
            Label("In your view? Point at it, pull the trigger. B stops.", muted, 680);
            ui.Flex();
            int hidden = rig.Hider.Count;
            if (hidden > 0 && Button(hidden == 1 ? "Show 1 hidden" : $"Show {hidden} hidden", false, 260)) rig.Hider.ShowAll();
            ui.Gap(4);
            if (Button("Hide objects", false, 260))
            {
                Toggle();
                rig.Hider.Start();
            }
            ui.EndRow();
            ui.Gap(16);
            Label("Game camera: the camera your view follows.", muted, 0);
            bool automatic = Settings.CameraName.Value.Length == 0;
            // Three cameras at a time; more pages through them (no scroll view: it is stripped in some IL2CPP games).
            const int columns = 3;
            int pages = Mathf.Max(1, (candidates.Count + columns - 1) / columns);
            camPage %= pages;
            ui.BeginRow();
            if (Button("Automatic", automatic, ThumbW)) PickCamera("");
            ui.Flex();
            if (pages > 1 && Button($"More cameras {camPage + 1}/{pages}", false, 360)) camPage = (camPage + 1) % pages;
            ui.EndRow();
            ui.Gap(16);
            ui.BeginRow();
            for (int j = camPage * columns; j < Mathf.Min((camPage + 1) * columns, candidates.Count); j++)
            {
                var cam = candidates[j];
                ui.BeginColumn(ThumbW);
                var rect = ui.Take(ThumbW, ThumbH);
                Fill(rect, Surface);
                if (Repainting && thumbs.TryGetValue(cam.GetInstanceID(), out var rt)) Image(rect, rt);
                Outline(rect, cam == rig.GameCam ? Lime : Line);
                // Lime fill only for a camera the player picked; the automatic pick gets the lime outline above.
                if (Button(cam.name, !automatic && cam == rig.GameCam, ThumbW, true)) PickCamera(cam.name);
                ui.EndColumn();
                ui.Gap(16);
            }
            ui.EndRow();
        }

        void PickCamera(string name)
        {
            Settings.CameraName.Value = name;
            rig.ReleaseCamera();
        }

        void DrawDiagnostics(Stats s)
        {
            DrawGraph((float)s.periodMs);
            ui.Gap(16);
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
            if (rig.GameCam == null && Settings.Display.Value != DisplayMode.Theater)
                Warning("No game camera found, so the game shows on the theater screen.", "Pick camera", () => Select(GameTab));
            if (Bindings.Conflict() is string conflict)
                Warning(conflict, "Fix bindings", () => Select(ControlsTab));
        }

        // Bars of recent frame intervals against the headset's frame budget: lime within it, danger over it.
        void DrawGraph(float budgetMs)
        {
            var rect = ui.Take(0, 160);
            if (!Repainting) return;
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
            Text(new Rect(rect.x + 12, budgetY - 30, 400, 28), $"HEADSET BUDGET {budgetMs:0.0} MS", eyebrow);
        }

        void Warning(string text, string fix, System.Action action)
        {
            ui.BeginRow();
            Label(text, warn, 0);
            ui.Flex();
            if (Button(fix, false, 260)) action();
            ui.EndRow();
        }

        void InfoRow(string name, string text)
        {
            ui.BeginRow();
            Label(name, muted, 360, 48);
            Text(ui.Take(0, 48), text, data);
            ui.EndRow();
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
            ui.BeginRow();
            Label(name, label, 360);
            if (Button(off, !v && !offIsNeutral, 300)) v = false;
            if (!v && offIsNeutral && Repainting) Outline(ui.Last, Ink);
            if (Button(on, v, 300)) v = true;
            ui.EndRow();
            ui.Gap(12);
        }

        // Segmented control over an enum setting, cells sharing the 600 px value column; `neutral` (an Off) is not lime.
        void Options<T>(string name, ConfigEntry<T> entry, T[] values, string[] labels, T? neutral) where T : struct, System.Enum
        {
            ui.BeginRow();
            Label(name, label, 360);
            // Each option as wide as its text, the rest of 600 shared out as padding (at least 12 each), so a long
            // label ("Keyboard & mouse") never touches its neighbours and the row keeps the column's right edge.
            float[] text = labels.Select(l => Natural(l.ToUpperInvariant(), cell)).ToArray();
            float pad = Mathf.Max(12f, (600f - text.Sum()) / text.Length);
            for (int i = 0; i < values.Length; i++)
            {
                bool on = entry.Value.Equals(values[i]), lit = on && !(neutral.HasValue && neutral.Value.Equals(values[i]));
                if (Button(labels[i], lit, text[i] + pad)) entry.Value = values[i];
                if (on && !lit && Repainting) Outline(ui.Last, Ink);
            }
            ui.EndRow();
            ui.Gap(12);
        }

        void Stepper(string name, ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            ui.BeginRow();
            Label(name, label, 360);
            StepperControls(entry, step, min, max, format);
            ui.EndRow();
            ui.Gap(12);
        }

        void StepperControls(ConfigEntry<float> entry, float step, float min, float max, string format)
        {
            // Rounded so repeated steps land on clean values.
            if (Button("−", false, 96)) entry.Value = Mathf.Round(Mathf.Max(min, entry.Value - step) * 100) / 100;
            Label(string.Format(format, entry.Value), value, 200);
            if (Button("+", false, 96)) entry.Value = Mathf.Round(Mathf.Min(max, entry.Value + step) * 100) / 100;
        }

        // Secondary cell: edge outline, ink outline on hover; `on` fills it lime (selected state). Labels are UPPERCASE
        // like the app's buttons; `data` (a camera object name) is shown as the game wrote it.
        bool Button(string text, bool on, float width, bool data = false)
        {
            string shown = data ? text : text.ToUpperInvariant();
            var style = on ? cellOn : cell;
            var r = ui.Take(width > 0 ? width : Natural(shown, style) + 48, Row);
            if (Repainting)
            {
                bool over = r.Contains(Mouse);
                GUI.backgroundColor = on ? Lime : over ? HoverFill : Surface;
                style.Draw(r, Content(shown), over, false, false, false);
                GUI.backgroundColor = Color.white;
                if (!on) Outline(r, Edge);
                // Outlined after everything else, so the next cell's edge cannot cover part of it.
                if (over) hoverRect = r;
                Hover(r);
            }
            bool clicked = Clicked(r);
            if (clicked) Native.prx_haptic_pulse(1, 0.35f, 0.02f);
            return clicked;
        }

        // The one primary action of a view: lime fill.
        bool Primary(string text, float width) => Button(text, true, width);

        void Hover(Rect r)
        {
            if (r.Contains(Mouse)) hovered = r.x * 7919f + r.y + 1;
        }

        void Eyebrow(string number, string word)
        {
            ui.BeginRow();
            Label(number + " // ", eyebrow, 0, 32);
            Label(word, eyebrowWord, 0, 32);
            ui.EndRow();
        }

        // Width 0: the text's own width in a row, the column's width in a column.
        void Label(string text, GUIStyle style, float width, float height = Row) =>
            Text(ui.Take(width > 0 ? width : ui.InRow ? Natural(text, style) : 0, height), text, style);

        void Text(Rect r, string text, GUIStyle style, bool hover = false)
        {
            if (Repainting) style.Draw(r, Content(text), hover && r.Contains(Mouse), false, false, false);
        }

        // Camera previews through an image-only style: GUI.DrawTexture is stripped in some IL2CPP games.
        void Image(Rect r, Texture texture)
        {
            if (!images.TryGetValue(texture.GetInstanceID(), out var content)) images[texture.GetInstanceID()] = content = new GUIContent { image = texture };
            imageStyle.Draw(r, content, false, false, false, false);
        }

        float Natural(string text, GUIStyle style) => style.CalcSize(Content(text)).x;

        GUIContent Content(string text)
        {
            if (!contents.TryGetValue(text, out var c)) contents[text] = c = new GUIContent(text);
            return c;
        }

        bool Repainting => !ui.Measuring && Event.current.type == EventType.Repaint;

        bool Clicked(Rect r) => !ui.Measuring && r.Contains(Mouse) && (pointing ? press && Event.current.type == EventType.Repaint : Event.current.type == EventType.MouseDown);

        static GUIStyle fillStyle;

        // Also draws the pointer dot outside the menu.
        internal static void Fill(Rect r, Color c)
        {
            if (Event.current.type != EventType.Repaint || (MenuLayout.Current != null && MenuLayout.Current.Measuring)) return;
            if (fillStyle == null)
            {
                fillStyle = new GUIStyle();
                fillStyle.normal.background = Texture2D.whiteTexture;
            }
            var saved = GUI.color;
            GUI.color = c;
            try
            {
                // GUIStyle.Draw goes straight to the engine; DrawTexture only if a game lacks it.
                if (fillMode == 0) fillStyle.Draw(r, false, false, false, false);
                else GUI.DrawTexture(r, Texture2D.whiteTexture);
            }
            catch (System.Exception ex) when (fillMode < 1 && (ex is System.NotSupportedException || ex is System.MissingMethodException))
            {
                fillMode++;
            }
            finally
            {
                GUI.color = saved;
            }
        }

        static int fillMode;

        static void Outline(Rect r, Color c, float t = 1)
        {
            Fill(new Rect(r.x, r.y, r.width, t), c);
            Fill(new Rect(r.x, r.yMax - t, r.width, t), c);
            Fill(new Rect(r.x, r.y, t, r.height), c);
            Fill(new Rect(r.xMax - t, r.y, t, r.height), c);
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
            cell = Style(TextAnchor.MiddleCenter, Ink, Small, null, true);
            cellOn = Style(TextAnchor.MiddleCenter, OnLime, Small, null, true);
            label = Style(TextAnchor.MiddleLeft, Ink, Body, null, false);
            muted = Style(TextAnchor.MiddleLeft, Ink2, Small, null, false);
            warn = Style(TextAnchor.MiddleLeft, Danger, Small, null, false);
            label.wordWrap = muted.wordWrap = warn.wordWrap = true;
            value = Style(TextAnchor.MiddleCenter, Ink, Body, monoFont, false);
            data = Style(TextAnchor.MiddleLeft, Ink, Body, monoFont, false);
            display = Style(TextAnchor.MiddleLeft, Ink, Display, displayFont, false);
            eyebrow = Style(TextAnchor.MiddleLeft, Ink3, Small, monoFont, false);
            eyebrowWord = Style(TextAnchor.MiddleLeft, Lime, Small, monoFont, false);
            tabStyle = Style(TextAnchor.MiddleLeft, Ink3, Small, monoFont, false);
            tabStyle.hover.textColor = Ink;
            tabOn = Style(TextAnchor.MiddleLeft, Ink, Small, monoFont, false);
            imageStyle = new GUIStyle { imagePosition = ImagePosition.ImageOnly, alignment = TextAnchor.MiddleCenter };
        }

        // Filled styles use the built-in white texture, tinted by GUI.backgroundColor when drawn (see Button).
        internal static GUIStyle Style(TextAnchor align, Color text, int size, Font font, bool filled)
        {
            var s = new GUIStyle { fontSize = size, alignment = align, font = font };
            s.normal.textColor = s.hover.textColor = s.active.textColor = text;
            if (filled) s.normal.background = s.hover.background = s.active.background = Texture2D.whiteTexture;
            return s;
        }
    }
}
