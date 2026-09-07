/*
 * test_netplay_mods_synthetic.c — end-to-end lobby mod-plan protocol, without
 * a lobby server or a socket.
 *
 * The netplay mods feature is a wire contract between the host's launcher
 * (publishes match_caps.mods), the lobby server (compares it to the guest's
 * mod_offer before seating), and the guest (reads the plan, verifies installs,
 * applies it at launch). None of that is reachable by hand in a live game, so
 * this drives the REAL psx_lobby_client.c against a scripted in-process server
 * script: each client->server message is captured where rnet_ws_write_text
 * would have sent it, and each server->client frame is injected straight into
 * ws_pending. It validates the shapes that actually go on the wire.
 *
 * Build/run: ctest -R netplay_mods_synthetic_test
 * (registered in runtime/CMakeLists.txt.)
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/psx_lobby_client.c"

/* --- capture the client's outbound messages ------------------------------ */

static char g_cap[8192];
static size_t g_cap_len;

/* psx_lobby_client.c calls rnet_ws_write_text in flush_pending /
 * psx_lobby_send_signal. We stub it to capture instead of writing to a
 * socket, so the test can assert on the exact JSON the client emits. */
int rnet_ws_write_text(int fd, const char *text, int client_mask)
{
    (void)fd; (void)client_mask;
    g_cap_len = strlen(text);
    if (g_cap_len >= sizeof(g_cap)) g_cap_len = sizeof(g_cap) - 1;
    memcpy(g_cap, text, g_cap_len);
    g_cap[g_cap_len] = '\0';
    return 0;
}
/* Not exercised here (receive path is injected directly into ws_pending), but
 * the linker needs it because the TU references it. */
int rnet_ws_read_text(int fd, char *buf, size_t cap, int *closed)
{
    (void)fd; (void)buf; (void)cap; (void)closed;
    return 0;
}

/* psx_host_mono_ms lives in the runtime's main TU. */
uint64_t psx_host_mono_ms(void)
{
    static uint64_t t;
    return ++t;
}

/* Inject one server frame (payload only; drain_ws_pending expects a bare
 * unmasked text frame: opcode byte + length byte(s) + payload). Use the
 * 16-bit length form once the payload exceeds 125 bytes. */
static void inject_server_text(const char *payload)
{
    size_t len = strlen(payload);
    g_lc.ws_pending[0] = 0x81;               /* FIN + text opcode, unmasked */
    if (len < 126) {
        g_lc.ws_pending[1] = (uint8_t)len;
        memcpy(g_lc.ws_pending + 2, payload, len);
        g_lc.ws_pending_len = len + 2;
    } else {
        g_lc.ws_pending[1] = 126;
        g_lc.ws_pending[2] = (uint8_t)((len >> 8) & 0xff);
        g_lc.ws_pending[3] = (uint8_t)(len & 0xff);
        memcpy(g_lc.ws_pending + 4, payload, len);
        g_lc.ws_pending_len = len + 4;
    }
    drain_ws_pending();
}

/* Pretend we are connected, no real socket. fd must be >= 0 for
 * psx_lobby_connected() to pass; the rnet_ws_write_text stub ignores it. */
static void fake_connected(void)
{
    memset(&g_lc, 0, sizeof(g_lc));
    g_lc.fd = 0;
    g_lc.connected = 1;
    g_lc.handshake_done = 1;
    snprintf(g_lc.player_id, sizeof(g_lc.player_id), "%s", "peer-abc123");
    snprintf(g_lc.filter_game_name, sizeof(g_lc.filter_game_name),
             "%s", "Bloody Roar 2");
    snprintf(g_lc.filter_game_version, sizeof(g_lc.filter_game_version),
             "%s", "0.6.2");
}

static int g_failures;
static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); g_failures++; }
}
static void ck_sub(int cond, const char *what)
{
    if (!cond) { printf("  FAIL: %s\n", what); g_failures++; }
}

/* A host plan with the three BR2 enhancement packages. */
static void fill_host_plan(PsxLobbyMatchCaps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->valid = 1;
    caps->aspect_num = 4; caps->aspect_den = 3;
    caps->input_delay = 2; caps->input_prediction = 10;
    snprintf(caps->language, sizeof(caps->language), "%s", "en");

    snprintf(caps->plan[0].id, sizeof(caps->plan[0].id),
             "%s", "br2.enhancement.unlock-all");
    snprintf(caps->plan[0].version, sizeof(caps->plan[0].version), "%s", "0.1.0");
    snprintf(caps->plan[0].name, sizeof(caps->plan[0].name), "%s", "Unlock All");
    snprintf(caps->plan[0].config, sizeof(caps->plan[0].config), "%s", "unlock");
    caps->plan[0].builtin = 1; caps->plan[0].size = 0;

    snprintf(caps->plan[1].id, sizeof(caps->plan[1].id),
             "%s", "br2.enhancement.turbo");
    snprintf(caps->plan[1].version, sizeof(caps->plan[1].version), "%s", "0.1.0");
    snprintf(caps->plan[1].name, sizeof(caps->plan[1].name), "%s", "Turbo");
    snprintf(caps->plan[1].config, sizeof(caps->plan[1].config), "%s", "turbo;rate=75");
    caps->plan[1].builtin = 1;

    snprintf(caps->plan[2].id, sizeof(caps->plan[2].id),
             "%s", "br2.enhancement.widescreen");
    snprintf(caps->plan[2].version, sizeof(caps->plan[2].version), "%s", "0.1.0");
    snprintf(caps->plan[2].name, sizeof(caps->plan[2].name), "%s", "Widescreen");
    snprintf(caps->plan[2].config, sizeof(caps->plan[2].config), "%s", "widescreen");
    caps->plan[2].builtin = 1;
    caps->plan_count = 3;
}

static size_t test_mod_offer_builder(char *out, size_t out_cap)
{
    /* Exactly the server's mod_offer shape: {"v":1,"pkgs":[{id,ver},…]}. */
    return (size_t)snprintf(out, out_cap,
        "{\"v\":1,\"pkgs\":[{\"id\":\"br2.enhancement.turbo\",\"ver\":\"0.1.0\"}]}");
}

static void case_host_serializes_plan(void)
{
    PsxLobbyMatchCaps caps;
    printf("  host create serializes match_caps.mods\n");
    fake_connected();
    fill_host_plan(&caps);

    ck(psx_lobby_create("Test Room", "Bloody Roar 2", "0.6.2",
                        NULL, "0.0.0.0:7777", &caps) == 0,
       "create accepts a host plan");
    ck(g_cap_len > 0, "create emits a message");
    ck(strstr(g_cap, "\"match_caps\"") != NULL, "match_caps present");
    ck(strstr(g_cap, "\"mods\":[") != NULL, "mods array present");
    ck(strstr(g_cap, "\"id\":\"br2.enhancement.unlock-all\"") != NULL,
       "mod 0 id on the wire");
    ck(strstr(g_cap, "\"id\":\"br2.enhancement.turbo\"") != NULL,
       "mod 1 id on the wire");
    ck(strstr(g_cap, "\"id\":\"br2.enhancement.widescreen\"") != NULL,
       "mod 2 id on the wire");
    ck(strstr(g_cap, "\"version\":\"0.1.0\"") != NULL, "mod version on the wire");
    ck(strstr(g_cap, "\"builtin\":true") != NULL, "builtin flag on the wire");
    ck(strstr(g_cap, "\"config\":\"turbo;rate=75\"") != NULL,
       "host feature config travels intact");
    ck(strstr(g_cap, "\"input_delay\":2") != NULL,
       "a non-mod caps field still travels");
    /* 3 mods + the mods array must not exceed the lobby send buffer. */
    ck(g_cap_len < 8000, "host message fits the lobby send queue");
}

static void case_guest_join_includes_mod_offer(void)
{
    printf("  guest join includes the mod_offer object\n");
    fake_connected();
    psx_lobby_set_mod_offer_builder(test_mod_offer_builder);

    ck(psx_lobby_join("lobby-xyz", NULL, "0.0.0.0:7778") == 0,
       "join emits a message");
    ck(strstr(g_cap, "\"mod_offer\":") != NULL, "mod_offer field present");
    ck(strstr(g_cap, "{\"v\":1,\"pkgs\":[{\"id\":\"br2.enhancement.turbo\"")
           != NULL,
       "mod_offer is the server's object shape, not a bare array");
    /* Sanity: the join itself is still valid JSON-ish and carries identity. */
    ck(strstr(g_cap, "\"op\":\"join\"") != NULL, "join op present");
    ck(strstr(g_cap, "\"lobby_id\":\"lobby-xyz\"") != NULL, "lobby id present");
    ck(g_cap_len < 8000, "join with mod_offer fits the send buffer");
}

static void case_guest_parses_plan_from_launch(void)
{
    const PsxLobbyMatchCaps *caps;
    printf("  guest parses match_caps.mods from a launch frame\n");
    fake_connected();
    psx_lobby_set_ice_signal_accept(0);
    inject_server_text(
        "{\"op\":\"launch\",\"ok\":true,\"lobby_id\":\"lobby-xyz\","
        "\"session_id\":2,\"host_endpoint\":\"203.0.113.10:7777\","
        "\"guest_endpoint\":\"203.0.113.11:7778\","
        "\"relay_endpoint\":\"relay.example:8777\",\"transport\":\"sfu\","
        "\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"G\",\"ready\":false}],"
        "\"match_caps\":{\"v\":1,\"aspect_num\":4,\"aspect_den\":3,"
        "\"input_delay\":2,\"input_prediction\":10,\"language\":\"en\","
        "\"mods\":["
        "{\"id\":\"br2.enhancement.unlock-all\",\"version\":\"0.1.0\","
        "\"name\":\"Unlock All\",\"builtin\":true,\"size\":0,\"config\":\"unlock\"},"
        "{\"id\":\"br2.enhancement.turbo\",\"version\":\"0.1.0\","
        "\"name\":\"Turbo\",\"builtin\":true,\"size\":0,\"config\":\"turbo;rate=75\"}"
        "]}}");

    caps = psx_lobby_match_caps();
    ck(caps != NULL && caps->valid, "match_caps became valid");
    ck(caps->plan_count == 2, "plan_count parsed from the wire");
    ck(strcmp(caps->plan[0].id, "br2.enhancement.unlock-all") == 0,
       "plan[0].id parsed");
    ck(strcmp(caps->plan[1].config, "turbo;rate=75") == 0,
       "plan[1] host config parsed intact (; separators survive)");
    ck(caps->plan[1].builtin == 1, "plan[1] builtin parsed");
    ck(caps->input_delay == 2, "a non-mod caps field still parsed");

    /* The launch should have surfaced through launch_pending too. */
    ck(psx_lobby_launch_pending() == 1, "launch_pending set by the launch op");
}

static void case_request_start_resends_plan(void)
{
    PsxLobbyMatchCaps caps;
    printf("  host request_start re-sends the plan on start\n");
    fake_connected();
    fill_host_plan(&caps);
    /* request_start requires host + in_lobby. */
    g_lc.is_host = 1;
    g_lc.in_lobby = 1;
    snprintf(g_lc.host_player_id, sizeof(g_lc.host_player_id), "%s", "peer-abc123");

    ck(psx_lobby_request_start(&caps) == 0, "request_start emits a message");
    ck(strstr(g_cap, "\"op\":\"start\"") != NULL, "start op present");
    ck(strstr(g_cap, "\"match_caps\"") != NULL, "match_caps attached to start");
    ck(strstr(g_cap, "\"mods\":[") != NULL, "mods array attached to start");
    ck(strstr(g_cap, "br2.enhancement.widescreen") != NULL,
       "the full plan travels on start (frozen launch settings)");
}

static void case_guest_apply_plan_transient(void)
{
    printf("  host plan applies and survives a netplay boot (flag wiring)\n");
    fake_connected();
    psx_lobby_set_ice_signal_accept(0);
    inject_server_text(
        "{\"op\":\"launch\",\"ok\":true,\"lobby_id\":\"l\",\"session_id\":2,"
        "\"host_endpoint\":\"h:1\",\"guest_endpoint\":\"g:2\","
        "\"relay_endpoint\":\"r:3\",\"transport\":\"sfu\","
        "\"player_count\":2,\"max_slots\":2,\"slots\":["
        "{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"G\",\"ready\":false}],"
        "\"match_caps\":{\"v\":1,\"mods\":["
        "{\"id\":\"br2.enhancement.widescreen\",\"version\":\"0.1.0\","
        "\"name\":\"Widescreen\",\"builtin\":true,\"size\":0,\"config\":\"widescreen\"}"
        "]}}");

    ck(psx_lobby_match_caps()->plan_count == 1, "guest received the host plan");
    /* The runtime's flag (mod_runtime_netplay_plan_applied) lives in the mod
     * runtime, not here; what we CAN assert is that the plan survives in
     * match_caps for provider_commit_netplay to consume at boot. The boot
     * guard is covered by test_mod_netplay_plan_wiring.py. */
    ck(strcmp(psx_lobby_match_caps()->plan[0].id, "br2.enhancement.widescreen")
           == 0,
       "plan id read back for the launcher's apply step");
}

int main(void)
{
    printf("netplay_mods_synthetic_test\n");
    case_host_serializes_plan();
    case_guest_join_includes_mod_offer();
    case_guest_parses_plan_from_launch();
    case_request_start_resends_plan();
    case_guest_apply_plan_transient();
    if (g_failures == 0) {
        printf("netplay_mods_synthetic_test: ok\n");
        return 0;
    }
    fprintf(stderr, "netplay_mods_synthetic_test: %d failure(s)\n", g_failures);
    return 1;
}
