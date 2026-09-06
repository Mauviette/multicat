// mgmp_follow.cpp -- see mgmp_follow.h.

#include "mgmp_catsync.h"
#include "mgmp_choice.h"
#include "mgmp_invsync.h"
#include "mgmp_nodehash.h"
#include "mgmp_runhist.h"
#include "mgmp_ownership.h"
#include "mgmp_cursor.h"     // F4.1: cursor_on_map_tick
#include "mgmp_follow.h"
#include "mgmp_lockstep.h"
#include "mgmp_leave.h"      // leave_consume_republish_due
#include "mgmp_savefile.h"   // savefile_republish
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

// MapScreen's node vector, read off MapNode::Click and cross-checked against
// every other MapScreen function that walks it (update, generate_map,
// generate_auto_connections, generate_bonus_nodes, load_map, select_boss_level,
// TickNemesis all use the same +124/+128 pair). Standard CustomVector shape:
// { u32 capacity @ 0x78, u32 count @ 0x7C, MapNode** data @ 0x80 }.
constexpr uintptr_t kMap_NodeCount = 124;
constexpr uintptr_t kMap_NodeData  = 128;

// MapNode+0x118: the node's own 32-byte xoshiro256 seed, written by
// MapScreen::generate_map and copied into TLS+0x178 by EnterNode.
// MapNode+0x138: MapNodeType, from MapNode::str_to_type.
constexpr uintptr_t kNode_Seed = 0x118;
constexpr uintptr_t kNode_Type = 0x138;

// MapScreen+0xA0: the map marker (see CLAUDE.md's "Where the run is
// standing" section). +0x50 is where the marker CURRENTLY IS (the walk
// animation updates this as it moves); +0x60 is what was last SELECTED
// (glaiel::MapNode::Click's only durable write, confirmed by static
// disassembly -- see tune::kFollowMarkerWalk's header note). Both are
// validated against the node vector before being trusted anywhere they are
// READ; select_node_for_walk below is the one place this module WRITES one.
constexpr uintptr_t kMap_Marker      = 0xA0;
constexpr uintptr_t kMarker_AtNode   = 0x50;
constexpr uintptr_t kMarker_Selected = 0x60;

// MapNode::str_to_type, in enum order. 1 and 18 are three-character names whose
// string constants IDA did not render; they are not battle types and nothing
// here depends on knowing them.
const char* node_type_name(uint32_t t) {
    static const char* kNames[] = {
        "none", "?1", "enter", "exit", "home", "battle", "hard", "miniboss",
        "boss", "event", "optional_event", "special_event", "shop", "treasure",
        "furniturebox", "foodbox", "bonus", "empty", "?18"
    };
    return t < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[t] : "?";
}

struct State {
    bool     on             = false;
    bool     is_client      = false;

    void*    map            = nullptr;   // latest MapScreen*, from its update

    // NODES THE HOST HAS ENTERED AND THIS PEER HAS NOT -- A QUEUE, NOT A SLOT.
    //
    // It was one slot, and a second ENTERNODE overwrote it in silence. That is
    // not a race to tolerate: it is the client SKIPPING A WHOLE NODE of the
    // run, and it happened live on 2026-08-25. The host entered node 6 (an
    // event), resolved it and walked on to node 17 faster than this peer's map
    // tick came round; node 17 replaced node 6 in the slot; the client never
    // entered node 6, never built its option list, and never applied the event.
    // Its log says `<- host entered node 6` with no `following host into node
    // 6` after it, and there was no other line anywhere to say a node had been
    // dropped.
    //
    // The run was already wrong at that point. What made it unrecoverable was
    // that node 6's CHOICE stayed held and landed on node 3 two nodes later --
    // see ChoiceMsg::node_seed.
    //
    // A queue is the whole fix, because the peers do not have to be on the same
    // node at the same moment -- only to walk the same nodes in the same order.
    // The battle layer already assumes exactly that: the join barrier exists so
    // a peer can arrive at a fight late. Trailing the host by a node is the same
    // situation one screen earlier.
    static constexpr uint32_t kMaxPendingNodes = 8;
    struct PendingNode {
        uint32_t index = 0;
        uint32_t type  = 0;
        uint32_t count = 0;
        uint64_t seed  = 0;
        uint64_t at    = 0;         // tick the host's choice arrived
        bool     held  = false;     // ...and whether we have said we are holding
    };
    PendingNode pending_q[kMaxPendingNodes];
    uint32_t    pending_head  = 0;
    uint32_t    pending_count = 0;

    // The node this peer is standing in, remembered so a peer that connects
    // afterwards can be told where the run is. ENTERNODE used to be published
    // once, at the moment of entry, and nothing kept it -- so a reconnecting
    // peer loaded the save, landed on the map and waited forever for a message
    // that had already been sent. Measured 2026-08-26.
    bool     have_here      = false;
    uint32_t here_index     = 0;

    // Last tick MapScreen::update ran. See follow_on_map: this is what tells
    // the save flush whether the run is between nodes or inside one.
    uint64_t map_tick       = 0;

    // A jump asked for by the debug panel, applied at the tail of the next
    // MapScreen::update -- the same site the follow path enters nodes from.
    bool     jump_pending   = false;
    uint32_t jump_index     = 0;
    uint32_t here_count     = 0;
    uint32_t here_type      = 0;
    uint64_t here_seed      = 0;

    uint32_t entered        = 0;         // nodes followed, for the shutdown line
    uint32_t published      = 0;
    uint32_t suppressed     = 0;
    bool     warned_no_map  = false;

    // See follow_on_button_update: pressing "CloseButton" for the client when
    // the host has already moved on to a node this peer has not entered.
    void (*button_click)(void*, bool) = nullptr;
    uint32_t closed_for_follow = 0;      // how many times this has fired, for shutdown

    // EXPERIMENTAL, F3.2 -- see tune::kFollowMarkerWalk and
    // select_node_for_walk/marker_at_node. Armed once select_node_for_walk
    // succeeds for the node currently at the front of pending_q; cleared once
    // marker_at_node confirms arrival, or after kWalkTimeoutMs.
    bool     walk_pending    = false;
    uint64_t walk_started_at = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

// --- the pending-node queue -------------------------------------------------
//
// All three are called with the Guard held. `front` is only valid while
// `pending_any()` is true.

bool pending_any() { return g.pending_count != 0; }

State::PendingNode& pending_front() {
    return g.pending_q[g.pending_head];
}

void pending_pop() {
    if (!g.pending_count) return;
    g.pending_head = (g.pending_head + 1) % State::kMaxPendingNodes;
    --g.pending_count;
}

void pending_clear() { g.pending_head = 0; g.pending_count = 0; }

bool read_nodes(void* map, uint32_t& count, void*& data) {
    count = 0; data = nullptr;
    if (!map) return false;
    if (!mem_read((const uint8_t*)map + kMap_NodeCount, &count, sizeof(count))) return false;
    if (!mem_read((const uint8_t*)map + kMap_NodeData,  &data,  sizeof(data)))  return false;
    // A map with no nodes, or an implausible count, means we are reading the
    // wrong object -- say nothing rather than index into it.
    return data != nullptr && count > 0 && count < 4096;
}

void* node_at(void* map, uint32_t index) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data) || index >= count) return nullptr;
    void* node = nullptr;
    if (!mem_read((const uint8_t*)data + index * sizeof(void*), &node, sizeof(node))) return nullptr;
    return node;
}

bool index_of_node(void* map, void* node, uint32_t& out) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &p, sizeof(p))) return false;
        if (p == node) { out = i; return true; }
    }
    return false;
}

uint32_t node_type(void* node) {
    uint32_t t = 0;
    if (node) mem_read((const uint8_t*)node + kNode_Type, &t, sizeof(t));
    return t;
}

uint64_t node_seed0(void* node) {
    uint64_t s = 0;
    if (node) mem_read((const uint8_t*)node + kNode_Seed, &s, sizeof(s));
    return s;
}

bool read_marker(void* map, const void** out) {
    if (!map) return false;
    const void* marker = nullptr;
    if (!mem_read((const uint8_t*)map + kMap_Marker, &marker, sizeof(marker)) || !marker)
        return false;
    *out = marker;
    return true;
}

// EXPERIMENTAL, F3.2 -- see tune::kFollowMarkerWalk's header note for the
// full derivation. Writes the map marker's SELECTED slot directly,
// replicating the confirmed common-path tail of the REAL
// glaiel::MapNode::Click (RVA 0x228700, not the RVA once pinned here):
//
//     mov rax, [rsi + 0x170]   ; rsi = MapNode*, rax = its MapScreen*
//     mov rcx, [rax + 0xA0]    ; rcx = the map marker
//     mov [rcx + 0x60], rsi    ; marker->selected = this node
//
// Deliberately does NOT call the real function (its "this" is a UI wrapper
// whose +8 holds the MapNode*, not the MapNode itself -- calling it with
// this=node is exactly what crashed the client on 2026-09-05) and does NOT
// replicate the type==4 ("home") branch, which builds an extra confirmation
// popup before writing anything. Returns false for that type so the caller
// falls back to an immediate, unanimated entry -- same as before this existed.
bool select_node_for_walk(void* map_screen, void* node) {
    if (!map_screen || !node) return false;
    if (node_type(node) == 4) return false;   // "home" -- not replicated, see above
    const void* marker = nullptr;
    if (!read_marker(map_screen, &marker)) return false;
    return mem_write((uint8_t*)marker + kMarker_Selected, &node, sizeof(node));
}

// Has the marker's own walk animation actually reached `node` yet? Same
// +0x50 slot follow_marker_node (below) resolves to an index for the panel;
// this compares the raw pointer directly since the caller already holds the
// live MapNode*, and it is polled every tick rather than trusted once.
bool marker_at_node(void* map_screen, const void* node) {
    if (!map_screen || !node) return false;
    const void* marker = nullptr;
    if (!read_marker(map_screen, &marker)) return false;
    const void* at = nullptr;
    if (!mem_read((const uint8_t*)marker + kMarker_AtNode, &at, sizeof(at))) return false;
    return at == node;
}

} // namespace

// ---------------------------------------------------------------------------

void follow_set_base(uintptr_t base) {
    (void)base;   // no longer needed: select_node_for_walk writes memory
                  // directly rather than calling into a pinned RVA -- see
                  // tune::kFollowMarkerWalk's header note.
    g.button_click = nullptr;
    const uintptr_t click = addr_of_call(C_ButtonClick);
    if (!click) {
        log_line("FOLLOW", "!! %s did not resolve -- this peer's modal screens will not"
                           " auto-close when the host moves on", kCalls[C_ButtonClick].name);
    } else {
        g.button_click = (void(*)(void*, bool))click;
    }
}

void follow_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on        = config().net_follow;
    g.is_client = (net_role() == NetRole::Client);
    g.map       = nullptr;
    g.walk_pending = false;
    pending_clear();
    if (!g.on) { log_line("FOLLOW", "map following disabled by net_follow = 0"); return; }
    log_line_lvl(LogLevel::Trace, "FOLLOW", "armed -- %s",
             g.is_client ? "following the host's map choices; local map input is suppressed"
                         : "publishing this peer's map choices");
    if (config().net_follow_delay_ms && g.is_client)
        log_line("FOLLOW", "!! TEST SETTING ACTIVE: net_follow_delay_ms = %u. This peer"
                           " will lag every node by that long ON PURPOSE, to exercise"
                           " the join barrier. Set it to 0 for a real session.",
                 config().net_follow_delay_ms);
    else if (config().net_follow_delay_ms)
        log_line("FOLLOW", "net_follow_delay_ms = %u is set but this peer is the host,"
                           " which never follows -- it will have no effect",
                 config().net_follow_delay_ms);
}

void follow_shutdown() {
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "FOLLOW",
             "done: %u published, %u followed, %u local click(s) suppressed, %u modal"
             " screen(s) auto-closed", g.published, g.entered, g.suppressed,
             g.closed_for_follow);
    g.on = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

bool follow_suppresses_local_input() { return g.on && g.is_client; }

// Where this peer is now. Recorded on BOTH peers -- the host so it can tell a
// joiner, the client so its own log says where it thinks it is.
void remember_node(uint32_t index, uint32_t count, uint32_t type, uint64_t seed) {
    const bool moved = !g.have_here || g.here_seed != seed;
    g.have_here  = true;
    g.here_index = index;
    g.here_count = count;
    g.here_type  = type;
    g.here_seed  = seed;

    // Tell the choice layer where the run now is. It holds a choice that has
    // not found its screen yet, and a choice belongs to exactly one node -- see
    // ChoiceMsg::node_seed for the run this cost.
    //
    // The seed is PUSHED rather than pulled. mgmp_choice could ask
    // follow_here_seed() when it applies, but that would have it take this
    // module's lock while holding its own, and this call already runs the other
    // way round -- the classic inversion. Handing the value over means neither
    // module ever waits on the other.
    if (moved) choice_on_node_entered(seed);

    // The meta layer's own hash, sampled at the one moment both peers reach
    // identically: immediately before EnterNode. Deliberately taken on BOTH
    // sides from the same function, because a hash whose two halves are
    // computed at different points in the frame compares two different things
    // and reports a mismatch that is entirely ours.
    //
    // Note this is BEFORE the publishes below, which is what makes the two
    // comparable: the host describes the run it is about to push, and the
    // client -- which applied that push when it popped, ahead of its own map
    // tick -- describes the same bytes.
    if (moved) nodehash_on_node(seed, index);
}

bool follow_on_enter_node(void* map_screen, void* node, bool* sent) {
    if (sent) *sent = false;
    if (!g.on || !net_active() || !map_screen || !node) return true;

    Guard guard;
    g.map = map_screen;                    // the click proves the pointer is live

    uint32_t index = 0;
    bool     known = index_of_node(map_screen, node, index);
    uint32_t type  = node_type(node);

    // --- the client: swallow it ------------------------------------------
    //
    // The host owns the run, so a click here is a second opinion nobody asked
    // for -- and acting on it would put the two peers on different nodes, which
    // no amount of battle-layer lockstep can recover from. Swallowing rather
    // than disabling the UI keeps the change to one function.
    if (g.is_client) {
        ++g.suppressed;
        log_line("FOLLOW", "suppressed local map input: node %u (%s) -- the host"
                           " drives the run",
                 known ? index : 0xFFFFFFFFu, node_type_name(type));
        return false;
    }

    // --- the host: publish it --------------------------------------------
    if (!known) {
        // Nothing to send that the client could resolve. Do not block the host
        // over it: a run that continues single-player is better than one that
        // stops, and the client will notice it is on a different node the
        // moment a battle starts and the hashes disagree.
        log_line("FOLLOW", "!! entered a node that is not in MapScreen's vector"
                           " -- cannot publish it; the client will not follow");
        return true;
    }

    uint32_t count = 0; void* data = nullptr;
    read_nodes(map_screen, count, data);

    // Battle identity first of all, and before the publish: from here on this
    // peer's ACTION/HASH/CONTROL name this node, and the client establishes the
    // same id from the same field of the same node in follow_map_update.
    const uint64_t seed = node_seed0(node);
    lockstep_enter_battle(seed);

    // F5, 2026-09-05: the symmetric half of the client's apply-before-
    // remember_node below. The client is not the only one who can locally
    // equip something any more (F6 lets either peer touch their own cats), so
    // the host needs the same safe landing point for whatever the CLIENT sent
    // while the host was off doing something else -- mid-battle, most
    // pointedly, since catsync/invsync's defer_applies() now holds a push for
    // exactly this moment on the host side too. Same two reasons as the
    // client: this is off the battle screen, and it is still before this
    // peer's own publish two lines down, so that publish reflects the merged
    // state rather than stomping the client's just-applied change straight
    // back out.
    invsync_apply_pending("entering a node");
    catsync_apply_pending("entering a node");

    remember_node(index, count, type, seed);

    // Cats FIRST, node second. TCP is ordered and the client applies a CATDATA
    // the moment it pops, while it defers the node entry to the tail of
    // MapScreen::update -- so sending in this order is what guarantees the
    // battle is built from the host's cats rather than from the client's stale
    // ones. Reversing these two lines reintroduces the bug this closes.
    catsync_publish("entering a node");
    // ...and the other half of the same equip. Order between these two does not
    // matter the way it does against ENTERNODE -- an equip has already moved
    // the item out of the inventory and onto the cat by the time either is
    // serialized, so neither push depends on the other -- but both must land
    // BEFORE the node, and the client applies each the moment it pops.
    invsync_publish("entering a node");
    // ...and the used-event list, which is the third piece of run state and the
    // one nothing pushed until now. Same ordering requirement as the other two:
    // BEFORE the node, because it is what the next event will be rolled from.
    runhist_publish("entering a node");
    // F2's ownership table, same ordering requirement: BEFORE the node,
    // because a battle built from this node is the reason the table needs to
    // already be there -- see mgmp_ownership.h.
    ownership_publish("entering a node");

    EnterNodeMsg m{};
    m.index      = index;
    m.node_count = count;
    m.type       = type;
    m.seed0      = seed;
    if (net_send_enter_node(m)) {
        ++g.published;
        if (sent) *sent = true;
        log_line("FOLLOW", "-> node %u/%u (%s) seed0=%016llx",
                 index, count, node_type_name(type), (unsigned long long)m.seed0);
    }
    return true;
}

// A peer connected or reconnected: tell it which node the run is standing in.
//
// Without this the whole save/cats/inventory catch-up leaves the joiner on the
// map with nothing to do, because ENTERNODE is otherwise published exactly once
// -- at the instant of entry -- and a peer that was not connected then never
// hears about it. That was the "client loads the save and the battle never
// activates" report.
bool follow_on_map() {
    if (!g.map_tick) return false;
    // 250 ms: MapScreen::update does not tick on the frame a node is entered,
    // and a single missed frame is not a reason to refuse a flush. A quarter
    // second is far shorter than any node and far longer than any frame.
    return (GetTickCount64() - g.map_tick) < 250;
}

uint32_t follow_node_count() {
    if (!g.map) return 0;
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(g.map, count, data)) return 0;
    return count;
}

bool follow_node_info(uint32_t index, uint32_t& type, uint64_t& seed0,
                      const char** type_name) {
    if (!g.map) return false;
    void* node = node_at(g.map, index);
    if (!node) return false;
    type  = node_type(node);
    seed0 = node_seed0(node);
    if (type_name) *type_name = node_type_name(type);
    return true;
}

bool follow_current_node(uint32_t& index) {
    if (!g.have_here) return false;
    index = g.here_index;
    return true;
}

// Deliberately unlocked. It is one naturally-aligned u64 written by the game
// thread in remember_node and read by the game thread when the host stamps a
// CHOICE, so the lock would only buy a lock-ordering hazard against the module
// that wants the value. See remember_node for the other half of that argument.
uint64_t follow_here_seed() { return g.have_here ? g.here_seed : 0; }

// --- where the run is standing, read from the GAME rather than remembered ----
//
// follow_current_node above reports the node this MODULE watched somebody
// enter. That is the wrong answer for the case that matters most: a run
// reloaded from disk has entered nothing this session, so it reports nothing --
// and a reloaded run parked on an unresolved node is exactly when you need to
// be told which node that is.
//
// kMap_Marker/kMarker_AtNode/kMarker_Selected are declared near the top of
// this file, where select_node_for_walk and marker_at_node also use them.
// BOTH slots below are VALIDATED AGAINST THE NODE VECTOR RATHER THAN TRUSTED:
// index_of_node only reports an index when the pointer is genuinely an
// element of MapScreen+0x80, so a wrong offset yields "unknown" instead of a
// confident wrong number -- the same rule the per-turn state hash follows.
// That is what makes reading two undocumented slots an acceptable risk here.

bool read_marker_node(uintptr_t slot, uint32_t& index) {
    if (!g.map) return false;
    const void* marker = nullptr;
    if (!mem_read((const uint8_t*)g.map + kMap_Marker, &marker, sizeof(marker)) || !marker)
        return false;
    void* node = nullptr;
    if (!mem_read((const uint8_t*)marker + slot, &node, sizeof(node)) || !node)
        return false;
    return index_of_node(g.map, node, index);
}

bool follow_marker_node(uint32_t& index)   { return read_marker_node(kMarker_AtNode,   index); }
bool follow_selected_node(uint32_t& index) { return read_marker_node(kMarker_Selected, index); }

void follow_request_jump(uint32_t index) {
    g.jump_pending = true;
    g.jump_index   = index;
}

void follow_catchup(uint8_t peer) {
    if (!g.on || g.is_client || !net_active()) return;

    Guard guard;
    if (!g.have_here) {
        log_line("FOLLOW", "peer %u joined before this peer entered any node --"
                           " nothing to catch it up to; the next node is published"
                           " normally", (unsigned)peer);
        return;
    }

    EnterNodeMsg m{};
    m.index      = g.here_index;
    m.node_count = g.here_count;
    m.type       = g.here_type;
    m.seed0      = g.here_seed;
    if (!net_send_enter_node_to(peer, m)) return;

    log_line("FOLLOW", "-> re-sent the CURRENT node %u/%u (%s) seed0=%016llx to"
                       " peer %u (it joined after we entered it)",
             m.index, m.node_count, node_type_name(m.type),
             (unsigned long long)m.seed0, (unsigned)peer);
}

void follow_on_message(const EnterNodeMsg& m) {
    if (!g.on) return;
    Guard guard;
    if (!g.is_client) {
        // Symmetric message, asymmetric authority: a host that receives one is
        // talking to a peer that thinks it is the host too.
        log_line("FOLLOW", "!! received a node choice while hosting -- both peers"
                           " believe they own the run");
        return;
    }
    // A full queue means this peer is eight nodes behind, which no amount of
    // waiting is going to fix. Refuse the newest and SAY SO: the run is already
    // wrong, and the one thing that must not happen is what used to -- losing a
    // node without a line in the log. Dropping the newest rather than the
    // oldest keeps the queue contiguous, the same rule the peer-hash ring
    // follows for the same reason.
    if (g.pending_count >= State::kMaxPendingNodes) {
        log_line("FOLLOW", "!! %u nodes already queued and the host entered another"
                           " (node %u) -- REFUSING it. This peer is too far behind"
                           " to follow the run and the two are no longer the same"
                           " run.",
                 g.pending_count, m.index);
        choice_on_node_skipped(m.seed0);
        return;
    }

    State::PendingNode& p =
        g.pending_q[(g.pending_head + g.pending_count) % State::kMaxPendingNodes];
    ++g.pending_count;
    p.index = m.index;
    p.type  = m.type;
    p.count = m.node_count;
    p.seed  = m.seed0;
    // Timed from ARRIVAL, not from the first map update that sees it: the point
    // of the delay is to put a known gap between the host entering a node and
    // this peer entering it, and the map screen may not be ready for a while
    // either way.
    p.at    = GetTickCount64();
    p.held  = false;
    log_line("FOLLOW", "<- host entered node %u/%u (%s) seed0=%016llx%s",
             m.index, m.node_count, node_type_name(m.type),
             (unsigned long long)m.seed0,
             g.pending_count > 1 ? " -- QUEUED, this peer is still behind" : "");
}

void* follow_map_update(void* map_screen) {
    if (!g.on || !map_screen) return nullptr;

    Guard guard;
    g.map = map_screen;

    // The map is ticking, so the run is BETWEEN nodes. This is the stamp the
    // save flush reads -- see follow_on_map.
    g.map_tick = GetTickCount64();

    // THE MAP SCREEN ITSELF TICKING IS THE SIGNAL mgmp_leave WAITS FOR to
    // republish the save after this host resumed a run straight from the
    // house (no fresh save-selection click, so nothing else republishes it).
    // Deliberately not the earlier "back InRun" reading (class-chooser /
    // item-setup already reads InRun before the player has validated
    // anything) and deliberately not the first EnterNode either, once this
    // existed to try: MapScreen::update only ever ticks once the run has
    // genuinely been launched onto the map, so it fires as soon as the host
    // ARRIVES on the map -- before they have clicked into any node -- letting
    // a waiting client rejoin that much sooner. See
    // leave_consume_republish_due's comment for the full history. Host-only:
    // a client's own follow_map_update runs too, and must not republish
    // anything.
    if (!g.is_client && leave_consume_republish_due()) savefile_republish();
    // Sibling case, same tick: a save published while the host was in the
    // house (ready=0 -- see SaveFileMsg::ready) rather than resuming an
    // announced departure. Both can call savefile_republish on the same
    // tick; that is harmless, it only resets a publish latch.
    savefile_on_host_map_tick();

    // F5, 2026-09-05: drain any cat/inventory push held because a modal
    // screen (the inventory) was open -- see the new hold branch in
    // mgmp_catsync.cpp/mgmp_invsync.cpp's on_message. Called on EVERY tick
    // now, not only at node entry, so a hold clears the instant the screen
    // closes rather than waiting for the next node. Both are no-ops with
    // nothing held, which is the common case.
    invsync_apply_pending("the map resumed ticking");
    catsync_apply_pending("the map resumed ticking");

    // F4.1, 2026-09-05: the peer cursor's map-screen half -- see
    // cursor_on_map_tick's own comment for why this is the right (and only
    // available) place for it.
    cursor_on_map_tick();

    // --- a jump asked for by the debug panel --------------------------------
    //
    // Handled before the follow path and returning early, because the two are
    // different intentions and letting a jump fall through into the pending
    // follow would enter two nodes on one tick.
    if (g.jump_pending) {
        g.jump_pending = false;
        const uint32_t index = g.jump_index;

        uint32_t count = 0; void* data = nullptr;
        if (!read_nodes(map_screen, count, data)) return nullptr;   // not ready

        void* node = node_at(map_screen, index);
        if (!node) {
            log_line("FOLLOW", "!! panel asked to enter node %u but this map has"
                               " %u node(s) -- ignored", index, count);
            return nullptr;
        }

        // Loud, unconditionally, and on both peers' behalf: this is the debug
        // panel EDITING THE RUN. Everything else in this module reacts to a
        // decision a player made by clicking the map.
        log_line("FOLLOW", "!! PANEL JUMP: entering node %u (%s) seed0=%016llx by"
                           " request from the debug panel -- this is not a move the"
                           " player made on the map",
                 index, node_type_name(node_type(node)),
                 (unsigned long long)node_seed0(node));

        // Mirror h_EnterNode exactly: publish through the ordinary path first,
        // then let the caller run the original. Calling o_EnterNode alone would
        // bypass the detour and the client would never hear about it -- the same
        // property the follow path relies on in the other direction.
        bool sent = false;
        if (!follow_on_enter_node(map_screen, node, &sent)) return nullptr;
        return node;
    }

    if (!pending_any()) return nullptr;
    State::PendingNode& p = pending_front();

    // --- the deliberate late-join delay (test knob) -------------------------
    //
    // Holding the node here rather than dropping it is what makes this a test
    // of the join barrier and not just a broken client: the host walks into the
    // battle alone, this peer arrives net_follow_delay_ms later, and the barrier
    // is what has to have kept the host from playing turns in the meantime.
    // Everything downstream -- the seed check, the type check -- still runs when
    // the hold expires, so the delay changes WHEN we follow and nothing else.
    if (const uint32_t delay = config().net_follow_delay_ms) {
        const uint64_t waited = GetTickCount64() - p.at;
        if (waited < delay) {
            if (!p.held) {
                p.held = true;
                log_line("FOLLOW", "!! net_follow_delay_ms = %u -- deliberately holding"
                                   " node %u for %u ms before following. This is a TEST"
                                   " setting: it manufactures the late-join gap.",
                         delay, p.index, delay);
            }
            return nullptr;
        }
        if (p.held)
            log_line("FOLLOW", "delay elapsed (%llu ms) -- following now",
                     (unsigned long long)waited);
    }

    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map_screen, count, data)) return nullptr;   // not ready yet

    // Same map, or the index means nothing. Both peers generate the map from
    // the same save, so a differing count is a real problem and not a race.
    if (p.count && count != p.count) {
        log_line("FOLLOW", "!! host's map has %u nodes, ours has %u -- not"
                           " following; the two runs are not the same run",
                 p.count, count);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    void* node = node_at(map_screen, p.index);
    if (!node) {
        log_line("FOLLOW", "!! node %u is out of range on our map (%u nodes)",
                 p.index, count);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    // Identity check, and it is the strong one: the node's stored seed is the
    // 32 bytes EnterNode is about to load into the simulation stream, so if it
    // differs the battle would start from a different RNG state and desync on
    // its first roll. Catching it here names the cause; catching it at the
    // first turn hash would only say "they disagree".
    uint64_t seed = node_seed0(node);
    if (p.seed && seed != p.seed) {
        log_line("FOLLOW", "!! node %u seed is %016llx here but %016llx on the"
                           " host -- refusing to enter; the maps differ",
                 p.index, (unsigned long long)seed,
                 (unsigned long long)p.seed);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    uint32_t type = node_type(node);
    if (p.type != type)
        log_line("FOLLOW", "!! node %u is '%s' here but '%s' on the host --"
                           " entering anyway, the seed matched",
                 p.index, node_type_name(type), node_type_name(p.type));

    // EXPERIMENTAL, F3.2: let the marker visibly WALK to this node before
    // entering it, instead of teleporting. Node/seed/type are already
    // confirmed to match the host above, so this only ever delays entry into
    // a node we were already going to enter -- it never changes WHICH node.
    if (tune::kFollowMarkerWalk) {
        if (!g.walk_pending) {
            if (select_node_for_walk(map_screen, node)) {
                g.walk_pending    = true;
                g.walk_started_at = GetTickCount64();
                log_line("FOLLOW", "selected node %u for the marker to walk to"
                                   " before entering", p.index);
                return nullptr;   // wait for a later tick
            }
            // select failed (a 'home' node, or a read/write failure) -- fall
            // through and enter immediately, same as the non-walk path.
        } else {
            constexpr uint64_t kWalkTimeoutMs = 5000;   // safety net, not a real design constraint
            const uint64_t waited = GetTickCount64() - g.walk_started_at;
            if (marker_at_node(map_screen, node)) {
                log_line("FOLLOW", "marker arrived after %llu ms -- entering",
                         (unsigned long long)waited);
                g.walk_pending = false;
            } else if (waited > kWalkTimeoutMs) {
                log_line("FOLLOW", "!! marker did not arrive within %llu ms --"
                                   " entering anyway (this node will teleport)",
                         (unsigned long long)kWalkTimeoutMs);
                g.walk_pending = false;
            } else {
                return nullptr;   // still walking
            }
        }
    }

    const uint32_t entering = p.index;
    pending_pop();
    ++g.entered;
    log_line("FOLLOW", "following host into node %u (%s)%s",
             entering, node_type_name(type),
             pending_any() ? " -- more nodes still queued" : "");

    // Battle identity, established without negotiating anything: this is the
    // same node the host entered, so `seed` is the same 64 bits it read. The
    // host does the matching call in follow_on_enter_node. Note this is the
    // CLIENT's only chance to make it -- the follow path calls o_EnterNode, the
    // MinHook trampoline, which bypasses the h_EnterNode detour entirely.
    lockstep_enter_battle(seed);

    // Everything the host published for this node lands HERE, and it must land
    // BEFORE remember_node, which is where the meta hash is taken.
    //
    // Two separate reasons, and both were measured wrong on 2026-08-26.
    //
    // The apply point itself: this is the last moment before EnterNode, so it
    // is still in time for the battle or shop the node is about to open; and it
    // is inside MapScreen::update, so the inventory apply's clear-and-rebuild
    // cannot free Equipment out from under an inventory screen this peer had
    // open when the host clicked. See the note above invsync_apply_pending.
    //
    // The ORDER: the host takes its node hash after publishing, so the hash
    // describes the run it just pushed. With these calls below remember_node
    // the client hashed the run it had BEFORE the push -- one node stale --
    // and reported an inventory mismatch that was entirely ours. That is
    // exactly the "a hash whose two halves are computed at different points in
    // the frame compares two different things" failure remember_node's own
    // comment warns about.
    invsync_apply_pending("about to follow the host into a node");
    catsync_apply_pending("about to follow the host into a node");
    runhist_apply_pending("about to follow the host into a node");
    ownership_apply_pending("about to follow the host into a node");

    remember_node(entering, count, type, seed);

    // A shift-held EnterNode takes a completely different branch (the very
    // first thing it does is SDL_GetScancodeFromKey(SDLK_LSHIFT) and, if down,
    // tail-calls sub_1403923F0 instead). That is local keyboard state, so the
    // client would diverge from the host purely by holding a key. Cheap to
    // notice, so say so rather than let it be mysterious.
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        log_line("FOLLOW", "!! LEFT SHIFT is down -- EnterNode branches on it and"
                           " this peer will not take the host's path");

    // The F3.2 marker walk (tune::kFollowMarkerWalk) already ran above, before
    // `entering` was even assigned -- by the time execution reaches here the
    // marker has either visibly arrived at `node` or the feature is off/not
    // applicable, so there is nothing left to do but enter.
    return node;
}

// Cahier des charges: close any screen the client is looking at when the
// host has already moved on, so the client is never caught browsing its
// inventory on the map while the host is mid-battle -- the user's own
// example of the failure mode this closes.
//
// FROM h_ButtonUpdate, NOT from a MapScreen hook, and that is the whole
// design: MapScreen::update -- and therefore follow_map_update above --
// DOES NOT TICK while a modal screen like the inventory is open (confirmed
// live in an earlier session, see mgmp_invsync.h's F5 history). So the
// client cannot even LEARN it has a node to follow until whatever screen it
// has open closes on its own. Button::update, by contrast, fires for every
// button regardless of screen state -- the same property cursor_on_map_tick
// and the (reverted) auto-reopen experiment both relied on.
//
// CLOSE ONLY, NEVER REOPEN. A prior session tried closing AND reopening the
// inventory automatically; the close half worked every time, the reopen
// click had no confirmed effect and once left the screen stuck shut with no
// way back -- worse than the accepted "stale until you close it yourself"
// limitation it was trying to remove. See mgmp_hooks.cpp's h_ButtonUpdate
// for where that attempt is preserved in a comment. This only ever presses
// a button already proven to work, and it only presses it when there is
// somewhere for the player to end up (the map, about to follow a queued
// node) -- never leaves them stranded.
//
// NAME-ONLY, NOT SCREEN-SPECIFIC. kBtnName_Close ("CloseButton") was only
// ever confirmed on the inventory screen, but it reads as a generic
// component name rather than an inventory-specific one, so this does not
// special-case that screen -- it presses ANY button by that exact name,
// gated purely on "a follow is due". If another modal screen shares the
// name, this closes it too, for free; if it uses a different name, this
// simply does nothing there yet.
void follow_on_button_update(void* button) {
    if (!g.on || !g.is_client || !button || !g.button_click) return;
    if (!pending_any()) return;   // nothing queued -- nothing to close for

    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name))) return;
    if (strcmp(name, kBtnName_Close) != 0) return;

    ++g.closed_for_follow;
    log_line("FOLLOW", "the host has moved on and this peer is looking at a modal"
                       " screen ('%s' is up) -- closing it so the map can follow",
             name);
    g.button_click(button, false);
}

bool follow_on_button_click(void* self) {
    if (!g.on || !g.is_client || !self) return false;

    char name[64];
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name))) return false;
    if (strcmp(name, kBtnName_MapNode) != 0) return false;

    log_line("FOLLOW", "swallowed a client click on a map node -- the host owns node"
                       " selection, this peer only follows");
    return true;
}

} // namespace mgmp
