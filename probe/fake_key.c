/* Presses keys in a KWin that runs headless for a test (tools/test-panel-key.sh), through KWin's
 * own test interface org_kde_kwin_fake_input, which that KWin is started to allow
 * (KWIN_WAYLAND_NO_PERMISSION_CHECKS=1). It talks to the compositor named by WAYLAND_DISPLAY and to
 * nothing else: on a desktop's own compositor the interface is not offered to it.
 *
 *   fake_key CODE:STATE ...     Linux input codes; state 1 pressed, 0 released; 40 ms between them
 *   fake_key 42:1 8:1 8:0 42:0  Shift+7
 *
 * The interface is written out here (its first twelve requests, as of version 4), so that the
 * protocol's description file is not needed to build this. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static const struct wl_interface *no_types[3];
static const struct wl_message requests[] = {
    {"authenticate", "ss", no_types},  {"pointer_motion", "ff", no_types}, {"button", "uu", no_types},
    {"axis", "uf", no_types},          {"touch_down", "2uff", no_types},   {"touch_motion", "2uff", no_types},
    {"touch_up", "2u", no_types},      {"touch_cancel", "2", no_types},    {"touch_frame", "2", no_types},
    {"pointer_motion_absolute", "3ff", no_types}, {"keyboard_key", "4uu", no_types}, {"destroy", "4", no_types},
};
static const struct wl_interface fake_input_interface = {"org_kde_kwin_fake_input", 4, 12, requests, 0, NULL};

static struct wl_proxy *fake_input;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    if (!strcmp(interface, fake_input_interface.name) && version >= 4)
        fake_input = wl_registry_bind(registry, name, &fake_input_interface, 4);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "fake_key: no compositor (WAYLAND_DISPLAY)\n"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!fake_input) { fprintf(stderr, "fake_key: this compositor does not offer org_kde_kwin_fake_input\n"); return 1; }
    wl_proxy_marshal(fake_input, 0 /* authenticate */, "myrddin key test", "a headless test of the layer's key");
    wl_display_roundtrip(display);
    for (int i = 1; i < argc; i++) {
        unsigned code = 0, state = 0;
        if (sscanf(argv[i], "%u:%u", &code, &state) != 2) { fprintf(stderr, "fake_key: CODE:STATE, not \"%s\"\n", argv[i]); return 1; }
        wl_proxy_marshal(fake_input, 10 /* keyboard_key */, code, state);
        wl_display_roundtrip(display);
        usleep(40000);
    }
    wl_display_disconnect(display);
    return 0;
}
