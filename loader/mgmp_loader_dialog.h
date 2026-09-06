#pragma once
// mgmp_loader_dialog -- the pre-launch connection picker.
//
// WHY THIS LIVES IN THE LOADER, NOT THE MOD. The game has no interactive
// overlay widget system outside the ImGui debug panel (a separate, F1-toggled
// window drawn INSIDE the game's own GL context) -- building text-entry and
// buttons into the game's own rendered UI would mean either extending ImGui
// into something that looks native (a real, ongoing UI project) or writing a
// whole clickable-widget layer from scratch on top of the screen-space
// overlay this mod already draws (mgmp_overlay.cpp), which currently only
// draws, never receives input. mgmp_loader.exe is a plain Win32 console
// program that runs BEFORE the game even exists as a process -- showing a
// native dialog there needs none of that, carries none of the risk of
// touching the game's own render/input path, and lets a player configure the
// connection without ever opening a text editor or the F1 panel.
//
// WHAT IT DOES. Reads mgmp.json's current net.role/addr/port to pre-fill the
// fields (same "seeded from disk" convention mgmp_ui.cpp's own connect panel
// already uses), shows a small DIALOGEX (see res/mgmp_loader.rc), and on
// "Lancer" writes the chosen role/addr/port straight back into mgmp.json --
// the loader already parses that file for the game path, and the DLL reads
// the very same three keys at startup, so no new hand-off mechanism is
// needed at all.
//
// WHY IT CAN BE SKIPPED, AND HOW. A blocking modal on every launch would
// break tools/net_test.ps1 outright -- it launches two loader processes back
// to back with the role already decided in each instance's own mgmp.json, no
// human present to click through two dialogs. So this is skipped, with NO UI
// shown at all, whenever ANY of:
//   - mgmp.json's "launcher.enabled" is false (the dialog's own "don't show
//     again" checkbox sets this, and net_test.ps1's generated per-peer
//     configs set it explicitly for exactly the reason above);
//   - the MGMP_NO_LAUNCHER=1 environment variable is set (a diagnostic
//     escape hatch, same shape as MGMP_NOINJECT/MGMP_WAIT/MGMP_NOFOLLOW).
// Either way, the existing behaviour (launch straight from whatever
// mgmp.json already says) is exactly what happens -- this is additive, not
// a replacement for the file-based config path.

#include <cstdint>

namespace mgmp_loader {

// Reads/shows/writes as described above. `dir` is the loader's own
// directory (where mgmp.json lives, same as game_from_config uses).
//
// Returns true if the caller should proceed to launch the game (either the
// dialog was skipped, or the user pressed "Lancer"). Returns false if the
// user pressed "Annuler" or closed the dialog -- the caller should exit
// quietly without launching anything.
bool run_launcher_dialog(const wchar_t* dir);

} // namespace mgmp_loader
