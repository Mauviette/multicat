// mgmp_shopmirror.cpp -- see mgmp_shopmirror.h for the design.
#include "mgmp_shopmirror.h"

#include "mgmp_addresses.h"   // kBtn_Name, C_ButtonClick
#include "mgmp_resolve.h"
#include "mgmp_mainmenulock.h"
#include "mgmp_choice.h"      // the level-up recipient fix
#include "mgmp_catsync.h"     // catsync_resolve_by_id
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"

#include "MinHook.h"

#include <windows.h>
#include <cstring>

namespace mgmp {
namespace {

// Button+0xD0: a short std::string, the shop's own per-slot key ("i1", "i2",
// ...). Button+0x1B8: a pointer to a UTF-16 buffer, the item's display name.
// Both found live, 2026-09-06 -- see mgmp_shopmirror.h.
constexpr uintptr_t kShopSlotOff = 0xD0;
constexpr uintptr_t kShopNameOff = 0x1B8;

// How long to watch for a LevelUpScreen to appear after a REAL (local, not
// mirrored) buy click before giving up on announcing a recipient at all --
// wall-clock, not a frame count, matching mgmp_follow's own kWalkTimeoutMs
// pattern for the same "wait for a UI transition to actually finish" shape.
// Generous on purpose: a purchase plays an item-received animation/popup
// before any level-up screen appears, and this must not fire early on an
// ordinary item that never gets one either.
constexpr uint64_t kWatchTimeoutMs = 8000;

bool is_tracked_name(const char* name) {
    // ChestButton_alley used to be tracked here too -- moved to
    // mgmp_screensync's kVoteChestOpen, 2026-09-06: this mirror resolves
    // identity by the shop's own "iN" per-slot key (Button+0xD0), which a
    // single chest button never carries, so the mirror silently never fired
    // and the other peer's chest just never opened. See mgmp_screensync.h.
    return strcmp(name, "Shop_BuyButton") == 0;
}

bool read_slot(void* button, char* out, size_t out_size) {
    return mem_read_std_string((const uint8_t*)button + kShopSlotOff, out, out_size) && out[0];
}

// UTF-16 -> UTF-8, bounded. `out` is empty (not a failure) if the pointer or
// the read faults -- the caller's cross-check just has nothing to compare
// against, same fail-open spirit as mem_read_std_string's own contract.
void read_item_name(void* button, char* out, size_t out_size) {
    out[0] = 0;
    void* wptr = nullptr;
    if (!mem_read((const uint8_t*)button + kShopNameOff, &wptr, sizeof(wptr)) || !wptr) return;
    wchar_t wbuf[128] = {};
    if (!mem_read(wptr, wbuf, sizeof(wbuf) - sizeof(wchar_t))) return;
    wbuf[127] = 0;
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)out_size, nullptr, nullptr);
    out[out_size - 1] = 0;
}

// ITEMS BANNED FROM CO-OP ENTIRELY, 2026-09-06 (revised same day). "Bonbon
// rare" grants a level to a RANDOMLY ROLLED recipient (a cat at the lowest
// level) -- confirmed live, across multiple attempts, that this random roll
// can ALSO grant MULTIPLE LEVELS AT ONCE (one purchase produced 8 options on
// the buyer's screen -- two stacked rounds of 4 -- against 4 on a mirrored,
// corrected screen), a class of variability the subject-redirect and
// init-recall fixes above do not fully cover: each fix closed one layer
// (subject, options, ...) and exposed another rather than reaching a stable
// end state.
//
// FIRST TRIED: host-only purchase (never mirrored, the result reaching the
// client through the ordinary cat-state sync instead). REVERTED THE SAME
// DAY, at the user's explicit call, after a second real bug surfaced live:
// the purchase spends the SHARED adventure_coins, but nothing in that
// host-only path ever told mgmp_invsync a change had happened (an ordinary
// mirrored purchase does not need this -- both peers fire the same real
// click and both deduct their own local copy by the same amount -- but here
// only the host's copy ever moved), so the client's coin count stayed stale
// until the next node/battle boundary -- long enough that the client could
// spend coins the host, from its own already-lower real balance, could no
// longer actually afford. A fix was attempted (publish immediately after
// the purchase) but the user judged the whole class of problem too fragile
// to keep chasing one layer at a time, the same call already made once for
// the earlier multi-level bug above, and asked to remove the feature
// entirely instead: KNOWN, ACCEPTED LIMITATION -- kQuarantinedShopItems are
// simply unbuyable in co-op, by EITHER peer. See
// shopmirror_on_button_click's own quarantine branch below.
const char* kQuarantinedShopItems[] = {
    "Bonbon rare",
};

bool is_quarantined_item(const char* name) {
    for (const char* q : kQuarantinedShopItems)
        if (strcmp(name, q) == 0) return true;
    return false;
}

constexpr int kMaxTracked = 32;
struct Tracked {
    void*    ptr  = nullptr;
    char     slot[8] = {};
    uint64_t seen_at = 0;
};

constexpr int kMaxPending = 4;
struct Pending {
    bool     used = false;
    ShopBuyMsg msg;
    uint64_t queued_at = 0;
};

// Buyer side ONLY: armed after a REAL local buy click (never after a
// mirrored/injected one -- shopmirror_on_button_click is never reached for
// those, see mgmp_mainmenulock's `injecting` guard), watching for a NEW
// LevelUpScreen to announce its recipient. See mgmp_shopmirror.h's
// "THE RECIPIENT PROBLEM" note for why this is fully decoupled from
// publishing the purchase itself.
constexpr int kMaxRecipientWatch = 4;
struct RecipientWatch {
    bool     used = false;
    void*    screen_before = nullptr;
    uint64_t deadline_at = 0;
};

// Receiver side. ARMED THE INSTANT A MIRRORED CLICK FIRES (`fire()`), not
// when a LevelUpTargetMsg arrives -- a real deadlock was found live,
// 2026-09-06, in the version that only armed on message receipt: the wrong
// screen can already be open and clickable (if ownership happens to allow
// it) in the window BEFORE the buyer has even discovered and announced the
// real recipient. `have_target` tracks which half is still missing;
// shopmirror_levelup_redirect_pending() (what mgmp_choice swallows clicks
// on) is true for the WHOLE window from firing to full resolution, not just
// the tail end of it.
constexpr int kMaxRedirect = 4;
struct Redirect {
    bool     used         = false;
    bool     have_target  = false;   // do we know the correct recipient yet?
    uint64_t target_cat_id = 0;
    uint64_t deadline_at  = 0;
};

struct State {
    uintptr_t base = 0;
    Tracked         tracked[kMaxTracked];
    Pending         pending[kMaxPending];
    RecipientWatch  recipient_watch[kMaxRecipientWatch];
    Redirect        redirect[kMaxRedirect];
} g;

constexpr uint64_t kPendingTimeoutMs = 5000;

Tracked* find_tracked_by_slot(const char* slot) {
    const uint64_t now = GetTickCount64();
    for (auto& t : g.tracked)
        if (t.ptr && strcmp(t.slot, slot) == 0 && now - t.seen_at < kPendingTimeoutMs)
            return &t;
    return nullptr;
}

void track(void* button, const char* slot) {
    for (auto& t : g.tracked) {
        if (t.ptr == button || strcmp(t.slot, slot) == 0) {
            t.ptr = button;
            strncpy(t.slot, slot, sizeof(t.slot) - 1);
            t.seen_at = GetTickCount64();
            return;
        }
    }
    for (auto& t : g.tracked) {
        if (!t.ptr) {
            t.ptr = button;
            strncpy(t.slot, slot, sizeof(t.slot) - 1);
            t.seen_at = GetTickCount64();
            return;
        }
    }
    // Cache full -- oldest entry loses. A shop realistically never has more
    // than a handful of items on screen at once, so this is a soft cap, not
    // an expected path.
    Tracked* oldest = &g.tracked[0];
    for (auto& t : g.tracked) if (t.seen_at < oldest->seen_at) oldest = &t;
    oldest->ptr = button;
    strncpy(oldest->slot, slot, sizeof(oldest->slot) - 1);
    oldest->seen_at = GetTickCount64();
}

void fire(void* target, const char* slot) {
    const uintptr_t addr = addr_of_call(C_ButtonClick);
    if (!addr) {
        log_line("SHOPMIRROR", "!! %s did not resolve -- cannot mirror slot '%s'",
                 kCalls[C_ButtonClick].name, slot);
        return;
    }

    // Armed BEFORE the click, not after -- see Redirect's own header note.
    // Every mirrored click gets one of these regardless of whether it turns
    // out to grant a level-up; tick_redirects clears it quickly (no screen
    // ever appears) for the common, ordinary-item case.
    Redirect* r = nullptr;
    for (auto& slot_r : g.redirect) if (!slot_r.used) { r = &slot_r; break; }
    if (r) {
        r->used         = true;
        r->have_target  = false;
        r->target_cat_id = 0;
        r->deadline_at  = GetTickCount64() + kWatchTimeoutMs;
    } else {
        log_line("SHOPMIRROR", "!! redirect queue full -- a mismatched level-up recipient"
                              " for slot '%s' may go uncorrected", slot);
    }

    log_line("SHOPMIRROR", "mirroring slot '%s' -- firing the matching click here", slot);
    mainmenulock_begin_injected_click();
    ((fn_button_click)addr)(target, true);
    mainmenulock_end_injected_click();
}

// Tries every still-pending mirror against the current tracked cache. Called
// every frame (from shopmirror_on_button_update) and immediately on message
// receipt, so ordering either way resolves as soon as both halves exist.
void try_pending() {
    const uint64_t now = GetTickCount64();
    for (auto& p : g.pending) {
        if (!p.used) continue;
        if (Tracked* t = find_tracked_by_slot(p.msg.slot)) {
            if (p.msg.name[0]) {
                char local_name[64] = {};
                read_item_name(t->ptr, local_name, sizeof(local_name));
                if (local_name[0] && strcmp(local_name, p.msg.name) != 0) {
                    log_line("SHOPMIRROR", "!! slot '%s' name mismatch: peer said '%s', "
                                          "this peer's item is '%s' -- mirroring anyway",
                             p.msg.slot, p.msg.name, local_name);
                }
            }
            fire(t->ptr, p.msg.slot);
            p.used = false;
        } else if (now - p.queued_at > kPendingTimeoutMs) {
            log_line("SHOPMIRROR", "!! slot '%s' never appeared on this peer's screen -- "
                                  "giving up on mirroring it", p.msg.slot);
            p.used = false;
        }
    }
}

// Buyer side: advances every armed RecipientWatch, announcing the recipient
// the moment a NEW level-up screen is found. Silently gives up (an ordinary
// item -- the common case) if the window elapses with none appearing.
void tick_recipient_watches() {
    const uint64_t now = GetTickCount64();
    for (auto& w : g.recipient_watch) {
        if (!w.used) continue;
        void* now_screen = choice_current_level_screen();
        if (now_screen && now_screen != w.screen_before) {
            uint64_t id = 0;
            if (choice_screen_subject_raw_id(now_screen, id) && id) {
                LevelUpTargetMsg m{};
                m.cat_id = id;
                net_send_levelup_target(m);
                log_line("SHOPMIRROR", "a purchase opened a level-up screen for cat id "
                                      "%016llx -- announced it", (unsigned long long)id);
            }
            w.used = false;
        } else if (now >= w.deadline_at) {
            w.used = false;   // no level-up screen showed up -- an ordinary item
        }
    }
}

// Receiver side: advances every armed Redirect. shopmirror_levelup_redirect_
// pending() is true for as long as ANY entry here has `used == true` --
// which now spans the WHOLE window from firing the mirrored click to full
// resolution, closing the softlock this used to leave open (see Redirect's
// own header note and mgmp_choice.h's use of the pending flag).
void tick_redirects() {
    const uint64_t now = GetTickCount64();
    for (auto& r : g.redirect) {
        if (!r.used) continue;
        void* screen = choice_current_level_screen();

        if (!screen) {
            // No level-up screen (yet, or ever). Keep swallowing until
            // either one appears or the whole window times out -- an
            // ordinary item resolves here, silently, once the timeout hits.
            if (now >= r.deadline_at) r.used = false;
            continue;
        }

        if (!r.have_target) {
            // A screen IS open but we do not yet know the correct
            // recipient -- keep swallowing (used stays true) rather than
            // let anyone decide on a possibly-wrong cat. Only the deadline
            // ends this, and it is worth a line when it does: it means the
            // buyer's own announcement never arrived at all.
            if (now >= r.deadline_at) {
                log_line("SHOPMIRROR", "!! a level-up screen opened here but no recipient"
                                      " was ever announced -- giving up on the redirect"
                                      " after %llums", (unsigned long long)kWatchTimeoutMs);
                r.used = false;
            }
            continue;
        }

        uint64_t local_id = 0;
        choice_screen_subject_raw_id(screen, local_id);
        if (local_id == r.target_cat_id) { r.used = false; continue; }   // already agrees

        if (void* correct = catsync_resolve_by_id(r.target_cat_id)) {
            if (choice_force_level_screen_subject(screen, correct))
                log_line("SHOPMIRROR", "!! local roll picked a different cat (%016llx) than "
                                      "the buyer got (%016llx) -- redirected this screen",
                         (unsigned long long)local_id, (unsigned long long)r.target_cat_id);
            else
                log_line("SHOPMIRROR", "!! could not redirect the level-up screen to cat "
                                      "%016llx -- write failed", (unsigned long long)r.target_cat_id);
        } else {
            log_line("SHOPMIRROR", "!! the buyer's chosen cat %016llx is not in this peer's "
                                  "run -- cannot redirect", (unsigned long long)r.target_cat_id);
        }
        r.used = false;
    }
}

// True (with the id) if a shop-purchase recipient is already known RIGHT
// NOW -- a peek, not a consume. Used only by the init hook below, which acts
// opportunistically: if the timing favours it, fine; if not, tick_redirects
// above remains the fallback (subject-only patch, imperfect but already
// working and carrying no new risk).
bool peek_known_target(uint64_t& out_id) {
    for (auto& r : g.redirect)
        if (r.used && r.have_target) { out_id = r.target_cat_id; return true; }
    return false;
}

// glaiel::LevelUpScreen::init(LevelUpScreen* this, CatData* subject,
// std::function<void()>* on_commit, void* unused) -- see
// mgmp_shopmirror.h's header note on shopmirror_install_levelup_hook for the
// full reasoning. RVA 0x37A290, found live 2026-09-06.
typedef void (__fastcall* fn_levelup_init)(void* self, void* subject, void* functor, void* r9);
fn_levelup_init o_levelup_init = nullptr;

void __fastcall h_levelup_init(void* self, void* subject, void* functor, void* r9) {
    o_levelup_init(self, subject, functor, r9);   // always, unmodified -- see below

    // Opportunistic second call: ONLY safe because it happens synchronously,
    // right here, before the real caller's own stack frame (which owns
    // `functor`) has any chance to unwind. If the target is not yet known at
    // this exact moment, we do NOT wait for it -- tick_redirects' existing
    // subject-only patch is the fallback, not this hook trying again later.
    uint64_t target_id = 0;
    if (!peek_known_target(target_id)) return;

    uint64_t current_id = 0;
    if (!choice_screen_subject_raw_id(self, current_id)) return;
    if (current_id == target_id) return;   // this call already got the right cat

    void* correct = catsync_resolve_by_id(target_id);
    if (!correct) return;   // tick_redirects will log the "not in this peer's run" case

    o_levelup_init(self, correct, functor, r9);
    log_line("SHOPMIRROR", "!! re-initialized a level-up screen for the correct cat"
                          " BEFORE its options were shown -- this purchase should now"
                          " match the buyer's exactly");
}

} // namespace

void shopmirror_set_base(uintptr_t base) {
    g.base = base;
    for (auto& t : g.tracked)         t = Tracked{};
    for (auto& p : g.pending)         p = Pending{};
    for (auto& w : g.recipient_watch) w = RecipientWatch{};
    for (auto& r : g.redirect)        r = Redirect{};
}

// Once per real FRAME (from FrameBegin) -- see shopmirror_on_button_update's
// own note on why this must not be driven from the per-button tick instead.
void shopmirror_frame_tick() {
    if (!tune::kShopMirror) return;
    tick_recipient_watches();
    tick_redirects();
}

// Once per button, per frame (h_ButtonUpdate fires for every button in the
// game). Deliberately does not drive the two ticks above -- there are dozens
// of buttons ticked per real frame, and a wall-clock check does not care,
// but running the same logic dozens of times a frame for no benefit is still
// wasted work. See shopmirror_frame_tick, called once from FrameBegin.
void shopmirror_on_button_update(void* self) {
    if (!tune::kShopMirror || !self) return;

    char name[64] = {};
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name))) return;
    if (name[0] == '\0' || !is_tracked_name(name)) return;

    char slot[8] = {};
    if (!read_slot(self, slot, sizeof(slot))) return;
    track(self, slot);

    try_pending();
}

bool shopmirror_on_button_click(void* self, bool force, fn_button_click original) {
    if (!tune::kShopMirror || !self) return false;

    char name[64] = {};
    if (!mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name))) return false;
    if (name[0] == '\0' || !is_tracked_name(name)) return false;

    // Identity read BEFORE the original runs: a purchase can remove the item
    // from the shop, and reading afterward risks a dangling or rebound
    // object.
    ShopBuyMsg m{};
    if (!read_slot(self, m.slot, sizeof(m.slot))) {
        log_line("SHOPMIRROR", "!! '%s' click has no readable slot key -- "
                              "not mirrored, letting it through unmirrored", name);
        original(self, force);
        return true;
    }
    read_item_name(self, m.name, sizeof(m.name));

    // Quarantined items are refused OUTRIGHT for EVERYONE, host included --
    // see kQuarantinedShopItems' own header note for the two real bugs found
    // (independent multi-level rolls, then a stale-coins race) and the
    // user's explicit call to stop chasing this class of item layer by
    // layer. The item just stays in the shop, unpurchased, for both peers.
    if (is_quarantined_item(m.name)) {
        log_line("SHOPMIRROR", "!! '%s' cannot be bought in co-op at all (known"
                              " random-recipient desync risk) -- purchase refused", m.name);
        return true;
    }

    void* screen_before = choice_current_level_screen();
    original(self, force);   // the real purchase happens locally, same as always

    log_line("SHOPMIRROR", "bought slot '%s' ('%s') -- publishing for the other peer(s)",
             m.slot, m.name[0] ? m.name : "?");
    net_send_shopbuy(m);   // ALWAYS immediate -- see mgmp_shopmirror.h's recipient note

    // Separately, watch for a level-up screen to announce its recipient --
    // this never delays the send above.
    for (auto& w : g.recipient_watch) {
        if (w.used) continue;
        w.used = true;
        w.screen_before = screen_before;
        w.deadline_at = GetTickCount64() + kWatchTimeoutMs;
        break;
    }
    return true;
}

void shopmirror_on_message(uint8_t from, const ShopBuyMsg& m) {
    if (!tune::kShopMirror) return;
    (void)from;

    for (auto& p : g.pending) {
        if (p.used) continue;
        p.used = true;
        p.msg = m;
        p.queued_at = GetTickCount64();
        try_pending();
        return;
    }
    log_line("SHOPMIRROR", "!! pending queue full -- dropping the mirror for slot '%s'", m.slot);
}

bool shopmirror_levelup_redirect_pending() {
    for (auto& r : g.redirect) if (r.used) return true;
    return false;
}

// PERMANENTLY DEAD, 2026-09-06 -- see kQuarantinedShopItems' own header note.
// The host-only-purchase path these two existed for was removed the same
// day it was added: quarantined items are refused for everyone now, so no
// screen from one of them can ever open on either peer. Kept as no-ops
// rather than deleted, because mgmp_choice.cpp's level_screen_decider and
// choice_on_level_select both call these unconditionally on every level-up
// screen -- removing the functions instead of just their effect would mean
// editing that ownership-decision logic for a case that can no longer
// happen, for no behavioural gain.
bool shopmirror_is_host_only_screen(void*) { return false; }
void shopmirror_notify_host_only_choice_committed() {}

void shopmirror_install_levelup_hook(uintptr_t base) {
    if (!tune::kShopMirror || !base) return;
    constexpr uintptr_t kRva_LevelUpInit = 0x37A290;
    void* addr = (void*)(base + kRva_LevelUpInit);

    MH_STATUS s = MH_CreateHook(addr, (void*)&h_levelup_init, (void**)&o_levelup_init);
    if (s != MH_OK) {
        log_line("SHOPMIRROR", "!! LevelUpScreen::init hook FAILED at %p (MH status %d) -- "
                              "the recipient fix stays subject-only, options may still"
                              " mismatch", addr, (int)s);
        return;
    }
    s = MH_EnableHook(addr);
    if (s != MH_OK) {
        log_line("SHOPMIRROR", "!! LevelUpScreen::init MH_EnableHook failed (%d)", (int)s);
        return;
    }
    log_line("SHOPMIRROR", "LevelUpScreen::init hook installed at %p", addr);
}

void shopmirror_on_levelup_target(uint8_t from, const LevelUpTargetMsg& m) {
    if (!tune::kShopMirror || !m.cat_id) return;
    (void)from;

    // Fill in whichever mirrored click is already waiting to be told the
    // recipient -- fire() armed it before this message could possibly have
    // arrived. Only falls back to a fresh entry if none is waiting (the
    // watch window already closed, or this message somehow arrived with no
    // matching fire at all) so the announcement is never silently dropped.
    for (auto& r : g.redirect)
        if (r.used && !r.have_target) {
            r.have_target   = true;
            r.target_cat_id = m.cat_id;
            return;
        }

    for (auto& r : g.redirect) {
        if (r.used) continue;
        r.used          = true;
        r.have_target   = true;
        r.target_cat_id = m.cat_id;
        r.deadline_at   = GetTickCount64() + kWatchTimeoutMs;
        return;
    }
    log_line("SHOPMIRROR", "!! redirect queue full -- dropping the level-up target for cat %016llx",
             (unsigned long long)m.cat_id);
}

} // namespace mgmp
