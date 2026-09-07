#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace PSXRecompV4 {

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path = {},
                            std::string* error = nullptr);
bool mod_runtime_commit(const std::filesystem::path& disc_path = {},
                        std::string* error = nullptr);
/* Drop the in-session mod plan for a netplay launch without rewriting the
 * user's persisted offline selection on disk. */
bool mod_runtime_clear_for_netplay(std::string* error = nullptr);
const std::string& mod_runtime_fingerprint();
const std::filesystem::path& mod_runtime_effective_disc_path();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

/* Netplay mod plan (match_caps.mods). The host serializes its current
 * selection (enabled features + resolved option values) so every peer adopts
 * the host's configuration at launch instead of running vanilla or its own
 * offline selection. The plan is transient: applying it never writes the
 * user's persisted offline selection to disk.
 *
 * `plan` is the C array used by the lobby protocol (id/version/name/builtin/
 * size/config). Each entry's `config` is a flat ";"-separated string of
 * feature tokens: `feature` enables a feature, `feature.option=value` sets an
 * option on it (e.g. "widescreen;widescreen.mode=16:9;turbo;turbo.rate=75").
 * mod_runtime_netplay_serialize_package builds it from the manager;
 * mod_runtime_netplay_apply_plan drives set_feature_enabled/set_feature_option
 * for every entry (packages this peer does not have installed are skipped and
 * counted as `missing`, which the launcher surfaces before launch) and then
 * commits without persisting the host selection to disk. */

/* Maximum host-feature-config bytes we serialize per plan entry. */
static constexpr size_t kNetplayPlanConfigCap = 512;

/* Serialize the manager's enabled features + resolved option values for
 * `package_id` into `config` (flat "feature;feature.opt=value" form). Returns
 * false when the package has nothing enabled (empty plan entry). */
bool mod_runtime_netplay_serialize_package(const std::string& package_id,
                                           char* config, size_t config_cap);

/* Fill `plan` (capacity `plan_cap` PsxLobbyPlanMod entries) with the manager's
 * selected packages that have at least one enabled feature. Returns the number
 * of entries written (0 = vanilla session). Used by the host to publish its
 * lobby mod plan. */
int mod_runtime_netplay_fill_plan(void* plan, int plan_cap);

/* 1 once provider_commit_netplay has staged this netplay session's plan
 * (host plan or vanilla) in memory this process. A later netplay boot must
 * not clear that staged plan a second time. */
bool mod_runtime_netplay_plan_applied(void);

/* 1 when the named package/version is installed and selected locally, else 0.
 * Used by lobby_mods_* to tell a guest which host-plan packages it lacks. */
int mod_runtime_netplay_package_installed(const char* id, const char* version);

/* Serialize the guest's installed-package catalog into `out` as the lobby
 * `mod_offer` object ({"v":1,"pkgs":[{"id":..,"ver":..},…]}) the server
 * compares against match_caps.mods before seating. Returns bytes written
 * (0 = nothing installed / no provider). */
size_t mod_runtime_netplay_offer_json(char* out, size_t out_cap);

/* Apply a received host plan over the local manager (transient) and commit.
 * Packages absent locally are skipped; `missing` reports how many were. */
bool mod_runtime_netplay_apply_plan(const void* plan, int count,
                                    int* missing,
                                    const std::filesystem::path& disc_path,
                                    std::string* error = nullptr);

} // namespace PSXRecompV4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called before a guest dispatch. Applies the complete main-EXE plan
 * transactionally on the first dispatch to the configured entry point. */
void mod_runtime_on_dispatch(uint32_t target);
/* A full-machine savestate restores guest RAM after the initial entry-point
 * application. Reapply the already-validated main-EXE plan so the current
 * enabled mod selection remains authoritative after the restore. */
void mod_runtime_on_savestate_loaded(void);
/* Invokes activation callbacks for the committed plan. Call after the final
 * launcher commit and before renderer/window initialization. */
void mod_runtime_activate_plugins(void);
void mod_runtime_on_vblank(void);
void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                   uint8_t* bytes, uint32_t size);
void mod_runtime_enable_disc_patches(void);

#ifdef __cplusplus
}
#endif
