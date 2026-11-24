// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

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
    /// Not Unity: PortXR cannot go inside, so the game's window is shown on the Theater screen.
    Theater,
}

#[derive(Clone, Serialize, Deserialize)]
pub struct Game {
    pub name: String,
    /// "Unity", "Unreal" or "Godot" (games in other engines aren't listed).
    #[serde(default = "unity")]
    pub engine: String,
    pub dir: PathBuf,
    pub exe: PathBuf,
    pub unity: String,
    pub il2cpp: bool,
    pub x86: bool,
    pub stripped: bool,
    /// Steam installed the game's own Linux version: PortXR needs the Windows one, run by Proton.
    #[serde(default)]
    pub linux_build: bool,
    /// Steam lists its own VR support: it is played in VR without PortXR.
    #[serde(default)]
    pub native_vr: bool,
    /// A Godot build without Direct3D 12 (custom engine builds): VR from the engine's own OpenXR, not the core.
    #[serde(default)]
    pub no_d3d12: bool,
    /// Unreal: the Shipping exe that really runs the game (Binaries\Win64), where the VR core goes as dwmapi.dll.
    #[serde(default)]
    pub unreal_exe: Option<PathBuf>,
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
    /// Linux, games Heroic or Lutris installed: how that launcher runs it.
    #[serde(default)]
    pub launcher: Option<Launcher>,
}

/// How a launcher other than Steam starts a game on Linux, and the Wine prefix it runs the game in.
#[derive(Clone, Default, Debug, PartialEq, Serialize, Deserialize)]
pub struct Launcher {
    /// The launcher's own link that starts the game (heroic://launch/..., lutris:rungame/...).
    pub url: String,
    /// The prefix folder the launcher set (Proton keeps the Wine prefix in a `pfx` folder inside it).
    pub prefix: PathBuf,
    /// The launcher's runner, when it is not Proton-based: plain Wine gives games no OpenXR.
    pub plain_wine: Option<String>,
    /// The launcher's settings file for the game, which holds its environment variables.
    pub config: PathBuf,
}

fn unity() -> String {
    "Unity".into()
}

impl Game {
    pub fn unity(&self) -> bool {
        self.engine == "Unity"
    }

    /// An Unreal game with its Shipping exe found: full VR, the core loaded beside that exe.
    pub fn unreal_vr(&self) -> bool {
        self.engine == "Unreal" && self.unreal_exe.is_some()
    }

    /// The VR core runs inside the game: its per-game settings and button layout apply.
    pub fn core_vr(&self) -> bool {
        self.unity() || self.godot_core() || self.unreal_vr()
    }

    /// A Godot 4 game: full VR (override.cfg and PortXR beside the exe).
    pub fn godot_vr(&self) -> bool {
        self.engine == "Godot" && self.unity.starts_with("4.")
    }

    /// Godot 4.3 and later (the GDExtension's minimum): the shared VR core, as Unity games have it. Older Godot 4 games,
    /// and builds without Direct3D 12, take their VR from the engine's own OpenXR.
    pub fn godot_core(&self) -> bool {
        self.godot_vr() && !self.no_d3d12 && self.unity.split('.').nth(1).and_then(|m| m.parse::<u32>().ok()).is_some_and(|m| m >= 3)
    }

    /// This game's settings file while VR is added: the Unity plugin's BepInEx config, or the Godot core's beside it.
    pub fn config_rel(&self) -> String {
        let exe = match &self.unreal_exe {
            Some(exe) if self.unreal_vr() => exe,
            _ if self.godot_core() => &self.exe,
            _ => return crate::installer::CONFIG.into(),
        };
        let dir = exe.parent().unwrap_or(&self.dir);
        let rel = dir.strip_prefix(&self.dir).map(|p| p.to_string_lossy().replace('/', "\\")).unwrap_or_default();
        if rel.is_empty() { r"PortXR\portxr.cfg".into() } else { format!(r"{rel}\PortXR\portxr.cfg") }
    }

    pub fn config_file(&self) -> PathBuf {
        self.file(&self.config_rel())
    }

    /// Where VR writes what happened (script errors included): BepInEx's log in Unity games, the core's beside its
    /// settings in Godot and Unreal games.
    pub fn log_file(&self) -> PathBuf {
        if self.unity() { self.file(r"BepInEx\LogOutput.log") } else { self.config_file().with_file_name("portxr.log") }
    }

    pub fn compat(&self) -> Compat {
        if self.blocker.is_some() {
            Compat::Blocked
        } else if self.native_vr {
            Compat::NativeVr
        } else if self.linux_build {
            Compat::LinuxBuild
        } else if self.godot_vr() || self.unreal_vr() {
            Compat::Supported
        } else if !self.unity() {
            Compat::Theater
        } else if self.x86 {
            Compat::X86
        } else if self.stripped {
            Compat::Stripped
        } else {
            Compat::Supported
        }
    }

    pub fn badge(&self) -> &'static str {
        // What the player gets in VR: inside the game, the game on a screen, or why neither.
        match self.compat() {
            Compat::Theater => "VR screen",
            _ if self.usable() => "Full VR",
            Compat::Supported => "Full VR",
            Compat::Blocked => self.blocker.unwrap_or("Anti-cheat"),
            Compat::NativeVr => "Own VR mode",
            Compat::LinuxBuild => "Linux build",
            Compat::X86 => "32-bit",
            Compat::Stripped => "Trimmed engine",
        }
    }

    /// Why a game PortXR would normally leave alone can still get VR if the player insists, and what to expect;
    /// None for games that are fine, and for those it must never touch (anti-cheat, 32-bit, the Linux build).
    pub fn caution(&self) -> Option<&'static str> {
        match self.compat() {
            Compat::Blocked if self.blocker == Some("Multiplayer only") => Some(
                "Play only offline or in private modes: mods online can get accounts banned.",
            ),
            Compat::NativeVr => Some("Start the game flat, not in its own VR mode, or the two clash."),
            Compat::Stripped => Some("The mod loader most likely won't start, and the game runs flat."),
            _ => None,
        }
    }

    /// PortXR runs in it: supported, or VR added anyway after the caution.
    pub fn usable(&self) -> bool {
        matches!(self.compat(), Compat::Supported | Compat::Theater) || (self.caution().is_some() && self.installed())
    }

    pub fn backend(&self) -> &'static str {
        if self.il2cpp { "IL2CPP" } else { "Mono" }
    }

    /// The engine and build, as the game page and the log export name them.
    pub fn engine_label(&self) -> String {
        if self.unity() {
            format!("Unity {}, {}", self.unity, self.backend())
        } else if self.unity.is_empty() {
            self.engine.clone()
        } else {
            format!("{} {}", self.engine, self.unity)
        }
    }

    pub fn installed(&self) -> bool {
        installer::receipt_path(&self.dir).exists()
    }

    /// A file in the game folder, by a backslash path as the receipt and the plugin name them.
    pub fn file(&self, rel: &str) -> PathBuf {
        native(&self.dir, rel)
    }

    /// The link that starts the game through its launcher: Steam's, Heroic's or Lutris's.
    pub fn launch_url(&self) -> Option<String> {
        if self.app_id > 0 { Some(format!("steam://rungameid/{}", self.app_id)) } else { self.launcher.as_ref().map(|l| l.url.clone()) }
    }

    /// The launcher that starts the game, as the page names it.
    pub fn launcher_name(&self) -> &'static str {
        match &self.launcher {
            _ if self.app_id > 0 => "Steam",
            Some(l) if l.url.starts_with("heroic:") => "Heroic",
            Some(_) => "Lutris",
            None => "its launcher",
        }
    }

    pub fn launch(&self) -> io::Result<()> {
        if let Some(url) = self.launch_url() {
            open(url)
        } else if cfg!(windows) {
            Command::new(&self.exe).current_dir(&self.dir).spawn().map(|_| ())
        } else {
            Err(io::Error::other(format!("Outside Steam, Heroic and Lutris, start the game from its launcher with {WINE_DLL} set.")))
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

/// Wine loads the game folder's own winhttp.dll (Unity's mod loader) and dwmapi.dll (the Unreal core) only when told;
/// a game without one keeps Wine's. PortXR tells the prefix of each game it knows (see `wine_overrides`); a game
/// started some other way needs this in its environment. Godot games load PortXR themselves.
pub const WINE_DLL: &str = "WINEDLLOVERRIDES=\"winhttp,dwmapi=n,b\"";
/// With WiVRn or Monado, Steam's Linux Runtime hands the game the OpenXR runtime only with this in its environment,
/// which no prefix setting can give.
pub const OPENXR_ENV: &str = "PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1";
pub const LAUNCH_OPTIONS: &str = "PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%";

// The labels are a handful of short strings, read once per start.
fn leak<'de, D: Deserializer<'de>>(d: D) -> Result<&'static str, D::Error> {
    Ok(String::deserialize(d)?.leak())
}

fn leak_opt<'de, D: Deserializer<'de>>(d: D) -> Result<Option<&'static str>, D::Error> {
    Ok(Option::<String>::deserialize(d)?.map(|s| &*s.leak()))
}

fn library_cache() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("library.json")
}

/// The last scan's games, shown at start while the new scan runs (Play checks the folder again before anything runs).
pub fn cached() -> Vec<Game> {
    // The labels make Game deserialize only from 'static data; the file is small and read once.
    let bytes: &'static [u8] = fs::read(library_cache()).unwrap_or_default().leak();
    let games: Vec<Game> = serde_json::from_slice(bytes).unwrap_or_default();
    games.into_iter().filter(|g| g.exe.exists() && g.engine != "Other").collect()
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
        engine: unity(),
        name: product.filter(|p| !p.is_empty()).unwrap_or_else(|| root.file_name().unwrap_or_default().to_string_lossy().into_owned()),
        unity: unity_version(&data_dir(&exe)).unwrap_or_else(|| "unknown".into()),
        il2cpp,
        x86: is_x86(&exe).unwrap_or(false),
        linux_build: exe.extension().is_some_and(|x| x == "x86_64"),
        native_vr: false,
        no_d3d12: false,
        unreal_exe: None,
        stripped: !il2cpp && is_stripped(&dir, &data_dir(&exe).join("Managed")),
        blocker,
        other_loader,
        app_id: 0,
        cover: None,
        added: None,
        store: "",
        last_played: 0,
        launcher: None,
        dir,
        exe,
    })
}

/// An Unreal or Godot game: only in folders a store or the player names as a game (never the "On disk" sweep, which
/// would list every program). The exe is the one a launch without a store would start.
pub fn detect_other(root: &Path) -> Option<Game> {
    let mut exes = Vec::new();
    collect_exes(root, 3, &mut exes);
    // The Win64 one is the game; another -Shipping exe can be its launcher (WARDOGS).
    let ends = |p: &PathBuf, s: &str| p.to_string_lossy().ends_with(s);
    let shipping = exes
        .iter()
        .find(|p| ends(p, "-Win64-Shipping.exe"))
        .or_else(|| exes.iter().find(|p| ends(p, "-Shipping.exe") || ends(p, "-Linux-Shipping")))
        .cloned();
    let godot = exes.iter().find(|p| p.with_extension("pck").exists() || ends_with_pck(p)).cloned();
    // Shallowest first, then largest: the game rather than a tool in a subfolder.
    let depth = |p: &PathBuf| p.strip_prefix(root).map(|r| r.components().count()).unwrap_or(9);
    let size = |p: &PathBuf| fs::metadata(p).map(|m| m.len()).unwrap_or(0);
    let main = exes.iter().min_by_key(|p| (depth(p), std::cmp::Reverse(size(p)))).cloned();
    let (engine, exe) = if shipping.is_some() || root.join("Engine").is_dir() {
        ("Unreal", main.or(shipping.clone())?)
    } else {
        // Another engine: PortXR has nothing to add.
        ("Godot", godot?)
    };
    let names = |d: &Path| -> Vec<String> {
        fs::read_dir(d).into_iter().flatten().flatten().map(|e| e.file_name().to_string_lossy().to_lowercase()).collect()
    };
    let mut files = names(root);
    for e in &exes {
        if let Some(d) = e.parent().filter(|d| *d != root) {
            files.extend(names(d));
        }
    }
    let version = if engine == "Godot" { godot_version(&exe).unwrap_or_default() } else { String::new() };
    let linux_build = is_linux_binary(&exe);
    let no_d3d12 = !linux_build && version.starts_with("4.") && !fs::read(&exe).is_ok_and(|b| b.windows(17).any(|w| w == b"D3D12CreateDevice"));
    let game = Game {
        name: root.file_name().unwrap_or_default().to_string_lossy().into_owned(),
        engine: engine.into(),
        // For Godot, its engine version (from the game's pack): Godot 4 games get full VR.
        unity: version,
        il2cpp: false,
        x86: false,
        stripped: false,
        linux_build,
        native_vr: false,
        no_d3d12,
        unreal_exe: shipping.filter(|p| engine == "Unreal" && ends(p, "-Win64-Shipping.exe")),
        blocker: anti_cheat_files(&files),
        other_loader: None,
        app_id: 0,
        cover: None,
        added: None,
        store: "",
        last_played: 0,
        launcher: None,
        dir: root.to_path_buf(),
        exe,
    };
    // Linux: the VR screen captures windows through Windows, so only the games PortXR goes inside. A Linux build of
    // one is listed too, so the page can say to force Proton for it.
    (cfg!(windows) || game.godot_core() || game.unreal_vr() || (linux_build && game.engine == "Unreal")).then_some(game)
}

/// A Linux build's game binary: Godot's `.x86_64` export, Unreal's `<Project>-Linux-Shipping`.
fn is_linux_binary(p: &Path) -> bool {
    let name = p.file_name().unwrap_or_default().to_string_lossy();
    name.ends_with(".x86_64") || name.ends_with("-Linux-Shipping")
}

// Windows exes up to `depth` folders down, minus installers, crash reporters, anti-cheat and developer tools. On Linux
// also a Linux build's binary, which Steam installs where a game has one.
fn collect_exes(dir: &Path, depth: u32, out: &mut Vec<PathBuf>) {
    const NOT_GAME: [&str; 16] = ["unins", "setup", "redist", "vc_", "dxsetup", "crash", "dotnet", "directx", "ue4prereq",
                                   "ueprereq", "cef", "launcherpatcher", "anticheat", "notification", "diag", "vconsole"];
    let Ok(entries) = fs::read_dir(dir) else { return };
    let mut dirs = Vec::new();
    for p in entries.flatten().map(|e| e.path()) {
        let name = p.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
        if p.is_dir() {
            // Unreal's Engine folder holds its crash reporter and prerequisites, never the game.
            if !["_commonredist", "redist", "directx", "support", "engine"].contains(&name.as_str()) {
                dirs.push(p);
            }
        } else if (name.ends_with(".exe") || (cfg!(unix) && is_linux_binary(&p))) && !NOT_GAME.iter().any(|n| name.contains(n)) {
            out.push(p);
        }
    }
    for d in dirs {
        if depth > 0 {
            collect_exes(&d, depth - 1, out);
        } else if dir.ends_with("Binaries") && (d.ends_with("Win64") || (cfg!(unix) && d.ends_with("Linux"))) {
            // Unreal's game exe, one folder past the rest (Marvel Rivals: MarvelGame\Marvel\Binaries\Win64).
            collect_exes(&d, 0, out);
        }
    }
}

/// The Godot version a game was exported with, from its pack's header ("GDPC", format, major, minor, patch): the
/// .pck beside the exe, or the pack embedded at the end of the exe (its size in the 8 bytes before the closing magic).
fn godot_version(exe: &Path) -> Option<String> {
    use std::io::{Seek, SeekFrom};
    let (mut f, start) = match File::open(exe.with_extension("pck")) {
        Ok(f) => (f, SeekFrom::Start(0)),
        Err(_) => {
            let mut f = File::open(exe).ok()?;
            let mut size = [0u8; 8];
            f.seek(SeekFrom::End(-12)).ok()?;
            f.read_exact(&mut size).ok()?;
            (f, SeekFrom::End(-12 - i64::from_le_bytes(size)))
        }
    };
    let mut head = [0u8; 20];
    f.seek(start).ok()?;
    f.read_exact(&mut head).ok()?;
    let word = |i: usize| u32::from_le_bytes(head[i..i + 4].try_into().unwrap());
    (&head[..4] == b"GDPC").then(|| format!("{}.{}.{}", word(8), word(12), word(16)))
}

// Godot can embed its game data in the exe: the file then ends with the pack's magic.
fn ends_with_pck(exe: &Path) -> bool {
    use std::io::{Seek, SeekFrom};
    let Ok(mut f) = File::open(exe) else { return false };
    let mut tail = [0u8; 4];
    f.seek(SeekFrom::End(-4)).is_ok() && f.read_exact(&mut tail).is_ok() && &tail == b"GDPC"
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
    dirs::config_dir().unwrap_or_default().join("PortXR").join("games.txt")
}

/// Remembers a picked folder: the game in it, or every game one level inside it (a folder like D:\Games).
/// Returns how many games were added.
pub fn add_folder(dir: &Path) -> io::Result<usize> {
    let roots: Vec<PathBuf> = if detect(dir).is_some() {
        vec![dir.to_path_buf()]
    } else {
        let inside: Vec<PathBuf> = fs::read_dir(dir)?.flatten().map(|e| e.path()).filter(|p| p.is_dir() && (detect(p).is_some() || detect_other(p).is_some())).collect();
        // No game one level in: the folder is one game in another engine (for the Theater screen).
        if inside.is_empty() && detect_other(dir).is_some() { vec![dir.to_path_buf()] } else { inside }
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

/// Install folders other launchers record. Linux: Heroic's (Epic, GOG, Amazon) and Lutris's games, with the Wine prefix
/// each runs in.
#[cfg(not(windows))]
fn other_stores() -> Vec<crate::launchers::Other> {
    crate::launchers::heroic().into_iter().chain(crate::launchers::lutris()).collect()
}

/// Install folders other launchers record: Epic Games manifests and GOG Galaxy's registry entries.
#[cfg(windows)]
fn other_stores() -> Vec<crate::launchers::Other> {
    let mut found = Vec::new();
    let manifests = PathBuf::from(std::env::var("ProgramData").unwrap_or_else(|_| r"C:\ProgramData".into()))
        .join(r"Epic\EpicGamesLauncher\Data\Manifests");
    for item in fs::read_dir(manifests).into_iter().flatten().flatten().map(|e| e.path()) {
        let Ok(json) = fs::read_to_string(&item).map(|t| serde_json::from_str::<serde_json::Value>(&t)) else { continue };
        let Ok(json) = json else { continue };
        if let Some(dir) = json["InstallLocation"].as_str() {
            found.push((PathBuf::from(dir), json["DisplayName"].as_str().map(String::from), "Epic Games", None));
        }
    }
    let hklm = winreg::RegKey::predef(winreg::enums::HKEY_LOCAL_MACHINE);
    let gog = [r"SOFTWARE\WOW6432Node\GOG.com\Games", r"SOFTWARE\GOG.com\Games"].map(|k| hklm.open_subkey(k));
    for key in gog.iter().flatten().flat_map(|k| k.enum_keys().flatten().filter_map(|id| k.open_subkey(id).ok()).collect::<Vec<_>>()) {
        if let Ok(dir) = key.get_value::<String, _>("path") {
            found.push((PathBuf::from(dir), key.get_value::<String, _>("gameName").ok(), "GOG", None));
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
            found.push((dir, None, "On disk", None));
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

/// The headset software games will start in: the active OpenXR runtime's name, read at most every 3 s. None when no
/// runtime is set up, which is the usual reason VR does not start.
pub fn openxr_runtime() -> Option<String> {
    type Seen = Option<(std::time::Instant, Option<String>)>;
    static SEEN: std::sync::Mutex<Seen> = std::sync::Mutex::new(None);
    let mut seen = SEEN.lock().unwrap();
    if let Some(name) = seen.as_ref().filter(|(at, _)| at.elapsed().as_secs() < 3).map(|(_, name)| name.clone()) {
        return name;
    }
    let name = active_runtime_json().map(|json| runtime_name(&json));
    *seen = Some((std::time::Instant::now(), name.clone()));
    name
}

#[cfg(windows)]
fn active_runtime_json() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("XR_RUNTIME_JSON") {
        return Some(PathBuf::from(p));
    }
    let key = winreg::RegKey::predef(winreg::enums::HKEY_LOCAL_MACHINE).open_subkey(r"SOFTWARE\Khronos\OpenXR\1").ok()?;
    key.get_value::<String, _>("ActiveRuntime").ok().map(PathBuf::from).filter(|p| p.is_file())
}

// Where the OpenXR loader looks, in its order: the user's config folder, then each of XDG_CONFIG_DIRS (/etc/xdg when
// unset), then /etc; in each, the file for this CPU before the plain one.
#[cfg(not(windows))]
fn active_runtime_json() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("XR_RUNTIME_JSON") {
        return Some(PathBuf::from(p));
    }
    let set = |v: &str| std::env::var(v).ok().filter(|s| !s.is_empty());
    let home = set("XDG_CONFIG_HOME").map(PathBuf::from).or_else(|| dirs::home_dir().map(|h| h.join(".config")));
    let system = set("XDG_CONFIG_DIRS").unwrap_or_else(|| "/etc/xdg".into());
    let arch = format!("openxr/1/active_runtime.{}.json", std::env::consts::ARCH);
    home.into_iter()
        .chain(system.split(':').filter(|d| !d.is_empty()).map(PathBuf::from))
        .chain([PathBuf::from("/etc")])
        .flat_map(|d| [d.join(&arch), d.join("openxr/1/active_runtime.json")])
        .find(|p| p.is_file())
}

/// Linux: Monado or WiVRn is the runtime, but no OpenVR bridge (xrizer, OpenComposite) is set as the OpenVR runtime.
/// Proton offers OpenXR to a game only through an OpenVR runtime, so the game would start flat.
pub fn needs_openvr_bridge(runtime: &str) -> bool {
    if cfg!(windows) || !matches!(runtime, "Monado" | "WiVRn") {
        return false;
    }
    let config = std::env::var_os("XDG_CONFIG_HOME").filter(|v| !v.is_empty()).map(PathBuf::from).or_else(|| dirs::home_dir().map(|h| h.join(".config")));
    let paths = config.and_then(|d| fs::read_to_string(d.join("openvr/openvrpaths.vrpath")).ok()).and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok());
    let first = paths.as_ref().and_then(|v| v["runtime"].get(0)).and_then(|r| r.as_str()).unwrap_or_default().to_lowercase();
    first.is_empty() || first.contains("steamvr")
}

// Known runtimes by their install path (their manifests name themselves inconsistently), else the manifest's name.
fn runtime_name(json: &Path) -> String {
    let path = json.to_string_lossy().to_lowercase();
    let known = [("steamvr", "SteamVR"), ("virtual desktop", "Virtual Desktop"), ("virtualdesktop", "Virtual Desktop"), ("oculus", "Meta Quest Link"),
        ("wivrn", "WiVRn"), ("monado", "Monado"), ("mixedreality", "Windows Mixed Reality"), ("pimax", "Pimax"), ("varjo", "Varjo"), ("alvr", "ALVR")];
    if let Some((_, name)) = known.iter().find(|(k, _)| path.contains(k)) {
        return name.to_string();
    }
    fs::read_to_string(json)
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|v| v["runtime"]["name"].as_str().map(str::to_string))
        .unwrap_or_else(|| json.file_stem().unwrap_or_default().to_string_lossy().into_owned())
}

#[cfg(windows)]
fn steam_root() -> Option<PathBuf> {
    let key = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER).open_subkey(r"Software\Valve\Steam").ok()?;
    key.get_value::<String, _>("SteamPath").ok().map(PathBuf::from)
}

// The native package, the old ~/.steam link, the Flatpak and the Snap.
#[cfg(not(windows))]
fn steam_root() -> Option<PathBuf> {
    let home = dirs::home_dir()?;
    [".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam", "snap/steam/common/.local/share/Steam"]
        .iter()
        .map(|rel| home.join(rel))
        .find(|p| p.join("steamapps").is_dir())
}

/// Linux: the Wine prefix the game runs in, the folder holding its registry (user.reg): Steam's
/// `steamapps/compatdata/<id>/pfx` beside the game's library, or the one its launcher set. Proton keeps the prefix in
/// a `pfx` folder inside the one it is given; Wine uses the folder itself.
pub fn wine_prefix(g: &Game) -> Option<PathBuf> {
    if cfg!(windows) {
        return None;
    }
    if let Some(l) = &g.launcher {
        return Some(if l.prefix.join("user.reg").exists() { l.prefix.clone() } else { l.prefix.join("pfx") });
    }
    if g.app_id == 0 {
        return None;
    }
    let steamapps = g.dir.ancestors().find(|a| a.file_name().is_some_and(|n| n == "steamapps"))?;
    Some(steamapps.join("compatdata").join(g.app_id.to_string()).join("pfx"))
}

/// The game's user folder as the game sees it (`%APPDATA%` with `roaming`, else `LocalLow`): on Linux, inside the
/// game's Wine prefix, under Proton's steamuser (or, with plain Wine, the login name).
fn game_user_dir(g: &Game, roaming: bool) -> Option<PathBuf> {
    if cfg!(windows) {
        return if roaming { dirs::config_dir() } else { Some(dirs::data_local_dir()?.parent()?.join("LocalLow")) };
    }
    let users = wine_prefix(g)?.join("drive_c/users");
    let login = std::env::var("USER").unwrap_or_default();
    let user = if !login.is_empty() && !users.join("steamuser").exists() && users.join(&login).is_dir() { login } else { "steamuser".into() };
    Some(users.join(user).join("AppData").join(if roaming { "Roaming" } else { "LocalLow" }))
}

/// The DLL the game's prefix must load from the game folder rather than Wine's own: Unity's mod loader is winhttp,
/// the Unreal core dwmapi. Godot games load PortXR themselves.
fn override_dll(g: &Game) -> Option<&'static str> {
    if g.unreal_vr() { Some("dwmapi") } else if g.unity() { Some("winhttp") } else { None }
}

/// Linux: tells the game's Wine prefix to load PortXR's loader from the game folder (Add VR and Play in VR). The Err
/// says plainly what the player does next: start the game once so the prefix exists, or close it first.
pub fn wine_overrides(g: &Game) -> Result<(), String> {
    let Some(dll) = override_dll(g).filter(|_| !cfg!(windows)) else { return Ok(()) };
    let Some(pfx) = wine_prefix(g) else {
        return Err(match &g.launcher {
            Some(_) => format!("Set a Wine prefix for this game in {}, start it once, then press Play in VR.", g.launcher_name()),
            None => format!("Start the game from its launcher with {WINE_DLL} set."),
        });
    };
    let wine = if g.launcher.as_ref().is_some_and(|l| l.plain_wine.is_some()) { "Wine" } else { "Proton" };
    crate::proton::enable(&pfx, dll, &g.dir).map_err(|why| match why {
        crate::proton::Why::NoPrefix => format!("{wine} sets this game up on its first start. Start it once from {}, close it, then press Play in VR.", g.launcher_name()),
        crate::proton::Why::Running => "The game is running. Close it, then press Play in VR.".into(),
        crate::proton::Why::Io(e) => format!("Couldn't set up the game's Wine prefix: {e}"),
    })
}

/// Linux with WiVRn or Monado: the game's launch options (Steam) or environment variables (Heroic, Lutris) lack
/// OPENXR_ENV, so the game would start flat. Asked every frame by the game page, so Steam's settings (every account's)
/// are read at most every two seconds; a launcher's file holds one game.
pub fn needs_launch_options(g: &Game) -> bool {
    if cfg!(windows) || !openxr_runtime().is_some_and(|r| matches!(r.as_str(), "Monado" | "WiVRn")) {
        return false;
    }
    if g.app_id == 0 {
        // The launcher's settings file for the game.
        return g.launcher.as_ref().is_some_and(|l| !fs::read_to_string(&l.config).is_ok_and(|t| t.contains("PRESSURE_VESSEL_IMPORT_OPENXR")));
    }
    // When it was read, and each app's launch options.
    type Saved = Option<(std::time::Instant, Vec<(u32, String)>)>;
    static SAVED: std::sync::Mutex<Saved> = std::sync::Mutex::new(None);
    let mut saved = SAVED.lock().unwrap();
    if saved.as_ref().is_none_or(|(at, _)| at.elapsed().as_secs() >= 2) {
        let options = steam_root().map(|s| localconfig_values(&s, "LaunchOptions")).unwrap_or_default();
        *saved = Some((std::time::Instant::now(), options));
    }
    !saved.as_ref().unwrap().1.iter().any(|(id, v)| *id == g.app_id && v.contains("PRESSURE_VESSEL_IMPORT_OPENXR"))
}

fn played_list() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("played.txt")
}

/// Records a launch from the app (`<unix seconds>\t<game dir>` lines) for the library's "Recently played" order.
/// When VR last started in this game: the plugin notes its first frame sent to the headset beside itself.
pub fn last_vr(g: &Game) -> Option<std::time::SystemTime> {
    // The Unity plugin's, or the Godot and Unreal core's beside its settings.
    let file = if g.unity() { g.file(r"BepInEx\plugins\PortXR\played.txt") } else { g.config_file().with_file_name("played.txt") };
    fs::metadata(file).and_then(|m| m.modified()).ok()
}

fn vr_list_path() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("vr-games.txt")
}

/// The games with VR added, for the VR menu's Library tab: the app's path on the first line ("app<TAB><exe>"), then
/// one "name<TAB>folder<TAB>cover<TAB>link" line per game, most recently played first. The link starts the game
/// through its launcher (Steam, Heroic, Lutris), for games under Wine, which can't start the app. Written on every
/// scan and change; a running game reads it when the tab opens.
pub fn write_vr_list(games: &[Game]) {
    let mut text = format!("app\t{}\n", std::env::current_exe().map(|p| p.display().to_string()).unwrap_or_default());
    let mut vr: Vec<&Game> = games.iter().filter(|g| g.installed() && g.usable()).collect();
    vr.sort_by(|a, b| b.last_played.cmp(&a.last_played).then_with(|| a.name.cmp(&b.name)));
    for g in vr {
        // The cover (Steam's cached art) is optional: the VR menu shows the initial without one.
        let cover = g.cover.as_ref().map(|c| c.display().to_string()).unwrap_or_default();
        text += &format!("{}\t{}\t{}\t{}\n", g.name.replace('\t', " "), g.dir.display(), cover, g.launch_url().unwrap_or_default());
    }
    let path = vr_list_path();
    if !fs::read_to_string(&path).is_ok_and(|old| old == text) {
        let _ = fs::create_dir_all(path.parent().unwrap());
        let _ = fs::write(path, text);
    }
    share_with_prefixes(games);
}

/// A Linux path as a game in the prefix whose drive C is `drive_c` sees it: C:\ inside that drive, Z:\ (Wine's drive
/// for the whole file system) anywhere else.
fn wine_path(path: &str, drive_c: &Path) -> String {
    if path.is_empty() {
        return String::new();
    }
    match Path::new(path).strip_prefix(drive_c) {
        Ok(rel) => format!("C:\\{}", rel.to_string_lossy().replace('/', "\\")),
        Err(_) => format!("Z:{}", path.replace('/', "\\")),
    }
}

/// The VR menu's list as a game in a Wine prefix reads it: folders and covers as Wine paths, and no app line (the
/// Linux app can't be started from inside Wine; the menu starts games by their links instead).
fn wine_list(text: &str, drive_c: &Path) -> String {
    let mut out = String::new();
    for line in text.lines().filter(|l| !l.starts_with("app\t")) {
        let mut f: Vec<String> = line.split('\t').map(String::from).collect();
        for p in f.iter_mut().skip(1).take(2) {
            *p = wine_path(p, drive_c);
        }
        out += &(f.join("\t") + "\n");
    }
    out
}

/// Linux: the in-game core reads the all-games defaults and the theme from %APPDATA%\PortXR inside the game's Proton
/// prefix, and the VR menu changes them there. So each copy is kept in step with the app's, the newer one winning,
/// for every game with VR whose prefix exists. The VR menu's game list goes there too, in Wine's paths.
/// Linux: takes out the copies `share_with_prefixes` keeps in each game's Wine prefix (`--uninstall`).
pub fn clear_prefixes(games: &[Game]) {
    if cfg!(windows) {
        return;
    }
    for dir in games.iter().filter_map(|g| game_user_dir(g, true)).map(|d| d.join("PortXR")) {
        for name in ["defaults.cfg", "theme.txt", "vr-games.txt"] {
            let _ = fs::remove_file(dir.join(name));
        }
        // Only when nothing else is in it (the game's own files there stay).
        let _ = fs::remove_dir(&dir);
    }
}

pub fn share_with_prefixes(games: &[Game]) {
    if cfg!(windows) {
        return;
    }
    let app = dirs::config_dir().unwrap_or_default().join("PortXR");
    let changed = |p: &Path| fs::metadata(p).and_then(|m| m.modified()).ok();
    let list = fs::read_to_string(vr_list_path()).unwrap_or_default();
    for g in games.iter().filter(|g| g.installed()) {
        let Some(prefix) = game_user_dir(g, true).filter(|d| d.is_dir()).map(|d| d.join("PortXR")) else { continue };
        if let Some(pfx) = wine_prefix(g) {
            let text = wine_list(&list, &pfx.join("drive_c"));
            let to = prefix.join("vr-games.txt");
            if !fs::read_to_string(&to).is_ok_and(|old| old == text) {
                let _ = fs::create_dir_all(&prefix).and_then(|_| fs::write(to, text));
            }
        }
        for name in ["defaults.cfg", "theme.txt"] {
            let (ours, theirs) = (app.join(name), prefix.join(name));
            let (from, to) = match (changed(&ours), changed(&theirs)) {
                (Some(a), Some(b)) if b > a => (&theirs, &ours),
                (Some(_), _) => (&ours, &theirs),
                (None, Some(_)) => (&theirs, &ours),
                (None, None) => continue,
            };
            let Ok(text) = fs::read(from) else { continue };
            if fs::read(to).is_ok_and(|old| old == text) {
                continue;
            }
            let _ = fs::create_dir_all(to.parent().unwrap()).and_then(|_| fs::write(to, text));
        }
    }
}

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
    // Newer clients name them library_capsule.jpg and library_header.jpg.
    ["library_600x900.jpg", "library_capsule.jpg", "header.jpg", "library_header.jpg"].into_iter().find_map(find)
}

/// The game's Lua scripts folder, where the plugin or core looks: `%APPDATA%\PortXR\scripts\<exe name>` (keyed by
/// the process's name: for Unreal, the Shipping exe). Outside the game folder, so scripts survive an uninstall.
pub fn scripts_dir(g: &Game) -> PathBuf {
    let exe = g.unreal_exe.as_ref().filter(|_| g.unreal_vr()).unwrap_or(&g.exe);
    game_user_dir(g, true).or_else(dirs::config_dir).unwrap_or_default().join("PortXR").join("scripts").join(exe.file_stem().unwrap_or_default())
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
    save_script(g, &file.file_name().unwrap_or_default().to_string_lossy(), &fs::read(file)?)
}

/// Writes a script into the game's scripts folder under this name (imported, or from the catalogue).
pub fn save_script(g: &Game, name: &str, bytes: &[u8]) -> io::Result<PathBuf> {
    // Linux: the scripts live in the game's Wine prefix, which Proton makes on the first start; making its folders
    // first would leave Proton a half-made prefix.
    if let Some(pfx) = wine_prefix(g)
        && !pfx.join("system.reg").exists() {
            return Err(io::Error::other(format!("Start the game once from {} first, so it sets up its Windows folder.", g.launcher_name())));
        }
    let dir = scripts_dir(g);
    fs::create_dir_all(&dir)?;
    let to = dir.join(name);
    fs::write(&to, bytes)?;
    Ok(to)
}

/// A game in the Steam library, installed or not, that gets full VR (a Unity, Unreal or Godot game PortXR can run).
#[derive(Clone)]
pub struct Owned {
    pub app_id: u32,
    pub name: String,
    pub cover: Option<PathBuf>,
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
    /// common/type: "Game", "Demo", "Tool", "Application", ...
    kind: String,
    name: String,
    /// common/steam_release_date, Unix seconds (0 if unknown).
    release: u64,
    /// The engine its Steam Cloud save folders give away (ufs/savefiles), installed or not.
    engine: Option<&'static str>,
    /// Online PvP (category 36), and the store's user tags, most voted first (common/store_tags).
    online_pvp: bool,
    tags: Vec<(u32, u32)>,
}

/// Steam tags of competitive play: PvP, Competitive, Team-Based, eSports, Massively Multiplayer, Battle Royale, MOBA, Hero Shooter.
const COMPETITIVE_TAGS: [u32; 8] = [1775, 3878, 5711, 5055, 128, 176981, 1718, 620519];
const SINGLEPLAYER_TAG: u32 = 4182;

/// The engine a Steam Cloud save folder belongs to: Unity saves under AppData\LocalLow (Windows), unity3d/ (Linux) or
/// unity.<company>.<game> (Mac); Unreal under <project>/Saved/SaveGames or Config; Godot under Godot/app_userdata. Matched
/// every engine Wikidata names in a 1,050-game library (61 of 61) and found 107 more.
fn save_engine(key: &str, value: &str) -> Option<&'static str> {
    if key == "root" {
        return (value == "WinAppDataLocalLow").then_some("Unity");
    }
    if key != "path" {
        return None;
    }
    let v = format!("/{}", value.replace('\\', "/").to_lowercase());
    if v.contains("/saved/savegames") || v.contains("/saved/config") {
        Some("Unreal")
    } else if v.contains("godot/app_userdata") {
        Some("Godot")
    } else if v.contains("/unity3d/") || v.split('/').any(|seg| seg.starts_with("unity.") && seg.matches('.').count() >= 2) {
        Some("Unity")
    } else {
        None
    }
}

impl Facts {
    fn blocker(&self) -> Option<&'static str> {
        if self.anti_cheat {
            Some("Anti-cheat")
        } else if self.multiplayer && !self.single_player {
            // Apps with no player modes listed at all (tools, overlays) are not called multiplayer.
            Some("Multiplayer only")
        } else if self.online_pvp && self.competitive_first() {
            Some("Competitive online")
        } else {
            None
        }
    }
}

impl Facts {
    /// Players rank a competitive tag among its top five, above Singleplayer: a multiplayer game with a token
    /// single-player mode (bots, training), where a ban costs the most. Co-op and campaign-first games stay.
    fn competitive_first(&self) -> bool {
        let mut tags = self.tags.clone();
        tags.sort();
        let at = |pred: &dyn Fn(u32) -> bool| tags.iter().position(|&(_, t)| pred(t)).unwrap_or(99);
        let comp = at(&|t| COMPETITIVE_TAGS.contains(&t));
        comp < 5 && comp < at(&|t| t == SINGLEPLAYER_TAG)
    }
}

fn steam_facts(steam: &Path, ids: &std::collections::HashSet<u32>) -> std::collections::HashMap<u32, Facts> {
    let mut out = std::collections::HashMap::new();
    let Ok(b) = fs::read(steam.join("appcache").join("appinfo.vdf")) else { return out };
    let anti_cheat = anti_cheat_games();
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
                ["anticheat", "anti-cheat", "anti_cheat", "battleye", "start_protected_game", "_eac", "eac_", "eac runtime", "_be.exe"]
                    .iter()
                    .any(|n| s.contains(n))
            };
            found.anti_cheat |= names_ac(&k);
            if path.len() == 3 && path[2] == "category" {
                match k.as_str() {
                    "category_2" => found.single_player = true,
                    "category_1" | "category_9" | "category_20" | "category_27" | "category_38" | "category_47" | "category_48" | "category_49" => found.multiplayer = true,
                    "category_8" => found.anti_cheat = true,
                    "category_31" | "category_53" | "category_54" => found.vr = true,
                    "category_36" => (found.multiplayer, found.online_pvp) = (true, true),
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
                    // Unreal and Godot folders are telling; LocalLow is Unity's but not only Unity's.
                    if path.get(1).is_some_and(|p| p == "ufs") && found.engine.is_none_or(|e| e == "Unity") {
                        found.engine = save_engine(&k, &v).or(found.engine);
                    }
                    if path.len() == 2 && path[1] == "common" && k == "osarch" {
                        found.osarch = v;
                    } else if path.len() == 2 && path[1] == "common" && k == "type" {
                        found.kind = v.to_lowercase();
                    } else if path.len() == 2 && path[1] == "common" && k == "name" {
                        found.name = v;
                    } else if path.len() == 2 && path[1] == "common" && k == "steam_release_date" {
                        found.release = v.parse().unwrap_or(0);
                    }
                }
                2 | 3 => {
                    let v = u32::from_le_bytes(b.get(*p..*p + 4)?.try_into().ok()?);
                    if path.len() == 3 && path[2] == "store_tags" {
                        found.tags.push((k.parse().unwrap_or(99), v));
                    }
                    if path.len() == 2 && path[1] == "common" && k == "osarch" {
                        found.osarch = v.to_string();
                    } else if path.len() == 2 && path[1] == "common" && k == "steam_release_date" {
                        found.release = v.into();
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
                found.anti_cheat |= anti_cheat.contains(&id);
                out.insert(id, found);
            }
        }
        p = next;
    }
    out
}

fn anti_cheat_list() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("anticheat-steam.json")
}

/// Steam app IDs of games with anti-cheat (Easy Anti-Cheat, BattlEye, EQU8, ...) from the community-kept Are We
/// Anti-Cheat Yet? list: Steam's own data names it for few of them. The copy built in holds offline and on a first
/// start; the cache, which the library refreshes weekly off the UI thread, adds games listed since. The whole list is
/// downloaded, so nothing about the player's library leaves the machine.
fn anti_cheat_games() -> std::collections::HashSet<u32> {
    let built_in: Vec<u32> = serde_json::from_str(include_str!("../assets/anticheat-steam.json")).unwrap_or_default();
    let fetched: Vec<u32> = fs::read(anti_cheat_list()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
    built_in.into_iter().chain(fetched).collect()
}

fn refresh_anti_cheat() {
    let path = anti_cheat_list();
    let fresh = fs::metadata(&path).and_then(|m| m.modified()).ok().and_then(|t| t.elapsed().ok()).is_some_and(|age| age.as_secs() < 7 * 86400);
    if fresh {
        return;
    }
    let url = "https://raw.githubusercontent.com/AreWeAntiCheatYet/AreWeAntiCheatYet/HEAD/games.json";
    let Some(json) = ureq::get(url)
        .header("User-Agent", "PortXR (VR injector; anti-cheat list)")
        .call()
        .ok()
        .and_then(|mut r| r.body_mut().with_config().limit(16 << 20).read_json::<serde_json::Value>().ok())
    else {
        return;
    };
    let ids: Vec<u32> = json
        .as_array()
        .into_iter()
        .flatten()
        .filter(|g| g["anticheats"].as_array().is_some_and(|a| !a.is_empty()))
        .filter_map(|g| g["storeIds"]["steam"].as_str()?.parse().ok())
        .collect();
    if !ids.is_empty() {
        let _ = fs::create_dir_all(path.parent().unwrap());
        let _ = fs::write(&path, serde_json::to_vec(&ids).unwrap_or_default());
    }
}

fn engine_list() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("engines-steam.json")
}

/// Steam app ID to (name, engine) for every game Wikidata knows in an engine PortXR goes inside: Unity, Unreal
/// Engine 4 or 5, Godot. The whole list is downloaded, so nothing about the player's library leaves the machine; it is
/// cached for a week.
fn engine_games() -> Option<std::collections::HashMap<u32, (String, String)>> {
    let path = engine_list();
    // The Unity-only list it replaces.
    let _ = fs::remove_file(path.with_file_name("unity-steam.json"));
    let fresh = fs::metadata(&path).and_then(|m| m.modified()).ok().and_then(|t| t.elapsed().ok()).is_some_and(|age| age.as_secs() < 7 * 86400);
    if !fresh {
        // QLever answers in under a second; the Wikidata Query Service is the fallback (it rate-limits during outages).
        let query = "PREFIX wdt: <http://www.wikidata.org/prop/direct/> PREFIX wd: <http://www.wikidata.org/entity/> \
                     PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#> SELECT ?steam ?gLabel ?e WHERE { \
                     VALUES ?e { wd:Q63966 wd:Q13156652 wd:Q94277753 wd:Q16972633 } ?g wdt:P408 ?e; wdt:P1733 ?steam. \
                     ?g rdfs:label ?gLabel FILTER(LANG(?gLabel) = \"en\") }";
        let fetched = ["https://qlever.dev/api/wikidata", "https://query.wikidata.org/sparql"].iter().find_map(|url| {
            ureq::get(*url)
                .query("query", query)
                .header("User-Agent", "PortXR (VR injector; engine game list)")
                .header("Accept", "application/sparql-results+json")
                .call()
                .ok()
                .and_then(|mut r| r.body_mut().with_config().limit(64 << 20).read_json::<serde_json::Value>().ok())
        });
        if let Some(json) = fetched {
            let mut map = serde_json::Map::new();
            for row in json["results"]["bindings"].as_array().into_iter().flatten() {
                let engine = match row["e"]["value"].as_str().and_then(|e| e.rsplit('/').next()) {
                    Some("Q63966") => "Unity",
                    Some("Q16972633") => "Godot",
                    _ => "Unreal",
                };
                if let (Some(id), Some(name)) = (row["steam"]["value"].as_str(), row["gLabel"]["value"].as_str()) {
                    map.insert(id.into(), serde_json::json!([name, engine]));
                }
            }
            if !map.is_empty() {
                let _ = fs::create_dir_all(path.parent().unwrap());
                let _ = fs::write(&path, serde_json::to_vec(&map).unwrap_or_default());
            }
        }
    }
    let map: std::collections::HashMap<String, (String, String)> = serde_json::from_slice(&fs::read(&path).ok()?).ok()?;
    Some(map.into_iter().filter_map(|(id, v)| Some((id.parse().ok()?, v))).collect())
}

/// Steam library games that would get full VR: Steam keeps library art for every app in the library, one folder (or
/// file prefix, on older clients) per app ID; Wikidata's engine list and, for the many games it doesn't know, the
/// app cache's Steam Cloud save folders pick the Unity, Unreal and Godot ones, and the app cache drops what PortXR can't run (anti-cheat, multiplayer only, its own VR mode, 32-bit). The rest of the
/// library is left out: once installed, any game plays on the VR screen. Network: the first call of the week downloads the engine and anti-cheat lists.
pub fn steam_library() -> Vec<Owned> {
    let Some(steam) = steam_root() else { return Vec::new() };
    let known = engine_games().unwrap_or_default();
    refresh_anti_cheat();
    let cache = steam.join("appcache").join("librarycache");
    let ids = fs::read_dir(&cache)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| e.file_name().to_string_lossy().split(['_', '.']).next()?.parse().ok())
        .collect();
    let facts = steam_facts(&steam, &ids);
    let played = steam_last_played(&steam);
    let mut owned: Vec<Owned> = ids
        .into_iter()
        .filter_map(|id| {
            let f = facts.get(&id).filter(|f| f.kind == "game" && f.blocker().is_none() && !f.vr && f.osarch != "32")?;
            let (name, engine) = match known.get(&id) {
                Some((name, engine)) => (name.as_str(), engine.as_str()),
                None => ("", f.engine?),
            };
            // ponytail: Wikidata doesn't say Godot 3 or 4; out before Godot 4 (March 2023) means Godot 3, VR screen only.
            if engine == "Godot" && f.release < 1_677_628_800 {
                return None;
            }
            let name = Some(name.to_string()).filter(|n| !n.is_empty()).unwrap_or_else(|| f.name.clone());
            let last_played = played.get(&id).copied().unwrap_or(0);
            Some(Owned { app_id: id, name, cover: cover_in(&cache.join(id.to_string())), last_played })
        })
        .collect();
    owned.sort_by_key(|o| o.name.to_lowercase());
    owned
}

/// Asks Steam to install a game (its own install dialog opens).
pub fn steam_install(app_id: u32) -> io::Result<()> {
    open(format!("steam://install/{app_id}"))
}

type Found = (PathBuf, u32, Option<String>, &'static str, u64, Option<Launcher>);

/// Every Steam install on this PC: its folder and its appmanifest text.
fn steam_manifests(steam: &Path) -> Vec<(PathBuf, String)> {
    let vdf = fs::read_to_string(steam.join("config").join("libraryfolders.vdf")).unwrap_or_default();
    let mut roots: Vec<PathBuf> = vdf_values(&vdf, "path").map(|p| library_path(steam, PathBuf::from(p))).collect();
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

/// Flatpak Steam's home folder, under the real one: inside its sandbox it is the home folder.
const FLATPAK_STEAM_HOME: &str = ".var/app/com.valvesoftware.Steam";

/// A Steam library as Steam wrote it down. Flatpak Steam writes paths as its sandbox sees them, where its own folder
/// under ~/.var is the home folder, so a library path in the home folder is looked for there first.
fn library_path(steam: &Path, p: PathBuf) -> PathBuf {
    let Some(home) = dirs::home_dir().filter(|_| cfg!(unix)) else { return p };
    let sandbox = home.join(FLATPAK_STEAM_HOME);
    match p.strip_prefix(&home) {
        Ok(rel) if steam.starts_with(&sandbox) && !rel.starts_with(".var") && sandbox.join(rel).exists() => sandbox.join(rel),
        _ => p,
    }
}

/// Every Steam install on this PC: folder, app ID, name, store, last played.
fn steam_installs(steam: &Path) -> Vec<Found> {
    steam_manifests(steam)
        .into_iter()
        .map(|(dir, text)| {
            let id = vdf_values(&text, "appid").next().and_then(|v| v.parse().ok()).unwrap_or(0);
            let last = vdf_values(&text, "LastPlayed").next().and_then(|v| v.parse().ok()).unwrap_or(0);
            (dir, id, vdf_values(&text, "name").next(), "Steam", last, None)
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
    let ids = games.iter().map(id_of).filter(|&id| id > 0).collect();
    let facts = steam_facts(steam, &ids);
    for g in games.iter_mut() {
        let Some(f) = facts.get(&id_of(g)) else { continue };
        // SteamVR, overlays, VTuber and wallpaper apps (many made in Unity): Steam calls them tools or applications.
        if !f.kind.is_empty() && f.kind != "game" && f.kind != "demo" {
            g.engine.clear();
        }
        g.native_vr = f.vr;
        if g.blocker.is_none() {
            g.blocker = f.blocker().or((f.multiplayer && has_cheat_toolkit(g)).then_some("Anti-Cheat Toolkit"));
        }
    }
}

/// One game folder, checked as the library checks it: its files, and Steam's word if Steam installed it.
pub fn inspect(dir: &Path) -> Option<Game> {
    let mut g = detect(dir).or_else(|| detect_other(dir))?;
    // Linux: a game Heroic or Lutris runs, with its prefix and link.
    if !cfg!(windows) {
        g.launcher = other_stores().into_iter().find(|(root, ..)| under(&g.dir, root)).and_then(|(.., l)| l);
    }
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
    found.extend(other_stores().into_iter().map(|(dir, name, store, launcher)| (dir, 0, name, store, 0, launcher)));
    let stores = found.len();
    found.extend(manual().into_iter().map(|dir| (dir, 0, None, "Added by you", 0, None)));
    let played = fs::read_to_string(played_list()).unwrap_or_default();

    let mut games: Vec<Game> = Vec::new();
    for (i, (dir, id, name, store, last, launcher)) in found.into_iter().enumerate() {
        let Some(mut g) = detect(&dir).or_else(|| if store == "On disk" { None } else { detect_other(&dir) }) else { continue };
        if games.iter().any(|other| other.dir == g.dir) {
            continue;
        }
        g.added = (i >= stores).then(|| dir.clone());
        g.store = store;
        let ours = played.lines().filter_map(|l| l.split_once('\t')).filter(|(_, d)| Path::new(d) == g.dir).filter_map(|(t, _)| t.parse().ok()).max();
        g.last_played = last.max(ours.unwrap_or(0));
        g.app_id = id;
        g.launcher = launcher;
        if let Some(name) = name {
            g.name = name;
        }
        g.cover = steam.as_ref().filter(|_| id > 0).and_then(|s| cover_in(&s.join("appcache").join("librarycache").join(id.to_string())));
        games.push(g);
    }
    if let Some(steam) = &steam {
        apply_steam(steam, &mut games);
    }
    games.retain(|g| !g.engine.is_empty());
    games.sort_by_key(|g| g.name.to_lowercase());
    if let Ok(json) = serde_json::to_vec(&games) {
        let _ = fs::create_dir_all(library_cache().parent().unwrap()).and_then(|_| fs::write(library_cache(), json));
    }
    games
}

/// One zip on the desktop for bug reports: the VR log, settings, install receipt and Unity's own Player.log.
pub fn export_logs(g: &Game) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let stem: String = g.name.chars().filter(|c| !r#"<>:"/\|?*"#.contains(*c)).collect();
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)?.as_secs();
    let desktop = dirs::desktop_dir().or_else(dirs::home_dir).ok_or("no desktop folder")?;
    let path = desktop.join(format!("PortXR logs - {stem} - {stamp}.zip"));
    let mut zip = zip::ZipWriter::new(File::create(&path)?);
    let opts = zip::write::SimpleFileOptions::default();
    let files = [
        g.log_file(),
        g.config_file(),
        installer::receipt_path(&g.dir),
    ];
    for file in files.into_iter().chain(player_log(g)) {
        // The game may still be running and holding its logs open; std opens files with full sharing.
        let Ok(bytes) = fs::read(&file) else { continue };
        zip.start_file(file.file_name().unwrap().to_string_lossy(), opts)?;
        zip.write_all(&bytes)?;
    }
    zip.start_file("game.txt", opts)?;
    write!(zip, "{}\r\n{}, {}\r\n", g.name, g.engine_label(), g.badge())?;
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

#[cfg(test)]
mod tests {
    use super::save_engine;
    use std::path::Path;

    #[test]
    fn prefix_list_has_wine_paths() {
        let text = "app\t/opt/PortXR/PortXR\n\
                    Hollow Knight\t/home/u/Games/hk/drive_c/Games/Hollow Knight\t\tlutris:rungame/hollow-knight\n\
                    Lethal Company\t/home/u/.local/share/Steam/steamapps/common/Lethal Company\t/home/u/c.jpg\tsteam://rungameid/1966720\n\
                    Old\t/home/u/old\n";
        let want = "Hollow Knight\tC:\\Games\\Hollow Knight\t\tlutris:rungame/hollow-knight\n\
                    Lethal Company\tZ:\\home\\u\\.local\\share\\Steam\\steamapps\\common\\Lethal Company\tZ:\\home\\u\\c.jpg\tsteam://rungameid/1966720\n\
                    Old\tZ:\\home\\u\\old\n";
        assert_eq!(super::wine_list(text, Path::new("/home/u/Games/hk/drive_c")), want);
    }

    #[test]
    fn anti_cheat_list_is_built_in() {
        // 7 Days to Die (Easy Anti-Cheat), whatever the cache holds.
        assert!(super::anti_cheat_games().contains(&251570));
    }

    #[test]
    fn competitive_games_need_a_top_tag() {
        let facts = |tags: &[u32]| super::Facts { online_pvp: true, tags: tags.iter().enumerate().map(|(i, &t)| (i as u32, t)).collect(), ..Default::default() };
        assert!(facts(&[19, 1775, 4182]).competitive_first()); // PvP above Singleplayer
        assert!(!facts(&[4182, 1775]).competitive_first()); // campaign first
        assert!(!facts(&[1, 2, 3, 4, 5, 1775]).competitive_first()); // PvP only sixth
        assert!(facts(&[1, 2, 5711]).competitive_first()); // no Singleplayer tag at all
    }

    #[test]
    fn save_folders_name_engines() {
        assert_eq!(save_engine("root", "WinAppDataLocalLow"), Some("Unity"));
        assert_eq!(save_engine("path", "Team Cherry/Hollow Knight"), None);
        assert_eq!(save_engine("path", r"FactoryGame\Saved\SaveGames\{64BitSteamID}"), Some("Unreal"));
        assert_eq!(save_engine("path", "Library/Application Support/unity.Studio.Game"), Some("Unity"));
        assert_eq!(save_engine("path", ".config/unity3d/Studio/Game"), Some("Unity"));
        assert_eq!(save_engine("path", "Godot/app_userdata/Bloodthief"), Some("Godot"));
        assert_eq!(save_engine("path", "My Games/Borderlands/SaveData"), None);
    }
}
