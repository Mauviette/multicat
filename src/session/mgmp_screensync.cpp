// mgmp_screensync.cpp -- see mgmp_screensync.h for the design.
#include "mgmp_screensync.h"

#include "mgmp_addresses.h"   // kBtn_Name, C_ButtonClick
#include "mgmp_resolve.h"
#include "mgmp_mainmenulock.h"
#include "mgmp_follow.h"      // follow_current_node/follow_node_info -- treasure-node check
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"

#include <cstring>

namespace mgmp {
namespace {

typedef void (__fastcall* fn_button_click)(void* self, bool force);

constexpr uint8_t kNumKinds = 3;   // kVoteEvent, kVoteShopExit, kVoteChestOpen

const char* kBtnName_EventExit  = "Event_ExitButton";
const char* kBtnName_ShopExit   = "Shop_ExitButton";
const char* kBtnName_ChestOpen  = "ChestButton_alley";

const char* kind_name(uint8_t k) {
    switch (k) {
        case kVoteEvent:     return kBtnName_EventExit;
        case kVoteShopExit:  return kBtnName_ShopExit;
        default:             return kBtnName_ChestOpen;
    }
}

// True while the run is standing in a treasure node RIGHT NOW -- the only
// way to tell a chest's Shop_ExitButton apart from a real shop's, since the
// button itself carries no such distinction (same name, same class). False
// (never force-fire) if the current node cannot be read at all -- refusing
// to guess and falling back to the safer "wait for everyone" vote.
constexpr uint32_t kNodeType_Treasure = 13;
bool in_treasure_node() {
    uint32_t idx = 0;
    if (!follow_current_node(idx)) return false;
    uint32_t type = 0;
    uint64_t seed0 = 0;
    const char* type_name = nullptr;
    if (!follow_node_info(idx, type, seed0, &type_name)) return false;
    return type == kNodeType_Treasure;
}

// How many votes `k` needs before maybe_fire actually presses the button.
// ChestButton_alley never waits (a chest has no reason for one player to
// browse while the other stands there); Shop_ExitButton only skips waiting
// when the node it's exiting is a treasure node -- see in_treasure_node().
// Event_ExitButton and a real shop's Shop_ExitButton are unchanged: the
// original F3.1 "everyone votes" barrier.
uint8_t threshold_for(uint8_t k) {
    if (k == kVoteChestOpen) return 1;
    if (k == kVoteShopExit && in_treasure_node()) return 1;
    return net_peer_count();
}

struct KindState {
    void*   armed      = nullptr;   // live pointer to the current button instance
    uint8_t voted_mask = 0;         // bit i = peer i has voted for THIS instance
    bool    fired      = false;     // the real click has already been fired for it
};

struct State {
    uintptr_t base = 0;
    KindState kind[kNumKinds];
} g;

// Resets a kind's vote state when the tracked button's own pointer changes --
// see mgmp_screensync.h for why a pointer change (a new Button object) is
// what stands in for a screen-instance token here.
void ensure_armed(uint8_t k, void* self) {
    KindState& s = g.kind[k];
    if (s.armed == self) return;
    s.armed      = self;
    s.voted_mask = 0;
    s.fired      = false;
    log_line("SCREENSYNC", "armed '%s' @ %p for the exit vote", kind_name(k), self);
}

int8_t kind_for_name(const char* name) {
    if (strcmp(name, kBtnName_EventExit) == 0) return (int8_t)kVoteEvent;
    if (strcmp(name, kBtnName_ShopExit)  == 0) return (int8_t)kVoteShopExit;
    if (strcmp(name, kBtnName_ChestOpen) == 0) return (int8_t)kVoteChestOpen;
    return -1;
}

uint8_t popcount8(uint8_t v) {
    uint8_t n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

// Fires the real click, on THIS peer's own copy of the tracked button, once
// enough votes are in -- every peer does this independently, which is what
// actually walks each peer's own screen forward. Reuses mainmenulock's
// injected-click guard: Button::Click is hooked exactly once (by
// mainmenulock), and any simulated call into it must pass through the same
// `injecting` flag or it re-enters every click handler on this same call,
// screensync's own included.
void maybe_fire(uint8_t k) {
    KindState& s = g.kind[k];
    if (s.fired || !s.armed) return;

    // required == 0 only for the "wait for everyone" kinds (event, or a
    // REAL shop's exit) with no session yet -- keep waiting, same as the
    // original behaviour. kVoteChestOpen and a TREASURE node's
    // Shop_ExitButton always require exactly 1, so they fire on the
    // clicking peer's own vote alone, session or not -- see threshold_for.
    const uint8_t required = threshold_for(k);
    if (required == 0) return;
    if (popcount8(s.voted_mask) < required) return;

    s.fired = true;
    const uintptr_t addr = addr_of_call(C_ButtonClick);
    if (!addr) {
        log_line("SCREENSYNC", "!! %s did not resolve -- cannot fire the "
                                "agreed screen exit", kCalls[C_ButtonClick].name);
        return;
    }
    log_line("SCREENSYNC", "vote complete (%u/%u) -- firing the real click on '%s'",
             (unsigned)required, (unsigned)required, kind_name(k));

    mainmenulock_begin_injected_click();
    ((fn_button_click)addr)(s.armed, true);
    mainmenulock_end_injected_click();
}

} // namespace

void screensync_set_base(uintptr_t base) {
    g.base = base;
    for (auto& k : g.kind) k = KindState{};
}

void screensync_on_button_update(void* self) {
    if (!tune::kScreenSync || !self) return;

    char name[64] = {};
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name))) return;
    if (name[0] == '\0') return;

    const int8_t k = kind_for_name(name);
    if (k < 0) return;

    ensure_armed((uint8_t)k, self);
    // A peer vote may have arrived before this screen was even armed on this
    // peer -- check every frame, not just at click/message time, so that
    // ordering does not leave a satisfied vote unfired.
    maybe_fire((uint8_t)k);
}

bool screensync_on_button_click(void* self) {
    if (!tune::kScreenSync || !self) return false;

    char name[64] = {};
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name))) return false;
    if (name[0] == '\0') return false;

    const int8_t ki = kind_for_name(name);
    if (ki < 0) return false;   // not a vote-gated button -- let it through
    const uint8_t k = (uint8_t)ki;

    ensure_armed(k, self);
    KindState& s = g.kind[k];

    if (s.fired) return true;   // this screen is already on its way out

    const uint8_t self_peer = net_self();
    const uint8_t self_bit  = (self_peer < 8) ? (uint8_t)(1u << self_peer) : 0;
    if (s.voted_mask & self_bit) {
        return true;   // already voted for this instance -- still waiting, swallow
    }

    s.voted_mask |= self_bit;
    ScreenVoteMsg m{};
    m.kind = k;
    net_send_screenvote(m);

    log_line("SCREENSYNC", "'%s' voted (%u/%u so far) -- waiting for the other player(s)",
             name, (unsigned)popcount8(s.voted_mask), (unsigned)threshold_for(k));

    maybe_fire(k);
    return true;   // swallowed either way -- the real click, if any, already fired above
}

void screensync_on_message(uint8_t from, const ScreenVoteMsg& m) {
    if (!tune::kScreenSync || m.kind >= kNumKinds || from >= 8) return;
    KindState& s = g.kind[m.kind];
    const uint8_t bit = (uint8_t)(1u << from);
    if (s.voted_mask & bit) return;   // duplicate, ignore
    s.voted_mask |= bit;
    log_line("SCREENSYNC", "peer %u voted for '%s' (%u/%u so far)",
             (unsigned)from, kind_name(m.kind),
             (unsigned)popcount8(s.voted_mask), (unsigned)threshold_for(m.kind));
    maybe_fire(m.kind);
}

bool screensync_hud_pending(uint8_t& count, uint8_t& total) {
    if (!tune::kScreenSync) return false;
    for (auto& s : g.kind) {
        if (!s.armed || s.fired || s.voted_mask == 0) continue;
        count = popcount8(s.voted_mask);
        total = threshold_for((uint8_t)(&s - g.kind));
        return true;
    }
    return false;
}

} // namespace mgmp
