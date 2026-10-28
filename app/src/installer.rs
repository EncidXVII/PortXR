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
    let game = checked(game_dir)?;
    if game.stripped {
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
        if !created_dirs.contains(&top) && !game_dir.join(&top).exists() {
            created_dirs.push(top);
        }
    }
    let mut files = Vec::new();
    for rel in sources {
        let target = game_dir.join(&rel);
        let backed_up = target.exists();
        if backed_up {
            let backup = game_dir.join(BACKUP_DIR).join(&rel);
            fs::create_dir_all(backup.parent().unwrap())?;
            fs::copy(&target, backup)?;
        }
        fs::create_dir_all(target.parent().unwrap())?;
        fs::copy(payload.join(&rel), &target)?;
        let sha256 = Sha256::digest(fs::read(&target)?).iter().map(|b| format!("{b:02x}")).collect();
        files.push(ReceiptFile { path: rel, sha256, backed_up });
    }
    let receipt = Receipt { version: env!("CARGO_PKG_VERSION").into(), loader_preexisting, created_dirs, files };
    fs::write(receipt_path(game_dir), serde_json::to_vec(&receipt)?)?;
    Ok(())
}

/// Brings an installed game's plugin files up to this build, in place: the loader, its caches (IL2CPP interop takes
/// minutes to regenerate) and the settings stay. Returns whether anything changed.
pub fn update(game_dir: &Path) -> Result<bool, Box<dyn Error>> {
    let path = receipt_path(game_dir);
    let mut receipt: Receipt = serde_json::from_slice(&fs::read(&path)?)?;
    let game = checked(game_dir)?;
    let payload = payload(game.il2cpp)?;
    let mut sources = Vec::new();
    walk(&payload, &payload, &mut sources)?;
    let mut changed = false;
    for rel in sources.into_iter().filter(|rel| rel.to_lowercase().starts_with(&PLUGIN_DIR.to_lowercase())) {
        let data = fs::read(payload.join(&rel))?;
        let target = game_dir.join(&rel);
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
        fs::write(&path, serde_json::to_vec(&receipt)?)?;
    }
    Ok(changed)
}

/// The game, unless it has anti-cheat or is online-only: then Parallax is taken out of it (a game update may have
/// added anti-cheat since the install) and it is refused. Install, every update and every Play pass through here.
fn checked(game_dir: &Path) -> Result<games::Game, Box<dyn Error>> {
    let game = games::inspect(game_dir).ok_or("No Unity game found in this folder.")?;
    let Some(b) = game.blocker else { return Ok(game) };
    let removed = receipt_path(game_dir).exists() && uninstall(game_dir).is_ok();
    let why = if b == "Multiplayer only" { "No single-player mode: online play risks a ban".to_string() } else { format!("{b} found: injecting risks a ban") };
    Err(format!("{why}, so this game is refused{}.", if removed { " and Parallax was removed from it" } else { "" }).into())
}

pub fn uninstall(game_dir: &Path) -> Result<(), Box<dyn Error>> {
    let receipt: Receipt = serde_json::from_slice(&fs::read(receipt_path(game_dir))?)?;
    for dir in &receipt.created_dirs {
        remove_dir(&game_dir.join(dir))?;
    }
    if receipt.loader_preexisting {
        remove_dir(&game_dir.join(PLUGIN_DIR))?;
        let _ = fs::remove_file(game_dir.join(CONFIG));
    }
    for entry in &receipt.files {
        let target = game_dir.join(&entry.path);
        if entry.backed_up {
            fs::create_dir_all(target.parent().unwrap())?;
            fs::copy(game_dir.join(BACKUP_DIR).join(&entry.path), &target)?;
        } else if target.exists() {
            fs::remove_file(&target)?;
        }
    }
    remove_dir(&game_dir.join(BACKUP_DIR))?;
    fs::remove_file(receipt_path(game_dir))?;
    Ok(())
}

fn remove_dir(dir: &Path) -> std::io::Result<()> {
    if dir.exists() { fs::remove_dir_all(dir) } else { Ok(()) }
}
