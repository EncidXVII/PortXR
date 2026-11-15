use std::error::Error;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::games;

const BACKUP_DIR: &str = ".vrbackup";
const PLUGIN_DIR: &str = r"BepInEx\plugins\Parallax";
pub const CONFIG: &str = r"BepInEx\config\parallax.core.cfg";

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
struct Receipt {
    version: String,
    loader_preexisting: bool,
    created_dirs: Vec<String>,
    files: Vec<ReceiptFile>,
    /// Added anyway, after the app's caution (Game::caution): updates keep it instead of refusing the game.
    #[serde(default)]
    forced: bool,
}

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
struct ReceiptFile {
    path: String,
    sha256: String,
    backed_up: bool,
}

pub fn receipt_path(game_dir: &Path) -> PathBuf {
    game_dir.join(".vrinstall.json")
}

// Mono games get BepInEx 5; IL2CPP games get BepInEx 6 with its .NET runtime and the IL2CPP plugin build.
fn payload(il2cpp: bool) -> Result<PathBuf, Box<dyn Error>> {
    let exe = std::env::current_exe()?;
    Ok(exe.with_file_name(if il2cpp { "payload-il2cpp" } else { "payload" }))
}

/// Paths relative to `root`, backslash-separated as the receipt stores them.
fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) -> std::io::Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_dir() {
            walk(root, &path, out)?;
        } else {
            out.push(path.strip_prefix(root).unwrap().to_string_lossy().replace('/', "\\"));
        }
    }
    Ok(())
}

// Uninstall must leave the game folder byte-identical: every overwritten file is backed up, and a loader folder we
// created is removed whole because the loader writes logs, caches and configs into it at runtime.
pub fn install(game_dir: &Path) -> Result<(), Box<dyn Error>> {
    install_with(game_dir, false)
}

/// For a game the app cautions about (Game::caution): the player read why and chose to try.
pub fn install_anyway(game_dir: &Path) -> Result<(), Box<dyn Error>> {
    install_with(game_dir, true)
}

fn install_with(game_dir: &Path, force: bool) -> Result<(), Box<dyn Error>> {
    let game = checked(game_dir, force)?;
    if game.godot_vr() {
        return install_godot(&game);
    }
    if game.unreal_vr() {
        return install_unreal(&game);
    }
    if force && game.caution().is_none() {
        return Err("Parallax can't run in this game, so it can't be added anyway.".into());
    }
    if game.native_vr && !force {
        return Err("Already runs in VR on its own: start it in VR from Steam or its launcher. Parallax stays out of anything that already supports VR.".into());
    }
    if game.stripped && !force {
        return Err("This game was built with Unity's code trimming, which removes engine code the mod loader needs. It isn't supported yet.".into());
    }
    if game.x86 {
        return Err("32-bit game: no OpenXR runtime supports 32-bit apps.".into());
    }
    if game.other_loader == Some("MelonLoader") {
        return Err("MelonLoader is installed; remove it first, the two loaders conflict.".into());
    }
    if receipt_path(game_dir).exists() {
        uninstall(game_dir)?;
    }

    let payload = payload(game.il2cpp)?;
    if !payload.is_dir() {
        return Err(format!("{} is missing next to Parallax.exe.", payload.display()).into());
    }
    let loader_preexisting = game_dir.join("BepInEx").is_dir();
    let mut sources = Vec::new();
    walk(&payload, &payload, &mut sources)?;
    sources.retain(|rel| !loader_preexisting || rel.to_lowercase().starts_with(&PLUGIN_DIR.to_lowercase()));
    let mut created_dirs: Vec<String> = Vec::new();
    for top in sources.iter().filter_map(|rel| rel.split_once('\\').map(|(t, _)| t.to_string())) {
        if !created_dirs.contains(&top) && !games::native(game_dir, &top).exists() {
            created_dirs.push(top);
        }
    }
    // Backups and the receipt reach the disk before any game file changes, so Remove VR can undo an install that a
    // crash or power cut stopped halfway.
    let mut files = Vec::new();
    for rel in &sources {
        let target = games::native(game_dir, rel);
        let backed_up = target.exists();
        if backed_up {
            let backup = games::native(&game_dir.join(BACKUP_DIR), rel);
            fs::create_dir_all(backup.parent().unwrap())?;
            fs::copy(&target, &backup)?;
            fs::OpenOptions::new().write(true).open(&backup)?.sync_all()?;
        }
        let sha256 = Sha256::digest(fs::read(games::native(&payload, rel))?).iter().map(|b| format!("{b:02x}")).collect();
        files.push(ReceiptFile { path: rel.clone(), sha256, backed_up });
    }
    let receipt = Receipt { version: env!("CARGO_PKG_VERSION").into(), loader_preexisting, created_dirs, files, forced: force };
    write_durable(&receipt_path(game_dir), &serde_json::to_vec(&receipt)?)?;
    for rel in &sources {
        let target = games::native(game_dir, rel);
        fs::create_dir_all(target.parent().unwrap())?;
        fs::copy(games::native(&payload, rel), &target)?;
    }
    Ok(())
}

/// The script Godot 4.0 to 4.2 games load for VR (godot/parallax_vr.gd), built into the app.
const GODOT_SCRIPT: &str = include_str!("../../godot/parallax_vr.gd");

/// The Godot adapter on the shared VR core (dist\godot: the GDExtension, the OpenXR loader, the adapter script and
/// its .gdextension), copied into a ParallaxVR folder beside the game's exe.
const GODOT_CORE: [&str; 4] = ["parallax_godot.dll", "openxr_loader.dll", "parallax.gd", "parallax.gdextension"];
const GODOT_DIR: &str = "ParallaxVR";

fn godot_payload() -> Result<PathBuf, Box<dyn Error>> {
    Ok(std::env::current_exe()?.with_file_name("godot"))
}

/// Godot 4: Parallax's VR core for 4.3 and later, else the engine's own OpenXR. Either way override.cfg beside the exe
/// (which Godot reads over the game's project settings) loads our script as an autoload. An override.cfg the game
/// already has is kept: backed up, and ours appended after it so its settings stay unless VR needs them changed.
fn install_godot(game: &games::Game) -> Result<(), Box<dyn Error>> {
    let game_dir = game.dir.as_path();
    // The game's settings stay through an update (a reinstall), as a Unity game's do.
    let settings = if game.godot_core() { fs::read(game.config_file()).ok() } else { None };
    if receipt_path(game_dir).exists() {
        uninstall(game_dir)?;
    }
    if game.godot_core() {
        install_godot_core(game)?;
        if let Some(settings) = settings {
            fs::write(game.config_file(), settings)?;
        }
        return Ok(());
    }
    // A game exported with a custom engine build may have left OpenXR out; it would just start flat.
    let needle = b"xrCreateInstance";
    if !fs::read(&game.exe)?.windows(needle.len()).any(|w| w == needle) {
        return Err("This game's Godot build has no OpenXR, so it can't open VR itself. Play it on the Theater screen instead.".into());
    }
    let exe_dir = game.exe.parent().ok_or("no game folder")?;
    let rel = |name: &str| exe_dir.join(name).strip_prefix(game_dir).map(|p| p.to_string_lossy().replace('/', "\\")).unwrap_or_else(|_| name.into());
    let script = exe_dir.join("parallax_vr.gd");
    let existing = fs::read_to_string(exe_dir.join("override.cfg")).unwrap_or_default();
    // Vulkan, Godot's main renderer: with OpenXR on, Godot otherwise switched one game from Vulkan to Direct3D 12.
    let ours = format!(
        "\n[xr]\n\nopenxr/enabled=true\nshaders/enabled=true\n\n[rendering]\n\nrendering_device/driver.windows=\"vulkan\"\n\n\
         [autoload]\n\nParallaxVR=\"*{}\"\n",
        script.to_string_lossy().replace('\\', "/")
    );
    let files: Vec<(String, Vec<u8>)> =
        vec![(rel("override.cfg"), format!("{existing}{ours}").into_bytes()), (rel("parallax_vr.gd"), GODOT_SCRIPT.as_bytes().to_vec())];
    install_files(game_dir, &files, Vec::new())
}

/// Godot 4.3+: the VR core runs inside the game on Direct3D 12 (Godot's own OpenXR stays off), loaded by the adapter
/// script as a GDExtension from the ParallaxVR folder; the folder goes whole on Remove VR, with the settings in it.
fn install_godot_core(game: &games::Game) -> Result<(), Box<dyn Error>> {
    let game_dir = game.dir.as_path();
    // The VR core needs Godot's Direct3D 12 renderer; a custom engine build may have left it out.
    let needle = b"D3D12CreateDevice";
    if !fs::read(&game.exe)?.windows(needle.len()).any(|w| w == needle) {
        return Err("This game's Godot build has no Direct3D 12, which Parallax needs inside Godot games. Play it on the Theater screen instead.".into());
    }
    let exe_dir = game.exe.parent().ok_or("no game folder")?;
    let rel = |name: &str| {
        let p = exe_dir.join(name);
        p.strip_prefix(game_dir).map(|p| p.to_string_lossy().replace('/', "\\")).unwrap_or_else(|_| name.into())
    };
    let folder = rel(GODOT_DIR);
    let created = if exe_dir.join(GODOT_DIR).exists() { Vec::new() } else { vec![folder.clone()] };
    let payload = godot_payload()?;
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    for name in GODOT_CORE {
        files.push((format!("{folder}\\{name}"), fs::read(payload.join(name)).map_err(|e| format!("{name} missing beside Parallax.exe: {e}"))?));
    }
    let script = exe_dir.join(GODOT_DIR).join("parallax.gd").to_string_lossy().replace('\\', "/");
    let existing = fs::read_to_string(exe_dir.join("override.cfg")).unwrap_or_default();
    let ours = format!("\n[rendering]\n\nrendering_device/driver.windows=\"d3d12\"\n\n[autoload]\n\nParallaxVR=\"*{script}\"\n");
    files.push((rel("override.cfg"), format!("{existing}{ours}").into_bytes()));
    install_files(game_dir, &files, created)
}

/// The Unreal adapter (dist\unreal): the VR core named dwmapi.dll and its OpenXR loader.
const UNREAL_CORE: [&str; 2] = ["dwmapi.dll", "openxr_loader.dll"];

fn unreal_dir(game: &games::Game) -> Result<&Path, Box<dyn Error>> {
    Ok(game.unreal_exe.as_deref().and_then(Path::parent).ok_or("This Unreal game's Shipping exe wasn't found.")?)
}

/// Unreal: the VR core goes beside the Shipping exe as dwmapi.dll, which the game loads from there before System32 (all
/// of dwmapi's own functions forward to the real one). Its settings and log live in a ParallaxVR folder beside it, which
/// goes whole on Remove VR; the settings stay through an update (a reinstall).
fn install_unreal(game: &games::Game) -> Result<(), Box<dyn Error>> {
    let game_dir = game.dir.as_path();
    let settings = fs::read(game.config_file()).ok();
    if receipt_path(game_dir).exists() {
        uninstall(game_dir)?;
    }
    let exe_dir = unreal_dir(game)?;
    if exe_dir.join("dwmapi.dll").exists() {
        return Err("Another mod loader (a dwmapi.dll, such as UE4SS) is in this game. Remove it first, then add VR.".into());
    }
    let rel = |name: &str| {
        let p = exe_dir.join(name);
        p.strip_prefix(game_dir).map(|p| p.to_string_lossy().replace('/', "\\")).unwrap_or_else(|_| name.into())
    };
    let payload = std::env::current_exe()?.with_file_name("unreal");
    let mut files = Vec::new();
    for name in UNREAL_CORE {
        files.push((rel(name), fs::read(payload.join(name)).map_err(|e| format!("{name} missing beside Parallax.exe: {e}"))?));
    }
    let created = if exe_dir.join(GODOT_DIR).exists() { Vec::new() } else { vec![rel(GODOT_DIR)] };
    install_files(game_dir, &files, created)?;
    if let Some(settings) = settings {
        let config = game.config_file();
        fs::create_dir_all(config.parent().unwrap())?;
        fs::write(config, settings)?;
    }
    Ok(())
}

/// Writes `files` (paths relative to the game folder) with a receipt and backups first, as install() does. Folders in
/// `created` were made for us and go whole on uninstall.
fn install_files(game_dir: &Path, files: &[(String, Vec<u8>)], created: Vec<String>) -> Result<(), Box<dyn Error>> {
    let mut entries = Vec::new();
    for (rel, data) in files {
        let target = games::native(game_dir, rel);
        let backed_up = target.exists();
        if backed_up {
            let backup = games::native(&game_dir.join(BACKUP_DIR), rel);
            fs::create_dir_all(backup.parent().unwrap())?;
            fs::copy(&target, &backup)?;
            fs::OpenOptions::new().write(true).open(&backup)?.sync_all()?;
        }
        let sha256 = Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect();
        entries.push(ReceiptFile { path: rel.clone(), sha256, backed_up });
    }
    let receipt = Receipt { version: env!("CARGO_PKG_VERSION").into(), loader_preexisting: false, created_dirs: created, files: entries, forced: false };
    write_durable(&receipt_path(game_dir), &serde_json::to_vec(&receipt)?)?;
    for (rel, data) in files {
        let target = games::native(game_dir, rel);
        fs::create_dir_all(target.parent().unwrap())?;
        fs::write(target, data)?;
    }
    Ok(())
}

/// Written beside and renamed over, flushed first: a crash leaves the old file or the new one, never a torn or
/// zero-filled one.
fn write_durable(path: &Path, data: &[u8]) -> std::io::Result<()> {
    let tmp = path.with_extension("json.tmp");
    let mut f = fs::File::create(&tmp)?;
    std::io::Write::write_all(&mut f, data)?;
    f.sync_all()?;
    drop(f);
    fs::rename(&tmp, path)
}

/// Brings an installed game's plugin files up to this build, in place: the loader, its caches (IL2CPP interop takes
/// minutes to regenerate) and the settings stay. Returns whether anything changed.
pub fn update(game_dir: &Path) -> Result<bool, Box<dyn Error>> {
    update_checked(&checked(game_dir, forced(game_dir))?)
}

/// VR was added to this game anyway (install_anyway).
pub fn forced(game_dir: &Path) -> bool {
    fs::read(receipt_path(game_dir)).ok().and_then(|b| serde_json::from_slice::<Receipt>(&b).ok()).is_some_and(|r| r.forced)
}

/// `update` for a game the caller has just checked (the library scan checks every game).
pub fn update_checked(game: &games::Game) -> Result<bool, Box<dyn Error>> {
    if game.blocker.is_some() && !(game.caution().is_some() && forced(&game.dir)) {
        uninstall(&game.dir)?;
        return Err("blocked".into());
    }
    if game.unreal_vr() {
        let (dir, payload) = (unreal_dir(game)?, std::env::current_exe()?.with_file_name("unreal"));
        if UNREAL_CORE.iter().all(|n| fs::read(dir.join(n)).ok().is_some_and(|a| fs::read(payload.join(n)).is_ok_and(|b| a == b))) {
            return Ok(false);
        }
        install_unreal(game)?;
        return Ok(true);
    }
    if game.godot_vr() {
        let current = if game.godot_core() {
            let (dir, payload) = (game.exe.with_file_name(GODOT_DIR), godot_payload()?);
            GODOT_CORE.iter().all(|n| fs::read(dir.join(n)).ok().is_some_and(|a| fs::read(payload.join(n)).is_ok_and(|b| a == b)))
        } else {
            fs::read(game.exe.with_file_name("parallax_vr.gd")).unwrap_or_default() == GODOT_SCRIPT.as_bytes()
        };
        if current {
            return Ok(false);
        }
        install_godot(game)?;
        return Ok(true);
    }
    let game_dir = game.dir.as_path();
    let path = receipt_path(game_dir);
    let mut receipt: Receipt = serde_json::from_slice(&fs::read(&path)?)?;
    let payload = payload(game.il2cpp)?;
    let mut sources = Vec::new();
    walk(&payload, &payload, &mut sources)?;
    let mut changed = false;
    for rel in sources.into_iter().filter(|rel| rel.to_lowercase().starts_with(&PLUGIN_DIR.to_lowercase())) {
        let data = fs::read(games::native(&payload, &rel))?;
        let target = games::native(game_dir, &rel);
        if fs::read(&target).is_ok_and(|current| current == data) {
            continue;
        }
        fs::create_dir_all(target.parent().unwrap())?;
        fs::write(&target, &data)?;
        let sha256: String = Sha256::digest(&data).iter().map(|b| format!("{b:02x}")).collect();
        match receipt.files.iter_mut().find(|f| f.path.eq_ignore_ascii_case(&rel)) {
            Some(f) => f.sha256 = sha256,
            None => receipt.files.push(ReceiptFile { path: rel, sha256, backed_up: false }),
        }
        changed = true;
    }
    if changed {
        receipt.version = env!("CARGO_PKG_VERSION").into();
        write_durable(&path, &serde_json::to_vec(&receipt)?)?;
    }
    Ok(changed)
}

/// The game, unless it has anti-cheat or is online-only: then Parallax is taken out of it (a game update may have
/// added anti-cheat since the install) and it is refused. Install, every update and every Play pass through here.
// Anti-cheat is refused whatever the player chose; a multiplayer-only game only when it was not added anyway.
fn checked(game_dir: &Path, force: bool) -> Result<games::Game, Box<dyn Error>> {
    let game = games::inspect(game_dir).ok_or("No game Parallax can add VR to was found in this folder.")?;
    let Some(b) = game.blocker else { return Ok(game) };
    if force && game.caution().is_some() {
        return Ok(game);
    }
    let removed = receipt_path(game_dir).exists() && uninstall(game_dir).is_ok();
    let why = if b == "Multiplayer only" { "No single-player mode: online play risks a ban".to_string() } else { format!("{b} found: injecting risks a ban") };
    Err(format!("{why}, so this game is refused{}.", if removed { " and Parallax was removed from it" } else { "" }).into())
}

pub fn uninstall(game_dir: &Path) -> Result<(), Box<dyn Error>> {
    let bytes = match fs::read(receipt_path(game_dir)) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err("VR isn't added to this game, so there is nothing to remove.".into()),
        r => r?,
    };
    let receipt: Receipt = serde_json::from_slice(&bytes)?;
    for dir in &receipt.created_dirs {
        remove_dir(&games::native(game_dir, dir))?;
    }
    if receipt.loader_preexisting {
        remove_dir(&games::native(game_dir, PLUGIN_DIR))?;
        let _ = fs::remove_file(games::native(game_dir, CONFIG));
    }
    for entry in &receipt.files {
        let target = games::native(game_dir, &entry.path);
        if entry.backed_up {
            fs::create_dir_all(target.parent().unwrap())?;
            patient(|| fs::copy(games::native(&game_dir.join(BACKUP_DIR), &entry.path), &target))?;
        } else if target.exists() {
            patient(|| fs::remove_file(&target))?;
        }
    }
    remove_dir(&game_dir.join(BACKUP_DIR))?;
    fs::remove_file(receipt_path(game_dir))?;
    Ok(())
}

fn remove_dir(dir: &Path) -> std::io::Result<()> {
    if dir.exists() { patient(|| fs::remove_dir_all(dir)) } else { Ok(()) }
}

/// Retries a file operation for up to three seconds: a game that just closed can hold its files a moment longer.
fn patient<T>(mut op: impl FnMut() -> std::io::Result<T>) -> std::io::Result<T> {
    for _ in 0..15 {
        match op() {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => std::thread::sleep(std::time::Duration::from_millis(200)),
            done => return done,
        }
    }
    op()
}
