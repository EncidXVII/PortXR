// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using System;

namespace PortXR
{
    // An engine call an IL2CPP game may have stripped. The first failure turns the feature off for the session (one
    // log line) instead of breaking every frame.
    internal sealed class Optional
    {
        readonly string feature;
        bool off;

        public Optional(string feature) { this.feature = feature; }

        public bool Run(Action action)
        {
            if (off) return false;
            try
            {
                action();
                return true;
            }
            catch (Exception ex) when (ex is NotSupportedException || ex is MissingMethodException || ex is EntryPointNotFoundException || ex is TypeInitializationException || ex is InvalidProgramException
                                       // IL2CPP wraps the game side's own exceptions (a null argument it refuses).
                                       || ex.GetType().Name == "Il2CppException")
            {
                off = true;
                // The first stack frame names the stripped member, which is what a fix needs.
                string at = ex.StackTrace?.Split('\n')[0].Trim() ?? "";
                Plugin.Log.LogWarning($"{feature} is unavailable in this game ({ex.Message} {at}); continuing without it.");
                return false;
            }
        }
    }
}
