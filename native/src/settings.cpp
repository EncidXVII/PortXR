#include "settings.h"
#include "common.h"
#include <io.h>
#include <share.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace {
std::mutex g_mutex;
std::wstring g_game, g_defaults;
std::map<std::string, std::string> g_values;  // "Section/Key"
FILETIME g_gameStamp{}, g_defaultsStamp{};
ULONGLONG g_checkedAt;

std::string read(const std::wstring& path) {
    std::string s;
    if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        char buf[8192];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
        fclose(f);
    }
    return s;
}

FILETIME stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) ? a.ftLastWriteTime : FILETIME{};
}

bool same(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void parse(const std::string& text, std::map<std::string, std::string>& out) {
    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) out[section + "/" + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

void load() {
    std::map<std::string, std::string> values;
    parse(read(g_defaults), values);
    std::map<std::string, std::string> game;
    parse(read(g_game), game);
    for (auto& [k, v] : game) values[k] = v;
    g_values.swap(values);
    g_gameStamp = stamp(g_game);
    g_defaultsStamp = stamp(g_defaults);
}

}  // namespace

namespace settings {
void open(const std::wstring& gameFile, const std::wstring& defaultsFile) {
    std::lock_guard lock(g_mutex);
    g_game = gameFile;
    g_defaults = defaultsFile.empty() ? prx_appdata(L"defaults.cfg") : defaultsFile;
    load();
}

// Re-read on the next refresh(): the Unity plugin just saved the file.
extern "C" __declspec(dllexport) void prx_settings_touch() { g_checkedAt = 0; }

bool refresh() {
    std::lock_guard lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    if (now - g_checkedAt < 500) return false;
    g_checkedAt = now;
    if (same(stamp(g_game), g_gameStamp) && same(stamp(g_defaults), g_defaultsStamp)) return false;
    load();
    return true;
}

std::string get(const char* section, const char* key, const char* fallback) {
    std::lock_guard lock(g_mutex);
    auto it = g_values.find(std::string(section) + "/" + key);
    return it == g_values.end() || it->second.empty() ? fallback : it->second;
}

std::string section_of(const char* key) {
    std::lock_guard lock(g_mutex);
    std::string tail = std::string("/") + key;
    for (auto& [k, v] : g_values)
        if (k.size() > tail.size() && k.compare(k.size() - tail.size(), tail.size(), tail) == 0) return k.substr(0, k.size() - tail.size());
    return {};
}

float getf(const char* section, const char* key, float fallback, float lo, float hi) {
    std::string v = get(section, key, "");
    char* end = nullptr;
    float f = v.empty() ? fallback : std::strtof(v.c_str(), &end);
    if (!v.empty() && end == v.c_str()) f = fallback;
    return std::clamp(f, lo, hi);
}

bool getb(const char* section, const char* key, bool fallback) {
    std::string v = get(section, key, fallback ? "true" : "false");
    return v == "true" || v == "True" || v == "1";
}

// Rewrites the value in place (or adds it to its section, or the section at the end), as the app's settings::set.
void set(const char* section, const char* key, const std::string& value) {
    std::lock_guard lock(g_mutex);
    std::string text = read(g_game);
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = end + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string header = std::string("[") + section + "]", entry = std::string(key) + " = " + value;
    bool in = false, done = false;
    size_t sectionEnd = std::string::npos;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        std::string t = trim(lines[i]);
        if (!t.empty() && t.front() == '[') {
            if (in) sectionEnd = i;
            in = t == header;
            if (!in && sectionEnd != std::string::npos) break;
            continue;
        }
        size_t eq = t.find('=');
        if (in && t[0] != '#' && eq != std::string::npos && trim(t.substr(0, eq)) == key) lines[i] = entry, done = true;
    }
    if (!done) {
        if (in) lines.push_back(entry);
        else if (sectionEnd != std::string::npos) {
            size_t at = sectionEnd;
            while (at > 0 && trim(lines[at - 1]).empty()) --at;
            lines.insert(lines.begin() + at, entry);
        } else {
            lines.push_back("");
            lines.push_back(header);
            lines.push_back(entry);
        }
    }
    std::string out;
    for (auto& l : lines) out += l + "\r\n";
    // Written beside and renamed over: a power cut leaves the old file or the new one, never a zeroed one.
    std::wstring tmp = g_game + L".tmp";
    if (FILE* f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYWR)) {
        fwrite(out.data(), 1, out.size(), f);
        fflush(f);
        _commit(_fileno(f));
        fclose(f);
        MoveFileExW(tmp.c_str(), g_game.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    g_values[std::string(section) + "/" + key] = value;
    g_gameStamp = stamp(g_game);
}

void accent(float rgb[3]) {
    static const char* names[] = {"Lime", "Mint", "Aqua", "Sky", "Violet", "Pink", "Amber", "White"};
    static const int rgbs[] = {0xC6FF1A, 0x5CF2A0, 0x3DF2E0, 0x5CC8FF, 0xB49AFF, 0xFF7ADB, 0xFFB627, 0xF5F5F7};
    std::string t = read(prx_appdata(L"theme.txt"));
    int c = rgbs[0];
    for (int i = 0; i < 8; ++i)
        if (t.rfind(names[i], 0) == 0) c = rgbs[i];
    rgb[0] = ((c >> 16) & 255) / 255.f, rgb[1] = ((c >> 8) & 255) / 255.f, rgb[2] = (c & 255) / 255.f;
}
}  // namespace settings
