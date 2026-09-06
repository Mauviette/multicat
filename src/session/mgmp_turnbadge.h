#pragma once
// mgmp_turnbadge -- F4 (cahier-des-charges-mgmp-fork.md): during a human
// turn, mark the acting cat's tile with the OWNING PLAYER's colour, so either
// screen shows whose cat's turn is active without reading a name.
//
// NO WIRE MESSAGE. Both peers already compute everything this needs locally
// and identically: mgmp_lockstep's own current-actor tracking (refreshed
// every frame of a human turn by fill_choice) plus F2's ownership table
// resolve to the same Character* and the same owner id on both machines,
// given the same battle roster -- the same determinism guarantee the rest of
// this project already leans on, not a new one. This is presentation riding
// on state that was already synchronised for other reasons, same shape as
// mgmp_cursor's alpha-is-the-turn-indicator, just keyed on WHICH peer rather
// than "mine or not".
//
// SAME DRAWING PRIMITIVES AS mgmp_cursor, same reasons -- see mgmp_cursor.h
// for why a Component was rejected in favour of the battle screen's
// glaiel::ImmediateModeGameUI. Resolves its own copy of the two call targets
// rather than sharing mgmp_cursor's (which are file-local there), matching
// how every other session module resolves independently.
//
// REUSES THE "TargetCursor" ANIMATION mgmp_cursor already proved safe,
// rather than guessing a new SWF clip name: Renderer::init fatal-throws a
// std::string on an unknown MovieClip name, unwinding with no mgmp.dll frame
// on the stack (see mgmp_cursor.cpp's header note) -- a cost worth avoiding
// for a cosmetic feature. A distinct immediate-mode id keeps this piece
// independent of the peer reticle even when both land on the same tile.
//
// FAILS SILENT, LIKE mgmp_combatlock. No session, no snapshot, an AI/summon
// turn, or ownership the F2 table has not decided yet -- lockstep's own
// current_actor()/owner_of_character() already say "nothing to draw" for
// every one of those; this module only checks for null/kNoOwner.

#include <cstdint>

namespace mgmp {

void turnbadge_set_base(uintptr_t base);

// From h_StatusMenuUpdate, alongside cursor_on_status_menu. Needs the same
// ImmediateModeGameUI the cursor draws through, so it only ever does
// anything on a genuine battle screen -- off battle there is no acting cat
// to badge in the first place (lockstep_current_actor() answers nullptr).
void turnbadge_on_status_menu(void* status_menu);

} // namespace mgmp
