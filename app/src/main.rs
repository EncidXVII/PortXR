#![windows_subsystem = "windows"]

mod games;
mod installer;
mod settings;
mod ui;

use std::path::Path;
use std::process::ExitCode;

// CLI: Parallax.exe install|uninstall|detect|export <gameDir> (used by the harness). Parallax.exe <gameDir> opens the
// app on that game's page (shortcuts); no arguments opens the library.
fn attach_console() {
    #[cfg(windows)]
    unsafe {
        windows_sys::Win32::System::Console::AttachConsole(windows_sys::Win32::System::Console::ATTACH_PARENT_PROCESS)
    };
}

/// Windows' "Animation effects" setting (Settings > Accessibility > Visual effects), as DesktopVR reads it; on when
/// unknown. Off keeps every state change and drops the movement.
pub fn animations_enabled() -> bool {
    #[cfg(windows)]
    {
        use windows_sys::Win32::UI::WindowsAndMessaging::{SPI_GETCLIENTAREAANIMATION, SystemParametersInfoW};
        let mut on: i32 = 1;
        // SAFETY: plain query into a BOOL we own.
        let ok = unsafe { SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, (&raw mut on).cast(), 0) };
        ok == 0 || on != 0
    }
    #[cfg(not(windows))]
    true
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // The Steam library's Unity games and what is known about them before install, for checking the library scan.
    if args.len() == 1 && args[0] == "owned" {
        attach_console();
        for o in games::steam_unity_library() {
            println!("{}	{}	{}	{}", o.app_id, o.blocker.unwrap_or("ok"), o.last_played, o.name);
        }
        return ExitCode::SUCCESS;
    }
    if let [cmd, dir] = args.as_slice() {
        // A GUI-subsystem exe has no console of its own; borrow the caller's so messages show up there.
        attach_console();
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
        "update" => installer::update(dir).map(|changed| println!("{}", if changed { "updated" } else { "up to date" })),
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
