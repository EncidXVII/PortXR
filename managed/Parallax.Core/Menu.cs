using System;
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

    // The VR menu is the core's (native/src/menu.cpp, shared with every engine), run in its frame and drawn on its own
    // headset layer over the middle of the UI panel or the screen; what it asks for comes back in the rig's frame. This
    // side offers what Unity has (cameras, hidden objects), keeps the game's cursor free while it is open, and keeps
    // the settings in step: the core and BepInEx write the same file.
    internal class Menu
    {
        [DllImport("parallax_xr", CharSet = CharSet.Unicode)] static extern void prx_menu_settings(string gameFile);
        [DllImport("parallax_xr")] static extern void prx_menu_host(ref MenuHost host);
        [DllImport("parallax_xr")] static extern void prx_menu_show(int page, int unlock);

        static float fade = 1;
        readonly Rig rig;
        internal bool Open;
        bool unlocked;
        CursorLockMode savedLock;
        bool savedVisible;
        float nextHost;
        DateTime configTime;

        public Menu(Rig rig)
        {
            this.rig = rig;
            prx_menu_settings(Settings.File.ConfigFilePath);
            configTime = File.GetLastWriteTimeUtc(Settings.File.ConfigFilePath);
            // A change made here (the plugin, tests) reaches the core at once rather than at its next look at the file.
            Settings.File.SettingChanged += (_, __) => Native.prx_settings_touch();
        }

        // Theater and Parallax: the screen's handles show only once the player unlocks them in the menu (tests set it).
        internal bool ScreenUnlocked
        {
            get => unlocked && Open;
            set { prx_menu_show(-3, value ? 1 : 0); unlocked = value; }
        }

        public void Toggle() { prx_menu_show(-2, -1); Opened(!Open); }

        // -1 is the first-run card.
        public void Show(int page) { prx_menu_show(page, -1); Opened(true); }

        // The core's menu opened or closed (its own buttons, the menu button, the watch, F11).
        internal void Opened(bool open)
        {
            if (open == Open) return;
            Open = open;
            if (open)
            {
                savedLock = Cursor.lockState;
                savedVisible = Cursor.visible;
                return;
            }
            unlocked = false;
            Cursor.lockState = savedLock;
            Cursor.visible = savedVisible;
        }

        public void Update()
        {
            if (Time.unscaledTime >= nextHost) Offer();
            // The core wrote the settings file (or the app did): read it again.
            var t = File.GetLastWriteTimeUtc(Settings.File.ConfigFilePath);
            if (t != configTime)
            {
                configTime = t;
                Settings.File.Reload();
            }
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

        // The pointer dot on the game's UI (IMGUI).
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
    }
}
