// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

use std::error::Error;
use std::fs;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use eframe::egui::{self, Align, Align2, Color32, FontFamily, FontId, Layout, Margin, Painter, Rect, RichText, Sense, Stroke, Ui, pos2, vec2};
use serde::Deserialize;

use crate::catalogue::{self, Catalogue};
use crate::games::{self, Compat, Game};
use crate::installer;
use crate::settings::{self, Def, Kind};

// "Night Lime" tokens in WristXR's graphite greys, shared with the VR menu (Menu.cs): dark stage, one lime accent,
// filled soft cells. Spacing scale 4/8/12/16/24/32/48/64.
const BG: Color32 = Color32::from_rgb(11, 11, 12);
const SURFACE: Color32 = Color32::from_rgb(20, 20, 22);
const RAISED: Color32 = Color32::from_rgb(29, 29, 32);
const LINE: Color32 = Color32::from_rgb(42, 42, 46);
const PRESSED: Color32 = Color32::from_rgb(54, 54, 59);
const PLACEHOLDER: Color32 = Color32::from_rgb(56, 56, 61);
const INK: Color32 = Color32::from_rgb(245, 245, 247);
const INK2: Color32 = Color32::from_rgb(168, 168, 175);
const INK3: Color32 = Color32::from_rgb(136, 136, 143);
// The accent follows the theme (PortXR/theme.txt in %APPDATA%, shared with the VR menu); dark text on it works for all.
const THEMES: [(&str, u32); 8] = [
    ("Lime", 0xC6FF1A),
    ("Mint", 0x5CF2A0),
    ("Aqua", 0x3DF2E0),
    ("Sky", 0x5CC8FF),
    ("Violet", 0xB49AFF),
    ("Pink", 0xFF7ADB),
    ("Amber", 0xFFB627),
    ("White", 0xF5F5F7),
];
static THEME: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

fn rgb(v: u32) -> Color32 {
    Color32::from_rgb((v >> 16) as u8, (v >> 8) as u8, v as u8)
}

fn lime() -> Color32 {
    rgb(THEMES[THEME.load(std::sync::atomic::Ordering::Relaxed)].1)
}

// Pressed fill: the accent a step darker.
fn lime_pressed() -> Color32 {
    let c = lime();
    let d = |v: u8| (f32::from(v) * 0.86) as u8;
    Color32::from_rgb(d(c.r()), d(c.g()), d(c.b()))
}

fn theme_path() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("theme.txt")
}

fn load_theme() {
    let name = fs::read_to_string(theme_path()).unwrap_or_default();
    let i = THEMES.iter().position(|(n, _)| n.eq_ignore_ascii_case(name.trim())).unwrap_or(0);
    THEME.store(i, std::sync::atomic::Ordering::Relaxed);
}

fn set_theme(ctx: &egui::Context, i: usize) -> std::io::Result<()> {
    THEME.store(i.min(THEMES.len() - 1), std::sync::atomic::Ordering::Relaxed);
    style(ctx);
    write(&theme_path(), THEMES[i].0.as_bytes())
}
const ON_LIME: Color32 = Color32::from_rgb(11, 11, 12);
const DANGER: Color32 = Color32::from_rgb(255, 90, 60);
// The catalogue's ratings: fixed colours, never the theme's accent, so Perfect and Playable never look alike.
const OK: Color32 = Color32::from_rgb(92, 242, 160);
const WARN: Color32 = Color32::from_rgb(255, 182, 39);
const DANGER_BG: Color32 = Color32::from_rgb(43, 23, 18);
const DANGER_INK: Color32 = Color32::from_rgb(255, 138, 110);
// Shape and rhythm shared by every control: soft corners, one control height, one uppercase tracking.
const RADIUS: u8 = 12;
const CELL_RADIUS: u8 = 10;
const CONTROL_H: f32 = 44.0;
// Motion (seconds): hovers and presses ease, things that travel take a little longer. Zero when Windows'
// "Animation effects" is off.
const FAST: f32 = 0.16;
// Widest a page's column grows on a big window, so rows and their links stay together.
const PAGE_W: f32 = 1040.0;
const TRACK: f32 = 1.2;
// Type scale, nothing in between: Anton titles, Barlow body and small text, Barlow SemiBold capital labels.
const TITLE: f32 = 52.0;
const HEADING: f32 = 32.0;
const BRAND: f32 = 30.0;
const BODY: f32 = 16.0;
const SMALL: f32 = 14.0;
const LABEL: f32 = 13.0;
const REPO: &str = "EncidXVII/PortXR";

/// Where PORTXR_SHOT asks for the window's picture.
fn shot_path() -> Option<PathBuf> {
    std::env::var_os("PORTXR_SHOT").filter(|p| !p.is_empty()).map(PathBuf::from)
}

/// The app icon for the window and taskbar (the same one the exe carries, see build.rs).
fn window_icon() -> Option<egui::IconData> {
    let img = image::load_from_memory(include_bytes!("../assets/brand/portxr-256.png")).ok()?.to_rgba8();
    Some(egui::IconData { width: img.width(), height: img.height(), rgba: img.into_raw() })
}

pub fn run(open: Option<PathBuf>) -> eframe::Result {
    // PORTXR_SHOT_SIZE=1120x1040: another window size for the picture PORTXR_SHOT takes.
    let size = std::env::var("PORTXR_SHOT_SIZE").ok().filter(|_| shot_path().is_some());
    let size = size.as_deref().and_then(|v| v.split_once('x')).and_then(|(w, h)| Some([w.parse().ok()?, h.parse().ok()?]));
    let mut viewport = egui::ViewportBuilder::default()
        .with_title("PortXR")
        .with_inner_size(size.unwrap_or([1120.0, 720.0]))
        .with_min_inner_size([960.0, 560.0]);
    if let Some(icon) = window_icon() {
        viewport = viewport.with_icon(icon);
    }
    let options = eframe::NativeOptions { viewport, ..Default::default() };
    eframe::run_native("PortXR", options, Box::new(|cc| Ok(Box::new(App::new(cc, open)))))
}

#[derive(Deserialize, Clone)]
struct Release {
    tag_name: String,
    html_url: String,
}

#[derive(Clone, Copy, PartialEq)]
enum Filter {
    All,
    FullVr,
    Screen,
    NotInstalled,
    Unsupported,
}

// A library card: a game on this PC (by index), or a Unity game in the Steam library that is not installed.
enum Entry {
    Here(usize),
    Steam(games::Owned),
}

#[derive(Clone, Copy, PartialEq)]
enum Sort {
    Recent,
    Name,
}

#[derive(Clone, Copy, PartialEq)]
enum Tone {
    Info,
    Good,
    Bad,
}

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Library,
    Game(usize),
    Settings,
    About,
}

struct App {
    games: Vec<Game>,
    page: Page,
    shown_page: Option<Page>,
    // The game page the settings page was opened from, so going back returns there.
    back_to: Option<usize>,
    // Window focus last frame: coming back to the window rescans the library.
    focused: bool,
    status: (String, Tone),
    // The open game's config, and the all-games values (as edited, and as last written to disk).
    cfg: String,
    defaults: String,
    defaults_saved: String,
    dirty: bool,
    query: String,
    filter: Filter,
    store: Option<&'static str>,
    sort: Sort,
    confirm_uninstall: bool,
    confirm_anyway: bool,
    advanced: bool,
    buttons: bool,
    scroll_to_buttons: bool,
    // Play pressed for this game folder at this time: the game page waits for the plugin's note that VR started.
    launched: Option<(PathBuf, std::time::SystemTime)>,
    scripts_open: bool,
    /// The store and order menus' width last frame: the toolbar keeps them beside the tabs while they fit.
    menus_w: f32,
    update: Arc<Mutex<Option<Release>>>,
    // Unity games in the Steam library, installed or not (filled in the background: the Unity list may download).
    steam_owned: Arc<Mutex<Option<Vec<games::Owned>>>>,
    // A scan running in the background (start, window focus, F5) hands its result over here.
    scanned: Arc<Mutex<Option<Vec<Game>>>>,
    scanning: bool,
    // PORTXR_SHOT: since when the window has been up, and whether its picture was asked for.
    shot_since: Option<std::time::Instant>,
    shot_asked: bool,
    shot_saved: Arc<std::sync::atomic::AtomicBool>,
    // The game catalogue: last time's copy at once, the fresh one handed over when it has downloaded.
    catalogue: Catalogue,
    catalogue_new: Arc<Mutex<Option<Catalogue>>>,
    // Get script: the game folder whose files are downloading, and the result for the UI thread to install.
    getting: Option<PathBuf>,
    got: Arc<Mutex<Option<(PathBuf, Downloaded)>>>,
}

/// Get script's result: the entry's files, each checked against its SHA-256, or why not.
type Downloaded = Result<Vec<(catalogue::File, Vec<u8>)>, String>;

fn display(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("display".into()))
}

// Spaced capital labels (nav, filters, chips, small buttons): Barlow SemiBold.
fn mono(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("mono".into()))
}

// File paths and names, where every character counts: a monospaced face.
fn code(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("code".into()))
}

fn body(size: f32) -> FontId {
    FontId::proportional(size)
}

fn filter_of(g: &Game) -> Filter {
    match g.compat() {
        Compat::Theater => Filter::Screen,
        _ if g.usable() => Filter::FullVr,
        _ => Filter::Unsupported,
    }
}

impl App {
    fn new(cc: &eframe::CreationContext, open: Option<PathBuf>) -> Self {
        egui_extras::install_image_loaders(&cc.egui_ctx);
        load_theme();
        style(&cc.egui_ctx);
        let update = Arc::new(Mutex::new(None));
        let (slot, ctx) = (update.clone(), cc.egui_ctx.clone());
        std::thread::spawn(move || {
            if let Some(r) = latest_release() {
                *slot.lock().unwrap() = Some(r);
                ctx.request_repaint();
            }
        });
        let steam_owned = Arc::new(Mutex::new(None));
        let (slot, ctx) = (steam_owned.clone(), cc.egui_ctx.clone());
        std::thread::spawn(move || {
            *slot.lock().unwrap() = Some(games::steam_library());
            ctx.request_repaint();
        });
        let catalogue_new = Arc::new(Mutex::new(None));
        let (slot, ctx) = (catalogue_new.clone(), cc.egui_ctx.clone());
        std::thread::spawn(move || {
            if let Some(c) = catalogue::fetch() {
                *slot.lock().unwrap() = Some(c);
                ctx.request_repaint();
            }
        });
        let defaults = fs::read_to_string(defaults_path()).unwrap_or_default();
        let mut app = App {
            // Last time's library, at once; the scan below refreshes it.
            games: games::cached(),
            page: Page::Library,
            shown_page: None,
            back_to: None,
            focused: true,
            status: (String::new(), Tone::Info),
            cfg: String::new(),
            defaults: defaults.clone(),
            defaults_saved: defaults,
            dirty: false,
            query: String::new(),
            filter: Filter::All,
            store: None,
            sort: Sort::Recent,
            confirm_uninstall: false,
            confirm_anyway: false,
            advanced: false,
            buttons: false,
            scroll_to_buttons: false,
            launched: None,
            scripts_open: false,
            menus_w: 360.0,
            update,
            steam_owned,
            scanned: Arc::new(Mutex::new(None)),
            scanning: false,
            shot_since: None,
            shot_asked: false,
            shot_saved: Arc::new(std::sync::atomic::AtomicBool::new(false)),
            catalogue: catalogue::cached(),
            catalogue_new,
            getting: None,
            got: Arc::new(Mutex::new(None)),
        };
        // The folder is remembered first, so the scan started next finds it too.
        if let Some(dir) = open {
            app.open_dir(&dir);
        }
        app.scan_in_background(&cc.egui_ctx);
        app
    }

    fn rescan(&mut self) {
        self.games = games::scan();
        self.page = Page::Library;
    }

    // PORTXR_SHOT=<file.png>: the window's own picture, taken once the library and its covers have had a moment to
    // load; then the app closes. The README and banner pictures come from it (RELEASING.md).
    fn shot(&mut self, ctx: &egui::Context) {
        let Some(path) = shot_path() else { return };
        ctx.request_repaint();
        if self.shot_saved.load(std::sync::atomic::Ordering::Relaxed) {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            return;
        }
        // Counted from the end of the scan: the covers load once the games are listed.
        if self.scanning && self.page == Page::Library {
            self.shot_since = None;
            return;
        }
        let waited = self.shot_since.get_or_insert_with(std::time::Instant::now).elapsed();
        if !self.shot_asked && waited > std::time::Duration::from_secs(4) {
            self.shot_asked = true;
            let saved = self.shot_saved.clone();
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::ScreenshotCallback::new(move |img| {
                let rgba: Vec<u8> = img.pixels.iter().flat_map(|p| p.to_array()).collect();
                let _ = image::save_buffer(&path, &rgba, img.size[0] as u32, img.size[1] as u32, image::ColorType::Rgba8);
                saved.store(true, std::sync::atomic::Ordering::Relaxed);
            })));
        }
    }

    // Scanning every store and drive takes seconds on a big library: the library shows a loader meanwhile. A new
    // build also updates the plugin in every installed game, so starting one from Steam runs it too (a running game
    // locks its files; Play retries).
    fn scan_in_background(&mut self, ctx: &egui::Context) {
        if self.scanning {
            return;
        }
        self.scanning = true;
        let (slot, ctx) = (self.scanned.clone(), ctx.clone());
        // Taking a picture of the window (PORTXR_SHOT) only looks: no game folder is touched.
        let look_only = shot_path().is_some();
        std::thread::spawn(move || {
            let found = games::scan();
            for g in found.iter().filter(|g| g.installed() && !look_only) {
                // Taken out of games that gained anti-cheat, brought up to this build in the rest.
                let _ = installer::update_checked(g);
            }
            if !look_only {
                games::write_vr_list(&found);
            }
            *slot.lock().unwrap() = Some(found);
            ctx.request_repaint();
        });
    }

    // A game folder from the command line: remembered if new, then its page opens.
    fn open_dir(&mut self, dir: &Path) {
        let known = self.games.iter().find(|g| g.dir == dir).cloned();
        let Some(game) = known.or_else(|| games::detect(dir)).or_else(|| games::detect_other(dir)) else {
            self.status = ("No Unity, Unreal or Godot game found in that folder.".into(), Tone::Bad);
            return;
        };
        if !self.games.iter().any(|g| g.dir == game.dir) {
            if let Err(e) = games::add_folder(dir) {
                self.status = (e.to_string(), Tone::Bad);
                return;
            }
            // Joins the shown library now; the next scan brings it in with everything else.
            let mut added = games::inspect(dir).unwrap_or(game.clone());
            added.added = Some(dir.to_path_buf());
            added.store = "Added by you";
            self.games.push(added);
        }
        if let Some(i) = self.games.iter().position(|g| g.dir == game.dir) {
            self.open(i);
        }
    }

    // A game's own folder opens its page; any other folder (D:\Games) adds every Unity game inside it.
    fn add_game(&mut self) {
        let Some(dir) = rfd::FileDialog::new().set_title("Pick a game folder, or a folder that holds several games").pick_folder() else { return };
        if games::detect(&dir).is_some() {
            self.open_dir(&dir);
            return;
        }
        match games::add_folder(&dir) {
            Ok(0) => self.status = (format!("No games found in {} or the folders inside it.", dir.display()), Tone::Bad),
            Ok(n) => {
                self.rescan();
                self.status = (format!("Added {n} games from {}.", dir.display()), Tone::Good);
            }
            Err(e) => self.status = (e.to_string(), Tone::Bad),
        }
    }

    // Steam library games with no copy on this PC; None while the list is still loading.
    fn missing(&self) -> Option<Vec<games::Owned>> {
        let owned = self.steam_owned.lock().unwrap();
        owned.as_ref().map(|list| list.iter().filter(|o| !self.games.iter().any(|g| g.app_id == o.app_id)).cloned().collect())
    }

    // Every card the library can show, with the status tab it belongs to; each card is in exactly one tab.
    fn entries(&self) -> Vec<(Filter, Entry)> {
        let here = (0..self.games.len()).map(|i| (filter_of(&self.games[i]), Entry::Here(i)));
        let steam = self.missing().unwrap_or_default().into_iter().map(|o| (Filter::NotInstalled, Entry::Steam(o)));
        here.chain(steam).collect()
    }

    fn open(&mut self, i: usize) {
        self.page = Page::Game(i);
        self.status = (String::new(), Tone::Info);
        self.confirm_uninstall = false;
        self.confirm_anyway = false;
        self.cfg = load_cfg(&self.games[i]);
    }

    fn library(&mut self, ui: &mut Ui) {
        ui.horizontal(|ui| {
            ui.label(RichText::new("YOUR GAMES.").font(display(TITLE)).color(INK));
            if !self.games.is_empty() {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    let search = egui::TextEdit::singleline(&mut self.query).hint_text("Search games").margin(Margin::symmetric(12, 10));
                    let r = ui.add_sized([280.0, CONTROL_H], search);
                    r.widget_info(|| egui::WidgetInfo::labeled(egui::Role::TextInput, true, "Search games"));
                });
            }
        });
        if !self.status.0.is_empty() {
            status_line(ui, &self.status.0, self.status.1);
        }
        ui.add_space(22.0);
        if self.games.is_empty() && self.scanning {
            ui.label(RichText::new("Looking for games…").color(INK2));
            ui.add_space(32.0);
            skeleton_grid(ui, 12);
            return;
        }
        if self.games.is_empty() {
            self.empty(ui, "NO GAMES FOUND.", "Nothing in Steam, Epic, GOG or itch. Add a game's folder.", "Add game +");
            return;
        }
        self.toolbar(ui);
        ui.add_space(32.0);
        if self.filter == Filter::NotInstalled && self.missing().is_none() {
            skeleton_grid(ui, 6);
            return;
        }

        let query = self.query.to_lowercase();
        let name = |e: &Entry| match e {
            Entry::Here(i) => self.games[*i].name.to_lowercase(),
            Entry::Steam(o) => o.name.to_lowercase(),
        };
        let mut shown: Vec<(Filter, Entry)> = self
            .entries()
            .into_iter()
            .filter(|(f, e)| {
                let store = match e {
                    Entry::Here(i) => self.games[*i].store,
                    Entry::Steam(_) => "Steam",
                };
                // All is what can be played; the games PortXR can't run stay in their own tab.
                (if self.filter == Filter::All { *f != Filter::Unsupported } else { *f == self.filter }) && self.store.is_none_or(|s| store == s) && name(e).contains(&query)
            })
            .collect();
        shown.sort_by_key(|(_, e)| name(e));
        // A stable sort keeps name order among games never played.
        if self.sort == Sort::Recent {
            shown.sort_by_key(|(_, e)| {
                std::cmp::Reverse(match e {
                    Entry::Here(i) => self.games[*i].last_played,
                    Entry::Steam(o) => o.last_played,
                })
            });
        }
        let shown: Vec<Entry> = shown.into_iter().map(|(_, e)| e).collect();
        if shown.is_empty() {
            ui.label(RichText::new("No matches.").color(INK2));
            ui.add_space(12.0);
            if secondary(ui, "Show all games").clicked() {
                self.query.clear();
                self.filter = Filter::All;
                self.store = None;
            }
            return;
        }
        // Your games first; Steam games that would get full VR follow as their own shelf, a suggestion, not clutter.
        let (here, steam): (Vec<Entry>, Vec<Entry>) = shown.into_iter().partition(|e| matches!(e, Entry::Here(_)));
        let (mut open, mut install) = (None, None);
        self.grid(ui, &here, &mut open, &mut install);
        if !steam.is_empty() {
            if !here.is_empty() {
                ui.add_space(24.0);
                eyebrow(ui, "", "Full VR after install");
                ui.add_space(16.0);
            }
            self.grid(ui, &steam, &mut open, &mut install);
        }
        if let Some(i) = open {
            self.open(i);
        }
        if let Some(o) = install {
            self.install_owned(&o);
        }
    }

    fn grid(&self, ui: &mut Ui, entries: &[Entry], open: &mut Option<usize>, install: &mut Option<games::Owned>) {
        let (cols, w) = columns(ui.available_width());
        for row in entries.chunks(cols) {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = GAP;
                for e in row {
                    match e {
                        Entry::Here(i) => {
                            // The rating for the mode this game plays in here (its chip), else the best one.
                            let g = &self.games[*i];
                            let mode = if g.compat() == Compat::Theater { "VR screen" } else { "Full VR" };
                            let rating = self.catalogue.find(g).and_then(|e| e.ratings.get(mode).map(|r| (mode, r.as_str())).or_else(|| e.best()));
                            if card(ui, &self.games[*i], w, rating).clicked() {
                                *open = Some(*i);
                            }
                        }
                        Entry::Steam(o) => {
                            if owned_card(ui, o, w).on_hover_text("Install with Steam").clicked() {
                                *install = Some(o.clone());
                            }
                        }
                    }
                }
            });
            ui.add_space(24.0);
        }
    }

    // A click on a game that is not installed asks Steam to install it; it joins the library when done.
    fn install_owned(&mut self, o: &games::Owned) {
        self.status = match games::steam_install(o.app_id) {
            Ok(()) => (format!("Installing {} in Steam.", o.name), Tone::Good),
            Err(e) => (e.to_string(), Tone::Bad),
        };
    }

    // Status (with counts) on the left; store and order on the right, or on their own row when the window is too
    // narrow for both. A filter with one possible value is left out.
    fn toolbar(&mut self, ui: &mut Ui) {
        let mut wrap = false;
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing = vec2(16.0, 12.0);
            // The tabs split the library: each card is in one; All is every tab but Unsupported.
            let entries = self.entries();
            let count = |f: Filter| entries.iter().filter(|(e, _)| if f == Filter::All { *e != Filter::Unsupported } else { *e == f }).count();
            let labels: Vec<(Filter, String)> = [Filter::All, Filter::FullVr, Filter::Screen, Filter::NotInstalled, Filter::Unsupported]
                .into_iter()
                .filter(|&f| f == self.filter || f == Filter::All || count(f) > 0)
                .map(|f| {
                    let name = match f {
                        Filter::All => "All",
                        Filter::FullVr => "Full VR",
                        Filter::Screen => "VR screen",
                        Filter::NotInstalled => "Not installed",
                        Filter::Unsupported => "Unsupported",
                    };
                    (f, format!("{name}  {}", count(f)))
                })
                .collect();
            let options: Vec<(Filter, &str)> = labels.iter().map(|(f, l)| (*f, l.as_str())).collect();
            if let Some(f) = segmented(ui, &options, self.filter, true) {
                self.filter = f;
            }
            // The menus at the row's end while they fit (their width as drawn last frame), else on a row of their own
            // under the tabs, from the left.
            wrap = ui.available_width() < self.menus_w + 16.0;
            if !wrap {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| self.menus_w = self.menus(ui));
            }
        });
        if wrap {
            ui.add_space(12.0);
            ui.horizontal(|ui| self.menus_w = self.menus(ui));
        }
    }

    // Store, then order, read left to right in either layout; returns their width.
    fn menus(&mut self, ui: &mut Ui) -> f32 {
        ui.spacing_mut().item_spacing.x = 16.0;
        let mut stores: Vec<&'static str> = self.games.iter().map(|g| g.store).collect();
        stores.sort();
        stores.dedup();
        let rtl = ui.layout().prefer_right_to_left();
        let mut w = 0.0;
        if rtl {
            w += self.sort_menu(ui).rect.width();
        }
        if stores.len() > 1 {
            let shown = self.store.unwrap_or("All stores");
            let menu = egui::ComboBox::from_id_salt("store").selected_text(button_text(shown, INK)).show_ui(ui, |ui| {
                ui.selectable_value(&mut self.store, None, button_text("All stores", INK));
                for s in stores {
                    ui.selectable_value(&mut self.store, Some(s), button_text(s, INK));
                }
            });
            menu.response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::ComboBox, true, format!("Store: {shown}")));
            w += menu.response.rect.width() + 16.0;
        }
        if !rtl {
            w += self.sort_menu(ui).rect.width();
        }
        w
    }

    fn sort_menu(&mut self, ui: &mut Ui) -> egui::Response {
        let order = if self.sort == Sort::Recent { "Recently played" } else { "Name A-Z" };
        let menu = egui::ComboBox::from_id_salt("sort").selected_text(button_text(order, INK)).show_ui(ui, |ui| {
            ui.selectable_value(&mut self.sort, Sort::Recent, button_text("Recently played", INK));
            ui.selectable_value(&mut self.sort, Sort::Name, button_text("Name A-Z", INK));
        });
        // Screen readers name controls by their label; these show only their value.
        menu.response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::ComboBox, true, format!("Order: {order}")));
        menu.response
    }

    fn empty(&mut self, ui: &mut Ui, title: &str, text: &str, action: &str) {
        ui.add_space(24.0);
        ui.label(RichText::new(title).font(display(HEADING)).color(INK));
        ui.label(RichText::new(text).color(INK2));
        ui.add_space(16.0);
        if primary(ui, action).clicked() {
            self.add_game();
        }
    }

    fn game_page(&mut self, ui: &mut Ui, i: usize) {
        // The page reads the folder live (installed or not); redraw now and then so changes made elsewhere show.
        ui.ctx().request_repaint_after(std::time::Duration::from_secs(1));
        let g = self.games[i].clone();
        if tertiary(ui, "← Library").on_hover_text("Esc").clicked() {
            self.back();
            return;
        }
        ui.add_space(16.0);
        // One page column, the same as the Settings page: the header sits on top, sections run its full width.
        ui.set_max_width(ui.available_width().min(PAGE_W));
        let entry = self.catalogue.find(&g).filter(|_| g.blocker.is_none()).cloned();
        ui.horizontal_top(|ui| {
            let (rect, _) = ui.allocate_exact_size(vec2(160.0, 240.0), Sense::hover());
            cover(ui, &g, rect);
            ui.add_space(32.0);
            ui.vertical(|ui| {
                ui.add(egui::Label::new(RichText::new(g.name.to_uppercase()).font(display(TITLE)).color(INK)).wrap());
                // Anton reserves a tall line; pull the metadata up so it groups with the title.
                ui.add_space(-10.0);
                ui.horizontal_wrapped(|ui| {
                    ui.spacing_mut().item_spacing.x = 8.0;
                    chip(ui, g.badge(), chip_kind(&g));
                    if let Some(e) = &entry {
                        rating_link(ui, e);
                    }
                });
                ui.add_space(24.0);
                ui.horizontal_wrapped(|ui| self.actions(ui, &g, entry.as_ref()));
                self.follow_launch(ui.ctx(), &g);
                let (text, tone) = if self.status.0.is_empty() { warning(&g) } else { self.status.clone() };
                if !text.is_empty() {
                    status_line(ui, &text, tone);
                } else if g.usable() {
                    // Checked before Play: without a runtime the game just starts flat.
                    let last = games::last_vr(&g).map(|t| format!(" Last played in VR {}.", ago(t))).unwrap_or_default();
                    match games::openxr_runtime() {
                        Some(name) if g.compat() == Compat::Theater => status_line(ui, &format!("Plays on the VR screen through {name}. Controllers work as keyboard and mouse.{last}"), Tone::Info),
                        Some(name) if games::needs_openvr_bridge(&name) => status_line(ui, &format!("{name} needs xrizer or OpenComposite set as the OpenVR runtime: Proton gives games OpenXR only through one. {name}'s guide shows how."), Tone::Bad),
                        Some(name) => status_line(ui, &format!("Starts in VR through {name}. Put your headset on first.{last}"), Tone::Info),
                        None if cfg!(unix) => status_line(ui, "No VR runtime found. Start SteamVR, or set up WiVRn or Monado.", Tone::Bad),
                        None => status_line(ui, "No VR runtime found. Start SteamVR, Virtual Desktop or Meta Quest Link.", Tone::Bad),
                    }
                }
            });
        });
        // VR screen games use the all-games screen settings; the per-game ones below are the VR core's (Unity, Godot 4.3+, Unreal).
        if !g.usable() || !g.core_vr() {
            self.troubleshooting(ui, &g);
            return;
        }
        section_break(ui);
        let changed = self.changed(settings::QUICK) + self.changed(settings::ADVANCED);
        ui.horizontal(|ui| {
            eyebrow(ui, "01", "Basics");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 24.0;
                if tertiary(ui, "All games →").clicked() {
                    self.back_to = Some(i);
                    self.page = Page::Settings;
                }
                if changed > 0 && tertiary(ui, "Reset all").on_hover_text("Back to your settings for all games").clicked() {
                    self.cfg = settings::all().fold(self.cfg.clone(), |t, d| settings::set(&t, d.section, d.key, &settings::global(&self.defaults, d)));
                    self.dirty = true;
                }
            });
        });
        if changed > 0 {
            let n = if changed == 1 { "1 setting".to_string() } else { format!("{changed} settings") };
            ui.label(RichText::new(format!("{n} changed for this game.")).color(INK2));
        }
        ui.add_space(24.0);
        self.settings(ui, settings::QUICK, true);
        self.advanced_section(ui, true);
        self.buttons_section(ui, true);
        if g.core_vr() {
            self.scripts_section(ui, &g);
        }
        let file = g.config_file();
        if self.advanced && file.exists() {
            ui.add_space(8.0);
            if tertiary(ui, "Open settings file").clicked() {
                let _ = games::edit(&file);
            }
        }
        self.troubleshooting(ui, &g);
    }

    // Get script beside Play, while the catalogue has files this game does not have yet (or newer ones).
    fn get_files(&mut self, ui: &mut Ui, g: &Game, e: &catalogue::Entry) {
        if e.files.is_empty() {
            return;
        }
        let have = catalogue::installed(g, e);
        if have.as_deref().is_some_and(|v| !catalogue::outdated(v, e)) {
            return;
        }
        let label = if self.getting.is_some() {
            "Downloading…".to_string()
        } else if have.is_some() {
            format!("Update {}", e.what())
        } else {
            format!("Get {}", e.what())
        };
        let get = ui.add_enabled_ui(self.getting.is_none(), |ui| secondary(ui, &label)).inner;
        if get.on_hover_text("From the game catalogue on everyvr.net. Each file's SHA-256 is checked before it is used.").clicked() {
            let (slot, ctx, dir, entry) = (self.got.clone(), ui.ctx().clone(), g.dir.clone(), e.clone());
            self.getting = Some(g.dir.clone());
            std::thread::spawn(move || {
                let result = catalogue::download(&entry);
                *slot.lock().unwrap() = Some((dir, result));
                ctx.request_repaint();
            });
        }
    }

    // Get script, once its files have downloaded and checked out: scripts go into the scripts folder, settings into
    // the game's settings (the open page's copy, so the page shows them at once).
    fn finish_get(&mut self, dir: &Path, result: Downloaded) {
        let Some(i) = self.games.iter().position(|g| g.dir == dir) else { return };
        let g = self.games[i].clone();
        let Some(e) = self.catalogue.find(&g).cloned() else { return };
        let files = match result {
            Ok(files) => files,
            Err(why) => {
                self.status = (why, Tone::Bad);
                return;
            }
        };
        let on_page = self.page == Page::Game(i);
        let cfg = if on_page { self.cfg.clone() } else { fs::read_to_string(g.config_file()).unwrap_or_default() };
        match catalogue::install(&g, &e, &files, &cfg) {
            Ok(new) => {
                if new != cfg {
                    if on_page {
                        self.cfg = new;
                        self.dirty = true;
                    } else if let Err(why) = save_cfg(&g, &new) {
                        self.status = (format!("Could not save the settings: {why}"), Tone::Bad);
                        return;
                    }
                }
                self.status = (format!("The catalogue's {} for {} is installed (entry {}).", e.what(), g.name, e.entry_version), Tone::Good);
            }
            Err(why) => self.status = (format!("Could not install it: {why}"), Tone::Bad),
        }
    }

    // What only matters when something goes wrong, quiet and last: where the game is, what it is built with, its logs.
    fn troubleshooting(&mut self, ui: &mut Ui, g: &Game) {
        section_break(ui);
        eyebrow(ui, "", "Troubleshooting");
        ui.add(egui::Label::new(RichText::new(format!("{}  ·  {}", g.engine_label(), g.dir.display())).font(code(LABEL)).color(INK3)).wrap());
        ui.add_space(12.0);
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing.x = 24.0;
            if tertiary(ui, "Open folder").clicked() {
                let _ = games::open(&g.dir);
            }
            if g.installed() && tertiary(ui, "Export logs").clicked() {
                self.report(games::export_logs(g).map(|zip| {
                    #[cfg(windows)]
                    let _ = std::os::windows::process::CommandExt::raw_arg(&mut std::process::Command::new("explorer"), format!("/select,\"{}\"", zip.display())).spawn();
                    #[cfg(not(windows))]
                    let _ = zip.parent().map(games::open);
                    "Logs saved to your desktop.".into()
                }));
            }
            if g.log_file().exists() && tertiary(ui, "Open log").clicked() {
                let _ = games::edit(&g.log_file());
            }
            if g.blocker.is_none() && tertiary(ui, "How did it run? ↗").on_hover_text("Tell other players in the game catalogue: a short form on GitHub").clicked() {
                let _ = games::open(catalogue::report_url(g));
            }
            // Only hand-added folders can be forgotten (store games come back on every scan), and not while installed,
            // or the game would vanish from the app with PortXR still in it.
            if let Some(root) = g.added.as_ref().filter(|_| !g.installed())
                && tertiary(ui, "Remove from library").clicked() {
                    match games::remove_folder(root) {
                        Ok(()) => {
                            self.rescan();
                            self.status = (format!("Removed {} from the library.", g.name), Tone::Good);
                        }
                        Err(e) => self.status = (e.to_string(), Tone::Bad),
                    }
                }
        });
    }

    // The all-games values every game starts from.
    fn settings_page(&mut self, ui: &mut Ui) {
        ui.set_max_width(ui.available_width().min(PAGE_W));
        // Opened from a game page: a way straight back to it.
        if let Some(i) = self.back_to {
            if tertiary(ui, &format!("← {}", self.games[i].name)).on_hover_text("Esc").clicked() {
                self.back();
                return;
            }
            ui.add_space(16.0);
        }
        ui.label(RichText::new("SETTINGS.").font(display(TITLE)).color(INK));
        ui.label(RichText::new("For every game. Each game's page can override them.").color(INK2));
        section_break(ui);
        ui.horizontal(|ui| {
            eyebrow(ui, "01", "Basics");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let stock = settings::all().all(|d| settings::same(&settings::global(&self.defaults, d), d.default));
                if !stock && tertiary(ui, "Reset all").on_hover_text("Back to PortXR's own defaults").clicked() {
                    self.defaults = settings::all().fold(self.defaults.clone(), |t, d| settings::set(&t, d.section, d.key, d.default));
                    self.dirty = true;
                }
            });
        });
        ui.add_space(24.0);
        self.settings(ui, settings::QUICK, false);
        self.advanced_section(ui, false);
        self.buttons_section(ui, false);
        section_break(ui);
        eyebrow(ui, "04", "Look");
        ui.add_space(16.0);
        if let Some(i) = theme_picker(ui)
            && let Err(e) = set_theme(ui.ctx(), i)
        {
            self.report(Err(e.into()));
        }
    }

    // What PortXR is, where to get help, who made it, the licence. Links open in the browser.
    fn about_page(&mut self, ui: &mut Ui) {
        ui.set_max_width(ui.available_width().min(PAGE_W));
        ui.label(RichText::new("ABOUT.").font(display(TITLE)).color(INK));
        ui.label(RichText::new(format!("PortXR {} plays flat Unity, Unreal Engine and Godot games in VR.", env!("CARGO_PKG_VERSION"))).color(INK2));
        section_break(ui);
        eyebrow(ui, "01", "Help");
        ui.add_space(16.0);
        ui.label(RichText::new("Export logs from the game's page and attach the zip to your report.").color(INK2));
        links(ui, &[("Report a problem", format!("https://github.com/{REPO}/issues/new/choose")), ("Guide", "https://everyvr.net/portxr".into())]);
        section_break(ui);
        eyebrow(ui, "02", "Made by");
        ui.add_space(16.0);
        ui.label(RichText::new("EncidXVII").color(INK2));
        links(ui, &[("GitHub", "https://github.com/EncidXVII".into()), ("X", "https://x.com/EncidXVII".into()), ("Threads", "https://www.threads.com/@encidxvii".into())]);
        section_break(ui);
        eyebrow(ui, "03", "Licence");
        ui.add_space(16.0);
        ui.label(RichText::new("Free and open source under GPL-3.0.").color(INK2));
        links(ui, &[("Licence", format!("https://github.com/{REPO}/blob/main/LICENSE"))]);
    }

    fn advanced_section(&mut self, ui: &mut Ui, game: bool) {
        let changed = if game { self.changed(settings::ADVANCED) } else { 0 };
        if fold(ui, "02", "Advanced", changed, &mut self.advanced) {
            ui.add_space(24.0);
            self.settings(ui, settings::ADVANCED, game);
        }
    }

    // Keyboard & mouse bindings: one row per controller input, one column per gesture.
    fn buttons_section(&mut self, ui: &mut Ui, game: bool) {
        let changed = if game { self.changed(&settings::BINDINGS) } else { 0 };
        if std::mem::take(&mut self.scroll_to_buttons) {
            ui.scroll_to_cursor(Some(Align::TOP));
        }
        if !fold(ui, "03", "Buttons", changed, &mut self.buttons) {
            return;
        }
        ui.add_space(8.0);
        let note = "Keys each controller button presses in keyboard & mouse mode.";
        ui.add(egui::Label::new(RichText::new(note).color(INK2)).wrap());
        ui.add_space(24.0);
        egui::Grid::new(("buttons", game)).num_columns(1 + settings::GESTURES.len()).spacing(vec2(16.0, 12.0)).show(ui, |ui| {
            ui.label("");
            for (_, name) in settings::GESTURES {
                ui.label(RichText::new(name.to_uppercase()).font(mono(LABEL)).color(INK3));
            }
            ui.end_row();
            for (i, (_, name, _)) in settings::INPUTS.iter().enumerate() {
                ui.label(RichText::new(*name).color(INK));
                for g in 0..settings::GESTURES.len() {
                    let Some(d) = settings::binding(i, g) else {
                        ui.label("");
                        continue;
                    };
                    let global = settings::global(&self.defaults, d);
                    let current = if game { settings::get(&self.cfg, d.section, d.key).unwrap_or_else(|| global.clone()) } else { global.clone() };
                    // On a game page a key set for this game only reads in full ink; one that follows all games is quieter.
                    let own = game && !settings::same(&current, &global);
                    let tone = if current == "None" { INK3 } else if !game || own { INK } else { INK2 };
                    let mut pick = current.clone();
                    let menu = egui::ComboBox::from_id_salt((d.key, game)).width(152.0).selected_text(button_text(settings::key_label(&current), tone)).show_ui(ui, |ui| {
                        for k in settings::KEYS {
                            ui.selectable_value(&mut pick, k.to_string(), button_text(settings::key_label(k), INK));
                        }
                    });
                    menu.response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::ComboBox, true, format!("{}: {}", d.label, settings::key_label(&current))));
                    if pick != current {
                        let text = if game { &mut self.cfg } else { &mut self.defaults };
                        *text = settings::set(text, d.section, d.key, &pick);
                        self.dirty = true;
                    }
                }
                ui.end_row();
            }
        });
    }

    // How many of these the open game sets differently from the all-games values.
    fn changed(&self, defs: &[Def]) -> usize {
        defs.iter()
            .filter(|d| !d.per_game() && settings::get(&self.cfg, d.section, d.key).is_some_and(|v| !settings::same(&v, &settings::global(&self.defaults, d))))
            .count()
    }

    fn actions(&mut self, ui: &mut Ui, g: &Game, entry: Option<&catalogue::Entry>) {
        ui.spacing_mut().item_spacing.x = 8.0;
        let playable = g.usable();
        if playable && primary(ui, "Play in VR ↗").clicked() {
            let started = play(g, &self.defaults);
            self.launched = started.is_ok().then(|| (g.dir.clone(), std::time::SystemTime::now()));
            self.report(started);
            self.confirm_uninstall = false;
        }
        // The catalogue's script, while there is one to get: a button beside Play, before the links.
        if let Some(e) = entry {
            self.get_files(ui, g, e);
        }
        // Linux with WiVRn or Monado: Steam keeps its launch options to itself while running, and the other launchers
        // their environment variables, so the player pastes them in.
        if playable && games::needs_launch_options(g) {
            if g.in_steam() && tertiary(ui, "Copy launch options").clicked() {
                ui.ctx().copy_text(games::LAUNCH_OPTIONS.into());
                let tab = if g.app_id > 0 { "General" } else { "Shortcut" };
                self.status = (format!("Copied. In Steam: the game's Properties → {tab} → Launch options, paste, then Play."), Tone::Good);
            } else if !g.in_steam() && tertiary(ui, "Copy variable").clicked() {
                ui.ctx().copy_text(games::OPENXR_ENV.into());
                self.status = (format!("Copied. In {}, add it to the game's environment variables, then Play.", g.launcher_name()), Tone::Good);
            }
        }
        // A game PortXR would leave alone, for players who want to try anyway: the caution first, then a confirm.
        if let Some(caution) = g.caution().filter(|_| !g.installed()) {
            let label = if self.confirm_anyway { "Confirm: add VR anyway" } else { "Add VR anyway" };
            if destructive(ui, label, self.confirm_anyway).clicked() {
                if self.confirm_anyway {
                    self.report(install_anyway(g, &self.defaults));
                    self.cfg = load_cfg(g);
                    games::write_vr_list(&self.games);
                } else {
                    self.status = (format!("{caution} Press again to add VR anyway."), Tone::Bad);
                }
                self.confirm_anyway = !self.confirm_anyway;
            }
        }
        // Everything below is occasional, so it reads as links, not buttons competing with Play.
        ui.spacing_mut().item_spacing.x = 24.0;
        if playable {
            ui.add_space(16.0);
        }
        // The button section is the VR core's (Unity, Godot 4.3+, Unreal); the VR screen keeps fixed keys.
        if playable && g.core_vr() && tertiary(ui, "Button layout").clicked() {
            self.buttons = true;
            self.scroll_to_buttons = true;
        }
        // Puts the game folder back as it was: a red link with the others, then a filled button for the second press.
        if g.installed() {
            let r = if self.confirm_uninstall { destructive(ui, "Confirm: remove VR", true) } else { link(ui, "Remove VR", DANGER_INK) };
            if r.clicked() {
                if self.confirm_uninstall {
                    self.report(uninstall(g));
                    self.cfg = load_cfg(g);
                    games::write_vr_list(&self.games);
                }
                self.confirm_uninstall = !self.confirm_uninstall;
            }
        }
    }

    // Lua scripts for this game (see SCRIPTING.md): for cases the automatic handling gets wrong. They load on the next
    // start, and reload while the game runs when a file changes.
    fn scripts_section(&mut self, ui: &mut Ui, g: &Game) {
        let list = games::scripts(g);
        if !fold(ui, "04", "Scripts", list.len(), &mut self.scripts_open) {
            return;
        }
        ui.add(egui::Label::new(RichText::new("Lua fixes for this game. They reload while it runs.").color(INK2)).wrap());
        ui.add_space(12.0);
        let mut remove = None;
        for path in &list {
            ui.horizontal(|ui| {
                ui.label(RichText::new(path.file_name().unwrap_or_default().to_string_lossy()).font(code(BODY)).color(INK));
                ui.spacing_mut().item_spacing.x = 24.0;
                if tertiary(ui, "Edit").clicked() {
                    let _ = games::edit(path);
                }
                if tertiary(ui, "Remove").clicked() {
                    remove = Some(path.clone());
                }
            });
        }
        if let Some(path) = remove {
            self.report(fs::remove_file(&path).map(|_| "Script removed.".into()).map_err(Into::into));
        }
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 24.0;
            if secondary(ui, "Import script").clicked()
                && let Some(file) = rfd::FileDialog::new().add_filter("Lua script", &["lua"]).pick_file() {
                    self.report(games::import_script(g, &file).map(|to| format!("Imported {}.", to.file_name().unwrap_or_default().to_string_lossy())).map_err(Into::into));
                }
            if tertiary(ui, "Open scripts folder").clicked() {
                let dir = games::scripts_dir(g);
                let _ = fs::create_dir_all(&dir);
                let _ = games::open(dir);
            }
            if tertiary(ui, "Scripting guide").clicked() {
                let _ = games::open(format!("https://github.com/{REPO}/blob/main/SCRIPTING.md"));
            }
        });
    }

    // After Play: "Starting" until the plugin notes its first frame in the headset, then a confirmation; a game that
    // shows no VR within 90 s gets what to check.
    fn follow_launch(&mut self, ctx: &egui::Context, g: &Game) {
        let Some((dir, at)) = self.launched.clone() else { return };
        if dir != g.dir {
            return;
        }
        if games::last_vr(g).is_some_and(|t| t >= at) {
            self.launched = None;
            self.status = ("In VR. Double-tap the left menu button for the VR menu.".into(), Tone::Good);
            return;
        }
        let waited = at.elapsed().map_or(0, |d| d.as_secs());
        if waited >= 90 {
            self.launched = None;
            let runtime = games::openxr_runtime().unwrap_or_else(|| "your VR runtime".into());
            self.status = (format!("No VR yet. If the game runs flat, wake the headset, check {runtime} is running and restart the game."), Tone::Bad);
            return;
        }
        // The launch message stays readable for its first seconds.
        if waited >= 4 {
            self.status = (format!("Starting… {waited} s. Put your headset on."), Tone::Info);
        }
        ctx.request_repaint_after(std::time::Duration::from_secs(1));
    }

    fn report(&mut self, result: Result<String, Box<dyn Error>>) {
        self.status = match result {
            Ok(text) => (text, Tone::Good),
            Err(e) => (e.to_string(), Tone::Bad),
        };
    }

    // On a game page (`game`) the rows show that game's values; one that differs from the all-games value gets a link
    // back to it. On the settings page they edit the all-games values.
    fn settings(&mut self, ui: &mut Ui, defs: &[Def], game: bool) {
        // Name column up to 300, giving way first when the window is narrow; controls keep their natural width.
        let name_w = (ui.available_width() - 32.0 - 480.0).clamp(180.0, 300.0);
        // Rows that only matter with another setting's value show with it, as in the VR menu.
        let value = |key: &str| {
            let d = settings::QUICK.iter().chain(settings::ADVANCED).find(|d| d.key == key).unwrap();
            let global = settings::global(&self.defaults, d);
            if game { settings::get(&self.cfg, d.section, d.key).unwrap_or(global) } else { global }
        };
        let unreal = game && matches!(self.page, Page::Game(i) if self.games.get(i).is_some_and(|g| g.unreal_vr()));
        // UI black keys Unity's UI panel; Godot and Unreal hand over their UI with its own transparency.
        let unity = !game || matches!(self.page, Page::Game(i) if self.games.get(i).is_some_and(|g| g.unity()));
        let display = value("Display");
        let (window3d, screen) = (display == "Parallax", display == "Parallax" || display == "Theater");
        let (aim, hand) = (value("HeadAim") != "Off", value("AimWith") == "Hand");
        let relevant = |d: &Def| match d.key {
            "Depth" => window3d,
            "Background" => screen,
            "AimWith" | "Crosshair" => aim,
            "WeaponInHand" => aim && hand,
            "UiBlack" => unity,
            "Console" => unreal,
            _ => true,
        };
        egui::Grid::new((defs[0].key, game)).num_columns(2).spacing(vec2(32.0, 24.0)).min_col_width(name_w).show(ui, |ui| {
            for d in defs.iter().filter(|d| (game || !d.per_game()) && relevant(d)) {
                let global = settings::global(&self.defaults, d);
                let current = if game { settings::get(&self.cfg, d.section, d.key).unwrap_or_else(|| global.clone()) } else { global.clone() };
                let mut back = false;
                ui.vertical(|ui| {
                    ui.set_max_width(name_w);
                    ui.spacing_mut().item_spacing.y = 4.0;
                    // Centre the name's first line on the control beside it; hint and reset link hang below.
                    ui.add_space((CONTROL_H - ui.text_style_height(&egui::TextStyle::Body)) / 2.0);
                    ui.label(RichText::new(d.label).color(INK));
                    if !d.hint.is_empty() {
                        ui.add(egui::Label::new(RichText::new(d.hint).font(body(SMALL)).color(INK3)).wrap());
                    }
                    if game && !d.per_game() && !settings::same(&current, &global) {
                        back = small_link(ui, &format!("Reset to {}", settings::shown(d, &global))).on_hover_text("Your setting for all games").clicked();
                    }
                });
                let new = ui.vertical(|ui| control(ui, d, &current)).inner.or(back.then_some(global));
                ui.end_row();
                if let Some(v) = new {
                    let text = if game { &mut self.cfg } else { &mut self.defaults };
                    *text = settings::set(text, d.section, d.key, &v);
                    self.dirty = true;
                }
            }
        });
    }
}

// A collapsible section header, "NN // WORD · N CHANGED  +"; true while open.
fn fold(ui: &mut Ui, number: &str, word: &str, changed: usize, open: &mut bool) -> bool {
    section_break(ui);
    let note = if changed > 0 && !*open { format!(" · {changed} changed") } else { String::new() };
    let mut text = eyebrow_text(number, word);
    text.append(&format!("{}  {}", note.to_uppercase(), if *open { "−" } else { "+" }), 0.0, label_format(INK2));
    let old = std::mem::replace(&mut ui.spacing_mut().button_padding, vec2(0.0, 0.0));
    let header = ui.add(egui::Button::new(text).frame(false).min_size(vec2(0.0, 24.0)));
    ui.spacing_mut().button_padding = old;
    focus_ring(ui, &header);
    if header.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() {
        *open = !*open;
    }
    *open
}

fn control(ui: &mut Ui, d: &Def, current: &str) -> Option<String> {
    match d.kind {
        Kind::Slider { lo, hi, step, per, .. } => {
            let mut v: f32 = current.parse().unwrap_or_else(|_| d.default.parse().unwrap());
            let parse = move |t: &str| t.trim().trim_end_matches(['×', '%', 'm', ' ']).trim().parse::<f64>().ok().map(|x| x * per as f64);
            // Lime fill on a dark rail with an ink knob (painted over egui's, which shares the rail's fill), then
            // the value in its own box.
            let changed = ui
                .horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 16.0;
                    let r = ui.add(egui::Slider::new(&mut v, lo..=hi).step_by(step as f64).show_value(false));
                    let radius = r.rect.height() / 2.5;
                    let t = ((v - lo) / (hi - lo)).clamp(0.0, 1.0);
                    let x = egui::lerp(r.rect.left() + radius..=r.rect.right() - radius, t);
                    let grow = ease(ui, r.id.with("knob"), r.hovered() || r.dragged(), FAST);
                    ui.painter().circle_filled(pos2(x, r.rect.center().y), radius + grow * 2.0, INK);
                    let value = egui::DragValue::new(&mut v)
                        .range(lo..=hi)
                        .speed(step)
                        .custom_formatter(|x, _| settings::number(&d.kind, x))
                        .custom_parser(parse);
                    let b = ui.add_sized([72.0, CONTROL_H - 4.0], value);
                    r.changed() || b.changed()
                })
                .inner;
            // A slider may nudge a value onto its step grid just by being drawn; only a real change counts.
            let new = format!("{}", (v * 100.0).round() / 100.0);
            (changed && !settings::same(&new, current)).then_some(new)
        }
        Kind::Toggle => {
            let on = current.eq_ignore_ascii_case("true");
            segmented(ui, &[(false, "Off"), (true, "On")], on, false).map(|b| b.to_string())
        }
        Kind::Choice(options) => {
            segmented(ui, options, options.iter().find(|(v, _)| settings::same(v, current)).map_or(options[0].0, |o| o.0), false).map(String::from)
        }
        Kind::Key => None,  // drawn by the Buttons table
        // The camera name: picked in VR, from the cameras the game has; here it only shows and can go back.
        Kind::Text => {
            ui.add_space((CONTROL_H - LABEL) / 2.0);
            if current.is_empty() {
                ui.label(RichText::new("AUTOMATIC").font(mono(LABEL)).color(INK2).extra_letter_spacing(TRACK));
                None
            } else {
                ui.label(RichText::new(current).font(mono(LABEL)).color(INK));
                small_link(ui, "Use automatic").clicked().then(String::new)
            }
        }
    }
}

fn warning(g: &Game) -> (String, Tone) {
    if let Some(caution) = g.caution().filter(|_| g.installed()) {
        return (format!("VR was added anyway. {caution}"), Tone::Bad);
    }
    // Every scan and every Play bring a game's plugin up to this build; where neither could, the page says so.
    if installer::outdated_now(g) {
        return ("This game still holds an older PortXR plugin: it was running, or its files were in use, when PortXR last looked. Close the game; Play in VR puts in this version's.".into(), Tone::Info);
    }
    match (g.compat(), g.other_loader) {
        (Compat::Blocked, _) if g.blocker == Some("Multiplayer only") => ("Multiplayer only: mods online can get accounts banned. Only playing offline? Add VR anyway.".into(), Tone::Bad),
        (Compat::Blocked, _) => (format!("{} found. Mods in anti-cheat games can get accounts banned, so PortXR stays out.", g.blocker.unwrap_or("Anti-cheat")), Tone::Bad),
        (Compat::NativeVr, _) => ("Has its own VR mode: start it in VR from Steam. Add VR anyway to try PortXR instead.".into(), Tone::Info),
        (Compat::LinuxBuild, _) if g.app_id > 0 => ("Steam installed the Linux version. In Steam: Properties → Compatibility → force Proton, then come back.".into(), Tone::Info),
        (Compat::LinuxBuild, _) => ("This is the game's Linux version. PortXR goes into its Windows version, run with Proton.".into(), Tone::Info),
        (Compat::Supported, _) if g.launcher.as_ref().is_some_and(|l| l.plain_wine.is_some()) => (format!(
            "{} runs this game with {}, which gives games no OpenXR. Pick a Proton-based runner for it there, such as GE-Proton.",
            g.launcher_name(), g.launcher.as_ref().and_then(|l| l.plain_wine.as_deref()).unwrap_or("Wine")), Tone::Info),
        (Compat::Supported, _) if games::needs_launch_options(g) => (format!("To reach the headset, {}", launch_options_note(g)), Tone::Info),
        (Compat::X86, _) => ("32-bit game: PortXR is built for 64-bit games only.".into(), Tone::Info),
        (Compat::Stripped, _) => ("Built with Unity code trimming, which usually stops the mod loader. Add VR anyway to try.".into(), Tone::Info),
        (_, Some(l)) if !g.installed() => (format!("{l} is already installed. PortXR adds itself alongside; other mods may interfere."), Tone::Info),
        _ => (String::new(), Tone::Info),
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        self.shot(ui.ctx());
        // The name in line with the page's left edge (32); the right edge over the content's, which stops short of
        // the scroll bar.
        let bar = egui::Frame::new().fill(SURFACE).inner_margin(Margin { left: 32, right: 40, top: 16, bottom: 16 });
        egui::Panel::top("top").frame(bar).show(ui, |ui| {
            ui.horizontal(|ui| {
                // The name, then the places (tabs, left); the one action of the page sits alone on the right.
                ui.label(RichText::new("PORTXR").font(display(BRAND)).color(INK));
                ui.add_space(32.0);
                ui.spacing_mut().item_spacing.x = 24.0;
                if nav(ui, "Library", matches!(self.page, Page::Library | Page::Game(_))).clicked() && self.page != Page::Library {
                    self.back_to = None;
                    self.back();
                }
                if nav(ui, "Settings", self.page == Page::Settings).on_hover_text("Settings for all games").clicked() && self.page != Page::Settings {
                    self.back_to = if let Page::Game(i) = self.page { Some(i) } else { None };
                    self.page = Page::Settings;
                }
                if nav(ui, "About", self.page == Page::About).clicked() && self.page != Page::About {
                    self.back_to = None;
                    self.page = Page::About;
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = 24.0;
                    if self.page == Page::Library && secondary(ui, "Add game +").clicked() {
                        self.add_game();
                    }
                    if let Some(r) = self.update.lock().unwrap().clone() {
                        let text = RichText::new(format!("UPDATE {} ↗", r.tag_name.trim_start_matches('v'))).font(mono(LABEL)).color(lime());
                        ui.hyperlink_to(text, r.html_url);
                    }
                });
            });
        });
        egui::CentralPanel::default().frame(egui::Frame::new().fill(BG)).show(ui, |ui| {
            // Each page keeps its own scroll position; a game page, Settings or About opens at the top.
            let entered = self.shown_page != Some(self.page);
            self.shown_page = Some(self.page);
            let salt = match self.page { Page::Library => 0, Page::Game(_) => 1, Page::Settings => 2, Page::About => 3 };
            let mut area = egui::ScrollArea::vertical().id_salt(salt).auto_shrink(false);
            if entered && self.page != Page::Library {
                area = area.vertical_scroll_offset(0.0);
            }
            area.show(ui, |ui| {
                egui::Frame::new().inner_margin(Margin::same(32)).show(ui, |ui| match self.page {
                    Page::Library => self.library(ui),
                    Page::Game(i) => self.game_page(ui, i),
                    Page::Settings => self.settings_page(ui),
                    Page::About => self.about_page(ui),
                })
            })
        });
        // A slider drag changes the value every frame; write the file once it is let go.
        if self.dirty && ui.ctx().dragged_id().is_none() {
            self.dirty = false;
            let saved = match self.page {
                Page::Game(i) => save_cfg(&self.games[i], &self.cfg),
                _ => self.save_defaults(),
            };
            if let Err(e) = saved {
                self.status = (format!("Could not save settings: {e}"), Tone::Bad);
            }
        }
        let focused = ui.input(|i| i.viewport().focused.unwrap_or(true));
        if self.page == Page::Library && ((focused && !self.focused) || ui.input(|i| i.key_pressed(egui::Key::F5))) {
            self.scan_in_background(ui.ctx());
        }
        // A finished scan waits for the library: replacing the list under an open game page would move its index.
        if self.page == Page::Library
            && let Some(found) = self.scanned.lock().unwrap().take() {
                self.scanning = false;
                self.games = found;
            }
        if let Some(c) = self.catalogue_new.lock().unwrap().take() {
            self.catalogue = c;
        }
        let got = self.got.lock().unwrap().take();
        if let Some((dir, result)) = got {
            self.getting = None;
            self.finish_get(&dir, result);
        }
        self.focused = focused;
        let back = ui.input(|i| i.key_pressed(egui::Key::Escape) || i.pointer.button_pressed(egui::PointerButton::Extra1));
        if back && self.page != Page::Library && !self.dirty && ui.ctx().memory(|m| m.focused().is_none()) {
            self.back();
        }
    }
}

impl App {
    fn back(&mut self) {
        match (self.page, self.back_to.take()) {
            (Page::Settings, Some(i)) => self.open(i),
            _ => {
                self.page = Page::Library;
                self.status = (String::new(), Tone::Info);
            }
        }
    }

    // Writes the all-games values, then moves every game still on an old value to the new one; a game whose value
    // differs was changed for that game (on its page or in its VR menu) and keeps it.
    fn save_defaults(&mut self) -> std::io::Result<()> {
        write(&defaults_path(), self.defaults.as_bytes())?;
        let old = std::mem::replace(&mut self.defaults_saved, self.defaults.clone());
        for g in self.games.iter().filter(|g| g.compat() == Compat::Supported) {
            let text = load_cfg(g);
            // No config yet: it is built from the all-games values on install.
            if text.is_empty() {
                continue;
            }
            let updated = settings::follow(&text, &old, &self.defaults);
            if updated != text {
                save_cfg(g, &updated)?;
            }
        }
        Ok(())
    }
}

// ---- components -------------------------------------------------------------------------------------------------

fn tone_color(tone: Tone) -> Color32 {
    match tone {
        Tone::Info => INK2,
        Tone::Good => lime(),
        Tone::Bad => DANGER,
    }
}

// Keyboard focus is always visible: a lime ring just outside the control.
fn focus_ring(ui: &Ui, r: &egui::Response) {
    if r.has_focus() {
        ui.painter().rect_stroke(r.rect.expand(3.0), RADIUS, Stroke::new(2.0, lime()), egui::StrokeKind::Outside);
    }
}

fn label_format(color: Color32) -> egui::TextFormat {
    egui::TextFormat { font_id: mono(LABEL), color, extra_letter_spacing: TRACK, ..Default::default() }
}

// "NN // WORD": number ink3, word lime. The same run is the Advanced toggle, so both read as one label style.
fn eyebrow_text(number: &str, word: &str) -> egui::text::LayoutJob {
    let mut job = egui::text::LayoutJob::default();
    if !number.is_empty() {
        job.append(&format!("{number} // "), 0.0, label_format(INK3));
    }
    job.append(&word.to_uppercase(), 0.0, label_format(lime()));
    job
}

fn eyebrow(ui: &mut Ui, number: &str, word: &str) {
    ui.label(eyebrow_text(number, word));
}

// Between two sections: space, a hairline, space. Sections never get boxes.
fn section_break(ui: &mut Ui) {
    ui.add_space(32.0);
    hairline(ui);
    ui.add_space(24.0);
}

// A row of quiet links, each opening in the browser.
fn links(ui: &mut Ui, links: &[(&str, String)]) {
    ui.horizontal_wrapped(|ui| {
        ui.spacing_mut().item_spacing.x = 24.0;
        for (text, url) in links {
            if tertiary(ui, &format!("{text} ↗")).clicked() {
                let _ = games::open(url);
            }
        }
    });
}

// One line of feedback under the thing it is about: ink2 info, lime success, danger error.
// How long ago, as people say it: "today", "yesterday", "3 days ago", "2 weeks ago", "5 months ago".
fn ago(t: std::time::SystemTime) -> String {
    let days = t.elapsed().map_or(0, |d| d.as_secs() / 86_400);
    match days {
        0 => "today".into(),
        1 => "yesterday".into(),
        2..=13 => format!("{days} days ago"),
        14..=59 => format!("{} weeks ago", days / 7),
        _ => format!("{} months ago", days / 30),
    }
}

fn status_line(ui: &mut Ui, text: &str, tone: Tone) {
    ui.add_space(8.0);
    ui.label(RichText::new(text).color(tone_color(tone)));
}

fn hairline(ui: &mut Ui) {
    let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), 1.0), Sense::hover());
    ui.painter().rect_filled(rect, 0.0, LINE);
}

fn button_text(text: &str, color: Color32) -> RichText {
    RichText::new(text.to_uppercase()).font(body(SMALL)).color(color).extra_letter_spacing(TRACK)
}

// Seconds for an animation, or none when Windows' "Animation effects" is off.
fn motion(seconds: f32) -> f32 {
    use std::sync::OnceLock;
    static ON: OnceLock<bool> = OnceLock::new();
    if *ON.get_or_init(crate::animations_enabled) { seconds } else { 0.0 }
}

// 0..1, easing toward `on` over `seconds`.
fn ease(ui: &Ui, id: egui::Id, on: bool, seconds: f32) -> f32 {
    ui.ctx().animate_bool_with_time(id, on, motion(seconds))
}

fn mix(a: Color32, b: Color32, t: f32) -> Color32 {
    Color32::from_rgb(lerp(a.r(), b.r(), t), lerp(a.g(), b.g(), t), lerp(a.b(), b.b(), t))
}

fn primary(ui: &mut Ui, text: &str) -> egui::Response {
    let enabled = ui.is_enabled();
    let ink = if enabled { ON_LIME } else { INK3 };
    let b = egui::Button::new(button_text(text, ink)).fill(Color32::TRANSPARENT).stroke(Stroke::NONE).corner_radius(RADIUS).min_size(vec2(0.0, CONTROL_H));
    let r = ui.add(b);
    // Pressed answers at once; release eases back.
    let down = r.is_pointer_button_down_on();
    let t = ease(ui, r.id.with("press"), down, if down { 0.0 } else { FAST });
    let fill = if enabled { mix(lime(), lime_pressed(), t) } else { RAISED };
    ui.painter().rect_filled(r.rect, RADIUS, fill);
    paint_label(ui.painter(), r.rect, text, ink);
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn secondary(ui: &mut Ui, text: &str) -> egui::Response {
    let r = ui.add(egui::Button::new(button_text(text, INK)).corner_radius(RADIUS).min_size(vec2(0.0, CONTROL_H)));
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Top-bar tab, drawn like the VR menu's tabs: mono word, ink2, ink and a rounded lime bar on the bar's bottom edge
// when active. The bar grows from the word's middle as a tab becomes active and shrinks on the one it leaves.
fn nav(ui: &mut Ui, text: &str, active: bool) -> egui::Response {
    let old = std::mem::replace(&mut ui.spacing_mut().button_padding, vec2(0.0, 0.0));
    let word = RichText::new(text.to_uppercase()).font(mono(SMALL)).color(if active { INK } else { INK2 }).extra_letter_spacing(TRACK);
    let r = ui.add(egui::Button::new(word).frame(false).min_size(vec2(0.0, CONTROL_H)));
    ui.spacing_mut().button_padding = old;
    // The bar's bottom margin is 16: the marker sits on its bottom edge, like the VR tab underline.
    let t = ease(ui, r.id.with("tab"), active, 0.28);
    let hover = ease(ui, r.id.with("hover"), r.hovered() && !active, FAST);
    let bar = |h: f32, k: f32| {
        let half = r.rect.width() * 0.5 * k;
        Rect::from_min_max(pos2(r.rect.center().x - half, r.rect.bottom() + 16.0 - h), pos2(r.rect.center().x + half, r.rect.bottom() + 16.0))
    };
    if t > 0.01 {
        ui.painter().rect_filled(bar(4.0, t), 2.0, lime());
    }
    if hover > 0.01 {
        ui.painter().rect_filled(bar(2.0, 1.0), 1.0, INK2.gamma_multiply(hover));
    }
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Quiet text action for occasional things: no box, underlined on hover.
fn tertiary(ui: &mut Ui, text: &str) -> egui::Response {
    link(ui, text, INK2)
}

fn link(ui: &mut Ui, text: &str, ink: Color32) -> egui::Response {
    let old = std::mem::replace(&mut ui.spacing_mut().button_padding, vec2(0.0, 0.0));
    let r = ui.add(egui::Button::new(button_text(text, ink)).frame(false).min_size(vec2(0.0, CONTROL_H)));
    ui.spacing_mut().button_padding = old;
    if r.hovered() {
        ui.painter().hline(r.rect.x_range(), r.rect.center().y + 9.0, Stroke::new(1.0, ink));
    }
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// The smallest action: a mono label line, for "Reset to …" under a changed setting's name.
fn small_link(ui: &mut Ui, text: &str) -> egui::Response {
    let b = egui::Button::new(RichText::new(text.to_uppercase()).font(mono(LABEL)).color(INK2).extra_letter_spacing(TRACK)).frame(false).min_size(vec2(0.0, 24.0));
    let old = std::mem::replace(&mut ui.spacing_mut().button_padding, vec2(0.0, 4.0));
    let r = ui.add(b);
    ui.spacing_mut().button_padding = old;
    if r.hovered() {
        ui.painter().hline(r.rect.x_range(), r.rect.bottom() - 2.0, Stroke::new(1.0, INK2));
    }
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn destructive(ui: &mut Ui, text: &str, armed: bool) -> egui::Response {
    let mut b = egui::Button::new(button_text(text, if armed { ON_LIME } else { DANGER_INK })).corner_radius(RADIUS).min_size(vec2(0.0, CONTROL_H));
    b = b.fill(if armed { DANGER } else { DANGER_BG });
    let r = ui.add(b);
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Cells side by side in a rounded track, the chosen one filled ink (WristXR's segment). `lime` fills the chosen
// one lime instead, for the library's status filter. The fill eases from the old choice to the new one.
fn segmented<T: Copy + PartialEq>(ui: &mut Ui, options: &[(T, &str)], current: T, lime: bool) -> Option<T> {
    let mut picked = None;
    let frame = egui::Frame::new().fill(RAISED).corner_radius(RADIUS + 2).inner_margin(Margin::same(4));
    frame.show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 0.0;
            for &(value, label) in options {
                let on = value == current;
                // Sized from the label itself: an unframed button has no padding of its own.
                let w = label_width(ui, label) + 32.0;
                let (_, r) = ui.allocate_exact_size(vec2(w, CONTROL_H - 4.0), Sense::click());
                r.widget_info(|| egui::WidgetInfo::selected(egui::Role::Button, true, on, label));
                let t = ease(ui, r.id.with("on"), on, FAST);
                let hover = ease(ui, r.id.with("hover"), r.hovered() && !on, FAST);
                let chosen = if lime { self::lime() } else { INK };
                let fill = mix(mix(RAISED, LINE, hover), chosen, t);
                ui.painter().rect_filled(r.rect, CELL_RADIUS, fill);
                let ink = mix(INK2, ON_LIME, t);
                paint_label(ui.painter(), r.rect, label, ink);
                focus_ring(ui, &r);
                if r.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() && !on {
                    picked = Some(value);
                }
            }
        });
    });
    picked
}

// One dot per theme, the chosen one ringed, its name beside them; returns a newly picked one.
fn theme_picker(ui: &mut Ui) -> Option<usize> {
    let current = THEME.load(std::sync::atomic::Ordering::Relaxed);
    let mut picked = None;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 8.0;
        for (i, &(name, c)) in THEMES.iter().enumerate() {
            let on = i == current;
            let (rect, r) = ui.allocate_exact_size(vec2(CONTROL_H, CONTROL_H), Sense::click());
            r.widget_info(|| egui::WidgetInfo::selected(egui::Role::Button, true, on, name));
            let hover = ease(ui, r.id.with("hover"), r.hovered() && !on, FAST);
            let ring = CONTROL_H / 2.0 - 2.0;
            if on {
                ui.painter().circle_stroke(rect.center(), ring, Stroke::new(2.0, INK));
            } else if hover > 0.01 {
                ui.painter().circle_stroke(rect.center(), ring, Stroke::new(2.0, mix(RAISED, INK3, hover)));
            }
            ui.painter().circle_filled(rect.center(), CONTROL_H / 2.0 - 9.0, rgb(c));
            focus_ring(ui, &r);
            if r.on_hover_text(name).on_hover_cursor(egui::CursorIcon::PointingHand).clicked() && !on {
                picked = Some(i);
            }
        }
        ui.add_space(8.0);
        ui.label(RichText::new(THEMES[current].0).color(INK2));
    });
    picked
}

fn label_width(ui: &Ui, text: &str) -> f32 {
    let mut job = egui::text::LayoutJob::default();
    job.append(&text.to_uppercase(), 0.0, egui::TextFormat { font_id: body(SMALL), extra_letter_spacing: TRACK, ..Default::default() });
    ui.painter().layout_job(job).size().x
}

// A button's label painted centred in `rect`: upper case with the button tracking.
fn paint_label(p: &Painter, rect: Rect, text: &str, color: Color32) {
    let mut job = egui::text::LayoutJob::default();
    job.append(&text.to_uppercase(), 0.0, egui::TextFormat { font_id: body(SMALL), color, extra_letter_spacing: TRACK, ..Default::default() });
    let galley = p.layout_job(job);
    p.galley(rect.center() - galley.size() / 2.0, galley, color);
}

#[derive(Clone, Copy)]
enum ChipKind {
    /// Full VR, on this PC (Play in VR adds VR when it is missing).
    Ready,
    /// Full VR once installed (the not-installed shelf).
    FullVr,
    /// The game on the VR screen.
    Screen,
    NotInstalled,
    Blocked,
    Unsupported,
}

fn chip_kind(g: &Game) -> ChipKind {
    match g.compat() {
        Compat::Theater => ChipKind::Screen,
        _ if g.usable() => ChipKind::Ready,
        Compat::Blocked => ChipKind::Blocked,
        _ => ChipKind::Unsupported,
    }
}

// Mono label with a status dot; the word always says the state, colour only reinforces it.
fn paint_chip(p: &Painter, at: egui::Pos2, text: &str, kind: ChipKind) -> f32 {
    let (dot, ink, fill) = match kind {
        // Strongest to weakest: full VR (lime), full VR once installed (lime dot), VR screen (white dot), not on this PC (grey).
        ChipKind::Ready => (None, ON_LIME, lime()),
        ChipKind::FullVr => (Some(lime()), INK2, RAISED),
        ChipKind::Screen => (Some(INK), INK2, RAISED),
        ChipKind::NotInstalled => (None, INK3, RAISED),
        ChipKind::Blocked => (Some(DANGER), DANGER_INK, DANGER_BG),
        ChipKind::Unsupported => (Some(INK3), INK3, RAISED),
    };
    let galley = p.layout_no_wrap(text.to_uppercase(), mono(LABEL), ink);
    let pad = if dot.is_some() { 22.0 } else { 10.0 };
    let rect = Rect::from_min_size(at, vec2(galley.size().x + pad + 10.0, 26.0));
    p.rect_filled(rect, 8, fill);
    if let Some(c) = dot {
        p.circle_filled(pos2(rect.left() + 12.0, rect.center().y), 3.0, c);
    }
    p.galley(pos2(rect.left() + pad, rect.center().y - galley.size().y / 2.0), galley, ink);
    rect.width()
}

fn chip(ui: &mut Ui, text: &str, kind: ChipKind) {
    let width = ui.painter().layout_no_wrap(text.to_uppercase(), mono(LABEL), INK).size().x + if matches!(kind, ChipKind::Ready | ChipKind::NotInstalled) { 20.0 } else { 32.0 };
    let (rect, _) = ui.allocate_exact_size(vec2(width, 26.0), Sense::hover());
    paint_chip(ui.painter(), rect.min, text, kind);
}

// A catalogue rating: a coloured dot and the words ("GOOD IN FULL VR"), never the colour alone.
fn paint_rating(p: &Painter, at: egui::Pos2, rating: &str, mode: Option<&str>) -> f32 {
    let (dot, word) = match rating {
        "perfect" => (OK, "Perfect"),
        "good" => (OK, "Good"),
        "playable" => (WARN, "Playable"),
        _ => (DANGER, "Broken"),
    };
    let text = mode.map_or(word.to_string(), |m| format!("{word} in {m}"));
    let galley = p.layout_no_wrap(text.to_uppercase(), mono(LABEL), INK2);
    let rect = Rect::from_min_size(at, vec2(galley.size().x + 32.0, 26.0));
    p.rect_filled(rect, 8, RAISED);
    p.circle_filled(pos2(rect.left() + 12.0, rect.center().y), 3.0, dot);
    p.galley(pos2(rect.left() + 22.0, rect.center().y - galley.size().y / 2.0), galley, INK2);
    rect.width()
}

// The catalogue's best rating beside the game's state chip. Hover lists every mode tried; a click opens the game's
// catalogue page.
fn rating_link(ui: &mut Ui, e: &catalogue::Entry) {
    let Some((mode, rating)) = e.best() else { return };
    let word = ui.painter().layout_no_wrap(format!("{rating} in {mode}").to_uppercase(), mono(LABEL), INK2).size().x;
    let (rect, resp) = ui.allocate_exact_size(vec2(word + 32.0, 26.0), Sense::click());
    paint_rating(ui.painter(), rect.min, rating, Some(mode));
    let modes: Vec<String> = catalogue::MODES.iter().filter_map(|m| e.ratings.get(*m).map(|r| format!("{m}: {r}"))).collect();
    let tip = format!("Game catalogue: {}. {} {}. Click for details.", modes.join(", "), e.reports, if e.reports == 1 { "report" } else { "reports" });
    // Only a catalogue page is opened: the address comes from a downloaded list and goes to the system's opener.
    if resp.on_hover_text(tip).on_hover_cursor(egui::CursorIcon::PointingHand).clicked() && e.url.starts_with("https://everyvr.net/portxr/catalogue/") {
        let _ = games::open(&e.url);
    }
}

const GAP: f32 = 16.0;

// Columns of at least 160 px that fill the row exactly, so the grid shares its edges with the toolbar's row.
fn columns(avail: f32) -> (usize, f32) {
    let cols = ((avail + GAP) / (160.0 + GAP)).floor().max(1.0) as usize;
    (cols, ((avail - GAP * (cols - 1) as f32) / cols as f32).floor())
}

// Placeholder cards in the grid's exact shape while games are found, so nothing jumps when they land. A soft pulse
// runs across them, a little later on each card.
fn skeleton_grid(ui: &mut Ui, n: usize) {
    let (cols, w) = columns(ui.available_width());
    let t = ui.input(|i| i.time) as f32;
    let shade = |k: usize| {
        let a = ((t * 2.4 - k as f32 * 0.35).sin() * 0.5 + 0.5) * 0.7;
        Color32::from_rgb(lerp(SURFACE.r(), RAISED.r(), a), lerp(SURFACE.g(), RAISED.g(), a), lerp(SURFACE.b(), RAISED.b(), a))
    };
    for (r, row) in (0..n).collect::<Vec<_>>().chunks(cols).enumerate() {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = GAP;
            for &k in row {
                let (rect, _) = ui.allocate_exact_size(vec2(w, w * 1.5 + 68.0), Sense::hover());
                let p = ui.painter();
                let image = Rect::from_min_size(rect.min, vec2(w, w * 1.5));
                p.rect_filled(image, RADIUS, shade(k + r));
                p.rect_filled(Rect::from_min_size(image.left_bottom() + vec2(0.0, 14.0), vec2(w * 0.7, 12.0)), RADIUS, shade(k + r));
                p.rect_filled(Rect::from_min_size(image.left_bottom() + vec2(0.0, 38.0), vec2(w * 0.45, 24.0)), RADIUS, shade(k + r));
            }
        });
        ui.add_space(24.0);
    }
    ui.ctx().request_repaint();
}

fn lerp(a: u8, b: u8, t: f32) -> u8 {
    (a as f32 + (b as f32 - a as f32) * t).round() as u8
}

fn card(ui: &mut Ui, g: &Game, w: f32, rating: Option<(&str, &str)>) -> egui::Response {
    let h = w * 1.5;
    let (rect, resp) = ui.allocate_exact_size(vec2(w, h + 68.0), Sense::click());
    resp.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Button, true, &g.name));
    let image = Rect::from_min_size(rect.min, vec2(w, h));
    cover(ui, g, image);
    hover_card(ui, &resp, image);
    let mut job = egui::text::LayoutJob::simple_singleline(g.name.clone(), body(BODY), INK);
    job.wrap = egui::text::TextWrapping::truncate_at_width(w);
    let name = ui.painter().layout_job(job);
    ui.painter().galley(image.left_bottom() + vec2(0.0, 12.0), name, INK);
    // Every card names its state, in the same words as the tabs.
    paint_chip(&ui.painter_at(rect), image.left_bottom() + vec2(0.0, 38.0), g.badge(), chip_kind(g));
    // The catalogue's best rating, on the cover's top corner: how well others found it plays.
    if let Some((_, r)) = rating {
        paint_rating(&ui.painter_at(image), image.min + vec2(10.0, 10.0), r, None);
    }
    focus_ring(ui, &resp);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Under the pointer a card's corner light comes up and a lime ring fades in around it.
fn hover_card(ui: &Ui, resp: &egui::Response, image: Rect) {
    let t = ease(ui, resp.id.with("hover"), resp.hovered(), FAST);
    if t > 0.01 {
        glow(&ui.painter_at(image), image.right_bottom(), 0.6 * t);
        ui.painter().rect_stroke(image, RADIUS, Stroke::new(2.0, lime().gamma_multiply(t)), egui::StrokeKind::Outside);
    }
}

// Faint lime light in one corner, the references' "glow": a radial gradient drawn as a fan mesh whose vertex colours
// fade to transparent, so the GPU interpolates it smoothly (stacked discs showed bands).
fn glow(p: &Painter, corner: egui::Pos2, strength: f32) {
    const STEPS: u32 = 64;
    let lime = |a: f32| Color32::from_rgba_unmultiplied(198, 255, 26, (a * strength) as u8);
    let mut mesh = egui::Mesh::default();
    mesh.colored_vertex(corner, lime(46.0));
    for ring in [(70.0, lime(18.0)), (170.0, Color32::TRANSPARENT)] {
        for i in 0..=STEPS {
            let a = i as f32 / STEPS as f32 * std::f32::consts::TAU;
            mesh.colored_vertex(corner + vec2(a.cos(), a.sin()) * ring.0, ring.1);
        }
    }
    let n = STEPS + 1;
    for i in 0..STEPS {
        mesh.add_triangle(0, 1 + i, 2 + i);
        let (a, b) = (1 + i, 1 + n + i);
        mesh.add_triangle(a, b, a + 1);
        mesh.add_triangle(a + 1, b, b + 1);
    }
    p.add(egui::Shape::mesh(mesh));
}

fn owned_card(ui: &mut Ui, o: &games::Owned, w: f32) -> egui::Response {
    let h = w * 1.5;
    let (rect, resp) = ui.allocate_exact_size(vec2(w, h + 68.0), Sense::click());
    resp.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Button, true, &o.name));
    let image = Rect::from_min_size(rect.min, vec2(w, h));
    cover_image(ui, &o.cover, &o.name, image);
    // Dimmed and marked: not on this PC yet.
    ui.painter_at(image).rect_filled(image, RADIUS, Color32::from_black_alpha(110));
    paint_chip(&ui.painter_at(image), image.min + vec2(10.0, 10.0), "Not installed", ChipKind::NotInstalled);
    hover_card(ui, &resp, image);
    let mut job = egui::text::LayoutJob::simple_singleline(o.name.clone(), body(BODY), INK);
    job.wrap = egui::text::TextWrapping::truncate_at_width(w);
    let name = ui.painter().layout_job(job);
    ui.painter().galley(image.left_bottom() + vec2(0.0, 12.0), name, INK);
    paint_chip(&ui.painter_at(rect), image.left_bottom() + vec2(0.0, 38.0), "Full VR", ChipKind::FullVr);
    focus_ring(ui, &resp);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn cover(ui: &mut Ui, g: &Game, rect: Rect) {
    cover_image(ui, &g.cover, &g.name, rect);
}

#[derive(Clone, Default)]
struct HeldCovers(HashMap<String, u64>);

// A cover's image, held while shown. Past 60 held (each about 2 MB decoded), the ones longest off screen are let go;
// one scrolled back loads again.
fn held_cover(ctx: &egui::Context, path: &Path) -> String {
    const HOLD: usize = 60;
    let uri = format!("file:///{}", path.display());
    let now = ctx.cumulative_pass_nr();
    let gone: Vec<String> = ctx.data_mut(|d| {
        let held = &mut d.get_temp_mut_or_default::<HeldCovers>(egui::Id::NULL).0;
        held.insert(uri.clone(), now);
        let mut old: Vec<(u64, String)> = held.iter().filter(|&(_, &t)| t + 1 < now).map(|(u, &t)| (t, u.clone())).collect();
        old.sort();
        old.truncate(held.len().saturating_sub(HOLD));
        old.into_iter().map(|(_, u)| held.remove(&u).map(|_| u).unwrap()).collect()
    });
    for u in gone {
        ctx.forget_image(&u);
    }
    uri
}

fn cover_image(ui: &mut Ui, cover: &Option<PathBuf>, name: &str, rect: Rect) {
    // Off screen: nothing to load (each cover held decoded is about 2 MB).
    if !ui.is_rect_visible(rect) {
        return;
    }
    match cover {
        // A wide header (no 2:3 cover cached) is shown whole, centred on the surface, never stretched.
        Some(path) if path.file_name().is_some_and(|n| n.to_string_lossy().ends_with("header.jpg")) => {
            ui.painter_at(rect).rect_filled(rect, RADIUS, SURFACE);
            let h = rect.width() * 215.0 / 460.0;
            let inner = Rect::from_center_size(rect.center(), vec2(rect.width(), h));
            egui::Image::new(held_cover(ui.ctx(), path)).show_loading_spinner(false).paint_at(ui, inner);
        }
        // The surface shows while the image loads, the shape of the card already in place.
        Some(path) => {
            ui.painter_at(rect).rect_filled(rect, RADIUS, SURFACE);
            egui::Image::new(held_cover(ui.ctx(), path)).corner_radius(RADIUS).show_loading_spinner(false).paint_at(ui, rect);
        }
        None => {
            let p = ui.painter_at(rect);
            p.rect_filled(rect, RADIUS, RAISED);
            glow(&p, rect.right_bottom(), 1.0);
            let initial = name.chars().next().unwrap_or('?').to_uppercase().to_string();
            p.text(rect.left_top() + vec2(18.0, 14.0), Align2::LEFT_TOP, initial, display(rect.width() * 0.35), PLACEHOLDER);
        }
    }
}

// ---- actions and storage ----------------------------------------------------------------------------------------

/// What a game under Proton needs with WiVRn or Monado, and where it goes (Steam's launch options, or the other
/// launcher's environment variables).
fn launch_options_note(g: &Game) -> String {
    let runtime = games::openxr_runtime().unwrap_or_default();
    if g.in_steam() {
        format!("{runtime} needs these in the game's Steam launch options: {}", games::LAUNCH_OPTIONS)
    } else {
        format!("{runtime} needs {} in the game's environment variables in {}.", games::OPENXR_ENV, g.launcher_name())
    }
}

fn install(g: &Game, defaults: &str) -> Result<String, Box<dyn Error>> {
    installer::install(&g.dir)?;
    configure(g, defaults)?;
    // Linux: set now, so the game starts in VR however it is started (its launcher, or the VR menu's Library).
    let next = games::wine_overrides(g).err().map(|why| format!(" {why}")).unwrap_or_default();
    Ok(format!("VR added.{next}"))
}

fn install_anyway(g: &Game, defaults: &str) -> Result<String, Box<dyn Error>> {
    installer::install_anyway(&g.dir)?;
    configure(g, defaults)?;
    // Linux: Play in VR sets the prefix up again, so what is still to do comes as a note.
    let next = games::wine_overrides(g).err().map(|why| format!(" {why}")).unwrap_or_default();
    Ok(format!("VR added anyway. Remove VR undoes it.{next}"))
}

fn configure(g: &Game, defaults: &str) -> Result<(), Box<dyn Error>> {
    // Unity's plugin and the Godot core read the same settings; older Godot games take their VR from the engine.
    if !g.core_vr() {
        return Ok(());
    }
    // The game's own settings (chosen before the first install, or kept from an earlier one) go in with the plugin;
    // everything else comes from the all-games values.
    let saved = fs::read_to_string(saved_path(g)).unwrap_or_default();
    write(&g.config_file(), settings::fill(&saved, defaults).as_bytes())?;
    Ok(())
}

fn play(g: &Game, defaults: &str) -> Result<String, Box<dyn Error>> {
    if g.compat() == Compat::Theater {
        return play_theater(g);
    }
    if games::steam_update_pending(&g.dir) {
        return Err("Steam has an update waiting. Let it finish, then press Play again.".into());
    }
    if !g.installed() {
        install(g, defaults)?;
    } else {
        installer::update(&g.dir).map_err(|e| format!("Couldn't update the plugin (is the game still running?): {e}"))?;
    }
    // Linux: the game's Wine prefix loads PortXR's loader from the game folder, set before every start.
    games::wine_overrides(g)?;
    // Godot 4.0 to 4.2 (the engine's own OpenXR) and Unreal (which looks for the headset as it starts) need the runtime
    // up before the game starts.
    if (g.godot_vr() && !g.godot_core()) || g.unreal_vr() {
        return play_godot(g);
    }
    games::share_with_prefixes(std::slice::from_ref(g));
    g.launch()?;
    games::mark_played(&g.dir);
    // Steam may hold freshly pasted launch options in memory until it exits, so this only reminds.
    if games::needs_launch_options(g) {
        return Ok(format!("Launching. If the game starts flat, {}", launch_options_note(g)));
    }
    Ok("Launching.".into())
}

// Games PortXR cannot go inside: PortXRTheater.exe (beside the app, in theater\) waits for the game's window and
// shows it on the Theater screen. Nothing is written to the game folder.
fn play_theater(g: &Game) -> Result<String, Box<dyn Error>> {
    let host = std::env::current_exe()?.with_file_name("theater").join("PortXRTheater.exe");
    if !host.exists() {
        return Err(format!("{} is missing; reinstall PortXR.", host.display()).into());
    }
    std::process::Command::new(&host).arg("--dir").arg(&g.dir).spawn()?;
    g.launch()?;
    games::mark_played(&g.dir);
    Ok("Launching. Double-tap the left menu button to bring the screen in front.".into())
}

// Godot opens VR itself as it starts, and gets none from a runtime that is still starting (SteamVR from cold): the
// Theater host wakes the runtime first (PortXRTheater.exe --wake, seconds), then the game is launched.
fn play_godot(g: &Game) -> Result<String, Box<dyn Error>> {
    // Linux: with no launcher to start it, launch() fails here and says what to do.
    if !cfg!(windows) && g.launch_url().is_none() {
        g.launch()?;
    }
    let host = std::env::current_exe()?.with_file_name("theater").join("PortXRTheater.exe");
    let game = g.clone();
    std::thread::spawn(move || {
        let _ = std::process::Command::new(&host).arg("--wake").status();
        let _ = game.launch();
    });
    games::mark_played(&g.dir);
    Ok("Starting VR, then the game.".into())
}

fn uninstall(g: &Game) -> Result<String, Box<dyn Error>> {
    // Keep the settings (including changes made in the VR menu) for the next install.
    if let Ok(cfg) = fs::read(g.config_file()) {
        write(&saved_path(g), &cfg)?;
    }
    installer::uninstall(&g.dir)?;
    Ok("VR removed.".into())
}

// Settings live in the game's BepInEx config while installed; a copy in %APPDATA% survives uninstalls.
fn saved_path(g: &Game) -> PathBuf {
    let stem: String = g.dir.file_name().unwrap_or_default().to_string_lossy().chars().filter(|c| c.is_alphanumeric() || " -_".contains(*c)).collect();
    dirs::config_dir().unwrap_or_default().join("PortXR").join("settings").join(format!("{stem}.cfg"))
}

fn defaults_path() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("PortXR").join("defaults.cfg")
}

fn load_cfg(g: &Game) -> String {
    let live = g.config_file();
    fs::read_to_string(if g.installed() && live.exists() { live } else { saved_path(g) }).unwrap_or_default()
}

fn save_cfg(g: &Game, text: &str) -> std::io::Result<()> {
    write(&saved_path(g), text.as_bytes())?;
    if g.installed() {
        write(&g.config_file(), text.as_bytes())?;
    }
    Ok(())
}

fn write(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    fs::create_dir_all(path.parent().unwrap())?;
    fs::write(path, bytes)
}

fn latest_release() -> Option<Release> {
    let url = format!("https://api.github.com/repos/{REPO}/releases/latest");
    let r: Release = ureq::get(&url).header("User-Agent", "PortXR").call().ok()?.body_mut().read_json().ok()?;
    (version(&r.tag_name) > version(env!("CARGO_PKG_VERSION"))).then_some(r)
}

fn version(s: &str) -> Vec<u32> {
    s.trim_start_matches('v').split('.').map(|p| p.chars().take_while(char::is_ascii_digit).collect::<String>().parse().unwrap_or(0)).collect()
}

// ---- style ------------------------------------------------------------------------------------------------------

// Display: Anton. Body: Barlow; labels: Barlow SemiBold (all bundled, SIL OFL). File paths: Cascadia Mono, else
// Consolas, which ship with Windows; egui's own fonts are the fallback (and the fonts on Linux).
fn style(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let dir = PathBuf::from(std::env::var("WINDIR").unwrap_or_else(|_| r"C:\Windows".into())).join("Fonts");
    let fallback = fonts.families[&FontFamily::Proportional].clone();
    fonts.font_data.insert("display".into(), Arc::new(egui::FontData::from_static(include_bytes!("../assets/Anton-Regular.ttf"))));
    fonts.families.insert(FontFamily::Name("display".into()), [vec!["display".to_string()], fallback.clone()].concat());
    fonts.font_data.insert("body".into(), Arc::new(egui::FontData::from_static(include_bytes!("../assets/Barlow-Regular.ttf"))));
    fonts.families.entry(FontFamily::Proportional).or_default().insert(0, "body".into());
    // Arrows Barlow lacks (↗); Segoe UI, which ships with Windows, fills them in.
    if let Ok(bytes) = fs::read(dir.join("segoeui.ttf")) {
        fonts.font_data.insert("symbols".into(), Arc::new(egui::FontData::from_owned(bytes)));
        fonts.families.entry(FontFamily::Proportional).or_default().insert(1, "symbols".into());
    }
    // Labels: Barlow SemiBold (bundled), Segoe UI for arrows it lacks.
    fonts.font_data.insert("label".into(), Arc::new(egui::FontData::from_static(include_bytes!("../assets/Barlow-SemiBold.ttf"))));
    let mut labels = vec!["label".to_string()];
    let mut code_family = Vec::new();
    if let Some(bytes) = ["CascadiaMono.ttf", "consola.ttf"].iter().find_map(|f| fs::read(dir.join(f)).ok()) {
        fonts.font_data.insert("code".into(), Arc::new(egui::FontData::from_owned(bytes)));
        code_family.push("code".to_string());
    }
    if fonts.font_data.contains_key("symbols") {
        labels.push("symbols".to_string());
        code_family.push("symbols".to_string());
    }
    let monospace = fonts.families[&FontFamily::Monospace].clone();
    fonts.families.insert(FontFamily::Name("mono".into()), [labels, fallback.clone()].concat());
    fonts.families.insert(FontFamily::Name("code".into()), [code_family, monospace].concat());
    ctx.set_fonts(fonts);

    ctx.all_styles_mut(|s| {
        s.visuals = egui::Visuals::dark();
        let v = &mut s.visuals;
        v.panel_fill = BG;
        v.window_fill = SURFACE;
        v.extreme_bg_color = RAISED;
        v.override_text_color = Some(INK);
        v.hyperlink_color = lime();
        // Lime selection with dark text on it; also the slider's filled part.
        v.selection.bg_fill = lime();
        v.selection.stroke = Stroke::new(1.0, ON_LIME);
        v.slider_trailing_fill = true;
        v.window_shadow = egui::Shadow::NONE;
        v.popup_shadow = egui::Shadow::NONE;
        v.widgets.noninteractive.bg_stroke = Stroke::new(1.0, LINE);
        v.widgets.noninteractive.fg_stroke = Stroke::new(1.0, INK2);
        // Filled cells, no outlines: a lighter fill under the pointer, lighter still while pressed.
        let states = [
            (&mut v.widgets.inactive, RAISED, RAISED),
            (&mut v.widgets.hovered, LINE, LINE),
            (&mut v.widgets.active, PRESSED, PRESSED),
            (&mut v.widgets.open, LINE, LINE),
        ];
        for (w, fill, weak) in states {
            w.bg_fill = fill;
            w.weak_bg_fill = weak;
            w.bg_stroke = Stroke::NONE;
            w.fg_stroke = Stroke::new(1.0, INK);
            w.corner_radius = egui::CornerRadius::same(RADIUS);
            w.expansion = 0.0;
        }
        s.spacing.item_spacing = vec2(8.0, 8.0);
        s.spacing.button_padding = vec2(18.0, 8.0);
        s.animation_time = motion(FAST);
        s.spacing.slider_width = 260.0;
        s.spacing.scroll = egui::style::ScrollStyle::solid();
        s.spacing.interact_size.y = CONTROL_H;
        for (style, size) in [(egui::TextStyle::Body, BODY), (egui::TextStyle::Button, SMALL), (egui::TextStyle::Small, LABEL)] {
            s.text_styles.insert(style, body(size));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn covers_off_screen_are_let_go() {
        let ctx = egui::Context::default();
        for pass in 0..5 {
            let mut out = ctx.run_pass(Default::default(), |ctx| {
                for i in 0..50 {
                    held_cover(ctx, Path::new(&format!("cover{pass}_{i}.jpg")));
                }
            });
            out.textures_delta.clear();
        }
        // This pass's and the last pass's covers stay (still on screen); older ones go once past 60.
        let held = ctx.data_mut(|d| d.get_temp::<HeldCovers>(egui::Id::NULL)).unwrap().0;
        assert_eq!(held.len(), 100);
        assert!(held.keys().all(|u| u.contains("cover3_") || u.contains("cover4_")));
    }
}
