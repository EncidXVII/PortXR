using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using UnityEngine;

namespace Parallax
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    internal struct MenuHost
    {
        [MarshalAs(UnmanagedType.LPStr)] public string product;
        [MarshalAs(UnmanagedType.LPWStr)] public string gameDir;
        [MarshalAs(UnmanagedType.LPStr)] public string cameras;
        [MarshalAs(UnmanagedType.LPStr)] public string currentCamera;
        public int hideObjects, hidden, aimExtras, uiBlack, eyeWidth, eyeHeight;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    internal struct MenuOut
    {
        public int open, unlocked, quit, recenter, hideObjects, showHidden, renderScale, cameraPicked;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string camera;
    }

    // The VR menu is the core's (native/src/menu.cpp, shared with every engine): drawn there on its own headset layer
    // over the middle of the UI panel or the screen. This side hands it the laser, what Unity offers (cameras, hidden
    // objects) and does what it asks; it writes the same settings file, which is read back here when it changes.
    internal class Menu
    {
        [DllImport("parallax_xr", CharSet = CharSet.Unicode)] static extern void prx_menu_settings(string gameFile);
        [DllImport("parallax_xr")] static extern void prx_menu_host(ref MenuHost host);
        [DllImport("parallax_xr")] static extern void prx_menu_show(int page, int unlock);
        [DllImport("parallax_xr")] static extern void prx_menu_frame(int pointing, float u, float v, float trigger, float dt, ref Pose pose, float heightM, out MenuOut result);

        const float Width = 1100f, Height = 840f;
        internal static Color Lime => Theme.Accent;
        internal static readonly Color OnLime = new Color(0.043f, 0.043f, 0.047f);
        static float fade = 1;

        readonly Rig rig;
        internal bool Open;
        bool unlocked = false;
        CursorLockMode savedLock;
        bool savedVisible;
        Vector2 pointer;
        bool pointing;
        float trigger, nextHost;
        DateTime configTime;
        // Where the menu shows (tracking space) and how tall: set by the rig from the layer it puts the UI on.
        internal Pose At;
        internal float AtHeight;

        public Menu(Rig rig)
        {
            this.rig = rig;
            prx_menu_settings(Settings.File.ConfigFilePath);
            configTime = File.GetLastWriteTimeUtc(Settings.File.ConfigFilePath);
        }

        // Theater and Parallax: the screen's handles show only once the player unlocks them in the menu.
        internal bool ScreenUnlocked
        {
            get => unlocked;
            set { prx_menu_show(-3, value ? 1 : 0); unlocked = value; }
        }

        public void Toggle() { prx_menu_show(-2, -1); Opened(!Open); }

        // -1 is the first-run card.
        public void Show(int page) { prx_menu_show(page, -1); Opened(true); }

        void Opened(bool open)
        {
            if (open == Open) return;
            Open = open;
            if (open)
            {
                savedLock = Cursor.lockState;
                savedVisible = Cursor.visible;
            }
            else
            {
                unlocked = false;
                Cursor.lockState = savedLock;
                Cursor.visible = savedVisible;
            }
        }

        // The laser on the UI panel, in screen pixels from the top-left.
        internal void Point(bool active, Vector2 at, float pull)
        {
            pointing = active;
            pointer = at;
            trigger = pull;
        }

        public void Update()
        {
            if (Time.unscaledTime >= nextHost) Offer();
            // The menu is the middle of the screen, as tall as 840 of 1080 rows: the laser's place on it.
            float h = Screen.height * Height / 1080f, w = h * Width / Height;
            float u = (pointer.x - (Screen.width - w) / 2) / w, v = (pointer.y - (Screen.height - h) / 2) / h;
            bool on = pointing && u >= 0 && u <= 1 && v >= 0 && v <= 1;
            var at = At;
            prx_menu_frame(on ? 1 : 0, u, v, trigger, Mathf.Min(0.1f, Time.unscaledDeltaTime), ref at, AtHeight, out var r);
            Opened(r.open != 0);
            unlocked = r.unlocked != 0;
            // The core wrote the settings file (or the app did): read it again.
            var t = File.GetLastWriteTimeUtc(Settings.File.ConfigFilePath);
            if (t != configTime)
            {
                configTime = t;
                Settings.File.Reload();
            }
            if (r.recenter != 0) rig.RequestRecenter();
            if (r.renderScale != 0) rig.ApplyRenderScale();
            if (r.cameraPicked != 0) rig.ReleaseCamera();
            if (r.showHidden != 0) rig.Hider.ShowAll();
            if (r.hideObjects != 0) rig.Hider.Start();
            if (r.quit != 0) Application.Quit();
            if (!Open) return;
            // Games re-lock the cursor every frame; keep it free while the menu is up.
            Cursor.lockState = CursorLockMode.None;
            Cursor.visible = true;
        }

        // What the menu offers from Unity, refreshed every two seconds: the cameras the view can follow, hidden objects.
        void Offer()
        {
            nextHost = Time.unscaledTime + 2f;
            var cams = Camera.allCameras.Where(c => !rig.IsEye(c) && Rig.Candidate(c)).Select(c => c.name).Distinct().ToArray();
            var host = new MenuHost
            {
                product = Application.productName,
                gameDir = Path.GetDirectoryName(Application.dataPath),
                cameras = string.Join("\n", cams),
                currentCamera = rig.GameCam != null ? rig.GameCam.name : "",
                hideObjects = 1,
                hidden = rig.Hider.Count,
                aimExtras = 1,
                uiBlack = 1,
                eyeWidth = rig.EyeSize.x,
                eyeHeight = rig.EyeSize.y,
            };
            prx_menu_host(ref host);
        }

        // Drawing helpers for the pointer dot and the screen handles, which IMGUI draws onto the UI.
        static GUIStyle fillStyle;

        // Also used outside the menu: the pointer dot and the screen handles.
        internal static void Fill(Rect r, Color c)
        {
            if (Event.current.type != EventType.Repaint) return;
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

        internal static void Round(Rect r, Color c, float radius) => Shape(r, c, radius, 0);

        static void Shape(Rect r, Color c, float radius, int mode)
        {
            if (Event.current.type != EventType.Repaint) return;
            int k = Mathf.Clamp(Mathf.RoundToInt(Mathf.Min(radius, r.width / 2, r.height / 2)), 1, 32);
            var style = RoundStyle(k, mode);
            if (style == null)
            {
                if (mode == 1) Outline(r, c, 2);
                else if (mode == 0) Fill(r, c);
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

        static GUIStyle RoundStyle(int k, int mode)
        {
            int key = mode * 100 + k;
            bool ring = mode == 1;
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
                        px[y * n + x] = new Color32(255, 255, 255, (byte)((mode == 2 ? 16 - inside : inside) * 255 / 16));
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
