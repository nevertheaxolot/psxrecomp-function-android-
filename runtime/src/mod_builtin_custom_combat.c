/*
 * mod_builtin_custom_combat.c — Alternative combat engine (SFEX-style) for
 * Dragon Ball Final Bout, hosted as a trusted psxrecomp .psxmod plugin.
 *
 * The engine is OPTIONAL and TOGGLEABLE. `custom_combat_engine_enabled` (the
 * mod's "enabled" option, rendered in the ImGui launcher's Mods tab) gates the
 * whole thing:
 *
 *   FALSE -> the VBlank hook returns immediately. The TOSE loop executes 1:1,
 *            byte-for-byte the original game.
 *   TRUE  -> the VBlank hook drives a host-side combat abstraction: input
 *            remap (0-frame lag), a state machine (idle/walk/jump/attack/
 *            hitstun/block), Newtonian jump physics, SFEX-style framedata and
 *            cancels, and a host-side hit resolver. The guest bridge that
 *            force-plays Final Bout's native animations and reads/writes the
 *            fighters' real RAM activates once cc_game_layout is filled from
 *            the Phase-2 Ghidra disassembly (see custom_combat.h).
 *
 * Architecture: thin wrapper over the recompiled game's RAM. The mirror
 * fighters are refreshed from / committed to the guest structs each frame.
 */
#include "custom_combat.h"
#include "mod_plugins.h"

#include <stdlib.h>
#include <string.h>

/* SIO0 pad service (runtime-internal, statically linked). */
extern uint16_t sio_get_pad_buttons_slot(int slot);
extern void     sio_set_pad_state_slot(int slot, uint16_t buttons);

#define CC_PKG       "psx.enhancement.custom-combat"
#define CC_FEATURE   "combat"

/* ---------------------------------------------------------------------------
 * Global toggle (custom_combat.h). FALSE = 1:1 TOSE loop; TRUE = hijack.
 * ------------------------------------------------------------------------- */
bool custom_combat_engine_enabled = false;

/* ---------------------------------------------------------------------------
 * Game-specific guest layout (Phase 2). All zeros until disassembly fills it.
 * ------------------------------------------------------------------------- */
static cc_game_layout_t g_layout = {
    /* Phase-2 verified (from disassembly / generated C):
     *  - input decode: func_8001B124 reads buttons at player+4, direction at
     *    player+50/+52 (masks 0x10 up, 0x40 down, 0x20 right, 0x1000 tri,
     *    0x8000 sq, 0x2000 circ, 0x4000 cross). Hook it for 0-frame remap.
     *
     *  Live-instrumentation (debug server 4370, user playing) mapped the fighter
     *  struct relative to its runtime base (re-allocated per fight, observed at
     *  0x80072800 / 0x8006EB20 / 0x800706AC across scenes):
     *    pos_x  +0x14   (s16; 0x80072814, also mirrored +0x34)
     *    vel_y  +0x64   (s16; 0x80072864, 0 when grounded, changes on jump)
     *    hp     halfword low  of +0x40 (0x80072840 low; ~0xED/0x138 by mode,
     *            falls with damage received)
     *    ki     halfword high of +0xBC (0x800728BC high; rises on ki charge)
     *    facing bits 0x2000/0x8000 of +0x1A8 (0x800729A8)
     *    anim   low word of +0x118 (0x80072918; changes with each action)
     *
     *  The base is NOT a stable constant, so the guest bridge stays inert
     *  (layout_ready=0) until a stable per-fight base is derived from a global
     *  (0x8003C30C points at the match struct ~0x2154 above the fighter).
     *  Writing the guest with a wrong base would corrupt fights (black screen
     *  + audio glitch on fight entry), so we never do it blind. */
    .input_decode_func = 0x8001B124u,
    .anim_id_offset    = 0x118,   /* low word of fighter+0x118 */
    .pos_x_offset      = 0x14,
    .vel_y_offset      = 0x64,
    .hp_offset         = 0x40,    /* halfword-low; see note above */
    .ki_offset         = 0xBC,    /* halfword-high; see note above */
    .facing_offset     = 0x1A8,   /* bits 0x2000/0x8000 */
    .layout_ready = 0,
};

/* Host-side fighter mirrors. */
static cc_fighter_t g_fighters[2];

/* Physics tunables (scaled by the "gravity"/"move_speed" options). */
static int g_gravity       = 100;   /* percent of baseline gravity           */
static int g_move_speed    = 100;   /* percent of baseline lateral speed     */
static int g_input_profile = 1;     /* 1 = fighter (SFEX-style), 0 = original */

#define CC_GRAVITY_BASE      2    /* units/frame^2 (falling)                */
#define CC_JUMP_VELOCITY    14    /* initial vertical velocity              */
#define CC_WALK_SPEED        4    /* units/frame lateral                    */
#define CC_DASH_SPEED        6    /* units/frame when attacking movement     */
#define CC_HITSTUN_BASE      14   /* frames of hitstun on connect            */
#define CC_BLOCKSTUN_BASE     8   /* frames of blockstun on guard            */
#define CC_KI_RANGE          120  /* ki-blast hitbox reach                   */

/* ---------------------------------------------------------------------------
 * Move (framedata) table — SFEX-derived baseline. anim_id is the FINAL BOUT
 * guest animation to bridge (0 = unassigned until Phase 2). Timing is in
 * guest frames (VBlanks).
 * ------------------------------------------------------------------------- */
static const cc_move_t g_moves[CC_MOVE_COUNT] = {
    /* name            anim  start act  rec  dmg  gdmg range canc low air */
    { "Punch",         0,    4,   3,   8,  60,   8,  60,  1,   0,  0 },
    { "Kick",          0,    6,   4,  12,  90,  12,  75,  1,   0,  0 },
    { "Low Punch",     0,    3,   3,   9,  55,   7,  55,  1,   1,  0 },
    { "Low Kick",      0,    5,   4,  13,  85,  11,  70,  1,   1,  0 },
    { "Jump Punch",    0,    3,   4,   8,  60,   8,  60,  1,   0,  1 },
    { "Jump Kick",     0,    5,   5,  10,  95,  13,  70,  1,   0,  1 },
    { "Ki Blast",      0,    2,   1,  12, 110,  15,  CC_KI_RANGE, 0, 0, 0 },
    { "Super",         0,    8,   6,  20, 320,  40, 150,  0,   0,  0 },
};

/* ---------------------------------------------------------------------------
 * Option helpers (mirror mod_builtin_speed.c conventions).
 * ------------------------------------------------------------------------- */
static int cc_option_flag(const char* feature, const char* id) {
    char text[16] = "";
    return psx_mod_option_value(CC_PKG, feature, id, text, sizeof text) &&
           strcmp(text, "true") == 0;
}
static int cc_option_number(const char* feature, const char* id, int fallback) {
    char text[16] = "";
    if (!psx_mod_option_value(CC_PKG, feature, id, text, sizeof text) || !text[0])
        return fallback;
    char* end = NULL;
    long v = strtol(text, &end, 10);
    if (!end || *end != '\0') return fallback;
    return (int)v;
}

/* ---------------------------------------------------------------------------
 * Input mapping (0-frame lag).
 *
 * Runs before the guest frame reads SIO: we read the latched physical pad,
 * derive the combat action set, and write the remapped state back so the game
 * observes our layout in the very same frame. The game's own input-lag frames
 * (its internal pad->action decode) are bypassed in Phase 2 by hooking
 * g_layout.input_decode_func via psx_mod_register_function_entry_plugin.
 *
 * Fighter profile (per the design):
 *   Square  -> punch        (air version while jumping)
 *   Circle  -> kick         (air version while jumping)
 *   Down+Square/Circle -> low punch / low kick (2 animations)
 *   Cross   -> guard        (standing, or crouching with Down)
 *   Up      -> jump         (Newtonian gravity)
 *   Forward/Back -> fast lateral movement (velocity-tunable)
 *   Triangle-> ki blast     (instant bolt, no recovery lag)
 *   [F],[F],Triangle or quarter-circle + Triangle -> Super command
 * ------------------------------------------------------------------------- */
static int cc_fighter_mirror(int slot) {
    return slot == 1 ? 1 : 0;
}

static void cc_parse_buttons(int player, uint16_t raw) {
    cc_fighter_t* f = &g_fighters[player];
    const int facing = f->facing >= 0 ? 1 : -1;
    const int forward = (raw & CC_PAD_RIGHT) && facing > 0 ? 1 :
                        (raw & CC_PAD_LEFT) && facing < 0 ? 1 : 0;
    const int back = (raw & CC_PAD_RIGHT) && facing < 0 ? 1 :
                     (raw & CC_PAD_LEFT) && facing > 0 ? 1 : 0;

    f->action.action_held = CC_ACT_NONE;
    f->action.action_pressed = CC_ACT_NONE;
    f->guard_down = (raw & CC_PAD_DOWN) != 0;

    /* Priority order: guard > low > kick/punch > ki > jump > movement. */
    if (raw & CC_PAD_CROSS) {
        f->action.action_held = CC_ACT_GUARD;
    } else if ((raw & CC_PAD_DOWN) && (raw & CC_PAD_SQUARE)) {
        f->action.action_held = CC_ACT_LOW_PUNCH;
    } else if ((raw & CC_PAD_DOWN) && (raw & CC_PAD_CIRCLE)) {
        f->action.action_held = CC_ACT_LOW_KICK;
    } else if (raw & CC_PAD_SQUARE) {
        f->action.action_held = f->airborne ? CC_ACT_JUMP_PUNCH : CC_ACT_PUNCH;
    } else if (raw & CC_PAD_CIRCLE) {
        f->action.action_held = f->airborne ? CC_ACT_JUMP_KICK : CC_ACT_KICK;
    } else if (raw & CC_PAD_TRIANGLE) {
        f->action.action_held = CC_ACT_KI_BLAST;
    } else if (raw & CC_PAD_UP) {
        f->action.action_held = CC_ACT_JUMP;
    } else if (forward) {
        f->action.action_held = CC_ACT_FORWARD;
    } else if (back) {
        f->action.action_held = CC_ACT_BACK;
    } else if (raw & CC_PAD_DOWN) {
        f->action.action_held = CC_ACT_CROUCH;
    }

    /* Edge trigger: a combat action fires only on its first frame (prevents
     * the original game's held-input buffering from re-triggering attacks). */
    if (f->action.action_held != f->last_held) {
        if (f->action.action_held != CC_ACT_NONE)
            f->action.action_pressed = f->action.action_held;
    }
    f->last_held = f->action.action_held;
}

/* Remap: write the canonical fighter-layout buttons back to SIO so the guest
 * sees our semantics. This is only safe once we know the game's button decode
 * (layout_ready, Phase 2) — remapping blind every frame during menus/loading
 * desyncs the game's input sequences (black screen + audio glitch on entering
 * a fight). Until then the engine runs its host-side mirror only and leaves
 * the guest pad untouched. */
static void cc_write_remapped_pad(int player, uint16_t raw) {
    if (!g_layout.layout_ready) return;      /* no guest mapping yet */
    if (g_input_profile == 0) return;        /* original buttons */
    sio_set_pad_state_slot(player, raw);
}

/* ---------------------------------------------------------------------------
 * State machine.
 * ------------------------------------------------------------------------- */
void cc_update_state_machine(int player) {
    cc_fighter_t* f = &g_fighters[player];

    /* Hitstun/blockstun count down on their own. */
    if (f->hitstun_left > 0) { f->hitstun_left--; if (!f->hitstun_left) f->state = CC_STATE_IDLE; }
    if (f->blockstun_left > 0) { f->blockstun_left--; if (!f->blockstun_left) f->state = CC_STATE_IDLE; }

    switch (f->state) {
    case CC_STATE_ATTACKING:
        f->move_frame++;
        if (f->move_frame > g_moves[f->move_id].startup +
                            g_moves[f->move_id].active +
                            g_moves[f->move_id].recovery) {
            f->state = CC_STATE_IDLE;
            f->move_id = -1;
        }
        break;

    case CC_STATE_HITSTUN:
    case CC_STATE_BLOCKING:
        break;   /* timers handled above */

    default: {
        const int act = f->action.action_pressed ? f->action.action_pressed
                                                 : f->action.action_held;
        switch (act) {
        case CC_ACT_JUMP:
            if (!f->airborne) {
                f->airborne = 1;
                f->vel_y = CC_JUMP_VELOCITY;
                f->state = CC_STATE_JUMPING;
            }
            break;
        case CC_ACT_PUNCH:   case CC_ACT_KICK:
        case CC_ACT_LOW_PUNCH: case CC_ACT_LOW_KICK:
        case CC_ACT_JUMP_PUNCH: case CC_ACT_JUMP_KICK:
        case CC_ACT_KI_BLAST: case CC_ACT_SUPER:
            f->move_id = (act == CC_ACT_PUNCH) ? CC_MOVE_PUNCH :
                         (act == CC_ACT_KICK) ? CC_MOVE_KICK :
                         (act == CC_ACT_LOW_PUNCH) ? CC_MOVE_LOW_PUNCH :
                         (act == CC_ACT_LOW_KICK) ? CC_MOVE_LOW_KICK :
                         (act == CC_ACT_JUMP_PUNCH) ? CC_MOVE_JUMP_PUNCH :
                         (act == CC_ACT_JUMP_KICK) ? CC_MOVE_JUMP_KICK :
                         (act == CC_ACT_KI_BLAST) ? CC_MOVE_KI_BLAST :
                                                    CC_MOVE_SUPER;
            f->move_frame = 0;
            f->state = CC_STATE_ATTACKING;
            break;
        case CC_ACT_GUARD:
            f->state = CC_STATE_BLOCKING;
            break;
        case CC_ACT_FORWARD: case CC_ACT_BACK:
            f->state = CC_STATE_WALKING;
            break;
        case CC_ACT_CROUCH:
            f->state = CC_STATE_IDLE;   /* crouch pose (bridge anim in Phase 2) */
            break;
        default:
            f->state = CC_STATE_IDLE;
            break;
        }
        break;
    }
    }
}

/* ---------------------------------------------------------------------------
 * Physics — Newtonian. Replaces the original floaty jump with a tunable
 * gravity curve; lateral movement is velocity-based (option-scaled).
 * ------------------------------------------------------------------------- */
void cc_update_physics(int player) {
    cc_fighter_t* f = &g_fighters[player];

    if (f->state == CC_STATE_WALKING) {
        const int speed = (CC_WALK_SPEED * g_move_speed) / 100;
        const int dir = (f->action.action_held == CC_ACT_FORWARD) ? f->facing
                                                                  : -f->facing;
        f->vel_x = dir * speed;
    } else {
        f->vel_x = 0;
    }

    if (f->airborne) {
        f->vel_y -= (CC_GRAVITY_BASE * g_gravity) / 100;   /* gravity pulls down */
        f->y += f->vel_y;
        if (f->y <= 0) {                 /* landing */
            f->y = 0;
            f->vel_y = 0;
            f->airborne = 0;
            if (f->state == CC_STATE_JUMPING) f->state = CC_STATE_IDLE;
        }
    }
    f->x += f->vel_x;
}

/* ---------------------------------------------------------------------------
 * Hit resolution (host-side mirror). Phase 2 commits damage/hitstun to the
 * guest structs and bridges the game's impact animation.
 * ------------------------------------------------------------------------- */
void cc_resolve_hits(void) {
    for (int p = 0; p < 2; p++) {
        cc_fighter_t* atk = &g_fighters[p];
        cc_fighter_t* def = &g_fighters[1 - p];
        if (atk->state != CC_STATE_ATTACKING || atk->move_id < 0) continue;
        const cc_move_t* m = &g_moves[atk->move_id];
        const int active_lo = m->startup;
        const int active_hi = m->startup + m->active;
        if (atk->move_frame < active_lo || atk->move_frame >= active_hi)
            continue;

        /* Host-side range check (mirror positions). */
        const int reach = (atk->x - def->x) * atk->facing;
        if (reach < 0 || reach > m->range) continue;

        atk->hit_pending = 1;
        if (def->state == CC_STATE_BLOCKING) {
            def->blockstun_left = CC_BLOCKSTUN_BASE;
            def->state = CC_STATE_BLOCKING;
        } else {
            def->hitstun_left = CC_HITSTUN_BASE;
            def->state = CC_STATE_HITSTUN;
            def->hp -= m->damage;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Guest bridge. Inert until cc_game_layout is filled (Phase 2). Once the
 * layout is ready, the mirror is refreshed from and committed to the guest
 * fighter structs each frame, and Trigger_Custom_Anim drives Final Bout's
 * native animation pipeline for the state the engine computed.
 * ------------------------------------------------------------------------- */
/* Fighter struct base: prefer the static layout base, else derive it live from
 * the game's match pointer 0x8003C30C (0 outside a fight, so the bridge stays
 * inert during menus). Final Bout re-allocates the fighter struct between
 * scenes, so a static base is unreliable — the live global is the source of
 * truth once the game is in a fight. */
#define CC_MATCH_PTR_ADDR 0x8003C30Cu
static uint32_t cc_guest_base(int player) {
    const cc_game_layout_t* L = &g_layout;
    if (L->player_state_base[player]) return L->player_state_base[player];
    uint32_t m = psx_mod_read_word(CC_MATCH_PTR_ADDR);
    if (!m) return 0;
    /* Observed: match ptr -> 0x800706AC, fighter struct -> 0x80072800.
     * (0x2154 apart.) Player 0 == match ptr base, player 1 offset once the
     * opponent struct location is confirmed — for now both read the match
     * struct so HP/KI offsets resolve without corruption. */
    return m;
}

static void cc_refresh_from_guest(int player) {
    const cc_game_layout_t* L = &g_layout;
    const uint32_t base = cc_guest_base(player);
    if (!base) return;
    cc_fighter_t* f = &g_fighters[player];
    if (L->pos_x_offset >= 0) f->x = (int32_t)psx_mod_read_word(base + (uint32_t)L->pos_x_offset);
    if (L->pos_y_offset >= 0) f->y = (int32_t)psx_mod_read_word(base + (uint32_t)L->pos_y_offset);
    if (L->hp_offset    >= 0) f->hp = (int32_t)(psx_mod_read_word(base + (uint32_t)L->hp_offset) & 0xFFFFu);
    if (L->ki_offset    >= 0) f->ki = (int32_t)(psx_mod_read_word(base + (uint32_t)L->ki_offset) >> 16);
    if (L->facing_offset >= 0) f->facing = (psx_mod_read_word(base + (uint32_t)L->facing_offset) & 0xA000u) ? 1 : -1;
    if (L->airborne_offset >= 0) f->airborne = psx_mod_read_word(base + (uint32_t)L->airborne_offset) != 0;
}

static void cc_commit_to_guest(int player) {
    const cc_game_layout_t* L = &g_layout;
    const uint32_t base = cc_guest_base(player);
    if (!base) return;
    const cc_fighter_t* f = &g_fighters[player];
    if (L->pos_x_offset >= 0) psx_mod_write_word(base + (uint32_t)L->pos_x_offset, (uint32_t)f->x);
    if (L->pos_y_offset >= 0) psx_mod_write_word(base + (uint32_t)L->pos_y_offset, (uint32_t)f->y);
    if (L->hp_offset    >= 0) {
        uint32_t old = psx_mod_read_word(base + (uint32_t)L->hp_offset);
        psx_mod_write_word(base + (uint32_t)L->hp_offset, (old & 0xFFFF0000u) | ((uint32_t)f->hp & 0xFFFFu));
    }
    if (L->ki_offset    >= 0) {
        uint32_t old = psx_mod_read_word(base + (uint32_t)L->ki_offset);
        psx_mod_write_word(base + (uint32_t)L->ki_offset, (old & 0xFFFFu) | (((uint32_t)f->ki & 0xFFFFu) << 16));
    }
    if (L->hitstun_offset >= 0) psx_mod_write_word(base + (uint32_t)L->hitstun_offset, (uint32_t)f->hitstun_left);
    if (L->blockstun_offset >= 0) psx_mod_write_word(base + (uint32_t)L->blockstun_offset, (uint32_t)f->blockstun_left);
}

void Trigger_Custom_Anim(int player_id, int anim_id) {
    const cc_game_layout_t* L = &g_layout;
    const uint32_t base = (player_id >= 0 && player_id < 2)
                              ? cc_guest_base(player_id) : 0;
    if (!base || L->anim_id_offset < 0) return;
    psx_mod_write_word(base + (uint32_t)L->anim_id_offset, (uint32_t)anim_id);
    g_fighters[player_id].current_anim = anim_id;
}

static void cc_bridge_to_guest(void) {
    if (!g_layout.layout_ready) return;
    for (int p = 0; p < 2; p++) {
        cc_refresh_from_guest(p);
        cc_commit_to_guest(p);
        /* Drive the guest animation from the engine's state/move. */
        const cc_fighter_t* f = &g_fighters[p];
        if (f->state == CC_STATE_ATTACKING && f->move_id >= 0)
            Trigger_Custom_Anim(p, g_moves[f->move_id].anim_id);
        else if (f->state == CC_STATE_JUMPING)
            Trigger_Custom_Anim(p, g_layout.anim_id_offset >= 0 ? 0 : 0); /* ANIM_JUMP */
    }
}

/* ---------------------------------------------------------------------------
 * Engine entry points (custom_combat.h).
 * ------------------------------------------------------------------------- */
void cc_init(void) {
    for (int i = 0; i < 2; i++) {
        cc_fighter_t* f = &g_fighters[i];
        memset(f, 0, sizeof *f);
        f->facing = i == 0 ? 1 : -1;
        f->move_id = -1;
        f->hp = 1000;
        f->ki = 0;
    }
}

void cc_frame_update(void) {
    if (!custom_combat_engine_enabled) return;
    for (int p = 0; p < 2; p++) {
        cc_fighter_t* f = &g_fighters[p];
        uint16_t raw = sio_get_pad_buttons_slot(p);
        cc_parse_buttons(p, raw);
        cc_write_remapped_pad(p, raw);
        cc_update_state_machine(p);
        cc_update_physics(p);
        (void)f;
    }
    cc_resolve_hits();
    cc_bridge_to_guest();
}

void cc_handle_input(int player) {
    cc_parse_buttons(player, sio_get_pad_buttons_slot(player));
}

cc_fighter_t* cc_get_fighter(int player) {
    return player == 0 || player == 1 ? &g_fighters[player] : NULL;
}
cc_game_layout_t* cc_get_layout(void)     { return &g_layout; }
const cc_move_t* cc_get_moves(int* count) { if (count) *count = CC_MOVE_COUNT; return g_moves; }

/* ---------------------------------------------------------------------------
 * Plugin wiring.
 * ------------------------------------------------------------------------- */

static void cc_activation(void) {
    custom_combat_engine_enabled =
        cc_option_flag(CC_FEATURE, "enabled");
    g_gravity    = cc_option_number(CC_FEATURE, "gravity", 100);
    g_move_speed = cc_option_number(CC_FEATURE, "move_speed", 100);
    {
        char text[16] = "";
        g_input_profile = (psx_mod_option_value(CC_PKG, CC_FEATURE, "input_profile",
                                                text, sizeof text) &&
                           strcmp(text, "original") == 0) ? 0 : 1;
    }
    cc_init();
}

static void cc_vblank(void) {
    /* When disabled this returns immediately — the TOSE loop runs 1:1. */
    cc_frame_update();
}

/* Function-entry hooks for the guest bridge (Phase 2). Registered only when
 * the addresses are known; the recompiler must also list them in
 * [recompiler] mod_function_entry_funcs (regen) for the emit to exist. */
static void cc_on_input_decode(struct CPUState* cpu, uint32_t address);
static void cc_on_anim_set(struct CPUState* cpu, uint32_t address);

static void cc_register_function_hooks(void) {
    if (g_layout.input_decode_func)
        psx_mod_register_function_entry_plugin("psx.custom-combat.input",
                                               g_layout.input_decode_func,
                                               cc_on_input_decode);
    if (g_layout.anim_set_func)
        psx_mod_register_function_entry_plugin("psx.custom-combat.anim",
                                               g_layout.anim_set_func,
                                               cc_on_anim_set);
}

/* Placeholder bridge handlers — filled once the game's decode/animation
 * routines are mapped. The input decode hook is where the game's internal
 * input-lag frames are bypassed (0-frame response end-to-end). */
static void cc_on_input_decode(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
}
static void cc_on_anim_set(struct CPUState* cpu, uint32_t address) {
    (void)cpu; (void)address;
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_custom_combat) {
    (void)psx_mod_register_activation_plugin("psx.custom-combat", cc_activation);
    (void)psx_mod_register_vblank_plugin("psx.custom-combat", cc_vblank);
    cc_register_function_hooks();
}