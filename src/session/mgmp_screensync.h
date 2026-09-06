#pragma once
// mgmp_screensync -- F3.1 (cahier-des-charges-mgmp-fork.md): a screen-exit
// vote barrier, so a player who finishes an event or a shop/chest visit does
// not silently strand the other player on a screen that has already moved on
// (or, worse, race them into two different screens with no coordination at
// all -- see the shop-purchase softlock below).
//
// SCOPE OF THIS PASS. Three buttons, all plain glaiel::Button instances
// found live (see CLAUDE.md's F3.1 button-name table):
//
//   Event_ExitButton   ends a WorldEvent (with or without a choice) and
//                      returns to the map.
//   Shop_ExitButton    leaves a Shop screen -- CONFIRMED to be the SAME
//                      button/class for a real shop AND a treasure chest
//                      (ChestButton_alley's own RTTI resolves to glaiel::Shop,
//                      and no distinct "close chest" button was ever seen in
//                      a live session), so one mechanism covers both without
//                      needing to tell them apart BY NAME -- see the
//                      threshold note below for how they're told apart at all.
//   ChestButton_alley  opens/reveals a treasure chest. Added 2026-09-06,
//                      moved OFF mgmp_shopmirror's per-slot purchase mirror:
//                      that mechanism needs a shop-style "iN" slot key to
//                      resolve which item was bought, and a chest -- a
//                      single button, not an itemized list -- never carried
//                      one, so the mirror silently never fired and the
//                      other peer's chest just never opened. Live-reported
//                      bug, not a theoretical gap.
//
// Shop_BuyButton is still DELIBERATELY NOT handled here -- mirroring a real
// shop purchase needs per-item identity (mgmp_shopmirror's slot key), which
// a vote/threshold with no payload cannot express.
//
// THE MECHANISM: A VOTE WITH A THRESHOLD, not always "everyone votes."
// Event_ExitButton and a real shop's Shop_ExitButton keep the original F3.1
// behaviour: swallow the click, count votes, fire (on every peer) only once
// the count reaches the session's peer count -- so nobody is yanked off a
// screen they are still using. ChestButton_alley and Shop_ExitButton WHEN
// THE CURRENT NODE IS A TREASURE need only ONE vote to fire, which is
// functionally "the clicking peer fires immediately, the other peer follows
// off the wire" -- the same "one click, everyone follows" shape a shop
// purchase already has (see mgmp_shopmirror.h), chosen because a chest's
// contents are shared, one-shot state with no reason for one player to
// browse it while the other waits. Distinguishing the two `Shop_ExitButton`
// cases (same name, same class, per CLAUDE.md) reads the CURRENT MAP NODE'S
// TYPE via mgmp_follow's `follow_current_node`/`follow_node_info` (type 13
// == "treasure") rather than anything about the button itself. Either way,
// EVERY peer still fires the real click on ITS OWN copy of the button once
// its own threshold is reached -- a vote message carries no state to apply,
// only a reason to press a button this peer is already looking at.
//
// WHY A LIVE-POINTER CACHE INSTEAD OF A SCREEN-INSTANCE TOKEN. See
// ScreenVoteMsg's own header note in mgmp_proto.h: both peers reach the
// screen together (F3.2's node sync), so there is no case where a vote for
// an OLD screen needs to be told apart from one for a new one within a
// single peer's own timeline -- the tracked button POINTER changing (a new
// Button object was constructed) is what resets the count, checked every
// frame via screensync_on_button_update, the same per-frame tick
// mgmp_diag_buttons used to find these names.
//
// FIRING REUSES mgmp_mainmenulock's injected-click escape hatch
// (mainmenulock_begin_injected_click/end_injected_click) rather than adding a
// second one: Button::Click is hooked exactly once, by mainmenulock, and any
// simulated call into it has to pass through the SAME `injecting` guard or it
// re-enters every click handler recursively, screensync's own included.
#include <cstdint>

namespace mgmp {

struct ScreenVoteMsg;

void screensync_set_base(uintptr_t base);

// Called every frame from h_ButtonUpdate for EVERY button, same as
// diag_buttons used to be -- cheap (one std::string read, one strcmp against
// two names) outside the two buttons this cares about. Refreshes the live
// pointer for whichever tracked button this frame's `self` is, and resets
// that button's vote state if the pointer just changed (a new screen).
void screensync_on_button_update(void* self);

// Called from the shared Button::Click hook (mainmenulock owns the one
// MinHook install), BEFORE the original would run. Returns true if this
// click was fully handled here (the caller must not call the original and
// must not run any other button-click logic for this call) -- true for a
// real human click on a tracked button (always swallowed or already
// counted), false for everything else, including a screensync-injected
// firing (self-recursion is intercepted by mainmenulock's own `injecting`
// guard before this is ever reached).
bool screensync_on_button_click(void* self);

void screensync_on_message(uint8_t from, const ScreenVoteMsg& m);

// For mgmp_overlay: true while a vote is pending on THIS peer's screen (i.e.
// a tracked button is currently armed and its count has not yet reached the
// peer count), with the count/total to show as "X/N".
bool screensync_hud_pending(uint8_t& count, uint8_t& total);

} // namespace mgmp
