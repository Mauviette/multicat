#pragma once
// mgmp_mainmenulock -- the client never presses Play itself.
//
// Cahier des charges (mgmp-fork): "Retirer le bouton Play du menu principal
// pour le client si possible." The host owns the run and clicks Play to pick
// a save slot; mgmp_savefile then redirects the LOAD and drives the client's
// own save-selection screen for it, and once the host's SAVEFILE lands,
// savefile_on_button_update presses Play FOR the client (see
// "PRESS PLAY FOR A CLIENT..." in mgmp_savefile.h). A client that pressed its
// own Play first would open ITS OWN save file -- one nobody is publishing to
// -- and end up on a run the host knows nothing about. There was never a
// legitimate reason for a client to click this button; this stops it.
//
// VERSION 2, 2026-09-05, SAME DAY. Version 1 forced the game's own DISABLED
// button state (kBtnState_Disabled, the grey a spell you cannot afford
// gets) -- cheap, and it broke the client's ability to connect at all: the
// assumption that Button::Click's real guards never check that state was
// never verified, and it was almost certainly wrong. See mgmp_tuning.h's
// kMainMenuLock note for the incident. Reverted, then replaced with this:
//
// HOOK glaiel::Button::Click DIRECTLY, at the SAME already signature-
// resolved address mgmp_savefile and mgmp_leave already CALL it through
// (Call::C_ButtonClick) -- so this needs no new signature and no raw RVA
// the way mgmp_invlock's InventoryItemBox::click hook did. Swallow the call
// only when it is a human click (name == "MainMenu_Button_Play") while this
// peer is a client; let everything else through untouched.
//
// THE COLLISION THIS DESIGN HAS TO SOLVE. MinHook patches the FUNCTION'S OWN
// bytes, not a specific call site -- so once the hook is installed, EVERY
// path into Button::Click lands in the same detour, including
// mgmp_savefile's own scripted press of this exact button (it calls the
// same address directly, cached before the hook existed). There is no
// ABI-level way to tell "the game's real dispatch from Button::update" apart
// from "our own injected call" from inside the hook -- both are just a call
// to the same function. mainmenulock_begin_injected_click /
// _end_injected_click is the flag that makes the distinction, the same
// pattern mgmp_choice.cpp already uses around its own level-up/event
// injection for the identical reason. mgmp_savefile's auto-Press wraps its
// call in this pair; nothing else needs to, because the block only ever
// matches the Play button by name and nothing else this mod presses is
// named that.

#include <cstdint>

namespace mgmp {

// Resolves Call::C_ButtonClick and installs the hook. Non-fatal if the
// address does not resolve: an unlocked Play button on the client is a
// missing nicety, not a correctness problem, so this only logs and leaves
// the feature off rather than refusing to load.
void mainmenulock_install_click_guard();

// Wrap the mod's OWN scripted press of ANY button in this pair -- currently
// only mgmp_savefile's auto-Play. See the header note above for why this is
// the only way to keep our own click from being swallowed by the same guard
// that exists to stop a human client's.
void mainmenulock_begin_injected_click();
void mainmenulock_end_injected_click();

} // namespace mgmp
