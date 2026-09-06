// mgmp_ownership -- see mgmp_ownership.h for the design, mgmp_ownertable.h
// for the growth rules this wires up.
#include "mgmp_ownership.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_ownertable.h"
#include "mgmp_config.h"
#include "mgmp_lockstep.h"   // lockstep_in_battle
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"

#include <windows.h>
#include <cstring>
#include <cstdlib>

namespace mgmp {
namespace {

typedef void* (__fastcall* fn_cat_by_id)(void* registry, uint64_t id);

struct State {
    uintptr_t     base     = 0;
    bool          resolved = false;
    const void**  director_slot = nullptr;

    bool on        = false;
    bool is_client = false;
    bool announced = false;
    bool declined_in_battle = false;
    bool held_in_battle     = false;

    OwnerTable table{};                 // this peer's live copy
    uint32_t   sent_count = 0xFFFFFFFFu; // host: table.count as of the last publish

    // A table that arrived while this peer must not apply it yet. Whole
    // state, not a delta -- see OwnershipMsg -- so a newer one just replaces
    // whatever was already held.
    bool       pend_valid = false;
    OwnerTable pend_table{};

    uint32_t pushed = 0, applied = 0, skipped = 0, deferred = 0;
};

State g;

// Same shape as mgmp_catsync's run_cats(), but this module never resolves a
// CatData* -- it only needs the COUNT, so it never touches the registry.
bool run_cat_count(uint32_t& out) {
    if (!g.director_slot) return false;
    const void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return false;
    uint32_t count = 0;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdCount, &count, sizeof(count)))
        return false;
    // Same implausibility guard catsync's run_cats() uses: a run has a few
    // dozen cats at most, and a wild count means the offset moved.
    if (count > 4096) return false;
    out = count;
    return true;
}

void ensure_state() {
    const Config& c = config();
    bool host   = _stricmp(c.net_role, "host")   == 0;
    bool client = _stricmp(c.net_role, "client") == 0;
    g.is_client = client;
    g.on = (host || client) && g.resolved;

    static bool said = false;
    if (!g.on && (host || client) && !said) {
        said = true;
        log_line("OWNERSHIP", "!! disabled: the MewDirector* global did not resolve");
    }
}

bool table_equal(const OwnerTable& a, const OwnerTable& b) {
    if (a.count != b.count) return false;
    return memcmp(a.owner, b.owner, a.count) == 0;
}

bool safe_by_id(fn_cat_by_id fn, void* registry, uint64_t id, void** out) {
    __try { *out = fn(registry, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void ownership_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;
    g.director_slot = (const void**)addr_of_data(D_MewDirectorPtr);
    if (!g.director_slot) {
        log_line("OWNERSHIP", "!! the MewDirector* global did not resolve -- "
                              "ownership sync is OFF");
        return;
    }
    g.resolved = true;
}

void ownership_init() {
    ensure_state();
    if (!g.on || g.announced) return;
    g.announced = true;
    log_line_lvl(LogLevel::Trace, "OWNERSHIP", "armed -- %s",
             g.is_client ? "this peer's cat ownership will be overwritten with the host's"
                         : "this peer's cat ownership table will be published to the client");
}

void ownership_shutdown() {
    if (!g.announced) return;
    log_line_lvl(LogLevel::Trace, "OWNERSHIP",
             "done: %u pushed, %u applied, %u unchanged, %u held%s",
             g.pushed, g.applied, g.skipped, g.deferred,
             g.pend_valid ? ", ONE STILL HELD at exit" : "");
}

// See the header: this table has no per-peer dedupe cache to drop, only a
// last-sent comparison to invalidate so the next publish is unconditional.
void ownership_forget() {
    ensure_state();
    if (!g.on || g.is_client) return;
    g.sent_count = 0xFFFFFFFFu;
}

void ownership_publish(const char* why) {
    ensure_state();
    if (!g.on || g.is_client || !net_active()) return;

    uint32_t total = 0;
    if (!run_cat_count(total)) return;   // no adventure loaded yet -- ordinary, silent

    if (g.table.count == 0) {
        // Coop has not genuinely started until a second player is actually in
        // the session -- see the header. A solo host recruiting cats must not
        // have them all locked to itself the moment this first computes.
        uint32_t peers = net_peer_count();
        if (peers >= 2) initial_split(g.table, total, peers);
    } else if (total > g.table.count) {
        extend_round_robin(g.table, total, net_peer_count());
    }

    if (g.table.count == 0) return;                    // nothing decided yet
    if (g.table.count == g.sent_count) { ++g.skipped; return; }

    OwnershipMsg m{};
    m.count = g.table.count;
    memcpy(m.owner, g.table.owner, m.count);
    if (net_send_ownership(m)) {
        g.sent_count = g.table.count;
        ++g.pushed;
        log_line("OWNERSHIP", "-> %u slot(s) decided (%s)", m.count, why);
    }
}

// --- applying, and holding until it is safe to apply ------------------------

static void apply_now(const OwnerTable& t, const char* when) {
    // Never shrinks and never rewrites what is already held -- see
    // OwnershipMsg. A count that went backwards would mean a stale or
    // out-of-order message, which TCP does not produce; refusing it is a
    // belt-and-suspenders check, not an expected path.
    if (t.count < g.table.count) {
        log_line("OWNERSHIP", "!! received a table with FEWER slots (%u) than "
                              "this peer already has (%u) -- ignored",
                 t.count, g.table.count);
        return;
    }
    if (table_equal(t, g.table)) { ++g.skipped; return; }
    g.table = t;
    ++g.applied;
    log_line("OWNERSHIP", "<- adopted %u slot(s) (%s)", t.count, when);
}

void ownership_apply_pending(const char* why) {
    ensure_state();
    if (!g.on || !g.pend_valid) return;
    apply_now(g.pend_table, why);
    g.pend_valid = false;
    g.held_in_battle = false;   // re-arms the line for the next battle
}

void ownership_on_message(const OwnershipMsg& m) {
    ensure_state();
    if (!g.on) return;
    if (!g.is_client) {
        log_line("OWNERSHIP", "!! received an ownership table from the peer while "
                              "hosting -- ignored (both peers configured as host?)");
        return;
    }
    if (m.count > kMaxOwnerSlots) return;

    OwnerTable incoming{};
    incoming.count = m.count;
    memcpy(incoming.owner, m.owner, m.count);

    // NOT WHILE THIS PEER IS IN A BATTLE -- the same rule and the same reason
    // as catsync/runhist. The table only ever grows and a grown slot never
    // affects a battle already in progress (its roster and split were fixed
    // at the first turn boundary), so holding here is a belt-and-suspenders
    // match to the rest of this codebase's "never write shared state mid-
    // battle" rule rather than a fix for an observed bug.
    if (!lockstep_in_battle()) {
        g.declined_in_battle = false;
    } else if (g.is_client && config().net_follow) {
        g.pend_table = incoming;
        g.pend_valid = true;
        ++g.deferred;
        if (!g.held_in_battle) {
            g.held_in_battle = true;
            log_line("OWNERSHIP", "holding an ownership update while a battle is in "
                                  "progress -- it will apply on the map, just before "
                                  "this peer enters the next node");
        }
        return;
    } else {
        if (!g.declined_in_battle) {
            g.declined_in_battle = true;
            log_line("OWNERSHIP", "declining an ownership update while a battle is in "
                                  "progress -- net_follow is off, so nothing would "
                                  "ever apply a held one.");
        }
        return;
    }

    apply_now(incoming, "on arrival");
}

uint8_t ownership_owner_of(uint32_t slot) {
    return owner_of(g.table, slot);
}

uint8_t ownership_owner_of_cat(uint64_t cat_id) {
    if (!g.director_slot || !cat_id) return kNoOwner;
    const void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return kNoOwner;

    uint32_t count = 0;
    const uint64_t* ids = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdCount, &count, sizeof(count)))
        return kNoOwner;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdData, &ids, sizeof(ids)))
        return kNoOwner;
    if (!ids || count == 0 || count > 4096) return kNoOwner;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t id = 0;
        if (!mem_read(&ids[i], &id, sizeof(id))) continue;
        if (id == cat_id) return owner_of(g.table, i);
    }
    return kNoOwner;   // not a cat in this run's own roster
}

uint8_t ownership_owner_of_catdata(const void* cat_data) {
    if (!g.director_slot || !cat_data) return kNoOwner;

    fn_cat_by_id by_id = (fn_cat_by_id)addr_of_call(C_CatDataById);
    if (!by_id) return kNoOwner;

    const void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return kNoOwner;

    const void* registry = nullptr;
    uint32_t    count = 0;
    const uint64_t* ids = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_CatRegistry, &registry, sizeof(registry)))
        return kNoOwner;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdCount, &count, sizeof(count)))
        return kNoOwner;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdData, &ids, sizeof(ids)))
        return kNoOwner;
    if (!registry || !ids || count == 0 || count > 4096) return kNoOwner;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t id = 0;
        if (!mem_read(&ids[i], &id, sizeof(id)) || !id) continue;
        void* p = nullptr;
        if (safe_by_id(by_id, (void*)registry, id, &p) && p == cat_data)
            return owner_of(g.table, i);
    }
    return kNoOwner;   // not a cat in this run's own roster
}

} // namespace mgmp
