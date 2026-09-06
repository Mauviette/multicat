#include "mgmp_invsync.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_hooks.h"     // hooks_is_live, for the two blob hooks this needs
#include "mgmp_lockstep.h"  // lockstep_in_battle -- see invsync_on_message, F5
#include "mgmp_follow.h"    // follow_on_map -- see the new hold branch below
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// --- ByteStream, the fields the two hooks touch -----------------------------
//
// Same struct mgmp_catsync documents at length; only the offsets used here are
// repeated. The load-side write pattern is copied field for field off
// sub_14022C620's own success path, which is the best possible specification of
// what the game expects a freshly-retrieved stream to look like.
constexpr uintptr_t kBS_Mode     = 0x00;  // u32: 0 = read, 1 = write, 2 = file
constexpr uintptr_t kBS_WriteLen = 0x0C;  // u32
constexpr uintptr_t kBS_WriteBuf = 0x10;  // void*  (game heap)
constexpr uintptr_t kBS_ReadBuf  = 0x18;  // void*
constexpr uintptr_t kBS_ReadOwns = 0x20;  // u8 -- leave 0 on a BORROWED buffer
constexpr uintptr_t kBS_ReadLen  = 0x24;  // u32
constexpr uintptr_t kBS_ReadPos  = 0x28;  // u32
constexpr uintptr_t kBS_WritePos = 0x2C;  // u32

// MSVC std::string. 32 bytes: a 16-byte union that is either the characters
// themselves or a pointer to them, then the length, then the capacity. Capacity
// 15 means "small", and std::string::_Tidy_deallocate frees only above that --
// which is what makes a small key safe to leave on our own stack and a long one
// require the game's heap.
struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size;
    uint64_t cap;
};

typedef void  (__fastcall* fn_bucket)(void* unused, void* savefile, void* key, void* bucket);
typedef void* (__fastcall* fn_alloc)(size_t n);
typedef void  (__fastcall* fn_strdtor)(void* s);

// The three buckets, in the order the game's own driver writes them. The order
// is the wire order too, so it must not be reshuffled without a proto bump.
struct BucketDesc { uintptr_t off; const char* key; const char* label; };
const BucketDesc kBuckets[kInvBuckets] = {
    { kInv_Backpack, "inventory_backpack", "backpack" },
    { kInv_Storage,  "inventory_storage",  "storage"  },
    { kInv_Trash,    "inventory_trash",    "trash"    },
};

struct State {
    uintptr_t base = 0;
    bool      resolved = false;

    fn_bucket  bucket_write = nullptr;
    fn_bucket  bucket_read  = nullptr;
    fn_alloc   str_alloc    = nullptr;
    fn_strdtor str_dtor     = nullptr;
    const void** director_slot = nullptr;

    bool on        = false;
    bool is_client = false;
    bool announced = false;

    // --- the hook rendezvous ---
    //
    // Plain non-atomic fields on purpose. Both hooks can only fire from inside
    // one of this module's own calls, which happen on the game thread, and the
    // arming and the call are adjacent statements. The receive thread never
    // touches any of this.
    bool     cap_armed = false;   // host: a bucket serialize is in flight
    bool     cap_hit   = false;
    uint8_t* cap_buf   = nullptr; // OUR heap; ownership passes to the caller
    uint32_t cap_len   = 0;

    bool           sup_armed = false;  // client: a bucket load is in flight
    bool           sup_hit   = false;
    const uint8_t* sup_buf   = nullptr;
    uint32_t       sup_len   = 0;

    // What we last sent, so an unchanged inventory costs three serializes and a
    // compare and puts nothing on the wire. Same trick as the per-cat hash.
    uint64_t last_hash = 0;
    bool     has_last  = false;

    // F5: the Lamport clock over the shared bag. The highest seq this peer has
    // either sent or accepted from the other side -- see invsync_publish and
    // the accept rule in invsync_on_message/apply_now.
    uint32_t seq = 0;

    // --- the deferred apply (client) ---
    //
    // A copy, because the frame that carried it is released the instant the
    // dispatch switch returns. See the "why deferred" note above apply_now.
    bool     pend_have = false;
    int32_t  pend_coins = 0, pend_food = 0, pend_boxes = 0;
    uint64_t pend_hash = 0;
    uint32_t pend_seq  = 0;
    uint32_t pend_size[kInvBuckets] = {};
    uint8_t* pend_data[kInvBuckets] = {};

    // F6, 2026-09-06: a PERSISTENT copy of the last successfully applied
    // inventory (as opposed to `pend_*` above, which is transient and freed
    // the moment it's applied). See invsync_reapply_last_good -- mgmp_invlock
    // uses this to undo the bag-side half of a client's blocked equip/unequip
    // attempt, which a raw byte revert of the cat's own equip slot cannot
    // reach on its own.
    bool     good_have = false;
    int32_t  good_coins = 0, good_food = 0, good_boxes = 0;
    uint64_t good_hash = 0;
    uint32_t good_seq  = 0;
    uint32_t good_size[kInvBuckets] = {};
    uint8_t* good_data[kInvBuckets] = {};

    uint32_t pushed = 0, applied = 0, skipped = 0;
    uint32_t deferred = 0, coalesced = 0;
};

State g;

uint64_t fnv1a(const void* p, uint32_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// --- SEH shims --------------------------------------------------------------
//
// Same rule as mgmp_catsync: every raw call into the game lives in its own
// function with no C++ objects in scope, so a recovered-rather-than-declared
// pointer produces a bad log line instead of taking the game down.

bool safe_bucket_write(void* sf, void* key, void* bucket) {
    __try { g.bucket_write(nullptr, sf, key, bucket); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_bucket_read(void* sf, void* key, void* bucket) {
    __try { g.bucket_read(nullptr, sf, key, bucket); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_str_dtor(void* s) {
    __try { g.str_dtor(s); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_alloc(size_t n, void** out) {
    __try { *out = g.str_alloc(n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// --- the key string ---------------------------------------------------------
//
// Both bucket functions take the key BY VALUE and destroy it, exactly the way
// MewDirector::init takes its filename -- so we hand over a fresh image per
// call and must not destroy it ourselves afterwards.
//
// Two of the three keys are longer than the 15-character small-string buffer,
// so their characters must live on the GAME's heap: the destructor at the far
// end will call the game's free on them, and our /MT CRT's allocation is not
// its to free. Capacity is computed the way the game's own copy constructor
// computes it (`n | 15`, floored at 22) purely so the two agree if anything
// ever compares them.
bool make_key(GameStr& s, const char* text) {
    size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) {
        memcpy(s.buf, text, n + 1);
        s.size = n;
        s.cap  = 15;
        return true;
    }
    size_t cap = n | 15;
    if (cap < 22) cap = 22;
    void* p = nullptr;
    if (!safe_alloc(cap + 1, &p) || !p) return false;
    memcpy(p, text, n + 1);
    s.ptr  = (char*)p;
    s.size = n;
    s.cap  = cap;
    return true;
}

// --- the run's inventory ----------------------------------------------------

struct RunInv {
    void* savefile  = nullptr;   // MewDirector+56, EMBEDDED -- an address, not a load
    void* inventory = nullptr;   // MewDirector+1416, a pointer
};

bool run_inv(RunInv& out) {
    if (!g.director_slot) return false;
    void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return false;

    uint8_t* d = (uint8_t*)dir;
    if (!mem_read(d + kDir_Inventory, &out.inventory, sizeof(out.inventory))) return false;
    if (!out.inventory) return false;
    out.savefile = d + kDir_SaveFile;
    return true;
}

// Serializes one bucket by driving the game's own writer and catching the blob
// at the bottom. `*out` is on OUR heap and the caller frees it.
bool serialize_bucket(void* sf, void* bucket, const char* keytext,
                      uint8_t** out, uint32_t* out_len) {
    *out = nullptr;
    *out_len = 0;

    GameStr key;
    if (!make_key(key, keytext)) return false;

    g.cap_buf = nullptr;
    g.cap_len = 0;
    g.cap_hit = false;
    g.cap_armed = true;
    bool ran = safe_bucket_write(sf, &key, bucket);
    g.cap_armed = false;

    if (!ran || !g.cap_hit) {
        free(g.cap_buf);
        g.cap_buf = nullptr;
        return false;
    }
    *out = g.cap_buf;
    *out_len = g.cap_len;
    g.cap_buf = nullptr;
    return true;
}

// Replaces one bucket by driving the game's own reader and feeding it our bytes
// at the bottom. The reader clears the bucket before it repopulates, so this is
// a wholesale replace and not a merge -- which is what we want, and is what the
// game does on ContinueAdventure.
bool apply_bucket(void* sf, void* bucket, const char* keytext,
                  const uint8_t* data, uint32_t len) {
    GameStr key;
    if (!make_key(key, keytext)) return false;

    g.sup_buf = data;
    g.sup_len = len;
    g.sup_hit = false;
    g.sup_armed = true;
    bool ran = safe_bucket_read(sf, &key, bucket);
    g.sup_armed = false;
    g.sup_buf = nullptr;
    g.sup_len = 0;

    return ran && g.sup_hit;
}

// NOT LATCHED -- see the note on mgmp_savefile's ensure_state. The panel's
// connect buttons can set the role after this has already run once, and both
// things this reads change at that moment: the role, and whether the two blob
// hooks exist (hooks_install_late puts them in).
//
// The hook test asks hooks_is_live rather than config().hook, and that is the
// stronger claim on purpose: config_set_role flips the flags, but a flag says
// what was ASKED for and a late install can still fail to resolve or to enable.
// Arming on the flag would push an inventory into the host's own save file and
// report it as sent.
void ensure_state() {
    const Config& c = config();
    bool host   = _stricmp(c.net_role, "host")   == 0;
    bool client = _stricmp(c.net_role, "client") == 0;
    g.is_client = client;

    bool hooks_on = hooks_is_live(T_SFStoreBlob) && hooks_is_live(T_SFLoadBlob);
    g.on = tune::kInvSync && (host || client) && g.resolved && hooks_on;

    static bool said = false;
    if (!g.on && tune::kInvSync && (host || client) && !said) {
        said = true;
        if (!g.resolved)
            log_line("INVSYNC", "!! disabled: the game functions it calls did "
                                "not verify against this build");
        else if (!hooks_on)
            log_line("INVSYNC", "!! disabled: hook_sfstoreblob / hook_sfloadblob "
                                "are not live, and without them a push would go "
                                "to the save file instead of to the peer");
    }
}

} // namespace

// ---------------------------------------------------------------------------

void invsync_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;

    // Only the four this module calls. The other kCalls entries are catsync's
    // and are verified by catsync_set_base; verifying them twice would report
    // the same drift under two names.
    static const int kNeed[] = { C_InvBucketWrite, C_InvBucketRead,
                                 C_GameStrAlloc,  C_GameStrDtor };
    for (int idx : kNeed) {
        if (!addr_of_call((Call)idx)) {
            log_line("INVSYNC", "!! %s did not resolve by signature "
                                "-- inventory sync is OFF", kCalls[idx].name);
            return;
        }
    }

    g.bucket_write  = (fn_bucket) addr_of_call(C_InvBucketWrite);
    g.bucket_read   = (fn_bucket) addr_of_call(C_InvBucketRead);
    g.str_alloc     = (fn_alloc)  addr_of_call(C_GameStrAlloc);
    g.str_dtor      = (fn_strdtor)addr_of_call(C_GameStrDtor);
    g.director_slot = (const void**)addr_of_data(D_MewDirectorPtr);
    if (!g.director_slot) {
        log_line("INVSYNC", "!! the MewDirector* global did not resolve"
                            " -- inventory sync is OFF");
        return;
    }
    g.resolved = true;
}

void invsync_init() {
    ensure_state();
    if (!g.on || g.announced) return;
    g.announced = true;
    log_line_lvl(LogLevel::Trace, "INVSYNC", "armed -- %s",
             g.is_client ? "this peer's run inventory will be overwritten with "
                           "the host's"
                         : "this peer's run inventory will be published to the "
                           "client");
}

void invsync_shutdown() {
    for (uint32_t i = 0; i < kInvBuckets; ++i) { free(g.pend_data[i]); g.pend_data[i] = nullptr; }
    g.pend_have = false;
    for (uint32_t i = 0; i < kInvBuckets; ++i) { free(g.good_data[i]); g.good_data[i] = nullptr; }
    g.good_have = false;
    if (!g.announced) return;
    log_line_lvl(LogLevel::Trace, "INVSYNC",
             "done: %u pushed, %u applied, %u unchanged, "
             "%u deferred to the map tick (%u superseded before it ran)",
             g.pushed, g.applied, g.skipped, g.deferred, g.coalesced);
}

// --- the hooks --------------------------------------------------------------

bool invsync_intercept_store(void* key, void* bs) {
    if (!g.cap_armed || !bs) return false;
    g.cap_armed = false;   // one bucket per arm; a second store in the same
                           // call would be a path we do not understand, and
                           // silently taking its blob instead would be worse
                           // than letting it through.

    // The stream arrives from sub_14022CBD0 in WRITE mode, but the game's own
    // store accepts either, so accept either here too rather than assuming.
    uint32_t mode = 0;
    const void* src = nullptr;
    uint32_t    len = 0;
    if (!mem_read((uint8_t*)bs + kBS_Mode, &mode, sizeof(mode))) return false;
    if (mode == 0) {
        if (!mem_read((uint8_t*)bs + kBS_ReadBuf, &src, sizeof(src))) return false;
        if (!mem_read((uint8_t*)bs + kBS_ReadLen, &len, sizeof(len))) return false;
    } else if (mode == 1) {
        if (!mem_read((uint8_t*)bs + kBS_WriteBuf, &src, sizeof(src))) return false;
        if (!mem_read((uint8_t*)bs + kBS_WriteLen, &len, sizeof(len))) return false;
    } else {
        return false;      // file mode: not a path we produce
    }

    if (len > kMaxInvBytes) {
        log_line("INVSYNC", "!! a bucket serialized to %u bytes, over the %u-byte "
                            "cap -- not captured", len, kMaxInvBytes);
        return false;
    }

    // An empty bucket is a real state and must round-trip. Take the hit with a
    // null buffer rather than falling through to the save file.
    if (len == 0 || !src) {
        g.cap_buf = nullptr;
        g.cap_len = 0;
        g.cap_hit = true;
        safe_str_dtor(key);
        return true;
    }

    uint8_t* copy = (uint8_t*)malloc(len);
    if (!copy) return false;
    if (!mem_read(src, copy, len)) { free(copy); return false; }

    g.cap_buf = copy;
    g.cap_len = len;
    g.cap_hit = true;

    // We are standing in for a function that would have destroyed this string.
    // Skipping it leaks 32 bytes of the game's heap per long-keyed bucket per
    // push, which over a run is small and permanent and exactly the kind of
    // thing that gets blamed on the game.
    safe_str_dtor(key);
    return true;
}

bool invsync_intercept_load(void* key, void* bs) {
    if (!g.sup_armed || !bs) return false;
    g.sup_armed = false;

    // Field for field, sub_14022C620's own success path -- with two changes:
    // the buffer is ours, and the "owns" byte therefore stays 0 so the game's
    // free is never called on our /MT CRT's memory.
    //
    // Deliberately no attempt to release whatever the stream held before. The
    // streams we are handed come from sub_14022DDF0, which builds one fresh and
    // zeroed for every call, so there is nothing to release; if that ever
    // changed, freeing a game-heap pointer from here would be the wrong fix
    // anyway.
    uint8_t owns = 0;
    if (!mem_read((uint8_t*)bs + kBS_ReadOwns, &owns, sizeof(owns))) return false;
    if (owns) {
        log_line("INVSYNC", "!! the load stream already owns a buffer -- not "
                            "substituting, falling through to the save file");
        return false;
    }

    const void* buf = g.sup_buf;
    uint32_t    len = g.sup_len;
    uint32_t    mode = 0, zero = 0;

    bool ok = mem_write((uint8_t*)bs + kBS_ReadBuf, &buf,  sizeof(buf))
           && mem_write((uint8_t*)bs + kBS_ReadLen, &len,  sizeof(len))
           && mem_write((uint8_t*)bs + kBS_Mode,    &mode, sizeof(mode))
           && mem_write((uint8_t*)bs + kBS_ReadPos, &zero, sizeof(zero))
           && mem_write((uint8_t*)bs + kBS_WritePos,&zero, sizeof(zero));
    if (!ok) return false;

    g.sup_hit = true;
    safe_str_dtor(key);
    return true;
}

// --- host -------------------------------------------------------------------

// Same reason as catsync_forget: the inventory is deduped against the last
// push, and a peer that just arrived has not seen it.
void invsync_forget() {
    ensure_state();
    if (!g.on) return;
    g.has_last  = false;
    g.last_hash = 0;
}

void invsync_publish(const char* why) {
    ensure_state();
    if (!g.on || !net_active()) return;

    RunInv ri{};
    if (!run_inv(ri)) {
        // No run yet. Not an error: the host publishes at its first map node,
        // which can be before there is anything to publish.
        return;
    }

    InventoryMsg m{};
    if (!mem_read((uint8_t*)ri.inventory + kInv_Coins, &m.coins, sizeof(m.coins)) ||
        !mem_read((uint8_t*)ri.inventory + kInv_Food,  &m.food,  sizeof(m.food))  ||
        !mem_read((uint8_t*)ri.inventory + kInv_Boxes, &m.boxes, sizeof(m.boxes))) {
        log_line("INVSYNC", "!! could not read the inventory scalars -- nothing "
                            "published (%s)", why);
        return;
    }

    bool failed = false;
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        void* bucket = (uint8_t*)ri.inventory + kBuckets[i].off;
        if (!serialize_bucket(ri.savefile, bucket, kBuckets[i].key,
                              &m.data[i], &m.size[i])) {
            // Do not send a partial inventory. A missing bucket applied on the
            // client would CLEAR that bucket -- the reader clears before it
            // repopulates -- so half a push is destructive in a way no push is.
            log_line("INVSYNC", "!! serializing the %s failed -- nothing "
                                "published (%s). If this repeats, the store hook "
                                "is not firing and the blob went to the save "
                                "file instead.", kBuckets[i].label, why);
            failed = true;
            break;
        }
    }

    if (!failed) {
        uint64_t h = fnv1a(&m.coins, sizeof(m.coins));
        h = fnv1a(&m.food,  sizeof(m.food),  h);
        h = fnv1a(&m.boxes, sizeof(m.boxes), h);
        for (uint32_t i = 0; i < kInvBuckets; ++i) {
            h = fnv1a(&m.size[i], sizeof(m.size[i]), h);
            if (m.size[i]) h = fnv1a(m.data[i], m.size[i], h);
        }
        m.hash = h;

        if (g.has_last && g.last_hash == h) {
            ++g.skipped;
        } else {
            // F5: stamp the Lamport clock only NOW, on the send that actually
            // goes out -- not earlier, so a run of skipped (unchanged)
            // publishes never burns seq numbers it does not need to.
            m.seq = ++g.seq;
            if (net_send_inventory(m)) {
                g.last_hash = h;
                g.has_last  = true;
                ++g.pushed;
                log_line("INVSYNC", "-> inventory %016llx seq=%u: %u/%u/%u bytes "
                                    "(backpack/storage/trash), %d coins %d food "
                                    "%d boxes (%s)",
                         (unsigned long long)h, m.seq, m.size[0], m.size[1], m.size[2],
                         m.coins, m.food, m.boxes, why);
            }
        }
    }

    for (uint32_t i = 0; i < kInvBuckets; ++i) free(m.data[i]);
}


// --- client -----------------------------------------------------------------

// WHY THIS IS DEFERRED RATHER THAN APPLIED WHERE IT ARRIVES.
//
// The reader clears the bucket before it repopulates, and that clear
// (sub_1401F0F00) walks the intrusive list and FREES every node. Any UI holding
// one of those Equipment pointers is left dangling -- and InventoryScreen2+0x20
// is a CustomVector<InventoryItemBox*> of exactly those. The game never hits
// this because its only caller is ContinueAdventure, which runs when no
// inventory screen exists; we can, because a push lands whenever the HOST enters
// a node, which is a moment the client's own UI knows nothing about. The client
// browsing its bag while the host clicks the next node is an ordinary thing for
// two people to do, and it is the one failure in this module that is a crash
// rather than a divergence.
//
// So the apply is moved to the point the client was already going to be on the
// map screen with no other screen up: the tail of MapScreen::update, the same
// place the node entry itself is deferred to. That is also exactly in time --
// nothing between a push and the next node entry can consume the inventory.
//
// If a second inventory arrives before the first has been applied, the newer one
// simply replaces it. The message is whole state, not a delta, so the older one
// has no information the newer one lacks.
static void free_pending() {
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        free(g.pend_data[i]);
        g.pend_data[i] = nullptr;
        g.pend_size[i] = 0;
    }
    g.pend_have = false;
}

static bool stash_pending(const InventoryMsg& m) {
    if (g.pend_have) ++g.coalesced;
    free_pending();
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        if (!m.size[i] || !m.data[i]) continue;
        uint8_t* copy = (uint8_t*)malloc(m.size[i]);
        if (!copy) { free_pending(); return false; }
        memcpy(copy, m.data[i], m.size[i]);
        g.pend_data[i] = copy;
        g.pend_size[i] = m.size[i];
    }
    g.pend_coins = m.coins;
    g.pend_food  = m.food;
    g.pend_boxes = m.boxes;
    g.pend_hash  = m.hash;
    g.pend_seq   = m.seq;
    g.pend_have  = true;
    return true;
}

static void apply_now(const InventoryMsg& m, const char* when) {
    RunInv ri{};
    if (!run_inv(ri)) {
        // Before a save is loaded there is nothing to write into. Not an error;
        // the host may publish while this peer is still on the menu.
        return;
    }

    uint32_t done = 0;
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        void* bucket = (uint8_t*)ri.inventory + kBuckets[i].off;
        if (apply_bucket(ri.savefile, bucket, kBuckets[i].key,
                         m.data[i], m.size[i])) {
            ++done;
        } else {
            log_line("INVSYNC", "!! applying the %s failed -- this peer's %s is "
                                "now UNTRUSTWORTHY", kBuckets[i].label,
                     kBuckets[i].label);
        }
    }

    bool scalars =
        mem_write((uint8_t*)ri.inventory + kInv_Coins, &m.coins, sizeof(m.coins)) &&
        mem_write((uint8_t*)ri.inventory + kInv_Food,  &m.food,  sizeof(m.food))  &&
        mem_write((uint8_t*)ri.inventory + kInv_Boxes, &m.boxes, sizeof(m.boxes));
    if (!scalars)
        log_line("INVSYNC", "!! could not write the inventory scalars");

    // F5 ECHO-LOOP FIX, found live 2026-09-05: this peer's own dedupe cache
    // (last_hash/has_last, used by invsync_publish to skip an unchanged send)
    // used to update ONLY on the send side. So the instant this peer applied
    // an incoming push, its OWN state changed under it without its OWN
    // "what have I already told the other side" cache changing to match --
    // the very next poll (mgmp_invlock's FrameBegin tick, ~300ms later) saw
    // bytes that differed from a STALE last_hash and re-sent the exact state
    // it had just received, with a new seq. The other peer accepted that as
    // newer (it was, by seq) and re-applied it, repeating the cycle. Measured
    // live: seq climbing into the 20s within seconds and the bucket sizes
    // visibly oscillating between two values the whole time the screen was
    // open, on BOTH peers. Setting the cache here, to what was just applied,
    // is what makes "nothing has changed since the other side's last word"
    // true again immediately after applying it.
    g.last_hash = m.hash;
    g.has_last  = true;

    // F6: refresh the persistent "last good" copy -- see the State comment.
    // Only on a COMPLETE, successful apply; a partial one is exactly the
    // "untrustworthy" case above and must not become the thing we'd revert
    // BACK to later.
    if (done == kInvBuckets && scalars) {
        // Copy BEFORE freeing the old buffer -- invsync_reapply_last_good
        // feeds `m.data[i]` straight from `g.good_data[i]` itself, so
        // freeing first would leave m.data[i] dangling before it's read.
        for (uint32_t i = 0; i < kInvBuckets; ++i) {
            uint8_t* fresh = nullptr;
            uint32_t fresh_size = 0;
            if (m.size[i] && m.data[i]) {
                fresh = (uint8_t*)malloc(m.size[i]);
                if (fresh) { memcpy(fresh, m.data[i], m.size[i]); fresh_size = m.size[i]; }
            }
            free(g.good_data[i]);
            g.good_data[i] = fresh;
            g.good_size[i] = fresh_size;
        }
        g.good_coins = m.coins;
        g.good_food  = m.food;
        g.good_boxes = m.boxes;
        g.good_hash  = m.hash;
        g.good_seq   = m.seq;
        g.good_have  = true;
    }

    ++g.applied;
    log_line("INVSYNC", "<- inventory %016llx seq=%u: %u/%u buckets applied, "
                        "%d coins %d food %d boxes (%s)%s",
             (unsigned long long)m.hash, m.seq, done, kInvBuckets,
             m.coins, m.food, m.boxes, when,
             (done == kInvBuckets && scalars) ? "" : "  -- INCOMPLETE");
}

// Deferral is only correct while something is guaranteed to call
// invsync_apply_pending. For the client that is the map-follow tick, gated on
// net_follow -- with it off nothing would ever drain the queue, so applying
// immediately is the only option left. For the HOST that guarantee is now its
// own EnterNode hook (mgmp_follow.cpp's host branch calls
// invsync_apply_pending too) -- unconditional, because the host always drives
// its own node entries.
static bool defer_applies() { return g.is_client ? config().net_follow : true; }

void invsync_on_message(const InventoryMsg& m) {
    ensure_state();
    if (!g.on) return;

    // F5 accept rule: the Lamport clock over the shared bag. Strictly newer
    // always wins. A TIE means both peers published from the same prior seq
    // without having seen each other yet -- the rare true-collision case --
    // and goes to the host: we are looking at an incoming message, so if WE
    // are the client the sender can only be the host (direct two-peer link),
    // and the host's version should win the tie; if we are the host, the
    // sender was the client, and the host's OWN already-applied version wins,
    // so the incoming one is rejected. Updated on acceptance, not on
    // application, because the inventory screen (the only way to make a new
    // local edit) is unreachable while a push is held for a battle to end, so
    // there is nothing for a later stash to race against in between.
    if (!(m.seq > g.seq || (m.seq == g.seq && g.is_client))) {
        log_line("INVSYNC", "!! inventory seq=%u <= our seq=%u -- stale, dropped",
                 m.seq, g.seq);
        return;
    }
    g.seq = m.seq;

    // F5 BUG, found live 2026-09-05: this used to defer unconditionally for
    // any client with net_follow on, no battle check at all -- correct under
    // the OLD host-only, per-node-only design, where a push only ever
    // happened AT node entry, so "apply just before the next node" was
    // already almost simultaneous with arrival. Now that mgmp_invlock polls
    // while the inventory screen is open mid-node, that same blanket
    // deferral held every push hostage until the NEXT node crossing --
    // which is not "immediate" at all if the players just stand on the map
    // browsing inventory. The inventory screen can only be open OUTSIDE a
    // battle in the first place, so gating on lockstep_in_battle() the same
    // way mgmp_catsync already does costs nothing and fixes it: apply right
    // away except in the one case (still mid-battle) deferral exists for.
    // NOT WHILE THE MAP ISN'T TICKING, either -- found live 2026-09-05, and NOT
    // a return to the blanket deferral the comment above describes reverting.
    // That old bug held a push until the NEXT NODE regardless of whether a
    // screen was actually open. This holds only for as long as
    // MapScreen::update genuinely is not ticking (confirmed: it does not run
    // under a modal screen like the inventory at all), and follow_map_update
    // now drains any hold on EVERY tick rather than only at node entry -- so
    // this lands the instant the screen closes, not at the next node. What it
    // buys: applying straight into the Inventory object while the screen
    // showing it is actually open produced a visible "everything just
    // vanished" flash on the peer watching it -- the state is correct again
    // moments later, but a person saw the transition happen.
    if (g.is_client && config().net_follow && !follow_on_map()) {
        if (stash_pending(m)) { ++g.deferred; return; }
        // Could not hold -- fall through and apply now rather than drop it.
    }

    if (!lockstep_in_battle()) { apply_now(m, "on arrival"); return; }
    if (!defer_applies()) { apply_now(m, "on arrival"); return; }

    if (!stash_pending(m)) {
        // Out of memory holding at most 3 x kMaxInvBytes. Applying it here is
        // the lesser risk: the crash it dodges needs the peer to have its
        // inventory screen open, and dropping the push silently is a divergence
        // every time.
        log_line("INVSYNC", "!! could not hold the inventory for the map tick -- "
                            "applying it here instead");
        apply_now(m, "on arrival, deferral failed");
        return;
    }
    ++g.deferred;
}

void invsync_apply_pending(const char* why) {
    ensure_state();
    if (!g.on || !g.pend_have) return;

    InventoryMsg m{};
    m.coins = g.pend_coins;
    m.food  = g.pend_food;
    m.boxes = g.pend_boxes;
    m.hash  = g.pend_hash;
    m.seq   = g.pend_seq;
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        m.size[i] = g.pend_size[i];
        m.data[i] = g.pend_data[i];
    }
    apply_now(m, why);
    free_pending();   // frees the same buffers m borrowed; m is dead here
}

// F6, 2026-09-06 -- see mgmp_invlock.h/State's `good_*` comment above.
// Deferring this behind `follow_on_map()` (queuing through the same
// `pend_*` path a real incoming push uses) was tried and made the visible
// glitch WORSE, live-confirmed: the longer the wrong state stays live while
// the screen is open, the bigger and more jarring the eventual catch-up
// looked once it finally landed (items going invisible, a correction
// animating in reverse on some later, unrelated interaction). Reverted to
// calling apply_now() immediately and unconditionally -- accepted by the
// user as "works, cosmetically rough, not serious" over every deferred
// variant tried. Known risk, stated plainly, not yet hit in practice: this
// runs while the client's OWN inventory screen may be open, which this
// header's own note above (and mgmp_invsync.h's) names as the one hazard in
// this module that is a crash rather than a divergence -- a screen holding a
// pointer into a bag node the reader just freed.
void invsync_reapply_last_good(const char* why) {
    ensure_state();
    if (!g.on || !g.good_have) return;

    InventoryMsg m{};
    m.coins = g.good_coins;
    m.food  = g.good_food;
    m.boxes = g.good_boxes;
    m.hash  = g.good_hash;
    m.seq   = g.good_seq;
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        m.size[i] = g.good_size[i];
        m.data[i] = g.good_data[i];
    }
    apply_now(m, why);   // re-populates good_* from itself -- a harmless no-op copy
}

} // namespace mgmp
