use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::installer;

#[derive(Clone, Copy, PartialEq)]
pub enum Compat {
    Supported,
    AntiCheat,
    X86,
    Stripped,
}

#[derive(Clone)]
pub struct Game {
    pub name: String,
    pub dir: PathBuf,
    pub exe: PathBuf,
    pub unity: String,
    pub il2cpp: bool,
    pub x86: bool,
    pub stripped: bool,
    pub blocker: Option<&'static str>,
    pub other_loader: Option<&'static str>,
    pub app_id: u32,
    pub cover: Option<PathBuf>,
}

impl Game {
    pub fn compat(&self) -> Compat {
        if self.blocker.is_some() {
            Compat::AntiCheat
        } else if self.x86 {
            Compat::X86
        } else if self.stripped {
            Compat::Stripped
        } else {
            Compat::Supported
        }
    }

    pub fn badge(&self) -> &'static str {
        match self.compat() {
            Compat::Supported if self.installed() => "Installed",
            Compat::Supported => "Ready",
            Compat::AntiCheat => "Anti-cheat: blocked",
            Compat::X86 => "32-bit: no VR runtime",
            Compat::Stripped => "Trimmed engine: not supported",
        }
    }

    pub fn backend(&self) -> &'static str {
        if self.il2cpp { "IL2CPP" } else { "Mono" }
    }

    pub fn installed(&self) -> bool {
        installer::receipt_path(&self.dir).exists()
    }

    pub fn file(&self, rel: &str) -> PathBuf {
        self.dir.join(rel)
    }

    pub fn launch(&self) -> io::Result<()> {
        if self.app_id > 0 {
            Command::new("explorer").arg(format!("steam://rungameid/{}", self.app_id)).spawn()?;
        } else {
            Command::new(&self.exe).current_dir(&self.dir).spawn()?;
        }
        Ok(())
    }
}

/// A Unity player is an .exe next to a `<name>_Data` folder that holds globalgamemanagers.
pub fn detect(dir: &Path) -> Option<Game> {
    let entries: Vec<PathBuf> = fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).collect();
    let exe = entries.iter().find(|p| {
        p.extension().is_some_and(|x| x.eq_ignore_ascii_case("exe")) && data_dir(p).join("globalgamemanagers").exists()
    })?;
    let file_name = |p: &PathBuf| p.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
    let has = |name: &str| dir.join(name).is_dir();
    let blocker = if has("EasyAntiCheat") || entries.iter().any(|p| file_name(p).starts_with("easyanticheat")) {
        Some("EasyAntiCheat")
    } else if has("BattlEye") || entries.iter().any(|p| file_name(p).ends_with("_be.exe")) {
        Some("BattlEye")
    } else {
        None
    };
    let other_loader = if has("MelonLoader") {
        Some("MelonLoader")
    } else if has("BepInEx") && !installer::receipt_path(dir).exists() {
        Some("BepInEx")
    } else {
        None
    };
    // app.info holds the company and product name; Steam games get their store name in scan() instead.
    let product = fs::read_to_string(data_dir(exe).join("app.info")).ok().and_then(|t| t.lines().nth(1).map(|l| l.trim().to_string()));
    Some(Game {
        name: product.filter(|p| !p.is_empty()).unwrap_or_else(|| dir.file_name().unwrap_or_default().to_string_lossy().into_owned()),
        dir: dir.to_path_buf(),
        exe: exe.clone(),
        unity: unity_version(&data_dir(exe)).unwrap_or_else(|| "unknown".into()),
        il2cpp: dir.join("GameAssembly.dll").exists(),
        x86: is_x86(exe).unwrap_or(false),
        stripped: !dir.join("GameAssembly.dll").exists() && is_stripped(dir, &data_dir(exe).join("Managed")),
        blocker,
        other_loader,
        app_id: 0,
        cover: None,
    })
}

fn data_dir(exe: &Path) -> PathBuf {
    exe.with_file_name(format!("{}_Data", exe.file_stem().unwrap_or_default().to_string_lossy()))
}

// The serialized-file header (big-endian) is followed by the editor version string; the header grew from 20 to
// 48 bytes in format 22 (Unity 2022.2).
fn unity_version(data: &Path) -> Option<String> {
    let mut b = [0u8; 128];
    File::open(data.join("globalgamemanagers")).ok()?.read_exact(&mut b).ok()?;
    let format = u32::from_be_bytes(b[8..12].try_into().ok()?);
    let start = if format >= 22 { 48 } else { 20 };
    let len = b[start..].iter().position(|&c| c == 0)?;
    String::from_utf8(b[start..start + len].to_vec()).ok()
}

// Mono games built with managed code stripping lack engine and class-library code the mod loader runs on, so it
// never starts. Untrimmed libraries are much larger: UnityEngine.CoreModule is 0.85 MB (2018.4) to 2 MB (Unity 6),
// trimmed 0.26 MB; mscorlib is 4.07 MB or more on the .NET 4.x runtime (2.5 MB on .NET 3.5), trimmed 2.6 MB.
fn is_stripped(dir: &Path, managed: &Path) -> bool {
    let size = |name: &str| fs::metadata(managed.join(name)).map(|m| m.len()).ok();
    let corlib_min = if dir.join("MonoBleedingEdge").is_dir() { 3_500_000 } else { 1_800_000 };
    size("UnityEngine.CoreModule.dll").is_some_and(|s| s < 600_000) || size("mscorlib.dll").is_some_and(|s| s < corlib_min)
}

// OpenXR runtimes (SteamVR, Virtual Desktop, Meta) ship 64-bit only.
fn is_x86(exe: &Path) -> io::Result<bool> {
    let mut b = vec![0u8; 4096];
    let n = File::open(exe)?.read(&mut b)?;
    let pe = u32::from_le_bytes(b[0x3C..0x40].try_into().unwrap()) as usize;
    Ok(pe + 6 <= n && u16::from_le_bytes([b[pe + 4], b[pe + 5]]) == 0x14C)
}

fn manual_list() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("Parallax").join("games.txt")
}

pub fn add_manual(dir: &Path) -> io::Result<()> {
    let list = manual_list();
    fs::create_dir_all(list.parent().unwrap())?;
    writeln!(fs::OpenOptions::new().create(true).append(true).open(list)?, "{}", dir.display())
}

/// Steam manifests hold one `"key"  "value"` pair per line.
fn vdf_values<'a>(text: &'a str, key: &str) -> impl Iterator<Item = String> + 'a {
    let quoted = format!("\"{key}\"");
    text.lines().filter_map(move |l| {
        let rest = l.trim().strip_prefix(quoted.as_str())?.trim();
        Some(rest.trim_matches('"').replace("\\\\", "\\"))
    })
}

fn steam_root() -> Option<PathBuf> {
    let key = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER).open_subkey(r"Software\Valve\Steam").ok()?;
    key.get_value::<String, _>("SteamPath").ok().map(PathBuf::from)
}

pub fn scan() -> Vec<Game> {
    let mut found: Vec<(PathBuf, u32, Option<String>)> = Vec::new();
    let steam = steam_root();
    if let Some(steam) = &steam {
        let vdf = fs::read_to_string(steam.join("config").join("libraryfolders.vdf")).unwrap_or_default();
        let mut roots: Vec<PathBuf> = vdf_values(&vdf, "path").map(PathBuf::from).collect();
        if roots.is_empty() {
            roots.push(steam.clone());
        }
        for apps in roots.iter().map(|r| r.join("steamapps")) {
            for manifest in fs::read_dir(&apps).into_iter().flatten().flatten().map(|e| e.path()) {
                let name = manifest.file_name().unwrap_or_default().to_string_lossy().into_owned();
                if !name.starts_with("appmanifest_") {
                    continue;
                }
                let text = fs::read_to_string(&manifest).unwrap_or_default();
                let Some(install) = vdf_values(&text, "installdir").next() else { continue };
                let id = vdf_values(&text, "appid").next().and_then(|v| v.parse().ok()).unwrap_or(0);
                found.push((apps.join("common").join(install), id, vdf_values(&text, "name").next()));
            }
        }
    }
    for line in fs::read_to_string(manual_list()).unwrap_or_default().lines().filter(|l| !l.trim().is_empty()) {
        found.push((PathBuf::from(line.trim()), 0, None));
    }

    let mut games: Vec<Game> = Vec::new();
    for (dir, id, name) in found {
        if games.iter().any(|g| g.dir == dir) {
            continue;
        }
        let Some(mut g) = detect(&dir) else { continue };
        g.app_id = id;
        if let Some(name) = name {
            g.name = name;
        }
        g.cover = steam.as_ref().filter(|_| id > 0).and_then(|s| {
            let cache = s.join("appcache").join("librarycache").join(id.to_string());
            ["library_600x900.jpg", "header.jpg"].iter().map(|f| cache.join(f)).find(|p| p.exists())
        });
        games.push(g);
    }
    games.sort_by_key(|g| g.name.to_lowercase());
    games
}

/// One zip on the desktop for bug reports: plugin log, settings, install receipt and Unity's own Player.log.
pub fn export_logs(g: &Game) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let stem: String = g.name.chars().filter(|c| !r#"<>:"/\|?*"#.contains(*c)).collect();
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
    let desktop = dirs::desktop_dir().ok_or("no desktop folder")?;
    let path = desktop.join(format!("Parallax logs - {stem} - {stamp}.zip"));
    let mut zip = zip::ZipWriter::new(File::create(&path)?);
    let opts = zip::write::SimpleFileOptions::default();
    let files = [
        g.file(r"BepInEx\LogOutput.log"),
        g.file(r"BepInEx\config\parallax.core.cfg"),
        installer::receipt_path(&g.dir),
    ];
    for file in files.into_iter().chain(player_log(g)) {
        // The game may still be running and holding its logs open; std opens files with full sharing.
        let Ok(bytes) = fs::read(&file) else { continue };
        zip.start_file(file.file_name().unwrap().to_string_lossy(), opts)?;
        zip.write_all(&bytes)?;
    }
    zip.start_file("game.txt", opts)?;
    write!(zip, "{}\r\nUnity {}, {}, {}\r\n", g.name, g.unity, g.backend(), g.badge())?;
    zip.finish()?;
    Ok(path)
}

// Unity writes Player.log under LocalLow\<company>\<product>, both listed in <game>_Data\app.info.
fn player_log(g: &Game) -> Option<PathBuf> {
    let info = fs::read_to_string(data_dir(&g.exe).join("app.info")).ok()?;
    let mut lines = info.lines();
    let (company, product) = (lines.next()?, lines.next()?);
    Some(dirs::data_local_dir()?.parent()?.join("LocalLow").join(company).join(product).join("Player.log"))
}
