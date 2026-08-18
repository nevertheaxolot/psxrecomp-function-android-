/*
 * Framework-owned widescreen view mod, available to every game.
 *
 * Display aspect is a property of the emulated GTE/GPU pair, not of any
 * particular disc, so the manifest lives in mods/builtin/packages targeting
 * game_id "*" (staged beside every title's own catalog). The activation plugin
 * selects a fixed host display aspect from the player's chosen option; the
 * per-title [widescreen]/[widescreen.cull] sites in game.toml provide the
 * actual FOV expansion on the codegen side.
 */
#include "mod_plugins.h"

#include <string.h>

#define PKG_WS "psx.enhancement.widescreen"

static void builtin_widescreen_activate(void) {
    char text[16] = "";
    const int have = psx_mod_option_value(PKG_WS, "widescreen", "aspect",
                                          text, sizeof text);
    if (have && strcmp(text, "21:9") == 0)
        (void)psx_mod_set_fixed_display_aspect(21u, 9u);
    else
        (void)psx_mod_set_fixed_display_aspect(16u, 9u);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin(
        "psx.widescreen", builtin_widescreen_activate);
}