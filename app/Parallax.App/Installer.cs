using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;

namespace Parallax.App
{
    [DataContract]
    internal class Receipt
    {
        [DataMember] public string Version;
        [DataMember] public bool LoaderPreexisting;
        [DataMember] public List<string> CreatedDirs = new List<string>();
        [DataMember] public List<ReceiptFile> Files = new List<ReceiptFile>();
    }

    [DataContract]
    internal class ReceiptFile
    {
        [DataMember] public string Path;
        [DataMember] public string Sha256;
        [DataMember] public bool BackedUp;
    }

    // Uninstall must leave the game folder byte-identical: every overwritten file is backed up, and a loader
    // folder we created is removed whole because the loader writes logs, caches and configs into it at runtime.
    internal static class Installer
    {
        public const string Version = "0.1.0";
        const string BackupDir = ".vrbackup";
        const string PluginDir = @"BepInEx\plugins\Parallax";

        // Mono games get BepInEx 5; IL2CPP games get BepInEx 6 with its .NET runtime and the IL2CPP plugin build.
        public static string Payload(Game game) =>
            Path.Combine(AppDomain.CurrentDomain.BaseDirectory, game.Il2Cpp ? "payload-il2cpp" : "payload");
        public static string ReceiptPath(string gameDir) => Path.Combine(gameDir, ".vrinstall.json");

        public static void Install(string gameDir)
        {
            var game = Game.Detect(gameDir);
            if (game.Exe == null) throw new InvalidOperationException("No Unity game found in this folder.");
            if (game.Blocker != null) throw new InvalidOperationException($"{game.Blocker} detected: injecting would risk a ban, so this game is refused.");
            if (game.X86) throw new InvalidOperationException("32-bit game: no OpenXR runtime supports 32-bit apps.");
            if (game.OtherLoader == "MelonLoader") throw new InvalidOperationException("MelonLoader is installed; remove it first, the two loaders conflict.");
            if (File.Exists(ReceiptPath(gameDir))) Uninstall(gameDir);

            var payload = Payload(game);
            var receipt = new Receipt { Version = Version, LoaderPreexisting = Directory.Exists(Path.Combine(gameDir, "BepInEx")) };
            var sources = Directory.GetFiles(payload, "*", SearchOption.AllDirectories)
                .Select(p => p.Substring(payload.Length + 1))
                .Where(rel => !receipt.LoaderPreexisting || rel.StartsWith(PluginDir, StringComparison.OrdinalIgnoreCase))
                .ToList();
            // Top-level folders that appear with the install (BepInEx, dotnet) are removed whole on uninstall: the
            // loader writes logs, caches and generated assemblies into them at runtime.
            receipt.CreatedDirs = sources.Where(rel => rel.Contains('\\')).Select(rel => rel.Split('\\')[0])
                .Distinct().Where(top => !Directory.Exists(Path.Combine(gameDir, top))).ToList();
            foreach (var rel in sources)
            {
                var target = Path.Combine(gameDir, rel);
                var entry = new ReceiptFile { Path = rel, BackedUp = File.Exists(target) };
                if (entry.BackedUp)
                {
                    var backup = Path.Combine(gameDir, BackupDir, rel);
                    Directory.CreateDirectory(Path.GetDirectoryName(backup));
                    File.Copy(target, backup, true);
                }
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                File.Copy(Path.Combine(payload, rel), target, true);
                entry.Sha256 = Hash(target);
                receipt.Files.Add(entry);
            }
            using (var f = File.Create(ReceiptPath(gameDir)))
                new DataContractJsonSerializer(typeof(Receipt)).WriteObject(f, receipt);
        }

        public static void Uninstall(string gameDir)
        {
            Receipt receipt;
            using (var f = File.OpenRead(ReceiptPath(gameDir)))
                receipt = (Receipt)new DataContractJsonSerializer(typeof(Receipt)).ReadObject(f);

            foreach (var dir in receipt.CreatedDirs) DeleteDir(Path.Combine(gameDir, dir));
            if (receipt.LoaderPreexisting)
            {
                DeleteDir(Path.Combine(gameDir, PluginDir));
                File.Delete(Path.Combine(gameDir, @"BepInEx\config\parallax.core.cfg"));
            }
            foreach (var entry in receipt.Files)
            {
                var target = Path.Combine(gameDir, entry.Path);
                if (entry.BackedUp)
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    File.Copy(Path.Combine(gameDir, BackupDir, entry.Path), target, true);
                }
                else if (File.Exists(target)) File.Delete(target);
            }
            DeleteDir(Path.Combine(gameDir, BackupDir));
            File.Delete(ReceiptPath(gameDir));
        }

        static void DeleteDir(string dir)
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }

        static string Hash(string path)
        {
            using (var sha = SHA256.Create())
            using (var f = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
        }
    }
}
