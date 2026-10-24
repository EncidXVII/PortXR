using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Parallax.App
{
    internal enum Compat { Supported, Il2Cpp, AntiCheat, X86, NotUnity }

    internal class Game
    {
        public string Name, Dir, Exe, UnityVersion, Blocker, OtherLoader;
        public int SteamAppId;
        public bool Il2Cpp, X86;

        public Compat Compat => Exe == null ? Compat.NotUnity : Blocker != null ? Compat.AntiCheat : X86 ? Compat.X86
            : Il2Cpp ? Compat.Il2Cpp : Compat.Supported;
        public bool Installed => File.Exists(Installer.ReceiptPath(Dir));

        public string Badge => Compat switch
        {
            Compat.Supported => "Ready",
            Compat.Il2Cpp => "IL2CPP: not yet",
            Compat.AntiCheat => "Anti-cheat: blocked",
            Compat.X86 => "32-bit: no VR runtime",
            _ => "Not a Unity game",
        };

        public static Game Detect(string dir)
        {
            var g = new Game { Dir = dir, Name = Path.GetFileName(dir.TrimEnd('\\')) };
            foreach (var exe in Directory.GetFiles(dir, "*.exe"))
            {
                var data = Path.Combine(dir, Path.GetFileNameWithoutExtension(exe) + "_Data");
                if (!Directory.Exists(data) || !File.Exists(Path.Combine(data, "globalgamemanagers")) && !File.Exists(Path.Combine(data, "mainData"))) continue;
                g.Exe = exe;
                var player = Path.Combine(dir, "UnityPlayer.dll");
                g.UnityVersion = FileVersionInfo.GetVersionInfo(File.Exists(player) ? player : exe).FileVersion;
                g.Il2Cpp = File.Exists(Path.Combine(dir, "GameAssembly.dll"));
                g.X86 = IsX86(exe);
                break;
            }
            if (Directory.Exists(Path.Combine(dir, "EasyAntiCheat")) || Directory.GetFiles(dir, "EasyAntiCheat*.exe").Any()) g.Blocker = "EasyAntiCheat";
            else if (Directory.Exists(Path.Combine(dir, "BattlEye")) || Directory.GetFiles(dir, "*_BE.exe").Any()) g.Blocker = "BattlEye";
            if (Directory.Exists(Path.Combine(dir, "MelonLoader"))) g.OtherLoader = "MelonLoader";
            else if (Directory.Exists(Path.Combine(dir, "BepInEx")) && !g.Installed) g.OtherLoader = "BepInEx";
            return g;
        }

        // OpenXR runtimes (SteamVR, Virtual Desktop, Meta) ship 64-bit only.
        static bool IsX86(string exe)
        {
            using (var r = new BinaryReader(File.OpenRead(exe)))
            {
                r.BaseStream.Position = 0x3C;
                r.BaseStream.Position = r.ReadInt32() + 4;
                return r.ReadUInt16() == 0x14C;
            }
        }
    }

    internal static class Library
    {
        static readonly string ManualList = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Parallax", "games.txt");

        public static List<Game> Scan()
        {
            var dirs = new List<(string dir, int appId)>();
            foreach (var steamapps in SteamLibraries())
            {
                foreach (var manifest in Directory.GetFiles(steamapps, "appmanifest_*.acf"))
                {
                    var text = File.ReadAllText(manifest);
                    var installDir = Regex.Match(text, "\"installdir\"\\s+\"([^\"]+)\"").Groups[1].Value;
                    int.TryParse(Regex.Match(text, "\"appid\"\\s+\"(\\d+)\"").Groups[1].Value, out int appId);
                    var dir = Path.Combine(steamapps, "common", installDir);
                    if (installDir.Length > 0 && Directory.Exists(dir)) dirs.Add((dir, appId));
                }
            }
            if (File.Exists(ManualList))
                dirs.AddRange(File.ReadAllLines(ManualList).Where(Directory.Exists).Select(d => (d, 0)));

            var games = new List<Game>();
            foreach (var (dir, appId) in dirs.Distinct())
            {
                try
                {
                    var g = Game.Detect(dir);
                    if (g.Exe == null) continue;
                    g.SteamAppId = appId;
                    games.Add(g);
                }
                catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException) { }
            }
            return games.OrderBy(g => g.Name, StringComparer.OrdinalIgnoreCase).ToList();
        }

        public static void AddManual(string dir)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(ManualList));
            File.AppendAllLines(ManualList, new[] { dir });
        }

        static IEnumerable<string> SteamLibraries()
        {
            var steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
            if (steam == null) yield break;
            var vdf = Path.Combine(steam, "config", "libraryfolders.vdf");
            var roots = File.Exists(vdf)
                ? Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\"").Cast<Match>().Select(m => m.Groups[1].Value.Replace(@"\\", @"\"))
                : new[] { steam };
            foreach (var root in roots)
            {
                var steamapps = Path.Combine(root, "steamapps");
                if (Directory.Exists(steamapps)) yield return steamapps;
            }
        }

        public static void Launch(Game g)
        {
            if (g.SteamAppId > 0) Process.Start("steam://rungameid/" + g.SteamAppId);
            else Process.Start(new ProcessStartInfo(g.Exe) { WorkingDirectory = g.Dir, UseShellExecute = false });
        }
    }
}
