// mgmp_lockstep.h -- deterministic lockstep over the battle. Phase 4, layer 3.
//
// THE SEAM IS THE ONE THE REPLAYER ALREADY PROVED. Phase 2B injected recorded
// decisions at Brain::GetChoice and got a byte-identical battle back -- 17 of
// 29 decisions injected, the other 12 left for the AI to re-derive, all 12
// matched. This module swaps the recorded FIFO for a socket. Same hook, same
// injection point, same validation; the decisions now arrive from a peer.
//
// WHY WAITING IS FREE. GetChoice is a *poll*, not a decision point: it returns
// type=1 ("nothing decided yet") on every frame while waiting on a human, and
// did so for 1695 of 1711 calls in one tutorial battle. So a remote decision
// that has not arrived needs no timeout, no blocking recv and no frame budget.
// Return type=1 and the game waits exactly as it already waits for a person.
//
// CAT IDENTITY, AND WHY IT IS A SNAPSHOT.
//
// Pointers are unusable across peers, and turn order is unusable as an index:
// NextTurn shuffles the turn list every round with an inlined Fisher-Yates
// (sub_140085F80) drawing from the simulation stream TLS+0x178. Deterministic,
// so lockstep-safe -- but a cat's position in it changes every round.
//
// The pre-shuffle source is the battle's character list:
//
//     TurnControl+0x18 -> +0x08 -> +0x20 -> +0x1F90
//         { u32 refcount@0; u32 pad@4; u32 cap@8; u32 count@12; Character** data@16 }
//
// (the same list Ability::TargetIsValid and Character::BeginTurn/EndTurn read).
// Its ORDER is spawn order -- authored, identical on both peers given the same
// save and RNG state.
//
// We snapshot it once at battle start and index into the snapshot. That is
// deliberately not a live lookup: summons append to the live list and deaths
// may remove from it, either of which would renumber every cat mid-battle. A
// snapshot pins the numbering for the whole battle, and Character pointers are
// stable *within* one battle (they are only unstable across runs, which is what
// runs C/D measured -- 0 of 21 matched).
//
// CONTROL IS A SPLIT OF INPUT, NOT OF STATE. Both peers simulate every cat
// identically. `net_control` only says which cats local input may decide for.
// For every other cat this module overwrites GetChoice unconditionally, which
// is also what suppresses a stray click on a cat you do not own.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

// Called once the transport reports Ready. Reads net_control from the config.
// Verifies and resolves the one game function this module CALLS
// (Character::get_affecting_elements). Called from hooks_install alongside
// catsync_set_base. Failure is non-fatal and only turns off element hashing.
void lockstep_set_base(uintptr_t base);

void lockstep_init();
void lockstep_shutdown();
bool lockstep_active();

// Drains the network queue and dispatches. Safe to call every frame; it is
// called from the frame hook so messages arrive even while no brain is polled.
void lockstep_pump();

// --- the seam ---------------------------------------------------------------

// Mirror of replay_fill_choice. Returns true if `out` was overwritten.
//
//   locally-controlled cat -> false, but the brain's own decision is captured
//                             and sent if it is a real one (type 2 or 3);
//   remote cat             -> true, always: either the peer's decision, or
//                             type=1 to keep waiting.
bool lockstep_fill_choice(void* brain, void* out);

// Called from the ApplyTurnAction hook, before the original runs. Clears the
// outstanding-send guard and validates that what landed is what we expected.
void lockstep_on_applied(const void* action, const void* actor);

// Called from the NextTurn hook. Snapshots the cat list on the first call of a
// battle, then exchanges and compares the turn hash.
void lockstep_turn_boundary(void* turn_control);

// --- what the cursor overlay needs ------------------------------------------

// This peer's battle counter, so a cosmetic message crossing a battle boundary
// can be dropped rather than drawn on the wrong board. 0 outside a battle.
// Which battle this peer is in: the node seed both peers read out of
// MapNode+0x118. kNoBattle when not in one. See mgmp_battleid.h.
uint64_t lockstep_battle_id();

// From the map layer, on BOTH peers, as a node is entered. Establishes battle
// identity without any negotiation -- both peers pass the same seed because
// they entered the same node.
void lockstep_enter_battle(uint64_t seed0);

// A peer joined or reconnected: replay this battle's decisions to it so it can
// fast-forward into a fight already under way. To that peer only.
void lockstep_catchup(uint8_t peer);

// True when the cat the game is currently asking for a decision is one this
// peer controls -- the "it is your turn" bit, and the thing peer cursors fade
// on. Refreshed at every GetChoice poll, which happens on essentially every
// frame of a battle, so it is never more than a frame stale.
//
// Deliberately NOT derived from the turn order: a cat's position in it is
// reshuffled every round by NextTurn's inlined Fisher-Yates, so the only stable
// answer comes from the roster index of whoever is actually being polled.
bool lockstep_local_actor();

// Does a peer -- specifically NOT this one -- own the input for this exact
// Character? Asked by the combat-menu lock, which has a Character* in hand
// (CombatMenu+280) and must not depend on who happens to be being polled.
//
// lockstep_local_actor answers "is the cat currently being asked for a decision
// mine", which is refreshed at a GetChoice poll and therefore holds its last
// value for as long as a decision stays cached -- fine for fading a cursor,
// wrong for a menu that is on screen across that whole window.
//
// Answers false for everything it cannot PROVE: no session, no snapshot, a cat
// that is not in the roster (a summon), or an AI cat. The caller is cosmetic,
// and a menu greyed out when it should not be reads as a bug in the game.
bool lockstep_peer_owns_character(const void* character);

// F4 (cahier-des-charges-mgmp-fork.md): which peer owns this Character, for
// the turn-owner badge -- generalises lockstep_peer_owns_character's boolean
// to an actual peer id. kNoOwner (mgmp_ownertable.h) covers everything that
// function returns false for (no session, a summon, an AI cat) plus the one
// case it cannot distinguish from those: a human cat whose owner the F2 table
// has not decided yet.
uint8_t lockstep_owner_of_character(const void* character);

// F4: the board tile a live Character* is standing on, trusted only if its
// TacticsObject round-trips back to it -- same validation read_cat_state
// applies for the state hash, exposed standalone for the badge. False (with
// x=y=0) if the round trip does not hold; the caller must not guess.
bool lockstep_character_tile(const void* character, int32_t& x, int32_t& y);

// F4: the Character* currently being asked for a decision, but only when a
// HUMAN is deciding -- nullptr for an AI/summon turn (nothing to badge) or
// between battles. Refreshed every frame of the actor's turn, same as
// lockstep_local_actor's own source.
const void* lockstep_current_actor();

// --- diagnostics ------------------------------------------------------------

struct LockstepStats {
    uint32_t sent = 0, applied = 0, pending = 0;
    uint32_t desyncs = 0;
    uint32_t cats = 0, local_cats = 0;
    bool     halted = false;
};
LockstepStats lockstep_stats();

// True once a HASH mismatch or a HALT from the peer has stopped the battle.
bool lockstep_halted();

// Whether this peer is standing in a battle it has already snapshotted -- i.e.
// a fight is on screen and lockstep is driving it.
//
// The distinction that matters is against a peer which is merely CONNECTED
// while the other one fights: that peer has no snapshot, so it returns false
// and is safe to push state at. A peer for which this returns true has live
// Character objects whose state belongs to the deterministic stream, and
// writing to them out of band changes the battle underneath the hashes.
//
// mgmp_catsync and mgmp_invsync gate their apply paths on it. See the note on
// catsync_on_message for the shield that got through before they did.
bool lockstep_in_battle();

// The aim preview writes Character+0x388, and the backstab test reads it, so a
// wall-clock-gated presentation write decides damage. Call `begin` on entry to
// Brain::UpdateDecision and `end` on the way out: whatever the preview turned
// the cat to is put back, leaving facing written only by committed actions.
// Both are inert outside a net session. See the long note on the definitions.
// Everything mgmp_aim needs about one character, answered once: is it a HUMAN
// cat in this battle's roster at all, which index, and does the PEER own it.
// False for a summon, for an AI cat and outside a battle -- an aim preview is
// meaningless in all three cases.
bool lockstep_aim_subject(const void* character, uint8_t& cat, bool& peer_owns);

void     lockstep_preview_facing_begin(void* brain);
void     lockstep_preview_facing_end();
uint32_t lockstep_preview_facing_count();

// F1.1: called from lockstep_on_applied for whichever cat a real TurnAction
// (type 2 or 3) just resolved for. Pushes that cat's CURRENT, settled facing
// as authoritative if this peer owns it -- not on every write, just once per
// action, which end-turn guarantees happens at least once per turn
// regardless of how the cat got to its final facing. See mgmp_lockstep.cpp
// for the earlier, continuously-policing shapes this replaced and why each
// one broke, and for the known limitation this simplification accepts.
void lockstep_on_action_applied(const void* actor);

// F1.1: called from lockstep_turn_boundary, once per turn boundary, for every
// locally-owned human cat -- deliberately NOT once per frame. See the long
// note in mgmp_lockstep.cpp for why: Character+0x388 is not a safe read while
// a cat's CombatAnimation is in flight (the same reason it cannot go in the
// state hash), and a per-frame poll reads it constantly during exactly that
// window. The turn boundary is the same "settled" checkpoint
// trace_state_deltas already relies on for the same field.
void lockstep_face_turn_tick();

// F1.1: called every frame (from FrameBegin). Continuously broadcasts every
// locally-owned human cat's facing EXCEPT the current actor's -- on change,
// and on a 200ms heartbeat regardless of change. An idle cat has no
// CombatAnimation in flight, so it is safe to poll every frame, and doing so
// is what a turn boundary alone cannot: an attack decided independently on
// another peer is not gated by anything, so the window between a free click
// and an attack that reads it can be shorter than one turn boundary. The
// heartbeat matters as much as the on-change send: a cat whose value is not
// currently changing still needs re-affirming, because the OTHER (non-owning)
// peer can corrupt its own local copy at any time with nothing on the
// sending side to detect it. The acting cat is excluded and left to the
// action-boundary push and the turn-boundary re-affirm, both of which read it
// at moments already known to be free of an in-flight animation. See the long
// note in mgmp_lockstep.cpp.
void lockstep_face_frame_tick();

struct FaceMsg;
// From the lockstep pump: applies an incoming facing change to the named
// cat. A real input, so a stale battle is dropped the same way ACTION is,
// never silently accepted.
void lockstep_on_face_message(const FaceMsg& m);

// --- the state fence -------------------------------------------------------
//
// Snapshot every snapped cat's simulation state, run something, then check that
// none of it moved. Built for mgmp_aim, which calls a function INTO the game on
// the one peer that does not own the cat -- the shape where any state write is
// a divergence by construction.
//
// `end` puts FACING back, because that is the field the aim path is known to
// write and the one whose correct value is unambiguous (the last action's).
// Everything else it can only report: a moved HP or tile is already a
// divergence and inventing a value for it would hide that. Returns the number
// of cats that changed, and names the first few in the log.
//
// Cheap enough for a per-frame caller: one pass over the roster, no allocation,
// and it returns immediately when lockstep is not in a battle.
void     lockstep_state_fence_begin();
uint32_t lockstep_state_fence_end(const char* what);
uint32_t lockstep_state_fence_hits();

} // namespace mgmp
