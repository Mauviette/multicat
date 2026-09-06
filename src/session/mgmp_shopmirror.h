#pragma once
// mgmp_shopmirror -- F3.1's shop-purchase mirror (cahier-des-charges-mgmp-
// fork.md): "dès qu'un joueur achète un objet, le jeu de l'autre l'achète
// automatiquement aussi en simulant un clic."
//
// WHY THIS EXISTS, MEASURED LIVE. A rare-candy purchase opened a LevelUpScreen
// for the buyer only; the other peer, having clicked nothing, had no
// LevelUpScreen for mgmp_choice's own sync to act on at all -- softlock. A
// purchase's EFFECT already rides the existing host-authoritative CatData/
// Inventory sync eventually, but that is not enough when the purchase also
// opens a SCREEN the other peer needs to be standing on. Mirroring the click
// itself is what makes the peer's own game walk the same path (Shop::
// ObtainItem -> possibly a LevelUpScreen) instead of just updating state in
// the background.
//
// THE IDENTITY PROBLEM AND HOW IT WAS SOLVED. Unlike Event_ExitButton/
// Shop_ExitButton (mgmp_screensync.h), there can be several Shop_BuyButton
// instances on screen at once, one per item, and "which one" needs an
// identity that survives the trip to the other peer. Found live, 2026-09-06,
// by dumping a real Shop_BuyButton object's memory:
//
//   Button+0xD0   a short std::string, e.g. "i1", "i3", "i4" -- almost
//                 certainly the shop's own internal per-slot key (seen with
//                 "i2" ALREADY MISSING from a shop where item 2 had been
//                 bought, i.e. it survives purchases elsewhere in the same
//                 shop rather than being a position that shifts).
//   Button+0x1B8  a pointer to a UTF-16 buffer holding the item's display
//                 name ("Bonbon rare", "Sels d'ammoniaque", "Crucifix" were
//                 read live, matching what was actually on screen).
//
// RESOLVE BY THE SLOT KEY, VALIDATE BY THE NAME -- the fourth time this
// protocol has settled on that exact split (see ChoiceMsg's header note in
// mgmp_proto.h). NOT YET CROSS-PEER VERIFIED that the key is assigned in the
// same order on both peers (only one peer's memory has been dumped so far);
// the name cross-check exists specifically to catch that assumption being
// wrong rather than trust it silently.
//
// ChestButton_alley used to be included in the same button-name list, on the
// assumption its layout matches Shop_BuyButton's -- WRONG, confirmed live
// 2026-09-06 (reported as "chest open doesn't reach the other peer"): a
// chest is a single button, not an itemized list, and never carries the
// per-slot key this mirror resolves identity by, so `read_slot` failed and
// the click fell through unmirrored every time. Moved to mgmp_screensync's
// `kVoteChestOpen` instead, which needs no per-item identity at all -- see
// mgmp_screensync.h.
//
// SHARES THE ONE Button::Click HOOK mgmp_mainmenulock installs, same as
// mgmp_screensync -- see that header's note on why a second MinHook on the
// same address is not an option, and why firing the mirrored click reuses
// mainmenulock_begin_injected_click/_end_injected_click.
//
// THE RECIPIENT PROBLEM, AND WHY IT IS A SEPARATE MESSAGE (LevelUpTargetMsg
// in mgmp_proto.h). Measured live, 2026-09-06: a rare-candy purchase, once
// mirrored, opened a LevelUpScreen for a DIFFERENT cat on each peer. The
// candy's recipient is picked by an internal random roll over "cats at the
// lowest level," and mirroring the BUY click makes both peers roll
// INDEPENDENTLY -- exactly the "replicate the choice, not the effect" trap
// this project's protocol has hit three times before (see ChoiceMsg's own
// header note in mgmp_proto.h). A first fix tried folding the recipient
// straight into ShopBuyMsg and delaying that whole message until a
// resulting level-up screen was seen or a watch window elapsed -- and that
// stalled EVERY ordinary purchase (the common case, no level-up at all) by
// the length of the watch window, since there is no fast way to know in
// advance whether a level-up is coming. LevelUpTargetMsg is fully
// independent instead: the plain purchase mirror goes out immediately as
// before, and a SEPARATE message follows later, only if/when a level-up
// screen actually appears -- the receiving peer applies it to whatever
// level-up screen it finds open (its own mirrored purchase's result, in
// practice) the moment the two are both known, in whichever order they
// arrive.
#include <cstdint>

namespace mgmp {

struct ShopBuyMsg;
struct LevelUpTargetMsg;

void shopmirror_set_base(uintptr_t base);

// Called every frame from h_ButtonUpdate for every button -- keeps a live
// {slot key -> button pointer} cache for Shop_BuyButton instances currently
// on screen, which is what lets a ShopBuyMsg (arriving
// either before or after this peer's own screen has finished ticking this
// frame) resolve to a real button to click. Also retries any ShopBuyMsg that
// arrived before its target button was seen.
void shopmirror_on_button_update(void* self);

// Call once per frame from FrameBegin, NOT from the per-button tick above --
// this is what actually paces the level-up-recipient watch/redirect windows
// in real frames rather than in "however many buttons exist" units.
void shopmirror_frame_tick();

// Called from the shared Button::Click hook, BEFORE the original would run.
// If `self` is a real (non-injected) human click on a tracked button, this
// captures its identity, calls `original(self, force)` itself, publishes a
// ShopBuyMsg, and returns true -- the caller must not call the original
// again and must not run any other button-click logic for this call. Returns
// false for everything else.
typedef void (__fastcall* fn_button_click)(void* self, bool force);
bool shopmirror_on_button_click(void* self, bool force, fn_button_click original);

void shopmirror_on_message(uint8_t from, const ShopBuyMsg& m);

// The independent level-up-recipient correction (LevelUpTargetMsg) -- see
// that struct's own header note in mgmp_proto.h for why this is fully
// decoupled from the purchase mirror above. Queues the target; applied by
// shopmirror_frame_tick as soon as a level-up screen (this peer's own, from
// whatever triggered it) is seen with a different subject.
void shopmirror_on_levelup_target(uint8_t from, const LevelUpTargetMsg& m);

// True while ANY Redirect is still in flight on THIS peer -- i.e. a
// LevelUpTargetMsg has arrived (or a local click is being watched) and the
// correction has not yet been confirmed to match. mgmp_choice checks this to
// avoid a real deadlock, reported live 2026-09-06: the redirect fixes WHICH
// CAT a choice applies to, but not instantly, and not what the screen
// visually shows in the meantime -- while it is wrong, this peer's own
// ownership check can independently conclude "not my cat" for a DIFFERENT
// reason than the other peer does, and both sides wait for each other
// forever. Swallowing every click (and holding every incoming choice)
// while this is true removes the window entirely; it costs at most the
// watch timeout, not a stuck run.
bool shopmirror_levelup_redirect_pending();

// PERMANENTLY RETURN FALSE / DO NOTHING, 2026-09-06 -- kQuarantinedShopItems
// used to be host-only-purchasable (never mirrored; the result reached the
// client through ordinary cat-state sync instead), which needed these two
// so mgmp_choice.cpp's level_screen_decider/choice_on_level_select could
// special-case that one screen. REMOVED THE SAME DAY: a second real bug
// (the purchase's coin cost went stale on the client) made the user call it
// too fragile to keep patching, and quarantined items are now refused for
// EVERYONE instead -- see kQuarantinedShopItems' own header note in the
// .cpp. No screen from one of these items can exist on either peer any
// more, so both functions are no-ops. Kept, not deleted, purely so
// mgmp_choice.cpp's two unconditional call sites need no changes.
bool shopmirror_is_host_only_screen(void* screen);
void shopmirror_notify_host_only_choice_committed();

// Raw hook on glaiel::LevelUpScreen::init (RVA 0x37A290, found live 2026-
//09-06 -- no signature generated, same accepted-risk shape as
// mgmp_invlock's InventoryItemBox::click hook). Call once at startup.
//
// Confirmed by its own write: [this+0xA0]=subject immediately followed by
// subject->[+0xC30]++ -- the exact two fields CLAUDE.md already documents as
// the level-up option roller's seed inputs. This means the options are
// rolled ENTIRELY INSIDE init, from whatever subject its caller passes --
// patching LevelUpScreen+160 afterward (the original fix) corrects which
// cat a choice applies to, but not the stale, wrong-cat options that were
// already rolled by the time that patch can run. Measured live: the two
// peers offered DIFFERENT boons at the same index ('Strength' vs
// 'Intelligence') and each applied its own -- a guaranteed stat divergence,
// not just a display glitch.
//
// THE FIX, and why it is safe: if the correct recipient is ALREADY known
// (mgmp_shopmirror's own redirect state) at the moment init first runs, call
// the ORIGINAL init a SECOND TIME, immediately after the first, with the
// corrected subject -- reusing the SAME functor pointer both calls, which is
// only safe because both happen synchronously inside this one hook
// invocation, before the original caller's stack frame (which owns that
// functor) has any chance to unwind. A version that DEFERRED the call
// instead (to cover the case where the target is not yet known) would need
// that pointer to outlive its owning stack frame -- rejected as a real
// crash risk, not just a data one; see mgmp_shopmirror.cpp's own note.
//
// Falls back to doing nothing extra (today's after-the-fact +0x160 patch
// remains the only correction) whenever the target is not yet known at
// this exact moment -- an accepted, honestly-documented partial fix rather
// than a riskier attempt at a complete one.
void shopmirror_install_levelup_hook(uintptr_t base);

} // namespace mgmp
