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

/// The script Godot 4 games load for VR (godot/parallax_vr.gd), built into the app.
const GODOT_SCRIPT: &str = include_str!("../../godot/parallax_vr.gd");

/// Godot 4: the engine's own OpenXR, switched on by override.cfg beside the exe (which Godot reads over the game's
/// project settings), and our script loaded as an autoload from beside it. An override.cfg the game already has is
/// kept: backed up, and ours appended after it so its settings stay unless VR needs them changed.
fn install_godot(game: &games::Game) -> Result<(), Box<dyn Error>> {
    let game_dir = game.dir.as_path();
    if receipt_path(game_dir).exists() {
        uninstall(game_dir)?;
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
    install_files(game_dir, &files)
}

/// Writes `files` (paths relative to the game folder) with a receipt and backups first, as install() does.
fn install_files(game_dir: &Path, files: &[(String, Vec<u8>)]) -> Result<(), Box<dyn Error>> {
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
    let receipt = Receipt { version: env!("CARGO_PKG_VERSION").into(), loader_preexisting: false, created_dirs: Vec::new(), files: entries, forced: false };
    write_durable(&receipt_path(game_dir), &serde_json::to_vec(&receipt)?)?;
    for (rel, data) in files {
        fs::write(games::native(game_dir, rel), data)?;
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
    if game.godot_vr() {
        let installed = fs::read(game.exe.with_file_name("parallax_vr.gd")).unwrap_or_default();
        if installed == GODOT_SCRIPT.as_bytes() {
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
