// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

// Drives the gesture engine with a fake clock: ctest -C Release in build\native.
#include "../src/gesture.h"
#include <cstdio>
#include <set>

static int failures;
static void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL %s\n", what), ++failures;
}

struct Pad {
    Gestures g;
    GestureBinds b{};
    bool down[kGestureInputs] = {};
    uint64_t now = 0;
    std::set<int> step(uint64_t ms) {
        now += ms;
        std::set<int> keys;
        g.update(down, b, now, [&](int k) { keys.insert(k); });
        return keys;
    }
};

int main() {
    {  // Press only: the key follows the button with no delay.
        Pad p;
        p.b.press[0] = 1;
        p.down[0] = true;
        expect(p.step(10).count(1), "press holds at once");
        p.down[0] = false;
        expect(p.step(10).empty(), "press lets go");
    }
    {  // Hold binding: a short press taps the press key on release, a long one holds the hold key.
        Pad p;
        p.b.press[0] = 1, p.b.held[0] = 2;
        p.down[0] = true;
        expect(p.step(10).empty(), "hold waits");
        p.down[0] = false;
        expect(p.step(100).count(1), "short press taps");
        expect(p.step(200).empty(), "tap ends");
        p.down[0] = true;
        p.step(10);
        expect(p.step(600).count(2), "long press holds");
        p.down[0] = false;
        expect(p.step(10).empty(), "hold lets go, no tap");
    }
    {  // Double: the first press acts at once, the second within 300 ms holds the double key instead.
        Pad p;
        p.b.press[0] = 1, p.b.twice[0] = 3;
        p.down[0] = true;
        expect(p.step(10).count(1), "first press");
        p.down[0] = false;
        p.step(80);
        p.down[0] = true;
        auto k = p.step(80);
        expect(k.count(3) && !k.count(1), "second press is double");
        p.down[0] = false;
        p.step(10);
        p.step(400);
        p.down[0] = true;
        expect(p.step(10).count(1), "late press is a new press");
    }
    {  // Combo: right grip held, then A: the combo key replaces both the grip's and A's own keys.
        Pad p;
        p.b.press[0] = 1, p.b.press[5] = 5, p.b.combo[0] = 9;
        p.down[5] = true;
        expect(p.step(10).count(5), "grip alone");
        p.down[0] = true;
        auto k = p.step(10);
        expect(k.count(9) && !k.count(1) && !k.count(5), "grip + A is the combo");
        p.down[0] = false;
        expect(p.step(10).empty(), "grip stays quiet until released");
        p.down[5] = false;
        p.step(10);
        p.down[0] = true;
        expect(p.step(10).count(1), "A alone again");
    }
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures != 0;
}
