using UnityEngine;

namespace Parallax
{
    // The accent colour, one choice for every game and the app: kept by name in %APPDATA%\Parallax\theme.txt, which
    // Parallax.exe writes too. The same eight as DesktopVR and FitnessVR; only the accent changes, never the warning red.
    internal static class Theme
    {
        internal static readonly string[] Names = { "Lime", "Mint", "Aqua", "Sky", "Violet", "Pink", "Amber", "White" };
        static readonly int[] Rgbs = { 0xC6FF1A, 0x5CF2A0, 0x3DF2E0, 0x5CC8FF, 0xB49AFF, 0xFF7ADB, 0xFFB627, 0xF5F5F7 };

        static int index = -1;
        static float checkedAt = -10;

        static string Path => System.IO.Path.Combine(
            System.Environment.GetFolderPath(System.Environment.SpecialFolder.ApplicationData), "Parallax\\theme.txt");

        // The chosen theme; the file is read again every few seconds, so a change in the app shows in a running game.
        internal static int Index
        {
            get
            {
                if (index < 0 || Time.unscaledTime - checkedAt > 3)
                {
                    checkedAt = Time.unscaledTime;
                    index = Read();
                }
                return index;
            }
        }

        internal static Color Accent => Rgb(Rgbs[Index]);

        internal static Color Of(int i) => Rgb(Rgbs[i]);

        // Pressed fill: the accent a step darker.
        internal static Color Pressed
        {
            get { var c = Accent; return new Color(c.r * 0.86f, c.g * 0.86f, c.b * 0.86f); }
        }

        internal static void Set(int i)
        {
            index = Mathf.Clamp(i, 0, Names.Length - 1);
            checkedAt = Time.unscaledTime;
            try
            {
                System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
                System.IO.File.WriteAllText(Path, Names[index]);
            }
            catch (System.Exception e) { Plugin.Log.LogWarning("theme: " + e.Message); }
        }

        static int Read()
        {
            try
            {
                string name = System.IO.File.ReadAllText(Path).Trim();
                int i = System.Array.FindIndex(Names, n => string.Equals(n, name, System.StringComparison.OrdinalIgnoreCase));
                return i < 0 ? 0 : i;
            }
            catch (System.Exception) { return 0; }
        }

        static Color Rgb(int v) => new Color((v >> 16 & 255) / 255f, (v >> 8 & 255) / 255f, (v & 255) / 255f);
    }
}
