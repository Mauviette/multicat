#pragma once
// mgmp_invlock -- the host manages the run's equipment in its entirety; a
// client cannot equip or unequip anything, on ANY cat, including its own.
//
// REDESIGNED, 2026-09-06 -- the click-hook mechanism below is GONE. Read why
// before touching this file again.
//
// WHAT BROKE. The previous design hooked glaiel::InventoryItemBox::click()
// directly at a RAW HARDCODED RVA (0x34DF60) -- unlike every other hook in
// this mod, no signature was ever generated for it (no IDA on this machine),
// so it had no scanning fallback. The game's 2026-09-06 update drifted every
// OTHER signature-resolved address by a nonzero amount (measured: 53/54
// targets moved, deltas +80 to +56048 bytes) -- so this one, uniquely unable
// to follow, was left pointing at whatever unrelated function now happens to
// occupy that byte offset. MinHook's length-disassembler accepted it as a
// plausible function start (it installed "successfully" on both peers) and
// that is exactly the trap: acceptance proves nothing about correctness. Live
// evidence it was hooking the wrong function: a real, deliberate equip/
// unequip on the host never once produced the "equip/unequip settled" log
// line the hook was supposed to emit, across four separate test rounds.
//
// THREE LIVE ATTEMPTS TO RE-DERIVE THE REAL ADDRESS ALL CAME BACK EMPTY,
// 2026-09-06 -- keep this before trying a fourth:
//   1. Hooked the reliably-resolved glaiel::Button::Click and logged every
//      real click's return address. A deliberate unequip+re-equip produced
//      ZERO new return addresses beyond the ones already seen from ordinary
//      UI clicks (opening the bag, switching cat) -- InventoryItemBox::
//      click() does not call Button::Click as part of its own body.
//   2. Hooked Character::recompute_stats (equipment changes stats, so this
//      looked promising) -- zero hits. Dead end understood: recompute_stats
//      operates on a battle-only `Character*`; equipping from the map/
//      backpack screen mutates the save-side `CatData` instead, and there is
//      no live Character outside a battle at all.
//   3. A hardware data breakpoint (DR0-DR3, armed on the CURRENT thread only
//      via a DuplicateHandle'd real handle -- the exact safe shape CLAUDE.md's
//      F1.1 investigation already proved for this game; the OTHER shape,
//      suspending every thread, crashed it outright and must not be
//      repeated) directly on the four already-confirmed-correct equip-slot
//      addresses (CatData+0xA10/+0xA70/+0xAD0/+0xB30 -- verified live with a
//      real equipped item, "ExtraSetOfEyes", read back correctly at +0xA70)
//      STILL caught nothing across a real equip/unequip. The offsets are
//      right; whatever writes them is not reachable this way, most likely
//      because it runs on a different thread than the one the watch was
//      armed on.
// Re-deriving the exact RVA is an open-ended reverse-engineering problem with
// no guaranteed timeline (CLAUDE.md's own F1.1 saga took nine design
// iterations and ultimately needed IDA). The user chose NOT to keep chasing
// it and asked for a redesign that does not depend on any single hardcoded
// address at all -- this file is that redesign.
//
// THE NEW MECHANISM: watch the MEMORY, every frame, instead of hooking a
// function. `invlock_frame_tick()` (already called once per frame from
// FrameBegin) walks every cat in the run (mgmp_catsync's own id list -- the
// same one catsync_publish already walks, so no new address is needed) and
// compares its four equip-slot bytes against a per-cat baseline:
//
//   - On the CLIENT: a divergence with no matching authoritative-write
//     notification (see below) is a local click that must not stick --
//     eventually overwritten back to the baseline (see ROUND 4 below for why
//     "eventually" and not "immediately"), logged, done. No function needs
//     to be identified for this to work; it only needs to be able to read
//     and write memory, which every build of this game supports identically.
//   - On the HOST: a divergence IS the host's own legitimate change -- the
//     baseline adopts it and arms the existing debounced publish
//     (unchanged from the old design: mgmp_tuning::kInvSyncDebounceFrames
//     frames after the LAST divergence, so a burst of clicks still
//     collapses into one publish).
//
// ROUND 4, 2026-09-06 -- a real, live-reported bug, not a cosmetic one this
// time: "if the client fiddles enough, they can duplicate items and equip
// them" -- and an equipment mismatch IS a real desync (the ability
// cross-check catches it even though `state_hash` does not, and HALTs; see
// CLAUDE.md). Root cause: reverting on EVERY SINGLE FRAME a divergence was
// seen (this file's own ROUND 3, and the immediate `invsync_reapply_last_good`
// it called) hammered the game's own bag reader up to 60 times a second
// during a rapid click burst -- exactly the frequency class mgmp_invsync.h's
// F5 history already found produces item duplication in the game's OWN
// local state, just on the read/apply side instead of the write/serialize
// side that bug was first found on. Fixed by separating DETECTING a change
// (every frame, cheap, `check_cat`) from CORRECTING it (`run_client_
// correction`, at most once per `kInvSyncDebounceFrames`-frame window with
// nothing moving) -- a rapid burst of clicks now costs ONE correction, not
// one per frame it was ever visible, giving the game's own multi-step
// mutation (bag node + cat slot are two separate writes) time to actually
// finish before this module's own corrective write lands on top of it. This
// is NOT the screen-close deferral tried and reverted earlier the same day
// (that waited on a UI STATE, sometimes for a long time, and made the
// visual glitch worse) -- this is a short, fixed settle window on the
// CORRECTION'S OWN CADENCE, the same shape the original click-hook design's
// publish debounce always had, applied to the client's revert instead of
// the host's publish.
//
// THE ECHO PROBLEM THIS HAS TO AVOID: an incoming CATDATA the client applies
// (a legitimate host equip arriving over the wire) also changes these same
// bytes. Without help, the very next frame's poll cannot tell that apart from
// a local click and would revert the host's own change right back out.
// Fixed by `invlock_note_authoritative_catdata`, called by mgmp_catsync.cpp
// immediately after every successful apply -- it refreshes the baseline to
// the just-applied bytes BEFORE the next poll runs, so the poll sees no
// divergence at all for a legitimate change. Same shape as mgmp_catsync's own
// echo-loop fix for its hash cache, for the identical reason.
//
// WHY THIS IS MORE ROBUST THAN THE THING IT REPLACES, NOT JUST A WORKAROUND:
// CatData's field LAYOUT (a compiled struct shape) has survived the
// 2026-09-06 update unchanged, confirmed live, while every CODE address in
// the image drifted. Struct offsets are "kept, semantic" per CLAUDE.md's own
// signature-resolution philosophy -- this leans on the more stable of the
// two categories instead of the more fragile one, and needs no signature,
// no IDA, and no re-derivation after a future update at all.
//
// SIDE EFFECT: this also restores F5's "changes reach the other peer without
// waiting for a node/battle boundary" for equipment specifically, which
// depended on the SAME now-dead click hook to arm its debounce. The host's
// own equip is detected by the poll exactly like the client's would-be
// local edit is, so the debounced publish fires again on a real change.
//
// ROUND 5, 2026-09-06 -- a PREVENTIVE block added alongside the reactive
// poll, at the user's own suggestion after the reactive-only design kept
// surfacing new edge cases (rapid-click duplication, then a second
// duplication even from a legitimate HOST-driven unequip reaching the
// client). `invlock_on_button_click` swallows a real click on any
// `EquippedButton_<type>`/`InventoryButton_<type>` button (F6's own
// previously-established name family -- see CLAUDE.md's F6 history) when
// `net_role()==Client`, via the SAME shared, RELIABLE, signature-resolved
// `Button::Click` hook screensync/shopmirror/follow/mainmenulock already
// share -- no raw RVA, unlike the original (and now-dead)
// InventoryItemBox::click() hook this file's ROUND 1 note describes.
//
// WHY THIS WAS NOT THE FIRST THING TRIED: earlier diagnostics (see ROUND 1's
// "three live re-derivation attempts") logged Button::Click firing on these
// exact button names even when the screen merely OPENED or the selected cat
// switched -- with no way at the time to tell that apart from a real click,
// which read as "this isn't the real command point." It may still not BE
// the deepest point (the actual mutation might live in a function these
// buttons' own Button::Click internally reaches, unconfirmed either way),
// but swallowing the click here is cheap to try and, if it works, needs no
// further RE at all. The poll above is kept regardless, as a backstop for
// any equip path that does not go through one of these two button
// families.
//
// STAYS A CLIENT-ONLY BLOCK, SAME AS THE POLL -- the host's own clicks on
// these buttons must reach the game untouched.

#include <cstdint>

namespace mgmp {

void invlock_set_base(uintptr_t base);

// From the shared Button::Click hook (mainmenulock owns the one MinHook
// install), BEFORE the original would run -- same calling convention as
// screensync_on_button_click/shopmirror_on_button_click/
// follow_on_button_click, which this shares the hook with. Returns true
// (swallowed, caller must not call the original) for a real client click on
// an EquippedButton_*/InventoryButton_* button; false for everything else,
// including the host's own clicks and any injected/scripted press.
bool invlock_on_button_click(void* self);

// F6/F4, 2026-09-06: "whose cat is this, while I'm looking at it in the
// bag" -- the same ownership badge the level-up/event screens already show,
// requested for the inventory screen too. From the shared T_ButtonUpdate
// hook (mgmp_hooks.cpp's h_ButtonUpdate), fires for every button; cheap
// outside the one name it cares about. Caches `CatSelector_Right`'s own
// live CatData* (Button+0x18C8 -- the same field F6's original, since-
// abandoned per-cat ownership design read, see mgmp_invlock.h's ROUND
// history above for why equip-blocking itself no longer depends on this)
// and a timestamp, so the cache self-clears the moment the inventory screen
// closes and CatSelector_Right stops ticking, the same recency-check shape
// mgmp_choice.cpp's choice_level_screen_owner already uses.
void invlock_on_button_update(void* self);

// kNoOwner if the inventory screen is not open (or has not ticked in the
// last 250ms) or the selected cat's owner is not yet decided; otherwise the
// peer index that owns the cat currently shown. Purely informational --
// unlike invlock_on_button_click above, nothing here gates any action.
uint8_t invlock_current_cat_owner();

// Announces the new design once, if armed. Kept named `_init` rather than
// `_install_click_guard` -- there is no click hook to install any more.
void invlock_init();

// Call once per frame from FrameBegin (same call site the old debounce timer
// used). Walks every cat in the run and enforces the equip-slot invariant
// described above. Cheap: a few dozen cats at 0x180 bytes of memory reads
// each, no game-function calls at all on the common (unchanged) path.
void invlock_frame_tick();

// Called by mgmp_catsync.cpp right after a successful incoming CatData apply
// -- refreshes this cat's baseline to the bytes that were just legitimately
// written, so invlock_frame_tick's very next poll does not mistake a real
// host update (arriving on the client) for a local click and revert it.
void invlock_note_authoritative_catdata(uint64_t id, const void* catdata);

} // namespace mgmp
