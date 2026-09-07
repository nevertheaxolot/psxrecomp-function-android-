#!/usr/bin/env python3
"""Guard: a mod-selected native VBlank rate survives the PAL 50 Hz fallback.

psx_mod_set_native_vblank_rate() fixes g_guest_frame_period_ms before the disc
region is resolved. The PAL branch then re-derives the stock 50 Hz pacing and
would silently undo a mod-owned turbo/rate selection unless it is told to keep
the mod's cadence.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")


def require(needle: str, message: str) -> None:
    if needle not in MAIN:
        raise AssertionError(message)


# The mod API is what fixes the guest cadence; keep it wired to the pacer.
require(
    "extern \"C\" int psx_mod_set_native_vblank_rate(",
    "mod API must remain declared",
)
require(
    "g_mod_native_vblank_fps = frames_per_second;",
    "mod rate must be recorded",
)
require(
    "g_guest_frame_period_ms = frames_per_second",
    "mod rate must fix the guest frame period",
)

# PAL region fallback must not clobber a mod-selected cadence.
require(
    "if (!g_mod_native_vblank_rate) {",
    "PAL fallback must skip re-pacing when a mod owns the VBlank rate",
)

# Ordering: the PAL branch and the mod activation must both exist, and the mod
# flag is what gates the PAL stock-50 Hz re-pace.
require("mod_runtime_activate_plugins();", "mod activation call is present")
require("ident.region == \"PAL\"", "PAL region detection is present")

# The mod flag lives in the same translation unit as the PAL branch.
require(
    "static bool          g_mod_native_vblank_rate = false;",
    "mod-native-vblank-rate flag must exist",
)

print("mod native VBlank rate survives PAL fallback guard passed")