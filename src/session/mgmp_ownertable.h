#pragma once
// mgmp_ownertable -- F2's persistent per-player cat ownership, as arithmetic.
//
// Extracted for the same reason mgmp_split is: exercising the growth rule in
// the game needs a live run, a recruit, and enough patience to reach one,
// which is a slow way to find out that a tie broke the wrong way. Here it is
// a table.
//
// WHY POSITION, NOT A POINTER OR AN ID. F2 (cahier-des-charges-mgmp-fork.md)
// wants a per-player cat assignment that survives the battle roster changing
// shape. The obvious identity is CatData's own id -- mgmp_catsync already
// trusts it across processes and battles -- but applying it INSIDE a battle
// means resolving a roster Character* back to that id, and no such link
// exists: a live memory scan (widened to 64KB, page-chunked, with a
// round-trip check) found none. See CLAUDE.md's Character field table --
// nothing there is named CatData either.
//
// What IS measured, live, 2026-09-04: the battle roster's human-brained cats
// appear in the SAME RELATIVE ORDER across different battles for a fixed
// team (two battles, same two cats, roster indices 5/6 then 20/21 -- position
// 0 and position 1 among humans carried the same identity both times). That
// matches CLAUDE.md's own claim that battle-roster order is authored spawn
// order. So slot N here means "the Nth human-brained cat encountered in
// roster order", which both peers derive locally and identically without any
// pointer at all -- exactly the way mgmp_split already derives the per-battle
// control split from roster position.
//
// Caveat carried forward rather than hidden: this was only measured with a
// two-human-cat team, where "order preserved" and "order reversed" are the
// only two options. If a 3+ human battle ever shows the sublist order change
// between fights with the same team, this assumption is wrong and needs
// revisiting -- do not delete this paragraph if that happens, extend it.
//
// TWO GROWTH RULES, NOT ONE, because they answer different questions.
//
//   initial_split -- decided ONCE, the moment coop genuinely starts (a second
//     player is in the session and the run already has some cats). Fair
//     division over all players at once, contiguous per player, host first --
//     the same formula and the same preference mgmp_split already uses for a
//     battle roster, applied here to the PERSISTENT position space instead so
//     it only ever runs a single time. Doing this one recruit at a time from
//     an empty table while the host is still playing solo would dump every
//     early cat on the host before a client ever gets a share.
//
//   extend_round_robin -- every cat recruited AFTER that never moves an
//     existing slot; it only ever appends. It goes to whichever player
//     currently owns the fewest slots, ties toward the lowest peer id (the
//     host) -- confirmed with the user as the rule to use, 2026-09-04.

#include <cstdint>

namespace mgmp {

constexpr uint32_t kMaxOwnerSlots = 32;   // matches ControlMsg's human-cat cap
constexpr uint8_t  kNoOwner       = 0xFF;
// Bounds the tally scratch space in extend_round_robin. Real sessions have at
// most kMaxPeers (4) from mgmp_proto.h; this header stays free of that
// dependency the way mgmp_split.h does, so the bound is restated rather than
// shared, generously.
constexpr uint32_t kMaxOwnerPeers = 8;

struct OwnerTable {
    uint32_t count = 0;                     // slots decided so far
    uint8_t  owner[kMaxOwnerSlots] = {};     // owner[i] = peer id, valid for i < count
};

// kNoOwner if `slot` has not been decided yet (including anything past
// kMaxOwnerSlots, which can only mean the table or the input drifted).
inline uint8_t owner_of(const OwnerTable& t, uint32_t slot) {
    if (slot >= t.count || slot >= kMaxOwnerSlots) return kNoOwner;
    return t.owner[slot];
}

// Called only when the table is still empty. `total` is how many cats the run
// already has; `peers` is how many players are in the session. A no-op if
// called again once count is nonzero, so a caller that is unsure whether the
// initial split has happened yet may call this unconditionally.
inline void initial_split(OwnerTable& t, uint32_t total, uint32_t peers) {
    if (t.count != 0 || total == 0) return;
    if (peers == 0) peers = 1;   // same convention as mgmp_split's split_for
    if (total > kMaxOwnerSlots) total = kMaxOwnerSlots;

    const uint32_t base  = total / peers;
    const uint32_t extra = total % peers;
    uint32_t i = 0;
    for (uint32_t p = 0; p < peers && i < total; ++p) {
        uint32_t share = base + (p < extra ? 1u : 0u);
        for (uint32_t k = 0; k < share && i < total; ++k, ++i) t.owner[i] = (uint8_t)p;
    }
    t.count = total;
}

// Appends slots [t.count .. total) one at a time, each to the peer with the
// fewest slots AT THAT MOMENT (so two consecutive recruits can land on two
// different players rather than both going to today's minimum). Never
// rewrites owner[0..old count), which is the entire point.
inline void extend_round_robin(OwnerTable& t, uint32_t total, uint32_t peers) {
    if (peers == 0) peers = 1;
    if (peers > kMaxOwnerPeers) peers = kMaxOwnerPeers;
    if (total > kMaxOwnerSlots) total = kMaxOwnerSlots;

    while (t.count < total) {
        uint32_t tally[kMaxOwnerPeers] = {};
        for (uint32_t i = 0; i < t.count; ++i)
            if (t.owner[i] < peers) ++tally[t.owner[i]];

        uint8_t best = 0;
        for (uint8_t p = 1; p < peers; ++p)
            if (tally[p] < tally[best]) best = p;

        t.owner[t.count++] = best;
    }
}

// The one entry point a caller actually needs: does whichever of the two
// rules above applies. `peers` must already reflect that coop has started
// (the caller's job -- see mgmp_ownership.cpp -- is not to call this with
// peers < 2 while the table is still empty, or a solo host would lock in
// 100% of the run before anyone else ever joins).
inline void grow(OwnerTable& t, uint32_t total, uint32_t peers) {
    if (t.count == 0) initial_split(t, total, peers);
    else               extend_round_robin(t, total, peers);
}

} // namespace mgmp
