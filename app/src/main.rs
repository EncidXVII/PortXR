#![windows_subsystem = "windows"]

mod games;
mod installer;
mod settings;
mod ui;

use std::path::Path;
use std::process::ExitCode;

// CLI: Parallax.exe install|install-anyway|uninstall|detect|export <gameDir> (used by the harness), and
// play <gameDir> [<pid>] (the VR menu's Library tab: starts that game once process <pid>, the game it ran in, has quit). Parallax.exe <gameDir> opens the
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

/// Waits up to 60 s for a process to end: a game holds the headset until it has quit.
fn wait_for_exit(pid: u32) {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::Threading::{OpenProcess, PROCESS_SYNCHRONIZE, WaitForSingleObject};
        let h = OpenProcess(PROCESS_SYNCHRONIZE, 0, pid);
        if !h.is_null() {
            WaitForSingleObject(h, 60_000);
            CloseHandle(h);
        }
    }
    #[cfg(not(windows))]
    let _ = pid;
    // Steam marks the game as stopped a moment after its process ends.
    std::thread::sleep(std::time::Duration::from_secs(2));
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // The Steam library's Unity games and what is known about them before install, for checking the library scan.
    if args.len() == 1 && args[0] == "owned" {
        attach_console();
        let t = std::time::Instant::now();
        for g in games::scan() {
            println!("here\t{}\t{}\t{}\t{}", g.engine, g.unity, g.badge(), g.name);
        }
        println!("scan took {:?}", t.elapsed());
        for o in games::steam_library() {
            println!("{}	{}	{}", o.app_id, o.last_played, o.name);
        }
        return ExitCode::SUCCESS;
    }
    if let [cmd, dir, pid] = args.as_slice()
        && cmd == "play" {
            wait_for_exit(pid.parse().unwrap_or(0));
            return match cli(cmd, Path::new(dir)) {
                Ok(()) => ExitCode::SUCCESS,
                Err(_) => ExitCode::FAILURE,
            };
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
        "install-anyway" => installer::install_anyway(dir),
        // Started from a game's VR menu: the same checks and plugin update as Play in VR.
        "play" => {
            let g = games::inspect(dir).ok_or("not a Unity game")?;
            if !g.installed() {
                return Err("VR isn't added to this game.".into());
            }
            installer::update(dir)?;
            g.launch()?;
            games::mark_played(dir);
            Ok(())
        }
        "uninstall" => installer::uninstall(dir),
        "update" => installer::update(dir).map(|changed| println!("{}", if changed { "updated" } else { "up to date" })),
        "detect" => {
            let g = games::inspect(dir).ok_or("no game found")?;
            println!("{}: {}, {}, loader {}", g.name, g.badge(), g.engine_label(), g.other_loader.unwrap_or("none"));
            Ok(())
        }
        "export" => {
            println!("{}", games::export_logs(&games::detect(dir).ok_or("not a Unity game")?)?.display());
            Ok(())
        }
        _ => Err(format!("unknown command {cmd}").into()),
    }
}
