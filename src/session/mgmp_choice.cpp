// mgmp_choice -- see mgmp_choice.h for why replicating the CHOICE replaces
// replicating everything the choice does.
#include "mgmp_choice.h"
#include "mgmp_follow.h"
#include "mgmp_ownership.h"
#include "mgmp_ownertable.h"   // kNoOwner
#include "mgmp_catsync.h"      // catsync_id_for -- the F3.1 recipient-id fix
#include "mgmp_shopmirror.h"   // shopmirror_levelup_redirect_pending -- softlock fix
#include "mgmp_net.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// --- the two option arrays -------------------------------------------------
//
// Both are {T* begin, T* end} with sizeof(T) == 240, and both are built in
// authored file order. Read off setupActionChoice__inner_0 (v20 = *(v1+28),
// v161 = *(v1+29), then "v20 += 240") and off the level-up button callback,
// which resolves *(LevelUpScreen+864) + 240 * index.
constexpr uintptr_t kEvt_OptBegin  = 224;
constexpr uintptr_t kEvt_OptEnd    = 232;

constexpr uintptr_t kLvl_OptBegin  = 864;
constexpr uintptr_t kLvl_OptEnd    = 872;
constexpr uintptr_t kLvl_Committed = 792;   // select_option's "already chose" guard
// The cat this level-up is FOR -- CLAUDE.md's "LevelUpScreen+160 is the
// subject CatData*", the same field the level-up roller's own seed
// (splitmix64(CatData+0x00)) is read from. F6 keys the ownership check on it.
constexpr uintptr_t kLvl_Subject  = 160;

// The cat a WorldEvent is ABOUT -- CLAUDE.md only ever confirmed this as the
// target of ONE command (self_status_next_fight), not as a general subject
// field. Promoted to general use here on live evidence gathered 2026-09-05
// (mgmp_diag_worldevent.cpp, since deleted): two UNRELATED event archetypes
// in one session ('RottenMeat', a bad-food/stat effect, and 'LockedDoor', a
// skill-check) both had this exact offset resolve to a real cat in the run's
// roster, identically on host and client, with different owners for the two
// events -- not a coincidence a single command's own local field would
// produce. Treated the same way kLvl_Subject is: falls back to the host if
// unresolved, exactly like level_screen_decider.
constexpr uintptr_t kEvt_Subject  = 296;

constexpr uintptr_t kOptStride     = 240;

// An event option entry's stat key -- "str".."lck", "coins", "quest", "none",
// "home". A std::string at entry+64, compared against exactly those literals
// all the way through setupActionChoice.
constexpr uintptr_t kEvtOpt_Stat = 64;
// A LevelUpOption's type (1 stats, 2/3 pools, 4/7 ability, 5, 6 item) and its
// authored name, both read straight off select_option's own switch.
constexpr uintptr_t kLvlOpt_Type = 0;
constexpr uintptr_t kLvlOpt_Name = 200;

// The shipped UI pads out to 4 buttons per screen. This bound only has to be
// loose enough not to reject a legitimate list and tight enough to refuse a
// wild pointer.
constexpr uint32_t kMaxOptions = 64;

using fn_cap = void (__fastcall*)(void*);

struct State {
    bool on        = false;
    bool is_client = false;

    // Resolved from the module base, prologue-checked, exactly like catsync's
    // call targets: a bad address here would run an unrelated function with our
    // arguments on the game's stack.
    uintptr_t base     = 0;
    bool      resolved = false;
    fn_cap    evt_commit = nullptr;   // sub_140937F30
    fn_cap    lvl_click  = nullptr;   // sub_140386810

    void* world_event  = nullptr;   // latest WorldEvent*,    from its update
    void* level_screen = nullptr;   // latest LevelUpScreen*, from its update
    // GetTickCount64 of the last choice_on_event_update call -- the event
    // half of the same freshness check level_screen_tick_at does below, for
    // the same reason (world_event is only cleared at the next node).
    uint64_t world_event_tick_at = 0;
    // GetTickCount64 of the last choice_on_level_update call -- i.e. the last
    // frame LevelUpScreen::update genuinely ticked. `level_screen` itself is
    // only cleared at the NEXT node transition (choice_on_node_entered), so a
    // closed screen would otherwise leave a stale-but-non-null pointer behind
    // for as long as the player stays on the same node -- exactly the shape
    // of bug CLAUDE.md's rule 4 already named once for lockstep_in_battle().
    // choice_level_screen_owner() checks THIS instead of just "is the pointer
    // non-null" so the ownership banner/badge disappears within one recency
    // window of the screen actually closing, not at the next node.
    uint64_t level_screen_tick_at = 0;

    // "It's your cat!" banner refinements requested by the user, 2026-09-06:
    // suppress it entirely for an event with no real choice (a single-option
    // "continue" event has nothing for either player to decide), and hide it
    // the instant a choice IS made rather than waiting for the screen to
    // actually close. Both reset whenever `world_event` changes to a new
    // pointer (see choice_on_event_update) -- a stale true from the PREVIOUS
    // event must not leak onto a new one.
    bool event_has_choice = false;
    bool event_committed  = false;

    // Same request, the level-up half: a 1s delay before "It's your cat!"/
    // "Partner's cat!" appears on a FRESH LevelUpScreen, so it does not flash
    // up during whatever transition animation opens the screen. Set once,
    // the first tick a given level_screen pointer is seen -- reset only when
    // the pointer changes to a genuinely new screen.
    uint64_t level_screen_first_seen_at = 0;

    // A CHOICE can arrive before this peer's screen exists -- the client is
    // following the host into the node and may not have opened it yet. Hold it
    // rather than dropping it, the way mgmp_follow holds a node.
    bool     pending[2]          = {false, false};
    // The node the host was standing in when it made the choice, and the node
    // this peer is standing in now. A held choice may only be applied when the
    // two agree -- the single check that stops a choice outliving its node.
    uint64_t pending_seed[2]     = {0, 0};
    uint64_t here_seed           = 0;

    // NODES THIS PEER WILL NEVER ENTER, so a choice that arrives for one after
    // the fact can be refused instead of held forever.
    //
    // The order is not fixed: mgmp_follow can discard a node before the host
    // has even made its choice there, in which case the CHOICE arrives second
    // and there is nothing held for choice_on_node_skipped to drop. Without
    // this ring that message would sit in the pending slot indefinitely and
    // block the next real one.
    //
    // Same device, and the same 16 entries, as the retired-battle_id ring in
    // mgmp_battleid.h -- and for the same reason: an id that falls off the end
    // degrades to "unknown, hold it", never to a wrong apply.
    static constexpr uint32_t kSkippedRing = 16;
    uint64_t skipped[kSkippedRing] = {};
    uint32_t skipped_next          = 0;
    uint32_t pending_index[2]    = {0, 0};
    uint32_t pending_count[2]    = {0, 0};
    uint32_t pending_aux[2]      = {0, 0};
    char     pending_name[2][48] = {};
    bool     pending_warned[2]   = {false, false};

    // Set while we drive the game's own commit path, so the hook that exists to
    // SWALLOW a click does not swallow the click we just injected. Not
    // thread-local: every path that touches it is the game thread.
    bool injecting = false;

    uint32_t sent      = 0;
    uint32_t applied   = 0;
    uint32_t swallowed = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

const char* kind_name(uint8_t k) { return k == kChoiceLevelUp ? "level-up" : "event"; }

// Both called with the Guard held.
void remember_skipped(uint64_t seed) {
    if (!seed) return;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return;          // already known, keep it once
    g.skipped[g.skipped_next] = seed;
    g.skipped_next = (g.skipped_next + 1) % State::kSkippedRing;
}

bool was_skipped(uint64_t seed) {
    if (!seed) return false;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return true;
    return false;
}

// --- reading a std::string out of the game ---------------------------------
//
// MSVC's std::string is {union{char buf[16]; char* ptr;}, size, capacity}, so
// the characters are inline when capacity <= 15 and behind a pointer otherwise.
// Copy at most cap-1 and always terminate: a garbled length must not walk.
bool read_game_string(const void* str, char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!str) return false;
    uint64_t len = 0, capacity = 0;
    if (!mem_read((const uint8_t*)str + 16, &len, sizeof(len))) return false;
    if (!mem_read((const uint8_t*)str + 24, &capacity, sizeof(capacity))) return false;
    if (len > 4096) return false;                  // not a string we recognise
    const uint8_t* chars = (const uint8_t*)str;
    if (capacity > 15) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)str, &p, sizeof(p)) || !p) return false;
        chars = (const uint8_t*)p;
    }
    uint32_t n = (uint32_t)len;
    if (n > cap - 1) n = cap - 1;
    if (n && !mem_read(chars, out, n)) { out[0] = '\0'; return false; }
    out[n] = '\0';
    return true;
}

// --- the option arrays -----------------------------------------------------

bool read_options(const void* screen, uintptr_t begin_off, uintptr_t end_off,
                  uint8_t*& begin, uint32_t& count) {
    begin = nullptr; count = 0;
    if (!screen) return false;
    uint8_t* b = nullptr; uint8_t* e = nullptr;
    if (!mem_read((const uint8_t*)screen + begin_off, &b, sizeof(b))) return false;
    if (!mem_read((const uint8_t*)screen + end_off,   &e, sizeof(e))) return false;
    if (!b || e < b) return false;
    const uintptr_t bytes = (uintptr_t)(e - b);
    if (bytes % kOptStride) return false;           // not the array we think it is
    const uintptr_t n = bytes / kOptStride;
    if (n == 0 || n > kMaxOptions) return false;
    begin = b; count = (uint32_t)n;
    return true;
}

void option_name(uint8_t kind, const uint8_t* entry, char* out, uint32_t cap) {
    const uintptr_t off = (kind == kChoiceLevelUp) ? kLvlOpt_Name : kEvtOpt_Stat;
    if (!read_game_string(entry + off, out, cap)) snprintf(out, cap, "?");
}

uint32_t option_type(const uint8_t* entry) {
    uint32_t t = 0;
    mem_read(entry + kLvlOpt_Type, &t, sizeof(t));
    return t;
}

// --- F6: who decides THIS level-up screen -----------------------------------
//
// Not g.is_client any more -- see mgmp_choice.h's header note on why the old
// role-based split (host always decides, client always swallows) never let a
// client act on its own cats at all. The decider is now whoever owns the
// screen's subject cat (LevelUpScreen+160), falling back to the host when
// ownership is not resolved yet: F6 must never DEADLOCK a screen neither peer
// will click (see the "armed client that cannot inject" note below), and the
// host is the one peer guaranteed to have an opinion.
uint8_t level_screen_decider(void* screen) {
    // Host-only purchase (mgmp_shopmirror.h): never mirrored, so the client
    // has no corresponding screen to defer to at all -- ownership of the
    // randomly-rolled recipient must not apply here, or a recipient that
    // happens to belong to the client would swallow the host's own (only)
    // click on their own (only) screen.
    if (shopmirror_is_host_only_screen(screen)) return kHostPeer;

    void* cat_data = nullptr;
    if (!screen || !mem_read((const uint8_t*)screen + kLvl_Subject, &cat_data, sizeof(cat_data))
        || !cat_data)
        return kHostPeer;

    // By POINTER, not by dereferencing CatData+0 for an id: that field held a
    // heap pointer, not the small sequential id, when measured live on
    // 2026-09-04 -- CLAUDE.md's "CatData+0x00 is a u64 id" turned out to
    // describe the SERIALIZED stream's first field, not the in-memory
    // object's. ownership_owner_of_catdata sidesteps the question entirely by
    // resolving the run's own ids through the game's by_id and comparing
    // pointers, so it does not matter where the real id field lives.
    const uint8_t owner = ownership_owner_of_catdata(cat_data);
    return owner == kNoOwner ? kHostPeer : owner;
}

// --- F6-events: who decides THIS world event --------------------------------
//
// Mirrors level_screen_decider exactly, now that WorldEvent+296 (kEvt_Subject)
// is trusted as a general subject field rather than one command's own local
// variable -- see kEvt_Subject's own header note for the live evidence.
// Same host fallback, same reason: an armed client that cannot inject a
// choice must never be left swallowing clicks on a screen NOBODY can commit.
uint8_t event_screen_decider(void* world_event) {
    void* cat_data = nullptr;
    if (!world_event || !mem_read((const uint8_t*)world_event + kEvt_Subject, &cat_data, sizeof(cat_data))
        || !cat_data)
        return kHostPeer;
    const uint8_t owner = ownership_owner_of_catdata(cat_data);
    return owner == kNoOwner ? kHostPeer : owner;
}

bool event_screen_is_mine(void* world_event) {
    return event_screen_decider(world_event) == net_self();
}

bool level_screen_is_mine(void* screen) {
    // F3.1 softlock fix, 2026-09-06: while a shop-purchase level-up
    // recipient correction is still in flight (mgmp_shopmirror.h), the
    // subject this screen currently shows may not be the one it will settle
    // on. Reported live: BOTH peers can independently conclude "not my
    // cat" for two DIFFERENT wrong-in-transit subjects and wait on each
    // other forever. Nobody decides until the correction (or its timeout)
    // resolves -- costs at most that wait, never a stuck run.
    if (shopmirror_levelup_redirect_pending()) return false;
    return level_screen_decider(screen) == net_self();
}

// --- injecting -------------------------------------------------------------
//
// Both are the game's own commit path with our index substituted, rather than a
// reimplementation of what the commit does. That matters: sub_140937F30 also
// sets MewDirector+1812 and calls sub_14091AA00, and the level-up callback also
// copy-constructs the LevelUpOption through sub_14037BBB0. Neither is something
// worth reproducing by hand.

bool inject_event(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.world_event, kEvt_OptBegin, kEvt_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    // sub_140937F30 reads only a1+8 and a1+16 and never touches the vftable
    // slot, so a bare three-qword block is a complete capture for it.
    void* cap[3] = { nullptr, g.world_event, begin + (uintptr_t)index * kOptStride };
    if (!g.evt_commit) return false;
    // Calling the spliced address re-enters our own hook, which sees `injecting`
    // and answers "run the original" -- so the game's commit runs exactly once
    // and we do not need the trampoline exposed outside mgmp_hooks.cpp.
    g.injecting = true;
    g.evt_commit(cap);
    g.injecting = false;
    return true;
}

bool inject_level(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.level_screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    // sub_140386810's capture is {vftable, LevelUpScreen*, int index}; it reads
    // the index as a 32-bit signed at +16 and does the +240*index itself.
    struct Cap { void* vt; void* screen; int32_t index; int32_t pad; };
    Cap cap = { nullptr, g.level_screen, (int32_t)index, 0 };
    if (!g.lvl_click) return false;
    g.injecting = true;
    g.lvl_click(&cap);
    g.injecting = false;
    return true;
}

// Shared by both apply ticks.
void apply_pending(uint8_t kind, void* screen) {
    if (!g.pending[kind]) return;

    // F3.1 softlock fix: a level-up choice can arrive (the buyer's own,
    // always-correct screen decided already) before THIS peer's own
    // shop-purchase recipient correction has landed. Applying it now would
    // land on whatever WRONG cat this screen currently shows. Hold, same
    // shape as the node-seed hold below -- it becomes due the instant the
    // correction resolves (mgmp_shopmirror.h's redirect tick runs every
    // frame regardless of this).
    if (kind == kChoiceLevelUp && shopmirror_levelup_redirect_pending()) {
        if (!g.pending_warned[kind]) {
            g.pending_warned[kind] = true;
            log_line("CHOICE", "holding the host's level-up choice %u -- a shop-purchase"
                               " recipient correction is still in flight on this peer",
                     g.pending_index[kind]);
        }
        return;
    }

    // NODE IDENTITY FIRST, BEFORE THE COUNT AND BEFORE THE NAME.
    //
    // The count and the name describe a screen; this describes WHICH NODE the
    // screen belongs to, and a choice about another node is not a choice about
    // this one however well its shape happens to match. Checking it last would
    // be checking it never: on 2026-08-25 a stale choice passed the count check
    // (both events offered 2 options) and the name check reported the
    // disagreement and obeyed it anyway.
    //
    // Both seeds must be known for this to mean anything. A zero on either side
    // is "not stated" -- a peer that has not entered a node this session, or a
    // pre-19 sender -- and unknown must not read as mismatched.
    //
    // A MISMATCH HOLDS, IT DOES NOT DROP, and the difference is the whole
    // correctness of the common case. The host publishes its choice the instant
    // it clicks, which is routinely BEFORE this peer has entered that node at
    // all -- the client is still finishing the previous one. At that moment the
    // seeds legitimately disagree and the choice is simply not due yet.
    // Dropping here would throw away every choice that arrives early, which is
    // most of them.
    //
    // Deciding a choice is stale needs the one fact this function does not
    // have: that the run has MOVED PAST the node it was made on. That is
    // choice_on_node_skipped's, and it is where the drop lives.
    if (g.pending_seed[kind] && g.here_seed &&
        g.pending_seed[kind] != g.here_seed) {
        if (!g.pending_warned[kind]) {
            g.pending_warned[kind] = true;
            log_line("CHOICE", "holding the host's %s choice %u -- it was made on node"
                               " %016llx and this peer is still in %016llx",
                     kind_name(kind), g.pending_index[kind],
                     (unsigned long long)g.pending_seed[kind],
                     (unsigned long long)g.here_seed);
        }
        return;
    }

    const uintptr_t bo = (kind == kChoiceLevelUp) ? kLvl_OptBegin : kEvt_OptBegin;
    const uintptr_t eo = (kind == kChoiceLevelUp) ? kLvl_OptEnd   : kEvt_OptEnd;

    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, bo, eo, begin, count)) {
        // The screen exists but its options are not built yet. Normal for a
        // frame or two; only worth a line if it never resolves.
        if (!g.pending_warned[kind]) {
            g.pending_warned[kind] = true;
            log_line("CHOICE", "holding the host's %s choice %u -- this peer's option"
                               " list is not built yet",
                     kind_name(kind), g.pending_index[kind]);
        }
        return;
    }

    const uint32_t want  = g.pending_index[kind];
    const uint32_t their = g.pending_count[kind];

    // Count first: if the two lists are different lengths the index means
    // nothing, and saying so beats picking the wrong option.
    if (their && their != count) {
        log_line("CHOICE", "!! host offered %u %s option(s), this peer built %u --"
                           " refusing to choose; the two screens are not the same screen",
                 their, kind_name(kind), count);
        g.pending[kind] = false;
        return;
    }
    if (want >= count) {
        log_line("CHOICE", "!! host chose %s option %u but this peer has only %u --"
                           " refusing", kind_name(kind), want, count);
        g.pending[kind] = false;
        return;
    }

    // Then the name. This is the cross-check that catches "same length,
    // different content", which is the failure an index cannot survive. It is
    // reported and then obeyed: the host DID press that button, and stalling
    // here would be worse than proceeding loudly.
    char mine[48] = {};
    option_name(kind, begin + (uintptr_t)want * kOptStride, mine, sizeof(mine));
    if (g.pending_name[kind][0] && strcmp(mine, g.pending_name[kind]) != 0)
        log_line("CHOICE", "!! %s option %u is '%s' here but '%s' on the host --"
                           " taking it anyway, the lists are the same length",
                 kind_name(kind), want, mine, g.pending_name[kind]);

    if (kind == kChoiceLevelUp) {
        const uint32_t mine_type = option_type(begin + (uintptr_t)want * kOptStride);
        if (g.pending_aux[kind] && mine_type != g.pending_aux[kind])
            log_line("CHOICE", "!! level-up option %u is type %u here but %u on the"
                               " host -- taking it anyway",
                     want, mine_type, g.pending_aux[kind]);
    }

    const bool ok = (kind == kChoiceLevelUp) ? inject_level(want) : inject_event(want);
    g.pending[kind] = false;
    g.pending_warned[kind] = false;
    if (ok) {
        ++g.applied;
        if (kind == kChoiceEvent) g.event_committed = true;   // same banner reasoning
        log_line("CHOICE", "applied the host's %s choice: option %u/%u ('%s')",
                 kind_name(kind), want, count, mine);
    } else {
        log_line("CHOICE", "!! failed to apply the host's %s choice %u",
                 kind_name(kind), want);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void choice_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;
    g.evt_commit = nullptr;
    g.lvl_click  = nullptr;

    // An armed client that cannot inject is worse than a desync: it swallows
    // clicks it has no way to replace and the screen is permanently dead with
    // nothing to time out. So both call targets must resolve or the whole
    // feature disarms.
    const uintptr_t evt = addr_of(T_EventChoice);
    const uintptr_t lvl = addr_of_call(C_LevelUpClick);
    if (!evt || !lvl) {
        log_line("CHOICE", "!! %s did not resolve by signature"
                           " -- choice replication is OFF",
                 !evt ? "WorldEvent option commit" : "LevelUpScreen button click");
        return;
    }
    g.evt_commit = (fn_cap)evt;
    g.lvl_click  = (fn_cap)lvl;
    g.resolved = true;
}

void choice_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on          = tune::kChoice;
    g.is_client   = (net_role() == NetRole::Client);
    g.world_event = nullptr;
    g.level_screen = nullptr;
    g.pending[0] = g.pending[1] = false;
    g.injecting = false;
    if (!g.on) { log_line("CHOICE", "choice replication disabled by net_choice = 0"); return; }
    if (!g.resolved) {
        // Refusing is the safe direction, and it applies to BOTH roles now:
        // ownership can put either peer on either side of a screen for both
        // level-up and events, so either peer may need to INJECT the other's
        // choice, not just the client. An armed peer SWALLOWS every local
        // click it does not own, so one that cannot inject the decider's
        // choice would not desync -- it would sit on a dead screen forever
        // with no way to press anything.
        g.on = false;
        log_line("CHOICE", "!! call targets unresolved -- choice replication is OFF."
                           " This peer keeps its own event and level-up clicks; expect"
                           " the two runs to diverge on the first event.");
        return;
    }
    log_line_lvl(LogLevel::Trace, "CHOICE", "armed -- event and level-up choices both"
             " follow F2's per-cat ownership; whoever owns the subject cat decides,"
             " the other peer's local click on that screen is suppressed");
}

void choice_shutdown() {
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "CHOICE",
             "done: %u sent, %u applied, %u local click(s) suppressed",
             g.sent, g.applied, g.swallowed);
    g.on = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

// How long a level-up or event screen is trusted to still be up after its
// last real tick. Generous compared to a frame, tight enough that closing
// the screen clears the banner within a fraction of a second rather than at
// the next node -- same idea and same order of magnitude as mgmp_cursor's
// kBoardStaleMs, which fixed the equivalent bug for the peer cursor.
constexpr uint64_t kScreenStaleMs = 250;

// How long to wait, after a FRESH LevelUpScreen first appears, before
// "It's your cat!"/"Partner's cat!" is allowed to show on it -- requested by
// the user, 2026-09-06, so the banner does not flash up during whatever
// transition animation opens the screen. Event screens are NOT delayed this
// way (not asked for, and events already gate on event_has_choice/
// event_committed instead).
constexpr uint64_t kLevelBannerDelayMs = 1000;

// F4: see mgmp_choice.h for why this is deliberately NOT level_screen_decider
// -- that one's host fallback would mislabel an undecided cat as the host's.
//
// Checks RECENCY, not just non-null: `level_screen` is only cleared at the
// next node transition, so a closed screen would otherwise read as owned for
// as long as the player stays on that node -- see level_screen_tick_at's own
// note in State.
uint8_t choice_level_screen_owner() {
    if (!g.on || !g.level_screen) return kNoOwner;
    Guard guard;
    if (GetTickCount64() - g.level_screen_tick_at > kScreenStaleMs) return kNoOwner;
    if (GetTickCount64() - g.level_screen_first_seen_at < kLevelBannerDelayMs) return kNoOwner;
    void* cat_data = nullptr;
    if (!mem_read((const uint8_t*)g.level_screen + kLvl_Subject, &cat_data, sizeof(cat_data))
        || !cat_data)
        return kNoOwner;
    return ownership_owner_of_catdata(cat_data);
}

// F3.1 shop-purchase-recipient fix -- see mgmp_choice.h's header note.
void* choice_current_level_screen() {
    if (!g.on || !g.level_screen) return nullptr;
    Guard guard;
    if (GetTickCount64() - g.level_screen_tick_at > kScreenStaleMs) return nullptr;
    return g.level_screen;
}

bool choice_screen_subject_raw_id(void* screen, uint64_t& out_id) {
    out_id = 0;
    if (!screen) return false;
    void* cat_data = nullptr;
    if (!mem_read((const uint8_t*)screen + kLvl_Subject, &cat_data, sizeof(cat_data)) || !cat_data)
        return false;
    // NOT a raw dereference of cat_data -- see catsync_id_for's own header
    // note. A first version of this function did exactly that
    // (*(uint64_t*)cat_data) and it is WRONG: that offset is not a stable
    // per-cat id (level_screen_decider's comment a few lines up already
    // documents this, found live 2026-09-04), and mgmp_shopmirror's
    // level-up-recipient fix was confirmed live, 2026-09-06, to desync again
    // for exactly this reason. catsync_id_for resolves the same portable id
    // catsync_publish itself sends, by pointer match against the run's own
    // id array -- the only correct source.
    return catsync_id_for(cat_data, out_id);
}

bool choice_force_level_screen_subject(void* screen, void* cat_data) {
    if (!screen || !cat_data) return false;
    return mem_write((uint8_t*)screen + kLvl_Subject, &cat_data, sizeof(cat_data));
}

// F4-events: the event half of choice_level_screen_owner, same recency check
// and same reasoning -- see that function's own note.
uint8_t choice_event_screen_owner() {
    if (!g.on || !g.world_event) return kNoOwner;
    Guard guard;
    if (GetTickCount64() - g.world_event_tick_at > kScreenStaleMs) return kNoOwner;
    // A single-option event has nothing for either player to decide -- no
    // banner needed. A choice already made should stop claiming the screen
    // immediately rather than lingering until it closes.
    if (!g.event_has_choice || g.event_committed) return kNoOwner;
    void* cat_data = nullptr;
    if (!mem_read((const uint8_t*)g.world_event + kEvt_Subject, &cat_data, sizeof(cat_data))
        || !cat_data)
        return kNoOwner;
    return ownership_owner_of_catdata(cat_data);
}

// --- the world event -------------------------------------------------------

bool choice_on_event_commit(void* cap) {
    if (!g.on || !cap) return true;
    if (g.injecting) return true;           // this is our own injected click

    Guard guard;

    void* we    = nullptr;
    void* entry = nullptr;
    if (!mem_read((const uint8_t*)cap + 8,  &we,    sizeof(we)))    return true;
    if (!mem_read((const uint8_t*)cap + 16, &entry, sizeof(entry))) return true;
    g.world_event = we;

    uint8_t* begin = nullptr; uint32_t count = 0;
    const bool have = read_options(we, kEvt_OptBegin, kEvt_OptEnd, begin, count);

    if (!event_screen_is_mine(we)) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed a local event click -- this event's cat belongs to"
                           " another player (this peer had %u option(s))", have ? count : 0);
        return false;                        // do NOT run the original
    }

    // Owner: name the index, publish it, then let the click through.
    if (!have) {
        log_line("CHOICE", "!! could not read this event's option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }
    const uintptr_t delta = (uintptr_t)((uint8_t*)entry - begin);
    if (delta % kOptStride || delta / kOptStride >= count) {
        log_line("CHOICE", "!! the chosen option is not an element of this event's array"
                           " -- not publishing");
        return true;
    }

    ChoiceMsg m{};
    m.kind  = kChoiceEvent;
    m.index = (uint32_t)(delta / kOptStride);
    m.count = count;
    m.node_seed = follow_here_seed();
    option_name(kChoiceEvent, (const uint8_t*)entry, m.name, sizeof(m.name));
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> event option %u/%u ('%s')", m.index, m.count, m.name);
    }
    g.event_committed = true;   // hide the ownership banner now, not at screen close
    return true;
}

void choice_on_event_update(void* world_event) {
    if (!g.on || !world_event) return;
    Guard guard;
    if (g.world_event != world_event) {
        // A NEW event screen -- reset the per-screen banner state before
        // anything else touches it below.
        g.world_event     = world_event;
        g.event_committed = false;
        uint8_t* begin = nullptr; uint32_t count = 0;
        g.event_has_choice = read_options(world_event, kEvt_OptBegin, kEvt_OptEnd, begin, count)
                           && count > 1;
    }
    g.world_event_tick_at = GetTickCount64();
    // Mirror of choice_on_level_update: whoever does NOT own this event's cat
    // is the one waiting for a choice to apply.
    if (!event_screen_is_mine(world_event)) apply_pending(kChoiceEvent, world_event);
}

// --- the level-up screen ---------------------------------------------------

bool choice_on_level_select(void* screen, void* opt) {
    if (!g.on || !screen || !opt) return true;
    if (g.injecting) return true;

    Guard guard;
    g.level_screen = screen;

    if (!level_screen_is_mine(screen)) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed a local level-up click -- this cat belongs to"
                           " another player");
        return false;
    }

    // The option arrives as a COPY (the button callback copy-constructs it
    // through sub_14037BBB0), so the index cannot come from pointer arithmetic
    // the way the event's does. Match on content instead: type plus authored
    // name, which is the same identity the wire carries.
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) {
        log_line("CHOICE", "!! could not read the level-up option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }

    char want_name[48] = {};
    option_name(kChoiceLevelUp, (const uint8_t*)opt, want_name, sizeof(want_name));
    const uint32_t want_type = option_type((const uint8_t*)opt);

    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = begin + (uintptr_t)i * kOptStride;
        if (option_type(e) != want_type) continue;
        char nm[48] = {};
        option_name(kChoiceLevelUp, e, nm, sizeof(nm));
        if (strcmp(nm, want_name) == 0) { found = i; break; }
    }
    if (found == UINT32_MAX) {
        log_line("CHOICE", "!! the chosen level-up option (type %u '%s') matches none of"
                           " the %u on offer -- not publishing",
                 want_type, want_name, count);
        return true;
    }

    // Host-only purchase (mgmp_shopmirror.h): the client has no matching
    // screen to apply a ChoiceMsg to at all -- publishing one here would
    // just sit forever as a held choice with nothing to apply it to (or
    // worse, get misapplied to some LATER, unrelated level-up screen once
    // one finally appears). The result reaches the client through the
    // ordinary cat-state sync instead, published PROMPTLY here rather than
    // waiting for the next node boundary, since the effect is already fully
    // resolved the instant this click lands.
    if (shopmirror_is_host_only_screen(screen)) {
        log_line("CHOICE", "host-only level-up option %u/%u (type %u '%s') applied locally --"
                           " publishing this cat's state instead of a choice",
                 found, count, want_type, want_name);
        // NOT catsync_publish() here directly -- the ORIGINAL click (which
        // actually applies the stat change) has not run yet at this point
        // (this hook returns true and the caller runs it afterward), so
        // publishing now would push STALE, pre-choice data. Same class of
        // bug mgmp_invlock's own debounce exists for; this defers to
        // shopmirror's identical debounce-then-publish tick instead of
        // duplicating it here.
        shopmirror_notify_host_only_choice_committed();
        return true;
    }

    ChoiceMsg m{};
    m.kind  = kChoiceLevelUp;
    m.index = found;
    m.count = count;
    m.aux   = want_type;
    m.node_seed = follow_here_seed();
    memcpy(m.name, want_name, sizeof(m.name));
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> level-up option %u/%u (type %u '%s')",
                 m.index, m.count, m.aux, m.name);
    }
    return true;
}

void choice_on_level_update(void* level_screen) {
    if (!g.on || !level_screen) return;
    Guard guard;
    if (g.level_screen != level_screen) {
        g.level_screen = level_screen;
        g.level_screen_first_seen_at = GetTickCount64();
    }
    g.level_screen_tick_at = GetTickCount64();

    // Whoever does NOT own this cat is the one waiting for a choice to apply
    // -- the mirror of the check in choice_on_level_select, now keyed on the
    // same per-cat ownership rather than on which peer is the client.
    if (level_screen_is_mine(level_screen)) return;

    // select_option no-ops once LevelUpScreen+792 is set, so injecting into a
    // screen that already committed would be swallowed by the GAME rather than
    // by us -- silently. Drop the held choice instead, and say so.
    uint8_t committed = 0;
    if (mem_read((const uint8_t*)level_screen + kLvl_Committed, &committed, 1) && committed) {
        if (g.pending[kChoiceLevelUp]) {
            log_line("CHOICE", "!! a level-up choice arrived for a screen that has already"
                               " committed -- dropping it");
            g.pending[kChoiceLevelUp] = false;
        }
        return;
    }
    apply_pending(kChoiceLevelUp, level_screen);
}

// --- receiving -------------------------------------------------------------

void choice_on_message(const ChoiceMsg& m) {
    if (!g.on) return;
    Guard guard;

    const uint8_t kind = (m.kind == kChoiceLevelUp) ? kChoiceLevelUp : kChoiceEvent;

    // BOTH KINDS ARE OWNERSHIP-BASED NOW (events joined level-up 2026-09-05,
    // on the live evidence behind kEvt_Subject), so either peer may
    // legitimately receive either kind: it means the OTHER peer owns the cat
    // the screen is about. There is no more a fixed "client" side to this
    // message for either kind.

    // A choice for a node mgmp_follow already gave up on. Refusing it here is
    // the other half of choice_on_node_skipped: that one catches the choice
    // that was already held when the node was discarded, this one catches the
    // choice that arrives afterwards. Holding it would block the next real
    // choice and eventually land on the wrong screen.
    if (was_skipped(m.node_seed)) {
        log_line("CHOICE", "!! a %s choice arrived for node %016llx, which this peer"
                           " passed over without entering -- refusing it. The host"
                           " resolved that node and this peer did not.",
                 kind_name(kind), (unsigned long long)m.node_seed);
        return;
    }

    if (g.pending[kind])
        log_line("CHOICE", "!! a %s choice was still held when another arrived --"
                           " the older one (option %u) is dropped",
                 kind_name(kind), g.pending_index[kind]);

    g.pending[kind]        = true;
    g.pending_seed[kind]   = m.node_seed;
    g.pending_index[kind]  = m.index;
    g.pending_count[kind]  = m.count;
    g.pending_aux[kind]    = m.aux;
    g.pending_warned[kind] = false;
    memcpy(g.pending_name[kind], m.name, sizeof(g.pending_name[kind]));
    g.pending_name[kind][sizeof(g.pending_name[kind]) - 1] = '\0';

    log_line("CHOICE", "<- host chose %s option %u/%u ('%s') on node %016llx",
             kind_name(kind), m.index, m.count, g.pending_name[kind],
             (unsigned long long)m.node_seed);

    // Apply immediately if the screen is already up; otherwise the update tick
    // takes it. Doing it here too is what makes the common case land in the
    // same frame the message arrived.
    void* screen = (kind == kChoiceLevelUp) ? g.level_screen : g.world_event;
    if (screen) apply_pending(kind, screen);
}

void choice_on_node_entered(uint64_t node_seed) {
    if (!g.on) return;
    Guard guard;
    g.here_seed = node_seed;

    // The cached screens belong to the node just left. Clearing them is not
    // tidiness: choice_on_message applies straight into whichever pointer is
    // held, so a stale one is both a wrong screen and a possible dead object.
    // The update ticks re-cache a live pointer on their very next frame.
    g.world_event  = nullptr;
    g.level_screen = nullptr;

    // A choice already waiting for THIS node becomes due the moment we arrive.
    // Re-arm the "holding" line so the next one is reported afresh rather than
    // suppressed by a warning about the node we just left.
    for (uint8_t kind = 0; kind < 2; ++kind)
        if (g.pending[kind] && g.pending_seed[kind] == node_seed)
            g.pending_warned[kind] = false;
}

void choice_on_node_skipped(uint64_t node_seed) {
    if (!g.on || !node_seed) return;
    Guard guard;
    remember_skipped(node_seed);

    for (uint8_t kind = 0; kind < 2; ++kind) {
        if (!g.pending[kind] || g.pending_seed[kind] != node_seed) continue;
        log_line("CHOICE", "!! node %016llx was passed over without this peer"
                           " entering it, and a %s choice for it (option %u '%s') was"
                           " still held -- DROPPING it rather than letting it surface"
                           " on a later node. The host resolved that node and this"
                           " peer did not: the two runs have diverged.",
                 (unsigned long long)node_seed, kind_name(kind),
                 g.pending_index[kind], g.pending_name[kind]);
        g.pending[kind]        = false;
        g.pending_warned[kind] = false;
    }
}

} // namespace mgmp
