use std::error::Error;
use std::fs;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};

use eframe::egui::{self, Align, Align2, Color32, FontFamily, FontId, Layout, Margin, RichText, Sense, Stroke, Ui, vec2};
use serde::Deserialize;

use crate::games::{self, Compat, Game};
use crate::installer;
use crate::settings::{self, Def, Kind};

// Same tokens as the VR menu: warm grays, one accent, two weights, spacing on a 4/8/16/24/32 scale.
const BG: Color32 = Color32::from_rgb(31, 29, 27);
const SURFACE: Color32 = Color32::from_rgb(43, 40, 38);
const HOVER: Color32 = Color32::from_rgb(58, 54, 51);
const INK: Color32 = Color32::from_rgb(237, 232, 227);
const MUTED: Color32 = Color32::from_rgb(163, 156, 148);
const ACCENT: Color32 = Color32::from_rgb(232, 131, 58);
const REPO: &str = "EncidXVII/Parallax";
const LOG: &str = r"BepInEx\LogOutput.log";

pub fn run() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Parallax")
            .with_inner_size([1120.0, 720.0])
            .with_min_inner_size([760.0, 480.0]),
        ..Default::default()
    };
    eframe::run_native("Parallax", options, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}

#[derive(Deserialize, Clone)]
struct Release {
    tag_name: String,
    html_url: String,
}

struct App {
    games: Vec<Game>,
    page: Option<usize>,
    status: String,
    cfg: String,
    dirty: bool,
    update: Arc<Mutex<Option<Release>>>,
}

fn strong() -> FontFamily {
    FontFamily::Name("strong".into())
}

impl App {
    fn new(cc: &eframe::CreationContext) -> Self {
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
        let mut app = App { games: Vec::new(), page: None, status: String::new(), cfg: String::new(), dirty: false, update };
        app.rescan();
        app
    }

    fn rescan(&mut self) {
        self.games = games::scan();
        self.page = None;
        self.status = if self.games.is_empty() { "No Unity games found. Use Add game to pick a folder.".into() } else { String::new() };
    }

    fn add_game(&mut self) {
        let Some(dir) = rfd::FileDialog::new().set_title("Pick the folder that contains the game's .exe").pick_folder() else { return };
        if games::detect(&dir).is_none() {
            self.status = "No Unity game in that folder (looking for an .exe next to a *_Data folder).".into();
            return;
        }
        if let Err(e) = games::add_manual(&dir) {
            self.status = e.to_string();
            return;
        }
        self.rescan();
        if let Some(i) = self.games.iter().position(|g| g.dir == dir) {
            self.open(i);
        }
    }

    fn open(&mut self, i: usize) {
        self.page = Some(i);
        self.status.clear();
        self.cfg = load_cfg(&self.games[i]);
    }

    fn library(&mut self, ui: &mut Ui) {
        if !self.status.is_empty() {
            ui.label(RichText::new(&self.status).color(MUTED));
        }
        const W: f32 = 160.0;
        let cols = ((ui.available_width() + 16.0) / (W + 16.0)).floor().max(1.0) as usize;
        let mut open = None;
        egui::ScrollArea::vertical().auto_shrink(false).show(ui, |ui| {
            for (row, chunk) in self.games.chunks(cols).enumerate() {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 16.0;
                    for (j, g) in chunk.iter().enumerate() {
                        if card(ui, g, W).clicked() {
                            open = Some(row * cols + j);
                        }
                    }
                });
                ui.add_space(16.0);
            }
        });
        if let Some(i) = open {
            self.open(i);
        }
    }

    fn game_page(&mut self, ui: &mut Ui, i: usize) {
        let g = self.games[i].clone();
        egui::ScrollArea::vertical().auto_shrink(false).show(ui, |ui| {
            ui.horizontal_top(|ui| {
                let (rect, _) = ui.allocate_exact_size(vec2(200.0, 300.0), Sense::hover());
                cover(ui, &g, rect);
                ui.add_space(16.0);
                ui.vertical(|ui| {
                    ui.label(RichText::new(&g.name).size(28.0).family(strong()));
                    ui.label(RichText::new(format!("Unity {}  ·  {}  ·  {}", g.unity, g.backend(), g.badge())).color(MUTED));
                    ui.label(RichText::new(g.dir.display().to_string()).color(MUTED).size(13.0));
                    ui.add_space(16.0);
                    ui.horizontal(|ui| self.actions(ui, &g));
                    ui.add_space(8.0);
                    let warning = match (g.blocker, g.other_loader) {
                        (Some(b), _) => format!("{b} found. Injecting into anti-cheat games can get accounts banned, so this game is refused."),
                        (_, Some(l)) if !g.installed() => format!("{l} is already installed. Parallax will add itself alongside; other mods may interfere."),
                        _ => String::new(),
                    };
                    let text = if self.status.is_empty() { warning } else { self.status.clone() };
                    ui.label(RichText::new(text).color(MUTED));
                });
            });
            ui.add_space(32.0);
            ui.label(RichText::new("Settings").size(20.0).family(strong()));
            ui.label(RichText::new("Saved as you change them. The game picks them up on its next start.").color(MUTED));
            ui.add_space(8.0);
            self.settings(ui, &g, settings::QUICK);
            ui.add_space(8.0);
            egui::CollapsingHeader::new("Advanced").show(ui, |ui| {
                self.settings(ui, &g, settings::ADVANCED);
                let file = g.file(installer::CONFIG);
                if file.exists() && ui.button("Open settings file").clicked() {
                    let _ = Command::new("notepad").arg(file).spawn();
                }
            });
        });
    }

    fn actions(&mut self, ui: &mut Ui, g: &Game) {
        let primary = egui::Button::new(RichText::new("Play in VR").family(strong()).color(BG)).fill(ACCENT).min_size(vec2(0.0, 36.0));
        if ui.add_enabled(g.compat() == Compat::Supported, primary).clicked() {
            self.report(play(g));
        }
        let button = |text| egui::Button::new(text).min_size(vec2(0.0, 36.0));
        if g.installed() && ui.add(button("Uninstall")).clicked() {
            self.report(uninstall(g));
            self.cfg = load_cfg(g);
        }
        if ui.add(button("Export logs")).clicked() {
            self.report(games::export_logs(g).map(|zip| {
                let _ = Command::new("explorer").raw_arg(format!("/select,\"{}\"", zip.display())).spawn();
                "Logs saved to your desktop. Attach the zip to your bug report.".into()
            }));
        }
        if g.file(LOG).exists() && ui.add(button("Open log")).clicked() {
            let _ = Command::new("notepad").arg(g.file(LOG)).spawn();
        }
    }

    fn report(&mut self, result: Result<String, Box<dyn Error>>) {
        self.status = result.unwrap_or_else(|e| e.to_string());
    }

    fn settings(&mut self, ui: &mut Ui, g: &Game, defs: &[Def]) {
        egui::Grid::new(defs[0].key).num_columns(2).spacing(vec2(24.0, 12.0)).show(ui, |ui| {
            for d in defs {
                let current = settings::get(&self.cfg, d.section, d.key).unwrap_or_else(|| d.default.into());
                ui.label(d.label);
                let new = match d.kind {
                    Kind::Slider(lo, hi) => {
                        let mut v: f32 = current.parse().unwrap_or_else(|_| d.default.parse().unwrap());
                        let step = if hi > 100.0 { 50.0 } else { 0.05 };
                        let changed = ui.add(egui::Slider::new(&mut v, lo..=hi).step_by(step)).changed();
                        changed.then(|| format!("{}", (v * 100.0).round() / 100.0))
                    }
                    Kind::Toggle => {
                        let mut on = current.eq_ignore_ascii_case("true");
                        ui.checkbox(&mut on, "").changed().then(|| on.to_string())
                    }
                    Kind::Choice(options) => {
                        let mut v = current.clone();
                        let shown = options.iter().find(|(val, _)| *val == v).map_or(v.as_str(), |(_, label)| label).to_string();
                        egui::ComboBox::from_id_salt(d.key).selected_text(shown).width(240.0).show_ui(ui, |ui| {
                            for (val, label) in options {
                                ui.selectable_value(&mut v, val.to_string(), *label);
                            }
                        });
                        (v != current).then_some(v)
                    }
                    Kind::Text => {
                        let mut v = current.clone();
                        ui.text_edit_singleline(&mut v).changed().then_some(v)
                    }
                };
                ui.end_row();
                if let Some(v) = new {
                    self.cfg = settings::set(&self.cfg, d.section, d.key, &v);
                    self.dirty = true;
                }
            }
        });
        // A slider drag changes the value every frame; write the file once it is let go.
        if self.dirty && ui.ctx().dragged_id().is_none() {
            self.dirty = false;
            if let Err(e) = save_cfg(g, &self.cfg) {
                self.status = format!("Could not save settings: {e}");
            }
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        let bar = egui::Frame::new().fill(BG).inner_margin(Margin::symmetric(24, 16));
        egui::Panel::top("top").frame(bar).show(ui, |ui| {
            ui.horizontal(|ui| {
                if self.page.is_some() {
                    if ui.button("← Library").clicked() {
                        self.page = None;
                        self.status.clear();
                    }
                } else {
                    ui.label(RichText::new("Library").size(20.0).family(strong()));
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if self.page.is_none() && ui.button("Add game").clicked() {
                        self.add_game();
                    }
                    if let Some(r) = self.update.lock().unwrap().clone() {
                        ui.hyperlink_to(format!("Parallax {} is available", r.tag_name.trim_start_matches('v')), r.html_url);
                    }
                });
            });
        });
        let body = egui::Frame::new().fill(BG).inner_margin(Margin { left: 24, right: 24, top: 8, bottom: 24 });
        egui::CentralPanel::default().frame(body).show(ui, |ui| match self.page {
            None => self.library(ui),
            Some(i) => self.game_page(ui, i),
        });
    }
}

fn card(ui: &mut Ui, g: &Game, w: f32) -> egui::Response {
    let h = w * 1.5;
    let (rect, resp) = ui.allocate_exact_size(vec2(w, h + 52.0), Sense::click());
    let image = egui::Rect::from_min_size(rect.min, vec2(w, h));
    cover(ui, g, image);
    if resp.hovered() {
        ui.painter().rect_stroke(image, 8.0, Stroke::new(2.0, ACCENT), egui::StrokeKind::Outside);
    }
    let line = |text: &str, font: FontId, color: Color32| {
        let mut job = egui::text::LayoutJob::simple_singleline(text.to_string(), font, color);
        job.wrap = egui::text::TextWrapping::truncate_at_width(w);
        ui.painter().layout_job(job)
    };
    let name = line(&g.name, FontId::new(15.0, strong()), INK);
    let badge_color = if g.compat() == Compat::Supported { ACCENT } else { MUTED };
    let badge = line(g.badge(), FontId::proportional(13.0), badge_color);
    ui.painter().galley(image.left_bottom() + vec2(0.0, 8.0), name, INK);
    ui.painter().galley(image.left_bottom() + vec2(0.0, 30.0), badge, badge_color);
    resp.on_hover_cursor(egui::CursorIcon::PointingHand)
}

fn cover(ui: &mut Ui, g: &Game, rect: egui::Rect) {
    match &g.cover {
        Some(path) => egui::Image::new(format!("file:///{}", path.display())).corner_radius(8).paint_at(ui, rect),
        None => {
            ui.painter().rect_filled(rect, 8.0, SURFACE);
            let initial = g.name.chars().next().unwrap_or('?').to_uppercase().to_string();
            ui.painter().text(rect.center(), Align2::CENTER_CENTER, initial, FontId::new(48.0, strong()), MUTED);
        }
    }
}

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

// Segoe UI regular and semibold: the two weights of the design system, from the fonts every Windows ships.
fn style(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    let dir = PathBuf::from(std::env::var("WINDIR").unwrap_or_else(|_| r"C:\Windows".into())).join("Fonts");
    for (name, file, family) in [("body", "segoeui.ttf", FontFamily::Proportional), ("strong", "seguisb.ttf", strong())] {
        if let Ok(bytes) = fs::read(dir.join(file)) {
            fonts.font_data.insert(name.into(), Arc::new(egui::FontData::from_owned(bytes)));
            fonts.families.entry(family).or_default().insert(0, name.into());
        }
    }
    let fallback = fonts.families[&FontFamily::Proportional].clone();
    fonts.families.entry(strong()).or_default().extend(fallback);
    ctx.set_fonts(fonts);

    ctx.all_styles_mut(|s| {
        s.visuals = egui::Visuals::dark();
        let v = &mut s.visuals;
        v.panel_fill = BG;
        v.window_fill = SURFACE;
        v.extreme_bg_color = SURFACE;
        v.override_text_color = Some(INK);
        v.hyperlink_color = ACCENT;
        v.selection.bg_fill = ACCENT;
        v.selection.stroke = Stroke::new(1.0, BG);
        for (w, fill) in [(&mut v.widgets.inactive, SURFACE), (&mut v.widgets.hovered, HOVER), (&mut v.widgets.active, HOVER), (&mut v.widgets.open, HOVER)] {
            w.bg_fill = fill;
            w.weak_bg_fill = fill;
            w.bg_stroke = Stroke::NONE;
            w.corner_radius = egui::CornerRadius::same(6);
        }
        s.spacing.item_spacing = vec2(8.0, 8.0);
        s.spacing.button_padding = vec2(16.0, 6.0);
        s.spacing.slider_width = 240.0;
        for (style, size) in [(egui::TextStyle::Body, 15.0), (egui::TextStyle::Button, 15.0), (egui::TextStyle::Small, 13.0)] {
            s.text_styles.insert(style, FontId::proportional(size));
        }
    });
}
