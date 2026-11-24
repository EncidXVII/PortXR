// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

//! Linux: games Heroic (Epic, GOG and Amazon) and Lutris installed, with the Wine prefix and runner each one runs in.
//! Heroic keeps its stores' install lists as JSON under its config folder; Lutris keeps one YAML file per game.
#![cfg_attr(windows, allow(dead_code))]

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

use crate::games::Launcher;

/// A game another launcher installed: its folder, its name there, the store, and how the launcher runs it.
pub type Other = (PathBuf, Option<String>, &'static str, Option<Launcher>);

/// Heroic's config folders: the native package's (and AppImage's), then the Flatpak's.
fn heroic_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    dirs.extend(dirs::config_dir().map(|d| d.join("heroic")));
    dirs.extend(dirs::home_dir().map(|h| h.join(".var/app/com.heroicgameslauncher.hgl/config/heroic")));
    dirs.into_iter().filter(|d| d.is_dir()).collect()
}

fn json(path: &Path) -> Value {
    fs::read(path).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or(Value::Null)
}

fn windows(platform: &Value) -> bool {
    platform.as_str().is_none_or(|p| p.eq_ignore_ascii_case("windows"))
}

/// Heroic's installed games: Epic (legendary's installed.json), GOG (gog_store, titles from the library cache) and
/// Amazon (nile's installed.json, titles from its library.json). Windows builds only: the rest run without Wine.
pub fn heroic() -> Vec<Other> {
    let mut found = Vec::new();
    for root in heroic_dirs() {
        let defaults = json(&root.join("config.json"))["defaultSettings"].clone();
        let mut add = |dir: &str, name: Option<&str>, store: &'static str, runner: &str, app: &str| {
            if dir.is_empty() || app.is_empty() {
                return;
            }
            let config = root.join("GamesConfig").join(format!("{app}.json"));
            let launcher = heroic_launcher(&json(&config)[app], &defaults, runner, app, config);
            found.push((PathBuf::from(dir), name.map(String::from), store, launcher));
        };
        let legendary = json(&root.join("legendaryConfig/legendary/installed.json"));
        for (app, g) in legendary.as_object().into_iter().flatten().filter(|(_, g)| windows(&g["platform"])) {
            add(g["install_path"].as_str().unwrap_or_default(), g["title"].as_str(), "Epic Games", "legendary", app);
        }
        let titles = json(&root.join("store_cache/gog_library.json"));
        let title = |app: &str| titles["games"].as_array()?.iter().find(|t| t["app_name"] == app)?["title"].as_str();
        for g in json(&root.join("gog_store/installed.json"))["installed"].as_array().into_iter().flatten().filter(|g| windows(&g["platform"])) {
            let app = g["appName"].as_str().unwrap_or_default();
            add(g["install_path"].as_str().unwrap_or_default(), title(app), "GOG", "gog", app);
        }
        let library = json(&root.join("nile_config/nile/library.json"));
        let title = |app: &str| library.as_array()?.iter().find(|t| t["product"]["id"] == app)?["product"]["title"].as_str();
        for g in json(&root.join("nile_config/nile/installed.json")).as_array().into_iter().flatten() {
            let app = g["id"].as_str().unwrap_or_default();
            add(g["path"].as_str().unwrap_or_default(), title(app), "Amazon Games", "nile", app);
        }
    }
    found
}

/// How Heroic runs one game: its own settings (GamesConfig/<app>.json), else Heroic's defaults. A Proton runner keeps
/// the prefix in a `pfx` folder inside winePrefix; Wine uses winePrefix itself.
fn heroic_launcher(game: &Value, defaults: &Value, runner: &str, app: &str, config: PathBuf) -> Option<Launcher> {
    let setting = |key: &str| Some(&game[key]).filter(|v| !v.is_null()).unwrap_or(&defaults[key]);
    let home = dirs::home_dir().unwrap_or_default();
    let prefix = setting("winePrefix").as_str().map(|p| p.replacen('~', &home.to_string_lossy(), 1))?;
    let wine = setting("wineVersion");
    let plain_wine = wine["type"].as_str().filter(|t| *t != "proton").map(|_| wine["name"].as_str().unwrap_or("Wine").to_string());
    Some(Launcher { url: format!("heroic://launch/{runner}/{app}"), prefix: PathBuf::from(prefix), plain_wine, config })
}

/// Lutris's folders: its config folder (where Lutris keeps games while it exists), its data folder, and the Flatpak's.
fn lutris_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    dirs.extend(dirs::config_dir().map(|d| d.join("lutris")));
    dirs.extend(dirs::data_dir().map(|d| d.join("lutris")));
    if let Some(h) = dirs::home_dir() {
        dirs.extend([h.join(".var/app/net.lutris.Lutris/config/lutris"), h.join(".var/app/net.lutris.Lutris/data/lutris")]);
    }
    dirs.into_iter().filter(|d| d.join("games").is_dir()).collect()
}

/// `section: key` from a Lutris YAML file: top-level sections, their settings indented under them.
fn yaml_value(text: &str, section: &str, key: &str) -> Option<String> {
    let mut inside = false;
    for line in text.lines() {
        if !line.starts_with(' ') && !line.trim().is_empty() {
            inside = line.trim_end() == format!("{section}:");
        } else if inside && let Some(v) = line.trim().strip_prefix(&format!("{key}:")) {
            let v = v.trim();
            let v = v.strip_prefix('\'').and_then(|v| v.strip_suffix('\'')).or_else(|| v.strip_prefix('"').and_then(|v| v.strip_suffix('"'))).unwrap_or(v);
            return Some(v.to_string()).filter(|v| !v.is_empty());
        }
    }
    None
}

/// A Proton build, which brings OpenXR to games through Steam's Linux Runtime. The `lutris-GE-Proton` and `wine-ge`
/// builds are Wine, and "ge-proton" is the newest GE-Proton (Lutris's default runner).
pub fn proton_based(version: &str) -> bool {
    let v = version.to_lowercase();
    v.contains("proton") && !v.starts_with("lutris-") && !v.starts_with("wine-")
}

/// The slug of a Lutris game file, `<slug>-<seconds>.yml`, and a name from it.
fn lutris_slug(file: &Path) -> Option<(String, String)> {
    let stem = file.file_stem()?.to_string_lossy();
    let slug = match stem.rsplit_once('-') {
        Some((slug, n)) if !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()) => slug,
        _ => &stem,
    };
    let name = slug
        .split('-')
        .filter_map(|w| {
            let mut c = w.chars();
            c.next().map(|first| first.to_uppercase().collect::<String>() + c.as_str())
        })
        .collect::<Vec<_>>()
        .join(" ");
    Some((slug.to_string(), name))
}

/// Lutris's Windows games: each game file names the exe and the prefix; the Wine version is the game's own, else the
/// Wine runner's, else Lutris's default (GE-Proton).
pub fn lutris() -> Vec<Other> {
    let mut found = Vec::new();
    for root in lutris_dirs() {
        let runner = fs::read_to_string(root.join("runners/wine.yml")).unwrap_or_default();
        for file in fs::read_dir(root.join("games")).into_iter().flatten().flatten().map(|e| e.path()) {
            if file.extension().is_none_or(|x| x != "yml") {
                continue;
            }
            let text = fs::read_to_string(&file).unwrap_or_default();
            let (Some(exe), Some((slug, name))) = (yaml_value(&text, "game", "exe"), lutris_slug(&file)) else { continue };
            let prefix = yaml_value(&text, "game", "prefix").map(PathBuf::from);
            let exe = match (PathBuf::from(&exe), &prefix) {
                (e, Some(p)) if e.is_relative() => p.join(e),
                (e, _) => e,
            };
            let Some(dir) = exe.parent().filter(|_| exe.extension().is_some_and(|x| x.eq_ignore_ascii_case("exe"))) else { continue };
            let version = yaml_value(&text, "wine", "version").or_else(|| yaml_value(&runner, "wine", "version")).unwrap_or_else(|| "ge-proton".into());
            let launcher = prefix.map(|prefix| Launcher {
                url: format!("lutris:rungame/{slug}"),
                prefix,
                plain_wine: (!proton_based(&version)).then_some(version),
                config: file.clone(),
            });
            found.push((dir.to_path_buf(), Some(name), "Lutris", launcher));
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lutris_files_read() {
        let text = "game:\n  exe: '/home/u/Games/hk/drive_c/Hollow Knight/hollow_knight.exe'\n  prefix: /home/u/Games/hk\nsystem:\n  env:\n    DXVK_HUD: fps\nwine:\n  version: GE-Proton9-20\n";
        assert_eq!(yaml_value(text, "game", "exe").as_deref(), Some("/home/u/Games/hk/drive_c/Hollow Knight/hollow_knight.exe"));
        assert_eq!(yaml_value(text, "game", "prefix").as_deref(), Some("/home/u/Games/hk"));
        assert_eq!(yaml_value(text, "wine", "version").as_deref(), Some("GE-Proton9-20"));
        assert_eq!(yaml_value(text, "system", "version"), None);
        assert_eq!(lutris_slug(Path::new("/x/hollow-knight-1700000000.yml")), Some(("hollow-knight".into(), "Hollow Knight".into())));
        assert_eq!(lutris_slug(Path::new("/x/celeste.yml")), Some(("celeste".into(), "Celeste".into())));
    }

    #[test]
    fn proton_runners() {
        for v in ["ge-proton", "GE-Proton9-20", "Proton - Experimental", "UMU-Proton-9.0-3"] {
            assert!(proton_based(v), "{v}");
        }
        for v in ["lutris-GE-Proton8-26-x86_64", "wine-ge-8-26-x86_64", "lutris-7.2-2-x86_64", "system"] {
            assert!(!proton_based(v), "{v}");
        }
    }

    #[test]
    fn heroic_settings_fall_back_to_defaults() {
        let game = serde_json::json!({ "wineVersion": { "name": "Wine-GE-Proton8-26", "type": "wine" } });
        let defaults = serde_json::json!({ "winePrefix": "/home/u/Games/Heroic/Prefixes/default", "wineVersion": { "name": "GE-Proton", "type": "proton" } });
        let l = heroic_launcher(&game, &defaults, "gog", "1207658930", PathBuf::from("c.json")).unwrap();
        assert_eq!(l.url, "heroic://launch/gog/1207658930");
        assert_eq!(l.prefix, PathBuf::from("/home/u/Games/Heroic/Prefixes/default"));
        assert_eq!(l.plain_wine.as_deref(), Some("Wine-GE-Proton8-26"));
        let l = heroic_launcher(&Value::Null, &defaults, "nile", "amzn1", PathBuf::new()).unwrap();
        assert_eq!(l.plain_wine, None);
    }
}
