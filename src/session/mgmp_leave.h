#pragma once
// mgmp_leave -- the host left the run, so the client should not still be in it.
//
// THE GAP THIS CLOSES. Everything the host does INSIDE a run announces itself:
// ENTERNODE at each node, CHOICE at each decision screen, CATDATA / INVENTORY /
// RUNHIST at each boundary, ACTION and HASH at each turn. Leaving the run
// announces nothing -- the host just stops sending, which looks exactly like a
// host who is thinking, reading a tooltip, or three turns into a long battle.
//
// So a host who goes back to the house, abandons the run, or quits out to the
// title screen leaves the client sitting inside a run nobody else is playing,
// and neither log says a word about it. That is the same class of failure as
// the stranded-on-the-main-menu report of 2026-08-28, arrived at from the other
// direction.
//
// --- how "in a run" is decided ----------------------------------------------
//
// From TWO readings, and it takes both. The game's own loaded-scene list says
// OUT when House, MainMenu or SaveSelectionScreen is loaded and not marked for
// destruction (see the kScene_* block in mgmp_addresses.h); savefile's cat-id
// test says whether a run is actually loaded. A peer is IN a run only when the
// scene list does not say OUT *and* the cat-id list says a run exists.
// Anything else is Unknown.
//
// THE POSITIVE HALF IS NOT BELT AND BRACES. Absence of an out-scene is a
// reading nothing has to be true for, and at startup nothing is: the game boots
// through Shared / Base / Tutorial / Cutscene with no MainMenu scene yet, so
// the absence test alone said "in a run", and the moment MainMenu loaded the
// host announced a departure that never happened and took the client out with
// it. Measured 2026-08-28 before a save had even been picked.
//
// The asymmetry is deliberate and survives that correction: Unknown neither
// arms the announcement nor triggers it, so a transient gap during a real
// transition can only ever suppress, never manufacture. Two consecutive OUT
// confirmations are required on top, which is a second of wall clock at the
// poll rate here and costs nothing because leaving a run is not a decision
// anybody takes twice a second.
//
// --- what the client does about it -------------------------------------
//
// It presses Quit To Menu for the player, through Button::Click, from the same
// Button::update hook that already presses Play -- the proven route, and the
// one that goes through the game's fade and the game's own guards rather than
// synthesising a scene transition over a live battle.
//
// THE PAUSE MENU HAS TO BE OPEN for that button to exist at all --
// Button_PauseMenu_QuitToMenu does not exist until PauseMenu::init has run and
// SetupMainSidebar has built the sidebar, and PauseMenu::init only runs when a
// person pauses. There is no persistent PauseMenu component to reach into and
// no update override to hook (its slots 6..15 are the ICF-folded empty
// virtual, the same dead end MainMenu had), so this module cannot call its way
// in the way it calls Button::Click.
//
// IT OPENS THE MENU ITSELF INSTEAD, by posting a synthetic Escape keypress to
// the game's own window (see ui_game_window) through the same WndProc the
// debug panel already subclasses -- the game processes it exactly as it would
// a real press, because as far as its input layer is concerned it is one.
// This was originally "press Escape and the mod does the rest", which worked
// but silently did nothing for a player who never saw that log line; a
// synthesized press has no such dependency. It stops the moment the sidebar is
// seen (Escape TOGGLES the menu -- a second one while it is already open would
// close it right as the click is about to land) and gives up loudly, the same
// shape as the click retry budget, if the menu never opens at all. Dragging
// someone out of a run by building the scene transition ourselves, instead of
// going through a real pause-and-quit, would risk the battle layer's
// Character* pointers into whatever is being torn down; that is a worse
// failure than the one being fixed, so this still goes through the game's own
// fade and guards -- it just no longer needs a finger on the key.
//
// A client that is ALREADY out of a run when the message arrives does nothing
// and says so at Trace. That is the common case when the host is merely
// re-picking a save slot, and it is not a problem to be reported.

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct HostLeftMsg;

void leave_init();
void leave_shutdown();
void leave_set_base(uintptr_t base);

// Every frame, from the session's Ready tick. Polls the scene list on a divisor
// (see kPollFrames) rather than every frame: three guarded reads per loaded
// scene is cheap but not free, and nothing here is urgent to the millisecond.
//
// Runs on BOTH roles. The host watches for its own departure; the client uses
// the same poll to notice that it has arrived back on the menu, so the "still
// trying to leave" state cannot outlive the leaving.
void leave_pump();

void leave_on_message(const HostLeftMsg& m);

// From h_ButtonUpdate, after the original -- same slot and same reasoning as
// savefile_on_button_update. Inert unless this peer is a client that has been
// told the host left and is still inside a run.
void leave_on_button_update(void* button);

// From the shared Button::Click chain (mgmp_mainmenulock.cpp), BEFORE the
// original is allowed to run. Returns true to swallow the click. Blocks a
// client from abandoning the adventure themselves (host-authoritative, like
// every other action that chain already guards); also logs any new distinct
// "Button_PauseMenu_*" name it sees, on EITHER role, so the guess this is
// built on can be corrected from the log if it is wrong.
bool leave_on_client_button_click(void* button);

// PANEL: arm the leave path as though the host had announced it.
//
// The feature has three stages that fail identically from the outside -- the
// host never announced, the client declined the announcement, or the client
// armed and never saw the button -- and telling them apart from a screenshot is
// impossible. This skips the first two: press it, open the pause menu, and if
// Quit To Menu is not pressed then the fault is the click and nothing else.
//
// Deliberately not role-gated. It is a test of a local mechanism, and refusing
// it on a host would remove the one machine a developer is usually sitting at.
void leave_request_local();

// PANEL: one line of live state -- armed or not, where this peer thinks it is,
// how many presses are left and whether the cooldown is holding one off.
//
// Every stage of this feature fails invisibly from the outside, and reading it
// back out of the log after the fact has now cost two rounds of "it did not
// work" with no way to say which half. Writing it on the screen while the
// player is looking at the pause menu is the difference between a report and an
// observation. Always NUL-terminates.
void leave_status(char* out, size_t out_size);

// Whether THIS peer is inside an adventure, by the scene test above. Exposed
// because it answers a question several modules currently answer by proxy, and
// because the debug panel should be able to show it.
bool leave_in_run();

// From mgmp_follow's host-side MapScreen::update tick: true exactly once, the
// first time the map is ticking again after this peer announced a departure
// (a real HostLeftMsg went out). Consumes itself on the call that returns
// true. See the .cpp for why this is not simply done at the "back InRun"
// transition.
bool leave_consume_republish_due();

// General scene-presence query for any module that just needs to know
// whether ONE named scene is currently loaded and not being torn down --
// reuses the exact scene walk this module's own House/MainMenu/SaveSelect
// detection already relies on, rather than a second copy of it elsewhere.
// False whenever the scene list itself could not be read, matching this
// module's own "gate on evidence, never a confident wrong answer" rule.
bool leave_scene_is_loaded(const char* name);

} // namespace mgmp
