using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Parallax.App
{
    // Same tokens as the VR menu: warm grays, one accent, two weights, spacing on a 4/8/16/24/32 scale.
    internal class MainForm : Form
    {
        static readonly Color Bg = Color.FromArgb(31, 29, 27), Surface = Color.FromArgb(43, 40, 38), Hover = Color.FromArgb(58, 54, 51);
        static readonly Color Ink = Color.FromArgb(237, 232, 227), Muted = Color.FromArgb(163, 156, 148), Accent = Color.FromArgb(232, 131, 58);
        static readonly Font Body = new Font("Segoe UI", 10.5f), Strong = new Font("Segoe UI Semibold", 10.5f), Title = new Font("Segoe UI Semibold", 20f);

        readonly ListBox list = new ListBox();
        readonly Label name = new Label(), meta = new Label(), status = new Label();
        readonly Button play, uninstall, settings, log, export;
        List<Game> games = new List<Game>();

        public MainForm()
        {
            Text = "Parallax";
            BackColor = Bg;
            ForeColor = Ink;
            Font = Body;
            ClientSize = new Size(1040, 640);
            MinimumSize = new Size(800, 480);

            var left = new Panel { Dock = DockStyle.Left, Width = 360, BackColor = Surface, Padding = new Padding(16) };
            var header = new Panel { Dock = DockStyle.Top, Height = 48 };
            header.Controls.Add(new Label { Text = "Library", Font = Strong, AutoSize = true, Location = new Point(0, 12) });
            var add = MakeButton("Add game", false);
            add.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            add.Location = new Point(header.Width - add.Width, 4);
            add.Click += (s, e) => AddGame();
            header.Controls.Add(add);
            header.Resize += (s, e) => add.Left = header.Width - add.Width;

            list.Dock = DockStyle.Fill;
            list.BorderStyle = BorderStyle.None;
            list.BackColor = Surface;
            list.ForeColor = Ink;
            list.DrawMode = DrawMode.OwnerDrawFixed;
            list.ItemHeight = 56;
            list.DrawItem += DrawGame;
            list.SelectedIndexChanged += (s, e) => ShowSelected();
            left.Controls.Add(list);
            left.Controls.Add(header);

            var right = new Panel { Dock = DockStyle.Fill, Padding = new Padding(32) };
            name.Font = Title;
            name.AutoSize = true;
            name.Location = new Point(32, 32);
            meta.ForeColor = Muted;
            meta.AutoSize = true;
            meta.Location = new Point(32, 88);
            play = MakeButton("Play in VR", true);
            uninstall = MakeButton("Uninstall", false);
            settings = MakeButton("Settings file", false);
            log = MakeButton("Open log", false);
            export = MakeButton("Export logs", false);
            var actions = new FlowLayoutPanel { Location = new Point(32, 200), AutoSize = true, BackColor = Bg };
            actions.Controls.AddRange(new Control[] { play, uninstall, settings, log, export });
            status.AutoSize = true;
            status.MaximumSize = new Size(600, 0);
            status.Location = new Point(32, 264);
            right.Controls.AddRange(new Control[] { name, meta, actions, status });

            play.Click += (s, e) => Run(g =>
            {
                if (!g.Installed) Installer.Install(g.Dir);
                Library.Launch(g);
                return "Launching. Put on your headset; double-tap the left menu button for the VR menu.";
            });
            uninstall.Click += (s, e) => Run(g =>
            {
                Installer.Uninstall(g.Dir);
                return "Removed. The game folder is back to its original files.";
            });
            settings.Click += (s, e) => OpenFile(@"BepInEx\config\parallax.core.cfg", "Settings appear after the first launch in VR.");
            log.Click += (s, e) => OpenFile(@"BepInEx\LogOutput.log", "The log appears after the first launch in VR.");
            export.Click += (s, e) => Run(g =>
            {
                var zip = ExportLogs(g);
                Process.Start("explorer.exe", $"/select,\"{zip}\"");
                return "Logs saved to your desktop. Attach the zip to your bug report.";
            });

            Controls.Add(right);
            Controls.Add(left);
            ShowSelected();
            Shown += async (s, e) => await Reload();
        }

        Game Selected => list.SelectedIndex >= 0 ? games[list.SelectedIndex] : null;

        async Task Reload()
        {
            status.Text = "Scanning Steam libraries...";
            games = await Task.Run(Library.Scan);
            list.Items.Clear();
            foreach (var g in games) list.Items.Add(g.Name);
            status.Text = games.Count == 0 ? "No Unity games found. Use Add game to pick a folder." : "";
            if (games.Count > 0) list.SelectedIndex = 0;
        }

        void ShowSelected()
        {
            var g = Selected;
            foreach (var b in new[] { play, uninstall, settings, log, export }) b.Visible = g != null;
            if (g == null)
            {
                name.Text = "";
                meta.Text = "";
                return;
            }
            name.Text = g.Name;
            meta.Text = $"Unity {g.UnityVersion}  ·  {(g.Il2Cpp ? "IL2CPP" : "Mono")}  ·  {g.Badge}\n{g.Dir}";
            play.Enabled = g.Compat == Compat.Supported;
            uninstall.Visible = settings.Visible = log.Visible = g.Installed;
            status.Text = g.Blocker != null ? $"{g.Blocker} found. Injecting into anti-cheat games can get accounts banned, so this game is refused."
                : g.OtherLoader != null ? $"{g.OtherLoader} is already installed. Parallax will add itself alongside; other mods may interfere."
                : "";
        }

        void Run(Func<Game, string> action)
        {
            var g = Selected;
            try { status.Text = action(g); }
            catch (Exception ex) { status.Text = ex.Message; }
            list.Invalidate();
            var keep = status.Text;
            ShowSelected();
            if (keep.Length > 0) status.Text = keep;
        }

        void OpenFile(string rel, string missing)
        {
            var path = Path.Combine(Selected.Dir, rel);
            if (File.Exists(path)) Process.Start("notepad.exe", $"\"{path}\"");
            else status.Text = missing;
        }

        // One zip on the desktop for bug reports: plugin log, settings, install receipt and Unity's own Player.log.
        internal static string ExportLogs(Game g)
        {
            var desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            var stem = string.Concat(g.Name.Split(Path.GetInvalidFileNameChars()));
            var zip = Path.Combine(desktop, $"Parallax logs - {stem} - {DateTime.Now:yyyy-MM-dd HHmmss}.zip");
            using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
            {
                foreach (var rel in new[] { @"BepInEx\LogOutput.log", @"BepInEx\config\parallax.core.cfg" })
                    AddFile(z, Path.Combine(g.Dir, rel));
                AddFile(z, Installer.ReceiptPath(g.Dir));
                AddFile(z, PlayerLog(g));
                using (var w = new StreamWriter(z.CreateEntry("game.txt").Open()))
                    w.Write($"{g.Name}\r\nUnity {g.UnityVersion}, {(g.Il2Cpp ? "IL2CPP" : "Mono")}, {g.Badge}\r\n{Environment.OSVersion}\r\n");
            }
            return zip;
        }

        // The game may still be running and holding its logs open.
        static void AddFile(ZipArchive z, string path)
        {
            if (path == null || !File.Exists(path)) return;
            using (var src = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var dst = z.CreateEntry(Path.GetFileName(path)).Open())
                src.CopyTo(dst);
        }

        // Unity writes Player.log under LocalLow\<company>\<product>, both listed in <game>_Data\app.info.
        static string PlayerLog(Game g)
        {
            var info = Path.Combine(g.Dir, Path.GetFileNameWithoutExtension(g.Exe) + "_Data", "app.info");
            if (!File.Exists(info)) return null;
            var lines = File.ReadAllLines(info);
            if (lines.Length < 2) return null;
            var localLow = Path.Combine(Path.GetDirectoryName(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)), "LocalLow");
            return Path.Combine(localLow, lines[0], lines[1], "Player.log");
        }

        async void AddGame()
        {
            using (var dlg = new FolderBrowserDialog { Description = "Pick the folder that contains the game's .exe" })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                if (Game.Detect(dlg.SelectedPath).Exe == null)
                {
                    status.Text = "No Unity game in that folder (looking for an .exe next to a *_Data folder).";
                    return;
                }
                Library.AddManual(dlg.SelectedPath);
            }
            await Reload();
        }

        void DrawGame(object sender, DrawItemEventArgs e)
        {
            if (e.Index < 0) return;
            var g = games[e.Index];
            bool selected = (e.State & DrawItemState.Selected) != 0;
            using (var bg = new SolidBrush(selected ? Hover : Surface)) e.Graphics.FillRectangle(bg, e.Bounds);
            TextRenderer.DrawText(e.Graphics, g.Name, Strong, new Point(e.Bounds.X + 8, e.Bounds.Y + 8), Ink);
            var badge = g.Installed ? "Installed" : g.Badge;
            TextRenderer.DrawText(e.Graphics, badge, Body, new Point(e.Bounds.X + 8, e.Bounds.Y + 30),
                g.Compat == Compat.Supported ? Accent : Muted);
        }

        static Button MakeButton(string text, bool primary)
        {
            var b = new Button
            {
                Text = text,
                AutoSize = true,
                Padding = new Padding(16, 6, 16, 6),
                Margin = new Padding(0, 0, 8, 0),
                FlatStyle = FlatStyle.Flat,
                BackColor = primary ? Accent : Surface,
                ForeColor = primary ? Bg : Ink,
                Font = primary ? Strong : Body,
                Cursor = Cursors.Hand,
            };
            b.FlatAppearance.BorderSize = 0;
            b.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(242, 148, 80) : Hover;
            return b;
        }
    }
}
