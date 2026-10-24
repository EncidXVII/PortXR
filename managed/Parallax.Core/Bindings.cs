using System;
using BepInEx.Configuration;

namespace Parallax
{
    // Keyboard & mouse mode: which key or mouse button each controller input presses. Stored by name in the config
    // ([Bindings] section) and handed to the native emulation as scan codes in the native PrxBind order.
    internal static class Bindings
    {
        public static readonly string[] Inputs =
            { "A", "B", "X", "Y", "LeftGrip", "RightGrip", "LeftStickClick", "RightStickClick", "LeftTrigger", "RightTrigger", "MenuTap", "MenuHold" };
        public static readonly string[] Labels =
            { "A", "B", "X", "Y", "Grip", "Grip", "Stick click", "Stick click", "Trigger", "Trigger", "Menu tap", "Menu hold" };
        static readonly string[] Defaults =
            { "Space", "LeftCtrl", "E", "R", "LeftShift", "Q", "C", "None", "MouseRight", "MouseLeft", "Esc", "Tab" };
        public static readonly int[] LeftHand = { 2, 3, 4, 8, 6, 10, 11 }, RightHand = { 0, 1, 5, 9, 7 };

        // Scan codes; 0x100 marks an extended key, 0x1000 + n is mouse button n (left, right, middle).
        public static readonly string[] KeyNames =
        {
            "None", "Space", "Enter", "Esc", "Tab", "Backspace", "LeftShift", "LeftCtrl", "LeftAlt",
            "Q", "E", "R", "F", "G", "C", "V", "Z", "X", "T", "I", "M",
            "1", "2", "3", "4", "Up", "Down", "Left", "Right", "MouseLeft", "MouseRight", "MouseMiddle",
        };
        static readonly int[] KeyCodes =
        {
            0, 0x39, 0x1C, 0x01, 0x0F, 0x0E, 0x2A, 0x1D, 0x38,
            0x10, 0x12, 0x13, 0x21, 0x22, 0x2E, 0x2F, 0x2C, 0x2D, 0x14, 0x17, 0x32,
            0x02, 0x03, 0x04, 0x05, 0x148, 0x150, 0x14B, 0x14D, 0x1000, 0x1001, 0x1002,
        };

        public static ConfigEntry<string>[] Entries;

        public static void Bind(ConfigFile cfg)
        {
            Entries = new ConfigEntry<string>[Inputs.Length];
            for (int i = 0; i < Inputs.Length; i++)
                Entries[i] = cfg.Bind("Bindings", Inputs[i], Defaults[i],
                    "Keyboard & mouse mode: what this controller input presses. One of: " + string.Join(", ", KeyNames) + ".");
        }

        public static string Hand(int input) => Array.IndexOf(LeftHand, input) >= 0 ? "Left" : "Right";

        public static void Codes(int[] codes)
        {
            for (int i = 0; i < Inputs.Length; i++)
            {
                int k = Array.IndexOf(KeyNames, Entries[i].Value);
                codes[i] = k >= 0 ? KeyCodes[k] : 0;
            }
        }

        // First key bound to two inputs, as a sentence; null when every binding is unique.
        public static string Conflict()
        {
            for (int i = 0; i < Inputs.Length; i++)
                for (int j = i + 1; j < Inputs.Length; j++)
                    if (Entries[i].Value != "None" && Entries[i].Value == Entries[j].Value)
                        return $"{Entries[i].Value} is on both {Hand(i)} {Labels[i]} and {Hand(j)} {Labels[j]}.";
            return null;
        }
    }
}
