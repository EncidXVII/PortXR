#![windows_subsystem = "windows"]

mod games;
mod installer;
mod settings;
mod ui;

use std::path::Path;
use std::process::ExitCode;

// CLI: Parallax.exe install|uninstall|detect|export <gameDir> (used by the harness). Parallax.exe <gameDir> opens the
// app on that game's page (shortcuts); no arguments opens the library.
fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let [cmd, dir] = args.as_slice() {
        // A GUI-subsystem exe has no console of its own; borrow the caller's so messages show up there.
        unsafe { windows_sys::Win32::System::Console::AttachConsole(windows_sys::Win32::System::Console::ATTACH_PARENT_PROCESS) };
        return match cli(cmd, Path::new(dir)) {
            Ok(()) => ExitCode::SUCCESS,
            Err(e) => {
                eprintln!("{e}");
                ExitCode::FAILURE
            }
        };
    }
    match ui::run(args.first().map(std::path::PathBuf::from)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(_) => ExitCode::FAILURE,
    }
}

fn cli(cmd: &str, dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    match cmd {
        "install" => installer::install(dir),
        "uninstall" => installer::uninstall(dir),
        "detect" => {
            let g = games::detect(dir).ok_or("not a Unity game")?;
            println!("{}: {}, Unity {}, {}, loader {}", g.name, g.badge(), g.unity, g.backend(), g.other_loader.unwrap_or("none"));
            Ok(())
        }
        "export" => {
            println!("{}", games::export_logs(&games::detect(dir).ok_or("not a Unity game")?)?.display());
            Ok(())
        }
        _ => Err(format!("unknown command {cmd}").into()),
    }
}
