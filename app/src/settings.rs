//! The plugin's BepInEx config file (`[Section]` headers, `Key = Value` lines). Keys are written in place so the
//! descriptions BepInEx adds survive; missing keys are appended and BepInEx fills in the rest on the next launch.

pub enum Kind {
    Slider(f32, f32),
    Toggle,
    Choice(&'static [(&'static str, &'static str)]),
    Text,
}

pub struct Def {
    pub section: &'static str,
    pub key: &'static str,
    pub label: &'static str,
    pub default: &'static str,
    pub kind: Kind,
}

const fn def(section: &'static str, key: &'static str, label: &'static str, default: &'static str, kind: Kind) -> Def {
    Def { section, key, label, default, kind }
}

pub const QUICK: &[Def] = &[
    def("Controls", "Mode", "Controls", "KeyboardMouse", Kind::Choice(&[("KeyboardMouse", "Keyboard & mouse"), ("Gamepad", "Gamepad")])),
    def("Comfort", "Turning", "Turning", "Smooth", Kind::Choice(&[("Smooth", "Smooth"), ("Snap15", "Snap 15°"), ("Snap30", "Snap 30°"), ("Snap45", "Snap 45°")])),
    def("Comfort", "Vignette", "Comfort vignette", "Off", Kind::Choice(&[("Off", "Off"), ("Low", "Low"), ("High", "High")])),
    def("View", "Theater", "Theater mode", "false", Kind::Toggle),
    def("View", "WorldScale", "World scale", "1", Kind::Slider(0.5, 2.0)),
    def("View", "Height", "Height offset (m)", "0", Kind::Slider(-1.0, 1.0)),
];

pub const ADVANCED: &[Def] = &[
    def("View", "RenderScale", "Resolution", "1", Kind::Slider(0.5, 2.0)),
    def("View", "AutoRenderScale", "Lower resolution when frames drop", "true", Kind::Toggle),
    def("View", "UiDistance", "UI panel distance (m)", "1.2", Kind::Slider(0.5, 3.0)),
    def("View", "UiWidth", "UI panel width (m)", "1.6", Kind::Slider(0.5, 4.0)),
    def("Controls", "MouseSpeed", "Mouse look speed", "1200", Kind::Slider(200.0, 4000.0)),
    def("Controls", "Pointer", "Laser pointer", "Auto", Kind::Choice(&[("Auto", "When the game shows a cursor"), ("MenuOnly", "VR menu only")])),
    def("Game", "CameraName", "Camera object (empty: automatic)", "", Kind::Text),
];

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

/// Every setting the app shows, back at its default; other keys (bindings, menu key) are left alone.
pub fn reset(text: &str) -> String {
    QUICK.iter().chain(ADVANCED).fold(text.to_string(), |t, d| set(&t, d.section, d.key, d.default))
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
}
