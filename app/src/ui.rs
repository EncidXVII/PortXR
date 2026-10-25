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
// Shape and rhythm shared by every control: sharp corners, one control height, one uppercase tracking.
const RADIUS: u8 = 0;
const CONTROL_H: f32 = 40.0;
// Widest a page's column grows on a big window, so rows and their links stay together.
const PAGE_W: f32 = 1040.0;
const TRACK: f32 = 1.2;
// Type scale, nothing in between: Anton titles, Segoe UI body and small text, mono labels.
const TITLE: f32 = 48.0;
const HEADING: f32 = 32.0;
const BRAND: f32 = 22.0;
const BODY: f32 = 15.0;
const SMALL: f32 = 13.0;
const LABEL: f32 = 12.0;
const REPO: &str = "EncidXVII/Parallax";
const LOG: &str = r"BepInEx\LogOutput.log";

pub fn run(open: Option<PathBuf>) -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Parallax")
            .with_inner_size([1120.0, 720.0])
            .with_min_inner_size([960.0, 560.0]),
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
}

struct App {
    games: Vec<Game>,
    page: Page,
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
    advanced: bool,
    buttons: bool,
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
        let defaults = fs::read_to_string(defaults_path()).unwrap_or_default();
        let mut app = App {
            games: Vec::new(),
            page: Page::Library,
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
            advanced: false,
            buttons: false,
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
        self.page = Page::Library;
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
        self.page = Page::Game(i);
        self.status = (String::new(), Tone::Info);
        self.confirm_uninstall = false;
        self.cfg = load_cfg(&self.games[i]);
    }

    fn library(&mut self, ui: &mut Ui) {
        ui.horizontal(|ui| {
            ui.label(RichText::new("YOUR GAMES.").font(display(TITLE)).color(INK));
            if !self.games.is_empty() {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    let search = egui::TextEdit::singleline(&mut self.query).hint_text("Search games").margin(Margin::symmetric(12, 10));
                    ui.add_sized([280.0, CONTROL_H], search);
                });
            }
        });
        if !self.status.0.is_empty() {
            status_line(ui, &self.status.0, self.status.1);
        }
        ui.add_space(24.0);
        if self.games.is_empty() {
            self.empty(ui, "NO GAMES FOUND.", "Parallax looks in Steam, Epic Games and GOG. Games from anywhere else: add their folder, or a folder that holds several games.", "Add game +");
            return;
        }
        self.toolbar(ui);
        ui.add_space(32.0);

        let query = self.query.to_lowercase();
        let mut shown: Vec<usize> = (0..self.games.len())
            .filter(|&i| {
                let g = &self.games[i];
                (self.filter == Filter::All || filter_of(g) == self.filter)
                    && self.store.is_none_or(|s| g.store == s)
                    && g.name.to_lowercase().contains(&query)
            })
            .collect();
        // Games arrive sorted by name; a stable sort keeps that order among games never played.
        if self.sort == Sort::Recent {
            shown.sort_by_key(|&i| std::cmp::Reverse(self.games[i].last_played));
        }
        if shown.is_empty() {
            ui.label(RichText::new("Nothing matches this search and these filters.").color(INK2));
            ui.add_space(12.0);
            if secondary(ui, "Show all games").clicked() {
                self.query.clear();
                self.filter = Filter::All;
                self.store = None;
            }
            return;
        }
        // Columns fill the row exactly, so the grid shares its edges with the toolbar's row.
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

    // Status (with counts) on the left; store and order on the right. A filter with one possible value is left out.
    fn toolbar(&mut self, ui: &mut Ui) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing = vec2(16.0, 12.0);
            let count = |f: Filter| self.games.iter().filter(|g| f == Filter::All || filter_of(g) == f).count();
            let labels = [Filter::All, Filter::Ready, Filter::Installed, Filter::Unsupported].map(|f| {
                let name = match f {
                    Filter::All => "All",
                    Filter::Ready => "Ready",
                    Filter::Installed => "Installed",
                    Filter::Unsupported => "Unsupported",
                };
                (f, format!("{name}  {}", count(f)))
            });
            let options: Vec<(Filter, &str)> = labels.iter().map(|(f, l)| (*f, l.as_str())).collect();
            if let Some(f) = segmented(ui, &options, self.filter, None) {
                self.filter = f;
            }

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                self.sort_menu(ui);
                let mut stores: Vec<&'static str> = self.games.iter().map(|g| g.store).collect();
                stores.sort();
                stores.dedup();
                if stores.len() > 1 {
                    egui::ComboBox::from_id_salt("store").selected_text(button_text(self.store.unwrap_or("All stores"), INK)).show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.store, None, button_text("All stores", INK));
                        for s in stores {
                            ui.selectable_value(&mut self.store, Some(s), button_text(s, INK));
                        }
                    });
                }
            });
        });
    }

    fn sort_menu(&mut self, ui: &mut Ui) {
        let order = if self.sort == Sort::Recent { "Recently played" } else { "Name A-Z" };
        egui::ComboBox::from_id_salt("sort").selected_text(button_text(order, INK)).show_ui(ui, |ui| {
            ui.selectable_value(&mut self.sort, Sort::Recent, button_text("Recently played", INK));
            ui.selectable_value(&mut self.sort, Sort::Name, button_text("Name A-Z", INK));
        });
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
        let g = self.games[i].clone();
        if tertiary(ui, "← Library").on_hover_text("Esc").clicked() {
            self.back();
            return;
        }
        ui.add_space(16.0);
        // One page column, the same as the Settings page: the header sits on top, sections run its full width.
        ui.set_max_width(ui.available_width().min(PAGE_W));
        ui.horizontal_top(|ui| {
            let (rect, _) = ui.allocate_exact_size(vec2(160.0, 240.0), Sense::hover());
            cover(ui, &g, rect);
            ui.add_space(32.0);
            ui.vertical(|ui| {
                ui.add(egui::Label::new(RichText::new(g.name.to_uppercase()).font(display(TITLE)).color(INK)).wrap());
                // Anton reserves a tall line; pull the metadata up so it groups with the title.
                ui.add_space(-10.0);
                ui.horizontal_wrapped(|ui| {
                    chip(ui, &format!("Unity {}", g.unity), ChipKind::Plain);
                    chip(ui, g.backend(), ChipKind::Plain);
                    chip(ui, g.badge(), chip_kind(&g));
                });
                ui.add_space(8.0);
                ui.add(egui::Label::new(RichText::new(g.dir.display().to_string()).font(mono(LABEL)).color(INK3)).wrap());
                ui.add_space(24.0);
                ui.horizontal_wrapped(|ui| self.actions(ui, &g));
                let (text, tone) = if self.status.0.is_empty() { warning(&g) } else { self.status.clone() };
                if !text.is_empty() {
                    status_line(ui, &text, tone);
                }
            });
        });
        if g.compat() != Compat::Supported {
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
        let text = match changed {
            0 => "Follows your settings for all games. Change one here for this game only; it applies on the next start.".to_string(),
            1 => "1 setting changed for this game only. Changes apply on the next start.".to_string(),
            n => format!("{n} settings changed for this game only. Changes apply on the next start."),
        };
        ui.add(egui::Label::new(RichText::new(text).color(INK2)).wrap());
        ui.add_space(24.0);
        self.settings(ui, settings::QUICK, true);
        self.advanced_section(ui, true);
        self.buttons_section(ui, true);
        let file = g.file(installer::CONFIG);
        if self.advanced && file.exists() {
            ui.add_space(8.0);
            if tertiary(ui, "Open settings file").clicked() {
                let _ = Command::new("notepad").arg(file).spawn();
            }
        }
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
        ui.label(RichText::new("For every game. A game's page can change any of them for that game only.").color(INK2));
        section_break(ui);
        ui.horizontal(|ui| {
            eyebrow(ui, "01", "Basics");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let parallax = settings::all().all(|d| settings::same(&settings::global(&self.defaults, d), d.default));
                if !parallax && tertiary(ui, "Reset all").on_hover_text("Back to Parallax's own defaults").clicked() {
                    self.defaults = settings::all().fold(self.defaults.clone(), |t, d| settings::set(&t, d.section, d.key, d.default));
                    self.dirty = true;
                }
            });
        });
        ui.label(RichText::new("Changes save at once and apply on a game's next start.").color(INK2));
        ui.add_space(24.0);
        self.settings(ui, settings::QUICK, false);
        self.advanced_section(ui, false);
        self.buttons_section(ui, false);
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
        if !fold(ui, "03", "Buttons", changed, &mut self.buttons) {
            return;
        }
        ui.add_space(8.0);
        let note = "What each controller input presses in Keyboard & mouse mode. Double press, hold and grip + button add more keys to a button; left at None they do nothing.";
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
                    egui::ComboBox::from_id_salt((d.key, game)).width(132.0).selected_text(button_text(&current, tone)).show_ui(ui, |ui| {
                        for k in settings::KEYS {
                            ui.selectable_value(&mut pick, k.to_string(), button_text(k, INK));
                        }
                    });
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

    fn actions(&mut self, ui: &mut Ui, g: &Game) {
        ui.spacing_mut().item_spacing.x = 8.0;
        let playable = g.compat() == Compat::Supported;
        if playable && primary(ui, "Play in VR ↗").clicked() {
            self.report(play(g, &self.defaults));
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
        // Everything below is occasional, so it reads as links, not buttons competing with Play.
        ui.spacing_mut().item_spacing.x = 24.0;
        if playable || g.installed() {
            ui.add_space(16.0);
        }
        if tertiary(ui, "Open folder").clicked() {
            let _ = Command::new("explorer").arg(&g.dir).spawn();
        }
        if g.installed() && tertiary(ui, "Export logs").clicked() {
            self.report(games::export_logs(g).map(|zip| {
                let _ = Command::new("explorer").raw_arg(format!("/select,\"{}\"", zip.display())).spawn();
                "Logs saved to your desktop. Attach the zip to your bug report.".into()
            }));
        }
        if g.file(LOG).exists() && tertiary(ui, "Open log").clicked() {
            let _ = Command::new("notepad").arg(g.file(LOG)).spawn();
        }
        // Only hand-added folders can be forgotten (store games come back on every scan), and not while installed,
        // or the game would vanish from the app with Parallax still in it.
        if let Some(root) = g.added.as_ref().filter(|_| !g.installed()) {
            if tertiary(ui, "Remove from library").clicked() {
                match games::remove_folder(root) {
                    Ok(()) => {
                        self.rescan();
                        self.status = (format!("Removed {} from the library. Its folder was not touched.", g.name), Tone::Good);
                    }
                    Err(e) => self.status = (e.to_string(), Tone::Bad),
                }
            }
        }
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
        egui::Grid::new((defs[0].key, game)).num_columns(2).spacing(vec2(32.0, 24.0)).min_col_width(name_w).show(ui, |ui| {
            for d in defs.iter().filter(|d| game || !d.per_game()) {
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
            let slider = egui::Slider::new(&mut v, lo..=hi)
                .step_by(step as f64)
                .custom_formatter(|x, _| settings::number(&d.kind, x))
                .custom_parser(move |t| t.trim().trim_end_matches(['×', '%', 'm', ' ']).trim().parse::<f64>().ok().map(|x| x * per as f64));
            // A slider may nudge a value onto its step grid just by being drawn; only a real change counts.
            let changed = ui.add(slider).changed();
            let new = format!("{}", (v * 100.0).round() / 100.0);
            (changed && !settings::same(&new, current)).then_some(new)
        }
        Kind::Toggle => {
            let on = current.eq_ignore_ascii_case("true");
            segmented(ui, &[(false, "Off"), (true, "On")], on, Some(false)).map(|b| b.to_string())
        }
        Kind::Choice(options) => {
            // An "Off" option is the neutral one: selected, it is outlined rather than lime.
            let neutral = options.iter().find(|(v, _)| *v == "Off").map(|o| o.0);
            segmented(ui, options, options.iter().find(|(v, _)| settings::same(v, current)).map_or(options[0].0, |o| o.0), neutral).map(String::from)
        }
        Kind::Key => None,  // drawn by the Buttons table
        Kind::Text => {
            let mut v = current.to_string();
            let edit = egui::TextEdit::singleline(&mut v).hint_text("Automatic").desired_width(260.0).margin(Margin::symmetric(12, 10));
            ui.add(edit).changed().then_some(v)
        }
    }
}

fn warning(g: &Game) -> (String, Tone) {
    match (g.compat(), g.other_loader) {
        (Compat::AntiCheat, _) => (format!("{} found. Injecting into anti-cheat games can get accounts banned, so this game is refused.", g.blocker.unwrap_or("Anti-cheat")), Tone::Bad),
        (Compat::X86, _) => ("32-bit game: no VR runtime supports 32-bit apps.".into(), Tone::Info),
        (Compat::Stripped, _) => ("Built with Unity's code trimming, which removes engine code the mod loader needs. Not supported yet.".into(), Tone::Info),
        (_, Some(l)) if !g.installed() => (format!("{l} is already installed. Parallax adds itself alongside; other mods may interfere."), Tone::Info),
        _ => (String::new(), Tone::Info),
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        let bar = egui::Frame::new().fill(BG).inner_margin(Margin::symmetric(32, 16)).stroke(Stroke::new(1.0, LINE));
        egui::Panel::top("top").frame(bar).show(ui, |ui| {
            ui.horizontal(|ui| {
                // Brand, then the places (tabs, left); the one action of the page sits alone on the right.
                ui.label(RichText::new("PARALLAX").font(display(BRAND)).color(INK));
                ui.add_space(32.0);
                ui.spacing_mut().item_spacing.x = 24.0;
                if nav(ui, "Library", self.page != Page::Settings).clicked() && self.page != Page::Library {
                    self.back_to = None;
                    self.back();
                }
                if nav(ui, "Settings", self.page == Page::Settings).on_hover_text("Settings for all games").clicked() && self.page != Page::Settings {
                    self.back_to = if let Page::Game(i) = self.page { Some(i) } else { None };
                    self.page = Page::Settings;
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = 24.0;
                    if self.page == Page::Library && secondary(ui, "Add game +").clicked() {
                        self.add_game();
                    }
                    if let Some(r) = self.update.lock().unwrap().clone() {
                        let text = RichText::new(format!("UPDATE {} ↗", r.tag_name.trim_start_matches('v'))).font(mono(LABEL)).color(LIME);
                        ui.hyperlink_to(text, r.html_url);
                    }
                });
            });
        });
        egui::CentralPanel::default().frame(egui::Frame::new().fill(BG)).show(ui, |ui| {
            egui::ScrollArea::vertical().auto_shrink(false).show(ui, |ui| {
                egui::Frame::new().inner_margin(Margin::same(32)).show(ui, |ui| match self.page {
                    Page::Library => self.library(ui),
                    Page::Game(i) => self.game_page(ui, i),
                    Page::Settings => self.settings_page(ui),
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
            self.rescan();
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

fn label_format(color: Color32) -> egui::TextFormat {
    egui::TextFormat { font_id: mono(LABEL), color, extra_letter_spacing: TRACK, ..Default::default() }
}

// "NN // WORD": number ink3, word lime. The same run is the Advanced toggle, so both read as one label style.
fn eyebrow_text(number: &str, word: &str) -> egui::text::LayoutJob {
    let mut job = egui::text::LayoutJob::default();
    job.append(&format!("{number} // "), 0.0, label_format(INK3));
    job.append(&word.to_uppercase(), 0.0, label_format(LIME));
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

// One line of feedback under the thing it is about: ink2 info, lime success, danger error.
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

fn primary(ui: &mut Ui, text: &str) -> egui::Response {
    let enabled = ui.is_enabled();
    let (fill, ink) = if enabled { (LIME, ON_LIME) } else { (RAISED, INK3) };
    let b = egui::Button::new(button_text(text, ink)).fill(fill).stroke(Stroke::NONE).corner_radius(RADIUS).min_size(vec2(0.0, CONTROL_H));
    let r = ui.add(b);
    if r.hovered() && enabled {
        ui.painter().rect_stroke(r.rect, RADIUS, Stroke::new(1.0, INK), egui::StrokeKind::Inside);
    }
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn secondary(ui: &mut Ui, text: &str) -> egui::Response {
    let r = ui.add(egui::Button::new(button_text(text, INK)).corner_radius(RADIUS).min_size(vec2(0.0, CONTROL_H)));
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Top-bar tab, drawn like the VR menu's tabs: mono word, ink3, ink and a lime bar on the bar's bottom edge when active.
fn nav(ui: &mut Ui, text: &str, active: bool) -> egui::Response {
    let old = std::mem::replace(&mut ui.spacing_mut().button_padding, vec2(0.0, 0.0));
    let word = RichText::new(text.to_uppercase()).font(mono(LABEL)).color(if active { INK } else { INK3 }).extra_letter_spacing(TRACK);
    let r = ui.add(egui::Button::new(word).frame(false).min_size(vec2(0.0, CONTROL_H)));
    ui.spacing_mut().button_padding = old;
    // The bar's bottom margin is 16: the marker sits on its hairline, like the VR tab underline.
    let bar = |h: f32| Rect::from_min_max(pos2(r.rect.left(), r.rect.bottom() + 16.0 - h), pos2(r.rect.right(), r.rect.bottom() + 16.0));
    if active {
        ui.painter().rect_filled(bar(3.0), 0.0, LIME);
    } else if r.hovered() {
        ui.painter().rect_filled(bar(1.0), 0.0, INK2);
    }
    focus_ring(ui, &r);
    r.on_hover_cursor(egui::CursorIcon::PointingHand)
}

// Quiet text action for occasional things: no box, underlined on hover.
fn tertiary(ui: &mut Ui, text: &str) -> egui::Response {
    let old = std::mem::replace(&mut ui.spacing_mut().button_padding, vec2(0.0, 0.0));
    let r = ui.add(egui::Button::new(button_text(text, INK2)).frame(false).min_size(vec2(0.0, CONTROL_H)));
    ui.spacing_mut().button_padding = old;
    if r.hovered() {
        ui.painter().hline(r.rect.x_range(), r.rect.center().y + 9.0, Stroke::new(1.0, INK2));
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
    let mut b = egui::Button::new(button_text(text, if armed { ON_LIME } else { INK })).corner_radius(RADIUS).min_size(vec2(0.0, CONTROL_H));
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
            let mut b = egui::Button::new(button_text(label, if lit { ON_LIME } else if on { INK } else { INK2 })).corner_radius(0).min_size(vec2(0.0, CONTROL_H));
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
    let galley = p.layout_no_wrap(text.to_uppercase(), mono(LABEL), ink);
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
    let width = ui.painter().layout_no_wrap(text.to_uppercase(), mono(LABEL), INK).size().x + if matches!(kind, ChipKind::Plain | ChipKind::Installed) { 20.0 } else { 32.0 };
    let (rect, _) = ui.allocate_exact_size(vec2(width, 24.0), Sense::hover());
    paint_chip(ui.painter(), rect.min, text, kind);
}

fn card(ui: &mut Ui, g: &Game, w: f32) -> egui::Response {
    let h = w * 1.5;
    let (rect, resp) = ui.allocate_exact_size(vec2(w, h + 68.0), Sense::click());
    resp.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Button, true, &g.name));
    let image = Rect::from_min_size(rect.min, vec2(w, h));
    cover(ui, g, image);
    if resp.hovered() {
        glow(&ui.painter_at(image), image.right_bottom(), 0.6);
        ui.painter().rect_stroke(image, RADIUS, Stroke::new(1.0, LIME), egui::StrokeKind::Outside);
    }
    let mut job = egui::text::LayoutJob::simple_singleline(g.name.clone(), body(BODY), INK);
    job.wrap = egui::text::TextWrapping::truncate_at_width(w);
    let name = ui.painter().layout_job(job);
    ui.painter().galley(image.left_bottom() + vec2(0.0, 12.0), name, INK);
    // Ready is the normal case, so it gets no tag; only the exceptions do.
    let chip_text = (g.compat() != Compat::Supported || g.installed()).then(|| g.badge());
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
        // A wide header (no 2:3 cover cached) is shown whole, centred on the surface, never stretched.
        Some(path) if path.ends_with("header.jpg") => {
            ui.painter_at(rect).rect_filled(rect, RADIUS, SURFACE);
            let h = rect.width() * 215.0 / 460.0;
            let inner = Rect::from_center_size(rect.center(), vec2(rect.width(), h));
            egui::Image::new(format!("file:///{}", path.display())).paint_at(ui, inner);
        }
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

fn play(g: &Game, defaults: &str) -> Result<String, Box<dyn Error>> {
    if !g.installed() {
        installer::install(&g.dir)?;
        // The game's own settings (chosen before the first install, or kept from an earlier one) go in with the
        // plugin; everything else comes from the all-games values.
        let saved = fs::read_to_string(saved_path(g)).unwrap_or_default();
        write(&g.file(installer::CONFIG), settings::fill(&saved, defaults).as_bytes())?;
    }
    g.launch()?;
    games::mark_played(&g.dir);
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

fn defaults_path() -> PathBuf {
    dirs::config_dir().unwrap_or_default().join("Parallax").join("defaults.cfg")
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
        s.spacing.scroll = egui::style::ScrollStyle::solid();
        s.spacing.interact_size.y = CONTROL_H;
        for (style, size) in [(egui::TextStyle::Body, BODY), (egui::TextStyle::Button, SMALL), (egui::TextStyle::Small, LABEL)] {
            s.text_styles.insert(style, body(size));
        }
    });
}
