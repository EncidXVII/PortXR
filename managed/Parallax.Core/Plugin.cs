using System.IO;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using UnityEngine;
using UnityEngine.Rendering;

namespace Parallax
{
    public enum InputMode { Gamepad, KeyboardMouse, Off }
    public enum PointerMode { Auto, MenuOnly }
    public enum TurnMode { Smooth, Snap15, Snap30, Snap45 }
    public enum VignetteLevel { Off, Low, High }

    internal static class Settings
    {
        public static ConfigFile File;
        public static ConfigEntry<float> WorldScale, Height, RenderScale, MouseSpeed, UiDistance, UiWidth;
        public static ConfigEntry<bool> Theater, AutoRenderScale, WristMenu, FirstRun;
        public static ConfigEntry<InputMode> Input;
        public static ConfigEntry<PointerMode> Pointer;
        public static ConfigEntry<TurnMode> Turning;
        public static ConfigEntry<VignetteLevel> Vignette;
        public static ConfigEntry<string> CameraName, Hidden;
        public static ConfigEntry<int> MenuKey;

        public static void Bind(ConfigFile cfg)
        {
            File = cfg;
            WorldScale = cfg.Bind("View", "WorldScale", 1f, "Player size multiplier. Above 1 the world looks smaller.");
            Height = cfg.Bind("View", "Height", 0f, "Metres added to the game camera height.");
            RenderScale = cfg.Bind("View", "RenderScale", 1f, "Eye resolution relative to the headset's recommendation.");
            AutoRenderScale = cfg.Bind("View", "AutoRenderScale", true, "Lower the eye resolution (down to half of RenderScale) while the headset misses frames.");
            Theater = cfg.Bind("View", "Theater", false, "Show the flat game on a large screen instead of stereo.");
            UiDistance = cfg.Bind("View", "UiDistance", 1.2f, "Distance of the game UI panel in metres.");
            UiWidth = cfg.Bind("View", "UiWidth", 1.6f, "Width of the game UI panel in metres.");
            CameraName = cfg.Bind("Game", "CameraName", "", "GameObject name of the camera used as the head. Empty picks automatically.");
            Hidden = cfg.Bind("Game", "Hidden", "", "Objects hidden from the VR menu's Game tab, as hierarchy paths separated by ;.");
            Input = cfg.Bind("Controls", "Mode", InputMode.KeyboardMouse,
                "KeyboardMouse: sticks drive WASD and mouse look (works in any game with keyboard controls). " +
                "Gamepad: controllers act as an Xbox pad; only games that read XInput directly see it. " +
                "Off: controllers only work the VR menu and laser pointer; play with your own keyboard, mouse or gamepad.");
            MouseSpeed = cfg.Bind("Controls", "MouseSpeed", 1200f, "KeyboardMouse mode: pixels per second at full right-stick deflection.");
            Pointer = cfg.Bind("Controls", "Pointer", PointerMode.Auto, "Auto: laser pointer works whenever the game shows its cursor. MenuOnly: only in the VR menu.");
            MenuKey = cfg.Bind("Controls", "MenuKey", 0x7A, "Windows virtual-key code that toggles the VR menu (0x7A = F11).");
            WristMenu = cfg.Bind("Controls", "WristMenu", true, "Look at the back of your left hand and pull the right trigger to open the VR menu.");
            FirstRun = cfg.Bind("Menu", "FirstRun", true, "Show the welcome card (recenter, scale, controls) the first time VR starts in this game.");
            Turning = cfg.Bind("Comfort", "Turning", TurnMode.Smooth,
                "Smooth: the right stick turns continuously. Snap15/30/45: each flick of the right stick turns by that many degrees.");
            Vignette = cfg.Bind("Comfort", "Vignette", VignetteLevel.Off, "Darken the edges of your view while moving or turning (Off, Low, High).");
            Bindings.Bind(cfg);
        }
    }

    [BepInPlugin("parallax.core", "Parallax", "0.1.0")]
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
            Settings.Bind(config);
            var api = SystemInfo.graphicsDeviceType;
            if (api != GraphicsDeviceType.Direct3D11 && api != GraphicsDeviceType.Direct3D12)
            {
                Log.LogError($"Graphics API {api} is not supported; VR stays off.");
                return false;
            }
            if (!Native.Load(Path.GetDirectoryName(typeof(Plugin).Assembly.Location)))
            {
                Log.LogError("parallax_xr.dll failed to load; VR stays off.");
                return false;
            }
            return true;
        }
    }
}
