// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

//! The PortXR game catalogue (everyvr.net/portxr/catalogue): how each game plays in each display mode, what goes
//! wrong, and the scripts and settings that help. The whole list is downloaded, so nothing about the player's library
//! leaves the machine; a copy is kept for offline starts. Files come only from everyvr.net and are used only when
//! their SHA-256 matches the list's.

use std::collections::BTreeMap;
use std::error::Error;
use std::fs;
use std::path::PathBuf;

use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::games::{self, Game};

const URL: &str = "https://everyvr.net/portxr/catalogue.json";
const FILES: &str = "https://everyvr.net/portxr/catalogue/files/";
/// The report form, in the catalogue's repository on GitHub.
const REPORT: &str = "https://github.com/EncidXVII/PortXR-catalogue/issues/new?template=game-report.yml";
/// The list's layout. A newer one is ignored, and the cached copy stays in use, until PortXR is updated.
const FORMAT: u32 = 1;
/// Where the marker of an installed entry sits, in the game's scripts folder: "<slug> <entry version>".
const MARKER: &str = ".catalogue";

#[derive(Deserialize, Clone, Default)]
pub struct Catalogue {
    format: u32,
    games: Vec<Entry>,
}

#[derive(Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    pub slug: String,
    #[serde(default)]
    steam_app_id: Option<u32>,
    exe: String,
    pub entry_version: String,
    pub url: String,
    /// Display mode ("Full VR", "3D screen", "VR screen") to rating ("perfect", "good", "playable", "broken").
    pub ratings: BTreeMap<String, String>,
    #[serde(default)]
    pub reports: u32,
    #[serde(default)]
    pub files: Vec<File>,
}

#[derive(Deserialize, Clone, Debug)]
pub struct File {
    pub path: String,
    pub kind: String,
    pub sha256: String,
    pub url: String,
}

/// The modes in the order the game page shows them: the deepest way into the game first.
pub const MODES: [&str; 3] = ["Full VR", "3D screen", "VR screen"];

fn cache() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("catalogue.json")
}

/// Last time's list, at once (empty on a first start without network).
pub fn cached() -> Catalogue {
    fs::read(cache()).ok().and_then(|b| serde_json::from_slice::<Catalogue>(&b).ok()).filter(|c| c.format == FORMAT).unwrap_or_default()
}

/// The list as it is now, kept for next time. None when offline or when the site has moved to a newer format.
pub fn fetch() -> Option<Catalogue> {
    let bytes = ureq::get(URL).header("User-Agent", "PortXR (game catalogue)").call().ok()?.body_mut().with_config().limit(4 << 20).read_to_vec().ok()?;
    let list: Catalogue = serde_json::from_slice(&bytes).ok()?;
    if list.format != FORMAT {
        return None;
    }
    let _ = fs::create_dir_all(cache().parent()?);
    let _ = fs::write(cache(), &bytes);
    Some(list)
}

impl Catalogue {
    /// The entry for a game: by its Steam app ID, else by its exe (the Unreal Shipping exe counts too).
    pub fn find(&self, g: &Game) -> Option<&Entry> {
        if g.app_id != 0
            && let Some(e) = self.games.iter().find(|e| e.steam_app_id == Some(g.app_id)) {
                return Some(e);
            }
        let names: Vec<String> = [Some(&g.exe), g.unreal_exe.as_ref()].into_iter().flatten().filter_map(|p| p.file_name()).map(|n| n.to_string_lossy().to_lowercase()).collect();
        self.games.iter().find(|e| e.steam_app_id.is_none_or(|_| g.app_id == 0) && names.contains(&e.exe.to_lowercase()))
    }
}

impl Entry {
    /// The best way to play it: the highest rating, and on a tie the mode that goes furthest into the game.
    pub fn best(&self) -> Option<(&'static str, &str)> {
        const ORDER: [&str; 4] = ["perfect", "good", "playable", "broken"];
        MODES.iter().filter_map(|m| self.ratings.get(*m).map(|r| (*m, r.as_str()))).min_by_key(|(_, r)| ORDER.iter().position(|o| o == r).unwrap_or(ORDER.len()))
    }

    /// "Script", "Settings": what Get installs.
    pub fn what(&self) -> &'static str {
        if self.files.iter().any(|f| f.kind == "script") { "script" } else { "settings" }
    }
}

/// The entry version installed for this game, if any.
pub fn installed(g: &Game, e: &Entry) -> Option<String> {
    let text = fs::read_to_string(games::scripts_dir(g).join(MARKER)).ok()?;
    let (slug, version) = text.trim().split_once(' ')?;
    (slug == e.slug).then(|| version.to_string())
}

/// Whether the installed files are older than the entry.
pub fn outdated(installed: &str, e: &Entry) -> bool {
    let v = |s: &str| s.split('.').map(|p| p.parse::<u32>().unwrap_or(0)).collect::<Vec<_>>();
    v(installed) < v(&e.entry_version)
}

/// Downloads an entry's files and checks each against its SHA-256. Run off the UI thread.
pub fn download(e: &Entry) -> Result<Vec<(File, Vec<u8>)>, String> {
    e.files
        .iter()
        .map(|f| {
            // Only files the site published for this entry, with a plain name: never a path out of the scripts folder.
            let plain = !f.path.is_empty() && f.path.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"._-".contains(&b)) && !f.path.starts_with('.');
            if !plain || !(f.path.ends_with(".lua") || f.path.ends_with(".cfg")) || f.url != format!("{FILES}{}/{}", e.slug, f.path) {
                return Err(format!("The catalogue lists a file PortXR does not accept: {}.", f.path));
            }
            let bytes = ureq::get(&f.url)
                .header("User-Agent", "PortXR (game catalogue)")
                .call()
                .map_err(|_| "Could not reach everyvr.net. Check the connection and try again.".to_string())?
                .body_mut()
                .with_config()
                .limit(128 << 10)
                .read_to_vec()
                .map_err(|_| format!("{} did not download completely.", f.path))?;
            let sum: String = Sha256::digest(&bytes).iter().map(|b| format!("{b:02x}")).collect();
            if sum != f.sha256 {
                return Err(format!("{} does not match its SHA-256, so it was not used.", f.path));
            }
            Ok((f.clone(), bytes))
        })
        .collect()
}

/// Puts downloaded files in place: scripts into the game's scripts folder, settings into its open settings text (the
/// caller saves it). Returns the new settings text.
pub fn install(g: &Game, e: &Entry, files: &[(File, Vec<u8>)], cfg: &str) -> Result<String, Box<dyn Error>> {
    let mut cfg = cfg.to_string();
    for (f, bytes) in files {
        let text = std::str::from_utf8(bytes)?;
        if f.kind == "script" {
            games::save_script(g, &f.path, bytes)?;
        } else {
            let mut section = String::new();
            for line in text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#') && !l.starts_with(';')) {
                if let Some(s) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
                    section = s.to_string();
                } else if let Some((key, value)) = line.split_once('=') {
                    cfg = crate::settings::set(&cfg, &section, key.trim(), value.trim());
                }
            }
        }
    }
    let dir = games::scripts_dir(g);
    fs::create_dir_all(&dir)?;
    fs::write(dir.join(MARKER), format!("{} {}\n", e.slug, e.entry_version))?;
    Ok(cfg)
}

/// The report form with this game filled in.
pub fn report_url(g: &Game) -> String {
    let enc = |s: &str| s.bytes().map(|b| if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect::<String>();
    let mut url = format!("{REPORT}&title={}&game={}&engine={}&portxr={}", enc(&format!("[Report] {}", g.name)), enc(&g.name), enc(&g.engine_label()), env!("CARGO_PKG_VERSION"));
    if g.app_id != 0 {
        url += &format!("&steam={}", enc(&format!("https://store.steampowered.com/app/{}", g.app_id)));
    }
    url
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(json: &str) -> Entry {
        serde_json::from_str(json).unwrap()
    }

    #[test]
    fn reads_the_site_list_and_compares_versions() {
        let e = entry(r#"{"slug":"a-game","game":"A Game","steamAppId":5,"exe":"A Game.exe","entryVersion":"1.2.0","url":"https://everyvr.net/portxr/catalogue/a-game","ratings":{"Full VR":"good"},"best":{"mode":"Full VR","rating":"good"},"needs":"script","issues":[{"tag":"hud","note":"x x x","fixed":true}],"reports":2,"files":[{"path":"script.lua","kind":"script","size":3,"sha256":"00","url":"https://everyvr.net/portxr/catalogue/files/a-game/script.lua"}]}"#);
        assert_eq!(e.what(), "script");
        assert!(outdated("1.1.9", &e) && !outdated("1.2.0", &e) && !outdated("1.10.0", &e));
    }

    #[test]
    fn refuses_a_file_from_anywhere_else() {
        let mut e = entry(r#"{"slug":"a","game":"A","exe":"A.exe","entryVersion":"1.0.0","url":"u","ratings":{},"needs":"script","files":[{"path":"x.lua","kind":"script","sha256":"0","url":"https://example.com/x.lua"}]}"#);
        assert!(download(&e).unwrap_err().contains("does not accept"));
        e.files[0].url = format!("{FILES}a/../x.lua");
        e.files[0].path = "../x.lua".into();
        assert!(download(&e).unwrap_err().contains("does not accept"));
    }
}
