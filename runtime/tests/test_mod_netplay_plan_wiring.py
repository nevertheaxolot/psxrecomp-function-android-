#!/usr/bin/env python3
"""Guard: host lobby mod plan (match_caps.mods) is wired end to end.

The netplay advanced flow requires four links to stay connected:
 1. Host serializes its enabled selection into match_caps.mods.
 2. Guests receive the plan and surface it through lobby_mods_* callbacks.
 3. provider_commit_netplay applies the host plan (transiently) at launch
    instead of always forcing a vanilla session.
 4. Guests send their installed catalog (mod_offer) on join so the server
    seats them against the host's required list without a transfer.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
MOD_RUNTIME = (ROOT / "runtime/src/mod_runtime.cpp").read_text(encoding="utf-8")
MOD_RUNTIME_H = (ROOT / "runtime/include/mod_runtime.h").read_text(encoding="utf-8")
MOD_PACKAGES_H = (ROOT / "runtime/include/mod_packages.h").read_text(encoding="utf-8")
LOBBY = (ROOT / "runtime/src/psx_lobby_client.c").read_text(encoding="utf-8")
LOBBY_H = (ROOT / "runtime/include/psx_lobby_client.h").read_text(encoding="utf-8")


def require(needle: str, message: str, source: str) -> None:
    if needle not in source:
        raise AssertionError(message)


# 1. Host publishes the plan in match_caps.
require(
    "mod_runtime_netplay_fill_plan(",
    "host must publish its lobby mod plan into match_caps",
    MAIN,
)
require(
    "caps.plan, PSX_LOBBY_MAX_PLAN_MODS",
    "host plan must target the match_caps plan array",
    MAIN,
)
require(
    "parse_plan_mods_object(obj, out)",
    "guest must parse the host mod plan from match_caps",
    LOBBY,
)
require(
    "parse_plan_mods_object",
    "match_caps serialization/parse must handle the mods array",
    LOBBY,
)

# 2. Guests surface the plan through lobby_mods_* callbacks.
require(
    "int ae_np_lobby_mods_count(void*)",
    "lobby_mods_count callback must exist",
    MAIN,
)
require(
    "int ae_np_lobby_mods_get(void*, int index, RecompLauncherCNetplayLobbyMod* out)",
    "lobby_mods_get callback must exist",
    MAIN,
)
require(
    "int ae_np_lobby_mods_missing(void*)",
    "lobby_mods_missing callback must exist",
    MAIN,
)
require(
    "g_lnch_netplay_callbacks.lobby_mods_count = ae_np_lobby_mods_count;",
    "lobby_mods_count must be wired into the netplay callbacks",
    MAIN,
)
require(
    "g_lnch_netplay_callbacks.mod_xfer_progress = ae_np_mod_xfer_progress;",
    "mod_xfer_progress must be wired (no-transfer build)",
    MAIN,
)

# 3. provider_commit_netplay applies the host plan instead of vanilla.
require(
    "mod_runtime_netplay_apply_plan(",
    "netplay commit must apply the host mod plan",
    MOD_RUNTIME,
)
require(
    "const PsxLobbyMatchCaps* caps = psx_lobby_match_caps();",
    "netplay commit must read the host plan from match_caps",
    MOD_RUNTIME,
)
require(
    "transient_netplay",
    "host-plan apply must be transient (never persist to disk)",
    MOD_RUNTIME,
)
require(
    "selections_snapshot",
    "apply must snapshot the user's offline selection",
    MOD_PACKAGES_H,
)
require(
    "selections_restore",
    "apply must restore the user's offline selection after commit",
    MOD_PACKAGES_H,
)

# 4. Guests send their installed catalog on join.
require(
    "psx_lobby_set_mod_offer_builder",
    "lobby must accept a mod_offer builder",
    LOBBY,
)
require(
    "mod_runtime_netplay_offer_json",
    "runtime must serialize the guest installed catalog for mod_offer",
    MOD_RUNTIME,
)
require(
    '{"v":1,"pkgs":[',
    "mod_offer must use the server's {'v':1,'pkgs':[...]} object shape",
    MOD_RUNTIME,
)
require(
    'mod_offer',
    "join must include mod_offer when the guest has packages",
    LOBBY,
)

# 5. A netplay boot must honor the launcher-staged plan instead of clearing it.
require(
    "mod_runtime_netplay_plan_applied",
    "runtime must expose whether the launcher staged the netplay plan",
    MOD_RUNTIME,
)
require(
    "netplay_plan_applied",
    "provider_commit_netplay must mark the staged netplay plan",
    MOD_RUNTIME,
)
require(
    "if (net_cfg.enabled && PSXRecompV4::mod_runtime_netplay_plan_applied())",
    "a netplay boot with a staged plan must not clear it",
    MAIN,
)
# The host create/join/start buffers must be large enough to carry the mod
# plan; a small caps_json silently dropped the mods array off the create.
require(
    "char caps_json[7680];",
    "create must have a caps buffer large enough for the mod plan",
    LOBBY,
)
require(
    "char msg[8192];",
    "create/join/start must have a message buffer large enough for the plan",
    LOBBY,
)

print("host lobby mod plan end-to-end guard passed")