#pragma once
// Settings, as the app and the Unity plugin keep them: "[Section]" headers and "Key = Value" lines (BepInEx's format).
// A game's own file wins over the all-games defaults (%APPDATA%\Parallax\defaults.cfg). The game's file is read again
// when it changes on disk, so the app's edits reach a running game; set() writes it back.
#include <string>

namespace settings {
void open(const std::wstring& gameFile, const std::wstring& defaultsFile);
// Re-reads the files if either changed since; cheap to call every frame (checks at most twice a second).
bool refresh();
std::string get(const char* section, const char* key, const char* fallback);
float getf(const char* section, const char* key, float fallback, float lo, float hi);
bool getb(const char* section, const char* key, bool fallback);
void set(const char* section, const char* key, const std::string& value);
// The section that holds `key` (the app writes every key into the game's file); empty if none does.
std::string section_of(const char* key);
// The theme accent (sRGB 0..1) from %APPDATA%\Parallax\theme.txt.
void accent(float rgb[3]);
}  // namespace settings
