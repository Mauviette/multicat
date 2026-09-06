#pragma once
// mgmp_ownership -- F2's persistent per-player cat ownership, wired into a
// session. See mgmp_ownertable.h for the growth rules and why this can be
// keyed on POSITION rather than a CatData id or a Character pointer.
//
// WHAT TRIGGERS GROWTH. The run's total cat count -- the same
// MewDirector+1468 mgmp_catsync already trusts for identity -- read at the
// same point catsync/invsync/runhist already publish from: entering a map
// node. A battle can only ever be reached through a node, so by the time any
// battle's roster is snapshotted, both peers have already been through at
// least one "entering a node" tick with the current table -- the same
// ordering guarantee CATDATA and RUNHIST already rely on, and OWNERSHIP rides
// the identical choke point for the identical reason.
//
// WHY THE INITIAL SPLIT WAITS FOR A SECOND PLAYER. A solo host who has
// recruited cats before anyone joins must not have them all locked to peer 0
// the moment ownership first computes a table -- coop has not started yet.
// So the initial split is gated on net_peer_count() >= 2, not merely > 0 (0
// or 1 both mean "no real second player is here"), and a host playing alone
// leaves the table empty for as long as that stays true.
//
// WHERE IT IS CONSUMED. mgmp_lockstep's snapshot_cats, for the Nth
// human-brained cat it encounters in roster order, in place of the old
// per-battle split_for call -- see the note there for what happens if a
// position is not covered yet (it is not guessed at; see ownership_owner_of).

#include <cstdint>

namespace mgmp {

struct OwnershipMsg;

// Resolves the one thing this module needs: the MewDirector* global. Called
// from hooks_verify_module beside the other sync modules' _set_base, so a
// drift here is a startup refusal rather than a silent no-op three screens in.
void ownership_set_base(uintptr_t base);

void ownership_init();
void ownership_shutdown();

// HOST: read the run's current cat count, grow the table if it changed (the
// initial split or a recruit -- see mgmp_ownertable.h), and publish if the
// table grew since the last publish. Called from the same point
// catsync/invsync/runhist already publish from -- entering a map node -- and
// from the peer-joined/reconnected point they publish from too.
void ownership_publish(const char* why);

// HOST: force the next publish to send even if the table itself has not
// grown, for a peer that just joined or reconnected and has no local copy.
// Unlike catsync/invsync/runhist this needs no per-peer dedupe cache -- the
// whole table is compared by value, not diffed -- but a joiner still needs an
// unconditional resend, which this achieves by invalidating that comparison.
void ownership_forget();

// CLIENT: adopt the host's table outright. There is no older/newer ambiguity
// to resolve here the way there is for CATDATA or INVENTORY -- see the note
// on OwnershipMsg for why a straight overwrite is always correct.
void ownership_on_message(const OwnershipMsg& m);

// CLIENT: apply a table that arrived while this peer was mid-battle. Called
// from the map-follow tick, the same point catsync/invsync/runhist apply
// from.
void ownership_apply_pending(const char* why);

// Both: who owns the Nth human-brained cat in roster order (0-indexed), or
// kNoOwner (mgmp_ownertable.h) if the table does not cover that position yet.
// THE CALLER MUST NOT GUESS on kNoOwner -- see snapshot_cats, which leaves
// such a cat unclaimed by both peers and says so loudly rather than risk
// assigning it now and having the real table contradict that assignment
// later, which is exactly the mid-run reshuffle F2 exists to prevent.
uint8_t ownership_owner_of(uint32_t slot);

// Both, at the META level (map, level-up, event, inventory -- never battle):
// who owns the cat with this persistent CatData id (CatData+0x00), or
// kNoOwner if the id is not in this run's roster or its position has no
// owner decided yet.
//
// Resolves id -> position by finding it in the SAME id list mgmp_catsync
// already trusts (MewDirector+1468/+1472, in registry order), then defers to
// the same table ownership_owner_of(slot) reads. This is what F6 needs to
// restrict a level-up or an equip to the cat's actual owner: those happen on
// the map, where there is no battle roster to index into at all -- only the
// run's own cat list, which is exactly the position space this table is
// already keyed on (see mgmp_ownertable.h for why that space is shared).
uint8_t ownership_owner_of_cat(uint64_t cat_id);

// Both, at the META level: who owns the cat a live `CatData*` POINTER refers
// to, or kNoOwner if it does not resolve to any id in this run's roster.
//
// THIS IS THE ONE TO USE WHEN ALL YOU HAVE IS THE POINTER, which is every
// caller so far -- LevelUpScreen+160 and an inventory button's cached slot
// both hand over a CatData*, never the id. Dereferencing it for "the id at
// +0x00" was tried first and was wrong: CLAUDE.md's "CatData+0x00 is a u64
// id" describes the SERIALIZED stream's first field (right after the version
// tag), not necessarily the in-memory object's first member -- measured
// 2026-09-04, the value read back looked exactly like another heap pointer,
// not a small sequential id. This function never needs to answer that
// question at all: it resolves each of the run's known ids through the
// game's own CatData::by_id and compares the RESULTING pointer against the
// one the caller has, so where the real id field lives stays irrelevant.
uint8_t ownership_owner_of_catdata(const void* cat_data);

} // namespace mgmp
