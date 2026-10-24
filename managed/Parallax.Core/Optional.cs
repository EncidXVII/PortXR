using System;

namespace Parallax
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
            catch (Exception ex) when (ex is NotSupportedException || ex is MissingMethodException || ex is EntryPointNotFoundException)
            {
                off = true;
                Plugin.Log.LogWarning($"{feature} is unavailable in this game ({ex.Message}); continuing without it.");
                return false;
            }
        }
    }
}
