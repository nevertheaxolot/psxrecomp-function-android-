/*
 * custom_combat.h — Alternative combat engine for Dragon Ball Final Bout,
 * hosted in the psxrecomp runtime as a trusted .psxmod plugin.
 *
 * Adaptation notes (vs the original Gemini proposal):
 *   - "Frontend Dear ImGui"            -> the launcher's Mods tab. The mod
 *     manifest declares a boolean option that the launcher renders and persists;
 *     it lands in `custom_combat_engine_enabled` below. The launcher is
 *     ImGui-based, so this is literally the ImGui frontend exposure.
 *   - "VBlank hook"                    -> psx_mod_register_vblank_plugin(). The
 *     runtime invokes registered plugins on every guest VBlank, before the
 *     guest's IRQ_VBLANK handler runs (gpu.c:2886). When the engine is off the
 *     callback returns immediately and the TOSE loop runs 1:1 untouched.
 *   - "0-frame input lag"              -> the plugin reads the latched SIO pad
 *     state (sio_get_pad_buttons_slot), applies its own mapping, and writes it
 *     back with sio_set_pad_state_slot before the guest frame reads it — the
 *     game sees the mapped input in the very same frame. The game's *internal*
 *     input-lag frames (its own pad->action decode) are bypassed in Phase 2 by
 *     function-entry hooks on the decode function (see cc_game_layout).
 *   - "Animation bridge"               -> Trigger_Custom_Anim() writes the guest
 *     animation id through the game's animation-state field (cc_game_layout)
 *     once Ghidra supplies the offsets. The recompiler can additionally emit
 *     psx_mod_function_entry() at the game's animation setter/action-dispatch
 *     addresses via [recompiler] mod_function_entry_funcs (regen required).
 *
 * Phasing: the host-side engine (input remap, state machine, Newtonian physics,
 * framedata, hit resolution) is fully functional without any guest knowledge.
 * The guest bridge (reading/writing the fighters' real state, driving real
 * animations, cancelling into the real Super cinematics) activates when
 * cc_game_layout is filled from the Phase-2 Ghidra disassembly.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Global toggle. Exposed to the Dear ImGui launcher through the mod option
 * "enabled". FALSE -> VBlank hook returns immediately, the game's TOSE loop
 * executes 1:1. TRUE  -> the engine hijacks the per-frame flow below.
 * ------------------------------------------------------------------------- */
extern bool custom_combat_engine_enabled;

/* ---------------------------------------------------------------------------
 * Game-specific guest layout — filled from Ghidra (Phase 2). Every field is a
 * KSEG0 address or a byte offset inside a fighter's state struct. Until
 * `layout_ready` is set the guest bridge is inert and only the host-side
 * engine runs (input remap + state machine + physics + hit resolution).
 * ------------------------------------------------------------------------- */
typedef struct {
    /* One state-struct base per fighter (KSEG0). 0 = unknown. */
    uint32_t player_state_base[2];

    /* Byte offsets within a fighter state struct (signed; -1 = unknown). */
    int32_t anim_id_offset;     /* current animation id                   */
    int32_t anim_frame_offset;  /* frame counter inside the animation     */
    int32_t pos_x_offset;       /* world X                                */
    int32_t pos_y_offset;       /* world Y (jump height)                  */
    int32_t vel_x_offset;       /* lateral velocity                       */
    int32_t vel_y_offset;       /* vertical velocity                      */
    int32_t hp_offset;          /* health                                 */
    int32_t ki_offset;          /* ki meter                               */
    int32_t facing_offset;      /* facing (+1 right, -1 left)             */
    int32_t hitstun_offset;     /* hitstun frames remaining               */
    int32_t blockstun_offset;   /* blockstun frames remaining             */
    int32_t airborne_offset;    /* bool: off the ground                   */
    int32_t ground_y_offset;    /* world Y of the floor (gravity anchor)  */

    /* Guest functions to hook via [recompiler] mod_function_entry_funcs.
     * 0 = not hooked yet. Requires a regen once discovered. */
    uint32_t input_decode_func; /* game's pad->action decoder entry       */
    uint32_t anim_set_func;     /* game's animation setter entry          */
    uint32_t damage_func;       /* game's damage/impact routine entry     */

    int layout_ready;           /* 1 once all required offsets are filled  */
} cc_game_layout_t;

/* ---------------------------------------------------------------------------
 * State machine (host-side abstraction of the player).
 * ------------------------------------------------------------------------- */
enum {
    CC_STATE_IDLE = 0,
    CC_STATE_WALKING,
    CC_STATE_JUMPING,    /* airborne, Newtonian gravity                    */
    CC_STATE_ATTACKING,  /* framedata tracked by cc_move.active            */
    CC_STATE_HITSTUN,
    CC_STATE_BLOCKING,
    CC_STATE_SUPER,      /* game's Super cinematic, driven via bridge      */
    CC_STATE_COUNT
};

/* Physical PSX pad bit layout (SIO0 digital). */
#define CC_PAD_SELECT    0x0001u
#define CC_PAD_L3        0x0002u
#define CC_PAD_R3        0x0004u
#define CC_PAD_START     0x0008u
#define CC_PAD_UP        0x0010u
#define CC_PAD_RIGHT     0x0020u
#define CC_PAD_DOWN      0x0040u
#define CC_PAD_LEFT      0x0080u
#define CC_PAD_L2        0x0100u
#define CC_PAD_R2        0x0200u
#define CC_PAD_L1        0x0400u
#define CC_PAD_R1        0x0800u
#define CC_PAD_TRIANGLE  0x1000u
#define CC_PAD_CIRCLE    0x2000u
#define CC_PAD_CROSS     0x4000u
#define CC_PAD_SQUARE    0x8000u

/* Combat action set (SFEX-style abstraction, fully host-side). */
enum {
    CC_ACT_NONE = 0,
    CC_ACT_FORWARD,    /* toward the opponent (fast lateral, vel_x scaled) */
    CC_ACT_BACK,       /* away from the opponent                          */
    CC_ACT_JUMP,       /* Up — Newtonian gravity, no float                */
    CC_ACT_CROUCH,     /* Down                                           */
    CC_ACT_PUNCH,      /* Square (ground; air version on jump)            */
    CC_ACT_KICK,       /* Circle (ground; air version on jump)            */
    CC_ACT_LOW_PUNCH,  /* Down + Square — 2 animations, low hit           */
    CC_ACT_LOW_KICK,   /* Down + Circle                                   */
    CC_ACT_JUMP_PUNCH, /* Square while airborne                            */
    CC_ACT_JUMP_KICK,  /* Circle while airborne                            */
    CC_ACT_GUARD,      /* Cross (standing or crouching with Down)         */
    CC_ACT_KI_BLAST,   /* Triangle — instant ki bolt, no recovery lag     */
    CC_ACT_SUPER,      /* command-sequence Super (game cinematic)         */
    CC_ACT_COUNT
};

typedef struct {
    int action;              /* current combat action                      */
    int action_pressed;      /* edge-triggered this frame                  */
    int action_held;         /* level-triggered                            */
} cc_action_t;

/* ---------------------------------------------------------------------------
 * Fighter — host-side mirror of the combat state. When the layout is ready
 * the mirror is refreshed from / committed to the guest struct each frame so
 * the engine remains a thin wrapper over the recompiled game's RAM.
 * ------------------------------------------------------------------------- */
typedef struct {
    int state;
    int facing;              /* +1 right, -1 left                          */
    int x, y;                /* world units (host-tracked until layout)     */
    int vel_x, vel_y;        /* units/frame (lateral + vertical)            */
    int hp, ki;
    int current_anim;        /* last bridged guest animation id             */
    int move_id;             /* active move in cc_moves[] (or -1)           */
    int move_frame;          /* frame within the move                       */
    int hitstun_left;        /* frames of hitstun remaining                 */
    int blockstun_left;      /* frames of blockstun remaining               */
    int airborne;
    int guard_down;          /* blocking while crouching                    */
    int hit_pending;         /* move active frame just connected            */
    int super_ready;         /* command sequence armed (combo buffer)       */
    int last_held;           /* previous frame's action (edge detection)    */
    cc_action_t action;
} cc_fighter_t;

/* ---------------------------------------------------------------------------
 * Move (framedata) table — SFEX-derived baseline. `anim_id` is the FINAL
 * BOUT guest animation to bridge (0 = unassigned until disassembly). The
 * startup/active/recovery split and the cancel-on-impact rules implement the
 * Street-Fighter-EX feel; damage/guard/range seed the host-side hit resolver.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char* name;
    int anim_id;             /* guest animation id (Phase 2)                */
    int startup;             /* frames before the hit connects              */
    int active;              /* frames the hitbox is live                   */
    int recovery;            /* frames after the active window              */
    int damage;
    int guard_damage;
    int range;               /* world units (host hit resolver)             */
    int cancelable;          /* 1 = cancel on active frame -> ki/super      */
    int low;                 /* 1 = low hit (crouch block only)             */
    int airborne_only;       /* 1 = only executable in the air              */
} cc_move_t;

enum {
    CC_MOVE_PUNCH = 0, CC_MOVE_KICK, CC_MOVE_LOW_PUNCH, CC_MOVE_LOW_KICK,
    CC_MOVE_JUMP_PUNCH, CC_MOVE_JUMP_KICK, CC_MOVE_KI_BLAST, CC_MOVE_SUPER,
    CC_MOVE_COUNT
};

/* ---------------------------------------------------------------------------
 * Engine API.
 * ------------------------------------------------------------------------- */

/* Initialise the engine. Reads the plugin options (enabled, input profile,
 * engine knobs) and resets both fighters. Safe to call multiple times. */
void cc_init(void);

/* Per-frame engine tick — invoked from the mod's VBlank hook only when
 * custom_combat_engine_enabled is true. Order:
 *   1. cc_handle_input()    — map physical buttons -> combat actions
 *   2. cc_update_state_machine() — state transitions (framedata-driven)
 *   3. cc_update_physics()  — lateral velocity + Newtonian gravity
 *   4. cc_resolve_hits()    — host-side hitbox resolution
 *   5. cc_bridge_to_guest() — commit mirror -> guest RAM + animations */
void cc_frame_update(void);

/* Map the physical buttons of `player` (SIO slot) into its combat action set.
 * Also writes the remapped SIO state so the game's own read sees our layout
 * the same frame (0-frame lag). */
void cc_handle_input(int player);

/* Advance the player state machine (idle/walk/jump/attack/hitstun/block).
 * Drives move framedata (startup -> active -> recovery) and cancel windows. */
void cc_update_state_machine(int player);

/* Apply lateral movement velocity and Newtonian jump gravity. Final Bout's
 * original floaty jumps are replaced by a tunable gravity constant. */
void cc_update_physics(int player);

/* Host-side hit resolution using the move table ranges + fighter mirrors.
 * Sets hit_pending on the striking side and hitstun/blockstun on the victim. */
void cc_resolve_hits(void);

/* Force the guest graphics engine to play `anim_id` for `player_id`. When the
 * layout is ready this writes the game's animation-state field directly; with
 * [recompiler] mod_function_entry_funcs on anim_set_func it routes through the
 * game's own animation setter for correct cleanup/interpolation. */
void Trigger_Custom_Anim(int player_id, int anim_id);

/* Expose the fighters + layout for the debug server / tuning. */
cc_fighter_t* cc_get_fighter(int player);
cc_game_layout_t* cc_get_layout(void);
const cc_move_t* cc_get_moves(int* count);

#ifdef __cplusplus
}
#endif