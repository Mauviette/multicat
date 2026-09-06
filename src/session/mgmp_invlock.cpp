// mgmp_invlock.cpp -- see mgmp_invlock.h for the design.
#include "mgmp_invlock.h"

#include "mgmp_net.h"          // net_role
#include "mgmp_invsync.h"      // F5: publish once a divergence settles
#include "mgmp_catsync.h"      // catsync_publish, catsync_run_cat_ids, catsync_resolve_by_id
#include "mgmp_addresses.h"    // kBtn_Name -- ROUND 5's click swallow
#include "mgmp_ownership.h"    // ownership_owner_of_catdata -- the inventory ownership badge
#include "mgmp_ownertable.h"   // kNoOwner
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"

#include <windows.h>           // GetTickCount64 -- the ownership badge's recency check
#include <cstring>

namespace mgmp {
namespace {

// Same offsets mgmp_catsync.cpp's own equip-slot-clear fix already validated
// live (see its "THE EQUIP-SLOT CLEAR-BEFORE-READ FIX" note): four
// self-contained 0x60-byte slots at CatData+0xA10, confirmed unchanged by the
// 2026-09-06 update (a live read-back of a real equipped item, "ExtraSetOfEyes"
// at +0xA70, matched exactly). Not shared via a header on purpose -- these are
// the only two consumers, and duplicating three numbers is cheaper than a new
// shared header for them.
constexpr uintptr_t kEquipSlotBase   = 0xA10;
constexpr uintptr_t kEquipSlotStride = 0x60;
constexpr uint32_t  kEquipSlotCount  = 4;
constexpr uint32_t  kEquipBytes      = kEquipSlotStride * kEquipSlotCount;

uintptr_t g_base = 0;

// F6/F4 ownership badge for the inventory screen -- see
// invlock_on_button_update's own header comment. Button+0x18C8 is
// CatSelector_Right's own cached CatData* for the currently-shown cat,
// found live in an earlier session (project memory) and reused here for
// pure display, not for gating any action.
constexpr uintptr_t kCatSelectorRight_Cat = 0x18C8;
constexpr uint64_t  kCurrentCatStaleMs    = 250;   // same figure mgmp_choice
                                                    // uses for its own screens
void*    g_current_cat         = nullptr;
uint64_t g_current_cat_tick_at = 0;

// F5 debounce, unchanged in shape from the old design: a divergence just
// happened and a publish is owed once things settle. See invlock_frame_tick.
struct Debounce {
    bool     pending     = false;
    uint32_t frames_left = 0;
};
Debounce g_debounce;

// Per-cat baseline for the equip-slot integrity poll. Flat array, matching
// mgmp_catsync's own `sent_id`/`sent_hash` shape -- a run is a few dozen
// cats, and a linear scan is not worth a hash map on a once-per-frame path.
struct Baseline {
    uint64_t id   = 0;
    bool     have = false;   // false until the first sighting seeds it
    uint8_t  bytes[kEquipBytes]     = {};   // the AUTHORITATIVE value
    uint8_t  last_seen[kEquipBytes] = {};   // what the live bytes read LAST FRAME
};
constexpr uint32_t kMaxTrackedCats = 64;   // matches catsync's own State::kMaxCats
Baseline g_baseline[kMaxTrackedCats];
uint32_t g_baseline_count = 0;

Baseline* baseline_for(uint64_t id) {
    for (uint32_t i = 0; i < g_baseline_count; ++i)
        if (g_baseline[i].id == id) return &g_baseline[i];
    if (g_baseline_count >= kMaxTrackedCats) return nullptr;
    g_baseline[g_baseline_count].id   = id;
    g_baseline[g_baseline_count].have = false;
    return &g_baseline[g_baseline_count++];
}

void arm_debounce() {
    g_debounce.pending     = true;
    g_debounce.frames_left = tune::kInvSyncDebounceFrames;
}

// CLIENT-SIDE CORRECTION DEBOUNCE, 2026-09-06 -- see run_client_correction's
// own header note for the live-reported bug this closes (repeated rapid
// clicking could duplicate an item permanently, a real desync/softlock
// risk, not just a visual one).
struct ClientCorrection {
    bool     pending     = false;
    uint32_t frames_left = 0;
};
ClientCorrection g_client_correction;

void arm_client_correction() {
    g_client_correction.pending     = true;
    g_client_correction.frames_left = tune::kInvSyncDebounceFrames;
}

// Re-scans every tracked cat and reverts any whose equip slots still differ
// from their baseline -- called ONCE, only after the client's own edits have
// gone quiet for a whole debounce window (see arm_client_correction's call
// site in check_cat). ROUND 4, 2026-09-06, closing a real bug reported live:
// "if the client fiddles enough, they can duplicate items and equip them,"
// causing a real desync/softlock (the ability cross-check catches an
// equipment mismatch even though state_hash does not, and HALTs).
//
// ROOT CAUSE OF ROUND 3'S GAP: reverting on EVERY SINGLE FRAME a divergence
// was seen called into the game's own bag reader
// (invsync_reapply_last_good -> apply_bucket, a real clear-and-rebuild)
// potentially many times a second during a burst of rapid clicks -- and per
// mgmp_invsync.h's own hard-won lesson from F5 ("calling the game's own
// bucket write/serialize path at ~3Hz produced ITEM DUPLICATION IN THE
// GAME'S OWN LOCAL STATE"), hammering the game's inventory machinery faster
// than it settles is exactly the class of bug that produces a duplicate,
// just on the READ/apply side this time instead of the write/serialize side
// that bug was first found on. The original F5 equip debounce existed for
// the identical reason ("the game's own mutation is not necessarily
// complete the instant o_click returns") and got dropped when this module
// switched from a click hook to a poll -- this restores the same discipline
// on the CLIENT's corrective writes specifically.
//
// THE FIX: separate DETECTING a change (every frame, cheap, no game calls)
// from CORRECTING it (at most once per debounce window). check_cat below
// only re-arms this debounce when a cat's live bytes actually MOVE
// frame-to-frame; once a whole window passes with nothing moving anywhere,
// this runs ONE full pass reverting whatever is still wrong and ONE
// invsync_reapply_last_good call for the whole batch -- a rapid burst of
// clicks now costs one correction, not one per frame it was ever seen.
void run_client_correction() {
    uint64_t ids[kMaxTrackedCats];
    uint32_t count = 0;
    if (!catsync_run_cat_ids(ids, kMaxTrackedCats, &count)) return;

    bool reverted_any = false;
    for (uint32_t i = 0; i < count; ++i) {
        Baseline* b = baseline_for(ids[i]);
        if (!b || !b->have) continue;
        void* cat = catsync_resolve_by_id(ids[i]);
        if (!cat) continue;

        uint8_t current[kEquipBytes];
        if (!mem_read((const uint8_t*)cat + kEquipSlotBase, current, sizeof(current)))
            continue;
        if (memcmp(b->bytes, current, sizeof(current)) == 0)
            continue;   // this cat settled back to the right value on its own

        if (mem_write((uint8_t*)cat + kEquipSlotBase, b->bytes, sizeof(b->bytes))) {
            log_line("INVLOCK", "reverted a local equipment change on cat "
                                "%016llx -- only the host manages equipment",
                     (unsigned long long)ids[i]);
            reverted_any = true;
        } else {
            log_line("INVLOCK", "!! detected a local equipment change on cat "
                                "%016llx but could NOT revert it -- this "
                                "peer's copy of that cat is now untrustworthy",
                     (unsigned long long)ids[i]);
            memcpy(b->bytes, current, sizeof(current));   // stop re-alarming every frame
        }
    }

    // One shared correction for the whole batch, not one per cat -- equip/
    // unequip moves an item through the ONE shared bag, so a single
    // known-good bag snapshot fixes every cat's bag-side half at once.
    if (reverted_any)
        invsync_reapply_last_good("reverted a local equip-slot edit");
}

// The per-cat check invlock_frame_tick runs for every cat in the run. On the
// client this ONLY detects and re-arms the correction debounce -- it never
// writes anything itself; see run_client_correction for why and for the
// actual revert.
void check_cat(uint64_t id, void* catdata) {
    Baseline* b = baseline_for(id);
    if (!b) return;   // more cats than kMaxTrackedCats -- logged once elsewhere

    uint8_t current[kEquipBytes];
    if (!mem_read((const uint8_t*)catdata + kEquipSlotBase, current, sizeof(current)))
        return;

    if (!b->have) {
        // First sighting of this cat this session -- seed, do not judge. The
        // game's own state at the moment we start watching is by definition
        // not a click we witnessed.
        memcpy(b->bytes, current, sizeof(current));
        memcpy(b->last_seen, current, sizeof(current));
        b->have = true;
        return;
    }

    const bool moved_this_frame = memcmp(b->last_seen, current, sizeof(current)) != 0;
    memcpy(b->last_seen, current, sizeof(current));   // always tracked, both roles

    if (net_role() == NetRole::Client) {
        // Never write here -- only decide whether a correction is owed and,
        // if so, how long to keep waiting for things to go quiet. A cat that
        // is STILL wrong but stopped moving several frames ago does not
        // re-arm; run_client_correction (fired once by invlock_frame_tick)
        // is what actually notices and fixes it.
        if (moved_this_frame) arm_client_correction();
        return;
    }

    // HOST: this divergence IS the source of truth. Adopt it and arm the
    // debounced publish -- unchanged in shape from the old click-hook
    // design, just triggered by detection instead of by a hook firing.
    if (memcmp(b->bytes, current, sizeof(current)) != 0) {
        memcpy(b->bytes, current, sizeof(current));
        arm_debounce();
    }
}

} // namespace

void invlock_set_base(uintptr_t base) { g_base = base; }

void invlock_init() {
    if (!tune::kInvLock) return;
    log_line_lvl(LogLevel::Trace, "INVLOCK", "armed -- polling every cat's "
                 "equip slots once per frame (%s)",
                 net_role() == NetRole::Client
                     ? "reverting any local change, only the host may equip"
                     : "publishing shortly after any change settles");
}

// ROUND 5 -- see mgmp_invlock.h's own note. Preventive: swallow the click
// before the game ever sees it, rather than let the mutation happen and
// clean up afterward.
bool invlock_on_button_click(void* self) {
    if (!tune::kInvLock || !self || net_role() != NetRole::Client) return false;

    char name[64] = {};
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name)))
        return false;
    if (strncmp(name, "EquippedButton_", 15) != 0 &&
        strncmp(name, "InventoryButton_", 16) != 0)
        return false;

    log_line("INVLOCK", "swallowed a click on '%s' -- only the host manages equipment", name);
    return true;
}

void invlock_on_button_update(void* self) {
    if (!tune::kInvLock || !self) return;

    char name[64] = {};
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name))) return;
    if (strcmp(name, "CatSelector_Right") != 0) return;

    void* cat = nullptr;
    if (!mem_read((const uint8_t*)self + kCatSelectorRight_Cat, &cat, sizeof(cat)) || !cat)
        return;   // not resolved yet this tick -- leave the previous cache alone

    g_current_cat         = cat;
    g_current_cat_tick_at = GetTickCount64();
}

uint8_t invlock_current_cat_owner() {
    if (!tune::kInvLock || !g_current_cat) return kNoOwner;
    if (GetTickCount64() - g_current_cat_tick_at > kCurrentCatStaleMs) return kNoOwner;
    return ownership_owner_of_catdata(g_current_cat);
}

void invlock_note_authoritative_catdata(uint64_t id, const void* catdata) {
    if (!tune::kInvLock || !catdata) return;
    Baseline* b = baseline_for(id);
    if (!b) return;
    if (mem_read((const uint8_t*)catdata + kEquipSlotBase, b->bytes, sizeof(b->bytes))) {
        b->have = true;
        // Keep last_seen in lockstep too -- otherwise check_cat's very next
        // frame reads bytes that moved (this legitimate apply) but were
        // never recorded as "seen", and arms a correction debounce for
        // nothing: harmless (run_client_correction finds nothing to revert,
        // since baseline was just set to match), but a wasted cycle.
        memcpy(b->last_seen, b->bytes, sizeof(b->bytes));
    }
}

void invlock_frame_tick() {
    if (tune::kInvLock) {
        uint64_t ids[kMaxTrackedCats];
        uint32_t count = 0;
        if (catsync_run_cat_ids(ids, kMaxTrackedCats, &count)) {
            for (uint32_t i = 0; i < count; ++i) {
                void* cat = catsync_resolve_by_id(ids[i]);
                if (cat) check_cat(ids[i], cat);
            }
        }
    }

    // F5 debounce (host) -- unchanged from the old design's own comment: a
    // plain integer decrement almost always, and only calls into the game's
    // serializer the ONE time the counter reaches zero after a real
    // divergence armed it.
    if (g_debounce.pending && --g_debounce.frames_left == 0) {
        g_debounce.pending = false;
        invsync_publish("equip/unequip settled");
        catsync_publish("equip/unequip settled");
    }

    // Client correction debounce -- same shape, independent counter, so one
    // does not gate the other. See run_client_correction's header note.
    if (g_client_correction.pending && --g_client_correction.frames_left == 0) {
        g_client_correction.pending = false;
        run_client_correction();
    }
}

} // namespace mgmp
