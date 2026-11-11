using System.Collections.Generic;
using System.Linq;
using BepInEx.Configuration;
using UnityEngine;

namespace Parallax
{
    // Drawn with IMGUI onto the screen, so it reaches the headset through the UI capture and the laser pointer.
    // Only engine members that survive IL2CPP stripping in typical games are used: no texture creation outside an
    // Optional guard, no style copies, layout options always passed as arrays (Il2CppInterop has no params).
    // Look: "Night Lime" in DesktopVR's graphite greys (one lime accent, filled rounded cells, spaced capital labels), the same tokens
    // as the desktop app. Motion as in DesktopVR (see Motion): the menu fades in, the tab bar and a row's choice slide,
    // hovers ease, presses answer on trigger-down.
    internal class Menu
    {
        static readonly string[] Tabs = { "Quick", "View", "Controls", "Game", "Diagnostics" };
        const int QuickTab = 0, ViewTab = 1, ControlsTab = 2, GameTab = 3;
        const float RefHeight = 1080f, Width = 1100f, Height = 800f, Row = 64f, Pad = 48f;
        const int ThumbW = 304, ThumbH = 171, History = 120;
        static readonly Color Bg = Rgb(0x0B0B0C), Surface = Rgb(0x1D1D20), Line = Rgb(0x2A2A2E);
        // Cells: Surface at rest, HoverFill under the laser, PressFill while the trigger is held on them.
        static readonly Color HoverFill = Rgb(0x2A2A2E), PressFill = Rgb(0x36363B);
        static readonly Color Ink = Rgb(0xF5F5F7), Ink2 = Rgb(0xA8A8AF), Ink3 = Rgb(0x88888F);
        // The accent follows the theme (Theme.cs); dark text on it works for all eight.
        internal static Color Lime => Theme.Accent;
        internal static readonly Color OnLime = Rgb(0x0B0B0C);
        static Color LimePressed => Theme.Pressed;
        static readonly Color Danger = Rgb(0xFF5A3C), DangerInk = Rgb(0xFF8A6E);
        // Corner radii (px at the 1080 reference height): the panel, cells, a segment's inner cell.
        const float PanelRadius = 24, CellRadius = 14, InnerRadius = 12;

        static Color Rgb(int v) => new Color((v >> 16 & 255) / 255f, (v >> 8 & 255) / 255f, (v & 255) / 255f);
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
        GUIStyle cell, cellOn, cellQuiet, quiet, sign, label, muted, eyebrow, eyebrowWord, display, value, data, warn, tabStyle, tabOn;

        // Where the menu was last drawn, in screen pixels from the top-left.
        internal Rect ScreenRect;
        Rect hoverRect;

        // Theater and Parallax: the screen's handles show only once the player unlocks them here, so nothing gets in the
        // way of playing; closing the menu locks the screen again.
        internal bool ScreenUnlocked;

        // Motion state: when the menu opened (it fades in), the tab bar's place, each row's sliding choice, and how far
        // each control's hover has come. Keys are a control's place in the panel, stable from frame to frame.
        float openedAt, barX = -1, barW;
        readonly Dictionary<string, Vector2> slides = new Dictionary<string, Vector2>();
        readonly Dictionary<int, float> hovers = new Dictionary<int, float>();
        static float fade = 1;

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
                openedAt = Time.unscaledTime;
            }
            else
            {
                welcome = false;
                ScreenUnlocked = false;
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
            fade = Motion.Out(openedAt, Motion.Fast);
            var savedColor = GUI.color;
            GUI.color = new Color(1, 1, 1, fade);
            Round(area, Bg, PanelRadius);
            // The content's origin moved by the matrix, so the layout starts at (0, 0).
            Matrix(new Vector2((area.x + Pad) * k, (area.y + 32) * k), k);
            ui.Run(Width - 2 * Pad, () =>
            {
                if (welcome) DrawWelcome();
                else DrawTabs();
                // In a headset a hairline is lost: the control under the laser gets a lighter fill and a 2 px ring.
                if (hoverRect.width > 0 && Repainting) Ring(hoverRect, Ink, CellRadius);
                hoverRect = default;
            });
            Matrix(Vector2.zero, 1);
            GUI.color = savedColor;
            fade = 1;
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
            Rect active = default;
            for (int i = 0; i < Tabs.Length; i++)
            {
                bool on = i == tab;
                string word = Tabs[i].ToUpperInvariant();
                var style = on ? tabOn : tabStyle;
                var r = ui.Take(Natural(word, style), Row);
                Text(r, word, style, true);
                if (Clicked(r)) Select(i);
                if (on) active = r;
                if (Repainting) Hover(r);
                ui.Gap(28);
            }
            ui.Flex();
            // Only tabs with settings have something to reset.
            if (tab <= GameTab && Quiet("Reset", 128)) ResetTab();
            ui.Gap(8);
            if (Quiet("Close", 128)) Toggle();
            ui.Gap(2);
            ui.EndRow();
            var rule = ui.Take(0, 1);
            Fill(rule, Line);
            // The lime bar under the active tab slides to a newly picked one.
            if (Repainting && active.width > 0)
            {
                if (barX < 0)
                {
                    barX = active.x;
                    barW = active.width;
                }
                float dt = Time.unscaledDeltaTime;
                barX = Motion.Follow(barX, active.x, Motion.Slow / 4, dt);
                barW = Motion.Follow(barW, active.width, Motion.Slow / 4, dt);
                Round(new Rect(barX, rule.y - 3, barW, 4), Lime, 2);
            }
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
            if (tab == QuickTab) entries = new ConfigEntryBase[] { Settings.Display, Settings.Depth, Settings.ScreenYaw, Settings.ScreenPitch, Settings.ScreenDistance, Settings.ScreenWidth, Settings.WorldScale, Settings.Turning, Settings.Vignette, Settings.Input, Settings.HeadAim, Settings.AimWith };
            else if (tab == ViewTab)
                entries = new ConfigEntryBase[] { Settings.Height, Settings.RenderScale, Settings.AutoRenderScale, Settings.UiDistance, Settings.UiWidth, Settings.Pointer, Settings.WristMenu, Settings.UiBlack };
            else if (tab == ControlsTab) entries = Bindings.Entries.SelectMany(g => g).Where(e => e != null).ToArray();
            // Hidden objects stay: they are picked one by one and undone the same way.
            else if (tab == GameTab) entries = new ConfigEntryBase[] { Settings.CameraName, Settings.Crosshair, Settings.WeaponInHand };
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
            if (Settings.Display.Value == DisplayMode.Parallax) Stepper("3D depth", Settings.Depth, 0.25f, 0f, 1f, "{0:0.00}×");
            if (Settings.Display.Value != DisplayMode.Stereo)
            {
                bool unlocked = ScreenUnlocked;
                Choice("Screen", "Locked", "Unlocked", ref unlocked);
                ScreenUnlocked = unlocked;
                ui.BeginRow();
                ui.Gap(360);
                Label(unlocked ? "Point at its corners to resize it, at the bar under it to move it."
                    : "Locked while you play. Unlock it to move or resize the screen.", muted, 0, 40);
                ui.EndRow();
                ui.Gap(12);
            }

            ui.BeginRow();
            Label("Position", label, 360);
            if (Button("Recenter", false, 300)) rig.RequestRecenter();
            ui.EndRow();
            ui.Gap(12);

            // Scale, turning, the vignette and aim only act on the stereo view: on a screen the game's own camera steers.
            if (Settings.Display.Value != DisplayMode.Stereo)
            {
                Options("Controls", Settings.Input, new[] { InputMode.Auto, InputMode.Gamepad, InputMode.KeyboardMouse, InputMode.Off },
                    new[] { "Auto", "Gamepad", "Keyboard & mouse", "Off" }, InputMode.Off);
                return;
            }
            Stepper("World scale", Settings.WorldScale, 0.1f, 0.5f, 3f, "{0:0.0}×");
            Options("Turning", Settings.Turning, new[] { TurnMode.Smooth, TurnMode.Snap15, TurnMode.Snap30, TurnMode.Snap45 },
                new[] { "Smooth", "Snap 15°", "Snap 30°", "Snap 45°" }, null);
            Options("Comfort vignette", Settings.Vignette, new[] { VignetteLevel.Off, VignetteLevel.Low, VignetteLevel.High },
                new[] { "Off", "Low", "High" }, VignetteLevel.Off);

            // Same order as the app.
            Options("Controls", Settings.Input, new[] { InputMode.Auto, InputMode.Gamepad, InputMode.KeyboardMouse, InputMode.Off },
                new[] { "Auto", "Gamepad", "Keyboard & mouse", "Off" }, InputMode.Off);
            Options("Aim", Settings.HeadAim, new[] { AimMode.Auto, AimMode.On, AimMode.Off }, new[] { "Auto", "On", "Off" }, AimMode.Off);
            if (Settings.HeadAim.Value != AimMode.Off)
                Options("Aim with", Settings.AimWith, new[] { AimHand.Head, AimHand.Hand }, new[] { "Head", "Right hand" }, null);
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
            ThemeRow();
        }

        // Accent colour for every game and the app: one dot per theme, the chosen one ringed and named.
        void ThemeRow()
        {
            ui.BeginRow();
            int current = Theme.Index;
            Label("Theme", label, 200);
            Label(Theme.Names[current], muted, 160);
            for (int i = 0; i < Theme.Names.Length; i++)
            {
                var r = ui.Take(Row + 6, Row);
                var cellRect = new Rect(r.x + 7, r.y + 4, Row - 8, Row - 8);
                var dot = new Rect(cellRect.x + 9, cellRect.y + 9, cellRect.width - 18, cellRect.height - 18);
                if (Repainting)
                {
                    bool over = cellRect.Contains(Mouse);
                    if (i == current) Ring(cellRect, Ink, cellRect.width / 2);
                    else if (Eased(cellRect, over) > 0.01f) Ring(cellRect, Ink3, cellRect.width / 2);
                    Round(dot, Theme.Of(i), dot.width / 2);
                    Hover(cellRect);
                }
                if (Clicked(cellRect) && i != current)
                {
                    Theme.Set(i);
                    Native.prx_haptic_pulse(1, 0.35f, 0.02f);
                }
            }
            ui.EndRow();
            ui.Gap(12);
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
            int pickedGesture = Segment("gesture", Bindings.Gestures, Bindings.Gestures.Select(_ => 212f).ToArray(), gesture);
            if (pickedGesture >= 0) gesture = pickedGesture;
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
                if (Button(Bindings.Shown(entry.Value), false, 220)) picking = i;
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
                    if (Button(Bindings.Shown(names[j]), names[j] == picked.Value, 160))
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
                Round(rect, Surface, InnerRadius);
                if (Repainting && thumbs.TryGetValue(cam.GetInstanceID(), out var rt)) Image(rect, rt);
                if (cam == rig.GameCam) Ring(rect, Lime, InnerRadius);
                // Lime fill only for a camera the player picked; the automatic pick gets the lime outline above.
                if (Button(cam.name, !automatic && cam == rig.GameCam, ThumbW, true)) PickCamera(cam.name);
                ui.EndColumn();
                ui.Gap(16);
            }
            ui.EndRow();
            // How this game's weapon and crosshair come along when you aim: they depend on how the game draws them.
            if (Settings.HeadAim.Value == AimMode.Off) return;
            ui.Gap(24);
            Options("Crosshair", Settings.Crosshair, new[] { CrosshairMode.Game, CrosshairMode.Lime, CrosshairMode.Off }, new[] { "Game's", "Parallax's", "Off" }, CrosshairMode.Off);
            if (Settings.AimWith.Value == AimHand.Hand) Toggle("Weapon in hand", Settings.WeaponInHand);
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
            Round(rect, Surface, 16);
            rect = new Rect(rect.x + 12, rect.y + 8, rect.width - 24, rect.height - 16);
            float top = Mathf.Max(budgetMs * 1.5f, 1f), bar = rect.width / History;
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

        // Segmented control: two 292 px halves in a rounded track, the chosen one ink.
        void Choice(string name, string off, string on, ref bool v, bool offIsNeutral = false)
        {
            ui.BeginRow();
            Label(name, label, 360);
            int picked = Segment(name, new[] { off, on }, new[] { 292f, 292f }, v ? 1 : 0);
            if (picked >= 0) v = picked == 1;
            ui.EndRow();
            ui.Gap(12);
        }

        // Segmented control over an enum setting in the 600 px value column, the chosen cell ink. `neutral` is kept for
        // the callers' sake: an Off is chosen like any other value now (lime is for the tab bar and primary actions).
        void Options<T>(string name, ConfigEntry<T> entry, T[] values, string[] labels, T? neutral) where T : struct, System.Enum
        {
            ui.BeginRow();
            Label(name, label, 360);
            // Each option as wide as its text, the rest of 592 shared out as padding (at least 12 each), so a long
            // label ("Keyboard & mouse") never touches its neighbours and the row keeps the column's right edge.
            float[] text = labels.Select(l => Natural(l.ToUpperInvariant(), cell)).ToArray();
            float pad = Mathf.Max(12f, (592f - text.Sum()) / text.Length);
            int current = System.Array.FindIndex(values, x => entry.Value.Equals(x));
            int picked = Segment(name, labels, text.Select(t => t + pad).ToArray(), current);
            if (picked >= 0) entry.Value = values[picked];
            ui.EndRow();
            ui.Gap(12);
        }

        // Cells of `widths` in a rounded track. The chosen one is filled ink and slides over when another is picked;
        // returns the cell clicked this event, or -1.
        int Segment(string id, string[] labels, float[] widths, int selected)
        {
            var track = ui.Take(widths.Sum() + 8, Row);
            Round(track, Surface, CellRadius + 2);
            int clicked = -1;
            float x = track.x + 4;
            var cells = new Rect[labels.Length];
            for (int i = 0; i < labels.Length; i++)
            {
                cells[i] = new Rect(x, track.y + 4, widths[i], track.height - 8);
                x += widths[i];
            }
            if (Repainting && selected >= 0)
            {
                var to = new Vector2(cells[selected].x, cells[selected].width);
                if (!slides.TryGetValue(id, out var at)) at = to;
                float dt = Time.unscaledDeltaTime;
                at = new Vector2(Motion.Follow(at.x, to.x, Motion.Slow / 4, dt), Motion.Follow(at.y, to.y, Motion.Slow / 4, dt));
                slides[id] = at;
                Round(new Rect(at.x, track.y + 4, at.y, track.height - 8), Ink, InnerRadius);
            }
            for (int i = 0; i < labels.Length; i++)
            {
                var r = cells[i];
                bool on = i == selected;
                if (Repainting)
                {
                    bool over = r.Contains(Mouse);
                    float h = Eased(r, over && !on);
                    if (!on && Pressed(r)) Round(r, PressFill, InnerRadius);
                    else if (!on && h > 0.01f) Round(r, new Color(HoverFill.r, HoverFill.g, HoverFill.b, h), InnerRadius);
                    var style = on ? cellOn : cellQuiet;
                    style.Draw(r, Content(labels[i].ToUpperInvariant()), false, false, false, false);
                    if (over && !on) hoverRect = r;
                    Hover(r);
                }
                if (Clicked(r) && !on)
                {
                    clicked = i;
                    Native.prx_haptic_pulse(1, 0.35f, 0.02f);
                }
            }
            return clicked;
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
            if (Button("−", false, 96, false, sign)) entry.Value = Mathf.Round(Mathf.Max(min, entry.Value - step) * 100) / 100;
            Label(string.Format(format, entry.Value), value, 200);
            if (Button("+", false, 96, false, sign)) entry.Value = Mathf.Round(Mathf.Min(max, entry.Value + step) * 100) / 100;
        }

        // A cell: filled Surface, lighter under the laser (eased), lighter still while the trigger is held on it; `on`
        // fills it ink (selected state). Labels are UPPERCASE like the app's buttons; `data` (a camera object name) is
        // shown as the game wrote it.
        bool Button(string text, bool on, float width, bool data = false, GUIStyle look = null)
        {
            string shown = data ? text : text.ToUpperInvariant();
            var style = look ?? (on ? cellOn : cell);
            var r = ui.Take(width > 0 ? width : Natural(shown, style) + 48, Row);
            return Cell(r, shown, style, on ? Ink : Surface, on);
        }

        bool Cell(Rect r, string shown, GUIStyle style, Color fill, bool on, bool lime = false)
        {
            if (Repainting)
            {
                bool over = r.Contains(Mouse);
                float h = Eased(r, over && !on);
                Color c = lime ? (Pressed(r) ? LimePressed : Lime)
                    : on ? fill : Pressed(r) ? PressFill : Color.Lerp(fill, HoverFill, h);
                Round(r, c, CellRadius);
                style.Draw(r, Content(shown), false, false, false, false);
                // Outlined after everything else, so the next cell cannot cover part of it.
                if (over) hoverRect = r;
                Hover(r);
            }
            bool clicked = Clicked(r);
            if (clicked) Native.prx_haptic_pulse(1, 0.35f, 0.02f);
            return clicked;
        }

        // Reset and Close: quiet cells, shorter than the row, on the tab line's right.
        bool Quiet(string text, float width)
        {
            var row = ui.Take(width, Row);
            var r = new Rect(row.x, row.y + 6, row.width, row.height - 12);
            return Cell(r, text.ToUpperInvariant(), quiet, Surface, false);
        }

        // How far a control's hover has come (0..1), eased over Motion.Fast.
        float Eased(Rect r, bool over)
        {
            int key = (int)(r.x * 7919f + r.y);
            hovers.TryGetValue(key, out float h);
            h = Motion.Follow(h, over ? 1f : 0f, Motion.Fast / 4, Time.unscaledDeltaTime);
            hovers[key] = h;
            if (hovers.Count > 512) hovers.Clear();
            return h;
        }

        // The trigger is held on `r`: pressed state, shown the moment it goes down. (The legacy Input class, which could
        // tell a held mouse button, throws in games that only use the Input System package.)
        bool Pressed(Rect r) => pointing && triggerDown && r.Contains(Mouse);

        // The one primary action of a view: lime fill.
        bool Primary(string text, float width)
        {
            var r = ui.Take(width, Row);
            return Cell(r, text.ToUpperInvariant(), cellOn, Lime, false, true);
        }

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

        // Also used outside the menu: the pointer dot and the screen handles.
        internal static void Fill(Rect r, Color c)
        {
            if (Event.current.type != EventType.Repaint || (MenuLayout.Current != null && MenuLayout.Current.Measuring)) return;
            if (fillStyle == null)
            {
                fillStyle = new GUIStyle();
                fillStyle.normal.background = Texture2D.whiteTexture;
            }
            var saved = GUI.color;
            GUI.color = new Color(c.r, c.g, c.b, c.a * fade);
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

        // Rounded fills and rings: a small white texture per radius, drawn nine-sliced (its corners kept, its middle
        // stretched) and tinted. Texture creation can be stripped in IL2CPP games: then the square fill and outline.
        static readonly Optional rounding = new Optional("Rounded corners");
        static readonly Dictionary<int, GUIStyle> roundStyles = new Dictionary<int, GUIStyle>();

        internal static void Round(Rect r, Color c, float radius) => Shape(r, c, radius, false);

        static void Ring(Rect r, Color c, float radius) => Shape(r, c, radius, true);

        static void Shape(Rect r, Color c, float radius, bool ring)
        {
            if (Event.current.type != EventType.Repaint || (MenuLayout.Current != null && MenuLayout.Current.Measuring)) return;
            int k = Mathf.Clamp(Mathf.RoundToInt(Mathf.Min(radius, r.width / 2, r.height / 2)), 1, 32);
            var style = RoundStyle(k, ring);
            if (style == null)
            {
                if (ring) Outline(r, c, 2);
                else Fill(r, c);
                return;
            }
            var saved = GUI.color;
            GUI.color = new Color(c.r, c.g, c.b, c.a * fade);
            try
            {
                style.Draw(r, false, false, false, false);
            }
            finally
            {
                GUI.color = saved;
            }
        }

        static GUIStyle RoundStyle(int k, bool ring)
        {
            int key = ring ? -k : k;
            if (roundStyles.TryGetValue(key, out var found)) return found;
            GUIStyle made = null;
#if IL2CPP
            // Il2CppInterop exposes no Texture2D constructor in the games this build targets: square cells there.
#else
            rounding.Run(() =>
            {
                // Twice the radius and a texel of middle; supersampled 4x4 for a smooth edge.
                int n = 2 * k + 2;
                // The two-argument constructor: IL2CPP games often strip the others.
                var tex = new Texture2D(n, n) { wrapMode = TextureWrapMode.Clamp, filterMode = FilterMode.Bilinear, hideFlags = HideFlags.HideAndDontSave };
                var px = new Color32[n * n];
                float c0 = k, c1 = n - k;
                for (int y = 0; y < n; y++)
                    for (int x = 0; x < n; x++)
                    {
                        int inside = 0;
                        for (int sy = 0; sy < 4; sy++)
                            for (int sx = 0; sx < 4; sx++)
                            {
                                float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                                float dx = Mathf.Max(0, Mathf.Max(c0 - fx, fx - c1)), dy = Mathf.Max(0, Mathf.Max(c0 - fy, fy - c1));
                                float d = Mathf.Sqrt(dx * dx + dy * dy);
                                // Distance to the edge from inside: the corner arc, or the straight sides.
                                float edge = Mathf.Min(Mathf.Min(fx, n - fx), Mathf.Min(fy, n - fy));
                                float inner = dx > 0 && dy > 0 ? k - d : edge;
                                bool hit = d <= k && (!ring || inner <= 2f);
                                if (hit) inside++;
                            }
                        px[y * n + x] = new Color32(255, 255, 255, (byte)(inside * 255 / 16));
                    }
                tex.SetPixels32(px);
                tex.Apply();
                var border = new RectOffset { left = k, right = k, top = k, bottom = k };
                made = new GUIStyle { border = border };
                made.normal.background = tex;
            });
#endif
            roundStyles[key] = made;
            return made;
        }

        void EnsureStyles()
        {
            if (cell != null)
            {
                // Styles keep their colour; the accent one follows a theme change.
                eyebrowWord.normal.textColor = eyebrowWord.hover.textColor = eyebrowWord.active.textColor = Lime;
                return;
            }
            Font displayFont = null, monoFont = null, bodyFont = null;
            // OS fonts: the app's Barlow where installed, else what every Windows has (Segoe UI, Cascadia
            // Mono or Consolas); Anton, else Impact (heavy condensed, like it) for headlines. The IMGUI default otherwise.
            fonts.Run(() =>
            {
                displayFont = Font.CreateDynamicFontFromOSFont(new[] { "Anton", "Impact" }, 60);
                monoFont = Font.CreateDynamicFontFromOSFont(new[] { "Cascadia Mono", "Consolas" }, 24);
                bodyFont = Font.CreateDynamicFontFromOSFont(new[] { "Barlow", "Segoe UI" }, 28);
            });
            // Cells draw their own rounded fill; their styles only carry the text.
            cell = Style(TextAnchor.MiddleCenter, Ink, 23, bodyFont, false);
            cellOn = Style(TextAnchor.MiddleCenter, OnLime, 23, bodyFont, false);
            cellOn.fontStyle = FontStyle.Bold;
            cellQuiet = Style(TextAnchor.MiddleCenter, Ink2, 23, bodyFont, false);
            quiet = Style(TextAnchor.MiddleCenter, Ink2, 22, bodyFont, false);
            sign = Style(TextAnchor.MiddleCenter, Ink, 30, bodyFont, false);
            label = Style(TextAnchor.MiddleLeft, Ink, Body, bodyFont, false);
            muted = Style(TextAnchor.MiddleLeft, Ink2, Small, bodyFont, false);
            warn = Style(TextAnchor.MiddleLeft, DangerInk, Small, bodyFont, false);
            label.wordWrap = muted.wordWrap = warn.wordWrap = true;
            // Labels, tabs and values in the body face like the app (one family throughout); mono only for diagnostics.
            value = Style(TextAnchor.MiddleCenter, Ink, Body, bodyFont, false);
            data = Style(TextAnchor.MiddleLeft, Ink, Body, monoFont, false);
            display = Style(TextAnchor.MiddleLeft, Ink, Display, displayFont, false);
            eyebrow = Style(TextAnchor.MiddleLeft, Ink3, Small, bodyFont, false);
            eyebrowWord = Style(TextAnchor.MiddleLeft, Lime, Small, bodyFont, false);
            eyebrow.fontStyle = eyebrowWord.fontStyle = FontStyle.Bold;
            tabStyle = Style(TextAnchor.MiddleLeft, Ink2, 26, bodyFont, false);
            tabStyle.hover.textColor = Ink;
            tabOn = Style(TextAnchor.MiddleLeft, Ink, 26, bodyFont, false);
            tabOn.fontStyle = FontStyle.Bold;
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
