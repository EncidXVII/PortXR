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

        // Gestures beyond a plain press, for the first 10 inputs (the menu button has its own tap and hold): double
        // press, held past half a second, and pressed while the same hand's grip is held (not for the grips).
        public const int Press = 0, Double = 1, Hold = 2, Combo = 3, GestureInputs = 10;
        public static readonly string[] Gestures = { "Press", "Double press", "Hold", "Grip +" };
        static readonly string[] Suffix = { "", "Double", "Hold", "Combo" };
        static readonly string[] Meaning =
        {
            "what this controller input presses",
            "what pressing this input twice quickly presses (the first press still does its own)",
            "what holding this input for half a second presses (a short press then taps its own key on release)",
            "what this input presses while the same hand's grip is held (the grip's own key lets go meanwhile)",
        };

        // [gesture][input]; null where a gesture does not apply to an input.
        public static ConfigEntry<string>[][] Entries;

        public static bool Applies(int gesture, int input) =>
            gesture == Press || input < GestureInputs && !(gesture == Combo && (input == 4 || input == 5));

        public static void Bind(ConfigFile cfg)
        {
            Entries = new ConfigEntry<string>[Gestures.Length][];
            for (int g = 0; g < Gestures.Length; g++)
            {
                Entries[g] = new ConfigEntry<string>[Inputs.Length];
                for (int i = 0; i < Inputs.Length; i++)
                    if (Applies(g, i))
                        Entries[g][i] = cfg.Bind("Bindings", Inputs[i] + Suffix[g], g == Press ? Defaults[i] : "None",
                            "Keyboard & mouse mode: " + Meaning[g] + ". One of: " + string.Join(", ", KeyNames) + ".");
            }
        }

        public static string Hand(int input) => Array.IndexOf(LeftHand, input) >= 0 ? "Left" : "Right";

        public static string Name(int gesture, int input) =>
            (gesture == Combo ? "Grip + " : "") + Labels[input] + (gesture == Double ? " twice" : gesture == Hold ? " held" : "");

        static int Code(ConfigEntry<string> e)
        {
            int k = e == null ? -1 : Array.IndexOf(KeyNames, e.Value);
            return k >= 0 ? KeyCodes[k] : 0;
        }

        public static void Codes(ref InputConfig cfg)
        {
            for (int i = 0; i < Inputs.Length; i++) cfg.binds[i] = Code(Entries[Press][i]);
            for (int i = 0; i < GestureInputs; i++)
            {
                cfg.twice[i] = Code(Entries[Double][i]);
                cfg.held[i] = Code(Entries[Hold][i]);
                cfg.combo[i] = Code(Entries[Combo][i]);
            }
        }

        // First key bound twice, as a sentence; null when every binding is unique.
        public static string Conflict()
        {
            var seen = new System.Collections.Generic.Dictionary<string, string>();
            for (int g = 0; g < Gestures.Length; g++)
                for (int i = 0; i < Inputs.Length; i++)
                {
                    var e = Entries[g][i];
                    if (e == null || e.Value == "None") continue;
                    string where = $"{Hand(i)} {Name(g, i)}";
                    if (seen.TryGetValue(e.Value, out var first)) return $"{e.Value} is on both {first} and {where}.";
                    seen[e.Value] = where;
                }
            return null;
        }
    }
}
