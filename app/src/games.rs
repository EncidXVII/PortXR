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
    /// The folder the user added by hand that found this game, if no store knows it.
    pub added: Option<PathBuf>,
    /// Where it was found ("Steam", "Epic Games", "GOG", "Added by you"), for the library's store filter.
    pub store: &'static str,
    /// Unix seconds: Steam's own record, or the last Play in VR from this app, whichever is later.
    pub last_played: u64,
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
            Compat::Supported if self.installed() => "VR added",
            Compat::Supported => "Ready",
            Compat::AntiCheat => "Anti-cheat",
            Compat::X86 => "32-bit",
            Compat::Stripped => "Trimmed engine",
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

// Unity players keep their data next to the exe in `<name>_Data`, holding one of these depending on the Unity version
// and build compression.
const DATA_MARKERS: [&str; 3] = ["globalgamemanagers", "data.unity3d", "mainData"];

fn is_player(exe: &Path) -> bool {
    exe.extension().is_some_and(|x| x.eq_ignore_ascii_case("exe")) && DATA_MARKERS.iter().any(|m| data_dir(exe).join(m).exists())
}

/// The Unity player under `dir`: in it, or up to two folders down (many stores put the game in a subfolder).
fn find_player(dir: &Path, depth: u32) -> Option<PathBuf> {
    let entries: Vec<PathBuf> = fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).collect();
    if let Some(exe) = entries.iter().find(|p| is_player(p)) {
        return Some(exe.clone());
    }
    if depth == 0 {
        return None;
    }
    entries.iter().filter(|p| p.is_dir() && !p.ends_with("_Data")).find_map(|p| find_player(p, depth - 1))
}

/// A Unity game installed in `root` (a store's install folder or a folder the user picked).
pub fn detect(root: &Path) -> Option<Game> {
    let exe = find_player(root, 2)?;
    let dir = exe.parent()?.to_path_buf();
    let names = |d: &Path| -> Vec<String> {
        fs::read_dir(d).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().to_lowercase()).collect()
    };
    // Anti-cheat may sit beside the exe or at the top of the install, next to a launcher.
    let files: Vec<String> = names(&dir).into_iter().chain(if dir == root { Vec::new() } else { names(root) }).collect();
    let blocker = if files.iter().any(|f| f == "easyanticheat" || f.starts_with("easyanticheat") || f == "start_protected_game.exe") {
        Some("EasyAntiCheat")
    } else if files.iter().any(|f| f == "battleye" || f.ends_with("_be.exe")) {
        Some("BattlEye")
    } else {
        None
    };
    let other_loader = if dir.join("MelonLoader").is_dir() {
        Some("MelonLoader")
    } else if dir.join("BepInEx").is_dir() && !installer::receipt_path(&dir).exists() {
        Some("BepInEx")
    } else {
        None
    };
    // app.info holds the company and product name; store games get their store name in scan() instead.
    let product = fs::read_to_string(data_dir(&exe).join("app.info")).ok().and_then(|t| t.lines().nth(1).map(|l| l.trim().to_string()));
    let il2cpp = dir.join("GameAssembly.dll").exists();
    Some(Game {
        name: product.filter(|p| !p.is_empty()).unwrap_or_else(|| root.file_name().unwrap_or_default().to_string_lossy().into_owned()),
        unity: unity_version(&data_dir(&exe)).unwrap_or_else(|| "unknown".into()),
        il2cpp,
        x86: is_x86(&exe).unwrap_or(false),
        stripped: !il2cpp && is_stripped(&dir, &data_dir(&exe).join("Managed")),
        blocker,
        other_loader,
        app_id: 0,
        cover: None,
        added: None,
        store: "",
        last_played: 0,
        dir,
        exe,
    })
}

fn data_dir(exe: &Path) -> PathBuf {
    exe.with_file_name(format!("{}_Data", exe.file_stem().unwrap_or_default().to_string_lossy()))
}

fn c_string(b: &[u8], start: usize) -> Option<(String, usize)> {
    let len = b.get(start..)?.iter().position(|&c| c == 0)?;
    Some((String::from_utf8(b[start..start + len].to_vec()).ok()?, start + len + 1))
}

// Serialized files (globalgamemanagers, mainData): a big-endian header, then the editor version string; the header
// grew from 20 to 48 bytes in format 22 (Unity 2022.2). Compressed builds (data.unity3d) are a UnityFS bundle:
// signature, format, bundle version, then the editor version.
fn unity_version(data: &Path) -> Option<String> {
    let read = |name: &str| -> Option<Vec<u8>> {
        let mut b = vec![0u8; 256];
        let n = File::open(data.join(name)).ok()?.read(&mut b).ok()?;
        b.truncate(n);
        Some(b)
    };
    if let Some(b) = read("data.unity3d").filter(|b| b.starts_with(b"UnityFS\0")) {
        let (_, next) = c_string(&b, 12)?;
        return c_string(&b, next).map(|(v, _)| v);
    }
    let b = read("globalgamemanagers").or_else(|| read("mainData"))?;
    let format = u32::from_be_bytes(b.get(8..12)?.try_into().ok()?);
    c_string(&b, if format >= 22 { 48 } else { 20 }).map(|(v, _)| v)
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

/// Remembers a picked folder: the game in it, or every game one level inside it (a folder like D:\Games).
/// Returns how many games were added.
pub fn add_folder(dir: &Path) -> io::Result<usize> {
    let roots: Vec<PathBuf> = if detect(dir).is_some() {
        vec![dir.to_path_buf()]
    } else {
        fs::read_dir(dir)?.flatten().map(|e| e.path()).filter(|p| p.is_dir() && detect(p).is_some()).collect()
    };
    let list = manual_list();
    fs::create_dir_all(list.parent().unwrap())?;
    let mut file = fs::OpenOptions::new().create(true).append(true).open(list)?;
    for root in &roots {
        writeln!(file, "{}", root.display())?;
    }
    Ok(roots.len())
}

/// Folders the user added by hand, in the order they were added.
pub fn manual() -> Vec<PathBuf> {
    fs::read_to_string(manual_list()).unwrap_or_default().lines().map(str::trim).filter(|l| !l.is_empty()).map(PathBuf::from).collect()
}

/// Forgets a hand-added folder; the game itself is not touched.
pub fn remove_folder(dir: &Path) -> io::Result<()> {
    let kept: String = manual().iter().filter(|p| p.as_path() != dir).map(|p| format!("{}\n", p.display())).collect();
    fs::write(manual_list(), kept)
}

/// Install folders other launchers record: Epic Games manifests and GOG Galaxy's registry entries.
fn other_stores() -> Vec<(PathBuf, Option<String>, &'static str)> {
    let mut found = Vec::new();
    let manifests = PathBuf::from(std::env::var("ProgramData").unwrap_or_else(|_| r"C:\ProgramData".into()))
        .join(r"Epic\EpicGamesLauncher\Data\Manifests");
    for item in fs::read_dir(manifests).into_iter().flatten().flatten().map(|e| e.path()) {
        let Ok(json) = fs::read_to_string(&item).map(|t| serde_json::from_str::<serde_json::Value>(&t)) else { continue };
        let Ok(json) = json else { continue };
        if let Some(dir) = json["InstallLocation"].as_str() {
            found.push((PathBuf::from(dir), json["DisplayName"].as_str().map(String::from), "Epic Games"));
        }
    }
    let hklm = winreg::RegKey::predef(winreg::enums::HKEY_LOCAL_MACHINE);
    let gog = [r"SOFTWARE\WOW6432Node\GOG.com\Games", r"SOFTWARE\GOG.com\Games"].map(|k| hklm.open_subkey(k));
    for key in gog.iter().flatten().flat_map(|k| k.enum_keys().flatten().filter_map(|id| k.open_subkey(id).ok()).collect::<Vec<_>>()) {
        if let Ok(dir) = key.get_value::<String, _>("path") {
            found.push((PathBuf::from(dir), key.get_value::<String, _>("gameName").ok(), "GOG"));
        }
    }
    // Everything else (itch, Amazon, Xbox, DRM-free copies, unregistered Steam libraries): each folder in the usual
    // game locations on every drive; detect() looks two levels into it for a Unity player.
    let env = |v: &str| PathBuf::from(std::env::var(v).unwrap_or_default());
    let mut roots = vec![env("APPDATA").join(r"itch\apps"), env("LOCALAPPDATA").join("Programs")];
    for drive in b'C'..=b'Z' {
        let drive = PathBuf::from(format!("{}:\\", drive as char));
        if !drive.exists() {
            continue;
        }
        for rel in ["Games", "GOG Games", "Epic Games", "Itch Games", r"Amazon Games\Library", "XboxGames", "Program Files",
                    "Program Files (x86)", r"SteamLibrary\steamapps\common", r"Steam\steamapps\common"] {
            roots.push(drive.join(rel));
        }
    }
    for root in roots {
        for dir in fs::read_dir(&root).into_iter().flatten().flatten().map(|e| e.path()).filter(|p| p.is_dir()) {
            found.push((dir, None, "On disk"));
        }
    }
    found
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

fn played_list() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("Parallax").join("played.txt")
}

/// Records a launch from the app (`<unix seconds>\t<game dir>` lines) for the library's "Recently played" order.
pub fn mark_played(dir: &Path) {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let mut lines: Vec<String> = fs::read_to_string(played_list()).unwrap_or_default().lines().filter(|l| !l.ends_with(&format!("\t{}", dir.display()))).map(String::from).collect();
    lines.push(format!("{now}\t{}", dir.display()));
    let _ = fs::create_dir_all(played_list().parent().unwrap());
    let _ = fs::write(played_list(), lines.join("\n") + "\n");
}

/// Steam's cached art for one game: the 2:3 cover, else the wide header. Newer Steam clients keep some of it one
/// folder deeper, under a content hash.
fn cover_in(cache: &Path) -> Option<PathBuf> {
    let find = |file: &str| {
        let top = cache.join(file);
        if top.exists() {
            return Some(top);
        }
        fs::read_dir(cache).ok()?.flatten().map(|e| e.path().join(file)).find(|p| p.exists())
    };
    find("library_600x900.jpg").or_else(|| find("header.jpg"))
}

pub fn scan() -> Vec<Game> {
    let mut found: Vec<(PathBuf, u32, Option<String>, &'static str, u64)> = Vec::new();
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
                let last = vdf_values(&text, "LastPlayed").next().and_then(|v| v.parse().ok()).unwrap_or(0);
                found.push((apps.join("common").join(install), id, vdf_values(&text, "name").next(), "Steam", last));
            }
        }
    }
    found.extend(other_stores().into_iter().map(|(dir, name, store)| (dir, 0, name, store, 0)));
    let stores = found.len();
    found.extend(manual().into_iter().map(|dir| (dir, 0, None, "Added by you", 0)));
    let played = fs::read_to_string(played_list()).unwrap_or_default();

    let mut games: Vec<Game> = Vec::new();
    for (i, (dir, id, name, store, last)) in found.into_iter().enumerate() {
        let Some(mut g) = detect(&dir) else { continue };
        if games.iter().any(|other| other.dir == g.dir) {
            continue;
        }
        g.added = (i >= stores).then(|| dir.clone());
        g.store = store;
        let ours = played.lines().filter_map(|l| l.split_once('\t')).filter(|(_, d)| Path::new(d) == g.dir).filter_map(|(t, _)| t.parse().ok()).max();
        g.last_played = last.max(ours.unwrap_or(0));
        g.app_id = id;
        if let Some(name) = name {
            g.name = name;
        }
        g.cover = steam.as_ref().filter(|_| id > 0).and_then(|s| cover_in(&s.join("appcache").join("librarycache").join(id.to_string())));
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
