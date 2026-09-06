#pragma once
// mgmp_invsync -- the run inventory is pushed between peers, whole, as bytes
// the GAME produced through the game's own bucket serializer.
//
// SYMMETRIC SINCE 2026-09-05 (F5 -- cahier-des-charges-mgmp-fork.md), TRIGGER
// STILL PER-NODE-OR-PEER-JOINED ONLY. Used to be host-only: a real gap once
// F6 let a peer equip their own cats, since the run inventory is a SINGLE
// shared resource and a client's local equip was invisible to the host until
// forever. Fixed by letting either peer call invsync_publish/on_message
// (matching CLAUDE.md's own CONTROL/NODEHASH precedent for state either side
// can locally change) and adding a Lamport `seq` to InventoryMsg so a stale
// publish cannot clobber a fresher one (ties go to the host) -- see
// invsync_on_message.
//
// WHAT WAS TRIED AND REVERTED, TWICE, THE SAME NIGHT -- read before adding a
// tighter trigger than "entering a node": the natural next step, publishing
// on a timer while mgmp_invlock sees the inventory screen open (so a change
// reaches the other peer in a fraction of a second instead of waiting for the
// next node), broke twice. First it broke THIS module's OWN click guard by
// calling into the heavy game-serialization functions below from inside
// T_ButtonUpdate, a hook that fires for every button every frame -- moving
// the call to FrameBegin fixed that specific breakage. Then, even from
// FrameBegin, calling the game's bucket write/serialize path at ~3 Hz
// produced ITEM DUPLICATION IN THE GAME'S OWN LOCAL STATE, observed on the
// SENDING peer with no network involved at all -- that function almost
// certainly exists to run once per real save, not dozens of times a second.
// Both attempts were fully reverted; only the symmetric direction and the
// seq field survive. F5's "immediate" goal is open again. Whatever replaces
// a timer needs to call the game's serializer AT MOST once per REAL change --
// e.g. determining Inventory::insert_item's actual ABI (still unknown, no
// signature generated -- would need IDA) and hooking the true mutation point,
// not polling for one.
//
// THE BUG THIS CLOSES. Equipping an item MOVES it: out of the run inventory and
// onto the cat. mgmp_catsync ships the cat half, and until now nothing shipped
// the other, so every item the host equipped mid-adventure stayed in the
// client's backpack as well as appearing on its cat. A duplicate of every
// equip, accumulating for the length of a run.
//
// Nothing detected it, and it is worth being precise about why, because the
// same blind spot will hide the next one: `state_hash` covers HP, shield, max
// HP, tile, facing and dead. The ability cross-check -- the one thing in the
// battle layer that looks at what a cat CAN DO rather than what has happened to
// it -- resolves by slot and validates by GON name, so it catches an item that
// is missing from a cat. Neither of them can see a spare copy sitting in a bag.
// The peers agree on every hash and every ability, and the run is still wrong.
//
// WHY THE CATSYNC SHAPE DOES NOT TRANSFER DIRECTLY. SerializeCatData takes a
// ByteStream, so the host could hand it one and take the bytes back out. The
// inventory has no such seam. Its serializer is a four-level stack that ends in
// the SAVE FILE rather than in a stream the caller owns:
//
//   sub_1402E10D0  the driver, (Inventory*, MewSaveFile*)
//     sub_1402E15B0  one bucket: walk the intrusive list into a vector
//       sub_14022CBD0  serialize that vector into a ByteStream it OWNS
//         sub_14022C4D0  hand the finished blob to the sqlite "files" table
//
// and the read side mirrors it exactly, bottoming out in
// glaiel::SQLSaveFile::Retrieve. There is no argument anywhere on that path
// that lets a caller supply or receive the bytes.
//
// WHAT WE DO INSTEAD: call the game's bucket driver and intercept the bottom.
// sub_14022C4D0 and sub_14022C620 are hooked, and while one of our own calls is
// in flight the hook takes the blob (host) or supplies one (client) and
// suppresses the sqlite access. So the host's push is
//
//   for each bucket: call sub_1402E15B0, catch the blob at the bottom
//
// and the client's apply is
//
//   for each bucket: call sub_1402E1740, feed it the blob at the bottom
//
// Everything that actually understands an item -- the list walk, the
// per-Equipment field loop, the version gating, the clear-and-rebuild, the item
// construction -- is the game's and is not reimplemented. That matters more
// here than it did for cats: glaiel::Inventory::insert_item alone has 36
// callers and its switch has a case whose whole job is working out which cat
// currently has a thing equipped.
//
// WHY NOT REPLAY THE CLICK. Same dead end as the cat half, for the same
// reasons, already written up in mgmp_catsync.h: InventoryItemBox::click() is
// `void click(void)` on a UI box that exists only while the inventory screen is
// open, and it merely STARTS a flow whose mutation fans out over ~8 CatData
// helpers. The client is not in that screen and never will be.
//
// WHY WHOLE BUCKETS AND NOT AN ITEM LIST. Items have no portable identity. On
// load each Equipment takes `++qword_1413BD998`, a process-global object-id
// counter that fifteen other classes also mint from and that is never written
// to or read back from the stream. The two peers mint different ids for the
// same item, so no message may key on one. An item's only durable identity is
// its GON name plus its stat fields -- which is exactly the situation that made
// abilities need a slot scheme, except here there is no slot to fall back on.
// Pushing each bucket whole sidesteps the question.
//
// WHAT IT COVERS: the three buckets the game itself persists -- backpack,
// storage, trash -- plus the three scalars from the same write driver
// (adventure_coins, adventure_food, adventure_furniture_boxes). The scalars
// need none of the above; they are plain ints on the Inventory object.
//
// WHAT IT DOES NOT COVER, stated plainly:
//
//   - The FOURTH bucket, Inventory+248. Inventory::init builds it identically
//     to the other three and the game's own save driver does not write it, so
//     we do not either. RULED OUT as the "currently equipped" tracker,
//     2026-09-05: measured live (a read-only serialize_bucket() dump on the
//     HOST across real equip/unequip cycles) that it stays at its empty size
//     the whole time, even on the host's own normal equip flow. Whatever
//     tracks equipped items lives entirely in CatData -- see the equip-slot
//     fix in mgmp_catsync.cpp for where.
//   - (ADDRESSED, but never observed either way -- see invsync_apply_pending.)
//     A CLIENT THAT HAS ITS INVENTORY SCREEN OPEN WHEN A PUSH ARRIVES was the
//     one hazard in this module that is a crash rather than a divergence:
//     sub_1401F0F00, the clear that sub_1402E1740 does before it repopulates,
//     walks the intrusive list and frees every node, and InventoryScreen2+0x20
//     is a vector of InventoryItemBox* holding exactly those Equipment
//     pointers. The game never hits it because its only caller is
//     ContinueAdventure, which runs when no inventory screen exists; we could,
//     because a push lands whenever the HOST enters a node, which is a moment
//     the client's own UI knows nothing about -- only the client's MAP input is
//     suppressed, not the rest of its game.
//
//     The apply is therefore deferred to the client's map-follow tick. Note
//     what that does and does not buy: it removes the window in which an
//     arriving push can free something a live screen is holding, because the
//     tick only runs from MapScreen::update. It is NOT a proof that no other
//     screen can be up at that moment. If a client ever crashes in or just
//     after the inventory screen, this is still the first place to look, and
//     the next step would be an explicit "is a screen open" test rather than a
//     structural one.
//
//   - Shop, level-up and world-event effects that write run state OUTSIDE the
//     Inventory and CatData: MewDirector+1552 (spawn_unit_next_fight),
//     MewDirector+1576 (familiars), *(MewDirector+1448)+1536 (BlankCollar).
//     Those are RUNSTATE and still open.

#include <cstdint>

namespace mgmp {

struct InventoryMsg;

// Resolves and prologue-checks the game functions this module calls. Called
// from hooks_install alongside catsync_set_base, for the same reason: a bad
// call address here runs an unrelated function with our arguments on the game's
// stack, so it is a refusal at startup rather than a crash three screens later.
void invsync_set_base(uintptr_t base);

void invsync_init();
void invsync_shutdown();

// HOST: serialize all three buckets and send them with the three scalars, if
// anything changed since the last push. Called from the same place as
// catsync_publish -- the map node entry both peers already synchronize on, and
// the last moment before a battle can be built out of a stale run.
void invsync_publish(const char* why);

// HOST: forget the last-pushed hash, so the next invsync_publish sends the
// inventory whether or not it changed. For a reconnecting peer.
void invsync_forget();

// CLIENT: take the host's inventory. While net_follow is on this only COPIES
// the message and holds it -- the run's inventory is not touched until
// invsync_apply_pending runs. With net_follow off it applies immediately,
// because nothing would ever drain the queue.
void invsync_on_message(const InventoryMsg& m);

// CLIENT: apply a held inventory, if there is one. Called from the map-follow
// tick, immediately before this peer enters the node the host entered.
//
// Two properties make that the right point and both are load-bearing. It runs
// from MapScreen::update, so the inventory screen whose boxes the apply would
// dangle is not the screen being ticked. And it is still before EnterNode, so
// nothing that consumes the inventory -- a battle, a shop -- can have run
// between the host's push and this peer applying it.
void invsync_apply_pending(const char* why);

// F6, 2026-09-06: re-apply the last inventory this peer successfully applied,
// right now. For mgmp_invlock's per-frame equip-slot poll: a raw byte revert
// of a CLIENT's blocked equip/unequip attempt can put the cat's own equip
// slot back, but the game's click handler also inserts/removes a node in the
// shared bag's intrusive list as the OTHER half of the same action, which a
// plain memory revert cannot undo. Re-running the exact apply this module
// already trusts for a real network push clears and rebuilds every bucket
// from scratch via the game's own reader, which correctly discards whatever
// the local click did to the bag too. A no-op if nothing has ever been
// successfully applied yet (no run loaded, or no push received this
// session).
void invsync_reapply_last_good(const char* why);

// --- the two hook bodies ----------------------------------------------------
//
// Both are inert -- one predicted branch -- unless one of THIS module's own
// calls is in flight. That gating is what keeps the game's ordinary saving and
// loading untouched: MewSaveFile::Store and ::Load carry every blob in the save
// file, not just ours, and a hook that fired on all of them would be rewriting
// the save.
//
// Each returns true to mean "handled, do not call the original", and when it
// does it has already destroyed the key string the original would have
// destroyed -- both game functions take that std::string BY VALUE.

// HOST: copy the finished blob off the ByteStream. `key` and `bs` are the
// original arguments.
bool invsync_intercept_store(void* key, void* bs);

// CLIENT: point the ByteStream at our bytes instead of at sqlite.
bool invsync_intercept_load(void* key, void* bs);

} // namespace mgmp
