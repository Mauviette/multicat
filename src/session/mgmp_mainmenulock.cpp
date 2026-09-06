// mgmp_mainmenulock.cpp -- see mgmp_mainmenulock.h for the design, and why
// this is version 2 (a direct Button::Click hook) rather than the original
// forced-disabled-state approach that broke the client's connection.
#include "mgmp_mainmenulock.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_screensync.h"   // F3.1 -- shares this one Button::Click hook
#include "mgmp_shopmirror.h"   // F3.1 -- same reason
#include "mgmp_follow.h"       // client map-node click block -- same reason
#include "mgmp_invlock.h"      // F6 ROUND 5 -- the preventive equip-click block
#include "mgmp_leave.h"        // client abandon-run click block + name discovery
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "MinHook.h"

#include <cstring>

namespace mgmp {
namespace {

typedef void (__fastcall* fn_button_click)(void* self, bool force);

fn_button_click o_click = nullptr;

struct State {
    // See the header note: set around mgmp_savefile's own scripted press so
    // THIS hook -- which patches Button::Click itself, not a call site --
    // does not swallow the mod's own click along with a human's.
    bool injecting = false;
    bool said      = false;   // log the first real swallow, not one per frame
};
State g;

void __fastcall h_ButtonClick(void* self, bool force) {
    if (g.injecting) {
        o_click(self, force);
        return;
    }

    // F3.1: vote-gated screen-exit buttons, checked first and independent of
    // kMainMenuLock/role -- see mgmp_screensync.h for why this shares this
    // one hook rather than installing a second one on the same address.
    // Fully owns the click (swallow while waiting, or fire it via this same
    // `injecting` guard once the vote completes) whenever it matches.
    if (screensync_on_button_click(self)) return;

    // F3.1: the shop/chest purchase mirror -- same sharing reason. Lets the
    // original run itself (a purchase is never swallowed, only mirrored), so
    // this must come before the unconditional o_click below, not instead of
    // it.
    if (shopmirror_on_button_click(self, force, o_click)) return;

    // Client map-node click block -- same sharing reason. See
    // mgmp_follow.h's own note: the host is authoritative over map
    // navigation, and this stops a client's click at the source instead of
    // only refusing it later at EnterNode.
    if (follow_on_button_click(self)) return;

    // F6 ROUND 5 -- same sharing reason. Swallows a client's click on an
    // equip/unequip button before the game ever sees it; see mgmp_invlock.h.
    if (invlock_on_button_click(self)) return;

    // Same sharing reason. Blocks a client from abandoning the adventure
    // through their own pause menu -- see mgmp_leave.h.
    if (leave_on_client_button_click(self)) return;

    if (!tune::kMainMenuLock || net_role() != NetRole::Client) {
        o_click(self, force);
        return;
    }

    char name[64];
    if (mem_read_std_string((const uint8_t*)self + kBtn_Name, name, sizeof(name)) &&
        strcmp(name, kBtnName_MainMenuPlay) == 0) {
        if (!g.said) {
            g.said = true;
            log_line("MENULOCK", "swallowed a human click on '%s' -- this peer is a"
                                 " client; the host's save arrives over the wire and"
                                 " presses this for you", name);
        }
        return;   // do NOT call the original -- this is the real block
    }
    o_click(self, force);
}

} // namespace

void mainmenulock_install_click_guard() {
    if (!tune::kMainMenuLock) return;

    const uintptr_t addr = addr_of_call(C_ButtonClick);
    if (!addr) {
        log_line("MENULOCK", "!! %s did not resolve -- the client's Play button"
                             " is not locked", kCalls[C_ButtonClick].name);
        return;
    }

    MH_STATUS s = MH_CreateHook((void*)addr, (void*)&h_ButtonClick, (void**)&o_click);
    if (s != MH_OK) {
        log_line("MENULOCK", "Button::Click hook FAILED at %p (MH status %d) -- the"
                             " client's Play button is not locked", (void*)addr, (int)s);
        return;
    }
    s = MH_EnableHook((void*)addr);
    if (s != MH_OK) {
        log_line("MENULOCK", "Button::Click MH_EnableHook failed (%d)", (int)s);
        return;
    }
    log_line("MENULOCK", "Button::Click guard installed at %p -- a client cannot press"
                         " Play itself", (void*)addr);
}

void mainmenulock_begin_injected_click() { g.injecting = true; }
void mainmenulock_end_injected_click()   { g.injecting = false; }

} // namespace mgmp
