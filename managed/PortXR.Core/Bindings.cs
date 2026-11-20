// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using BepInEx.Configuration;

namespace PortXR
{
    // Keyboard & mouse mode: which key or mouse button each controller input presses, by name in the config ([Bindings]
    // section). The core reads and applies them (native/src/rig.cpp bindings) and the VR menu edits them; declared here
    // so the file lists every one with its description, and tests can set them.
    internal static class Bindings
    {
        static readonly string[] Inputs =
            { "A", "B", "X", "Y", "LeftGrip", "RightGrip", "LeftStickClick", "RightStickClick", "LeftTrigger", "RightTrigger", "MenuTap", "MenuHold" };
        static readonly string[] Defaults =
            { "Space", "LeftCtrl", "E", "R", "LeftShift", "Q", "C", "None", "MouseRight", "MouseLeft", "Esc", "Tab" };
        static readonly string[] KeyNames =
        {
            "None", "Space", "Enter", "Esc", "Tab", "Backspace", "LeftShift", "LeftCtrl", "LeftAlt",
            "Q", "E", "R", "F", "G", "C", "V", "Z", "X", "T", "I", "M",
            "1", "2", "3", "4", "Up", "Down", "Left", "Right", "MouseLeft", "MouseRight", "MouseMiddle",
        };

        // Gestures beyond a plain press, for the first 10 inputs (the menu button has its own tap and hold): double
        // press, held past half a second, and pressed while the same hand's grip is held (not for the grips).
        const int Press = 0, Double = 1, Hold = 2, Combo = 3, GestureInputs = 10;
        static readonly string[] Suffix = { "", "Double", "Hold", "Combo" };
        static readonly string[] Meaning =
        {
            "what this controller input presses",
            "what pressing this input twice quickly presses (the first press still does its own)",
            "what holding this input for half a second presses (a short press then taps its own key on release)",
            "what this input presses while the same hand's grip is held (the grip's own key lets go meanwhile)",
        };

        static bool Applies(int gesture, int input) =>
            gesture == Press || input < GestureInputs && !(gesture == Combo && (input == 4 || input == 5));

        public static void Bind(ConfigFile cfg)
        {
            for (int g = 0; g < Suffix.Length; g++)
                for (int i = 0; i < Inputs.Length; i++)
                    if (Applies(g, i))
                        cfg.Bind("Bindings", Inputs[i] + Suffix[g], g == Press ? Defaults[i] : "None",
                            "Keyboard & mouse mode: " + Meaning[g] + ". One of: " + string.Join(", ", KeyNames) + ".");
        }
    }
}
