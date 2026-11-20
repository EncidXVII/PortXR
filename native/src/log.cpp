// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <share.h>
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
    // Tests: PORTXR_LOG_FILE gets every line at once, so the last one before a crash survives.
    static FILE* file = [] {
        char path[MAX_PATH];
        return GetEnvironmentVariableA("PORTXR_LOG_FILE", path, MAX_PATH) ? _fsopen(path, "a", _SH_DENYNO) : nullptr;
    }();
    if (file) fprintf(file, "%s\n", buf), fflush(file);
}

bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (xrResultToString && g_xr.instance) xrResultToString(g_xr.instance, r, name);
    prx_log("%s failed: %s (%d)", what, name, (int)r);
    // The runtime crashed or was closed: stop the frame loop so the plugin can tear down and retry.
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST || r == XR_ERROR_RUNTIME_FAILURE)
        g_xr.state = StateFailed;
    return false;
}

PRX_API int32_t prx_poll_log(char* out, int32_t capacity) {
    std::lock_guard lock(g_mutex);
    if (g_lines.empty()) return 0;
    snprintf(out, capacity, "%s", g_lines.front().c_str());
    g_lines.pop_front();
    return 1;
}
