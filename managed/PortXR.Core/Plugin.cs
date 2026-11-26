// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using System.IO;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using UnityEngine;
using UnityEngine.Rendering;

namespace PortXR
{
    public enum DisplayMode { Stereo, Parallax, Theater }
    public enum ScreenBackground { Dark, Passthrough }

    public enum InputMode { Gamepad, KeyboardMouse, Off, Auto }
    public enum PointerMode { Auto, MenuOnly }
    public enum TurnMode { Smooth, Snap15, Snap30, Snap45 }
    public enum VignetteLevel { Off, Low, High }
    public enum AimMode { Auto, On, Off }
    public enum AimHand { Head, Hand }
    public enum CrosshairMode { Game, Lime, Off }
    public enum UiBackground { Auto, SeeThrough, Keep }

    internal static class Settings
    {
        // Controls as played: Auto picks the gamepad for games that ship a gamepad input library, as their controller
        // layouts are then made for it; keyboard and mouse otherwise.
        internal static InputMode InputNow => Input.Value != InputMode.Auto ? Input.Value : PadGame() ? InputMode.Gamepad : InputMode.KeyboardMouse;
        static bool? padGame;

        internal static bool PadGame()
        {
            if (padGame is bool known) return known;
            string[] pads = { "Unity.InputSystem", "Rewired_Core", "InControl", "XInputDotNetPure", "SharpDX.XInput", "XInputInterface" };
            padGame = false;
            // Mono games ship them in Managed; IL2CPP games get interop assemblies of the same names.
            string[] dirs = { System.IO.Path.Combine(UnityEngine.Application.dataPath, "Managed"), System.IO.Path.Combine(BepInEx.Paths.BepInExRootPath, "interop") };
            foreach (var dir in dirs)
                foreach (var name in pads)
                    if (System.IO.File.Exists(System.IO.Path.Combine(dir, name + ".dll"))) padGame = true;
            Plugin.Log.LogInfo($"Controls (auto): {(padGame.Value ? "gamepad" : "keyboard and mouse")}");
            return padGame.Value;
        }

        public static ConfigFile File;
        public static ConfigEntry<float> WorldScale, Height, RenderScale, MouseSpeed, UiDistance, UiWidth, Depth, ScreenYaw, ScreenPitch, ScreenDistance, ScreenWidth;
        public static ConfigEntry<bool> AutoRenderScale, WristMenu, FirstRun;
        public static ConfigEntry<DisplayMode> Display;
        public static ConfigEntry<InputMode> Input;
        public static ConfigEntry<PointerMode> Pointer;
        public static ConfigEntry<TurnMode> Turning;
        public static ConfigEntry<VignetteLevel> Vignette;
        public static ConfigEntry<AimMode> HeadAim;
        public static ConfigEntry<AimHand> AimWith;
        public static ConfigEntry<CrosshairMode> Crosshair;
        public static ConfigEntry<bool> WeaponInHand;
        public static ConfigEntry<UiBackground> UiBlack;
        public static ConfigEntry<ScreenBackground> Background;
        public static ConfigEntry<string> CameraName, Hidden;
        public static ConfigEntry<int> MenuKey;

        public static void Bind(ConfigFile cfg)
        {
            File = cfg;
            WorldScale = cfg.Bind("View", "WorldScale", 1f, "Player size multiplier. Above 1 the world looks smaller.");
            Height = cfg.Bind("View", "Height", 0f, "Metres added to the game camera height.");
            RenderScale = cfg.Bind("View", "RenderScale", 1f, "Eye resolution relative to the headset's recommendation.");
            AutoRenderScale = cfg.Bind("View", "AutoRenderScale", true, "Lower the eye resolution (down to half of RenderScale) while the headset misses frames.");
            Display = cfg.Bind("View", "Display", DisplayMode.Stereo,
                "Stereo: you stand in the game. PortXR: the game in 3D inside a big screen, played as on a monitor. Theater: the flat game on a big screen.");
            // Before Display, a Theater switch: carried over once, then dropped from the file.
            var theater = cfg.Bind("View", "Theater", false, "");
            if (theater.Value) Display.Value = DisplayMode.Theater;
            cfg.Remove(theater.Definition);
            Depth = cfg.Bind("View", "Depth", 1f, "Parallax mode: strength of the 3D, 0 (flat) to 1 (distant things an eye-width apart on the screen; more would make the eyes diverge).");
            // Set in VR with the screen handles; the screen always faces the spot the player recentered at.
            ScreenYaw = cfg.Bind("View", "ScreenYaw", 0f, "Theater and Parallax screen: degrees to the right of straight ahead.");
            ScreenPitch = cfg.Bind("View", "ScreenPitch", 0f, "Theater and Parallax screen: degrees above eye level.");
            ScreenDistance = cfg.Bind("View", "ScreenDistance", 3f, "Theater and Parallax screen: distance in metres.");
            ScreenWidth = cfg.Bind("View", "ScreenWidth", 3.2f, "Theater and Parallax screen: width in metres.");
            Background = cfg.Bind("View", "Background", ScreenBackground.Dark,
                "Theater and Parallax: around the screen. Passthrough: your room, on headset software that shares its passthrough with apps (not SteamVR).");
            UiDistance = cfg.Bind("View", "UiDistance", 1.2f, "Distance of the game UI panel in metres.");
            UiWidth = cfg.Bind("View", "UiWidth", 1.6f, "Width of the game UI panel in metres.");
            UiBlack = cfg.Bind("View", "UiBlack", UiBackground.Auto,
                "Auto: when the game paints the whole UI panel opaque, its black turns see-through. SeeThrough: always. Keep: never.");
            CameraName = cfg.Bind("Game", "CameraName", "", "GameObject name of the camera used as the head. Empty picks automatically.");
            Hidden = cfg.Bind("Game", "Hidden", "", "Objects hidden from the VR menu's Game tab, as hierarchy paths separated by ;.");
            Input = cfg.Bind("Controls", "Mode", InputMode.Auto,
                "Auto: Gamepad when the game ships a gamepad input library (Input System, Rewired, InControl, XInput wrappers), else KeyboardMouse. " +
                "KeyboardMouse: sticks drive WASD and mouse look (works in any game with keyboard controls). " +
                "Gamepad: controllers act as an Xbox pad; only games that read XInput directly see it. " +
                "Off: controllers only work the VR menu and laser pointer; play with your own keyboard, mouse or gamepad.");
            MouseSpeed = cfg.Bind("Controls", "MouseSpeed", 1200f, "KeyboardMouse mode: pixels per second at full right-stick deflection.");
            Pointer = cfg.Bind("Controls", "Pointer", PointerMode.Auto, "Auto: laser pointer works whenever the game shows its cursor. MenuOnly: only in the VR menu.");
            HeadAim = cfg.Bind("Controls", "HeadAim", AimMode.Auto,
                "On: the game camera turns to where you look, so first-person games aim with your head (also in their menus). Auto: on in " +
                "first-person games while the game hides the cursor, off for cameras that orbit the player. Off: the head only looks around.");
            AimWith = cfg.Bind("Controls", "AimWith", AimHand.Hand,
                "What head aim follows. Head: where you look. Hand: where the right controller points; the view still follows your head.");
            WeaponInHand = cfg.Bind("Controls", "WeaponInHand", true,
                "With hand aim, move the weapon the game hangs under its camera (arms and gun) into the right hand while the eyes render.");
            Crosshair = cfg.Bind("Controls", "Crosshair", CrosshairMode.Game,
                "While aiming, the game's crosshair sits on the flat UI panel and misses. Game: the middle of the UI (crosshair, grab and use " +
                "icons, prompts, hit markers) moves to what the camera aims at, at its depth. Lime: PortXR's own crosshair there instead, in the theme colour. Off: as the game draws it.");
            MenuKey = cfg.Bind("Controls", "MenuKey", 0x7A, "Windows virtual-key code that toggles the VR menu (0x7A = F11).");
            WristMenu = cfg.Bind("Controls", "WristMenu", true, "Look at the back of your left hand and pull the right trigger to open the VR menu.");
            FirstRun = cfg.Bind("Menu", "FirstRun", true, "Show the welcome card (recenter, scale, controls) the first time VR starts in this game.");
            Turning = cfg.Bind("Comfort", "Turning", TurnMode.Smooth,
                "Smooth: the right stick turns continuously. Snap15/30/45: each flick of the right stick turns by that many degrees.");
            Vignette = cfg.Bind("Comfort", "Vignette", VignetteLevel.Off, "Darken the edges of your view while moving or turning (Off, Low, High).");
            Bindings.Bind(cfg);
        }
    }

    [BepInPlugin("portxr.core", "PortXR", "0.7.1")]
#if IL2CPP
    public class Plugin : BepInEx.Unity.IL2CPP.BasePlugin
    {
        internal static new ManualLogSource Log;

        public override void Load()
        {
            Log = base.Log;
            if (!Start(Config)) return;
            Il2CppInterop.Runtime.Injection.ClassInjector.RegisterTypeInIl2Cpp<Rig>();
            AddComponent<Rig>();
        }
#else
    public class Plugin : BaseUnityPlugin
    {
        internal static ManualLogSource Log;

        void Awake()
        {
            Log = Logger;
            if (Start(Config)) gameObject.AddComponent<Rig>();
        }
#endif

        static bool Start(ConfigFile config)
        {
            // The app refuses anti-cheat games, but an update may have added it since: then nothing runs, and the
            // loader is switched off so later starts load nothing at all.
            string antiCheat = AntiCheat();
            if (antiCheat != null)
            {
                Log.LogError($"{antiCheat} found: PortXR stays off and disables its loader. Remove it with the PortXR app.");
                try
                {
                    string ini = Path.Combine(Paths.GameRootPath, "doorstop_config.ini");
                    File.WriteAllText(ini, System.Text.RegularExpressions.Regex.Replace(File.ReadAllText(ini), @"(?m)^enabled\s*=\s*true", "enabled = false"));
                }
                catch (System.Exception e) { Log.LogError($"Could not disable the loader: {e.Message}"); }
                return false;
            }
            Settings.Bind(config);
            TestChannel.ApplyRuntimeOverride();
            var api = SystemInfo.graphicsDeviceType;
            if (api != GraphicsDeviceType.Direct3D11 && api != GraphicsDeviceType.Direct3D12)
            {
                Log.LogError($"Graphics API {api} is not supported; VR stays off.");
                return false;
            }
            if (!Native.Load(Path.GetDirectoryName(typeof(Plugin).Assembly.Location)))
            {
                Log.LogError("portxr_xr.dll failed to load; VR stays off.");
                return false;
            }
            return true;
        }

        // Client anti-cheat by its files beside the game, or its modules in the process (same names as the app checks).
        static string AntiCheat()
        {
            string[] marks = { "easyanticheat", "start_protected_game", "eaanticheat", "battleye", "beclient", "beservice", "gameguard", "npgg",
                "xigncode", "x3.xem", "anticheatexpert", "ace-base", "sguard", "mhyprot", "hoyokprotect", "equ8", "pbsvc", "neac" };
            System.Func<string, bool> flagged = name =>
            {
                name = name.ToLowerInvariant();
                foreach (var m in marks)
                    if (name.StartsWith(m)) return true;
                return name.EndsWith("_be.exe") || name.Contains("anticheat") || name.Contains("anti-cheat") || name.Contains("anti_cheat");
            };
            try
            {
                foreach (var entry in Directory.GetFileSystemEntries(Paths.GameRootPath))
                    if (flagged(Path.GetFileName(entry))) return Path.GetFileName(entry);
                foreach (System.Diagnostics.ProcessModule module in System.Diagnostics.Process.GetCurrentProcess().Modules)
                    if (flagged(module.ModuleName)) return module.ModuleName;
            }
            catch (System.Exception e) { Log.LogWarning($"Anti-cheat check incomplete: {e.Message}"); }
            return null;
        }
    }
}
