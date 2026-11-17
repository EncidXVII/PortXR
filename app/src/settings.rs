//! The plugin's BepInEx config file (`[Section]` headers, `Key = Value` lines). Keys are written in place so the
//! descriptions BepInEx adds survive; missing keys are appended and BepInEx fills in the rest on the next launch.

pub enum Kind {
    /// Range and step in stored units; shown as `value / per` with `unit` (so 1200 px/s can read "100%").
    Slider { lo: f32, hi: f32, step: f32, per: f32, unit: &'static str },
    Toggle,
    Choice(&'static [(&'static str, &'static str)]),
    Text,
    /// A key or mouse button from KEYS (keyboard & mouse bindings).
    Key,
}

pub struct Def {
    pub section: &'static str,
    pub key: &'static str,
    pub label: &'static str,
    pub default: &'static str,
    pub kind: Kind,
    /// One short line under the label, only where the name alone does not say what it does.
    pub hint: &'static str,
}

const fn def(section: &'static str, key: &'static str, label: &'static str, default: &'static str, kind: Kind, hint: &'static str) -> Def {
    Def { section, key, label, default, kind, hint }
}

const fn slider(lo: f32, hi: f32, step: f32, unit: &'static str) -> Kind {
    Kind::Slider { lo, hi, step, per: 1.0, unit }
}

// Names, ranges, steps and order match the VR menu, so a setting reads the same in both places.
/// Display for Unreal games, which have no VR screen.
pub static DISPLAY_NO_THEATER: Def = def("View", "Display", "Display", "Stereo", Kind::Choice(&[("Stereo", "Full VR"), ("Parallax", "3D screen")]), "Full VR: inside the game. 3D screen: the game in 3D on a big screen.");

pub const QUICK: &[Def] = &[
    def("View", "Display", "Display", "Stereo", Kind::Choice(&[("Stereo", "Full VR"), ("Parallax", "3D screen"), ("Theater", "VR screen")]), "Full VR: inside the game. 3D screen: the game in 3D on a big screen. VR screen: flat on a big screen."),
    def("View", "Background", "Around the screen", "Dark", Kind::Choice(&[("Dark", "Dark"), ("Passthrough", "Your room")]), "Your room needs passthrough (Meta, Virtual Desktop; not SteamVR)."),
    def("View", "Depth", "3D depth", "1", slider(0.0, 1.0, 0.25, "×"), "3D screen only. 1× is the comfortable maximum."),
    def("View", "WorldScale", "World scale", "1", slider(0.5, 3.0, 0.1, "×"), "Above 1 you feel bigger and the world smaller."),
    def("Comfort", "Turning", "Turning", "Smooth", Kind::Choice(&[("Smooth", "Smooth"), ("Snap15", "Snap 15°"), ("Snap30", "Snap 30°"), ("Snap45", "Snap 45°")]), ""),
    def("Comfort", "Vignette", "Comfort vignette", "Off", Kind::Choice(&[("Off", "Off"), ("Low", "Low"), ("High", "High")]), "Darkens the edges of your view while you move or turn."),
    def("Controls", "Mode", "Controls", "Auto", Kind::Choice(&[("Auto", "Auto"), ("Gamepad", "Gamepad"), ("KeyboardMouse", "Keyboard & mouse"), ("Off", "Off")]), "Auto picks gamepad in games made for one. Off: controllers only work the VR menu."),
    def("Controls", "HeadAim", "Aim", "Auto", Kind::Choice(&[("Auto", "Auto"), ("On", "On"), ("Off", "Off")]), "Auto: in first-person games."),
    def("Controls", "AimWith", "Aim with", "Hand", Kind::Choice(&[("Head", "Head"), ("Hand", "Right hand")]), ""),
];

pub const ADVANCED: &[Def] = &[
    def("View", "Height", "Height", "0", slider(-1.0, 1.0, 0.05, " m"), ""),
    def("View", "RenderScale", "Resolution", "1", slider(0.5, 2.0, 0.1, "×"), "Of the headset's recommended size."),
    def("View", "AutoRenderScale", "Auto resolution", "true", Kind::Toggle, "Lowers resolution while the headset misses frames."),
    def("View", "UiDistance", "UI distance", "1.2", slider(0.5, 3.0, 0.1, " m"), ""),
    def("View", "UiWidth", "UI width", "1.6", slider(0.5, 4.0, 0.1, " m"), ""),
    def("View", "UiBlack", "UI black", "Auto", Kind::Choice(&[("Auto", "Auto"), ("SeeThrough", "See-through"), ("Keep", "Keep")]), "Auto: see-through when a game fills the whole panel."),
    def("Controls", "MouseSpeed", "Look speed", "1200", Kind::Slider { lo: 240.0, hi: 3840.0, step: 60.0, per: 12.0, unit: "%" }, "Keyboard & mouse: how fast the right stick turns."),
    def("Controls", "Pointer", "Laser pointer", "Auto", Kind::Choice(&[("Auto", "With game cursor"), ("MenuOnly", "Menu only")]), ""),
    def("Controls", "WristMenu", "Wrist glance menu", "true", Kind::Toggle, "Look at the back of your left hand and pull the right trigger for the VR menu."),
    def("Controls", "WeaponInHand", "Weapon in hand", "true", Kind::Toggle, "With hand aim, the game's gun and arms move into your right hand."),
    def("Controls", "Crosshair", "Crosshair", "Game", Kind::Choice(&[("Game", "Game's"), ("Lime", "Parallax's"), ("Off", "Off")]), "Game's: the game's own crosshair moves to where you aim. Parallax's: ours instead."),
    def("Game", "CameraName", "Game camera", "", Kind::Text, "Easier in the VR menu's Game tab, with live previews."),
    def("Game", "Console", "Console commands", "", Kind::Text, "Run as each level starts, separated by ;  For example: r.MotionBlurQuality 0; r.DepthOfFieldQuality 0"),
];

/// What a controller input can press, as the plugin names them.
pub const KEYS: &[&str] = &[
    "None", "Space", "Enter", "Esc", "Tab", "Backspace", "LeftShift", "LeftCtrl", "LeftAlt", "Q", "E", "R", "F", "G", "C", "V", "Z", "X",
    "T", "I", "M", "1", "2", "3", "4", "Up", "Down", "Left", "Right", "MouseLeft", "MouseRight", "MouseMiddle",
];

/// A key as players read it; the plugin's names (LeftShift, MouseRight) stay in the settings files.
pub fn key_label(key: &str) -> &str {
    match key {
        "LeftShift" => "Left Shift",
        "LeftCtrl" => "Left Ctrl",
        "LeftAlt" => "Left Alt",
        "MouseLeft" => "Left click",
        "MouseRight" => "Right click",
        "MouseMiddle" => "Middle click",
        "Up" => "Up arrow",
        "Down" => "Down arrow",
        "Left" => "Left arrow",
        "Right" => "Right arrow",
        other => other,
    }
}

/// Controller inputs in the plugin's [Bindings] keys, left hand first: (key, shown name, what a press does by default).
pub const INPUTS: &[(&str, &str, &str)] = &[
    ("X", "Left X", "E"),
    ("Y", "Left Y", "R"),
    ("LeftGrip", "Left grip", "LeftShift"),
    ("LeftTrigger", "Left trigger", "MouseRight"),
    ("LeftStickClick", "Left stick click", "C"),
    ("MenuTap", "Menu tap", "Esc"),
    ("MenuHold", "Menu hold", "Tab"),
    ("A", "Right A", "Space"),
    ("B", "Right B", "LeftCtrl"),
    ("RightGrip", "Right grip", "Q"),
    ("RightTrigger", "Right trigger", "MouseLeft"),
    ("RightStickClick", "Right stick click", "None"),
];

/// Gestures as key suffixes and column names. The menu button has its own tap and hold; grips have no grip combo.
pub const GESTURES: &[(&str, &str)] = &[("", "Press"), ("Double", "Double press"), ("Hold", "Hold"), ("Combo", "Grip +")];

fn applies(input: &str, gesture: usize) -> bool {
    gesture == 0 || !input.starts_with("Menu") && !(gesture == 3 && input.ends_with("Grip"))
}

/// One setting per input and gesture that applies to it, input-major.
pub static BINDINGS: std::sync::LazyLock<Vec<Def>> = std::sync::LazyLock::new(|| {
    let mut list = Vec::new();
    for &(input, name, press) in INPUTS {
        for (g, &(suffix, gesture)) in GESTURES.iter().enumerate() {
            if applies(input, g) {
                let key: &'static str = Box::leak(format!("{input}{suffix}").into_boxed_str());
                list.push(def("Bindings", key, name, if g == 0 { press } else { "None" }, Kind::Key, gesture));
            }
        }
    }
    list
});

/// The binding for an input (index into INPUTS) and gesture, if that gesture applies.
pub fn binding(input: usize, gesture: usize) -> Option<&'static Def> {
    let key = format!("{}{}", INPUTS[input].0, GESTURES[gesture].0);
    BINDINGS.iter().find(|d| d.key == key)
}

fn key_of(line: &str) -> Option<&str> {
    let (k, _) = line.split_once('=')?;
    let k = k.trim();
    (!k.starts_with('#')).then_some(k)
}

pub fn get(text: &str, section: &str, key: &str) -> Option<String> {
    let mut current = "";
    for line in text.lines().map(str::trim) {
        if let Some(s) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            current = s;
        } else if current == section && key_of(line) == Some(key) {
            return line.split_once('=').map(|(_, v)| v.trim().to_string());
        }
    }
    None
}

pub fn set(text: &str, section: &str, key: &str, value: &str) -> String {
    let mut lines: Vec<String> = text.lines().map(String::from).collect();
    let entry = format!("{key} = {value}");
    let mut current = String::new();
    let mut header = None;
    for i in 0..lines.len() {
        let line = lines[i].trim().to_string();
        if let Some(s) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            current = s.to_string();
            if current == section {
                header = Some(i);
            }
        } else if current == section && key_of(&line) == Some(key) {
            lines[i] = entry;
            return lines.join("\r\n") + "\r\n";
        }
    }
    match header {
        Some(i) => lines.insert(i + 1, entry),
        None => lines.extend([String::new(), format!("[{section}]"), entry]),
    }
    lines.join("\r\n") + "\r\n"
}

impl Def {
    /// The only text setting, a camera name, means nothing outside its own game, so it has no all-games value.
    pub fn per_game(&self) -> bool {
        matches!(self.kind, Kind::Text)
    }
}

pub fn all() -> impl Iterator<Item = &'static Def> {
    QUICK.iter().chain(ADVANCED).chain(BINDINGS.iter())
}

/// The all-games value: the defaults file's, else Parallax's own.
pub fn global(defaults: &str, d: &Def) -> String {
    if d.per_game() { None } else { get(defaults, d.section, d.key) }.unwrap_or_else(|| d.default.into())
}

/// Values compare as numbers when both are ("1" and "1.00"), else ignoring case ("True" and "true").
pub fn same(a: &str, b: &str) -> bool {
    match (a.parse::<f32>(), b.parse::<f32>()) {
        (Ok(x), Ok(y)) => (x - y).abs() < 1e-4,
        _ => a.eq_ignore_ascii_case(b),
    }
}

/// A game's config with every setting it has no value for taken from the all-games values.
pub fn fill(text: &str, defaults: &str) -> String {
    all().fold(text.to_string(), |t, d| if get(&t, d.section, d.key).is_some() { t } else { set(&t, d.section, d.key, &global(defaults, d)) })
}

/// A game's config after the all-games values went from `old` to `new`: values that matched the old one (or were
/// missing) follow; values that differ were set for this game and stay.
pub fn follow(text: &str, old: &str, new: &str) -> String {
    all().filter(|d| !d.per_game()).fold(text.to_string(), |t, d| {
        let (was, now) = (global(old, d), global(new, d));
        match get(&t, d.section, d.key) {
            _ if same(&was, &now) => t,
            Some(v) if !same(&v, &was) => t,
            _ => set(&t, d.section, d.key, &now),
        }
    })
}

/// A slider value with as many decimals as its step has, and its unit: "1.2×", "0.05 m", "100%".
pub fn number(kind: &Kind, v: f64) -> String {
    let Kind::Slider { lo, step, per, unit, .. } = *kind else { return v.to_string() };
    let shown_step = step / per;
    let decimals = if shown_step >= 1.0 { 0 } else if (shown_step * 10.0).fract().abs() < 1e-4 { 1 } else { 2 };
    let x = v / per as f64;
    // Ranges around zero show their sign, like the VR menu ("+0.25 m").
    if lo < 0.0 && x.abs() > 1e-6 { format!("{:+.*}{unit}", decimals, x) } else { format!("{:.*}{unit}", decimals, if x.abs() < 1e-6 { 0.0 } else { x }) }
}

/// The value as the app shows it: a choice's label, On/Off, or the number.
pub fn shown(d: &Def, value: &str) -> String {
    match d.kind {
        Kind::Choice(options) => options.iter().find(|(v, _)| same(v, value)).map_or(value, |o| o.1).to_string(),
        Kind::Toggle => if value.eq_ignore_ascii_case("true") { "On" } else { "Off" }.into(),
        Kind::Slider { .. } => value.parse::<f32>().map_or(value.to_string(), |v| number(&d.kind, v as f64)),
        Kind::Text | Kind::Key => value.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn edits_in_place_and_appends() {
        let cfg = "[View]\r\n## Player size\r\n# Default value: 1\r\nWorldScale = 1\r\n\r\n[Controls]\r\nMode = KeyboardMouse\r\n";
        let cfg = set(cfg, "View", "WorldScale", "1.5");
        assert!(cfg.contains("## Player size\r\n# Default value: 1\r\nWorldScale = 1.5\r\n"));
        let cfg = set(&cfg, "Controls", "Pointer", "MenuOnly");
        let cfg = set(&cfg, "Game", "CameraName", "Main");
        assert_eq!(get(&cfg, "View", "WorldScale").as_deref(), Some("1.5"));
        assert_eq!(get(&cfg, "Controls", "Pointer").as_deref(), Some("MenuOnly"));
        assert_eq!(get(&cfg, "Game", "CameraName").as_deref(), Some("Main"));
        assert_eq!(get(&cfg, "View", "Default value: 1"), None);
        assert_eq!(get("", "View", "Theater"), None);
    }

    #[test]
    fn games_inherit_all_games_values() {
        let defaults = "[Comfort]\r\nTurning = Snap30\r\n";
        let game = fill("[View]\r\nWorldScale = 1.2\r\n", defaults);
        assert_eq!(get(&game, "Comfort", "Turning").as_deref(), Some("Snap30"));
        assert_eq!(get(&game, "View", "WorldScale").as_deref(), Some("1.2"));
        assert_eq!(get(&game, "Game", "CameraName").as_deref(), Some(""));
        assert!(same("1", "1.00") && same("True", "true") && !same("Off", "Low"));
        assert_eq!(get(&game, "Bindings", "A").as_deref(), Some("Space"));
        assert_eq!(get(&game, "Bindings", "AHold").as_deref(), Some("None"));
        assert!(binding(5, 1).is_none() && binding(2, 3).is_none() && binding(7, 3).is_some());
    }

    #[test]
    fn games_follow_unless_changed_for_that_game() {
        let old = "[Comfort]\r\nTurning = Smooth\r\n";
        let new = "[Comfort]\r\nTurning = Snap30\r\nVignette = High\r\n";
        let inherits = follow("[Comfort]\r\nTurning = Smooth\r\nVignette = Off\r\n", old, new);
        assert_eq!(get(&inherits, "Comfort", "Turning").as_deref(), Some("Snap30"));
        assert_eq!(get(&inherits, "Comfort", "Vignette").as_deref(), Some("High"));
        let own = follow("[Comfort]\r\nTurning = Snap45\r\nVignette = Low\r\n[View]\r\nWorldScale = 1.3\r\n", old, new);
        assert_eq!(get(&own, "Comfort", "Turning").as_deref(), Some("Snap45"));
        assert_eq!(get(&own, "Comfort", "Vignette").as_deref(), Some("Low"));
        assert_eq!(get(&own, "View", "WorldScale").as_deref(), Some("1.3"));
    }
}
