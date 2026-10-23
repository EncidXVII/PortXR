using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Parallax.App
{
    internal static class Program
    {
        [DllImport("kernel32")] static extern bool AttachConsole(int pid);

        // CLI: Parallax.exe install|uninstall|detect <gameDir> (used by the harness); no arguments opens the app.
        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length == 2)
            {
                AttachConsole(-1);
                try
                {
                    switch (args[0])
                    {
                        case "install": Installer.Install(args[1]); break;
                        case "uninstall": Installer.Uninstall(args[1]); break;
                        case "detect":
                            var g = Game.Detect(args[1]);
                            Console.WriteLine($"{g.Name}: {g.Badge}, Unity {g.UnityVersion}, loader {g.OtherLoader ?? "none"}");
                            break;
                        default: throw new ArgumentException("unknown command " + args[0]);
                    }
                    return 0;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(ex.Message);
                    return 1;
                }
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }
}
