// mgmp_cutscene.cpp -- see mgmp_cutscene.h.

#include "mgmp_cutscene.h"
#include "mgmp_leave.h"      // leave_scene_is_loaded
#include "mgmp_ui.h"         // ui_game_window
#include "mgmp_log.h"
#include "mgmp_addresses.h"  // kScene_Cutscene

#include <windows.h>

namespace mgmp {
namespace {

// Short and deliberately much shorter than mgmp_leave's kPollFrames -- see
// the header note on why a held click bleeding into the next screen is worse
// than a slightly slower skip.
constexpr uint32_t kPollFrames    = 5;     // ~80 ms at 60 Hz
constexpr uint32_t kMaxHoldFrames = 900;   // ~15 s -- release regardless, safety net

struct State {
    bool     on             = false;
    bool     holding        = false;
    uint32_t tick           = 0;
    uint32_t held_for       = 0;
    bool     said_no_window = false;
};
State g;

void release(const char* why) {
    if (!g.holding) return;
    g.holding  = false;
    g.held_for = 0;
    HWND hwnd = (HWND)ui_game_window();
    if (hwnd) PostMessageW(hwnd, WM_LBUTTONUP, 0, 0);
    log_line_lvl(LogLevel::Trace, "CUTSCENE", "released the held click (%s)", why);
}

} // namespace

void cutscene_init() {
    g = State{};
    g.on = true;
    log_line_lvl(LogLevel::Trace, "CUTSCENE",
             "armed -- the intro cutscene will be auto-skipped");
}

void cutscene_shutdown() {
    release("shutdown");
    g.on = false;
}

void cutscene_pump() {
    if (!g.on) return;

    if (g.holding && ++g.held_for > kMaxHoldFrames) {
        release("safety timeout -- the scene read never said the cutscene ended");
        return;
    }

    if (++g.tick % kPollFrames) return;

    const bool up = leave_scene_is_loaded(kScene_Cutscene);
    if (up && !g.holding) {
        HWND hwnd = (HWND)ui_game_window();
        if (!hwnd) {
            if (!g.said_no_window) {
                g.said_no_window = true;
                log_line_lvl(LogLevel::Error, "CUTSCENE",
                             "!! no game window yet -- cannot auto-skip");
            }
            return;
        }
        // Same measured reason as mgmp_leave's press_escape: a synthesized
        // input this game's window did not have focus for was seen to be
        // silently ignored, and a bare SetForegroundWindow was not reliable
        // enough on its own -- see ui_force_foreground's own comment.
        ui_force_foreground(hwnd);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int cx = (rc.right  - rc.left) / 2;
        const int cy = (rc.bottom - rc.top)  / 2;
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));

        g.holding  = true;
        g.held_for = 0;
        log_line("CUTSCENE", "the intro cutscene is up -- holding a click to skip it");
    } else if (!up && g.holding) {
        release("cutscene scene gone");
    }
}

} // namespace mgmp
