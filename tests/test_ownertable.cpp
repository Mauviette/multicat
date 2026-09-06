// Pins mgmp_ownertable.h -- F2's persistent per-player cat ownership. In-game
// this needs a real run and a recruit to exercise, so the arithmetic is
// pinned here and the live run is left to prove only what needs a game.
//
// The property that actually matters is not any single row: it is that an
// EXISTING slot never changes once grow() has assigned it, across any
// sequence of totals and peer counts. That is F2's whole requirement --
// "no recalcul automatique en cours de partie" -- and the exhaustive sweep at
// the bottom is what actually proves it; the hand-written rows above it say
// WHICH case broke when it doesn't.

#include "mgmp_ownertable.h"

#include <cstdio>
#include <cstring>

using namespace mgmp;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) { printf("FAIL: %s\n", what); ++failures; }
}

int main() {
    // --- initial_split: fair, contiguous, host-first, same shape as mgmp_split -
    {
        OwnerTable t{};
        initial_split(t, 4, 2);
        check(t.count == 4, "initial_split(4,2) decides all 4 slots");
        check(owner_of(t, 0) == 0 && owner_of(t, 1) == 0, "first half is peer 0");
        check(owner_of(t, 2) == 1 && owner_of(t, 3) == 1, "second half is peer 1");
    }
    {
        OwnerTable t{};
        initial_split(t, 3, 2);
        check(owner_of(t, 0) == 0 && owner_of(t, 1) == 0, "host takes the odd one");
        check(owner_of(t, 2) == 1, "remainder split is 2/1, not 1/2");
    }
    // Called again once nonzero: a no-op, not a re-split.
    {
        OwnerTable t{};
        initial_split(t, 4, 2);
        initial_split(t, 4, 2);   // must not touch anything
        check(t.count == 4, "a second initial_split call is a no-op");
    }
    // Degenerate inputs.
    {
        OwnerTable t{};
        initial_split(t, 0, 2);
        check(t.count == 0, "no cats yet: nothing decided");
    }
    {
        OwnerTable t{};
        initial_split(t, 4, 0);
        check(t.count == 4 && owner_of(t, 3) == 0, "peers=0 is treated as 1");
    }

    // --- extend_round_robin: goes to whoever has fewest, ties to the host ----
    {
        OwnerTable t{};
        initial_split(t, 2, 2);           // {0,1} -- one each
        extend_round_robin(t, 3, 2);      // a recruit: tied 1/1, host wins the tie
        check(owner_of(t, 2) == 0, "a tied recruit goes to the host");
        extend_round_robin(t, 4, 2);      // now 2/1: the client is behind
        check(owner_of(t, 3) == 1, "the next recruit goes to whoever is behind");
    }
    // Existing slots are NEVER rewritten by further growth -- F2's actual point.
    {
        OwnerTable t{};
        initial_split(t, 3, 2);
        uint8_t before[3] = { owner_of(t, 0), owner_of(t, 1), owner_of(t, 2) };
        for (uint32_t total = 4; total <= 10; ++total) grow(t, total, 2);
        check(owner_of(t, 0) == before[0] && owner_of(t, 1) == before[1] &&
              owner_of(t, 2) == before[2],
              "growing the table never changes an already-decided slot");
    }

    // --- grow(): the client joining after solo play does not reshuffle -------
    //
    // grow() takes `peers` on trust -- it has no way to tell "nobody has
    // connected yet" from "a real 2-player session" by itself, which is why
    // mgmp_ownership.cpp (the caller) gates on net_peer_count() >= 2 before
    // ever calling this on an empty table. What IS this module's job: once
    // the initial split has fired for whatever peer count was current at the
    // time, a later change in peer count must never touch it.
    {
        OwnerTable t{};
        grow(t, 2, 2);                  // the client just joined; 2 cats exist
        check(t.count == 2 && owner_of(t, 0) == 0 && owner_of(t, 1) == 1,
              "the initial split is the fair 1/1 split over the peers present "
              "at the moment it fires");
        uint8_t before0 = owner_of(t, 0), before1 = owner_of(t, 1);
        grow(t, 5, 2);                  // three more recruits
        check(owner_of(t, 0) == before0 && owner_of(t, 1) == before1,
              "recruits after the initial split do not touch it");
    }

    // --- THE PROPERTY: growth is monotonic and never rewrites a decided slot -
    //
    // For every plausible sequence of (peers, growing totals), every slot's
    // owner, once assigned, is identical no matter how much further the table
    // grows afterward -- checked against every larger total in the same run.
    for (uint32_t peers = 2; peers <= 4; ++peers) {
        for (uint32_t first = 1; first <= 6; ++first) {
            OwnerTable t{};
            grow(t, first, peers);   // the initial split
            uint8_t snapshot[kMaxOwnerSlots];
            memcpy(snapshot, t.owner, sizeof(snapshot));
            uint32_t snapshot_count = t.count;

            for (uint32_t total = first + 1; total <= first + 10; ++total) {
                grow(t, total, peers);
                for (uint32_t i = 0; i < snapshot_count; ++i) {
                    if (t.owner[i] == snapshot[i]) continue;
                    printf("FAIL: peers=%u first=%u total=%u -- slot %u changed "
                           "from %u to %u\n",
                           peers, first, total, i, snapshot[i], t.owner[i]);
                    ++failures;
                }
                // Fairness at every step: nobody is more than one ahead.
                uint32_t tally[kMaxOwnerPeers] = {};
                for (uint32_t i = 0; i < t.count; ++i) ++tally[t.owner[i]];
                uint32_t lo = 0xFFFFFFFFu, hi = 0;
                for (uint32_t p = 0; p < peers; ++p) {
                    if (tally[p] < lo) lo = tally[p];
                    if (tally[p] > hi) hi = tally[p];
                }
                if (hi - lo > 1) {
                    printf("FAIL: peers=%u total=%u -- shares differ by %u\n",
                           peers, total, hi - lo);
                    ++failures;
                }
            }
        }
    }

    if (failures) { printf("test_ownertable: FAILED -- %d failure(s)\n", failures); return 1; }
    printf("test_ownertable: PASSED -- 0 failure(s)\n");
    return 0;
}
