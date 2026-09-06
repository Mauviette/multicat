// mgmp_tuning.h -- the settings that are NOT in mgmp.json.
//
// Every constant here used to be a config key. They were removed from the file
// because a knob nobody turns is not a feature: it costs a parser branch, a
// line of documentation, a field in Config, and -- worst of the four -- it
// makes the config file long enough that the keys that DO matter stop being
// visible in it.
//
// The test for staying in mgmp.json was "has this ever actually been changed",
// and the evidence accepted was a flag in tools/net_test.ps1: role, address,
// port, cat split, the four diagnostic switches that script can flip, the
// recorder, and the panel. Nothing else passed, so nothing else is in the file.
//
// Changing one of these means editing this header and rebuilding, which for a
// mod that is rebuilt every session is the same gesture as editing a config
// file -- and it puts the switch next to the code it governs instead of in a
// file the code has to be trusted to read correctly.
#pragma once

#include <cstdint>

namespace mgmp {
namespace tune {

// --- the phase-1 text trace -------------------------------------------------

// Open a console window alongside the log file. OFF: everything it ever showed
// is now readable somewhere better -- the stamped file log keeps the whole run,
// the ImGui panel shows it live and filtered, and the failures that happen
// before the logger exists go to mgmp_boot.log, which the console never saw
// either. All it added was a second window to alt-tab past.
constexpr bool     kConsole      = false;
// Print raw pointers in trace lines. They differ between processes, so two
// peers' logs only diff cleanly with this off -- which is why it existed. In
// practice the diffing is done on the tagged lines, not the whole log.
constexpr bool     kPointers     = true;
// Bytes of TurnAction to hexdump when kTaRaw asks for one.
constexpr uint32_t kTaDump       = 64;
constexpr bool     kTaRaw        = false;
// Log every Brain::GetChoice poll, not just the ones that decided something.
// 1695 of 1711 calls in one battle return "nothing decided".
constexpr bool     kChoiceAll    = false;
// Frames between FRAME lines; 0 silences them. Effectively always 0 already:
// the frame hook is forced on by net_role and by the panel, and both silenced
// the logging when they did it, because they wanted the socket pumped and not
// a line every 60 frames.
constexpr uint32_t kFrameLogEvery = 0;

// --- phase 2: the recorder --------------------------------------------------

// Record draws from every RNG stream, not just the simulation one. On is the
// wrong setting and always was: the non-global draws turned out to be the
// interesting ones (they are how TLS+0x198 was found at all), and the volume
// that justified filtering never materialised.
constexpr bool     kRngGlobalOnly = false;
// Put EV_FRAME records in the stream. Only run D needed them -- a starved run
// has fewer frames by construction, so frames are excluded from the diff.
constexpr bool     kRecordFrames  = false;

// Which brains a decision may be injected into, and -- the load-bearing use --
// which cats count as HUMAN when the control split is derived. Substring match
// against the live RTTI class name.
//
// This is the one culled setting with a real consequence: it decides which cats
// the two peers divide between them. It is a constant because there has never
// been a second answer. If a summonable cat ever turns up with a PlayerBrain,
// this is where the exception goes -- see the summon note in CLAUDE.md.
constexpr const char* kReplayBrains = "PlayerBrain";

// --- the two hooks that CHANGE the game rather than watch it ----------------
//
// Both still announce themselves loudly in the startup banner while on, which
// is the property that mattered -- not that they were reachable from a file.

// No-op MewDirector::ApplySaveScumPenalty, so that reloading the same save to
// repeat a battle does not mutate cat state as a function of the reload count.
// This is a capture-methodology switch, not a cheat: with it live, run N and
// run N+1 start from different states by construction.
// ON: a multiplayer test session relaunches the game constantly for reasons
// that have nothing to do with the run -- a rebuilt DLL, a peer that dropped,
// a deliberate reconnect -- and Steven counts every one of them. The penalty
// was measuring our tooling, not the player.
constexpr bool     kHookSaveScum  = true;
// Convert TimeDelayStatusApplication's wall-clock countdown to a turn count.
// Never yet run against real content -- the one reachable user is AZ_LoseHead.
constexpr bool     kHookTimeDelay = false;
// Turn boundaries such a status waits before firing. 1 = the next boundary.
constexpr uint32_t kTimeDelayTurns = 1;

// --- the meta layer's sync modules ------------------------------------------
//
// Each of these was a switch whose documented purpose was "turn it off to tell
// a broken push apart from a broken assumption". That is a real debugging move
// and it is still available -- by editing the line, which is a rebuild rather
// than a relaunch. What it is NOT any more is something a stale config file in
// one peer's directory can silently disagree with the other peer about, and
// every one of them is a setting where the two peers disagreeing is worse than
// either value.

constexpr bool     kCatSync   = true;   // push the host's cats per map node
constexpr bool     kInvSync   = true;   // push the run inventory per map node
constexpr bool     kChoice    = true;   // replicate event / level-up decisions
constexpr bool     kRunHist   = true;   // push *(MewDirector+1424), the used-event list
constexpr bool     kNodeHash  = true;   // the meta layer's per-node hash

// Whether a NODEHASH mismatch is fatal. Off, unlike the battle layer's halt:
// a battle desync makes every later turn fiction, a meta divergence is usually
// one number and a run that still plays.
constexpr bool     kNodeHashHalt = false;

// --- the battle layer's detection -------------------------------------------

// Include character HP/shield/tile in the per-turn hash. Off leaves the RNG
// stream and the queue. The hash gates itself on evidence anyway.
constexpr bool     kStateHash  = true;
// One line per cat whose state changed, at every turn boundary. This is the
// only thing that answers "when did the field last move, and by how much",
// which is the question a two-HP difference actually turns on.
constexpr bool     kStateTrace = true;

// --- peer cursors -----------------------------------------------------------
//
// Presentation. Nothing here is hashed, sent at a command boundary, or able to
// reach a decision, so these are the safest constants in the file.

constexpr bool     kCursors        = true;  // the board reticle
constexpr bool     kCursorGl       = true;  // the screen-space pointer
// One magenta arrow at screen centre with no peer and no session, to tell "our
// GL is broken" apart from "no peer position arrived". Both look like no arrow.
constexpr bool     kCursorGlTest   = false;
// Bounded trace of the pointer fraction at both ends, re-armed on every resize.
constexpr bool     kCursorTrace    = false;
// Lowered from 100, 2026-09-05: reported live as a bit too visible/intense
// on the peer's own turn -- it is a hover indicator, not meant to compete
// with the board for attention.
constexpr uint32_t kCursorAlpha    = 65;    // the peer whose cat is deciding
constexpr uint32_t kCursorAlphaDim = 30;    // everybody else
// Ink height in pixels at kCursorRefH, scaled by the content rectangle so a
// bigger window draws a bigger cursor the way the game's own does.
constexpr uint32_t kCursorPx       = 34;
constexpr uint32_t kCursorRefH     = 720;

// F4.2: "look here" -- cursor.ping_key (mgmp_config.h) enlarges the pressing
// peer's cursor on every screen, including their own, for exactly as long as
// the key is held (CursorPingMsg.active mirrors the physical key state, no
// timeout). Scale is a multiple of the ordinary kCursorPx size; alpha is
// forced to full regardless of whose turn it is, since the whole point is to
// be seen NOW.
constexpr bool     kCursorPing      = true;
constexpr float    kCursorPingScale = 1.8f;

// --- text banners --------------------------------------------------------

// F4 (cahier-des-charges-mgmp-fork.md): "It's your turn!" across the top of
// the screen while lockstep_local_actor() is true AND a battle board is
// genuinely up right now (cursor_recently_on_board() -- see mgmp_cursor.h;
// lockstep_local_actor() alone stays true well past a battle's end, since
// nothing resets it until the NEXT battle's first poll). GDI-rendered to a
// texture once and drawn through the same screen-space GL path as the peer
// pointer. See mgmp_overlay.cpp's build_banner_texture/draw_banner.
constexpr bool     kTurnBanner          = true;
// On-screen height in pixels at kCursorRefH, scaled by the content rectangle
// exactly like kCursorPx -- see draw_cursor's own comment for why a fixed
// reference height rather than a fixed pixel size.
constexpr uint32_t kBannerPx            = 30;
constexpr uint32_t kBannerTopMarginPx   = 22;

// F6/F4: "It's your cat!" / "Partner's cat!" across the top of the screen
// while the level-up screen's subject cat has a decided owner -- whichever
// of the two a peer sees depends on whether IT owns that cat. Replaced the
// original top-right coloured badge entirely (user's own call, 2026-09-05):
// a short message reads the same way the turn banner already does, and
// needed no separate visual language of its own. See
// mgmp_choice.h's choice_level_screen_owner.
constexpr bool     kCatOwnershipBanner  = true;

// DRAW THE PEER'S POINTER AS THE CURSOR THEIR GAME IS SHOWING, rather than
// always as the plain arrow. OFF while the reported drift is being isolated.
//
// The report (2026-08-28) was that the peer pointer is exact until the player
// starts AIMING, at which point it moves. `mode` is the only thing in
// draw_cursor that changes at that moment, so it is the only candidate, and the
// shipped art says why it could move anything at all:
//
//   state       ink box              ink h    vs default
//   default     ( 29, 2)-(108,108)   106      --
//   move        ( 29, 2)-(107,120)   118      x1.113
//   spell       ( 29, 2)-(111,123)   121      x1.142
//   attack      ( 29, 2)-(106,128)   126      x1.189
//
// Every aiming state shares default's ink ORIGIN and is 11-19% TALLER, because
// the badge hangs below the arrow. The sizing divided by that height, so the
// glyph changed size the moment the state changed -- measured from the shipped
// PNGs, not inferred.
//
// That defect is fixed independently (kCursorInkRefH below): every state is
// scaled by `default`'s ink height, so the glyph is the same size and sits in
// the same place whichever cursor the peer is showing. The one candidate the
// report had is therefore gone, and this is back ON -- while it was off, `mode`
// was read, published, transmitted and stored, and then thrown away one line
// into draw_cursor. A field that survives the whole pipeline and dies at the
// draw call looks exactly like a replication failure from every log line.
constexpr bool     kPeerCursorArt  = true;

// Sizing reference: `default`'s ink height, measured above. EVERY state is now
// scaled by this rather than by its own ink box, so which cursor a peer is
// showing can never change how big their pointer is or where it sits. The badge
// on an aiming cursor simply hangs below the arrow, which is what it does in
// the game.
constexpr float    kCursorInkRefH  = 106.0f;
// THE GAME'S FIXED CONTENT ASPECT, and the letterbox rectangle is derived from
// it rather than from the GL viewport.
//
// The viewport was the original source and it is not trustworthy: whether the
// value observed at swap time belongs to the game's fixed-aspect offscreen pass
// or to its full-window composite depends on which pass happened to be bound
// last. When it is the composite, the "content rectangle" collapses to the whole
// window and the letterbox correction silently disappears -- which is invisible
// while both peers run the SAME window size, because both then measure the
// pointer against the same wrong rectangle and the error cancels exactly. It
// only shows when the two aspects differ, and it shows as the peer pointer
// gaining speed on the short axis and straying into the black bars. Reported
// from the wild 2026-08-28, with "same resolution everything is good" as the
// tell.
//
// 16:9, from TWO independent readings. The shipped `swfs/ui.swf` declares a
// stage of 25600x14400 twips = 1280x720 exactly, which is what the whole UI is
// authored against; and the one recorded runtime measurement agrees -- a
// 958x1120 window rendered content 958x539 (958 * 9 / 16 = 538.9) with
// 290-pixel bars top and bottom ((1120 - 539) / 2 = 290.5).
//
// Derived arithmetic is identical on both peers and does not depend on GL state,
// which is exactly the property the viewport lacked. The binary confirms why it
// lacked it: the game's own cursor pass (sub_140A16B00 @ 0x140A16E05 and
// 0x140A17069) calls SDL_GetWindowSizeInPixels and then
// glViewport(0, 0, whole drawable) before drawing, so on any frame that pass
// runs last the viewport we observe at swap time is the full window.
//
// It is cross-checked rather than trusted: the overlay logs the game's own
// viewport beside the derived rectangle, so a wrong aspect is one line away
// from being visible instead of being a slow drift nobody can name.
constexpr int      kContentAspectW = 16;
constexpr int      kContentAspectH = 9;

// Exponential smoothing time constant. CURSOR is throttled to ~20 messages a
// second, so drawn raw the arrow reads as a peer with an unsteady hand. 60 ms
// is roughly one throttle interval.
constexpr uint32_t kCursorSmoothMs = 60;

// --- the combat menu lock ---------------------------------------------------

// Grey the ability bar out while it belongs to a cat a PEER controls, using the
// game's own disabled button state. Presentation only: it writes one int on a
// UI object after the game has computed it, and the clicks it stops were already
// being discarded at Brain::GetChoice. See mgmp_combatlock.h.
//
// Here rather than in mgmp.json by the same test as everything else in this
// file: there is no experiment that wants it off. The reason it is a constant at
// all is that it costs two hooks, one of them on Button::update -- so turning it
// off has to remove the hooks, not just the effect, which is why the config
// layer reads this rather than the module doing it.
constexpr bool     kCombatLock = true;

// --- the peer's aim preview -------------------------------------------------

// Draw the range / AOE tiles the OTHER player is currently aiming at, using the
// game's own Brain::DrawAbilityAOE. Presentation, and in the strictest sense:
// the call is made with the simulation stream saved and restored around it, so
// it cannot move the sim even if the fence pass missed a site. See mgmp_aim.h.
//
// A constant rather than a config key by the usual test: there is no experiment
// that wants it off. It is read once at init, so turning it off removes the
// publish and the draw, not just the effect.
constexpr bool     kAimPreview = true;

// --- the debug panel --------------------------------------------------------

// Lines the log pane keeps for scrollback. The ring behind it holds 4096.
constexpr uint32_t kUiLogLines = 2000;

// F6 (cahier-des-charges-mgmp-fork.md): the host manages the run's
// equipment; a client cannot equip/unequip anything. See mgmp_invlock.h.
constexpr bool     kInvLock = true;

// F5: frames to wait after the host's last equip/unequip click before
// publishing, so the snapshot is taken after the game's own mutation (bag
// bucket + the cat's equipped-slot field, two separate writes) has settled
// rather than mid-transition. Found live 2026-09-05: publishing on the SAME
// frame as the click produced duplicated/vanishing items on the client. 30
// frames is roughly half a second at 60 FPS -- generous on purpose, since
// the cost of waiting longer is only latency, while the cost of not waiting
// long enough is a corrupted-looking client inventory.
constexpr uint32_t kInvSyncDebounceFrames = 30;

// --- the main menu lock ------------------------------------------------------

// Cahier des charges: the client never presses Play itself -- the host owns
// the run and the mod presses Play FOR the client once the host's save
// arrives (see mgmp_savefile's "PRESS PLAY FOR A CLIENT" path).
//
// VERSION 2, back ON. Version 1 forced Button+752 to kBtnState_Disabled and
// broke the client's connection entirely, live -- the assumption that
// Button::Click's real guards never check that state was never verified and
// was almost certainly wrong. This version hooks glaiel::Button::Click
// directly (the same already signature-resolved address mgmp_savefile
// already calls it through, no new signature needed) and swallows only a
// human click on the named button, with the mod's own scripted press let
// through via an explicit `injecting` flag -- see mgmp_mainmenulock.h for
// why that flag is structurally necessary, not just a safety margin. Needs
// a live test before being trusted the way the rest of this file is.
constexpr bool     kMainMenuLock = true;

// --- the turn-owner badge -----------------------------------------------

// F4 (cahier-des-charges-mgmp-fork.md): mark the acting cat's tile with its
// owning player's colour during a human turn, computed identically and
// locally on both peers -- no wire message. See mgmp_turnbadge.h. Same test
// as kCombatLock/kMainMenuLock for living here: no experiment wants it off,
// and turning it off should skip the resolve and the draw call, not just
// leave the badge invisible.
constexpr bool     kTurnBadge = true;

// --- the client's map marker walk (EXPERIMENTAL) ----------------------------

// Cahier des charges F3.2: the client's little team should visibly WALK to
// the node the host selected, not teleport there.
//
// HISTORY, for anyone tempted to re-derive this from scratch: a first attempt
// (2026-09-05) called glaiel::MapNode::Click directly at RVA 0x227C90 with
// just the MapNode* as an argument, and crashed the client (C0000005, 0x163
// bytes into the callee). Re-investigated 2026-09-06 by statically
// disassembling the shipped PE (capstone, no IDA, no game running) and
// cross-checking against .pdata's function table -- ground truth for x64
// function boundaries with no IDA needed. Two things came out of that:
//
//   1. RVA 0x227C90 was NEVER a function start. It sits 0x10F0 bytes INSIDE
//      an unrelated 0x124C-byte function -- a plain reverse-engineering
//      error from whenever that address was first pinned, not a build-drift
//      issue. This is why the crash looked like "the call landed but the
//      body faulted": the trampoline was executing the MIDDLE of a totally
//      different function's bytes.
//   2. The REAL glaiel::MapNode::Click is at RVA 0x228700 (confirmed:
//      contains exactly CLAUDE.md's documented write, `[[this+0x170]+0xA0]
//      +0x60 = this`, guarded by a `type==4` "home node" branch that instead
//      builds a confirmation popup). Its "this" is NOT the MapNode -- the
//      real function does `mov rsi, [rcx+8]` first and uses rsi as the
//      MapNode from then on, so `this` is some UI wrapper whose +8 holds the
//      MapNode*. That mismatch (this=node instead of this=wrapper) is what
//      actually crashed the first attempt.
//
// Rather than find that wrapper object and its full ABI, mgmp_follow now
// sidesteps calling the function at all: it performs the one confirmed write
// directly (glaiel::MapNode::Click's own common-path tail, replicated by
// mem_write, not called into), then WAITS -- polling the marker's own +0x50
// "where it currently is" slot every tick -- for the marker to actually
// finish walking before entering the node, instead of guessing a delay. The
// `type==4` branch is deliberately not replicated (unverified extra popup
// logic); selecting a home node falls back to immediate teleport, same as
// today. See mgmp_follow.cpp's select_node_for_walk/marker_at_node.
//
// ON, 2026-09-06: live-testing the new mechanism (select via mem_write, wait
// for marker_at_node, then enter). Flip back to false if this needs reverting.
constexpr bool     kFollowMarkerWalk = true;

// F3.1 (cahier-des-charges-mgmp-fork.md): the screen-exit vote barrier on
// Event_ExitButton and Shop_ExitButton (the latter shared with the treasure-
// chest screen -- see mgmp_screensync.h). ON by default like every other
// live-tested QoL flag in this file; flip off only to isolate whether a
// stuck screen is this feature's doing.
constexpr bool     kScreenSync = true;

// Where the "X/N" vote indicator sits, as a shift from draw_banner's default
// dead-centre-top anchor (see mgmp_overlay.cpp's draw_banner header note --
// positive x is right, positive y is down, both as a fraction of the
// screen). Approximate, not yet tuned against a live screenshot: nudge these
// if the text sits off the bottom-right corner rather than in it.
constexpr float    kScreenVoteXShift = 0.40f;
constexpr float    kScreenVoteYShift = 0.85f;

// F3.1: the shop/chest purchase mirror -- see mgmp_shopmirror.h. Newer and
// less proven than kScreenSync (the item-identity scheme rests on a single
// live memory dump, not yet cross-peer verified); kept as its own flag so it
// can be switched off independently while kScreenSync stays on.
constexpr bool     kShopMirror = true;

} // namespace tune
} // namespace mgmp
