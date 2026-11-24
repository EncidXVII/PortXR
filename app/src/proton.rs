// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

//! Linux: Wine loads a game folder's own winhttp.dll (Unity's mod loader) and dwmapi.dll (the Unreal core) only when
//! the game's Wine prefix says so. Adding VR sets `"winhttp"="native,builtin"` (or dwmapi) under
//! HKCU\Software\Wine\DllOverrides in the prefix's user.reg, so the game needs no launch options for it.
//!
//! Undoing it: while Wine has not rewritten user.reg since, the saved original goes back byte for byte; after the
//! game ran (Wine rewrites the whole file on exit), only our value is taken out (or the one it replaced put back), and
//! the key too if we added it. Wine keeps the registry in memory while the prefix runs and writes it back on exit, so
//! a running prefix is never edited. A prefix can hold several games (Heroic and Lutris can share one): the value
//! goes when the last game that needs it has its VR removed.

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

const KEY: &str = r"Software\\Wine\\DllOverrides";
const BACKUP: &str = "user.reg.portxr-backup";

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Override {
    pub reg: PathBuf,
    pub dll: String,
    /// The game folders with VR that need it.
    pub games: Vec<PathBuf>,
    /// Hash of user.reg as we left it: unchanged since means the backup can go back as it is.
    pub written: u64,
    /// We added the key (it was not there before), so it goes when our value goes.
    pub created_key: bool,
    /// The value's line before ours, put back when ours goes.
    pub previous: Option<String>,
}

#[derive(Debug, PartialEq)]
pub enum Why {
    /// No user.reg: Proton or Wine has not set the prefix up yet (the game never started).
    NoPrefix,
    /// A Wine process of this prefix runs, and would write over the edit when it exits.
    Running,
    Io(String),
}

fn hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, b| (h ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3))
}

fn line(dll: &str) -> String {
    format!("\"{dll}\"=\"native,builtin\"")
}

/// The value name a .reg line sets, lowercase (Wine's value names ignore case).
fn value_name(l: &str) -> Option<String> {
    Some(l.strip_prefix('"')?.split_once("\"=")?.0.to_lowercase())
}

/// Windows FILETIME (100 ns since 1601) for `#time=` lines.
fn filetime(unix: u64) -> u64 {
    (unix + 11_644_473_600) * 10_000_000
}

/// Where the DllOverrides key's values are: the lines after its header, up to the next key.
fn section(lines: &[&str]) -> Option<(usize, usize)> {
    let header = format!("[{KEY}]");
    let at = lines.iter().position(|l| l.starts_with(&header))?;
    let end = lines[at + 1..].iter().position(|l| l.starts_with('[')).map_or(lines.len(), |e| at + 1 + e);
    Some((at, end))
}

/// Our value is set in this user.reg text.
pub fn is_set(text: &str, dll: &str) -> bool {
    let lines: Vec<&str> = text.split('\n').collect();
    section(&lines).is_some_and(|(at, end)| lines[at + 1..end].iter().any(|l| l.trim_end() == line(dll)))
}

/// user.reg with our value set (and the key added if missing). Returns the text, whether the key was created, and the
/// line it replaced.
pub fn set_value(text: &str, dll: &str, now: u64) -> (String, bool, Option<String>) {
    let mut lines: Vec<String> = text.split('\n').map(String::from).collect();
    let refs: Vec<&str> = lines.iter().map(String::as_str).collect();
    let Some((at, end)) = section(&refs) else {
        let mut out = text.trim_end_matches('\n').to_owned();
        out.push_str(&format!("\n\n[{KEY}] {now}\n#time={:x}\n{}\n", filetime(now), line(dll)));
        return (out, true, None);
    };
    if let Some(i) = (at + 1..end).find(|&i| value_name(&lines[i]).as_deref() == Some(dll)) {
        let previous = std::mem::replace(&mut lines[i], line(dll));
        return (lines.join("\n"), false, Some(previous.trim_end().to_owned()));
    }
    // After the key line and its #time line.
    let insert = if lines.get(at + 1).is_some_and(|l| l.starts_with("#time=")) { at + 2 } else { at + 1 };
    lines.insert(insert, line(dll));
    (lines.join("\n"), false, None)
}

/// user.reg with our value taken out: the line it replaced put back, or the key removed if we created it and nothing
/// else is in it.
pub fn unset_value(text: &str, dll: &str, created_key: bool, previous: Option<&str>) -> String {
    let mut out: Vec<&str> = text.split('\n').collect();
    let Some((at, end)) = section(&out) else { return text.to_owned() };
    let ours = line(dll);
    let Some(i) = (at + 1..end).find(|&i| out[i].trim_end() == ours) else { return text.to_owned() };
    match previous {
        Some(p) => out[i] = p,
        None => {
            out.remove(i);
        }
    }
    if created_key && let Some((at, end)) = section(&out) {
        let body_empty = out[at + 1..end].iter().all(|l| l.trim().is_empty() || l.starts_with('#'));
        if body_empty {
            // The section and the blank line before it, as set_value wrote them.
            let start = if at > 0 && out[at - 1].trim().is_empty() { at - 1 } else { at };
            let keep_blank = end < out.len();
            out.drain(start..if keep_blank { end - 1 } else { end });
        }
    }
    let mut s = out.join("\n");
    if created_key && !s.ends_with('\n') {
        s.push('\n');
    }
    s
}

fn records_path() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("wine-overrides.json")
}

fn load(path: &Path) -> Vec<Override> {
    fs::read(path).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
}

fn save(path: &Path, records: &[Override]) -> Result<(), Why> {
    let io = |e: std::io::Error| Why::Io(e.to_string());
    if records.is_empty() {
        return match fs::remove_file(path) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(io(e)),
            _ => Ok(()),
        };
    }
    fs::create_dir_all(path.parent().unwrap_or(Path::new("."))).map_err(io)?;
    fs::write(path, serde_json::to_vec_pretty(records).unwrap_or_default()).map_err(io)
}

/// Whether a Wine process of this prefix runs. Proton names it `<prefix>/` in WINEPREFIX, and a launcher's prefix
/// can be reached through a link (`pfx` pointing at its own folder), so the paths are compared resolved.
fn running(pfx: &Path) -> bool {
    #[cfg(unix)]
    {
        let want = fs::canonicalize(pfx).unwrap_or_else(|_| pfx.to_path_buf());
        let Ok(procs) = fs::read_dir("/proc") else { return false };
        procs.flatten().any(|p| {
            fs::read(p.path().join("environ")).is_ok_and(|env| {
                env.split(|b| *b == 0).filter_map(|v| v.strip_prefix(b"WINEPREFIX=")).any(|v| {
                    let v = PathBuf::from(String::from_utf8_lossy(v).trim_end_matches('/'));
                    v == pfx || fs::canonicalize(&v).is_ok_and(|c| c == want)
                })
            })
        })
    }
    #[cfg(not(unix))]
    {
        let _ = pfx;
        false
    }
}

/// Sets the override for `dll` in the prefix, for the game in `game`.
pub fn enable(pfx: &Path, dll: &str, game: &Path) -> Result<(), Why> {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    enable_in(&records_path(), pfx, dll, game, now)
}

fn enable_in(records_path: &Path, pfx: &Path, dll: &str, game: &Path, now: u64) -> Result<(), Why> {
    let reg = pfx.join("user.reg");
    let original = fs::read(&reg).map_err(|_| Why::NoPrefix)?;
    let text = String::from_utf8_lossy(&original);
    let mut records = load(records_path);
    let at = records.iter().position(|r| r.reg == reg && r.dll == dll);
    if is_set(&text, dll) {
        // Ours already (another game in this prefix), or set by the player: nothing to write.
        if let Some(i) = at.filter(|&i| !records[i].games.iter().any(|g| g == game)) {
            records[i].games.push(game.to_path_buf());
            save(records_path, &records)?;
        }
        return Ok(());
    }
    if running(pfx) {
        return Err(Why::Running);
    }
    let io = |e: std::io::Error| Why::Io(e.to_string());
    let (new, created_key, previous) = set_value(&text, dll, now);
    fs::write(pfx.join(BACKUP), &original).map_err(io)?;
    fs::write(&reg, new.as_bytes()).map_err(io)?;
    // A record whose value is gone (the prefix was made again) is replaced; its games still need it.
    let mut games = at.map(|i| records.remove(i).games).unwrap_or_default();
    if !games.iter().any(|g| g == game) {
        games.push(game.to_path_buf());
    }
    records.push(Override { reg, dll: dll.into(), games, written: hash(new.as_bytes()), created_key, previous });
    save(records_path, &records)
}

/// Takes out every override set for the game in `game` that no other game still needs. Nothing on Windows.
pub fn disable(game: &Path) -> Result<(), Why> {
    if cfg!(windows) {
        return Ok(());
    }
    disable_in(&records_path(), Some(game))
}

/// Every override undone (`--uninstall`). A prefix that cannot be undone now (its game runs) does not stop the others;
/// it is reported after them.
pub fn disable_all() -> Result<(), Why> {
    if cfg!(windows) {
        return Ok(());
    }
    disable_in(&records_path(), None)
}

fn disable_in(records_path: &Path, game: Option<&Path>) -> Result<(), Why> {
    let mut result = Ok(());
    let mut kept = Vec::new();
    for mut r in load(records_path) {
        let before = r.games.clone();
        r.games.retain(|g| game.is_some_and(|game| g != game));
        if (game.is_some() && r.games.len() == before.len()) || !r.games.is_empty() {
            kept.push(r);
            continue;
        }
        let pfx = r.reg.parent().unwrap_or(Path::new("/"));
        if r.reg.exists() && running(pfx) {
            result = Err(Why::Running);
            kept.push(Override { games: before, ..r });
            continue;
        }
        if let Err(e) = restore(&r) {
            result = Err(e);
            kept.push(r);
        }
    }
    save(records_path, &kept).and(result)
}

fn restore(r: &Override) -> Result<(), Why> {
    let io = |e: std::io::Error| Why::Io(e.to_string());
    let backup = r.reg.with_file_name(BACKUP);
    match fs::read(&r.reg) {
        // Untouched since: the original, byte for byte.
        Ok(now) if hash(&now) == r.written && backup.exists() => fs::rename(&backup, &r.reg).map_err(io)?,
        Ok(now) => {
            let text = unset_value(&String::from_utf8_lossy(&now), &r.dll, r.created_key, r.previous.as_deref());
            fs::write(&r.reg, text).map_err(io)?;
            let _ = fs::remove_file(&backup);
        }
        // The prefix is gone: nothing left to undo.
        Err(_) => {
            let _ = fs::remove_file(&backup);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const REG: &str = "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n[Software\\\\Wine] 1700000000\n#time=1da0000000000000\n\"Version\"=\"win10\"\n";

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("portxr-proton-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn set_then_unset_is_the_original() {
        let (added, created, previous) = set_value(REG, "winhttp", 1_732_000_000);
        assert!(created && previous.is_none());
        assert!(added.contains("[Software\\\\Wine\\\\DllOverrides] 1732000000\n#time="));
        assert!(added.contains("\n\"winhttp\"=\"native,builtin\"\n"));
        assert!(is_set(&added, "winhttp") && !is_set(&added, "dwmapi") && !is_set(REG, "winhttp"));
        assert_eq!(unset_value(&added, "winhttp", true, None), REG);
    }

    #[test]
    fn existing_key_keeps_other_values() {
        let with_key = format!("{REG}\n[{KEY}] 1700000001\n#time=1da\n\"d3d11\"=\"native\"\n\n[Software\\\\Wine\\\\Fonts] 1\n");
        let (added, created, _) = set_value(&with_key, "dwmapi", 9);
        assert!(!created);
        let lines: Vec<&str> = added.lines().collect();
        let at = lines.iter().position(|l| l.starts_with(&format!("[{KEY}]"))).unwrap();
        assert_eq!(lines[at + 2], "\"dwmapi\"=\"native,builtin\"");
        assert_eq!(unset_value(&added, "dwmapi", false, None), with_key);
    }

    #[test]
    fn a_value_set_before_comes_back() {
        let with_value = format!("{REG}\n[{KEY}] 1700000001\n\"WinHttp\"=\"builtin\"\n");
        let (added, created, previous) = set_value(&with_value, "winhttp", 9);
        assert!(!created);
        assert_eq!(previous.as_deref(), Some("\"WinHttp\"=\"builtin\""));
        assert!(is_set(&added, "winhttp") && !added.contains("\"WinHttp\""));
        assert_eq!(unset_value(&added, "winhttp", false, previous.as_deref()), with_value);
    }

    #[test]
    fn enable_then_disable_restores_the_prefix() {
        let dir = temp("roundtrip");
        let (pfx, records) = (dir.join("pfx"), dir.join("records.json"));
        fs::create_dir_all(&pfx).unwrap();
        fs::write(pfx.join("user.reg"), REG).unwrap();
        let (a, b) = (dir.join("game-a"), dir.join("game-b"));
        enable_in(&records, &pfx, "winhttp", &a, 1_732_000_000).unwrap();
        enable_in(&records, &pfx, "winhttp", &b, 1_732_000_001).unwrap();
        assert!(is_set(&fs::read_to_string(pfx.join("user.reg")).unwrap(), "winhttp"));
        assert_eq!(load(&records)[0].games, vec![a.clone(), b.clone()]);
        // Another game in the prefix still needs it.
        disable_in(&records, Some(&a)).unwrap();
        assert!(is_set(&fs::read_to_string(pfx.join("user.reg")).unwrap(), "winhttp"));
        disable_in(&records, Some(&b)).unwrap();
        assert_eq!(fs::read_to_string(pfx.join("user.reg")).unwrap(), REG);
        assert!(!pfx.join(BACKUP).exists() && !records.exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn after_the_game_ran_only_our_value_goes() {
        let dir = temp("rewritten");
        let records = dir.join("records.json");
        fs::write(dir.join("user.reg"), REG).unwrap();
        enable_in(&records, &dir, "dwmapi", &dir.join("game"), 5).unwrap();
        // Wine rewrote the file on exit, with a change of its own.
        let ran = fs::read_to_string(dir.join("user.reg")).unwrap().replace("win10", "win11");
        fs::write(dir.join("user.reg"), &ran).unwrap();
        disable_in(&records, None).unwrap();
        assert_eq!(fs::read_to_string(dir.join("user.reg")).unwrap(), REG.replace("win10", "win11"));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn no_prefix_yet() {
        let dir = temp("none");
        assert_eq!(enable_in(&dir.join("records.json"), &dir.join("pfx"), "winhttp", &dir, 0), Err(Why::NoPrefix));
        let _ = fs::remove_dir_all(&dir);
    }
}
