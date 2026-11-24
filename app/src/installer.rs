// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

use std::error::Error;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use crate::games;

const BACKUP_DIR: &str = ".vrbackup";
const PLUGIN_DIR: &str = r"BepInEx\plugins\PortXR";
pub const CONFIG: &str = r"BepInEx\config\portxr.core.cfg";
/// The plugin's settings file and folder under the name it had before (Parallax).
const OLD_CONFIG: &str = r"BepInEx\config\parallax.core.cfg";
const OLD_PLUGIN_DIR: &str = r"BepInEx\plugins\Parallax";

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
        return Err("PortXR can't run in this game, so it can't be added anyway.".into());
    }
    if game.native_vr && !force {
        return Err("Already runs in VR on its own: start it in VR from Steam or its launcher. PortXR stays out of anything that already supports VR.".into());
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
        return Err(format!("{} is missing next to PortXR.exe.", payload.display()).into());
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
        let backed_up = back_up(game_dir, rel)?;
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

/// The script Godot 4.0 to 4.2 games load for VR (godot/portxr_vr.gd), built into the app.
const GODOT_SCRIPT: &str = include_str!("../../godot/portxr_vr.gd");

/// The Godot adapter on the shared VR core (dist\godot: the GDExtension, the OpenXR loader, the adapter script and
/// its .gdextension), copied into a PortXR folder beside the game's exe.
const GODOT_CORE: [&str; 4] = ["portxr_godot.dll", "openxr_loader.dll", "portxr.gd", "portxr.gdextension"];
const GODOT_DIR: &str = "PortXR";

fn godot_payload() -> Result<PathBuf, Box<dyn Error>> {
    Ok(std::env::current_exe()?.with_file_name("godot"))
}

/// Godot 4: PortXR's VR core for 4.3 and later, else the engine's own OpenXR. Either way override.cfg beside the exe
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
    let rel = |name: &str| beside_exe(game_dir, exe_dir, name);
    let script = exe_dir.join("portxr_vr.gd");
    let existing = fs::read_to_string(exe_dir.join("override.cfg")).unwrap_or_default();
    // Vulkan, Godot's main renderer: with OpenXR on, Godot otherwise switched one game from Vulkan to Direct3D 12.
    let ours = format!(
        "\n[xr]\n\nopenxr/enabled=true\nshaders/enabled=true\n\n[rendering]\n\nrendering_device/driver.windows=\"vulkan\"\n\n\
         [autoload]\n\nPortXR=\"*{}\"\n",
        script.to_string_lossy().replace('\\', "/")
    );
    let files: Vec<(String, Vec<u8>)> =
        vec![(rel("override.cfg"), format!("{existing}{ours}").into_bytes()), (rel("portxr_vr.gd"), GODOT_SCRIPT.as_bytes().to_vec())];
    install_files(game_dir, &files, Vec::new())
}

/// Godot 4.3+: the VR core runs inside the game on Direct3D 12 (Godot's own OpenXR stays off), loaded by the adapter
/// script as a GDExtension from the PortXR folder; the folder goes whole on Remove VR, with the settings in it.
fn install_godot_core(game: &games::Game) -> Result<(), Box<dyn Error>> {
    let game_dir = game.dir.as_path();
    // The VR core needs Godot's Direct3D 12 renderer; a custom engine build may have left it out.
    let needle = b"D3D12CreateDevice";
    if !fs::read(&game.exe)?.windows(needle.len()).any(|w| w == needle) {
        return Err("This game's Godot build has no Direct3D 12, which PortXR needs inside Godot games. Play it on the Theater screen instead.".into());
    }
    let exe_dir = game.exe.parent().ok_or("no game folder")?;
    let rel = |name: &str| beside_exe(game_dir, exe_dir, name);
    let folder = rel(GODOT_DIR);
    let created = if exe_dir.join(GODOT_DIR).exists() { Vec::new() } else { vec![folder.clone()] };
    let payload = godot_payload()?;
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    for name in GODOT_CORE {
        files.push((format!("{folder}\\{name}"), fs::read(payload.join(name)).map_err(|e| format!("{name} missing beside PortXR.exe: {e}"))?));
    }
    let script = exe_dir.join(GODOT_DIR).join("portxr.gd").to_string_lossy().replace('\\', "/");
    let existing = fs::read_to_string(exe_dir.join("override.cfg")).unwrap_or_default();
    let ours = format!("\n[rendering]\n\nrendering_device/driver.windows=\"d3d12\"\n\n[autoload]\n\nPortXR=\"*{script}\"\n");
    files.push((rel("override.cfg"), format!("{existing}{ours}").into_bytes()));
    install_files(game_dir, &files, created)
}

/// The Unreal adapter (dist\unreal): the VR core named dwmapi.dll and its OpenXR loader.
const UNREAL_CORE: [&str; 2] = ["dwmapi.dll", "openxr_loader.dll"];

fn unreal_dir(game: &games::Game) -> Result<&Path, Box<dyn Error>> {
    Ok(game.unreal_exe.as_deref().and_then(Path::parent).ok_or("This Unreal game's Shipping exe wasn't found.")?)
}

/// Unreal: the VR core goes beside the Shipping exe as dwmapi.dll, which the game loads from there before System32 (all
/// of dwmapi's own functions forward to the real one). Its settings and log live in a PortXR folder beside it, which
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
    let rel = |name: &str| beside_exe(game_dir, exe_dir, name);
    let payload = std::env::current_exe()?.with_file_name("unreal");
    let mut files = Vec::new();
    for name in UNREAL_CORE {
        files.push((rel(name), fs::read(payload.join(name)).map_err(|e| format!("{name} missing beside PortXR.exe: {e}"))?));
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
        let backed_up = back_up(game_dir, rel)?;
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

/// Copies the game's own file at `rel`, if there is one, into the backup folder and flushes it. Returns whether
/// there was one.
fn back_up(game_dir: &Path, rel: &str) -> std::io::Result<bool> {
    let target = games::native(game_dir, rel);
    if !target.exists() {
        return Ok(false);
    }
    let backup = games::native(&game_dir.join(BACKUP_DIR), rel);
    fs::create_dir_all(backup.parent().unwrap())?;
    fs::copy(&target, &backup)?;
    fs::OpenOptions::new().write(true).open(&backup)?.sync_all()?;
    Ok(true)
}

/// `name` beside the game's exe, as the receipt stores it: relative to the game folder, backslash-separated.
fn beside_exe(game_dir: &Path, exe_dir: &Path, name: &str) -> String {
    exe_dir.join(name).strip_prefix(game_dir).map(|p| p.to_string_lossy().replace('/', "\\")).unwrap_or_else(|_| name.into())
}

/// Every one of `names` in `dir` is byte for byte the payload's.
fn same_files(dir: &Path, payload: &Path, names: &[&str]) -> bool {
    names.iter().all(|n| fs::read(dir.join(n)).ok().is_some_and(|a| fs::read(payload.join(n)).is_ok_and(|b| a == b)))
}

/// A receipt path that stays inside the game folder. The receipt lies in that folder, where anything can write it,
/// and Remove VR deletes what it lists: a path that is absolute or climbs out with `..` is never followed.
fn inside(rel: &str) -> bool {
    use std::path::Component;
    !rel.is_empty() && !rel.contains(':') && Path::new(&rel.replace('\\', "/")).components().all(|c| matches!(c, Component::Normal(_)))
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
        if same_files(dir, &payload, &UNREAL_CORE) {
            return Ok(false);
        }
        install_unreal(game)?;
        return Ok(true);
    }
    if game.godot_vr() {
        let current = if game.godot_core() {
            same_files(&game.exe.with_file_name(GODOT_DIR), &godot_payload()?, &GODOT_CORE)
        } else {
            fs::read(game.exe.with_file_name("portxr_vr.gd")).unwrap_or_default() == GODOT_SCRIPT.as_bytes()
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
    let mut changed = drop_stale(game_dir, &mut receipt, &sources);
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

/// Plugin files an earlier build installed that this one no longer ships (the plugin's folder under the name it had
/// before): taken out of the game and the receipt. Left in, the old plugin loads beside the new one and takes the
/// headset first, so the game keeps running the old build. Its settings were the ones in use: they become the new
/// plugin's. A file a running game still holds stays for the next update. Returns whether anything was removed.
fn drop_stale(game_dir: &Path, receipt: &mut Receipt, shipped: &[String]) -> bool {
    let shipped: Vec<String> = shipped.iter().map(|s| s.to_lowercase()).collect();
    let mut removed = false;
    receipt.files.retain(|f| {
        if !stale(f, &shipped) {
            return true;
        }
        let target = games::native(game_dir, &f.path);
        if target.exists() && fs::remove_file(&target).is_err() {
            return true;
        }
        if let Some(dir) = target.parent() {
            let _ = fs::remove_dir(dir); // once it is empty
        }
        removed = true;
        false
    });
    let old = games::native(game_dir, OLD_CONFIG);
    if removed && old.exists() {
        let _ = fs::rename(&old, games::native(game_dir, CONFIG));
    }
    clear_old_dir(game_dir);
    removed
}

/// The old plugin's folder once none of its DLLs is left: what the plugin itself wrote there while it ran (its
/// played list) moves to the new plugin's folder unless that has its own, and the folder goes. With a DLL still in
/// it (a running game holds it, or it is not ours) it stays as it is.
fn clear_old_dir(game_dir: &Path) {
    let (old, new) = (games::native(game_dir, OLD_PLUGIN_DIR), games::native(game_dir, PLUGIN_DIR));
    let Ok(entries) = fs::read_dir(&old) else { return };
    let files: Vec<PathBuf> = entries.flatten().map(|e| e.path()).collect();
    if files.iter().any(|f| f.is_dir() || f.extension().is_some_and(|x| x.eq_ignore_ascii_case("dll"))) {
        return;
    }
    for file in files {
        let target = new.join(file.file_name().unwrap_or_default());
        if target.exists() || fs::rename(&file, &target).is_err() {
            let _ = fs::remove_file(&file);
        }
    }
    let _ = fs::remove_dir(&old);
}

/// A plugin file of ours (never one that was there before us) that this build no longer ships; `shipped` in lowercase.
fn stale(f: &ReceiptFile, shipped: &[String]) -> bool {
    let rel = f.path.to_lowercase();
    !f.backed_up && rel.starts_with(r"bepinex\plugins\") && !shipped.contains(&rel) && inside(&f.path)
}

/// Whether a game with VR added holds other plugin files than this build ships: an older build's, or the plugin under
/// its old name. Every library scan and every Play in VR bring a game up to this build (update_checked), so this is
/// true only where they could not: the game was running, or something else held its files.
pub fn outdated(game: &games::Game) -> bool {
    if !game.installed() {
        return false;
    }
    if game.unreal_vr() {
        let payload = std::env::current_exe().map(|exe| exe.with_file_name("unreal"));
        return unreal_dir(game).ok().zip(payload.ok()).is_some_and(|(dir, payload)| !same_files(dir, &payload, &UNREAL_CORE));
    }
    if game.godot_vr() {
        return if game.godot_core() {
            godot_payload().is_ok_and(|payload| !same_files(&game.exe.with_file_name(GODOT_DIR), &payload, &GODOT_CORE))
        } else {
            fs::read(game.exe.with_file_name("portxr_vr.gd")).unwrap_or_default() != GODOT_SCRIPT.as_bytes()
        };
    }
    let (Ok(payload), mut sources) = (payload(game.il2cpp), Vec::new()) else { return false };
    if walk(&payload, &payload, &mut sources).is_err() {
        return false;
    }
    let plugin = PLUGIN_DIR.to_lowercase();
    let differs = sources.iter().filter(|rel| rel.to_lowercase().starts_with(&plugin)).any(|rel| {
        fs::read(games::native(&payload, rel)).is_ok_and(|ours| !fs::read(games::native(&game.dir, rel)).is_ok_and(|theirs| theirs == ours))
    });
    let shipped: Vec<String> = sources.iter().map(|s| s.to_lowercase()).collect();
    let receipt = fs::read(receipt_path(&game.dir)).ok().and_then(|b| serde_json::from_slice::<Receipt>(&b).ok());
    differs || receipt.is_some_and(|r| r.files.iter().any(|f| stale(f, &shipped) && games::native(&game.dir, &f.path).exists()))
}

/// `outdated`, read from the disk at most every two seconds per game: a game's page asks on every frame it draws.
pub fn outdated_now(game: &games::Game) -> bool {
    use std::collections::HashMap;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};
    static ASKED: Mutex<Option<HashMap<PathBuf, (Instant, bool)>>> = Mutex::new(None);
    let mut asked = ASKED.lock().unwrap_or_else(|e| e.into_inner());
    let map = asked.get_or_insert_with(HashMap::new);
    if let Some((at, answer)) = map.get(&game.dir)
        && at.elapsed() < Duration::from_secs(2)
    {
        return *answer;
    }
    let answer = outdated(game);
    map.insert(game.dir.clone(), (Instant::now(), answer));
    answer
}

/// The game, unless it has anti-cheat or is online-only: then PortXR is taken out of it (a game update may have
/// added anti-cheat since the install) and it is refused. Install, every update and every Play pass through here.
// Anti-cheat is refused whatever the player chose; a multiplayer-only game only when it was not added anyway.
fn checked(game_dir: &Path, force: bool) -> Result<games::Game, Box<dyn Error>> {
    let game = games::inspect(game_dir).ok_or("No game PortXR can add VR to was found in this folder.")?;
    let Some(b) = game.blocker else { return Ok(game) };
    if force && game.caution().is_some() {
        return Ok(game);
    }
    let removed = receipt_path(game_dir).exists() && uninstall(game_dir).is_ok();
    let why = if b == "Multiplayer only" { "No single-player mode: online play risks a ban".to_string() } else { format!("{b} found: injecting risks a ban") };
    Err(format!("{why}, so this game is refused{}.", if removed { " and PortXR was removed from it" } else { "" }).into())
}

pub fn uninstall(game_dir: &Path) -> Result<(), Box<dyn Error>> {
    let bytes = match fs::read(receipt_path(game_dir)) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err("VR isn't added to this game, so there is nothing to remove.".into()),
        r => r?,
    };
    let receipt: Receipt = serde_json::from_slice(&bytes)?;
    if !receipt.created_dirs.iter().chain(receipt.files.iter().map(|f| &f.path)).all(|rel| inside(rel)) {
        return Err("This game's install record lists files outside the game folder, so nothing was removed. Verify the game's files in its store instead.".into());
    }
    // Linux: the Wine prefix's setting to load PortXR's loader goes first; while the game runs, nothing changes.
    crate::proton::disable(game_dir).map_err(|why| match why {
        crate::proton::Why::Io(e) => format!("Couldn't undo the game's Wine prefix setting: {e}"),
        // Undoing never needs the prefix; a running game is what stops it.
        _ => "The game is running. Close it, then remove VR again.".to_string(),
    })?;
    for dir in &receipt.created_dirs {
        remove_dir(&games::native(game_dir, dir))?;
    }
    if receipt.loader_preexisting {
        remove_dir(&games::native(game_dir, PLUGIN_DIR))?;
        let _ = fs::remove_file(games::native(game_dir, CONFIG));
        let _ = fs::remove_file(games::native(game_dir, OLD_CONFIG));
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

/// `PortXR.exe --uninstall` (Uninstall PortXR.cmd): VR out of every game it was added to, then PortXR's own settings
/// folder. While one game still has VR in it nothing else is deleted, so a second run can finish the job.
pub fn uninstall_all() -> Result<String, Box<dyn Error>> {
    let with_vr: Vec<_> = games::scan().into_iter().filter(|g| g.installed()).collect();
    let failed: Vec<String> = with_vr.iter().filter_map(|g| uninstall(&g.dir).err().map(|e| format!("{}: {e}", g.name))).collect();
    if !failed.is_empty() {
        return Err(format!("VR is still in these games, so PortXR's settings were kept. Close the games and run this again.\n{}", failed.join("\n")).into());
    }
    // Linux: any Wine prefix setting left behind (a game whose folder has gone, or no longer listed).
    if crate::proton::disable_all().is_err() {
        return Err("A game's Wine prefix still has PortXR's setting, so PortXR's settings were kept. Close your games and run this again.".into());
    }
    let home = dirs::config_dir().ok_or("no settings folder on this system")?.join("PortXR");
    let scripts = clear_settings(&home)?;
    let kept = if scripts { format!(" Your game scripts stay in {}.", home.join("scripts").display()) } else { String::new() };
    Ok(format!("VR removed from {} game(s) and PortXR's settings deleted.{kept} Now delete the PortXR folder.", with_vr.len()))
}

/// Empties PortXR's settings folder. Game scripts are the player's own files: a `scripts` folder with anything in it
/// stays (true), and only then does the folder itself.
fn clear_settings(home: &Path) -> std::io::Result<bool> {
    let entries = match fs::read_dir(home) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        r => r?,
    };
    let mut scripts = false;
    for entry in entries {
        let path = entry?.path();
        if path.file_name().is_some_and(|n| n == "scripts") && fs::read_dir(&path).is_ok_and(|mut d| d.next().is_some()) {
            scripts = true;
        } else if path.is_dir() {
            fs::remove_dir_all(&path)?;
        } else {
            fs::remove_file(&path)?;
        }
    }
    if !scripts {
        fs::remove_dir(home)?;
    }
    Ok(scripts)
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

#[cfg(test)]
mod tests {
    use super::*;

    fn game_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("portxr-installer-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("bin")).unwrap();
        dir
    }

    #[test]
    fn uninstalling_portxr_keeps_only_the_players_scripts() {
        let home = game_dir("settings");
        fs::write(home.join("defaults.cfg"), "x").unwrap();
        fs::create_dir_all(home.join("settings")).unwrap();
        fs::create_dir_all(home.join("scripts").join("Game")).unwrap();
        assert!(clear_settings(&home).unwrap(), "scripts kept");
        let left: Vec<_> = fs::read_dir(&home).unwrap().flatten().map(|e| e.file_name()).collect();
        assert_eq!(left, ["scripts"]);
        // No scripts: the whole folder goes; a folder that is not there is fine.
        fs::remove_dir_all(home.join("scripts")).unwrap();
        fs::create_dir(home.join("scripts")).unwrap();
        assert!(!clear_settings(&home).unwrap());
        assert!(!home.exists());
        assert!(!clear_settings(&home).unwrap());
    }

    #[test]
    fn remove_vr_puts_the_folder_back_as_it_was() {
        let dir = game_dir("roundtrip");
        fs::write(dir.join("bin").join("override.cfg"), "theirs").unwrap();
        let files = vec![
            (r"bin\override.cfg".to_string(), b"theirs+ours".to_vec()),
            (r"bin\PortXR\core.dll".to_string(), b"core".to_vec()),
        ];
        install_files(&dir, &files, vec![r"bin\PortXR".to_string()]).unwrap();
        assert_eq!(fs::read(dir.join("bin").join("override.cfg")).unwrap(), b"theirs+ours");
        assert!(dir.join("bin").join("PortXR").join("core.dll").exists());
        // The game writes its own files into our folder while it runs; they go with it.
        fs::write(dir.join("bin").join("PortXR").join("log.txt"), "log").unwrap();
        uninstall(&dir).unwrap();
        let left: Vec<_> = fs::read_dir(&dir).unwrap().flatten().map(|e| e.file_name()).collect();
        assert_eq!(left, ["bin"], "nothing of ours stays");
        let left: Vec<_> = fs::read_dir(dir.join("bin")).unwrap().flatten().map(|e| e.file_name()).collect();
        assert_eq!(left, ["override.cfg"]);
        assert_eq!(fs::read(dir.join("bin").join("override.cfg")).unwrap(), b"theirs");
        assert!(uninstall(&dir).is_err(), "nothing left to remove");
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn an_update_takes_out_the_plugin_under_its_old_name() {
        let dir = game_dir("stale");
        let file = |rel: &str, backed_up| ReceiptFile { path: rel.into(), sha256: String::new(), backed_up };
        let (old, new, other) = (r"BepInEx\plugins\Parallax\Parallax.Core.dll", r"BepInEx\plugins\PortXR\PortXR.Core.dll", r"BepInEx\plugins\Theirs\mod.dll");
        let played = r"BepInEx\plugins\Parallax\played.txt"; // written by the old plugin as it ran: in no receipt
        for rel in [old, new, other, OLD_CONFIG, CONFIG, played] {
            let path = games::native(&dir, rel);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, rel).unwrap();
        }
        // `other` was there before us (backed up): never ours to remove. winhttp.dll is no plugin file.
        let mut receipt = Receipt {
            version: "0".into(),
            loader_preexisting: false,
            created_dirs: vec![],
            files: vec![file(old, false), file(new, false), file(other, true), file("winhttp.dll", false)],
            forced: false,
        };
        let shipped = vec![new.to_uppercase(), "winhttp.dll".to_string()];
        assert!(drop_stale(&dir, &mut receipt, &shipped));
        let left: Vec<&str> = receipt.files.iter().map(|f| f.path.as_str()).collect();
        assert_eq!(left, [new, other, "winhttp.dll"]);
        assert!(!games::native(&dir, old).parent().unwrap().exists(), "the old plugin folder goes");
        assert_eq!(fs::read_to_string(games::native(&dir, r"BepInEx\plugins\PortXR\played.txt")).unwrap(), played, "what the old plugin wrote moves over");
        assert!(games::native(&dir, new).exists() && games::native(&dir, other).exists());
        // The old plugin's settings were the ones in use: they are the new plugin's now.
        assert!(!games::native(&dir, OLD_CONFIG).exists());
        assert_eq!(fs::read_to_string(games::native(&dir, CONFIG)).unwrap(), OLD_CONFIG);
        assert!(!drop_stale(&dir, &mut receipt, &shipped), "nothing left to take out");
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_receipt_pointing_outside_the_game_is_not_followed() {
        for (i, bad) in [r"..\victim", "../victim", r"C:\victim", "/victim", r"bin\..\..\victim", ""].iter().enumerate() {
            assert!(!inside(bad), "{bad}");
            let root = game_dir(&format!("escape{i}"));
            let (dir, victim) = (root.join("game"), root.join("victim"));
            fs::create_dir_all(&dir).unwrap();
            fs::create_dir_all(&victim).unwrap();
            fs::write(victim.join("keep.txt"), "keep").unwrap();
            let receipt = Receipt {
                version: "0".into(),
                loader_preexisting: false,
                created_dirs: vec![r"..\victim".into()],
                files: vec![ReceiptFile { path: (*bad).into(), sha256: String::new(), backed_up: false }],
                forced: false,
            };
            fs::write(receipt_path(&dir), serde_json::to_vec(&receipt).unwrap()).unwrap();
            assert!(uninstall(&dir).is_err());
            assert!(victim.join("keep.txt").exists(), "{bad}");
            let _ = fs::remove_dir_all(root);
        }
        assert!(inside(r"BepInEx\plugins\PortXR\PortXR.Core.dll") && inside("winhttp.dll"));
    }
}
