// mgmp_turnbadge.cpp -- see mgmp_turnbadge.h for the design.
#include "mgmp_turnbadge.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_lockstep.h"
#include "mgmp_ownertable.h"   // kNoOwner
#include "mgmp_log.h"
#include "mgmp_tuning.h"

#include <cstring>

namespace mgmp {
namespace {

// Identical shapes to mgmp_cursor's -- see that file for why each field is
// what it is (alignment, the 15-char small-string budget, the ABI of the
// tile_piece call). Not shared between the two files because both are
// file-local there; duplicating ~20 lines is cheaper than exporting them.
typedef void* (__fastcall* fn_im_game_ui)(void* component);
typedef void* (__fastcall* fn_tile_piece)(void* ui, void* id, int layer, void* anim,
                                          uint64_t tile, const float* rgba,
                                          int frame, const double* scale);

struct alignas(16) PieceColour { float  v[4]; };
struct alignas(16) PieceScale  { double v[3]; };

struct GameStr {
    char     buf[16];
    uint64_t size;
    uint64_t cap;
};

bool make_str(GameStr& s, const char* text) {
    size_t n = strlen(text);
    if (n > 15) return false;
    memset(&s, 0, sizeof(s));
    memcpy(s.buf, text, n);
    s.size = n;
    s.cap  = 15;
    return true;
}

// Same table as mgmp_cursor and mgmp_overlay: one peer, one colour,
// everywhere a peer is represented on screen.
const float kPeerRGB[kMaxPeers][3] = {
    { 1.00f, 0.80f, 0.20f },   // 0, the host -- gold
    { 0.30f, 0.80f, 1.00f },   // 1           -- cyan
    { 0.45f, 1.00f, 0.45f },   // 2           -- green
    { 1.00f, 0.40f, 0.85f },   // 3           -- magenta
};

const char* kAnimName = "TargetCursor";   // proven safe by mgmp_cursor
constexpr int kLayer = 6;                 // the same layer TargetCursor is authored for
constexpr int kFrame = -1;

struct State {
    fn_im_game_ui im_game_ui = nullptr;
    fn_tile_piece tile_piece = nullptr;
    bool          resolved   = false;
};
State g;

} // namespace

void turnbadge_set_base(uintptr_t base) {
    (void)base;   // the call targets are resolved from the module's own table
    g.im_game_ui = nullptr;
    g.tile_piece = nullptr;
    g.resolved   = false;

    struct { int which; void** slot; } want[] = {
        { C_ImGameUI,    (void**)&g.im_game_ui },
        { C_ImTilePiece, (void**)&g.tile_piece },
    };
    for (auto& w : want) {
        const uintptr_t addr = addr_of_call((Call)w.which);
        if (!addr) {
            // Non-fatal, same policy as mgmp_cursor: a badge is a nicety, and
            // a call to the wrong address would run an unrelated function
            // with our arguments on the game's stack.
            log_line("TURNBADGE", "!! %s did not resolve by signature -- "
                                  "the turn-owner badge is OFF", kCalls[w.which].name);
            g.im_game_ui = nullptr;
            g.tile_piece = nullptr;
            return;
        }
        *w.slot = (void*)addr;
    }
    g.resolved = true;
}

void turnbadge_on_status_menu(void* status_menu) {
    if (!tune::kTurnBadge || !status_menu || !g.resolved) return;

    // Answers nullptr for no session, no snapshot, or an AI/summon turn --
    // nothing to badge in every one of those cases.
    const void* actor = lockstep_current_actor();
    if (!actor) return;

    const uint8_t owner = lockstep_owner_of_character(actor);
    if (owner == kNoOwner) return;   // F2 has not decided this cat's owner yet

    int32_t x = 0, y = 0;
    if (!lockstep_character_tile(actor, x, y)) return;   // TacticsObject round trip failed

    void* ui = g.im_game_ui(status_menu);
    if (!ui) return;   // not actually a battle screen

    const float* rgb = kPeerRGB[owner % kMaxPeers];
    const PieceColour rgba = { { rgb[0], rgb[1], rgb[2], 1.0f } };
    // Packed exactly as the game stores an iVec2D: x in the low dword.
    const uint64_t tile = ((uint64_t)(uint32_t)y << 32) | (uint32_t)x;

    GameStr id{}, anim{};
    if (!make_str(id, "MGMPTurn") || !make_str(anim, kAnimName)) return;

    const PieceScale scale = { { 1.0, 1.0, 1.0 } };
    g.tile_piece(ui, &id, kLayer, &anim, tile, rgba.v, kFrame, scale.v);
}

} // namespace mgmp
