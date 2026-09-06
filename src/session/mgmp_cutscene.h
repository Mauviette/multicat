#pragma once
// mgmp_cutscene -- auto-skip the intro cutscene.
//
// WHY THIS EXISTS. mgmp_leave now returns a client to the main menu on its
// own (see mgmp_leave.h), which makes the main menu's intro cutscene play far
// more often than it would for an ordinary player -- every time the host
// leaves a run, not just at the first cold boot. The game's own skip is a
// HELD left click (confirmed by a player describing exactly that to get past
// it), so a peer who is not actively watching their own screen at that
// moment sits through it in full.
//
// THE MECHANISM is the same class of synthesis mgmp_leave's Escape press
// uses: PostMessage a real WM_LBUTTONDOWN to the game's own window when the
// "Cutscene" scene is seen loaded, and WM_LBUTTONUP the moment it is not --
// SetForegroundWindow first, for the same measured reason the Escape press
// needed it (see mgmp_leave.cpp's press_escape).
//
// WHY A SHORT POLL, NOT THE USUAL kPollFrames-style half-second one. A held
// click that outlives the cutscene by even a fraction of a second is a held
// click over whatever loads right after -- the main menu's own buttons, most
// of the time -- and an accidental press there is a worse failure than a
// slightly slower skip. Polled every kPollFrames frames (see the .cpp),
// deliberately much shorter than mgmp_leave's, to bound that window.
//
// SAFETY NET: released unconditionally after kMaxHoldFrames regardless of
// what the scene read says, so a drifted scene offset degrades to "the
// cutscene stops being skipped early" rather than "a mouse button is stuck
// down for the rest of the process".
#include <cstdint>

namespace mgmp {

void cutscene_init();
void cutscene_shutdown();

// From h_FrameBegin, unconditionally -- this is a local, per-machine
// presentation skip with no protocol involvement, so it runs regardless of
// session phase or role, the same as the ImGui panel.
void cutscene_pump();

} // namespace mgmp
