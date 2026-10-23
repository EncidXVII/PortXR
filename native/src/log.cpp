#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace {
std::mutex g_mutex;
std::deque<std::string> g_lines;
}

// Render-thread and hook code cannot call into managed logging; lines are drained by the plugin each frame.
void prx_log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard lock(g_mutex);
    if (g_lines.size() < 256) g_lines.emplace_back(buf);
}

bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (xrResultToString && g_xr.instance) xrResultToString(g_xr.instance, r, name);
    prx_log("%s failed: %s (%d)", what, name, (int)r);
    return false;
}

PRX_API int32_t prx_poll_log(char* out, int32_t capacity) {
    std::lock_guard lock(g_mutex);
    if (g_lines.empty()) return 0;
    snprintf(out, capacity, "%s", g_lines.front().c_str());
    g_lines.pop_front();
    return 1;
}
