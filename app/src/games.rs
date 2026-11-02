use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use serde::{Deserialize, Deserializer, Serialize};

use crate::installer;

#[derive(Clone, Copy, PartialEq)]
pub enum Compat {
    Supported,
    /// Anti-cheat or online-only: injecting could get the account banned.
    Blocked,
    NativeVr,
    LinuxBuild,
    X86,
    Stripped,
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Game {
    pub name: String,
    pub dir: PathBuf,
    pub exe: PathBuf,
    pub unity: String,
    pub il2cpp: bool,
    pub x86: bool,
    pub stripped: bool,
    /// Steam installed the game's own Linux version: Parallax needs the Windows one, run by Proton.
    #[serde(default)]
    pub linux_build: bool,
    /// Steam lists its own VR support: it is played in VR without Parallax.
    #[serde(default)]
    pub native_vr: bool,
    #[serde(deserialize_with = "leak_opt")]
    pub blocker: Option<&'static str>,
    #[serde(deserialize_with = "leak_opt")]
    pub other_loader: Option<&'static str>,
    pub app_id: u32,
    pub cover: Option<PathBuf>,
    /// The folder the user added by hand that found this game, if no store knows it.
    pub added: Option<PathBuf>,
    /// Where it was found ("Steam", "Epic Games", "GOG", "Added by you"), for the library's store filter.
    #[serde(deserialize_with = "leak")]
    pub store: &'static str,
    /// Unix seconds: Steam's own record, or the last Play in VR from this app, whichever is later.
    pub last_played: u64,
}

impl Game {
    pub fn compat(&self) -> Compat {
        if self.blocker.is_some() {
            Compat::Blocked
        } else if self.native_vr {
            Compat::NativeVr
        } else if self.linux_build {
            Compat::LinuxBuild
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
            Compat::Supported => "Installed",
            Compat::Blocked => self.blocker.unwrap_or("Anti-cheat"),
            Compat::NativeVr => "Already VR",
            Compat::LinuxBuild => "Linux build",
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

    /// A file in the game folder, by a backslash path as the receipt and the plugin name them.
    pub fn file(&self, rel: &str) -> PathBuf {
        native(&self.dir, rel)
    }

    pub fn launch(&self) -> io::Result<()> {
        if self.app_id > 0 {
            open(format!("steam://rungameid/{}", self.app_id))
        } else if cfg!(windows) {
            Command::new(&self.exe).current_dir(&self.dir).spawn().map(|_| ())
        } else {
            Err(io::Error::other(format!("Outside Steam, start the game from its launcher (Heroic, Lutris) with {WINE_DLL} set.")))
        }
    }
}

/// `base` joined with a backslash-separated relative path, on any OS.
pub fn native(base: &Path, rel: &str) -> PathBuf {
    base.join(rel.replace('\\', "/"))
}

/// Opens a folder, file or URL with the desktop's default handler.
pub fn open(target: impl AsRef<std::ffi::OsStr>) -> io::Result<()> {
    Command::new(if cfg!(windows) { "explorer" } else { "xdg-open" }).arg(target).spawn().map(|_| ())
}

/// Opens a text file in an editor.
pub fn edit(file: &Path) -> io::Result<()> {
    if cfg!(windows) { Command::new("notepad").arg(file).spawn().map(|_| ()) } else { open(file) }
}

/// Proton loads the Windows mod loader only with this override; with it, Monado and WiVRn reach the game too.
pub const WINE_DLL: &str = "WINEDLLOVERRIDES=\"winhttp=n,b\"";
pub const LAUNCH_OPTIONS: &str = "WINEDLLOVERRIDES=\"winhttp=n,b\" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%";

// The labels are a handful of short strings, read once per start.
fn leak<'de, D: Deserializer<'de>>(d: D) -> Result<&'static str, D::Error> {
    Ok(String::deserialize(d)?.leak())
}

fn leak_opt<'de, D: Deserializer<'de>>(d: D) -> Result<Option<&'static str>, D::Error> {
    Ok(Option::<String>::deserialize(d)?.map(|s| &*s.leak()))
}

fn library_cache() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("Parallax").join("library.json")
}

/// The last scan's games, shown at start while the new scan runs (Play checks the folder again before anything runs).
pub fn cached() -> Vec<Game> {
    // The labels make Game deserialize only from 'static data; the file is small and read once.
    let bytes: &'static [u8] = fs::read(library_cache()).unwrap_or_default().leak();
    let games: Vec<Game> = serde_json::from_slice(bytes).unwrap_or_default();
    games.into_iter().filter(|g| g.exe.exists()).collect()
}

// Unity players keep their data next to the exe in `<name>_Data`, holding one of these depending on the Unity version
// and build compression.
const DATA_MARKERS: [&str; 3] = ["globalgamemanagers", "data.unity3d", "mainData"];

// Windows players are .exe; a Linux build (Steam's default where one exists) is .x86_64.
fn is_player(exe: &Path) -> bool {
    exe.extension().is_some_and(|x| x.eq_ignore_ascii_case("exe") || x == "x86_64") && DATA_MARKERS.iter().any(|m| data_dir(exe).join(m).exists())
}

/// The Unity player under `dir`: in it, or up to two folders down (many stores put the game in a subfolder).
fn find_player(dir: &Path, depth: u32) -> Option<PathBuf> {
    let entries: Vec<PathBuf> = fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).collect();
    let windows = |p: &PathBuf| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("exe"));
    // Some folders hold a second player (Dusk ships Duskworld beside it): the one named like the folder, else the
    // shortest name, is the game itself.
    let folder = dir.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
    let stem = |p: &PathBuf| p.file_stem().unwrap_or_default().to_string_lossy().to_lowercase();
    let rank = |p: &&PathBuf| (windows(p), stem(p) == folder, std::cmp::Reverse(stem(p).len()));
    if let Some(exe) = entries.iter().filter(|p| is_player(p)).max_by_key(rank) {
        return Some(exe.clone());
    }
    if depth == 0 {
        return None;
    }
    entries.iter().filter(|p| p.is_dir() && !p.ends_with("_Data")).find_map(|p| find_player(p, depth - 1))
}

/// Client anti-cheat by the files it ships (lowercase names in the game's folders). Anything named anti-cheat counts:
/// a false alarm only keeps one game flat, a miss can cost an account.
fn anti_cheat_files(files: &[String]) -> Option<&'static str> {
    const KNOWN: [(&str, &str); 18] = [
        ("easyanticheat", "EasyAntiCheat"),
        ("start_protected_game", "EasyAntiCheat"),
        ("eaanticheat", "EA Anti-Cheat"),
        ("battleye", "BattlEye"),
        ("beclient", "BattlEye"),
        ("beservice", "BattlEye"),
        ("gameguard", "GameGuard"),
        ("npgg", "GameGuard"),
        ("xigncode", "XIGNCODE3"),
        ("x3.xem", "XIGNCODE3"),
        ("anticheatexpert", "ACE"),
        ("ace-base", "ACE"),
        ("sguard", "ACE"),
        ("mhyprot", "HoYoverse anti-cheat"),
        ("hoyokprotect", "HoYoverse anti-cheat"),
        ("equ8", "EQU8"),
        ("pbsvc", "PunkBuster"),
        ("neac", "NetEase anti-cheat"),
    ];
    for f in files {
        if let Some((_, name)) = KNOWN.iter().find(|(k, _)| f.starts_with(k)) {
            return Some(name);
        }
        if f.ends_with("_be.exe") {
            return Some("BattlEye");
        }
        if ["anticheat", "anti-cheat", "anti_cheat"].iter().any(|k| f.contains(k)) {
            return Some("Anti-cheat");
        }
    }
    None
}

/// The Anti-Cheat Toolkit asset in the game's code: it reports injected code to the game, which an online game may
/// pass on to its servers.
fn has_cheat_toolkit(g: &Game) -> bool {
    let data = data_dir(&g.exe);
    if data.join("Managed").join("ACTk.Runtime.dll").exists() {
        return true;
    }
    let code = if g.il2cpp { data.join("il2cpp_data").join("Metadata").join("global-metadata.dat") } else { data.join("Managed").join("Assembly-CSharp.dll") };
    let needle = b"CodeStage.AntiCheat";
    fs::read(code).is_ok_and(|b| b.windows(needle.len()).any(|w| w == needle))
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
    let blocker = anti_cheat_files(&files);
    let other_loader = if dir.join("MelonLoader").is_dir() {
        Some("MelonLoader")
    } else if dir.join("BepInEx").is_dir() && !installer::receipt_path(&dir).exists() {
        Some("BepInEx")
    } else {
        None
    };
    // app.info holds the company and product name; store games get their store name in scan() instead.
    let product = fs::read_to_string(data_dir(&exe).join("app.info")).ok().and_then(|t| t.lines().nth(1).map(|l| l.trim().to_string()));
    let il2cpp = dir.join("GameAssembly.dll").exists() || dir.join("GameAssembly.so").exists();
    Some(Game {
        name: product.filter(|p| !p.is_empty()).unwrap_or_else(|| root.file_name().unwrap_or_default().to_string_lossy().into_owned()),
        unity: unity_version(&data_dir(&exe)).unwrap_or_else(|| "unknown".into()),
        il2cpp,
        x86: is_x86(&exe).unwrap_or(false),
        linux_build: exe.extension().is_some_and(|x| x == "x86_64"),
        native_vr: false,
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
    // A player on the old runtime keeps its own Mono folder, even beside another player's MonoBleedingEdge.
    let old_mono = managed.with_file_name("Mono").is_dir() || dir.join("Mono").is_dir();
    let corlib_min = if dir.join("MonoBleedingEdge").is_dir() && !old_mono { 3_500_000 } else { 1_800_000 };
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
#[cfg(not(windows))]
fn other_stores() -> Vec<(PathBuf, Option<String>, &'static str)> {
    // On Linux, Parallax works through Proton: Steam games, and folders added by hand.
    Vec::new()
}

#[cfg(windows)]
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

/// `dir` is `root` or inside it, however each is spelled (Steam's paths differ in case and slashes).
fn under(dir: &Path, root: &Path) -> bool {
    let norm = |p: &Path| p.to_string_lossy().to_lowercase().replace('/', "\\").trim_end_matches('\\').to_string();
    let (d, r) = (norm(dir), norm(root));
    d == r || d.starts_with(&(r + "\\"))
}

#[cfg(windows)]
fn steam_root() -> Option<PathBuf> {
    let key = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER).open_subkey(r"Software\Valve\Steam").ok()?;
    key.get_value::<String, _>("SteamPath").ok().map(PathBuf::from)
}

// The native package, the old ~/.steam link, and the Flatpak.
#[cfg(not(windows))]
fn steam_root() -> Option<PathBuf> {
    let home = dirs::home_dir()?;
    [".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
        .iter()
        .map(|rel| home.join(rel))
        .find(|p| p.join("steamapps").is_dir())
}

/// The game's user folder as the game sees it (`%APPDATA%` with `roaming`, else `LocalLow`): on Linux, inside the
/// game's Proton prefix next to its Steam library (`steamapps/compatdata/<id>/pfx`).
fn game_user_dir(g: &Game, roaming: bool) -> Option<PathBuf> {
    if cfg!(windows) {
        return if roaming { dirs::config_dir() } else { Some(dirs::data_local_dir()?.parent()?.join("LocalLow")) };
    }
    if g.app_id == 0 {
        return None;
    }
    let steamapps = g.dir.ancestors().find(|a| a.file_name().is_some_and(|n| n == "steamapps"))?;
    let users = steamapps.join("compatdata").join(g.app_id.to_string()).join("pfx/drive_c/users/steamuser/AppData");
    Some(users.join(if roaming { "Roaming" } else { "LocalLow" }))
}

/// Linux: Steam's saved launch options for the game lack the loader override, so Proton would start it without
/// Parallax. Asked every frame by the game page, so Steam's file is read at most every two seconds.
pub fn needs_launch_options(g: &Game) -> bool {
    if cfg!(windows) || g.app_id == 0 {
        return false;
    }
    // When it was read, and each app's launch options.
    type Saved = Option<(std::time::Instant, Vec<(u32, String)>)>;
    static SAVED: std::sync::Mutex<Saved> = std::sync::Mutex::new(None);
    let mut saved = SAVED.lock().unwrap();
    if saved.as_ref().is_none_or(|(at, _)| at.elapsed().as_secs() >= 2) {
        let options = steam_root().map(|s| localconfig_values(&s, "LaunchOptions")).unwrap_or_default();
        *saved = Some((std::time::Instant::now(), options));
    }
    !saved.as_ref().unwrap().1.iter().any(|(id, v)| *id == g.app_id && v.contains("winhttp=n"))
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

/// The game's Lua scripts folder, where the plugin looks: `%APPDATA%\Parallax\scripts\<exe name>` (it keys by process
/// name). Outside the game folder, so scripts survive an uninstall.
pub fn scripts_dir(g: &Game) -> PathBuf {
    game_user_dir(g, true).or_else(dirs::config_dir).unwrap_or_default().join("Parallax").join("scripts").join(g.exe.file_stem().unwrap_or_default())
}

pub fn scripts(g: &Game) -> Vec<PathBuf> {
    let mut list: Vec<PathBuf> = fs::read_dir(scripts_dir(g))
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("lua")))
        .collect();
    list.sort();
    list
}

pub fn import_script(g: &Game, file: &Path) -> io::Result<PathBuf> {
    // Linux: the scripts live in the game's Proton prefix, which Proton makes on the first start; making its folders
    // first would leave Proton a half-made prefix.
    if let Some(pfx) = game_user_dir(g, true).filter(|_| !cfg!(windows)).and_then(|d| d.ancestors().nth(5).map(Path::to_path_buf))
        && !pfx.join("system.reg").exists() {
            return Err(io::Error::other("Start the game once through Steam first, so Proton sets up its Windows folder."));
        }
    let dir = scripts_dir(g);
    fs::create_dir_all(&dir)?;
    let to = dir.join(file.file_name().unwrap_or_default());
    fs::copy(file, &to)?;
    Ok(to)
}

/// A Unity game in the Steam library, installed or not.
#[derive(Clone)]
pub struct Owned {
    pub app_id: u32,
    pub name: String,
    pub cover: Option<PathBuf>,
    /// Why Parallax will not run it, known before it is installed: "Anti-cheat", "Multiplayer only", "Already VR" or
    /// "32-bit".
    pub blocker: Option<&'static str>,
    /// Unix seconds, from Steam's per-account play records (kept after an uninstall); 0 if never played.
    pub last_played: u64,
}

/// App ID to the last time any Steam account on this PC played it: localconfig.vdf lists `"<appid>" { "LastPlayed" "<t>" }`.
fn steam_last_played(steam: &Path) -> std::collections::HashMap<u32, u64> {
    let mut out = std::collections::HashMap::new();
    for (id, t) in localconfig_values(steam, "LastPlayed") {
        if let Ok(t) = t.parse::<u64>() {
            let e = out.entry(id).or_insert(0);
            *e = (*e).max(t);
        }
    }
    out
}

/// `(app ID, value)` for `key` in every Steam account's localconfig.vdf, where apps list `"<appid>" { "<key>" "<v>" }`.
fn localconfig_values(steam: &Path, key: &str) -> Vec<(u32, String)> {
    let quoted = format!("\"{key}\"");
    let mut out = Vec::new();
    for user in fs::read_dir(steam.join("userdata")).into_iter().flatten().flatten() {
        let text = fs::read_to_string(user.path().join("config").join("localconfig.vdf")).unwrap_or_default();
        let mut app: Option<u32> = None;
        for line in text.lines().map(str::trim) {
            if let Some(v) = line.strip_prefix(quoted.as_str()) {
                if let Some(id) = app {
                    out.push((id, v.trim().trim_matches('"').replace("\\\"", "\"")));
                }
            } else if let Some(id) = line.strip_prefix('"').and_then(|l| l.strip_suffix('"')).and_then(|n| n.parse().ok()) {
                // A bare quoted number opens an app's block.
                app = Some(id);
            }
        }
    }
    out
}

/// What Steam's app cache says about a game, before or after it is installed.
#[derive(Default)]
struct Facts {
    single_player: bool,
    multiplayer: bool,
    /// VAC, an anti-cheat support page, a protected launcher or a Steam Deck anti-cheat test result.
    anti_cheat: bool,
    /// VR support of its own (31, 53, 54).
    vr: bool,
    osarch: String,
}

impl Facts {
    fn blocker(&self) -> Option<&'static str> {
        if self.anti_cheat {
            Some("Anti-cheat")
        } else if self.multiplayer && !self.single_player {
            // Apps with no player modes listed at all (tools, overlays) are not called multiplayer.
            Some("Multiplayer only")
        } else {
            None
        }
    }
}

fn steam_facts(steam: &Path, ids: &[u32]) -> std::collections::HashMap<u32, Facts> {
    let mut out = std::collections::HashMap::new();
    let Ok(b) = fs::read(steam.join("appcache").join("appinfo.vdf")) else { return out };
    let u32_at = |p: usize| b.get(p..p + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap()));
    let Some(magic) = u32_at(0) else { return out };
    // 0x28: keys inline; 0x29: keys in a string table at the offset after the header.
    let (table, mut p) = match magic & 0xFF {
        0x29 => {
            let Some(at) = b.get(8..16).map(|s| u64::from_le_bytes(s.try_into().unwrap()) as usize) else { return out };
            let count = u32_at(at).unwrap_or(0) as usize;
            let mut strings = Vec::with_capacity(count);
            let mut q = at + 4;
            for _ in 0..count {
                let Some(end) = b[q.min(b.len())..].iter().position(|&c| c == 0) else { break };
                strings.push(String::from_utf8_lossy(&b[q..q + end]).into_owned());
                q += end + 1;
            }
            (Some(strings), 16)
        }
        0x28 => (None, 8),
        _ => return out,
    };
    let cstr = |p: &mut usize| -> String {
        let end = b[*p..].iter().position(|&c| c == 0).unwrap_or(0);
        let s = String::from_utf8_lossy(&b[*p..*p + end]).into_owned();
        *p += end + 1;
        s
    };
    // Walks the binary key-values of one app: common/category/* (2 single-player; 1, 9, 20, 27, 36, 38, 47, 48, 49 multiplayer; 8 VAC),
    // common/osarch, and anything naming anti-cheat (keys like extended/anti_cheat_support_url, values like launch
    // executables and Steam Deck test results).
    fn walk(b: &[u8], p: &mut usize, path: &mut Vec<String>, key: &mut dyn FnMut(&mut usize) -> String, found: &mut Facts) -> Option<()> {
        loop {
            let t = *b.get(*p)?;
            *p += 1;
            if t == 8 {
                return Some(());
            }
            let k = key(p);
            let names_ac = |s: &str| {
                let s = s.to_lowercase();
                ["anticheat", "anti-cheat", "anti_cheat", "battleye", "start_protected_game"].iter().any(|n| s.contains(n))
            };
            found.anti_cheat |= names_ac(&k);
            if path.len() == 3 && path[2] == "category" {
                match k.as_str() {
                    "category_2" => found.single_player = true,
                    "category_1" | "category_9" | "category_20" | "category_27" | "category_36" | "category_38" | "category_47" | "category_48" | "category_49" => found.multiplayer = true,
                    "category_8" => found.anti_cheat = true,
                    "category_31" | "category_53" | "category_54" => found.vr = true,
                    _ => {}
                }
            }
            match t {
                0 => {
                    path.push(k);
                    walk(b, p, path, key, found)?;
                    path.pop();
                }
                1 => {
                    let end = b[*p..].iter().position(|&c| c == 0)?;
                    let v = String::from_utf8_lossy(&b[*p..*p + end]).into_owned();
                    *p += end + 1;
                    found.anti_cheat |= names_ac(&v);
                    if path.len() == 2 && path[1] == "common" && k == "osarch" {
                        found.osarch = v;
                    }
                }
                2 | 3 => {
                    if path.len() == 2 && path[1] == "common" && k == "osarch" {
                        found.osarch = u32::from_le_bytes(b.get(*p..*p + 4)?.try_into().ok()?).to_string();
                    }
                    *p += 4;
                }
                7 => *p += 8,
                _ => return None,
            }
        }
    }
    while let (Some(id), Some(size)) = (u32_at(p), u32_at(p + 4)) {
        if id == 0 {
            break;
        }
        let next = p + 8 + size as usize;
        if ids.contains(&id) {
            let mut q = p + 8 + 60;
            let mut found = Facts::default();
            let mut key = |q: &mut usize| match &table {
                Some(t) => {
                    let i = u32_at(*q).unwrap_or(0) as usize;
                    *q += 4;
                    t.get(i).cloned().unwrap_or_default()
                }
                None => cstr(q),
            };
            if walk(&b, &mut q, &mut Vec::new(), &mut key, &mut found).is_some() {
                out.insert(id, found);
            }
        }
        p = next;
    }
    out
}

fn unity_list() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("Parallax").join("unity-steam.json")
}

/// Steam app ID to name for every Unity game Wikidata knows (engine Unity, with a Steam ID). The whole list is
/// downloaded, so nothing about the player's library leaves the machine; it is cached for a week.
fn unity_games() -> Option<std::collections::HashMap<u32, String>> {
    let path = unity_list();
    let fresh = fs::metadata(&path).and_then(|m| m.modified()).ok().and_then(|t| t.elapsed().ok()).is_some_and(|age| age.as_secs() < 7 * 86400);
    if !fresh {
        // QLever answers in under a second; the Wikidata Query Service is the fallback (it rate-limits during outages).
        let query = "PREFIX wdt: <http://www.wikidata.org/prop/direct/> PREFIX wd: <http://www.wikidata.org/entity/>                      PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> SELECT ?steam ?gLabel WHERE { ?g wdt:P408 wd:Q63966;                      wdt:P1733 ?steam. ?g rdfs:label ?gLabel FILTER(LANG(?gLabel) = \"en\") }";
        let fetched = ["https://qlever.dev/api/wikidata", "https://query.wikidata.org/sparql"].iter().find_map(|url| {
            ureq::get(*url)
                .query("query", query)
                .header("User-Agent", "Parallax (Unity VR injector; Unity game list)")
                .header("Accept", "application/sparql-results+json")
                .call()
                .ok()
                .and_then(|mut r| r.body_mut().with_config().limit(64 << 20).read_json::<serde_json::Value>().ok())
        });
        if let Some(json) = fetched {
            let mut map = serde_json::Map::new();
            for row in json["results"]["bindings"].as_array().into_iter().flatten() {
                if let (Some(id), Some(name)) = (row["steam"]["value"].as_str(), row["gLabel"]["value"].as_str()) {
                    map.insert(id.into(), name.into());
                }
            }
            if !map.is_empty() {
                let _ = fs::create_dir_all(path.parent().unwrap());
                let _ = fs::write(&path, serde_json::to_vec(&map).unwrap_or_default());
            }
        }
    }
    let map: std::collections::HashMap<String, String> = serde_json::from_slice(&fs::read(&path).ok()?).ok()?;
    Some(map.into_iter().filter_map(|(id, name)| Some((id.parse().ok()?, name))).collect())
}

/// Unity games the Steam account has: Steam keeps library art for every app in the library, one folder (or file
/// prefix, on older clients) per app ID. Network: the first call of the week downloads the Unity list.
pub fn steam_unity_library() -> Vec<Owned> {
    let Some(steam) = steam_root() else { return Vec::new() };
    let Some(unity) = unity_games() else { return Vec::new() };
    let cache = steam.join("appcache").join("librarycache");
    let mut ids: Vec<u32> = fs::read_dir(&cache)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| e.file_name().to_string_lossy().split(['_', '.']).next()?.parse().ok())
        .collect();
    ids.sort();
    ids.dedup();
    let mut owned: Vec<Owned> = ids
        .into_iter()
        .filter_map(|id| {
            let name = unity.get(&id)?.clone();
            Some(Owned { app_id: id, name, cover: cover_in(&cache.join(id.to_string())), blocker: None, last_played: 0 })
        })
        .collect();
    let ids: Vec<u32> = owned.iter().map(|o| o.app_id).collect();
    let facts = steam_facts(&steam, &ids);
    let played = steam_last_played(&steam);
    for o in &mut owned {
        o.last_played = played.get(&o.app_id).copied().unwrap_or(0);
        o.blocker = facts.get(&o.app_id).and_then(|f| f.blocker().or(f.vr.then_some("Already VR")).or((f.osarch == "32").then_some("32-bit")));
    }
    owned.sort_by_key(|o| o.name.to_lowercase());
    owned
}

/// Asks Steam to install a game (its own install dialog opens).
pub fn steam_install(app_id: u32) -> io::Result<()> {
    open(format!("steam://install/{app_id}"))
}

type Found = (PathBuf, u32, Option<String>, &'static str, u64);

/// Every Steam install on this PC: its folder and its appmanifest text.
fn steam_manifests(steam: &Path) -> Vec<(PathBuf, String)> {
    let vdf = fs::read_to_string(steam.join("config").join("libraryfolders.vdf")).unwrap_or_default();
    let mut roots: Vec<PathBuf> = vdf_values(&vdf, "path").map(PathBuf::from).collect();
    if roots.is_empty() {
        roots.push(steam.to_path_buf());
    }
    let mut found = Vec::new();
    for apps in roots.iter().map(|r| r.join("steamapps")) {
        for manifest in fs::read_dir(&apps).into_iter().flatten().flatten().map(|e| e.path()) {
            if !manifest.file_name().unwrap_or_default().to_string_lossy().starts_with("appmanifest_") {
                continue;
            }
            let text = fs::read_to_string(&manifest).unwrap_or_default();
            let Some(install) = vdf_values(&text, "installdir").next() else { continue };
            found.push((apps.join("common").join(install), text));
        }
    }
    found
}

/// Every Steam install on this PC: folder, app ID, name, store, last played.
fn steam_installs(steam: &Path) -> Vec<Found> {
    steam_manifests(steam)
        .into_iter()
        .map(|(dir, text)| {
            let id = vdf_values(&text, "appid").next().and_then(|v| v.parse().ok()).unwrap_or(0);
            let last = vdf_values(&text, "LastPlayed").next().and_then(|v| v.parse().ok()).unwrap_or(0);
            (dir, id, vdf_values(&text, "name").next(), "Steam", last)
        })
        .collect()
}

/// Steam is updating the game, or will on its next start (StateFlags other than 4, fully installed). The update could
/// add anti-cheat after the check, so Play waits for it.
pub fn steam_update_pending(dir: &Path) -> bool {
    let Some(steam) = steam_root() else { return false };
    steam_manifests(&steam)
        .into_iter()
        .find(|(root, _)| under(dir, root))
        .and_then(|(_, text)| vdf_values(&text, "StateFlags").next())
        .is_some_and(|flags| flags.trim() != "4")
}

/// Steam's word on each game, for those it knows: anti-cheat or online-only games are blocked even when no
/// anti-cheat file is in their folder (server-side checks, VAC, a launcher that fetches it later).
fn apply_steam(steam: &Path, games: &mut [Game]) {
    // A game copied out of Steam keeps its app ID in steam_appid.txt.
    let id_of = |g: &Game| match g.app_id {
        0 => fs::read_to_string(g.dir.join("steam_appid.txt")).ok().and_then(|t| t.trim().parse().ok()).unwrap_or(0),
        id => id,
    };
    let ids: Vec<u32> = games.iter().map(id_of).filter(|&id| id > 0).collect();
    let facts = steam_facts(steam, &ids);
    for g in games.iter_mut() {
        let Some(f) = facts.get(&id_of(g)) else { continue };
        g.native_vr = f.vr;
        if g.blocker.is_none() {
            g.blocker = f.blocker().or((f.multiplayer && has_cheat_toolkit(g)).then_some("Anti-Cheat Toolkit"));
        }
    }
}

/// One game folder, checked as the library checks it: its files, and Steam's word if Steam installed it.
pub fn inspect(dir: &Path) -> Option<Game> {
    let mut g = detect(dir)?;
    if let Some(steam) = steam_root() {
        if let Some((_, id, ..)) = steam_installs(&steam).into_iter().find(|(root, ..)| under(&g.dir, root)) {
            g.app_id = id;
        }
        apply_steam(&steam, std::slice::from_mut(&mut g));
    }
    Some(g)
}

pub fn scan() -> Vec<Game> {
    let steam = steam_root();
    let mut found: Vec<Found> = steam.as_deref().map(steam_installs).unwrap_or_default();
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
    if let Some(steam) = &steam {
        apply_steam(steam, &mut games);
    }
    games.sort_by_key(|g| g.name.to_lowercase());
    if let Ok(json) = serde_json::to_vec(&games) {
        let _ = fs::create_dir_all(library_cache().parent().unwrap()).and_then(|_| fs::write(library_cache(), json));
    }
    games
}

/// One zip on the desktop for bug reports: plugin log, settings, install receipt and Unity's own Player.log.
pub fn export_logs(g: &Game) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let stem: String = g.name.chars().filter(|c| !r#"<>:"/\|?*"#.contains(*c)).collect();
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
    let desktop = dirs::desktop_dir().or_else(dirs::home_dir).ok_or("no desktop folder")?;
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
    Some(game_user_dir(g, false)?.join(company).join(product).join("Player.log"))
}
