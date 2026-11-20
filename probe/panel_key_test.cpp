// The panel's key reader (fg/panel_key.h) by itself: how key names are read, and the X11 way of
// reading the keyboard (the Wayland way is tested with the layer, in tools/test-panel-key.sh).
//
//   panel_key_test names           the names: prints PASS/FAIL lines, exit 1 on a failure
//   panel_key_test x11 KEYS SECS   reads the X server named by DISPLAY for SECS seconds and prints
//                                  "press N" for every press of KEYS, then "presses N source S"
#include "../fg/panel_key.h"

#include <chrono>
#include <thread>

static int failed;
static void Expect(const char* asked, bool ok, const char* text, uint16_t code, bool shift, bool ctrl, bool alt) {
    panel_key::Combo combo;
    const bool got = panel_key::Parse(asked, combo);
    const bool same = got == ok && (!ok || (!std::strcmp(combo.text, text) && combo.code == code && combo.shift == shift &&
                                            combo.ctrl == ctrl && combo.alt == alt));
    std::printf("%s \"%s\" -> %s\n", same ? "PASS" : "FAIL", asked, got ? combo.text : "(not a key)");
    failed += !same;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "names")) {
        Expect("Shift+7", true, "Shift+7", 8, true, false, false);
        Expect("shift+7", true, "Shift+7", 8, true, false, false);
        Expect("ctrl+shift+grave", true, "Ctrl+Shift+`", 41, true, true, false);
        Expect("Ctrl+Shift+`", true, "Ctrl+Shift+`", 41, true, true, false);
        Expect("Alt+Control+f10", true, "Ctrl+Alt+F10", 68, false, true, true);
        Expect("F9", true, "F9", 67, false, false, false);
        Expect("pagedown", true, "PageDown", 109, false, false, false);
        Expect("Shift+=", true, "Shift+=", 13, true, false, false);
        Expect("Super+7", false, "", 0, false, false, false);
        Expect("Meta+F9", false, "", 0, false, false, false);
        Expect("Shift+", false, "", 0, false, false, false);
        Expect("Shift+Enter", false, "", 0, false, false, false);
        Expect("", false, "", 0, false, false, false);
        for (const panel_key::KeyName& key : panel_key::KEY_NAMES) { // every name reads back as its key
            panel_key::Combo combo;
            if (!panel_key::Parse(key.name, combo) || combo.code != key.code || std::strcmp(combo.text, panel_key::NameOf(key.code))) {
                std::printf("FAIL the name \"%s\"\n", key.name);
                ++failed;
            }
        }
        std::printf("%s\n", failed ? "names: FAILED" : "names: ok");
        return failed ? 1 : 0;
    }
    if (argc > 3 && !std::strcmp(argv[1], "x11")) {
        panel_key::Combo combo;
        if (!panel_key::Parse(argv[2], combo)) {
            std::printf("not a key: %s\n", argv[2]);
            return 1;
        }
        panel_key::Reader reader;
        reader.SeenX11();
        int presses = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::atof(argv[3]));
        while (std::chrono::steady_clock::now() < end) {
            if (const int now = reader.Poll(combo)) {
                presses += now;
                std::printf("press %d\n", presses);
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8)); // (a frame at 120 a second)
        }
        std::printf("presses %d source %s%s\n", presses, *reader.Source() ? reader.Source() : "none", reader.Failed() ? " (failed)" : "");
        return 0;
    }
    if (argc > 2 && !std::strcmp(argv[1], "wayland")) { // a connection of its own, no window: is the keyboard found?
        void* lib = dlopen("libwayland-client.so.0", RTLD_NOW);
        const auto connect = lib ? reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "wl_display_connect")) : nullptr;
        const auto roundtrip = lib ? reinterpret_cast<int (*)(void*)>(dlsym(lib, "wl_display_roundtrip")) : nullptr;
        void* display = connect ? connect(nullptr) : nullptr;
        if (!display || !roundtrip) {
            std::printf("no Wayland connection\n");
            return 1;
        }
        panel_key::Combo combo;
        panel_key::Parse("Shift+7", combo);
        panel_key::Reader reader;
        reader.SeenWayland(display);
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(std::atof(argv[2]));
        int rounds = 0;
        while (std::chrono::steady_clock::now() < end && !*reader.Source() && !reader.Failed()) {
            reader.Poll(combo);
            roundtrip(display); // (reads what the compositor sent, as a program's own event loop does)
            ++rounds;
        }
        std::printf("source %s%s after %d rounds\n", *reader.Source() ? reader.Source() : "none", reader.Failed() ? " (failed)" : "", rounds);
        return 0;
    }
    std::printf("panel_key_test names | x11 KEYS SECONDS | wayland SECONDS\n");
    return 1;
}
