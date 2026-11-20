// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
// Per-button gestures for keyboard + mouse mode: press, double press, hold, and grip + button combos. Pure logic
// (no Windows calls) so tests can drive it with their own clock.
#include <cstdint>

// Controller inputs that take gestures, in PrxBind order: A B X Y, grips, stick clicks, triggers.
constexpr int kGestureInputs = 10;
constexpr int kGripOf[kGestureInputs] = {5, 5, 4, 4, -1, -1, 4, 5, 4, 5};  // the same hand's grip; -1 for a grip
constexpr uint64_t kDoubleMs = 300, kLongMs = 500, kTapMs = 120;

struct GestureBinds {
    int32_t press[kGestureInputs], twice[kGestureInputs], held[kGestureInputs], combo[kGestureInputs];
};

class Gestures {
    enum Phase { Up, Press, Waiting, Twice, Held, Combo, Spent };
    Phase phase[kGestureInputs] = {};
    uint64_t pressAt[kGestureInputs] = {}, releaseAt[kGestureInputs] = {}, tapUntil[kGestureInputs] = {};
    bool tapPending[kGestureInputs] = {};

public:
    // Calls out(code) for every key or mouse button that should be down this frame.
    template <class Out>
    void update(const bool (&down)[kGestureInputs], const GestureBinds& b, uint64_t now, Out out) {
        bool comboOn[2] = {};  // a combo on the left (0) or right (1) grip is in use
        for (int i = 0; i < kGestureInputs; ++i) {
            Phase& p = phase[i];
            int grip = kGripOf[i];
            if (down[i] && p == Up) {
                if (grip >= 0 && b.combo[i] && down[grip]) p = Combo;
                else if (b.twice[i] && tapPending[i] && now - releaseAt[i] < kDoubleMs) p = Twice;
                else p = b.held[i] ? Waiting : Press;
                tapPending[i] = false;
                pressAt[i] = now;
            } else if (down[i] && p == Waiting && now - pressAt[i] >= kLongMs) {
                p = Held;
            } else if (!down[i] && p != Up) {
                // A short press of a button with a hold binding taps its own key on release.
                if (p == Waiting) tapUntil[i] = now + kTapMs;
                if ((p == Waiting || p == Press) && now - pressAt[i] < kDoubleMs) tapPending[i] = true, releaseAt[i] = now;
                p = Up;
            }
            if (p == Combo) comboOn[grip == 5] = true;
        }
        for (int i = 0; i < kGestureInputs; ++i) {
            Phase p = phase[i];
            // A grip whose combo is in use lets go of its own key until it is released.
            if (kGripOf[i] < 0 && comboOn[i == 5]) {
                if (p != Up) phase[i] = Spent;
                continue;
            }
            int code = p == Twice ? b.twice[i] : p == Held ? b.held[i] : p == Combo ? b.combo[i]
                     : (p == Press || now < tapUntil[i]) ? b.press[i] : 0;
            if (code > 0) out(code);
        }
    }
};
