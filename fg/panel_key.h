// The performance panel's key, read inside the game's own process.
//
// On Wayland a program is sent the keyboard's events while one of its windows has the keyboard. The
// layer asks for those events a second time, on the connection the game's window was made on, with
// an event queue of its own: the game's own handling is not touched, nothing is injected, nothing of
// the game is read or changed. On X11 the X server is asked which keys are down, over a connection
// of the layer's own. This is how MangoHud reads its keys. No desktop shortcut is needed, so the key
// works on every desktop. Wayland: only while the game has the keyboard. X11: the server reports the
// keyboard as a whole (under Xwayland that is: while an X11 window has it).
//
// The key is a physical one, named as on a US keyboard ("Shift+7": Shift and the 7 of the number
// row), because that is what the keyboard reports; the game gets the press as well.
//
// libwayland-client and libX11 are not linked. Whichever of them the game's window was made with is
// in the process already and is looked up at run time; where neither is there, no key is read.
#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

namespace panel_key {

/// A key with the modifiers that have to be down with it (and no others).
struct Combo {
    uint16_t code{}; ///< the Linux input code of the key (KEY_7 is 8); 0: none
    bool shift{}, ctrl{}, alt{};
    char text[40]{}; ///< as it was asked for, tidied: "Shift+7"
};

/// The keys that can be asked for: as they are written ("F9", "7", "PageUp"; upper or lower case)
/// and the Linux input code of each. Where a key has two names the first is the one shown.
struct KeyName {
    const char* name;
    uint16_t code;
};
inline constexpr KeyName KEY_NAMES[] = {
    {"1", 2},   {"2", 3},   {"3", 4},   {"4", 5},   {"5", 6},   {"6", 7},   {"7", 8},   {"8", 9},   {"9", 10},
    {"0", 11},  {"-", 12},  {"Minus", 12},  {"=", 13},  {"Equal", 13},  {"Tab", 15},
    {"Q", 16},  {"W", 17},  {"E", 18},  {"R", 19},  {"T", 20},  {"Y", 21},  {"U", 22},  {"I", 23},  {"O", 24},
    {"P", 25},  {"[", 26},  {"]", 27},  {"A", 30},  {"S", 31},  {"D", 32},  {"F", 33},  {"G", 34},  {"H", 35},
    {"J", 36},  {"K", 37},  {"L", 38},  {";", 39},  {"'", 40},  {"`", 41},  {"Grave", 41},  {"\\", 43},
    {"Backslash", 43},  {"Z", 44},  {"X", 45},  {"C", 46},  {"V", 47},  {"B", 48},  {"N", 49},  {"M", 50},
    {",", 51},  {".", 52},  {"/", 53},  {"F1", 59},  {"F2", 60},  {"F3", 61},  {"F4", 62},  {"F5", 63},
    {"F6", 64}, {"F7", 65}, {"F8", 66}, {"F9", 67}, {"F10", 68},  {"F11", 87},  {"F12", 88},
    {"ScrollLock", 70},  {"Home", 102},  {"PageUp", 104},  {"End", 107},  {"PageDown", 109},  {"Insert", 110},
    {"Delete", 111},  {"Pause", 119},
};

/// The Linux input code of a key by its name, 0 where the name is not known.
inline uint16_t CodeOf(const char* name) {
    for (const KeyName& key : KEY_NAMES) {
        if (!strcasecmp(name, key.name)) {
            return key.code;
        }
    }
    return 0;
}
/// The name a key is shown by, nullptr where it is none of the keys above.
inline const char* NameOf(uint16_t code) {
    for (const KeyName& key : KEY_NAMES) {
        if (key.code == code) {
            return key.name;
        }
    }
    return nullptr;
}

/// "Shift+7", "ctrl+shift+grave", "F9" ... into a Combo, whose text is the tidy form ("Ctrl+Shift+`").
/// False (and `out` untouched) where the key is not known or a modifier is asked for that is not
/// Shift, Ctrl or Alt.
inline bool Parse(const char* text, Combo& out) {
    Combo combo;
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%s", text ? text : "");
    char* part = buffer;
    for (char* plus; (plus = std::strchr(part, '+')) != nullptr && plus[1] != '\0'; part = plus + 1) {
        *plus = '\0';
        if (!strcasecmp(part, "shift")) {
            combo.shift = true;
        } else if (!strcasecmp(part, "ctrl") || !strcasecmp(part, "control")) {
            combo.ctrl = true;
        } else if (!strcasecmp(part, "alt")) {
            combo.alt = true;
        } else {
            return false;
        }
    }
    combo.code = CodeOf(part);
    if (!combo.code) {
        return false;
    }
    std::snprintf(combo.text, sizeof combo.text, "%s%s%s%s", combo.ctrl ? "Ctrl+" : "", combo.shift ? "Shift+" : "",
                  combo.alt ? "Alt+" : "", NameOf(combo.code));
    out = combo;
    return true;
}

/// Reads the keyboard. `SeenWayland` / `SeenX11` may come from any thread (a window is being made);
/// everything else belongs to the one thread that calls `Poll`.
class Reader {
  public:
    void SeenWayland(void* display) {
        void* none = nullptr;
        wl_display_.compare_exchange_strong(none, display); // (the first one stays: a process has one connection)
    }
    void SeenX11() { x11_seen_.store(true); }

    /// How often `combo` was pressed since the last call.
    int Poll(const Combo& combo) {
        if (!combo.code) {
            return 0;
        }
        combo_ = combo;
        hits_ = 0;
        if (void* display = wl_display_.load()) {
            if (!wl_started_) {
                wl_started_ = true;
                StartWayland(display);
            }
            if (wl_queue_) {
                wl_.dispatch_queue_pending(display, wl_queue_);
                if (wl_flush_) {
                    wl_flush_ = false;
                    wl_.flush(display);
                }
            }
        } else if (x11_seen_.load()) {
            if (!x_started_) {
                x_started_ = true;
                StartX11();
            }
            // (the X server is asked, which is a round trip: sixty times a second at the most; a key
            // press lasts several times that long)
            timespec time {};
            clock_gettime(CLOCK_MONOTONIC, &time);
            const double now = static_cast<double>(time.tv_sec) + 1e-9 * static_cast<double>(time.tv_nsec);
            if (x_display_ && now - x_asked_ >= 1.0 / 60.0) {
                x_asked_ = now;
                PollX11();
            }
        }
        return hits_;
    }

    /// Where the keys come from: "Wayland" or "X11" once a keyboard is being read, else "".
    const char* Source() const { return wl_keyboard_ ? "Wayland" : x_display_ ? "X11" : ""; }
    /// True once it is known that no key can be read here (the library or the keyboard is missing).
    bool Failed() const { return wl_failed_ || x_failed_; }

  private:
    static constexpr size_t KEYS = 768;
    static constexpr uint16_t L_SHIFT = 42, R_SHIFT = 54, L_CTRL = 29, R_CTRL = 97, L_ALT = 56, R_ALT = 100, L_META = 125,
                              R_META = 126;

    bool Matches() const {
        const bool shift = down_[L_SHIFT] || down_[R_SHIFT], ctrl = down_[L_CTRL] || down_[R_CTRL];
        const bool alt = down_[L_ALT] || down_[R_ALT], meta = down_[L_META] || down_[R_META];
        return shift == combo_.shift && ctrl == combo_.ctrl && alt == combo_.alt && !meta;
    }

    // ------------------------------------------------------------------------------- Wayland
    struct WaylandArray {
        size_t size, alloc;
        void* data;
    };
    struct WaylandCalls {
        void* (*display_create_queue)(void*){};
        void* (*proxy_create_wrapper)(void*){};
        void (*proxy_wrapper_destroy)(void*){};
        void (*proxy_set_queue)(void*, void*){};
        void* (*proxy_marshal_constructor)(void*, uint32_t, const void*, ...){};
        void* (*proxy_marshal_constructor_versioned)(void*, uint32_t, const void*, uint32_t, ...){};
        int (*proxy_add_listener)(void*, void (**)(void), void*){};
        void (*proxy_destroy)(void*){};
        int (*display_dispatch_queue_pending)(void*, void*){};
        int (*display_flush)(void*){};
        const void* registry_interface{};
        const void* seat_interface{};
        const void* keyboard_interface{};
        int dispatch_queue_pending(void* display, void* queue) const { return display_dispatch_queue_pending(display, queue); }
        void flush(void* display) const { display_flush(display); }
    };

    void StartWayland(void* display) {
        void* lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) {
            lib = dlopen("libwayland-client.so.0", RTLD_NOW);
        }
        bool ok = lib != nullptr;
        const auto find = [&](const char* name) -> void* {
            void* found = ok ? dlsym(lib, name) : nullptr;
            ok = ok && found;
            return found;
        };
#define PANEL_KEY_FIND(member, name) wl_.member = reinterpret_cast<decltype(wl_.member)>(find(name))
        PANEL_KEY_FIND(display_create_queue, "wl_display_create_queue");
        PANEL_KEY_FIND(proxy_create_wrapper, "wl_proxy_create_wrapper");
        PANEL_KEY_FIND(proxy_wrapper_destroy, "wl_proxy_wrapper_destroy");
        PANEL_KEY_FIND(proxy_set_queue, "wl_proxy_set_queue");
        PANEL_KEY_FIND(proxy_marshal_constructor, "wl_proxy_marshal_constructor");
        PANEL_KEY_FIND(proxy_marshal_constructor_versioned, "wl_proxy_marshal_constructor_versioned");
        PANEL_KEY_FIND(proxy_add_listener, "wl_proxy_add_listener");
        PANEL_KEY_FIND(proxy_destroy, "wl_proxy_destroy");
        PANEL_KEY_FIND(display_dispatch_queue_pending, "wl_display_dispatch_queue_pending");
        PANEL_KEY_FIND(display_flush, "wl_display_flush");
#undef PANEL_KEY_FIND
        wl_.registry_interface = find("wl_registry_interface");
        wl_.seat_interface = find("wl_seat_interface");
        wl_.keyboard_interface = find("wl_keyboard_interface");
        if (!ok) {
            wl_failed_ = true;
            return;
        }
        // A queue of the layer's own: what arrives for it waits there until Poll hands it out, and
        // the game's own queue is not touched. The registry is made through a wrapper of the display
        // so that it is on that queue from the start.
        wl_queue_ = wl_.display_create_queue(display);
        void* wrapper = wl_queue_ ? wl_.proxy_create_wrapper(display) : nullptr;
        if (!wrapper) {
            wl_queue_ = nullptr;
            wl_failed_ = true;
            return;
        }
        wl_.proxy_set_queue(wrapper, wl_queue_);
        wl_registry_ = wl_.proxy_marshal_constructor(wrapper, 1 /* wl_display.get_registry */, wl_.registry_interface, nullptr);
        wl_.proxy_wrapper_destroy(wrapper);
        if (!wl_registry_) {
            wl_queue_ = nullptr;
            wl_failed_ = true;
            return;
        }
        static void (*registry_listener[])(void) = {reinterpret_cast<void (*)(void)>(OnGlobal),
                                                    reinterpret_cast<void (*)(void)>(OnGlobalRemove)};
        wl_.proxy_add_listener(wl_registry_, registry_listener, this);
        wl_flush_ = true;
    }

    static void OnGlobal(void* data, void* registry, uint32_t name, const char* interface, uint32_t version) {
        auto* self = static_cast<Reader*>(data);
        // (the name of an interface is the first thing in its description)
        const char* const seat_name = *static_cast<const char* const*>(self->wl_.seat_interface);
        if (self->wl_seat_ || std::strcmp(interface, seat_name)) {
            return;
        }
        // Up to version 5: the events of the keyboard are the six listened to below.
        const uint32_t use = version < 5 ? version : 5;
        self->wl_seat_ = self->wl_.proxy_marshal_constructor_versioned(registry, 0 /* wl_registry.bind */, self->wl_.seat_interface,
                                                                         use, name, seat_name, use, nullptr);
        if (!self->wl_seat_) {
            return;
        }
        static void (*seat_listener[])(void) = {reinterpret_cast<void (*)(void)>(OnCapabilities),
                                                reinterpret_cast<void (*)(void)>(OnSeatName)};
        self->wl_.proxy_add_listener(self->wl_seat_, seat_listener, self);
        self->wl_flush_ = true;
    }
    static void OnGlobalRemove(void*, void*, uint32_t) {}
    static void OnSeatName(void*, void*, const char*) {}
    static void OnCapabilities(void* data, void* seat, uint32_t capabilities) {
        auto* self = static_cast<Reader*>(data);
        const bool keyboard = (capabilities & 2u) != 0; // WL_SEAT_CAPABILITY_KEYBOARD
        if (keyboard && !self->wl_keyboard_) {
            self->wl_keyboard_ = self->wl_.proxy_marshal_constructor(seat, 1 /* wl_seat.get_keyboard */, self->wl_.keyboard_interface, nullptr);
            if (self->wl_keyboard_) {
                static void (*keyboard_listener[])(void) = {
                    reinterpret_cast<void (*)(void)>(OnKeymap), reinterpret_cast<void (*)(void)>(OnEnter),
                    reinterpret_cast<void (*)(void)>(OnLeave),  reinterpret_cast<void (*)(void)>(OnKey),
                    reinterpret_cast<void (*)(void)>(OnModifiers), reinterpret_cast<void (*)(void)>(OnRepeat)};
                self->wl_.proxy_add_listener(self->wl_keyboard_, keyboard_listener, self);
                self->wl_flush_ = true;
            }
        } else if (!keyboard && self->wl_keyboard_) {
            self->wl_.proxy_destroy(self->wl_keyboard_);
            self->wl_keyboard_ = nullptr;
            self->down_.reset();
        }
    }
    static void OnKeymap(void*, void*, uint32_t, int32_t fd, uint32_t) {
        close(fd); // (the layout is not needed: keys are taken by where they are, not by what they print)
    }
    static void OnEnter(void* data, void*, uint32_t, void*, WaylandArray* keys) {
        auto* self = static_cast<Reader*>(data);
        self->down_.reset();
        const auto* codes = static_cast<const uint32_t*>(keys->data);
        for (size_t i = 0; i < keys->size / sizeof(uint32_t); ++i) {
            if (codes[i] < KEYS) {
                self->down_[codes[i]] = true; // (held while the window got the keyboard: no press)
            }
        }
    }
    static void OnLeave(void* data, void*, uint32_t, void*) { static_cast<Reader*>(data)->down_.reset(); }
    static void OnKey(void* data, void*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        auto* self = static_cast<Reader*>(data);
        if (key >= KEYS) {
            return;
        }
        const bool was = self->down_[key];
        self->down_[key] = state != 0;
        if (state == 1 && !was && key == self->combo_.code && self->Matches()) {
            ++self->hits_;
        }
    }
    static void OnModifiers(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
    static void OnRepeat(void*, void*, int32_t, int32_t) {}

    // ----------------------------------------------------------------------------------- X11
    void StartX11() {
        void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) {
            lib = dlopen("libX11.so.6", RTLD_NOW);
        }
        const auto open = lib ? reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "XOpenDisplay")) : nullptr;
        x_query_ = lib ? reinterpret_cast<int (*)(void*, char*)>(dlsym(lib, "XQueryKeymap")) : nullptr;
        x_display_ = open && x_query_ ? open(nullptr) : nullptr; // (a connection of the layer's own)
        x_failed_ = !x_display_;
    }
    void PollX11() {
        char keys[32] = {};
        x_query_(x_display_, keys);
        // (an X key code is the Linux input code plus 8)
        down_.reset();
        for (unsigned code = 8; code < 256; ++code) {
            if (keys[code >> 3] & (1 << (code & 7))) {
                down_[code - 8] = true;
            }
        }
        const bool now = down_[combo_.code] && Matches();
        if (now && !x_was_) {
            ++hits_;
        }
        x_was_ = now;
    }

    std::atomic<void*> wl_display_{};
    std::atomic<bool> x11_seen_{};
    Combo combo_;
    int hits_{};
    std::bitset<KEYS> down_;

    WaylandCalls wl_;
    bool wl_started_{}, wl_failed_{}, wl_flush_{};
    void* wl_queue_{};
    void* wl_registry_{};
    void* wl_seat_{};
    void* wl_keyboard_{};

    bool x_started_{}, x_failed_{}, x_was_{};
    double x_asked_{};
    void* x_display_{};
    int (*x_query_)(void*, char*){};
};

} // namespace panel_key
