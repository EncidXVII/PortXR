use std::error::Error;
use std::fs;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};

use eframe::egui::{self, Align, Align2, Color32, FontFamily, FontId, Layout, Margin, Painter, Rect, RichText, Sense, Stroke, Ui, pos2, vec2};
use serde::Deserialize;

use crate::games::{self, Compat, Game};
use crate::installer;
use crate::settings::{self, Def, Kind};

// "Night Lime" tokens, shared with the VR menu (Menu.cs): black stage, one lime accent, hairlines instead of boxes,
// sharp corners. Spacing scale 4/8/12/16/24/32/48/64.
const BG: Color32 = Color32::from_rgb(10, 10, 10);
const SURFACE: Color32 = Color32::from_rgb(18, 18, 18);
const RAISED: Color32 = Color32::from_rgb(26, 26, 26);
const LINE: Color32 = Color32::from_rgb(38, 38, 38);
const EDGE: Color32 = Color32::from_rgb(94, 94, 94);
const INK: Color32 = Color32::from_rgb(242, 242, 242);
const INK2: Color32 = Color32::from_rgb(163, 163, 163);
const INK3: Color32 = Color32::from_rgb(128, 128, 128);
const LIME: Color32 = Color32::from_rgb(198, 255, 26);
const ON_LIME: Color32 = Color32::from_rgb(10, 10, 10);
const DANGER: Color32 = Color32::from_rgb(255, 90, 60);
const RADIUS: u8 = 2;
const REPO: &str = "EncidXVII/Parallax";
const LOG: &str = r"BepInEx\LogOutput.log";

pub fn run(open: Option<PathBuf>) -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Parallax")
            .with_inner_size([1120.0, 720.0])
            .with_min_inner_size([760.0, 480.0]),
        ..Default::default()
    };
    eframe::run_native("Parallax", options, Box::new(|cc| Ok(Box::new(App::new(cc, open)))))
}

#[derive(Deserialize, Clone)]
struct Release {
    tag_name: String,
    html_url: String,
}

#[derive(Clone, Copy, PartialEq)]
enum Filter {
    All,
    Ready,
    Installed,
    Unsupported,
}

#[derive(Clone, Copy, PartialEq)]
enum Tone {
    Info,
    Good,
    Bad,
}

struct App {
    games: Vec<Game>,
    page: Option<usize>,
    status: (String, Tone),
    cfg: String,
    dirty: bool,
    query: String,
    filter: Filter,
    confirm_uninstall: bool,
    advanced: bool,
    update: Arc<Mutex<Option<Release>>>,
}

fn display(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("display".into()))
}

fn mono(size: f32) -> FontId {
    FontId::new(size, FontFamily::Name("mono".into()))
}

fn body(size: f32) -> FontId {
    FontId::proportional(size)
}

fn filter_of(g: &Game) -> Filter {
    match (g.compat(), g.installed()) {
        (Compat::Supported, true) => Filter::Installed,
        (Compat::Supported, false) => Filter::Ready,
        _ => Filter::Unsupported,
    }
}

impl App {
    fn new(cc: &eframe::CreationContext, open: Option<PathBuf>) -> Self {
        egui_extras::install_image_loaders(&cc.egui_ctx);
        style(&cc.egui_ctx);
        let update = Arc::new(Mutex::new(None));
        let (slot, ctx) = (update.clone(), cc.egui_ctx.clone());
        std::thread::spawn(move || {
            if let Some(r) = latest_release() {
                *slot.lock().unwrap() = Some(r);
                ctx.request_repaint();
            }
        });
        let mut app = App {
            games: Vec::new(),
            page: None,
            status: (String::new(), Tone::Info),
            cfg: String::new(),
            dirty: false,
            query: String::new(),
            filter: Filter::All,
            confirm_uninstall: false,
            advanced: false,
            update,
        };
        app.rescan();
        if let Some(dir) = open {
            app.open_dir(&dir);
        }
        app
    }

    fn rescan(&mut self) {
        self.games = games::scan();
        self.page = None;
    }

    // A game folder from the command line: remembered if new, then its page opens.
    fn open_dir(&mut self, dir: &Path) {
        let Some(game) = games::detect(dir) else {
            self.status = ("No Unity game in that folder.".into(), Tone::Bad);
            return;
        };
        if !self.games.iter().any(|g| g.dir == game.dir) {
            if let Err(e) = games::add_folder(dir) {
                self.status = (e.to_string(), Tone::Bad);
                return;
            }
            self.rescan();
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
            Ok(0) => self.status = (format!("No Unity games in {} or the folders inside it.", dir.display()), Tone::Bad),
            Ok(n) => {
                self.rescan();
                self.status = (format!("Added {n} games from {}.", dir.display()), Tone::Good);
            }
            Err(e) => self.status = (e.to_string(), Tone::Bad),
        }
    }

    fn open(&mut self, i: usize) {
        self.page = Some(i);
        self.status = (String::new(), Tone::Info);
        self.confirm_uninstall = false;
        self.cfg = load_cfg(&self.games[i]);
    }

    fn library(&mut self, ui: &mut Ui) {
        ui.label(RichText::new("YOUR GAMES.").font(display(44.0)).color(INK));
        ui.add_space(16.0);
        if !self.status.0.is_empty() {
            ui.label(RichText::new(&self.status.0).color(tone_color(self.status.1)));
            ui.add_space(16.0);
        }
        if self.games.is_empty() {
            self.empty(ui, "NO GAMES FOUND.", "Parallax looks in Steam, Epic Games and GOG. Games from anywhere else: add their folder, or a folder that holds several games.", "Add game");
            return;
        }
        let count = |f: Filter| self.games.iter().filter(|g| filter_of(g) == f).count();
        let cells = [
            (Filter::All, "All games", self.games.len()),
            (Filter::Ready, "Ready", count(Filter::Ready)),
            (Filter::Installed, "Installed", count(Filter::Installed)),
            (Filter::Unsupported, "Unsupported", count(Filter::Unsupported)),
        ];
        if let Some(f) = stat_strip(ui, &cells, self.filter) {
            self.filter = f;
        }
        ui.add_space(16.0);
        // Exactly one stat cell wide, so its right edge lines up with the first cell's divider.
        let search = egui::TextEdit::singleline(&mut self.query).hint_text("Search games").margin(Margin::symmetric(12, 10));
        ui.add_sized([ui.available_width() / 4.0, 40.0], search);
        ui.add_space(24.0);

        let query = self.query.to_lowercase();
        let shown: Vec<usize> = (0..self.games.len())
            .filter(|&i| {
                let g = &self.games[i];
                (self.filter == Filter::All || filter_of(g) == self.filter) && g.name.to_lowercase().contains(&query)
            })
            .collect();
        if shown.is_empty() {
            ui.label(RichText::new("Nothing matches this search and filter.").color(INK2));
            ui.add_space(12.0);
            if secondary(ui, "Show all games").clicked() {
                self.query.clear();
                self.filter = Filter::All;
            }
            return;
        }
        // Columns fill the row exactly, so the grid shares its right edge with the stat strip.
        const GAP: f32 = 16.0;
        let avail = ui.available_width();
        let cols = ((avail + GAP) / (160.0 + GAP)).floor().max(1.0) as usize;
        let w = ((avail - GAP * (cols - 1) as f32) / cols as f32).floor();
        let mut open = None;
        for row in shown.chunks(cols) {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = GAP;
                for &i in row {
                    if card(ui, &self.games[i], w).clicked() {
                        open = Some(i);
                    }
                }
            });
            ui.add_space(24.0);
        }
        if let Some(i) = open {
            self.open(i);
        }
    }

    fn empty(&mut self, ui: &mut Ui, title: &str, text: &str, action: &str) {
        ui.add_space(24.0);
        ui.label(RichText::new(title).font(display(32.0)).color(INK));
        ui.label(RichText::new(text).color(INK2));
        ui.add_space(16.0);
        if primary(ui, action).clicked() {
            self.add_game();
        }
    }

    fn game_page(&mut self, ui: &mut Ui, i: usize) {
        let g = self.games[i].clone();
        ui.horizontal_top(|ui| {
            let (rect, _) = ui.allocate_exact_size(vec2(200.0, 300.0), Sense::hover());
            cover(ui, &g, rect);
            ui.add_space(32.0);
            ui.vertical(|ui| {
                ui.add(egui::Label::new(RichText::new(g.name.to_uppercase()).font(display(48.0)).color(INK)).wrap());
                // Anton reserves a tall line; pull the metadata up so it groups with the title.
                ui.add_space(-10.0);
                ui.horizontal(|ui| {
                    chip(ui, &format!("Unity {}", g.unity), ChipKind::Plain);
                    chip(ui, g.backend(), ChipKind::Plain);
                    chip(ui, g.badge(), chip_kind(&g));
                });
                ui.add_space(8.0);
                ui.label(RichText::new(g.dir.display().to_string()).font(mono(12.0)).color(INK3));
                ui.add_space(24.0);
                ui.horizontal(|ui| self.actions(ui, &g));
                ui.add_space(12.0);
                let (text, tone) = if self.status.0.is_empty() { (warning(&g), Tone::Info) } else { self.status.clone() };
                let color = if tone == Tone::Info { INK } else { tone_color(tone) };
                if !text.is_empty() {
                    ui.label(RichText::new(text).color(color));
                }
                if g.compat() != Compat::Supported {
                    return;
                }
                ui.add_space(24.0);
                hairline(ui);
                ui.add_space(24.0);
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        eyebrow(ui, "01", "Settings");
                        ui.label(RichText::new("Saved as you change them. The game picks them up on its next start.").color(INK2));
                    });
                    ui.with_layout(Layout::right_to_left(Align::Min), |ui| {
                        if secondary(ui, "Reset to defaults").clicked() {
                            self.cfg = settings::reset(&self.cfg);
                            self.dirty = true;
                        }
                    });
                });
                ui.add_space(16.0);
                self.settings(ui, settings::QUICK);
                ui.add_space(24.0);
                hairline(ui);
                let header = egui::Button::new(
                    RichText::new(format!("02 // ADVANCED  {}", if self.advanced { "−" } else { "+" })).font(mono(12.0)).color(LIME).extra_letter_spacing(1.5),
                )
                .frame(false);
                if ui.add(header).clicked() {
                    self.advanced = !self.advanced;
                }
                if self.advanced {
                    ui.add_space(8.0);
                    self.settings(ui, settings::ADVANCED);
                    let file = g.file(installer::CONFIG);
                    if file.exists() {
                        ui.add_space(8.0);
                        if secondary(ui, "Open settings file").clicked() {
                            let _ = Command::new("notepad").arg(file).spawn();
                        }
                    }
                }
            });
        });
    }

    fn actions(&mut self, ui: &mut Ui, g: &Game) {
        ui.spacing_mut().item_spacing.x = 8.0;
        if ui.add_enabled_ui(g.compat() == Compat::Supported, |ui| primary(ui, "Play in VR")).inner.clicked() {
            self.report(play(g));
            self.confirm_uninstall = false;
        }
        if g.installed() {
            let label = if self.confirm_uninstall { "Confirm uninstall" } else { "Uninstall" };
            if destructive(ui, label, self.confirm_uninstall).clicked() {
                if self.confirm_uninstall {
                    self.report(uninstall(g));
                    self.cfg = load_cfg(g);
                }
                self.confirm_uninstall = !self.confirm_uninstall;
            }
        }
        if secondary(ui, "Open folder").clicked() {
            let _ = Command::new("explorer").arg(&g.dir).spawn();
        }
        if g.installed() && secondary(ui, "Export logs").clicked() {
            self.report(games::export_logs(g).map(|zip| {
                let _ = Command::new("explorer").raw_arg(format!("/select,\"{}\"", zip.display())).spawn();
                "Logs saved to your desktop. Attach the zip to your bug report.".into()
            }));
        }
        if g.file(LOG).exists() && secondary(ui, "Open log").clicked() {
            let _ = Command::new("notepad").arg(g.file(LOG)).spawn();
        }
    }

    fn report(&mut self, result: Result<String, Box<dyn Error>>) {
        self.status = match result {
            Ok(text) => (text, Tone::Good),
            Err(e) => (e.to_string(), Tone::Bad),
        };
    }

    fn settings(&mut self, ui: &mut Ui, defs: &[Def]) {
        egui::Grid::new(defs[0].key).num_columns(2).spacing(vec2(32.0, 16.0)).min_col_width(260.0).show(ui, |ui| {
            for d in defs {
                let current = settings::get(&self.cfg, d.section, d.key).unwrap_or_else(|| d.default.into());
                ui.label(RichText::new(d.label).color(INK));
                let new = match d.kind {
                    Kind::Slider(lo, hi) => {
                        let mut v: f32 = current.parse().unwrap_or_else(|_| d.default.parse().unwrap());
                        let step = if hi > 100.0 { 50.0 } else { 0.05 };
                        let changed = ui.add(egui::Slider::new(&mut v, lo..=hi).step_by(step).fixed_decimals(if hi > 100.0 { 0 } else { 2 })).changed();
                        changed.then(|| format!("{}", (v * 100.0).round() / 100.0))
                    }
                    Kind::Toggle => {
                        let on = current.eq_ignore_ascii_case("true");
                        segmented(ui, &[(false, "Off"), (true, "On")], on, Some(false)).map(|b| b.to_string())
                    }
                    Kind::Choice(options) => {
                        // An "Off" option is the neutral one: selected, it is outlined rather than lime.
                        let neutral = options.iter().find(|(v, _)| *v == "Off").map(|o| o.0);
                        segmented(ui, options, options.iter().find(|(v, _)| *v == current).map_or(options[0].0, |o| o.0), neutral)
                    }
                        .map(String::from),
                    Kind::Text => {
                        let mut v = current.clone();
                        let edit = egui::TextEdit::singleline(&mut v).desired_width(260.0).margin(Margin::symmetric(12, 8));
                        ui.add(edit).changed().then_some(v)
                    }
                };
                ui.end_row();
                if let Some(v) = new {
                    self.cfg = settings::set(&self.cfg, d.section, d.key, &v);
                    self.dirty = true;
                }
            }
        });
    }
}

fn warning(g: &Game) -> String {
    match (g.compat(), g.other_loader) {
        (Compat::AntiCheat, _) => format!("{} found. Injecting into anti-cheat games can get accounts banned, so this game is refused.", g.blocker.unwrap_or("Anti-cheat")),
        (Compat::X86, _) => "32-bit game: no VR runtime supports 32-bit apps.".into(),
        (Compat::Stripped, _) => "Built with Unity's code trimming, which removes engine code the mod loader needs. Not supported yet.".into(),
        (_, Some(l)) if !g.installed() => format!("{l} is already installed. Parallax adds itself alongside; other mods may interfere."),
        _ => String::new(),
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        let bar = egui::Frame::new().fill(BG).inner_margin(Margin::symmetric(32, 16)).stroke(Stroke::new(1.0, LINE));
        egui::Panel::top("top").frame(bar).show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(RichText::new("PARALLAX").font(display(22.0)).color(INK));
                if self.page.is_some() {
                    ui.add_space(24.0);
                    if secondary(ui, "← Library").clicked() {
                        self.page = None;
                        self.status = (String::new(), Tone::Info);
                    }
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if self.page.is_none() {
                        if secondary(ui, "Add game  +").clicked() {
                            self.add_game();
                        }
                        if secondary(ui, "Rescan").clicked() {
                            self.rescan();
                            self.status = (String::new(), Tone::Info);
                        }
                    }
                    if let Some(r) = self.update.lock().unwrap().clone() {
                        let text = RichText::new(format!("UPDATE {} ↗", r.tag_name.trim_start_matches('v'))).font(mono(12.0)).color(LIME);
                        ui.hyperlink_to(text, r.html_url);
                    }
                });
            });
        });
        let body = egui::Frame::new().fill(BG).inner_margin(Margin { left: 32, right: 32, top: 32, bottom: 32 });
        egui::CentralPanel::default().frame(body).show(ui, |ui| {
            egui::ScrollArea::vertical().auto_shrink(false).show(ui, |ui| match self.page {
                None => self.library(ui),
                Some(i) => self.game_page(ui, i),
            })
        });
        // A slider drag changes the value every frame; write the file once it is let go.
        if self.dirty && ui.ctx().dragged_id().is_none() {
            self.dirty = false;
            if let Some(g) = self.page.map(|i| self.games[i].clone()) {
                if let Err(e) = save_cfg(&g, &self.cfg) {
                    self.status = (format!("Could not save settings: {e}"), Tone::Bad);
                }
            }
        }
    }
}

// ---- components -------------------------------------------------------------------------------------------------

fn tone_color(tone: Tone) -> Color32 {
    match tone {
        Tone::Info => INK2,
        Tone::Good => LIME,
        Tone::Bad => DANGER,
    }
}

// Keyboard focus is always visible: a lime ring just outside the control.
fn focus_ring(ui: &Ui, r: &egui::Response) {
    if r.has_focus() {
        ui.painter().rect_stroke(r.rect.expand(3.0), RADIUS, Stroke::new(2.0, LIME), egui::StrokeKind::Outside);
    }
}

fn eyebrow(ui: &mut Ui, number: &str, word: &str) {
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 0.0;
        ui.label(RichText::new(format!("{number} // ")).font(mono(12.0)).color(INK3).extra_letter_spacing(1.5));
        ui.label(RichText::new(word.to_uppercase()).font(mono(12.0)).color(LIME).extra_letter_spacing(1.5));
    });
    ui.add_space(8.0);
}

fn hairline(ui: &mut Ui) {
    let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), 1.0), Sense::hover());
    ui.painter().rect_filled(rect, 0.0, LINE);
}

fn button_text(text: &str, color: Color32) -> RichText {
    RichText::new(text.to_uppercase()).font(body(13.0)).color(color).extra_letter_spacing(1.0)
}

fn primary(ui: &mut Ui, text: &str) -> egui::Response {
    let enabled = ui.is_enabled();
    let (fill, ink) = if enabled { (LIME, ON_LIME) } else { (RAISED, INK3) };
    let b = egui::Button::new(button_text(&format!("{text}  ↗"), ink)).fill(fill).stroke(Stroke::NONE).corner_radius(RADIUS).min_size(vec2(0.0, 40.0));
    let r = ui.add(b);
    if r.hovered() && enabled {
        ui.painter().rect_stroke(r.rect, RADIUS, Stroke::new(1.0, INK), egui::StrokeKind::Inside);
    }
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn secondary(ui: &mut Ui, text: &str) -> egui::Response {
    let r = ui.add(egui::Button::new(button_text(text, INK)).corner_radius(RADIUS).min_size(vec2(0.0, 40.0)));
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn destructive(ui: &mut Ui, text: &str, armed: bool) -> egui::Response {
    let mut b = egui::Button::new(button_text(text, if armed { ON_LIME } else { INK })).corner_radius(RADIUS).min_size(vec2(0.0, 40.0));
    if armed {
        b = b.fill(DANGER);
    }
    let r = ui.add(b);
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Cells side by side, the selected one filled lime; a selected `neutral` value (a toggle's Off) gets an ink outline
// instead, so lime always means on.
fn segmented<T: Copy + PartialEq>(ui: &mut Ui, options: &[(T, &str)], current: T, neutral: Option<T>) -> Option<T> {
    let mut picked = None;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 0.0;
        for &(value, label) in options {
            let on = value == current;
            let lit = on && Some(value) != neutral;
            let mut b = egui::Button::new(button_text(label, if lit { ON_LIME } else if on { INK } else { INK2 })).corner_radius(0).min_size(vec2(0.0, 36.0));
            if lit {
                b = b.fill(LIME).stroke(Stroke::new(1.0, LIME));
            } else if on {
                b = b.fill(RAISED).stroke(Stroke::new(1.0, INK));
            }
            let r = ui.add(b);
            focus_ring(ui, &r);
            if r.on_hover_cursor(egui::CursorIcon::PointingHand).clicked() && !on {
                picked = Some(value);
            }
        }
    });
    picked
}

// One row of numbers sharing hairline dividers, full width. Each cell is also the filter for its category: the
// selected one gets the lime number and a lime bar on top.
fn stat_strip(ui: &mut Ui, cells: &[(Filter, &str, usize)], current: Filter) -> Option<Filter> {
    let w = ui.available_width();
    let (rect, _) = ui.allocate_exact_size(vec2(w, 88.0), Sense::hover());
    let cell = w / cells.len() as f32;
    let mut picked = None;
    for (i, &(filter, label, n)) in cells.iter().enumerate() {
        let r = Rect::from_min_size(pos2(rect.left() + i as f32 * cell, rect.top()), vec2(cell, rect.height()));
        let resp = ui.interact(r, ui.scope_id().with(("stat", i)), Sense::click()).on_hover_cursor(egui::CursorIcon::PointingHand);
        let on = filter == current;
        let p = ui.painter();
        if resp.hovered() && !on {
            p.rect_filled(r, 0.0, RAISED);
        }
        if on {
            p.rect_filled(Rect::from_min_size(r.min, vec2(r.width(), 3.0)), 0.0, LIME);
        }
        p.text(pos2(r.left() + 20.0, r.top() + 14.0), Align2::LEFT_TOP, n.to_string(), display(32.0), if on { LIME } else { INK });
        p.text(pos2(r.left() + 20.0, r.bottom() - 16.0), Align2::LEFT_BOTTOM, label.to_uppercase(), mono(11.0), if on { INK } else { INK3 });
        focus_ring(ui, &resp);
        if resp.clicked() {
            picked = Some(filter);
        }
    }
    let p = ui.painter();
    p.rect_stroke(rect, 0.0, Stroke::new(1.0, LINE), egui::StrokeKind::Inside);
    for i in 1..cells.len() {
        p.rect_filled(Rect::from_min_size(pos2(rect.left() + i as f32 * cell, rect.top()), vec2(1.0, rect.height())), 0.0, LINE);
    }
    picked
}

#[derive(Clone, Copy)]
enum ChipKind {
    Plain,
    Ready,
    Installed,
    Blocked,
    Unsupported,
}

fn chip_kind(g: &Game) -> ChipKind {
    match (g.compat(), g.installed()) {
        (Compat::Supported, true) => ChipKind::Installed,
        (Compat::Supported, false) => ChipKind::Ready,
        (Compat::AntiCheat, _) => ChipKind::Blocked,
        _ => ChipKind::Unsupported,
    }
}

// Mono label with a status dot; the word always says the state, colour only reinforces it.
fn paint_chip(p: &Painter, at: egui::Pos2, text: &str, kind: ChipKind) -> f32 {
    let (dot, ink, fill) = match kind {
        ChipKind::Plain => (None, INK2, None),
        ChipKind::Ready => (Some(LIME), INK2, None),
        ChipKind::Installed => (None, ON_LIME, Some(LIME)),
        ChipKind::Blocked => (Some(DANGER), INK2, None),
        ChipKind::Unsupported => (Some(INK3), INK3, None),
    };
    let galley = p.layout_no_wrap(text.to_uppercase(), mono(11.0), ink);
    let pad = if dot.is_some() { 22.0 } else { 10.0 };
    let rect = Rect::from_min_size(at, vec2(galley.size().x + pad + 10.0, 24.0));
    match fill {
        Some(f) => {
            p.rect_filled(rect, RADIUS, f);
        }
        None => {
            p.rect_stroke(rect, RADIUS, Stroke::new(1.0, LINE), egui::StrokeKind::Inside);
        }
    }
    if let Some(c) = dot {
        p.circle_filled(pos2(rect.left() + 12.0, rect.center().y), 3.0, c);
    }
    p.galley(pos2(rect.left() + pad, rect.center().y - galley.size().y / 2.0), galley, ink);
    rect.width()
}

fn chip(ui: &mut Ui, text: &str, kind: ChipKind) {
    let width = ui.painter().layout_no_wrap(text.to_uppercase(), mono(11.0), INK).size().x + if matches!(kind, ChipKind::Plain | ChipKind::Installed) { 20.0 } else { 32.0 };
    let (rect, _) = ui.allocate_exact_size(vec2(width, 24.0), Sense::hover());
    paint_chip(ui.painter(), rect.min, text, kind);
}

fn card(ui: &mut Ui, g: &Game, w: f32) -> egui::Response {
    let h = w * 1.5;
    let (rect, resp) = ui.allocate_exact_size(vec2(w, h + 68.0), Sense::click());
    let image = Rect::from_min_size(rect.min, vec2(w, h));
    cover(ui, g, image);
    if resp.hovered() {
        glow(&ui.painter_at(image), image.right_bottom(), 0.6);
        ui.painter().rect_stroke(image, RADIUS, Stroke::new(1.0, LIME), egui::StrokeKind::Outside);
    }
    let mut job = egui::text::LayoutJob::simple_singleline(g.name.clone(), body(15.0), INK);
    job.wrap = egui::text::TextWrapping::truncate_at_width(w);
    let name = ui.painter().layout_job(job);
    ui.painter().galley(image.left_bottom() + vec2(0.0, 12.0), name, INK);
    // Ready is the normal case, so it gets no tag; only the exceptions do.
    let chip_text = match g.compat() {
        Compat::Supported if g.installed() => Some("Installed"),
        Compat::Supported => None,
        Compat::AntiCheat => Some("Anti-cheat"),
        Compat::X86 => Some("32-bit"),
        Compat::Stripped => Some("Trimmed"),
    };
    if let Some(text) = chip_text {
        paint_chip(&ui.painter_at(rect), image.left_bottom() + vec2(0.0, 38.0), text, chip_kind(g));
    }
    focus_ring(ui, &resp);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
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

fn cover(ui: &mut Ui, g: &Game, rect: Rect) {
    match &g.cover {
        Some(path) => egui::Image::new(format!("file:///{}", path.display())).corner_radius(RADIUS).paint_at(ui, rect),
        None => {
            let p = ui.painter_at(rect);
            p.rect_filled(rect, RADIUS, SURFACE);
            p.rect_stroke(rect, RADIUS, Stroke::new(1.0, LINE), egui::StrokeKind::Inside);
            glow(&p, rect.right_bottom(), 1.0);
            let initial = g.name.chars().next().unwrap_or('?').to_uppercase().to_string();
            p.text(rect.left_top() + vec2(16.0, 12.0), Align2::LEFT_TOP, initial, display(rect.width() * 0.4), LINE);
        }
    }
}

// ---- actions and storage ----------------------------------------------------------------------------------------

fn play(g: &Game) -> Result<String, Box<dyn Error>> {
    if !g.installed() {
        installer::install(&g.dir)?;
        // Settings chosen before the first install (or kept from an earlier one) go in with the plugin.
        if let Ok(saved) = fs::read(saved_path(g)) {
            write(&g.file(installer::CONFIG), &saved)?;
        }
    }
    g.launch()?;
    Ok("Launching. Put on your headset; double-tap the left menu button for the VR menu.".into())
}

fn uninstall(g: &Game) -> Result<String, Box<dyn Error>> {
    // Keep the settings (including changes made in the VR menu) for the next install.
    if let Ok(cfg) = fs::read(g.file(installer::CONFIG)) {
        write(&saved_path(g), &cfg)?;
    }
    installer::uninstall(&g.dir)?;
    Ok("Removed. The game folder is back to its original files.".into())
}

// Settings live in the game's BepInEx config while installed; a copy in %APPDATA% survives uninstalls.
fn saved_path(g: &Game) -> PathBuf {
    let stem: String = g.dir.file_name().unwrap_or_default().to_string_lossy().chars().filter(|c| c.is_alphanumeric() || " -_".contains(*c)).collect();
    dirs::config_dir().unwrap_or_default().join("Parallax").join("settings").join(format!("{stem}.cfg"))
}

fn load_cfg(g: &Game) -> String {
    let live = g.file(installer::CONFIG);
    fs::read_to_string(if g.installed() && live.exists() { live } else { saved_path(g) }).unwrap_or_default()
}

fn save_cfg(g: &Game, text: &str) -> std::io::Result<()> {
    write(&saved_path(g), text.as_bytes())?;
    if g.installed() {
        write(&g.file(installer::CONFIG), text.as_bytes())?;
    }
    Ok(())
}

fn write(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    fs::create_dir_all(path.parent().unwrap())?;
    fs::write(path, bytes)
}

fn latest_release() -> Option<Release> {
    let url = format!("https://api.github.com/repos/{REPO}/releases/latest");
    let r: Release = ureq::get(&url).header("User-Agent", "Parallax").call().ok()?.body_mut().read_json().ok()?;
    (version(&r.tag_name) > version(env!("CARGO_PKG_VERSION"))).then_some(r)
}

fn version(s: &str) -> Vec<u32> {
    s.trim_start_matches('v').split('.').map(|p| p.chars().take_while(char::is_ascii_digit).collect::<String>().parse().unwrap_or(0)).collect()
}

// ---- style ------------------------------------------------------------------------------------------------------

// Display: Anton (bundled, SIL OFL). Body: Segoe UI. Labels: Cascadia Mono, else Consolas. All but Anton ship with
// Windows; egui's own fonts stay as the fallback for symbols.
fn style(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let dir = PathBuf::from(std::env::var("WINDIR").unwrap_or_else(|_| r"C:\Windows".into())).join("Fonts");
    let fallback = fonts.families[&FontFamily::Proportional].clone();
    fonts.font_data.insert("display".into(), Arc::new(egui::FontData::from_static(include_bytes!("../assets/Anton-Regular.ttf"))));
    fonts.families.insert(FontFamily::Name("display".into()), [vec!["display".to_string()], fallback.clone()].concat());
    if let Ok(bytes) = fs::read(dir.join("segoeui.ttf")) {
        fonts.font_data.insert("body".into(), Arc::new(egui::FontData::from_owned(bytes)));
        fonts.families.entry(FontFamily::Proportional).or_default().insert(0, "body".into());
    }
    let mut mono_family = Vec::new();
    if let Some(bytes) = ["CascadiaMono.ttf", "consola.ttf"].iter().find_map(|f| fs::read(dir.join(f)).ok()) {
        fonts.font_data.insert("mono".into(), Arc::new(egui::FontData::from_owned(bytes)));
        mono_family.push("mono".to_string());
    }
    fonts.families.insert(FontFamily::Name("mono".into()), [mono_family, fallback].concat());
    ctx.set_fonts(fonts);

    ctx.all_styles_mut(|s| {
        s.visuals = egui::Visuals::dark();
        let v = &mut s.visuals;
        v.panel_fill = BG;
        v.window_fill = SURFACE;
        v.extreme_bg_color = SURFACE;
        v.override_text_color = Some(INK);
        v.hyperlink_color = LIME;
        v.selection.bg_fill = LIME.gamma_multiply(0.35);
        v.selection.stroke = Stroke::new(1.0, LIME);
        v.slider_trailing_fill = true;
        v.window_shadow = egui::Shadow::NONE;
        v.popup_shadow = egui::Shadow::NONE;
        v.widgets.noninteractive.bg_stroke = Stroke::new(1.0, LINE);
        v.widgets.noninteractive.fg_stroke = Stroke::new(1.0, INK2);
        let states = [
            (&mut v.widgets.inactive, SURFACE, Color32::TRANSPARENT, EDGE),
            (&mut v.widgets.hovered, RAISED, RAISED, INK2),
            (&mut v.widgets.active, RAISED, RAISED, LIME),
            (&mut v.widgets.open, RAISED, RAISED, LINE),
        ];
        for (w, fill, weak, stroke) in states {
            w.bg_fill = fill;
            w.weak_bg_fill = weak;
            w.bg_stroke = Stroke::new(1.0, stroke);
            w.fg_stroke = Stroke::new(1.0, INK);
            w.corner_radius = egui::CornerRadius::same(RADIUS);
            w.expansion = 0.0;
        }
        s.spacing.item_spacing = vec2(8.0, 8.0);
        s.spacing.button_padding = vec2(16.0, 8.0);
        s.spacing.slider_width = 260.0;
        s.spacing.interact_size.y = 36.0;
        for (style, size) in [(egui::TextStyle::Body, 15.0), (egui::TextStyle::Button, 13.0), (egui::TextStyle::Small, 12.0)] {
            s.text_styles.insert(style, body(size));
        }
    });
}
