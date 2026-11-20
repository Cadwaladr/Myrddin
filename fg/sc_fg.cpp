// sc-fg: host-side Vulkan layer that generates frames for Star Citizen with FSR 3.1 frame
// interpolation (AMD FidelityFX SDK 1.1.4 as built for Linux by the Lichii project), fed by the
// motion vectors and depth the game hands to DLSS.
//
// How it works
//   * DLSS on Linux runs through VK_NVX_image_view_handle / VK_NVX_binary_import.  The views the
//     game hands to the driver there are DLSS's inputs.  Just before DLSS's first kernel of a
//     frame, copies of the motion vectors and the depth are recorded into the game's own command
//     buffer, into images this layer owns.  Nothing of the game's state is changed.
//   * At vkQueuePresentKHR the layer copies the finished frame, runs optical flow and frame
//     interpolation between the previous and the current frame in its own command buffer, presents
//     the generated frame(s) through extra swapchain images, then the game's frame.
//
// Stage 1: the generated frames are presented synchronously inside the game's present call, paced
// by the swapchain's vsync.  No pacing thread, no HUD separation.
//
// Environment
//   SC_FG=1            enables the layer (manifest)          SC_FG_MATCH   process filter
//   SC_FG_DIR          log / control / dump folder (/tmp)    SC_FG_MULT    2..4 (default 2)
//   SC_FG_NEAR, SC_FG_FOV   camera near plane (m) and vertical field of view (rad): guesses
//   SC_FG_FLAGS        FFX context flags (default 3: inverted + infinite depth)
//   SC_FG_FALLBACK=1   interpolate even without DLSS inputs (zero vectors; optical flow only)
//   SC_FG_TARGET       the display's refresh rate (120)      SC_FG_CAP=0   no limit on shown frames
//   SC_FG_CHAIN=0      captures handed out in order of arrival, not by the frame chain
//   SC_FG_WAIT_GPU=0   generated frames are sent without waiting until they are ready
//   SC_FG_GPU_TIME=0   no fence behind the game's frame (the limiter then knows less about the GPU)
//   SC_FG_GATE=0       frames are let on by the timer alone, not in step with the present call (see Limiter)
//   SC_FG_GUARD=0      the game's next frame is not kept back while the last one waits to be copied out
//   SC_FG_FIRST=0      the game's next frame does not wait for the generated frames' GPU work (see WaitForGenerated)
//   SC_FG_GPUWAIT=1    the game's next frame waits until the GPU is done with the one before (off by default:
//                      less delay where the GPU is the slower one, at the cost of a few frames in some cases)
//   SC_FG_FILL=0      the multiplier is the largest that does not hold the game back (default: the smallest
//                      that fills the display)    SC_FG_PROBE=0   a held game is not probed for what it could do
//   SC_FG_MAX_BASE=0   the multiplier stays as given: it does not follow the game's rate
//   SC_FG_ADAPT=0      the same, and this one can be changed while the game runs (PARAMS adapt): 0 the
//                      multiplier is held where it was asked for, 1 (default) it follows the game's rate
//   SC_FG_AUTO_OFF=0   generation never stops on its own
//   SC_FG_FIX_VECTORS  the game's vectors are checked against the picture (mv_check.comp): 1 where the
//                      scene without the HUD is known (default), 2 also on the presented frame, 0 never
//                      (SC_FG_VECTOR_FACTOR, SC_FG_VECTOR_MARGIN: how much better a neighbour's vector must be)
//   SC_FG_PANEL_KEY=Shift+7   the key that steps the panel on, read in the game's own process (panel_key.h)
//   SC_FG_PANEL=1      the layer's own performance panel can be shown (control file PANEL); without it
//                      there is none, whatever that file says
//   SC_FG_TEST_INPUTS=1, SC_FG_TEST_LAYER, SC_FG_TEST_WAIT_MS, SC_FG_TEST_CHECKS, SC_FG_TEST_LOCKS   tests only
//                      (probe/test_chain.c)
// Control files in SC_FG_DIR, looked at once a second:
//   OFF     while it exists, frames pass through untouched
//   DUMP    removed when seen; the next generated frame and its real frame are written as .ppm
//   EVENTS  removed when seen; the last presents (captures used, timings) are written as text
//   PARAMS  name=value lines, removed when read: HUD detection, and chain / nohud / wait / shift / vectors /
//           gate / guard / first / gpuwait / fill / probe / adapt
//   (sc-turbo has a command for each of these and for the others)
// Files the layer keeps there for when something goes wrong (see Crumb, Lock, Stage):
//   STAGE-<pid>    where the present hook is
//   THREADS-<pid>  every thread that calls into the layer: its hook, the locks it holds and waits for
//                  ("sc-turbo fg-where" reads both, also after the game has ended)
//   and in the log: "lock ..." lines when a thread waited for one of the layer's locks for a second,
//   or when a lock was left behind by a call that never returned and has been taken over.

#include <algorithm>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include "hud_apply.spv.h"
#include "hud_composite.spv.h"
#include "hud_detect.spv.h"
#include "hud_layer.spv.h"
#include "hud_unmix.spv.h"
#include "mv_check.spv.h"
#include "mv_marks.spv.h"
#include "mv_reach.spv.h"
#include "mv_spread.spv.h"
#include "mv_tiles.spv.h"
#include "hud_panel.h"
#include "panel_key.h"
#include "panel.spv.h"
#include "hud_moved.spv.h"
#include "hud_blocks.spv.h"
#include "hud_motion.spv.h"
#include "hud_around.spv.h"
#include "hud_pieces.spv.h"
#include "hud_field.spv.h"
#include "hud_refine.spv.h"
#include "hud_reach.spv.h"
#include "hud_which.spv.h"
#include "hud_layer_moving.spv.h"

#define EXPORT __attribute__((visibility("default")))

namespace {

inline void* Key(const void* handle) {
    return *static_cast<void* const*>(handle);
}
template <typename T>
inline uint64_t U64(T handle) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

// ----------------------------------------------------------------------------- settings, log

const char* Dir() {
    const char* dir = std::getenv("SC_FG_DIR");
    return dir && *dir ? dir : "/tmp";
}
float EnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtof(value, nullptr) : fallback;
}
double Now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ----------------------------------------------------------------------------- who is where

/// Every thread that calls into the layer keeps a line about itself: the hook it is in and since
/// when, how far it has come in it, the locks it holds and the one it waits for. The lines are in
/// a file (THREADS-<pid> in the log directory; "sc-turbo fg-where" reads it), so that a game that
/// stood still can be asked afterwards what each of its threads was doing in here. A line is
/// written by its own thread only.
struct Crumb {
    std::atomic<int32_t> tid;
    std::atomic<int32_t> waiting;   ///< the lock it waits for (its number plus one); 0: none
    std::atomic<uint32_t> held;     ///< the locks it holds, a bit each
    std::atomic<uint32_t> step;     ///< how far it is in the hook: a line of this file; 0: just entered
    std::atomic<uint64_t> calls;    ///< calls into the layer so far (a hook entered from inside one is the same call)
    std::atomic<double> since;      ///< when the hook was entered; 0: in none (the name is then the last one's)
    std::atomic<double> wait_since; ///< when the wait for the lock began
    std::atomic<uint64_t> hook[2];  ///< the hook's name
    std::atomic<uint64_t> lost;     ///< calls of this thread that never came back (see InHook)
};
static_assert(sizeof(Crumb) == 64, "a cache line each");
/// (The first line is the file's head; the last is shared by the threads beyond that many.)
constexpr uint32_t CRUMBS = 256;

enum : uint8_t {
    LOCK_LOG, LOCK_INPUTS, LOCK_MARK, LOCK_FG, LOCK_TABLES, LOCK_RECORDS, LOCK_INPUT, LOCK_CHAIN, LOCK_LIMITER,
    LOCK_GATE, LOCK_TRACE, LOCK_SCAN, LOCK_HUD_PASS, LOCK_COUNT
};
const char* const LOCK_NAMES[LOCK_COUNT] = {"log",   "inputs",  "mark", "fg",    "tables", "records", "input",
                                            "chain", "limiter", "gate", "trace", "scan",   "hud-pass"};

Crumb* Crumbs() {
    static Crumb* const table = [] {
        static Crumb fallback[CRUMBS];
        const size_t size = sizeof(Crumb) * CRUMBS + 512;
        char path[600];
        std::snprintf(path, sizeof path, "%s/THREADS-%d", Dir(), getpid());
        const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        void* memory = fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) == 0
                           ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                           : MAP_FAILED;
        if (fd >= 0) {
            close(fd);
        }
        if (memory == MAP_FAILED) {
            return static_cast<Crumb*>(fallback);
        }
        // The head: what a reader needs to make sense of the rest (the times are Now()'s).
        struct Head {
            char magic[8];
            int32_t pid;
            uint32_t slots;
            double wall_at_zero;
        } head{{'S', 'C', 'F', 'G', 'T', 'H', 'R', '1'}, getpid(), CRUMBS, 0.0};
        timespec wall{};
        clock_gettime(CLOCK_REALTIME, &wall);
        head.wall_at_zero = static_cast<double>(wall.tv_sec) + static_cast<double>(wall.tv_nsec) * 1e-9 - Now();
        std::memcpy(memory, &head, sizeof head);
        char* text = static_cast<char*>(memory) + sizeof(Crumb) * CRUMBS;
        int n = std::snprintf(text, 512, "locks:");
        for (const char* name : LOCK_NAMES) {
            n += std::snprintf(text + n, static_cast<size_t>(512 - n), " %s", name);
        }
        return static_cast<Crumb*>(memory);
    }();
    return table;
}

class Lock;
class InHook;
/// What a thread knows about itself.
struct Self {
    Crumb* crumb{};
    InHook* top{};  ///< the hook it is in (the innermost one)
    Lock* held[8]{}; ///< the locks it holds
    uint32_t held_count{};
};
constinit thread_local Self t_self{};
std::atomic<uint32_t> g_crumbs_used{1};

Self& Me() {
    Self& self = t_self;
    if (!self.crumb) [[unlikely]] {
        const uint32_t slot = g_crumbs_used.fetch_add(1);
        self.crumb = &Crumbs()[std::min(slot, CRUMBS - 1)];
        self.crumb->tid.store(static_cast<int32_t>(syscall(SYS_gettid)), std::memory_order_relaxed);
    }
    return self;
}

/// A line into the log that takes no lock (one write): for what is said about the locks themselves.
__attribute__((format(printf, 1, 2))) void RawLog(const char* format, ...) {
    static const int fd = [] {
        char path[600];
        std::snprintf(path, sizeof path, "%s/fg-%d.log", Dir(), getpid());
        return open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }();
    char line[900];
    int n = std::snprintf(line, sizeof line, "%10.3f ", Now());
    va_list args;
    va_start(args, format);
    const int more = std::vsnprintf(line + n, sizeof line - static_cast<size_t>(n) - 1, format, args);
    va_end(args);
    n = std::min(n + std::max(more, 0), static_cast<int>(sizeof line) - 2);
    line[n++] = '\n';
    if (fd >= 0 && write(fd, line, static_cast<size_t>(n)) < 0) {
        // (nowhere to say it)
    }
}

void HookName(const Crumb& c, char name[17]) {
    const uint64_t words[2] = {c.hook[0].load(std::memory_order_relaxed), c.hook[1].load(std::memory_order_relaxed)};
    std::memcpy(name, words, 16);
    name[16] = 0;
    if (!name[0]) {
        std::strcpy(name, "-");
    }
}

void LockNames(uint32_t held, char* text, size_t size) {
    int n = 0;
    text[0] = 0;
    for (uint32_t i = 0; i < LOCK_COUNT && n < static_cast<int>(size) - 16; ++i) {
        if (held & (1u << i)) {
            n += std::snprintf(text + n, size - static_cast<size_t>(n), "%s%s", n ? "+" : "", LOCK_NAMES[i]);
        }
    }
    if (!n) {
        std::snprintf(text, size, "none");
    }
}

/// A thread in words: its name and state as the system has them, and its line here. False: there is
/// no such thread any more.
bool Describe(int32_t tid, char* text, size_t size) {
    char comm[32] = "?";
    char state = '?';
    bool there = true;
    {
        char path[64], stat[256];
        std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            there = errno != ENOENT && errno != ESRCH;
        } else {
            const ssize_t n = read(fd, stat, sizeof stat - 1);
            close(fd);
            if (n > 0) {
                stat[n] = 0;
                const char* open_at = std::strchr(stat, '(');
                const char* close_at = std::strrchr(stat, ')');
                if (open_at && close_at && close_at > open_at) {
                    const size_t length = std::min(static_cast<size_t>(close_at - open_at - 1), sizeof comm - 1);
                    std::memcpy(comm, open_at + 1, length);
                    comm[length] = 0;
                    state = close_at[1] == ' ' ? close_at[2] : '?';
                }
            }
        }
    }
    const Crumb* crumb = nullptr;
    Crumb* const table = Crumbs();
    const uint32_t used = std::min(g_crumbs_used.load(), CRUMBS);
    for (uint32_t i = 1; i < used; ++i) {
        if (table[i].tid.load(std::memory_order_relaxed) == tid) {
            crumb = &table[i];
        }
    }
    if (!there) {
        std::snprintf(text, size, "thread %d, which no longer exists", tid);
    } else if (!crumb) {
        std::snprintf(text, size, "thread %d (%s, state %c), which has no line here", tid, comm, state);
    } else {
        char name[17], held[160];
        HookName(*crumb, name);
        LockNames(crumb->held.load(std::memory_order_relaxed), held, sizeof held);
        const double since = crumb->since.load(std::memory_order_relaxed);
        const int32_t waiting = crumb->waiting.load(std::memory_order_relaxed);
        char in[80];
        if (since > 0.0) {
            std::snprintf(in, sizeof in, "in %s for %.3f s (line %u)", name, Now() - since,
                          crumb->step.load(std::memory_order_relaxed));
        } else {
            std::snprintf(in, sizeof in, "in no hook (last: %s)", name);
        }
        std::snprintf(text, size, "thread %d (%s, state %c) %s, holding %s, waiting for %s", tid, comm, state, in, held,
                      waiting > 0 && waiting <= LOCK_COUNT ? LOCK_NAMES[waiting - 1] : "none");
    }
    return there;
}

/// The layer's lock: a mutex that knows who holds it. A thread that waits for one for a second
/// says so in the log, with what the holder is doing. A holder that will never let go (a thread
/// that no longer exists, or one whose call into the layer never came back: see InHook) is found
/// out by the thread that waits, which then takes the lock over: the game is not to stand still
/// for good on a lock nobody holds.
class Lock {
public:
    constexpr explicit Lock(uint8_t id) : id_{id} {}
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void lock() {
        Self& self = Me();
        if (pthread_mutex_trylock(&mutex_) != 0) {
            Wait(self, -1.0);
        }
        Got(self);
    }
    bool try_lock() {
        Self& self = Me();
        if (pthread_mutex_trylock(&mutex_) != 0) {
            return false;
        }
        Got(self);
        return true;
    }
    /// False: not to be had in that time; nothing is held then.
    bool lock_for(double seconds) {
        Self& self = Me();
        if (pthread_mutex_trylock(&mutex_) != 0 && !Wait(self, seconds)) {
            return false;
        }
        Got(self);
        return true;
    }
    void unlock() {
        Self& self = t_self;
        Crumb& c = *self.crumb;
        for (uint32_t i = self.held_count; i-- > 0;) {
            if (self.held[i] == this) {
                self.held[i] = self.held[--self.held_count];
                break;
            }
        }
        c.held.store(c.held.load(std::memory_order_relaxed) & ~(1u << id_), std::memory_order_relaxed);
        owner_.store(0, std::memory_order_release);
        pthread_mutex_unlock(&mutex_);
    }
    /// The thread that holds it lost the call it was taken in (see InHook): let go for it, by itself.
    void LetGoLost(int32_t tid) {
        int32_t expected = tid;
        if (owner_.compare_exchange_strong(expected, 0)) {
            pthread_mutex_unlock(&mutex_);
            RawLog("lock %s: let go, for the call that never came back", LOCK_NAMES[id_]);
        }
    }
    const char* name() const {
        return LOCK_NAMES[id_];
    }

private:
    void Got(Self& self) {
        Crumb& c = *self.crumb;
        owner_calls_.store(c.calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
        turns_.store(turns_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        owner_.store(c.tid.load(std::memory_order_relaxed), std::memory_order_release);
        if (self.held_count < 8) {
            self.held[self.held_count++] = this;
        }
        c.held.store(c.held.load(std::memory_order_relaxed) | (1u << id_), std::memory_order_relaxed);
    }
    bool Wait(Self& self, double limit);

    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    std::atomic<int32_t> owner_{};        ///< the thread that holds it; 0: nobody; below 0: being taken over
    std::atomic<uint32_t> turns_{};       ///< how often it has been taken
    std::atomic<uint64_t> owner_calls_{}; ///< the holder's count of calls into the layer when it took it
    const uint8_t id_;
};

/// The lock is held. `limit` >= 0: for at most that long (false: not got).
__attribute__((noinline)) bool Lock::Wait(Self& self, double limit) {
    Crumb& c = *self.crumb;
    const int32_t me = c.tid.load(std::memory_order_relaxed);
    const double start = Now();
    c.wait_since.store(start, std::memory_order_relaxed);
    c.waiting.store(id_ + 1, std::memory_order_relaxed);
    double say_at = 1.0;
    bool got = false, said = false;
    int32_t lost_owner = 0; // the holder found lost at the last look, with the lock not taken since
    uint32_t lost_turn = 0;
    bool lost_seen = false;
    for (;;) {
        double slice = 0.25;
        if (limit >= 0.0) {
            const double left = start + limit - Now();
            if (left <= 0.0) {
                break;
            }
            slice = std::min(slice, left);
        }
        timespec until{};
        clock_gettime(CLOCK_MONOTONIC, &until);
        const long nanoseconds = until.tv_nsec + static_cast<long>(slice * 1e9);
        until.tv_sec += nanoseconds / 1000000000L;
        until.tv_nsec = nanoseconds % 1000000000L;
        if (pthread_mutex_clocklock(&mutex_, CLOCK_MONOTONIC, &until) == 0) {
            got = true;
            break;
        }
        const double waited = Now() - start;
        const int32_t owner = owner_.load(std::memory_order_acquire);
        const uint32_t turn = turns_.load(std::memory_order_relaxed);
        const uint64_t owner_calls = owner_calls_.load(std::memory_order_relaxed);
        // Will the holder ever let go? Not if there is none (a thread torn out of the middle of
        // taking it), if it is this very thread (in a call that never came back), if it no
        // longer exists, or if it has begun another call into the layer since it took the lock:
        // locks are let go before a call returns.
        const char* lost = nullptr;
        char holder[400] = "nobody";
        if (owner == 0) {
            lost = "held by nobody";
        } else if (owner == me) {
            lost = "held by this very thread, from a call that never came back";
        } else if (owner > 0) {
            if (!Describe(owner, holder, sizeof holder)) {
                lost = "its holder no longer exists";
            } else {
                Crumb* const table = Crumbs();
                const uint32_t used = std::min(g_crumbs_used.load(), CRUMBS - 1);
                for (uint32_t i = 1; i < used; ++i) {
                    if (table[i].tid.load(std::memory_order_relaxed) == owner &&
                        table[i].calls.load(std::memory_order_relaxed) != owner_calls) {
                        lost = "its holder left the call it took it in without letting go";
                    }
                }
            }
        }
        if (waited >= say_at) {
            char mine[400];
            Describe(me, mine, sizeof mine);
            RawLog("lock %s: waited for %.1f s by %s. Held by %s%s%s", LOCK_NAMES[id_], waited, mine, holder,
                   lost ? ": " : "", lost ? lost : "");
            say_at = say_at < 4.0 ? 5.0 : say_at < 19.0 ? 20.0 : say_at + 60.0;
            said = true;
        }
        // Taken over when that was found twice in a row, a quarter of a second apart, with the
        // lock not taken by anybody in between. One thread does it (the others go on waiting).
        if (lost && lost_seen && lost_owner == owner && lost_turn == turn && waited >= 0.45) {
            int32_t expected = owner;
            if (owner_.compare_exchange_strong(expected, -me)) {
                RawLog("lock %s: %s (%s). Taken over by thread %d after %.2f s", LOCK_NAMES[id_], lost, holder, me,
                       waited);
                pthread_mutex_unlock(&mutex_);
                said = true;
            }
            lost_seen = false;
        } else {
            lost_seen = lost != nullptr;
            lost_owner = owner;
            lost_turn = turn;
        }
    }
    c.waiting.store(0, std::memory_order_relaxed);
    if (said || (!got && limit >= 0.0)) {
        static std::atomic<uint32_t> told{};
        if (said || told.fetch_add(1) < 40) {
            char holder[400] = "nobody";
            const int32_t owner = owner_.load(std::memory_order_acquire);
            if (!got && owner > 0) {
                Describe(owner, holder, sizeof holder);
            }
            if (got) {
                RawLog("lock %s: thread %d has it after %.2f s", LOCK_NAMES[id_], me, Now() - start);
            } else {
                RawLog("lock %s: thread %d went on without it after %.2f s. Held by %s", LOCK_NAMES[id_], me,
                       Now() - start, holder);
            }
        }
    }
    return got;
}

/// A lock held for a scope if it can be had in time (see Lock::lock_for), for what is better left
/// out than waited for: `if (HeldFor lock{mutex, 0.25}; lock) { ... }`.
class HeldFor {
public:
    HeldFor(Lock& lock, double seconds) : lock_{lock}, held_{lock.lock_for(seconds)} {}
    ~HeldFor() {
        if (held_) {
            lock_.unlock();
        }
    }
    HeldFor(const HeldFor&) = delete;
    HeldFor& operator=(const HeldFor&) = delete;
    explicit operator bool() const {
        return held_;
    }

private:
    Lock& lock_;
    const bool held_;
};

struct Name16 {
    uint64_t a{}, b{};
};
consteval Name16 Tag(const char* text) {
    Name16 name;
    for (int i = 0; i < 16 && text[i]; ++i) {
        (i < 8 ? name.a : name.b) |= static_cast<uint64_t>(static_cast<unsigned char>(text[i])) << (8 * (i % 8));
    }
    return name;
}

/// A hook is being run, from its first line to its return: the thread's line says which.
///
/// Wine can take a thread out of a call for good: when something goes wrong in it (a fault), the
/// call "returns" to the game with an error and whatever it was doing here is simply dropped,
/// with the locks it held still locked. The next hook the thread enters finds that out: it begins
/// at or above the place on the stack where the hook that never returned was, which a hook entered
/// from inside one never does. The locks are then let go, and it is said in the log.
class InHook {
public:
    explicit InHook(Name16 name) : self_{Me()} {
        Crumb& c = *self_.crumb;
        InHook* outer = self_.top;
        if (outer && reinterpret_cast<uintptr_t>(this) >= reinterpret_cast<uintptr_t>(outer)) [[unlikely]] {
            Lost();
            outer = nullptr;
        }
        outer_ = outer;
        self_.top = this;
        if (outer) {
            name_[0] = c.hook[0].load(std::memory_order_relaxed);
            name_[1] = c.hook[1].load(std::memory_order_relaxed);
            since_ = c.since.load(std::memory_order_relaxed);
            step_ = c.step.load(std::memory_order_relaxed);
        } else {
            c.calls.store(c.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        c.hook[0].store(name.a, std::memory_order_relaxed);
        c.hook[1].store(name.b, std::memory_order_relaxed);
        c.step.store(0, std::memory_order_relaxed);
        c.since.store(Now(), std::memory_order_relaxed);
    }
    ~InHook() {
        Crumb& c = *self_.crumb;
        self_.top = outer_;
        if (outer_) {
            c.hook[0].store(name_[0], std::memory_order_relaxed);
            c.hook[1].store(name_[1], std::memory_order_relaxed);
            c.step.store(step_, std::memory_order_relaxed);
            c.since.store(since_, std::memory_order_relaxed);
        } else {
            c.since.store(0.0, std::memory_order_relaxed);
        }
    }
    InHook(const InHook&) = delete;
    InHook& operator=(const InHook&) = delete;

private:
    __attribute__((noinline)) void Lost() {
        Crumb& c = *self_.crumb;
        char name[17], held[160];
        HookName(c, name);
        LockNames(c.held.load(std::memory_order_relaxed), held, sizeof held);
        const int32_t waiting = c.waiting.load(std::memory_order_relaxed);
        RawLog("thread %d: its call into %s, entered %.3f s ago (line %u), never came back. It held: %s; it waited "
               "for: %s",
               c.tid.load(std::memory_order_relaxed), name, Now() - c.since.load(std::memory_order_relaxed),
               c.step.load(std::memory_order_relaxed), held,
               waiting > 0 && waiting <= LOCK_COUNT ? LOCK_NAMES[waiting - 1] : "none");
        const int32_t tid = c.tid.load(std::memory_order_relaxed);
        for (uint32_t i = self_.held_count; i-- > 0;) {
            self_.held[i]->LetGoLost(tid);
        }
        self_.held_count = 0;
        c.held.store(0, std::memory_order_relaxed);
        c.waiting.store(0, std::memory_order_relaxed);
        c.lost.store(c.lost.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    Self& self_;
    InHook* outer_{};
    uint64_t name_[2]{};
    double since_{};
    uint32_t step_{};
};
#define IN_HOOK(name) InHook in_hook_{Tag(name)}
/// How far the hook has come: this line.
#define STEP() Me().crumb->step.store(__LINE__, std::memory_order_relaxed)

Lock g_log_mutex{LOCK_LOG};

/// Tests only (SC_FG_TEST_LOCKS=1, run once when the first instance is made): the ways out of a
/// lock whose holder never lets go, and that a holder at work is waited for and left alone.
__attribute__((noinline)) void TestLostCall(Lock& lock, jmp_buf& out) {
    IN_HOOK("TestLost");
    lock.lock();
    STEP();
    longjmp(out, 1); // (as Wine does it: the call is dropped where it stands)
}
void TestLocks() {
    static Lock gone{LOCK_SCAN}, busy{LOCK_TRACE}, lost{LOCK_HUD_PASS};
    std::thread{[] { gone.lock(); }}.join();
    double start = Now();
    gone.lock();
    RawLog("locks test 1: the lock of a thread that ended was had after %.2f s", Now() - start);
    gone.unlock();

    std::thread worker{[] {
        IN_HOOK("TestWork");
        busy.lock();
        usleep(1600000);
        busy.unlock();
    }};
    usleep(100000);
    start = Now();
    const bool at_once = busy.lock_for(0.25);
    RawLog("locks test 2: the lock of a thread at work was %s in 0.25 s", at_once ? "HAD" : "not had");
    if (at_once) {
        busy.unlock();
    }
    busy.lock();
    RawLog("locks test 2: ... and had after %.2f s", Now() - start);
    busy.unlock();
    worker.join();

    std::thread{[] {
        jmp_buf out;
        if (!setjmp(out)) {
            TestLostCall(lost, out);
        }
        const double begun = Now();
        {
            IN_HOOK("TestNext");
            lost.lock();
            RawLog("locks test 3: the lock of a call that never came back was had by its own thread after %.2f s",
                   Now() - begun);
            lost.unlock();
        }
    }}.join();

    // The same, with another thread waiting before the one that lost its call comes back.
    std::atomic<bool> held{false};
    std::thread again{[&held] {
        jmp_buf out;
        if (!setjmp(out)) {
            TestLostCall(lost, out);
        }
        held = true;
        usleep(700000);
        IN_HOOK("TestNext");
    }};
    while (!held) {
        usleep(1000);
    }
    start = Now();
    lost.lock();
    RawLog("locks test 4: the lock of a call that never came back was had by a waiting thread after %.2f s",
           Now() - start);
    lost.unlock();
    again.join();
    RawLog("locks test: done");
}
FILE* g_log{};

void Log(const char* format, ...) {
    std::lock_guard lock{g_log_mutex};
    if (!g_log) {
        char path[600];
        std::snprintf(path, sizeof path, "%s/fg-%d.log", Dir(), getpid());
        g_log = std::fopen(path, "a");
        if (!g_log) {
            return;
        }
        std::setvbuf(g_log, nullptr, _IOLBF, 0);
    }
    std::fprintf(g_log, "%10.3f ", Now());
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
}

/// The calling thread's name as the kernel has it: Wine hands the names a game gives its threads
/// on, so the log can say which part of the game made a call.
const char* ThreadName() {
    static thread_local char name[17];
    if (prctl(PR_GET_NAME, name) != 0) {
        name[0] = 0;
    }
    return name;
}

bool ControlFile(const char* name, bool remove) {
    char path[600];
    std::snprintf(path, sizeof path, "%s/%s", Dir(), name);
    if (access(path, F_OK) != 0) {
        return false;
    }
    if (remove) {
        unlink(path);
    }
    return true;
}

const bool g_selftest = EnvFloat("SC_FG_SELFTEST", 0.0f) != 0.0f;
/// Tests: an empty pipeline barrier stands in for DLSS's kernel launch, every view counts as handed to it.
const bool g_test_inputs = g_selftest || EnvFloat("SC_FG_TEST_INPUTS", 0.0f) != 0.0f;
const bool g_trace = EnvFloat("SC_FG_TRACE", 0.0f) != 0.0f;
const bool g_hud_capture = EnvFloat("SC_FG_HUD_CAPTURE", 1.0f) != 0.0f;
std::atomic<double> g_trace_until{0.0};
void TraceCount(VkCommandBuffer cmd, int kind);
struct Device;
void ForgetHudPass(VkCommandBuffer cmd);
void ForgetImage(Device& d, VkImage image);
void TracePresent(uint32_t device, VkQueue queue);
extern std::atomic<bool> g_scan_armed;
void ScanWrite();

// ----------------------------------------------------------------------------- dispatch tables

struct Instance {
    VkInstance handle{};
    char engine[40]{}; ///< who made it, as it says itself: the game's renderer, DXVK, ...
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkDestroyInstance DestroyInstance{};
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{};
    PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2{};
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties{};
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2{};
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures{};
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2{};
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties{};
};

// What a display says about when a frame reached it (only recent Vulkan headers have this).
#ifdef VK_EXT_present_timing
#define PRESENT_TIMING_FUNCTIONS(X)                                                                \
    X(GetPastPresentationTimingEXT) X(GetCalibratedTimestampsKHR) X(WaitForPresent2KHR)            \
    X(GetSwapchainTimeDomainPropertiesEXT) X(SetSwapchainPresentTimingQueueSizeEXT)
#else
#define PRESENT_TIMING_FUNCTIONS(X)
#endif

#define DEVICE_FUNCTIONS(X)                                                                        \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(AllocateCommandBuffers)                \
    X(FreeCommandBuffers) X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView)    \
    X(GetImageViewHandleNVX) X(GetImageViewHandle64NVX) X(CmdCuLaunchKernelNVX)                    \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR)   \
    X(QueuePresentKHR) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)             \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory)              \
    X(BindImageMemory) X(MapMemory) X(CreateCommandPool) X(DestroyCommandPool)                     \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier)          \
    X(CmdCopyImage) X(CmdCopyImageToBuffer) X(CmdCopyBufferToImage) X(CmdClearColorImage)          \
    X(QueueSubmit) X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence)           \
    X(WaitForFences) X(ResetFences) X(GetMemoryFdKHR)                                              \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)                      \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)                 \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool)  \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler)           \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdPushConstants) X(CmdFillBuffer)  \
    X(CmdCopyBuffer) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) X(CmdWriteTimestamp) \
    X(GetQueryPoolResults)                                                                         \
    X(SetLatencySleepModeNV) X(LatencySleepNV) X(SetLatencyMarkerNV) X(WaitForPresentKHR)          \
    PRESENT_TIMING_FUNCTIONS(X)

/// Only looked at by the trace (SC_FG_TRACE=1): the structure of the game's frame.
#define TRACE_FUNCTIONS(X)                                                                         \
    X(CreateRenderPass) X(CreateRenderPass2) X(CreateFramebuffer) X(CmdBeginRenderPass)            \
    X(CmdBeginRenderPass2) X(CmdEndRenderPass) X(CmdEndRenderPass2) X(CmdBeginRendering)           \
    X(CmdEndRendering) X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect)   \
    X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) X(CmdDispatchIndirect) X(CmdBlitImage)  \
    X(CmdResolveImage) X(CmdExecuteCommands) X(QueueSubmit2) X(CmdCopyImage2) X(CmdBlitImage2)

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    bool general{}; ///< already taken to the GENERAL layout
    int fd{-1};     ///< exported memory, until another device imports it
    VkDeviceSize alloc_size{};
};

struct HostBuffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* data{};
    VkDeviceSize size{};
};

struct Candidate {
    uint64_t image{};
    uint32_t width{}, height{};
};

/// DLSS's inputs as last seen, and this layer's copies of the vectors and the depth.
struct Inputs {
    Lock mutex{LOCK_INPUTS};
    uint64_t depth{};
    uint32_t width{}, height{};
    VkFormat depth_format{};
    std::vector<Candidate> vectors; ///< last RG16F sampled image per size
    bool launched{};                ///< a kernel was launched since the evaluation began
    bool fresh{};                   ///< a new evaluation began and has not been copied yet
    // Copies (GENERAL layout), made at copy_width x copy_height. A ring: evaluation n goes to slot
    // n % SLOTS, so a reader is never handed an image that is being rewritten. Their memory is
    // exportable: the game presents on another device than it renders on.
    static constexpr uint32_t SLOTS = 6;
    Image vector_copy[SLOTS], depth_copy[SLOTS];
    double slot_time[SLOTS]{};
    uint32_t generation{}; ///< changes whenever the copies are re-made
    uint32_t failed_width{}, failed_height{};
    std::vector<Image> retired;
    // The game's HUD layer (a display-size target the HUD is drawn into before it is put over the
    // scene), copied the same way when its render pass ends.
    Image hud_copy[SLOTS];
    Image scene_copy[SLOTS]; ///< the finished frame before the HUD layer is put over it
    double hud_time[SLOTS]{};
    uint32_t hud_width{}, hud_height{}, hud_generation{}, hud_failed_width{};
    std::atomic<uint64_t> hud_serial{};
    VkBuffer depth_hop{};
    VkDeviceMemory depth_hop_memory{};
    uint32_t copy_width{}, copy_height{};
    std::atomic<uint64_t> serial{}; ///< evaluations copied so far
    // Copies are recorded into the game's command buffers, and the game records ahead: the newest
    // recorded copy can belong to a later frame than the one being presented. What has been
    // submitted to the queue is the firmer link: command buffer -> count at its copy, and the
    // highest count submitted.
    std::unordered_map<uint64_t, uint64_t> tags, hud_tags;
    std::atomic<uint32_t> tag_count{};
    std::atomic<uint64_t> submitted{}, hud_submitted{};
    // A HUD capture and the evaluation that had been submitted when it was (in the same submit or
    // an earlier one) belong to one frame: capture -> evaluation, per ring slot.
    uint64_t pair_hud[SLOTS]{}, pair_input[SLOTS]{};
    HostBuffer self_vectors, self_depth; ///< self-test: read-back of the copies
    // diagnostics
    std::atomic<uint32_t> n_handle32{}, n_handle64{}, n_launch{}, n_fresh{}, n_copy_calls{}, n_not_fresh{},
        n_no_depth{}, n_no_vectors{}, n_bad_format{}, n_unknown_view{};
};

struct Fg;
struct Device;

/// Another device's input copies, imported.
struct Imports {
    const Device* from{};
    uint32_t generation{};
    bool ok{};
    Image vectors[Inputs::SLOTS], depth[Inputs::SLOTS];
    uint32_t hud_generation{};
    bool hud_ok{};
    Image hud[Inputs::SLOTS], scene[Inputs::SLOTS];
    std::vector<Image> retired;
};

/// One frame on its way from the device the game renders on to the one it presents on (see
/// NoteHandover): the captures that had been submitted when it was handed over.
struct ChainFrame {
    uint64_t number{};                         ///< running count of hand-overs; 0: none yet
    uint64_t input{}, hud{};                   ///< newest submitted evaluation copy / HUD capture
    uint64_t input_recorded{}, hud_recorded{}; ///< newest recorded ones
    uint64_t passes{}, frame_copies{};         ///< running counts of HUD-layer passes begun / frame copies seen
    double time{};                             ///< when it was handed over
    float slept_ms{};                          ///< how long the limiter held it back
    float gpu_ms{}; ///< the GPU's time for the frame, when it was done before the frame was due (else 0)
    float own_ms{};   ///< the game's own time for it: from the frame before going on to this one arriving
    float guard_ms{}; ///< how long its submit was kept back for the frame before (see GuardHandover)
    bool gated{};     ///< let on by the gate (when the present hook was about to return), not by the timer
    double gpu_done{}; ///< when the GPU was done with it, where the limiter saw that (else 0)
    bool late{};       ///< its copy was submitted only after the game's next frame (see g_late_copies)
    bool lost{};       ///< ... and after that frame was finished: the copy took the next frame's picture
};

struct Device {
    VkDevice handle{};
    VkPhysicalDevice physical{};
    Instance* instance{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
#define X(name) PFN_vk##name name{};
    DEVICE_FUNCTIONS(X)
#undef X
    VkPhysicalDeviceMemoryProperties memory{};
    bool nvx{}; ///< the device was created with the NVX extensions DLSS uses
    Inputs inputs;
    Imports imports;
    ChainFrame chain; ///< the frame whose hand-over was submitted last on this device (g_chain_mutex)
    // A fence behind the frame's submit tells when the GPU is done with it (MarkGpu).
    Lock mark_mutex{LOCK_MARK};
    VkFence mark_fences[4]{};
    bool mark_busy[4]{};
    int mark_current{-1};
    int mark_waiting{-1}; ///< the fence the limiter is waiting on
    int mark_last{-1};    ///< the fence behind the last frame
    int mark_before{-1};  ///< ... behind the frame before that one; -1: none (that frame is finished)
    uint32_t mark_serial[4]{}; ///< how often each fence has been taken for a new frame
    /// The frames submitted after the last hand-overs (the hand-over's number; the frame's fence and
    /// that fence's serial then): when such a frame is finished, the shared image holds its picture.
    struct Drawn {
        uint64_t after{};
        int fence{-1};
        uint32_t serial{};
    } drawn[8];
    double mark_time{};
    Lock fg_mutex{LOCK_FG};
    std::unordered_map<uint64_t, Fg*> swapchains;
    uint32_t index{}; ///< order of creation, for the trace
#define X(name) PFN_vk##name Trace##name{};
    TRACE_FUNCTIONS(X)
#undef X
};

struct ImageRecord {
    VkFormat format{};
    uint32_t width{}, height{};
    bool shared{};    ///< created with external memory: can be seen by another device
    bool swapchain{}; ///< a swapchain's image
};
struct ViewRecord {
    uint64_t image{};
    VkFormat format{};
    VkImageAspectFlags aspect{};
};

Lock g_tables_mutex{LOCK_TABLES};
std::unordered_map<void*, Instance*> g_instances;
std::unordered_map<void*, Device*> g_devices;
std::unordered_map<uint64_t, std::pair<Device*, uint32_t>> g_queues; ///< queue -> device, family
std::unordered_map<uint64_t, Device*> g_command_buffers;
Lock g_records_mutex{LOCK_RECORDS};
std::unordered_map<uint64_t, ImageRecord> g_images;
std::unordered_map<uint64_t, ViewRecord> g_views;
/// The device DLSS runs on (the last one that copied inputs).
Lock g_input_mutex{LOCK_INPUT};
Device* g_input_device{};
std::atomic<Device*> g_input_device_hint{};
/// The device FidelityFX works on: AMD's backend calls a few Vulkan functions by name (below).
Device* g_ffx_device{};

Instance* GetInstance(void* key) {
    std::lock_guard lock{g_tables_mutex};
    const auto it = g_instances.find(key);
    return it == g_instances.end() ? nullptr : it->second;
}
/// The devices once more, for the look every hook begins with: read without a lock, so that no
/// hook waits for another thread (or for one that is being kept from running) before it has begun.
struct DeviceSlot {
    std::atomic<void*> key{};
    std::atomic<Device*> device{};
};
DeviceSlot g_device_slots[16];
Device* GetDevice(void* key) {
    for (const DeviceSlot& slot : g_device_slots) {
        if (slot.key.load(std::memory_order_acquire) == key) {
            if (Device* const d = slot.device.load(std::memory_order_acquire)) {
                return d;
            }
        }
    }
    std::lock_guard lock{g_tables_mutex}; // (more devices than slots, or none such)
    const auto it = g_devices.find(key);
    return it == g_devices.end() ? nullptr : it->second;
}
Device* DeviceOfCommandBuffer(VkCommandBuffer cmd) {
    std::lock_guard lock{g_tables_mutex};
    if (const auto it = g_command_buffers.find(U64(cmd)); it != g_command_buffers.end()) {
        return it->second;
    }
    const auto it = g_devices.find(Key(cmd));
    return it == g_devices.end() ? nullptr : it->second;
}

// ----------------------------------------------------------------------------- GPU helpers

uint32_t MemoryType(const Device& d, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < d.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

/// `exported`: the memory can be handed to another device (image.fd). `import_fd` >= 0: the
/// memory comes from another device's exported image of the same description.
bool CreateOwnImage(Device& d, Image& image, VkFormat format, uint32_t width, uint32_t height,
                    bool exported = false, int import_fd = -1, VkDeviceSize import_size = 0) {
    image = {};
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (exported || import_fd >= 0) {
        image.info.pNext = &external;
    }
    image.info.imageType = VK_IMAGE_TYPE_2D;
    image.info.format = format;
    image.info.extent = {width, height, 1};
    image.info.mipLevels = 1;
    image.info.arrayLayers = 1;
    image.info.samples = VK_SAMPLE_COUNT_1_BIT;
    image.info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult created = d.CreateImage(d.handle, &image.info, nullptr, &image.image);
    image.info.pNext = nullptr;
    if (created != VK_SUCCESS) {
        image.image = VK_NULL_HANDLE;
        if (import_fd >= 0) {
            close(import_fd);
        }
        return false;
    }
    VkMemoryRequirements requirements;
    d.GetImageMemoryRequirements(d.handle, image.image, &requirements);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image.image;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = import_fd;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = import_fd >= 0 ? import_size : requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (exported) {
        dedicated.pNext = &export_info;
        allocate.pNext = &dedicated;
    } else if (import_fd >= 0) {
        dedicated.pNext = &import_info;
        allocate.pNext = &dedicated;
    }
    bool ok = allocate.memoryTypeIndex != UINT32_MAX &&
              d.AllocateMemory(d.handle, &allocate, nullptr, &image.memory) == VK_SUCCESS;
    if (!ok && import_fd >= 0) {
        close(import_fd); // a successful import owns the descriptor
    }
    ok = ok && d.BindImageMemory(d.handle, image.image, image.memory, 0) == VK_SUCCESS;
    if (ok && exported) {
        VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        get.memory = image.memory;
        get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        ok = d.GetMemoryFdKHR(d.handle, &get, &image.fd) == VK_SUCCESS;
    }
    if (!ok) {
        if (image.memory) {
            d.FreeMemory(d.handle, image.memory, nullptr);
        }
        d.DestroyImage(d.handle, image.image, nullptr);
        image = {};
        return false;
    }
    image.alloc_size = allocate.allocationSize;
    return true;
}

void DestroyOwnImage(Device& d, Image& image) {
    if (image.fd >= 0) {
        close(image.fd);
    }
    if (image.image) {
        d.DestroyImage(d.handle, image.image, nullptr);
        d.FreeMemory(d.handle, image.memory, nullptr);
    }
    image = {};
}

bool CreateBufferWithMemory(Device& d, VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                            VkBuffer& buffer, VkDeviceMemory& memory, void** data) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.CreateBuffer(d.handle, &info, nullptr, &buffer) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements;
    d.GetBufferMemoryRequirements(d.handle, buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    // Host buffers are read back: cached memory when there is any (uncached reads are very slow).
    allocate.memoryTypeIndex = host ? MemoryType(d, requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                                    : UINT32_MAX;
    if (allocate.memoryTypeIndex == UINT32_MAX) {
        allocate.memoryTypeIndex = MemoryType(
            d, requirements.memoryTypeBits,
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (allocate.memoryTypeIndex == UINT32_MAX ||
        d.AllocateMemory(d.handle, &allocate, nullptr, &memory) != VK_SUCCESS ||
        d.BindBufferMemory(d.handle, buffer, memory, 0) != VK_SUCCESS ||
        (data && d.MapMemory(d.handle, memory, 0, VK_WHOLE_SIZE, 0, data) != VK_SUCCESS)) {
        if (memory) {
            d.FreeMemory(d.handle, memory, nullptr);
        }
        d.DestroyBuffer(d.handle, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void ImageBarrier(Device& d, VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                  VkImageLayout to) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

void ToGeneral(Device& d, VkCommandBuffer cmd, Image& image) {
    if (!image.general) {
        ImageBarrier(d, cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        image.general = true;
    }
}

VkImageCopy WholeCopy(uint32_t width, uint32_t height) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    return region;
}

// ----------------------------------------------------------------------------- DLSS inputs

/// How long a call of the game's that only passes through here waits for one of the layer's locks.
constexpr double PATIENCE = 0.25;

/// Every view the game hands to the driver. `handle32`: asked through vkGetImageViewHandleNVX.
/// An evaluation is a run of 32-bit handle requests, the input views, kernel launches (with more
/// view requests in between) and one closing 32-bit request; so the first 32-bit request after
/// any kernel launch starts the next evaluation.
void NoteView(Device& d, VkImageView view, VkDescriptorType type, bool handle32) {
    ViewRecord view_record;
    ImageRecord image_record;
    {
        // (DLSS is in the middle of the game's frame: what is noted here is not worth holding it
        // for. A lock that is not to be had at once costs this evaluation's inputs, no more.)
        const HeldFor lock{g_records_mutex, PATIENCE};
        if (!lock) {
            return;
        }
        const auto view_it = g_views.find(U64(view));
        if (view_it == g_views.end()) {
            ++d.inputs.n_unknown_view;
            return;
        }
        view_record = view_it->second;
        const auto image_it = g_images.find(view_record.image);
        if (image_it == g_images.end()) {
            return;
        }
        image_record = image_it->second;
    }
    Inputs& in = d.inputs;
    ++(handle32 ? in.n_handle32 : in.n_handle64);
    const HeldFor lock{in.mutex, PATIENCE};
    if (!lock) {
        return;
    }
    if (handle32 && in.launched) {
        ++in.n_fresh;
        in.fresh = true;
        in.launched = false;
    }
    if (view_record.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        in.depth = view_record.image;
        in.width = image_record.width;
        in.height = image_record.height;
        in.depth_format = image_record.format;
    } else if (view_record.format == VK_FORMAT_R16G16_SFLOAT &&
               (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
        for (Candidate& candidate : in.vectors) {
            if (candidate.width == image_record.width && candidate.height == image_record.height) {
                candidate.image = view_record.image;
                return;
            }
        }
        if (in.vectors.size() < 8) {
            in.vectors.push_back({view_record.image, image_record.width, image_record.height});
        }
    }
}

/// DLSS is about to run in `cmd` (the game's command buffer): copy the vectors and the depth.
void CopyInputs(Device& d, VkCommandBuffer cmd) {
    Inputs& in = d.inputs;
    if (g_input_device_hint.load() != &d) {
        std::lock_guard global{g_input_mutex};
        g_input_device = &d;
        g_input_device_hint = &d;
    }
    const HeldFor lock{in.mutex, PATIENCE};
    if (!lock) {
        return;
    }
    ++in.n_copy_calls;
    if (!in.fresh) {
        ++in.n_not_fresh;
        return;
    }
    in.fresh = false; // one copy per evaluation, whether or not it succeeds
    uint64_t vectors = 0;
    for (const Candidate& candidate : in.vectors) {
        if (candidate.width == in.width && candidate.height == in.height) {
            vectors = candidate.image;
        }
    }
    if (!in.depth || !vectors ||
        (in.depth_format != VK_FORMAT_D32_SFLOAT && in.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        ++(!in.depth ? in.n_no_depth : !vectors ? in.n_no_vectors : in.n_bad_format);
        return;
    }
    if (in.copy_width != in.width || in.copy_height != in.height) {
        // First use, or the render size changed. The old copies are left to the device's end: a
        // command buffer still in flight may reference them.
        if (in.failed_width == in.width && in.failed_height == in.height) {
            return;
        }
        const bool exported = d.GetMemoryFdKHR != nullptr;
        // One buffer for the depth's way into a colour image, big enough for any render size.
        const VkDeviceSize hop_size = static_cast<VkDeviceSize>(4096) * 2304 * 4;
        Image vector_copy[Inputs::SLOTS], depth_copy[Inputs::SLOTS];
        VkBuffer hop{};
        VkDeviceMemory hop_memory{};
        bool ok = true;
        for (uint32_t i = 0; ok && i < Inputs::SLOTS; ++i) {
            ok = CreateOwnImage(d, vector_copy[i], VK_FORMAT_R16G16_SFLOAT, in.width, in.height, exported) &&
                 CreateOwnImage(d, depth_copy[i], VK_FORMAT_R32_SFLOAT, in.width, in.height, exported);
        }
        ok = ok && static_cast<VkDeviceSize>(in.width) * in.height * 4 <= hop_size &&
             (in.depth_hop ||
                    CreateBufferWithMemory(d, hop_size,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           false, hop, hop_memory, nullptr));
        if (!ok) {
            Log("inputs: no memory for the %ux%u copies", in.width, in.height);
            for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
                DestroyOwnImage(d, vector_copy[i]);
                DestroyOwnImage(d, depth_copy[i]);
            }
            in.failed_width = in.width;
            in.failed_height = in.height;
            return;
        }
        for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
            // The old copies stay until the device ends: a command buffer in flight may use them.
            if (in.vector_copy[i].image) {
                in.retired.push_back(in.vector_copy[i]);
                in.retired.push_back(in.depth_copy[i]);
            }
            in.vector_copy[i] = vector_copy[i];
            in.depth_copy[i] = depth_copy[i];
        }
        if (!in.depth_hop) {
            in.depth_hop = hop;
            in.depth_hop_memory = hop_memory;
        }
        ++in.generation;
        in.copy_width = in.width;
        in.copy_height = in.height;
        Log("inputs: device %p render size %ux%u, depth format %d", static_cast<void*>(&d), in.width, in.height,
            in.depth_format);
    }
    const uint32_t slot = static_cast<uint32_t>(in.serial.load() % Inputs::SLOTS);
    Image& vector_slot = in.vector_copy[slot];
    Image& depth_slot = in.depth_copy[slot];
    in.slot_time[slot] = Now();
    ToGeneral(d, cmd, vector_slot);
    ToGeneral(d, cmd, depth_slot);
    // The game's images are being sampled by DLSS; their layouts are left alone. The NVIDIA
    // driver does not act on image layouts, so naming the transfer layout is enough and nothing
    // has to be transitioned and restored.
    const VkImageCopy whole = WholeCopy(in.width, in.height);
    d.CmdCopyImage(cmd, reinterpret_cast<VkImage>(vectors), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   vector_slot.image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    // Depth -> R32F goes through a buffer: images of a depth and a colour format cannot be copied
    // to each other directly.
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageExtent = {in.width, in.height, 1};
    d.CmdCopyImageToBuffer(cmd, reinterpret_cast<VkImage>(in.depth),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.depth_hop, 1, &region);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = in.depth_hop;
    barrier.size = VK_WHOLE_SIZE;
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    d.CmdCopyBufferToImage(cmd, in.depth_hop, depth_slot.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
    if (g_selftest) {
        const VkDeviceSize size = static_cast<VkDeviceSize>(in.width) * in.height * 4;
        if (!in.self_vectors.buffer) {
            CreateBufferWithMemory(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, in.self_vectors.buffer,
                                   in.self_vectors.memory, &in.self_vectors.data);
            CreateBufferWithMemory(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, in.self_depth.buffer,
                                   in.self_depth.memory, &in.self_depth.data);
            in.self_vectors.size = in.self_depth.size = size;
        }
        d.CmdCopyImageToBuffer(cmd, vector_slot.image, VK_IMAGE_LAYOUT_GENERAL, in.self_vectors.buffer, 1, &region);
        d.CmdCopyImageToBuffer(cmd, depth_slot.image, VK_IMAGE_LAYOUT_GENERAL, in.self_depth.buffer, 1, &region);
    }
    in.tags[U64(cmd)] = in.serial.fetch_add(1) + 1;
    in.tag_count = static_cast<uint32_t>(in.tags.size() + in.hud_tags.size());
}

/// What frame generation reads: the newest copies, on this device or imported from the one DLSS
/// runs on.
/// Where the present hook is, kept in a small file (STAGE-<pid> in the log directory) so that a
/// hang can be told from outside: 0 not in the hook, 1 entered, 2 waiting for an earlier frame's
/// work, 3 recording, 4 waiting for a swapchain image, 5 pacing sleep, 6 presenting a generated
/// frame, 7 presenting the real frame, 8 the game's own present (untouched frame), 9 dump wait.
void Stage(int stage) {
    static char* map = [] {
        char path[600];
        std::snprintf(path, sizeof path, "%s/STAGE-%d", Dir(), getpid());
        const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || ftruncate(fd, 64) != 0) {
            return static_cast<char*>(nullptr);
        }
        void* memory = mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        return memory == MAP_FAILED ? static_cast<char*>(nullptr) : static_cast<char*>(memory);
    }();
    if (map) {
        std::snprintf(map, 64, "stage %d since %.3f\n", stage, Now());
    }
    Me().crumb->step.store(900000u + static_cast<uint32_t>(stage), std::memory_order_relaxed); // (see Crumb)
}

/// The copy to use at a present: the newest submitted one, or (when submits are not seen, or it
/// fell far behind) the newest recorded one.
uint64_t SubmittedCopy(uint64_t recorded, uint64_t submitted) {
    return submitted && submitted <= recorded && recorded - submitted <= 3 ? submitted : recorded;
}

/// The copy that belongs to this present. Presents and copies both come one per frame, so they are
/// handed out in order: when the game has already submitted the next frame's copy, the newest one
/// is not this frame's. `last`: the one handed out at the last present. Whether the hand-out is a
/// frame off as a whole is found by comparing pictures (see CheckSync) and corrected with `adjust`:
/// by that many copies, once. `lag`: how many copies behind the newest the hand-out usually is
/// (kept up to date here); after a break it starts again at that distance.
uint64_t NextInOrder(uint64_t last, uint64_t submitted, int* adjust = nullptr, float* lag = nullptr) {
    if (last && submitted <= last) {
        return submitted;
    }
    if (!last || submitted - last > 5) {
        const uint64_t behind = lag ? static_cast<uint64_t>(*lag + 0.5f) : 0;
        return submitted > behind + last ? submitted - behind : submitted;
    }
    uint64_t next = last + 1;
    if (adjust && *adjust) {
        const int64_t moved = static_cast<int64_t>(next) + *adjust;
        next = static_cast<uint64_t>(std::clamp<int64_t>(moved, 1, static_cast<int64_t>(submitted)));
        *adjust = 0;
    }
    if (lag) {
        *lag += (static_cast<float>(submitted - next) - *lag) * 0.05f;
    }
    return next;
}

/// The command buffers of one vkQueueSubmit call: notes which copies are now on their way. True
/// when an evaluation copy was among them: the submit of a frame.
bool NoteSubmitted(Device& d, const VkCommandBuffer* buffers, size_t count) {
    Inputs& in = d.inputs;
    if (!in.tag_count.load()) {
        return false;
    }
    std::lock_guard lock{in.mutex};
    bool frame = false;
    uint64_t hud = 0;
    for (size_t i = 0; i < count; ++i) {
        if (const auto it = in.tags.find(U64(buffers[i])); it != in.tags.end()) {
            in.submitted = std::max(in.submitted.load(), it->second);
            in.tags.erase(it);
            frame = true;
        }
        if (const auto it = in.hud_tags.find(U64(buffers[i])); it != in.hud_tags.end()) {
            hud = std::max(hud, it->second);
            in.hud_tags.erase(it);
        }
    }
    if (hud > in.hud_submitted.load()) {
        in.hud_submitted = hud;
        in.pair_hud[(hud - 1) % Inputs::SLOTS] = hud;
        in.pair_input[(hud - 1) % Inputs::SLOTS] = in.submitted.load();
    }
    if (in.tags.size() > 64) { // command buffers that were never submitted
        in.tags.clear();
    }
    if (in.hud_tags.size() > 64) {
        in.hud_tags.clear();
    }
    in.tag_count = static_cast<uint32_t>(in.tags.size() + in.hud_tags.size());
    return frame;
}

// ----------------------------------------------------------------------------- frame chain
// The game renders on one device and presents on another (D3D12, through vkd3d-proton). At the end
// of a frame its thread submits the frame on the first and then records, on the second, a copy
// out of the image both devices share; vkd3d's queue thread submits that copy later and presents.
// Presents can lag behind the rendering (vkd3d lets the game get up to three frames ahead), so the
// newest capture is not always the presented frame's, and counting presents against captures
// slips whenever a frame comes without one. The link that holds: the captures submitted when the
// copy is recorded are that frame's; the copy's command buffer carries them to its submit, and the
// last hand-over submitted before a present is the frame that present shows.

Lock g_chain_mutex{LOCK_CHAIN};
std::unordered_map<uint64_t, ChainFrame> g_chain_tags; ///< command buffer of a hand-over copy -> its frame
std::atomic<bool> g_chain_pending{};
std::atomic<uint64_t> g_chain_count{};
std::atomic<uint64_t> g_hud_pass_count{}, g_frame_copy_count{};
ChainFrame g_chain_newest; ///< the last frame handed over (under g_chain_mutex)
/// Switches that can be set while the game runs (PARAMS file: chain, nohud, wait, shift), to
/// compare in place: the frame chain on/off; a frame without a HUD capture restarts the
/// interpolation (0) or goes on with the last capture, as before the chain (1); the wait for the
/// generated frames on/off; a shift of the chain by hand, in captures.
std::atomic<float> g_live_chain{1.0f}, g_live_nohud{0.0f}, g_live_wait{1.0f}, g_live_shift{0.0f};
/// A frame without a HUD capture of its own keeps the scene and the HUD apart (PARAMS: gapfill; see
/// RecordUnmix). A frame copy recorded apart from the HUD pass still makes a capture (PARAMS: cross;
/// see NoteFrameCopy).
std::atomic<float> g_live_gapfill{EnvFloat("SC_FG_GAPFILL", 1.0f)}, g_live_cross{EnvFloat("SC_FG_CROSS", 1.0f)};
std::atomic<uint32_t> g_cross_captures{}; ///< captures made that way, for the statistics
/// The check of the game's vectors against the picture (PARAMS: vectors; see RecordVectorCheck).
std::atomic<float> g_live_vectors{EnvFloat("SC_FG_FIX_VECTORS", 1.0f)};
/// HUD content that moves over the screen is put where it is on its way in the generated frames
/// (PARAMS: markers; see RecordMarkers). Off unless asked for, which sc-turbo does at launch
/// (proto/hud-motion/hudmotion.py is the reference the passes are checked against).
std::atomic<float> g_live_markers{EnvFloat("SC_FG_MARKERS", 0.0f)};
/// ... except pictures that change where they stand, the game's map (PARAMS: pictures=0 moves them
/// too; see hud_pieces.comp).
std::atomic<float> g_live_pictures{EnvFloat("SC_FG_PICTURES", 1.0f)};
/// The limiter's gate and guard (PARAMS: gate, guard; see Limiter and GuardHandover).
std::atomic<float> g_live_gate{EnvFloat("SC_FG_GATE", 1.0f)}, g_live_guard{EnvFloat("SC_FG_GUARD", 1.0f)};
/// This layer's GPU work before the game's next frame (PARAMS: first; see WaitForGenerated).
std::atomic<float> g_live_first{EnvFloat("SC_FG_FIRST", 1.0f)};
/// The game's next frame is kept back while the GPU is still at work on the one before
/// (PARAMS: gpuwait; see WaitForGenerated). Off unless asked for: where the GPU is the slower
/// one the picture shown is a frame's time newer for it (measured: 46 ms old instead of 82 with
/// three generated frames), but a frame queued on the GPU is also what keeps it at work while
/// the game takes long over one, and without it such a hitch costs a frame (measured: up to 5 %
/// fewer frames with a hitch every second, 1 to 3 % in two steady cases, none in the others).
std::atomic<float> g_live_lag{EnvFloat("SC_FG_GPUWAIT", 0.0f)};
std::atomic<uint32_t> g_lag_waits{}, g_lag_us{}; ///< such waits and their time, since the last statistics line
/// What the frame that is handed over next waited like that (it is also part of g_guard_ms).
std::atomic<float> g_gpu_wait_ms{};
/// ... and went on the moment the GPU was done, with no present hook's work put first (for
/// Limiter::gpu_paced).
std::atomic<bool> g_gpu_at_once{};
/// Such waits that ran out, in a row; and no more of them until then. (A GPU that needs longer
/// than that for a frame, or a fence that does not tell: the game is not to be held on it.)
std::atomic<uint32_t> g_gpu_wait_failed{};
std::atomic<double> g_gpu_wait_off_until{};

/// A wait for the GPU has ended, `waited` seconds long, with the GPU done or not.
void GpuWaited(double waited, bool done) {
    if (waited > 0.0001) {
        ++g_lag_waits;
        g_lag_us += static_cast<uint32_t>(waited * 1e6);
        g_gpu_wait_ms = g_gpu_wait_ms.load() + static_cast<float>(waited * 1000.0);
    }
    if (done) {
        g_gpu_wait_failed = 0;
    } else if (++g_gpu_wait_failed >= 4) {
        g_gpu_wait_failed = 0;
        g_gpu_wait_off_until = Now() + 60.0;
        static std::atomic<uint32_t> logged{};
        if (logged.fetch_add(1) < 4) {
            Log("fg: the GPU was not done with a frame in time, four times in a row: frames do not wait for it for 60 s");
        }
    }
}
/// How the multiplier is chosen (PARAMS: fill): 1 the fewest generated frames that fill the
/// display, 0 the most that do not hold the game back (see where it is chosen, in Generate).
std::atomic<float> g_live_fill{EnvFloat("SC_FG_FILL", 1.0f)};
/// Whether the multiplier follows the game's rate at all (PARAMS: adapt; 1: it does, up to what
/// was asked for, 0: it is held at what was asked for, whatever the game's rate: a player's "x3"
/// as opposed to "Auto").
std::atomic<float> g_live_adapt{EnvFloat("SC_FG_ADAPT", 1.0f)};
bool FollowsRate() {
    static const bool allowed = EnvFloat("SC_FG_MAX_BASE", 62.0f) > 0.0f;
    return allowed && g_live_adapt.load() != 0.0f;
}
/// ... and whether a held game is probed for what it could do (PARAMS: probe; 0: taken from the
/// time its handing-over thread and the GPU need, which is more than the game can do).
std::atomic<float> g_live_probe{EnvFloat("SC_FG_PROBE", 1.0f)};
/// ... and the GPU is asked to keep its speed up (the boost of VK_NV_low_latency2; PARAMS: boost).
/// The game leaves it well short of full load, and a GPU that has let its clocks drop takes longer
/// over the generated frames, which every frame waits for.
std::atomic<float> g_live_boost{EnvFloat("SC_FG_BOOST", 0.0f)};
/// ... and the thread that presents runs at a higher priority: N makes its nice value -N, as far
/// as the user's limits allow (PARAMS: priority; 0: left as it is). In the busy places of the game
/// more threads want to run than there are CPUs, and this one sleeps and wakes for every frame.
std::atomic<float> g_live_priority{EnvFloat("SC_FG_PRIORITY", 0.0f)};
/// ... and every frame sent out asks the display when it got there (PARAMS: timing). vkd3d-proton
/// asks that only for frames presented with vsync, and the game presents without.
std::atomic<float> g_live_timing{EnvFloat("SC_FG_TIMING", 0.0f)};
/// A present with such a request has failed: no more of them in this process.
std::atomic<bool> g_own_timing_failed{};

float Level(const float* ring, uint32_t count);

/// Holds the game to the frame rate the display has room for. The wait is on the game's own
/// thread, at the hand-over, when the frame is already with the GPU. Holding presents back instead
/// (which is what a wait in the present hook does) stalls vkd3d's queue thread; the game then
/// renders frames ahead of the ones shown: two more frames of delay, for nothing.
///
/// Two things let a frame on. The timer: one frame per `interval`, what the display has room for.
/// And the gate: the present hook shows a frame's generated frames one refresh apart and returns
/// only with the last, so with three of them it is busy for nearly the whole frame time. A frame
/// handed over while it is busy waits in vkd3d's queue, and the next one behind it: a frame time of
/// delay that never goes away again, and the game, which expects its frame to be copied out of the
/// shared image at once, draws the next one over it first (the picture then is a frame ahead of
/// its captures). So a frame is let on when the hook of the frame before it is about to return:
/// `advance` before that, which is what the game and the GPU need for the next frame beyond the
/// hook's own time, so that the wait never costs a frame. It is taken from what happens: for
/// every frame, how much earlier than the hook's return the one before it had to be let on for
/// this one to be there in time (measured where the GPU was seen to finish it, or where the hook
/// had to wait for it); the advance is the most that any of the last frames needed.
///
/// The gate is for a game that is faster than the present hook, where the hook cannot keep up
/// with what the timer lets through. Where it can, the timer is all there is: a frame that
/// comes late is made up for by the next, and what waits behind a hook for a moment is gone a
/// few frames later. And a game that needs longer for its frame than a hook does at its quickest
/// is never held by the gate: there a wait is a frame's time lost. (Unless it only looks slow
/// because it is itself waiting for its presents, frames queued as deep as vkd3d lets them.
/// Hooks that find their frame waiting many times in a row tell.)
struct Limiter {
    static constexpr double LEAD = 0.0005; ///< the least advance: the frame's way to the present hook
    std::atomic<double> interval{};  ///< seconds from frame to frame; 0: no limit
    std::atomic<double> published{}; ///< when the presenting side last set it
    /// How long the last frame waited for the present hook beyond the usual: frames are queued
    /// behind the presents (after a hitch, say), each of them a frame time of delay. (For where
    /// the timer is all there is; where the gate is at work nothing queues.)
    std::atomic<double> queued{};
    Lock mutex{LOCK_LIMITER};
    double last_release{};
    double last_on{}; ///< when the last frame went on, held or not
    /// The frame let on last was held by the timer and went on at the timer's moment: the game
    /// is keeping up with the timer. (Not one the gate held longer, for a present hook; nor one
    /// that came too late to be held at all. See WaitForGenerated.)
    bool last_timed{};
    /// Frames lately whose submit had to wait for the GPU and went on the moment it was done
    /// (WaitForGenerated): one more for each, up to 8, one less for each that did not. From 4 on
    /// the GPU is what the game's frames wait for, not the present hooks.
    std::atomic<uint32_t> gpu_paced{};
    // since the presenting side last read them
    uint32_t frames{}, held{}, gpu_frames{}, gpu_late{}, gated{}, own_frames{};
    double slept{}, gpu_seconds{}, own_seconds{};
    // A probe: one frame let on early, to see what the game does with the time. A game that is
    // held shows only how long the thread that hands frames over needs for one; what is in front
    // of that thread (the game's main thread) works on meanwhile and is never seen waiting. Let
    // on early, the thread either has its next frame as soon as ever, and the game can do that
    // much more; or it has to wait for what is in front of it, and how long says what the game
    // can do. (The choice of the multiplier asks for probes, see Generate.)
    double probe_advance{};   ///< asked for: the next frame goes on this much early; 0: none
    double probe_gap{};       ///< the last frame was one: it went on this long after the one before it; 0: it was none
    double probe_result{};    ///< the game's frame time by the last probe: the gap, and what the thread then waited
    uint32_t probe_results{}; ///< probes that have come to a result
    float own_ring[15]{};     ///< the game's own time for its last frames (but those after a probe)
    uint32_t own_count{};

    // The present hook's progress, for the gate.
    Lock gate_mutex{LOCK_GATE};
    std::condition_variable_any gate;
    uint64_t hook_frame{}; ///< the frame (its hand-over's number) whose present hook was entered last
    bool in_hook{};        ///< ... and has not returned yet
    bool hook_generating{}; ///< ... and has frames of its own to show
    double hook_free{};    ///< when it expects to return; after it has: when it did
    double hook_least{};   ///< what a hook takes at its quickest (its frames as close as the display allows); 0: nothing
    double hook_ready{};   ///< how long the last hook took to have its generated frames; 0: it had none to wait for
    double advance{LEAD};  ///< how long before a hook's return the next frame is let on
    float need_ring[32]{}; ///< what the last frames needed for that (seconds; see where the hook begins)
    uint32_t need_count{};
    double lead_before{-1.0}; ///< how long before the last hook's return its frame's successor was let on; < 0: after it
    bool lead_gated{};        ///< ... by the gate
    double return_before{};   ///< when the hook before the last one returned; 0: not known
    uint32_t waiting{};    ///< hooks in a row that found their frame waiting
    static constexpr uint32_t QUEUED = 8; ///< that many: frames are queued and stay so
    static constexpr uint32_t QUEUED_LONG = 32; ///< ... and it is the hooks they wait for, whatever the GPU says
    uint32_t queued_long{QUEUED_LONG}; ///< ... as it stands: twice as many after every time that did not last
    uint32_t gated_since{};            ///< frames the gate has let on since a frame was last held on that count
    uint64_t generated_frame{}; ///< the last frame whose hook has its generated frames (or has returned without)
    uint32_t first_waits{};     ///< frames whose submit waited for that (WaitForGenerated), since last read
    uint32_t first_ahead{};     ///< ... past the timer, because the game was a frame ahead of the presents
    uint32_t first_timeouts{};  ///< such waits in a row that ran out
    uint32_t first_unbegun{};   ///< waits in a row that ended with the hook they waited for not even begun
    bool first_gave_up{};       ///< the wait was given up, and has not been of use since
    double first_off_time{10.0}; ///< for how long it is given up the next time
    uint32_t first_useful{};     ///< waits that were of use since it was last given up
    double first_seconds{};
    // From where a frame's submit may wait (WaitForGenerated) to the game's coming to the limiter
    // with a frame: what such a wait has to leave of the time until the timer lets the frame on.
    // Next to nothing for a game that hands a frame over as soon as it has submitted it; all of
    // the game's own time for one that does it the other way round. (With `mutex`.)
    float lead_ring[32]{};
    uint32_t lead_count{};
    std::atomic<double> lead{}; ///< the most of the last frames, but for the odd one
    uint32_t gate_timeouts{}; ///< frames in a row that waited for a hook that did not come
    double gate_off_until{};
    // The guard (GuardHandover), since the presenting side last read them.
    uint32_t guard_waits{}, guard_timeouts{}, guard_failed{};
    double guard_seconds{};
    double guard_off_until{};
    double guard_off_time{30.0}; ///< for how long it is off the next time
};
Limiter g_limiter;
std::atomic<float> g_guard_ms{}; ///< the guard's last wait, for the frame that is handed over next
/// The frame handed over last went on early (a probe, see LimitFrame): the present hook in front of
/// it is still at work then, wherever the limiter is, and the guard sees to its copy.
std::atomic<bool> g_went_on_early{};
/// No wait before a frame's submit until then (WaitForGenerated): it was of no use several times in a row.
std::atomic<double> g_first_off_until{};
/// When the game's frame was about to go to the GPU: the first one since the last hand-over; 0: none.
std::atomic<double> g_frame_going{};
/// Calls of VK_NV_low_latency2 seen since the last statistics line (see HookLatencySleepNV).
std::atomic<uint32_t> g_latency_sleeps{}, g_latency_markers{};

/// The present hook has been entered for frame `frame`, and expects to return at `free`.
/// `generating`: it has frames of its own to show (without them it takes no time).
void GateEnter(uint64_t frame, double free, bool generating) {
    Limiter& l = g_limiter;
    {
        std::lock_guard lock{l.gate_mutex};
        l.hook_frame = frame;
        l.in_hook = true;
        l.hook_generating = generating;
        l.hook_free = free;
        l.hook_ready = 0.0;
        if (!generating) {
            l.hook_least = 0.0;
        }
    }
    l.gate.notify_all();
}

/// It has its generated frames, `ready` after it began, and knows better now when it will return,
/// and what a hook like it takes at its quickest.
void GatePlan(double free, double least, double ready) {
    Limiter& l = g_limiter;
    {
        std::lock_guard lock{l.gate_mutex};
        if (!l.in_hook) {
            return;
        }
        l.hook_free = free;
        l.hook_least = least;
        l.hook_ready = ready;
        l.generated_frame = l.hook_frame; // (this is called when the generated frames are ready)
    }
    l.gate.notify_all();
}

/// Its generated frames are done on the GPU. (They go out a moment later, each at the usual level:
/// the game's next frame does not have to wait for that, see WaitForGenerated.)
void GateGenerated() {
    Limiter& l = g_limiter;
    {
        std::lock_guard lock{l.gate_mutex};
        if (!l.in_hook) {
            return;
        }
        l.generated_frame = l.hook_frame;
    }
    l.gate.notify_all();
}

/// It returns.
void GateLeave() {
    Limiter& l = g_limiter;
    {
        std::lock_guard lock{l.gate_mutex};
        if (!l.in_hook) {
            return;
        }
        l.in_hook = false;
        l.hook_free = Now();
        l.generated_frame = std::max(l.generated_frame, l.hook_frame);
    }
    l.gate.notify_all();
}

/// The multiplier has changed. Whether the wait for the generated frames (WaitForGenerated) is
/// of use belongs to the multiplier that was: given up at x2, where the GPU was the slower one
/// and frames queued behind the presents, it stayed given up at x3 for the 10 to 40 s that were
/// left. The generated frames then shared the GPU with the game's, took twice as long, and x3
/// was found to have no room for them; and without the wait a picture was now and then drawn
/// over. (How long it is given up the next time stays as it is: that is for a wait that comes
/// to nothing whenever it is tried, whatever the multiplier.)
void GateMultiplierChanged() {
    Limiter& l = g_limiter;
    std::lock_guard lock{l.gate_mutex};
    l.first_timeouts = l.first_unbegun = 0;
    l.first_gave_up = false;
    g_first_off_until = 0.0;
}

/// The frame's submit has gone to the queue. An empty one behind it, with a fence, tells when the
/// GPU is done with the frame; the limiter looks at it while it waits anyway.
void MarkGpu(Device& d, VkQueue queue) {
    static const bool enabled = EnvFloat("SC_FG_GPU_TIME", 1.0f) != 0.0f;
    if (!enabled || !d.CreateFence || g_limiter.interval.load() <= 0.0) {
        std::lock_guard lock{d.mark_mutex};
        d.mark_last = d.mark_before = -1;
        return;
    }
    std::lock_guard lock{d.mark_mutex};
    const int last = d.mark_last;
    d.mark_current = d.mark_last = d.mark_before = -1;
    for (int i = 0; i < 4; ++i) {
        if (i == d.mark_waiting) {
            continue;
        }
        if (d.mark_busy[i]) {
            if (d.WaitForFences(d.handle, 1, &d.mark_fences[i], VK_TRUE, 0) != VK_SUCCESS) {
                continue; // that frame is still being worked on
            }
            d.ResetFences(d.handle, 1, &d.mark_fences[i]);
            d.mark_busy[i] = false;
        }
        if (!d.mark_fences[i]) {
            VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (d.CreateFence(d.handle, &info, nullptr, &d.mark_fences[i]) != VK_SUCCESS) {
                d.mark_fences[i] = VK_NULL_HANDLE;
                return;
            }
        }
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (d.QueueSubmit(queue, 1, &empty, d.mark_fences[i]) == VK_SUCCESS) {
            d.mark_busy[i] = true;
            ++d.mark_serial[i];
            d.mark_current = d.mark_last = i;
            d.mark_time = Now();
            // (A fence that was taken again had signalled: that frame is finished.)
            d.mark_before = last >= 0 && last != i && d.mark_busy[last] ? last : -1;
        }
        return;
    }
}

/// Whether the GPU is still at work on the frame before the one submitted last. Then that frame
/// is not waiting for a present hook, and the one behind it waits for the GPU whatever is done here.
bool BehindGpu(Device& d) {
    std::lock_guard lock{d.mark_mutex};
    const int i = d.mark_before;
    return i >= 0 && d.mark_busy[i] && d.WaitForFences(d.handle, 1, &d.mark_fences[i], VK_TRUE, 0) != VK_SUCCESS;
}

/// On the game's thread, as it hands a frame over: waits until the frame may go on (see Limiter).
void LimitFrame(Device& input, ChainFrame& frame) {
    Limiter& l = g_limiter;
    const bool gate_on = g_live_gate.load() != 0.0f;
    double interval = l.interval.load();
    bool hooks_keep_up = true; // with the timer: then the timer is all there is (see Limiter)
    if (gate_on) {
        std::lock_guard lock{l.gate_mutex};
        hooks_keep_up = interval >= l.hook_least + 0.001;
    }
    if (hooks_keep_up) {
        // Frames queued behind the presents (a hitch leaves them there): held a little longer
        // until they are gone. Only for more than half a frame of waiting, which the GPU's time
        // for a frame alone does not explain. (Hooks that hurry show their frames 3 % closer
        // together, no more: left to that, a frame's worth of delay takes a second or two to go.
        // Where the gate is at work it sees to this itself.)
        interval += std::clamp(0.3 * (l.queued.load() - 0.5 * interval), 0.0, 0.3 * interval);
    }
    const double start = Now();
    const double going = g_frame_going.exchange(0.0);
    // What this frame waited for the GPU before it was submitted (WaitForGenerated): not the
    // game's own time where the limiter decides when to let it on, but part of what the game
    // can do, which is what a probe asks about.
    const double for_gpu = g_gpu_wait_ms.exchange(0.0f) * 0.001;
    {
        const uint32_t paced = l.gpu_paced.load(); // (written on this thread only)
        l.gpu_paced = g_gpu_at_once.exchange(false) ? std::min(paced + 1, 8u) : paced ? paced - 1 : 0;
    }
    VkFence fence{};
    double marked = 0.0;
    const bool limiting = interval > 0.0 && start - l.published.load() <= 0.25;
    {
        std::lock_guard lock{input.mark_mutex};
        if (limiting && input.mark_current >= 0 && start - input.mark_time < 0.05) {
            fence = input.mark_fences[input.mark_current];
            marked = input.mark_time;
            input.mark_waiting = input.mark_current; // not to be taken for another frame meanwhile
        }
        input.mark_current = -1;
    }
    const auto release = [&] {
        if (fence) {
            std::lock_guard lock{input.mark_mutex};
            input.mark_waiting = -1;
        }
    };
    double due = start;
    double own = 1.0; // not known: as if the game were slow
    double probe = 0.0;           // this frame goes on that much early (Limiter::probe_advance)
    double released_before = 0.0; // when the frame before it went on
    {
        std::lock_guard lock{l.mutex};
        released_before = l.last_on;
        if (going > 0.0 && start - going < 0.25) {
            l.lead_ring[l.lead_count++ % 32] = static_cast<float>(start - going);
            float sorted[32];
            const uint32_t n = std::min(l.lead_count, 32u), k = n >= 8 ? 2 : 0;
            std::copy_n(l.lead_ring, n, sorted);
            std::nth_element(sorted, sorted + k, sorted + n, std::greater<float>());
            l.lead = sorted[k];
        }
        if (l.last_on > 0.0 && start - l.last_on < 0.25) {
            // The game's own time for the frame: from being let on with the last one to coming
            // here with this one. (Without what the guard made it wait: that is this layer's doing.)
            own = std::max(start - l.last_on - g_guard_ms.load() * 0.001, 0.0);
            frame.own_ms = static_cast<float>(own * 1000.0);
            l.own_seconds += own;
            ++l.own_frames;
        }
        l.last_on = start;
        if (limiting) {
            ++l.frames;
            due = l.last_release + interval;
            if (due <= start + 0.0002 || due - start > 0.1) {
                due = start; // late: the game is the slower one (or the first frame after a pause)
            }
            if (own < 1.0) {
                if (l.probe_gap > 0.0) {
                    // The frame before went on early. What this one took longer than frames
                    // usually do, the thread waited for what is in front of it. Longer than
                    // all but the odd one of the last, that is: measured against the middle
                    // one, every other frame of a game that waited for nothing would seem to
                    // have waited (frame times differ), and a game that could do twice the
                    // rate asked about was found to fall just short of it half the time.
                    float sorted[15];
                    const uint32_t n = std::min(l.own_count, 15u);
                    const uint32_t k = n >= 10 ? n - 3 : n >= 5 ? n - 2 : 0;
                    std::copy_n(l.own_ring, n, sorted);
                    std::nth_element(sorted, sorted + k, sorted + n);
                    if (n >= 5) {
                        l.probe_result = l.probe_gap + std::max(own + for_gpu - static_cast<double>(sorted[k]), 0.0);
                        ++l.probe_results;
                    }
                } else {
                    l.own_ring[l.own_count++ % 15] = static_cast<float>(own + for_gpu);
                }
            }
            l.probe_gap = 0.0;
            probe = std::exchange(l.probe_advance, 0.0);
        }
    }
    if (!limiting) {
        return;
    }
    // When the frame may go on, as things stand (with l.gate_mutex held): the timer, and the gate.
    constexpr double GRACE = 0.0015;
    const double deadline = start + 0.1;
    bool for_hook = false; // waiting for a present hook that has not begun
    bool against_gpu = false; // ... with the GPU still at work on the frame in front (see below)
    uint32_t queued_next = 0;
    const auto target = [&](double now) {
        double when = due;
        for_hook = false;
        // (See Limiter: only hooks that cannot keep up with the timer, and a game that is faster
        // than they are, or frames that stay queued.)
        const bool queued = l.waiting >= Limiter::QUEUED;
        if (gate_on && now >= l.gate_off_until && interval < l.hook_least + 0.001 && (own < l.hook_least || queued)) {
            if (l.in_hook && l.hook_frame + 1 >= frame.number) {
                // The frame before this one is being shown: this one is wanted when that is done.
                when = std::max(when, l.hook_free - l.advance);
            } else if (l.in_hook) {
                // A frame is still in front of that one. Waiting for a hook: then this one not
                // before that frame's hook has begun. But not while the GPU is still at work on
                // it: with a GPU that is the slower one frames wait for that, how many of them is
                // the game's own business, and a hook is running most of the time anyway. (A
                // frame can be kept on the GPU by something else than work; hooks that never once
                // had to wait for their frame in a long time tell. A frame held on that count
                // brings the frames back in step. Where that does not last, a GPU that sets the
                // pace after all, it takes twice as many such hooks the next time: a frame held
                // for most of a frame's time every few seconds is a hitch every few seconds.)
                const bool behind = BehindGpu(input);
                if (!behind || l.waiting >= l.queued_long) {
                    when = deadline;
                    for_hook = true;
                    against_gpu |= behind;
                }
            } else if (l.hook_frame + 1 < frame.number && now - l.hook_free < GRACE) {
                // A hook has just returned and a frame is waiting for it: the next begins any moment.
                when = std::max(when, l.hook_free + GRACE);
            }
        }
        return std::min(when, deadline);
    };
    // The wait. While the GPU is still at work on the frame it is a wait for that, a little at a
    // time, so that the moment it is done is known: what the gate's advance is made from.
    bool by_gate = false, given_up = false, open = fence != VK_NULL_HANDLE, waited_for_gpu = false;
    double early = 0.0; // how much sooner than it would have the frame goes on (a probe)
    for (;;) {
        double now = 0.0, when = 0.0;
        {
            std::unique_lock gate{l.gate_mutex};
            now = Now();
            const double usual = target(now);
            // (A probe is not for a frame that waits for a hook to begin: that is no time of day.)
            when = probe > 0.0 && !for_hook ? std::max(usual - probe, start) : usual;
            if (!(when <= deadline + 0.001)) {
                when = now; // (not a number, or past the longest wait there is: the frame goes on)
            }
            by_gate = usual > due + 0.0002;
            if (now >= when - 0.00005) {
                early = probe > 0.0 && !for_hook ? std::max(usual - now, 0.0) : 0.0;
                frame.gated = by_gate;
                // A hook that does not come (frames handed over that nobody presents): after a
                // few of those the gate stays open for a while.
                l.gate_timeouts = for_hook ? l.gate_timeouts + 1 : 0;
                if (l.gate_timeouts >= 5) {
                    l.gate_timeouts = 0;
                    l.gate_off_until = now + 10.0;
                    given_up = true;
                }
                // (See above. The usual number again once the gate has had frames to let on for
                // a good while after such a hold: it was the hooks, then.)
                if (against_gpu) {
                    queued_next = l.queued_long = std::min(l.queued_long * 2, 16384u);
                    l.gated_since = 0;
                } else if (by_gate && ++l.gated_since == 1000) {
                    l.queued_long = Limiter::QUEUED_LONG;
                }
                break;
            }
            if (!open || when - now < 0.0015) {
                l.gate.wait_for(gate, std::chrono::duration<double>(when - now));
                continue;
            }
        }
        // (Not to the very end: a wait that times out may come back late. And not for long at a
        // time: the moment the frame may go on can move.)
        waited_for_gpu = true;
        const auto timeout = static_cast<uint64_t>(std::min(when - now - 0.001, 0.002) * 1e9);
        if (input.WaitForFences(input.handle, 1, &fence, VK_TRUE, timeout) == VK_SUCCESS) {
            open = false;
            const double done = Now();
            std::lock_guard lock{l.mutex};
            ++l.gpu_frames;
            l.gpu_seconds += done - marked;
            frame.gpu_ms = static_cast<float>((done - marked) * 1000.0);
            frame.gpu_done = done;
        }
    }
    if (given_up) {
        Log("fg: frames waited for a present call that did not come: the limiter goes by its timer alone for 10 s");
    }
    const double end = Now();
    if (against_gpu) {
        static std::atomic<uint32_t> logged{};
        if (logged.fetch_add(1) < 8) {
            Log("fg: frames stayed queued behind the present calls: one held for %.0f ms to bring them back in step "
                "(the next time after %u hooks that find their frame waiting)",
                (end - start) * 1000.0, queued_next);
        }
    }
    release();
    std::lock_guard lock{l.mutex};
    l.last_on = end;
    l.gpu_late += open && waited_for_gpu;
    if (early > 0.0003) {
        g_went_on_early = true;
    }
    if (early > 0.0003 && released_before > 0.0 && end - released_before < 0.25) {
        l.probe_gap = end - released_before; // what comes of it shows when the next frame is here
    } else if (probe > 0.0 && l.probe_advance == 0.0) {
        l.probe_advance = probe; // this frame had no wait to cut short: the next one, then
    }
    if (end - start <= 0.0002 && early <= 0.0) {
        l.last_release = start;
        l.last_timed = false;
        return;
    }
    // (A frame that went on early leaves the timer where it was: the next is held to the old step.)
    l.last_release = by_gate ? end + early : due;
    // (Not one the gate held until the hook in front returned sooner than it had said: by then
    // the gate asks for nothing, and the frame goes on well after the timer's moment all the same.)
    l.last_timed = !by_gate && early <= 0.0 && end - start > 0.0005 && end - due < 0.0005;
    ++l.held;
    l.gated += by_gate;
    l.slept += end - start;
    frame.slept_ms = static_cast<float>((end - start) * 1000.0);
}

std::atomic<uint64_t> g_chain_handed{}, g_chain_copied{}; ///< the newest hand-over let on / submitted (its number)
/// The newest hand-over that the game's next frame has been submitted after: from then on the
/// shared image is being drawn over. A copy that is submitted only then may take the wrong
/// picture; how often that happens is counted (it is what the gate and the guard are there to
/// prevent, and should be nothing). Not for the frames handed over before generation was set up:
/// the first present hook does that and takes a good while over it, the game's first frames are
/// on their way by then, and one of them goes like that every time. And all of this counts on
/// the game handing a frame over after it has submitted it: the other way round the frame drawn
/// "after" a hand-over is that frame itself.
std::atomic<uint64_t> g_chain_drawn_after{};
std::atomic<uint32_t> g_late_copies{}, g_lost_copies{}; ///< ... and those submitted when that next frame was finished
std::atomic<double> g_fg_ready_time{}; ///< when generation was last set up; 0: not yet

/// Before the game's next frame goes to the GPU. The game draws every frame into the one image its
/// two devices share and counts on the copy out of it being made at once; that copy is submitted
/// by vkd3d's queue thread, which may still be inside the present hook of the frame before. When
/// the last frame is finished, its copy has not been submitted and the present hook is busy, the
/// next frame waits here until it has been: it would overwrite the picture the copy is to take.
/// (The gate keeps this from happening; this is the net under it, for a hitch or a wrong guess,
/// and like the gate it is only there where the hooks are what frames wait for.)
void GuardHandover(Device& d) {
    Limiter& l = g_limiter;
    const bool after_early = g_went_on_early.exchange(false);
    const uint64_t handed = g_chain_handed.load();
    if (g_live_guard.load() == 0.0f || g_chain_copied.load() >= handed || l.interval.load() <= 0.0) {
        return;
    }
    const double start = Now();
    if (start - l.published.load() > 0.25) {
        return; // nothing is being presented
    }
    {
        // A frame the GPU still works on is not copied before it is done, and the next one comes
        // behind it on the GPU anyway: nothing to wait for.
        std::lock_guard lock{d.mark_mutex};
        if (d.mark_last < 0 || !d.mark_busy[d.mark_last] ||
            d.WaitForFences(d.handle, 1, &d.mark_fences[d.mark_last], VK_TRUE, 0) != VK_SUCCESS) {
            return;
        }
    }
    bool failed = false, switched_off = false;
    double waited = 0.0, off_time = 0.0;
    {
        std::unique_lock gate{l.gate_mutex};
        // (Only where the gate is at work, hooks that cannot keep up with the timer: where they
        // can, a copy is not kept waiting for a whole frame of the game's, and a wait here would
        // only cost the game time. Except behind a frame that went on early, a probe: that one
        // is waiting for the hook in front of it, by as much as it was early.)
        if (start < l.guard_off_until || (!after_early && l.interval.load() >= l.hook_least + 0.001)) {
            return;
        }
        for (;;) {
            const double now = Now();
            if (g_chain_copied.load() >= handed) {
                break;
            }
            // The copy is submitted when the present hooks in front of it have returned. No hook
            // running and none begun for a moment: nothing is on its way, and nothing to wait for.
            if (!l.in_hook && now - l.hook_free > 0.0015 && now - start > 0.0015) {
                break;
            }
            if (now >= start + 0.04) {
                failed = true;
                break;
            }
            l.gate.wait_for(gate, std::chrono::duration<double>(l.in_hook ? start + 0.04 - now : 0.0005));
        }
        waited = Now() - start;
        ++l.guard_waits;
        l.guard_seconds += waited;
        l.guard_timeouts += failed;
        // A wait that ends without the copy having been submitted was of no use, however it
        // ended. Several in a row, and the guard is off for a while; for longer every time,
        // until it is of use again. (A game that hands its frame over before it submits it, if
        // there is one: that copy cannot be submitted before the very submit that waits here.)
        if (g_chain_copied.load() >= handed) {
            l.guard_failed = 0;
            l.guard_off_time = 30.0;
        } else if (++l.guard_failed >= 8) {
            l.guard_failed = 0;
            off_time = l.guard_off_time;
            l.guard_off_time = std::min(off_time * 2.0, 480.0);
            l.guard_off_until = start + off_time;
            switched_off = true;
        }
    }
    g_guard_ms = g_guard_ms.load() + static_cast<float>(waited * 1000.0);
    if (switched_off) {
        static std::atomic<uint32_t> logged{};
        if (logged.fetch_add(1) < 6) {
            Log("fg: the game's frames kept waiting for a copy that was not submitted: no guard for %.0f s", off_time);
        }
    }
}

/// Before the game's next frame goes to the GPU: this layer's work for the frame before it goes
/// first. A hook has its generated frames a few milliseconds after it begins when the GPU has
/// nothing else to do. With the game's next frame on the GPU at the same time both take longer;
/// the hook is the one that cannot wait (its frames go out late, the hook after it begins late,
/// and the limiter then takes that for what a hook needs), and the game's frame is finished no
/// sooner for having begun sooner: the GPU's work is the same either way. So the frame is
/// submitted when the hook of the frame before it has its generated frames. Only for as long as
/// the game has the time anyway, that is until the limiter's timer would let it on: a game that
/// is the slower one never waits here.
///
/// (Tried and dropped: not to wait while the GPU has nothing to do, the frame before finished
/// and its hook not begun because the one before that still has frames to show. Measured, that
/// gained nothing where the GPU is the slower one, and lost a good part of this where the hooks
/// are: a frame submitted then is still on the GPU when the hook does begin.)
bool WaitForGenerated(Device& d) {
    Limiter& l = g_limiter;
    const double interval = l.interval.load();
    const uint64_t handed = g_chain_handed.load();
    if (g_live_first.load() == 0.0f || interval <= 0.0 || !handed) {
        return false;
    }
    const double start = Now();
    if (start - l.published.load() > 0.25 || start < g_first_off_until.load()) {
        return false; // nothing is being presented, or the wait has been of no use (see below)
    }
    const double limit = start + 0.06; // whatever happens
    double until = 0.0;
    bool by_timer = false; // the timer held the frame before and let it on: the game keeps up with it
    {
        // Not past the moment the limiter would let the frame on anyway. That can be most of a
        // frame's time away: a frame that went on long before the hook in front of it returns
        // waits that long for its own hook to begin, and the copy of it is not made any sooner
        // either. To give up before that puts the game's next frame on the GPU just when the
        // hook does begin, which is the worst moment there is: the generated frames then take
        // three times as long, the hooks with them, and that is how it stays.
        //
        // Less what the game still has to do before it comes to the limiter (Limiter::lead):
        // the wait may take what the limiter would make the game wait, and no more.
        std::lock_guard lock{l.mutex};
        until = std::min(l.last_release + interval - 0.0005 - l.lead.load(), limit);
        if (!(until <= limit)) {
            until = limit; // (not a number)
        }
        by_timer = l.last_timed;
    }
    const bool gpu_tells = start >= g_gpu_wait_off_until.load(); // (see GpuWaited)
    const bool for_gpu = g_live_lag.load() != 0.0f && gpu_tells;
    bool extended = false, given_up = false, gpu = false, gpu_done = false;
    double waited = 0.0, gpu_waited = 0.0, off_time = 0.0;
    {
        std::unique_lock gate{l.gate_mutex};
        // (The hook of the frame handed over last has begun before this submit went on: the wait
        // is for something that comes.)
        const auto of_use = [&] {
            l.first_unbegun = 0;
            l.first_gave_up = false;
            // (Given up for the shortest time again only after a good while of use: a wait that
            // comes to nothing whenever it is tried is not tried every ten seconds.)
            if (++l.first_useful == 1000) {
                l.first_off_time = 10.0;
            }
        };
        if (l.generated_frame >= handed) {
            of_use();
            l.first_timeouts = 0;
            return false;
        }
        // Past that moment in two cases.
        //
        // The hook that is waited for has begun: its generated frames are a few milliseconds
        // away, and the GPU is at work on them. A frame submitted now shares the GPU with them
        // and is finished no sooner than one submitted when they are done, but they take two or
        // three times as long, the hook with them, the next hook begins that much later, and
        // the frame after this one meets the same: a few slow frames in a row, with frames
        // queued and now and then one drawn over. (It happens when the hooks have slipped
        // behind the timer by a little, after a slow stretch for instance; a hook only begins
        // this late where the hooks or the GPU set the pace, so the game has the time, and what
        // it waits here moves its timer once and is not lost again.)
        //
        // Or the game is ahead of the presents by a whole frame: the frame before this one is
        // finished, and its copy has still not been submitted because the present hooks in
        // front of it are not done (a hitch has left frames queued, or the hooks take longer
        // than was thought). What the game submits then is drawn over the picture that copy is
        // to take, and it comes on the GPU together with the generated frames; and a frame that
        // is queued is a frame time of delay. (A frame the GPU still works on is not waiting
        // for a hook: there the timer's word stands. Nor is one with no hook running in front
        // of it.)
        const auto ahead = [&] {
            if (!l.in_hook) {
                return false;
            }
            if (l.hook_frame >= handed) {
                return l.hook_generating;
            }
            if (l.first_gave_up || g_chain_copied.load() >= handed) {
                return false;
            }
            std::lock_guard lock{d.mark_mutex};
            return d.mark_last >= 0 && d.mark_busy[d.mark_last] &&
                   d.WaitForFences(d.handle, 1, &d.mark_fences[d.mark_last], VK_TRUE, 0) == VK_SUCCESS;
        };
        // All of that is for a frame whose predecessor is finished and waits for its hook, and
        // for a game the timer holds: one that keeps up with the timer loses nothing by a frame
        // submitted while the GPU is still at work on the one before, and now and then needs
        // it. (A GPU that is almost as slow as the timer falls behind for a few frames at a
        // time. The frame submitted behind the unfinished one lets the game go on in step with
        // the timer meanwhile, and the GPU catches up. Kept back until the GPU was done, the
        // game was late for the timer every time, and that time is not made up: measured, 58
        // frames a second where 60 were to be had.)
        //
        // A game the timer does not hold, with the GPU still at work on the frame before when
        // it comes here with the next (PARAMS: gpuwait): there the GPU is what the frames wait
        // for. Submitted now, the frame would wait its turn on the GPU, drawn from what the
        // game knew a frame's time earlier than it had to be, and behind it the game would run
        // on as far ahead as vkd3d lets it. So it waits here until the GPU is done with the
        // frame before, and goes on at that moment: the GPU is never without work, and nothing
        // queues on it. (What the drivers' low-latency modes do for a game that asks for them.)
        // The hook's work is not put first there: that would leave the GPU without work while
        // the hook gets ready, every frame (measured: 2 % fewer frames). Only then can be seen
        // whether the finished frame's copy has to wait for a hook in front of it, the last
        // case above: a frame submitted without looking has now and then been finished before
        // that copy was made.
        //
        // One case is left where a game the timer holds is kept back for the GPU too: a present
        // hook in front of the frame before is still running when the timer's moment comes
        // (hooks that have fallen a frame behind), so that frame's copy cannot be submitted,
        // and the GPU is still at work on it. Submitted then, this frame comes on the GPU right
        // behind it, and has been finished before that copy was made (the picture drawn over:
        // seen once or twice a minute at the game's size). It waits until the GPU is done with
        // the frame before, and then as in the last case above. (Whatever PARAMS gpuwait says:
        // left to that switch at first, a desktop that used the GPU as well brought one or two
        // such pictures in 20 s with it off.)
        // (`whatever`: also where frames do not wait for the GPU as a rule, PARAMS gpuwait=0.)
        const auto busy = [&](bool whatever) -> VkFence { // the frame before's fence, where the GPU is still at work on it
            std::lock_guard lock{d.mark_mutex};
            const int i = d.mark_last;
            if (!(for_gpu || (whatever && gpu_tells)) || i < 0 || !d.mark_busy[i] || d.mark_waiting >= 0 ||
                d.WaitForFences(d.handle, 1, &d.mark_fences[i], VK_TRUE, 0) == VK_SUCCESS) {
                return VK_NULL_HANDLE;
            }
            d.mark_waiting = i; // not to be taken for another frame meanwhile
            return d.mark_fences[i];
        };
        const auto wait_for = [&](VkFence fence) { // (with the gate free meanwhile)
            gpu = true;
            gate.unlock();
            const double before = Now();
            gpu_done = d.WaitForFences(d.handle, 1, &fence, VK_TRUE, 60'000'000ull) == VK_SUCCESS;
            {
                std::lock_guard lock{d.mark_mutex};
                d.mark_waiting = -1;
            }
            gpu_waited += Now() - before;
            gate.lock();
        };
        const auto go = [&](bool paced) { // the frame goes on, after a wait for the GPU and no other
            gate.unlock();
            g_guard_ms = g_guard_ms.load() + static_cast<float>(gpu_waited * 1000.0);
            GpuWaited(gpu_waited, gpu_done);
            g_gpu_at_once = paced && gpu_done && gpu_waited > 0.0003;
        };
        const auto copy_waits = [&] { // the frame before's copy, for a hook in front of it
            return l.in_hook && l.hook_frame < handed && !l.first_gave_up && g_chain_copied.load() < handed;
        };
        if (const VkFence fence = by_timer ? VkFence{} : busy(false)) {
            wait_for(fence);
            if (!gpu_done || l.generated_frame >= handed || !l.in_hook || l.hook_frame >= handed || !ahead()) {
                go(true);
                return true;
            }
            extended = true;
            until = limit;
        } else if (until - start < 0.0003) {
            if (!ahead()) {
                const VkFence behind = copy_waits() ? busy(true) : VkFence{};
                if (!behind) {
                    return false;
                }
                wait_for(behind);
                if (!gpu_done || !ahead()) {
                    go(false);
                    return true;
                }
            }
            extended = true;
            until = limit;
        }
        for (;;) {
            const double now = Now();
            if (l.generated_frame >= handed) {
                break;
            }
            if (now >= until) {
                if (extended) {
                    break;
                }
                if (!ahead()) {
                    const VkFence behind = copy_waits() ? busy(true) : VkFence{};
                    if (!behind) {
                        break;
                    }
                    wait_for(behind);
                    if (!gpu_done || !ahead()) {
                        break;
                    }
                }
                extended = true;
                until = limit;
                continue;
            }
            if (!l.in_hook && now - l.hook_free > 0.05) {
                break; // no presents come this way
            }
            l.gate.wait_for(gate, std::chrono::duration<double>(until - now));
        }
        const double end = Now();
        waited = end - start - gpu_waited;
        ++l.first_waits;
        l.first_ahead += extended;
        l.first_seconds += waited;
        // A wait that is of no use, whatever the reason, is given up for a while, and for longer
        // every time until it is of use again: the game is never to be held on a guess gone
        // wrong. Waits past the timer that ran out; and waits that ended with the hook they were
        // for not even begun. That is how it goes for a game that hands a frame over before it
        // submits it (none is known to, but then the hook cannot begin before the very submit
        // that waits for it), or when the frames handed over are not presented.
        const bool came = l.generated_frame >= handed;
        if (came || l.hook_frame >= handed) {
            of_use();
        } else if (!extended) {
            ++l.first_unbegun;
        }
        if (came) {
            l.first_timeouts = 0;
        } else {
            if (extended) {
                l.first_timeouts = end >= limit - 0.001 ? l.first_timeouts + 1 : 0;
            }
            if (l.first_timeouts >= 4 || l.first_unbegun >= (l.first_gave_up ? 2u : 8u)) {
                l.first_timeouts = l.first_unbegun = l.first_useful = 0;
                l.first_gave_up = true;
                off_time = l.first_off_time;
                l.first_off_time = std::min(off_time * 2.0, 320.0);
                g_first_off_until = end + off_time;
                given_up = true;
            }
        }
    }
    g_guard_ms = g_guard_ms.load() + static_cast<float>((waited + gpu_waited) * 1000.0);
    if (gpu) {
        GpuWaited(gpu_waited, gpu_done);
    }
    if (given_up) {
        static std::atomic<uint32_t> logged{};
        if (logged.fetch_add(1) < 6) {
            Log("fg: the game's frames kept waiting for present calls that did not come in time: frames go on "
                "without that wait for %.0f s",
                off_time);
        }
    }
    return gpu;
}

/// Before the game's next frame goes to the GPU, where the wait above is off or had nothing to
/// wait for (PARAMS: gpuwait): the same wait for the GPU to be done with the frame before, and
/// no more than that. Like it, not for a game the timer holds.
void WaitForGpu(Device& d) {
    Limiter& l = g_limiter;
    if (g_live_lag.load() == 0.0f || l.interval.load() <= 0.0) {
        return;
    }
    const double start = Now();
    if (start - l.published.load() > 0.25 || start < g_gpu_wait_off_until.load()) {
        return; // nothing is being presented; or the GPU does not tell (see GpuWaited)
    }
    {
        std::lock_guard lock{l.mutex};
        if (l.last_timed) {
            return;
        }
    }
    VkFence fence{};
    {
        std::lock_guard lock{d.mark_mutex};
        const int i = d.mark_last;
        if (i < 0 || !d.mark_busy[i] || d.mark_waiting >= 0 ||
            d.WaitForFences(d.handle, 1, &d.mark_fences[i], VK_TRUE, 0) == VK_SUCCESS) {
            return;
        }
        fence = d.mark_fences[i];
        d.mark_waiting = i; // not to be taken for another frame meanwhile
    }
    const bool done = d.WaitForFences(d.handle, 1, &fence, VK_TRUE, 50'000'000ull) == VK_SUCCESS;
    {
        std::lock_guard lock{d.mark_mutex};
        d.mark_waiting = -1;
    }
    const double waited = Now() - start;
    g_guard_ms = g_guard_ms.load() + static_cast<float>(waited * 1000.0);
    GpuWaited(waited, done);
    g_gpu_at_once = done && waited > 0.0003;
}

std::atomic<int> g_handover_thread{}, g_submit_thread{};

/// A copy has been recorded on `d`. Out of an image shared between devices, into one of the
/// same size, on a device the game does not render on: a finished frame being handed over.
void NoteHandover(Device& d, VkCommandBuffer cmd, VkImage source, VkImage destination) {
    Device* const hint = g_input_device_hint.load();
    if (!hint || hint == &d) {
        return;
    }
    {
        std::lock_guard lock{g_records_mutex};
        const auto from = g_images.find(U64(source)), to = g_images.find(U64(destination));
        if (from == g_images.end() || to == g_images.end()) {
            return;
        }
        const ImageRecord& a = from->second;
        const ImageRecord& b = to->second;
        if (!a.shared || b.shared || b.swapchain || a.width < 1280 || a.height < 720 || a.width != b.width ||
            a.height != b.height) {
            return;
        }
    }
    ChainFrame frame;
    Device* input = nullptr;
    {
        std::lock_guard global{g_input_mutex};
        input = g_input_device;
        if (!input || input == &d) {
            return;
        }
        const Inputs& in = input->inputs;
        frame.input = in.submitted.load();
        frame.hud = in.hud_submitted.load();
        frame.input_recorded = in.serial.load();
        frame.hud_recorded = in.hud_serial.load();
    }
    frame.passes = g_hud_pass_count.load();
    frame.frame_copies = g_frame_copy_count.load();
    frame.number = g_chain_count.fetch_add(1) + 1;
    // Which of the game's threads do this, for the log: the limiter's wait is on this one.
    const int thread = static_cast<int>(syscall(SYS_gettid));
    if (g_handover_thread.exchange(thread) != thread) {
        static std::atomic<int> logged{};
        if (logged.fetch_add(1) < 12) {
            Log("fg: frames are handed over by thread %d (their submits come from thread %d)", thread,
                g_submit_thread.load());
        }
    }
    LimitFrame(*input, frame);
    frame.guard_ms = g_guard_ms.exchange(0.0f);
    frame.time = Now();
    {
        std::lock_guard lock{g_chain_mutex};
        if (g_chain_tags.size() > 64) { // command buffers that were never submitted
            g_chain_tags.clear();
        }
        g_chain_tags[U64(cmd)] = frame;
        g_chain_newest = frame;
        g_chain_pending = true;
    }
    g_chain_handed = frame.number;
}

/// The command buffers of a submit on a device the game does not render on: a hand-over among
/// them is the frame the next present shows.
void NoteChainSubmitted(Device& d, const VkCommandBuffer* buffers, size_t count) {
    if (!g_chain_pending.load() || g_input_device_hint.load() == &d) {
        return;
    }
    bool found = false;
    {
        std::lock_guard lock{g_chain_mutex};
        for (size_t i = 0; i < count; ++i) {
            if (const auto it = g_chain_tags.find(U64(buffers[i])); it != g_chain_tags.end()) {
                if (it->second.number <= g_chain_drawn_after.load()) {
                    const double ready = g_fg_ready_time.load();
                    const bool counted = ready > 0.0 && it->second.time >= ready;
                    it->second.late = true;
                    g_late_copies += counted;
                    // Late for certain when the frame drawn after it is finished: its fence has
                    // signalled, or has been taken for another frame since (which says the same).
                    if (Device* const input = g_input_device_hint.load()) {
                        std::lock_guard marks{input->mark_mutex};
                        const Device::Drawn& drawn = input->drawn[it->second.number % 8];
                        if (drawn.after == it->second.number && drawn.fence >= 0 &&
                            (input->mark_serial[drawn.fence] != drawn.serial || !input->mark_busy[drawn.fence] ||
                             input->WaitForFences(input->handle, 1, &input->mark_fences[drawn.fence], VK_TRUE, 0) ==
                                 VK_SUCCESS)) {
                            it->second.lost = true;
                            g_lost_copies += counted;
                        }
                    }
                }
                if (it->second.number > d.chain.number) {
                    d.chain = it->second;
                }
                if (it->second.number > g_chain_copied.load()) {
                    g_chain_copied = it->second.number;
                }
                g_chain_tags.erase(it);
                found = true;
            }
        }
        g_chain_pending = !g_chain_tags.empty();
    }
    if (found) {
        { std::lock_guard gate{g_limiter.gate_mutex}; } // a guard that is about to wait sees it, or is woken
        g_limiter.gate.notify_all();
    }
}

struct Picked {
    uint64_t recorded{}; ///< copies recorded so far (serial: the one to use)
    Image* vectors{};
    Image* depth{};
    uint32_t width{}, height{};
    uint64_t serial{};
    double age{}; ///< seconds since the copy was recorded
};

/// `use`: the evaluation to hand out (its count, 1-based); 0 for the newest.
Picked PickInputs(Device& p, bool want_images, uint64_t use = 0) {
    Picked pick;
    std::lock_guard global{g_input_mutex};
    Device* r = g_input_device;
    if (!r) {
        return pick;
    }
    Inputs& in = r->inputs;
    std::lock_guard lock{in.mutex};
    pick.recorded = in.serial.load();
    pick.serial = SubmittedCopy(pick.recorded, in.submitted.load());
    if (!want_images || !pick.serial || !in.copy_width) {
        return pick;
    }
    static const uint64_t lag = static_cast<uint64_t>(EnvFloat("SC_FG_LAG", 0.0f));
    uint64_t which = (use && use <= pick.serial ? use : pick.serial) - 1;
    which -= which >= lag ? lag : 0;
    const uint32_t slot = static_cast<uint32_t>(which % Inputs::SLOTS);
    pick.age = Now() - in.slot_time[slot];
    if (r == &p) {
        pick.vectors = &in.vector_copy[slot];
        pick.depth = &in.depth_copy[slot];
    } else {
        Imports& im = p.imports;
        if (im.from != r || im.generation != in.generation) {
            for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
                if (im.vectors[i].image) {
                    im.retired.push_back(im.vectors[i]);
                }
                if (im.depth[i].image) {
                    im.retired.push_back(im.depth[i]);
                }
                im.vectors[i] = {};
                im.depth[i] = {};
            }
            im.from = r;
            im.generation = in.generation;
            im.ok = true;
            for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
                const int vector_fd = in.vector_copy[i].fd, depth_fd = in.depth_copy[i].fd;
                in.vector_copy[i].fd = in.depth_copy[i].fd = -1;
                im.ok = vector_fd >= 0 && depth_fd >= 0 && im.ok;
                if (vector_fd < 0 || !CreateOwnImage(p, im.vectors[i], VK_FORMAT_R16G16_SFLOAT, in.copy_width,
                                                      in.copy_height, false, vector_fd,
                                                      in.vector_copy[i].alloc_size)) {
                    im.ok = false;
                }
                if (depth_fd < 0 || !CreateOwnImage(p, im.depth[i], VK_FORMAT_R32_SFLOAT, in.copy_width,
                                                     in.copy_height, false, depth_fd,
                                                     in.depth_copy[i].alloc_size)) {
                    im.ok = false;
                }
                im.vectors[i].general = im.depth[i].general = true;
            }
            Log("inputs: %ux%u copies of device %p %s into device %p", in.copy_width, in.copy_height,
                static_cast<void*>(r), im.ok ? "imported" : "could NOT be imported", static_cast<void*>(&p));
        }
        if (!im.ok) {
            return pick;
        }
        pick.vectors = &im.vectors[slot];
        pick.depth = &im.depth[slot];
    }
    pick.width = in.copy_width;
    pick.height = in.copy_height;
    return pick;
}

constexpr VkFormat HUD_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat SCENE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

/// The game is about to put its HUD layer `image` over the finished frame `scene`: copy both.
void CopyHud(Device& d, VkCommandBuffer cmd, VkImage image, VkImage scene, VkImageLayout scene_layout, uint32_t width,
             uint32_t height) {
    Inputs& in = d.inputs;
    std::lock_guard lock{in.mutex};
    if (in.hud_width != width || in.hud_height != height) {
        if (in.hud_failed_width == width) {
            return;
        }
        Image copies[Inputs::SLOTS], scenes[Inputs::SLOTS];
        bool ok = true;
        for (uint32_t i = 0; ok && i < Inputs::SLOTS; ++i) {
            ok = CreateOwnImage(d, copies[i], HUD_FORMAT, width, height, d.GetMemoryFdKHR != nullptr) &&
                 CreateOwnImage(d, scenes[i], SCENE_FORMAT, width, height, d.GetMemoryFdKHR != nullptr);
        }
        if (!ok) {
            Log("inputs: no memory for the %ux%u HUD copies", width, height);
            for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
                DestroyOwnImage(d, copies[i]);
                DestroyOwnImage(d, scenes[i]);
            }
            in.hud_failed_width = width;
            return;
        }
        for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
            if (in.hud_copy[i].image) {
                in.retired.push_back(in.hud_copy[i]);
                in.retired.push_back(in.scene_copy[i]);
            }
            in.hud_copy[i] = copies[i];
            in.scene_copy[i] = scenes[i];
        }
        ++in.hud_generation;
        in.hud_width = width;
        in.hud_height = height;
        Log("inputs: device %p HUD layer %ux%u", static_cast<void*>(&d), width, height);
    }
    const uint32_t slot = static_cast<uint32_t>(in.hud_serial.load() % Inputs::SLOTS);
    ToGeneral(d, cmd, in.hud_copy[slot]);
    ToGeneral(d, cmd, in.scene_copy[slot]);
    const VkImageCopy whole = WholeCopy(width, height);
    d.CmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.hud_copy[slot].image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &whole);
    d.CmdCopyImage(cmd, scene, scene_layout, in.scene_copy[slot].image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    in.hud_time[slot] = Now();
    in.hud_tags[U64(cmd)] = in.hud_serial.fetch_add(1) + 1;
    in.tag_count = static_cast<uint32_t>(in.tags.size() + in.hud_tags.size());
}

/// A HUD layer copy, on this device or imported; null when there is none.
/// `use`: the capture to hand out (its count, 1-based; see LinkFrame); 0 for the newest.
/// `neighbours`: the scene copies of the frame before, (when it is already there) after, and two before; or null.
/// `before`: the scene copy of the capture before this one, when it is still held and was taken
/// right before it: the picture this frame's vectors point into. Null otherwise. `layer_before`:
/// that capture's HUD layer, under the same conditions. `used`: the count of the capture handed out.
/// `max_age`: an older capture is none (seconds).
Image* PickHud(Device& p, uint32_t& width, uint32_t& height, Image** scene = nullptr, uint64_t use = 0,
               Image** neighbours = nullptr, Image** before = nullptr, Image** layer_before = nullptr,
               uint64_t* used = nullptr, double max_age = 0.5) {
    static const uint64_t lag = static_cast<uint64_t>(EnvFloat("SC_FG_HUD_LAG", 0.0f));
    std::lock_guard global{g_input_mutex};
    Device* r = g_input_device;
    if (!r) {
        return nullptr;
    }
    Inputs& in = r->inputs;
    std::lock_guard lock{in.mutex};
    const uint64_t newest = SubmittedCopy(in.hud_serial.load(), in.hud_submitted.load());
    const uint64_t serial = use && use <= newest ? use : newest;
    const bool local = r == &p;
    const auto note_neighbours = [&] {
        if (neighbours) {
            const Image* const scenes = local ? in.scene_copy : p.imports.scene;
            neighbours[0] = serial >= 2 + lag ? const_cast<Image*>(&scenes[(serial - 2 - lag) % Inputs::SLOTS]) : nullptr;
            neighbours[1] = serial < newest ? const_cast<Image*>(&scenes[(serial - lag) % Inputs::SLOTS]) : nullptr;
            neighbours[2] = serial >= 3 + lag ? const_cast<Image*>(&scenes[(serial - 3 - lag) % Inputs::SLOTS]) : nullptr;
        }
        // The capture before sits in a slot that the capture SLOTS later takes over; two more may
        // be recorded before what reads it here has run.
        if (before && serial >= 2 + lag && in.hud_serial.load() + 3 <= serial - lag - 1 + Inputs::SLOTS &&
            in.hud_time[(serial - 1) % Inputs::SLOTS] - in.hud_time[(serial - 2) % Inputs::SLOTS] < 0.25) {
            const Image* const scenes = local ? in.scene_copy : p.imports.scene;
            *before = const_cast<Image*>(&scenes[(serial - 2 - lag) % Inputs::SLOTS]);
            if (layer_before) {
                const Image* const layers = local ? in.hud_copy : p.imports.hud;
                *layer_before = const_cast<Image*>(&layers[(serial - 2 - lag) % Inputs::SLOTS]);
            }
        }
        if (used) {
            *used = serial;
        }
    };
    if (serial <= lag || !in.hud_width || Now() - in.hud_time[(serial - 1) % Inputs::SLOTS] > max_age) {
        return nullptr;
    }
    const uint32_t slot = static_cast<uint32_t>((serial - 1 - lag) % Inputs::SLOTS);
    width = in.hud_width;
    height = in.hud_height;
    if (r == &p) {
        if (scene) {
            *scene = &in.scene_copy[slot];
        }
        note_neighbours();
        return &in.hud_copy[slot];
    }
    Imports& im = p.imports;
    if (im.from != r || im.hud_generation != in.hud_generation) {
        im.hud_generation = in.hud_generation;
        im.hud_ok = true;
        for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
            if (im.hud[i].image) {
                im.retired.push_back(im.hud[i]);
            }
            im.hud[i] = {};
            const int fd = in.hud_copy[i].fd;
            in.hud_copy[i].fd = -1;
            if (fd < 0 || !CreateOwnImage(p, im.hud[i], HUD_FORMAT, width, height, false, fd, in.hud_copy[i].alloc_size)) {
                im.hud_ok = false;
            }
            im.hud[i].general = true;
            if (im.scene[i].image) {
                im.retired.push_back(im.scene[i]);
            }
            im.scene[i] = {};
            const int scene_fd = in.scene_copy[i].fd;
            in.scene_copy[i].fd = -1;
            if (scene_fd < 0 || !CreateOwnImage(p, im.scene[i], SCENE_FORMAT, width, height, false, scene_fd,
                                                in.scene_copy[i].alloc_size)) {
                im.hud_ok = false;
            }
            im.scene[i].general = true;
        }
        Log("inputs: HUD layer copies %s into device %p", im.hud_ok ? "imported" : "could NOT be imported",
            static_cast<void*>(&p));
    }
    if (scene) {
        *scene = &im.scene[slot];
    }
    if (im.hud_ok) {
        note_neighbours();
    }
    return im.hud_ok ? &im.hud[slot] : nullptr;
}

/// Self-test: a second device reads the first device's copies back through the import path.
void SelftestImport(Device& p) {
    VkQueue queue{};
    {
        std::lock_guard lock{g_tables_mutex};
        for (const auto& [handle, owner] : g_queues) {
            if (owner.first == &p) {
                queue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(handle));
            }
        }
    }
    const Picked pick = PickInputs(p, true);
    if (!queue || !pick.vectors || g_input_device_hint.load() == &p) {
        return;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(pick.width) * pick.height * 4;
    HostBuffer vectors, depth;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!CreateBufferWithMemory(p, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, vectors.buffer, vectors.memory, &vectors.data) ||
        !CreateBufferWithMemory(p, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, depth.buffer, depth.memory, &depth.data) ||
        p.CreateCommandPool(p.handle, &pool_info, nullptr, &pool) != VK_SUCCESS ||
        (allocate.commandPool = pool, p.AllocateCommandBuffers(p.handle, &allocate, &cmd)) != VK_SUCCESS ||
        p.CreateFence(p.handle, &fence_info, nullptr, &fence) != VK_SUCCESS) {
        Log("selftest: import read-back could not be set up");
        return;
    }
    if (p.set_loader_data) {
        p.set_loader_data(p.handle, cmd);
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p.BeginCommandBuffer(cmd, &begin);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {pick.width, pick.height, 1};
    p.CmdCopyImageToBuffer(cmd, pick.vectors->image, VK_IMAGE_LAYOUT_GENERAL, vectors.buffer, 1, &region);
    p.CmdCopyImageToBuffer(cmd, pick.depth->image, VK_IMAGE_LAYOUT_GENERAL, depth.buffer, 1, &region);
    p.EndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    p.QueueSubmit(queue, 1, &submit, fence);
    p.WaitForFences(p.handle, 1, &fence, VK_TRUE, 5'000'000'000ull);
    char path[600];
    std::snprintf(path, sizeof path, "%s/selftest-import-%ux%u.bin", Dir(), pick.width, pick.height);
    if (FILE* file = std::fopen(path, "wb")) {
        std::fwrite(vectors.data, 1, size, file);
        std::fwrite(depth.data, 1, size, file);
        std::fclose(file);
    }
    uint32_t hud_w = 0, hud_h = 0;
    Image* hud_scene = nullptr;
    if (Image* hud = PickHud(p, hud_w, hud_h, &hud_scene)) {
        HostBuffer buffer;
        const VkDeviceSize hud_size = static_cast<VkDeviceSize>(hud_w) * hud_h * 8;
        if (CreateBufferWithMemory(p, hud_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buffer.buffer, buffer.memory,
                                   &buffer.data)) {
            p.ResetFences(p.handle, 1, &fence);
            p.ResetCommandBuffer(cmd, 0);
            p.BeginCommandBuffer(cmd, &begin);
            region.imageExtent = {hud_w, hud_h, 1};
            p.CmdCopyImageToBuffer(cmd, hud->image, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1, &region);
            p.EndCommandBuffer(cmd);
            p.QueueSubmit(queue, 1, &submit, fence);
            p.WaitForFences(p.handle, 1, &fence, VK_TRUE, 5'000'000'000ull);
            std::snprintf(path, sizeof path, "%s/selftest-hud-%ux%u.bin", Dir(), hud_w, hud_h);
            FILE* file = std::fopen(path, "wb");
            if (file) {
                std::fwrite(buffer.data, 1, hud_size, file);
            }
            // then the frame without the HUD (RGBA8)
            p.ResetFences(p.handle, 1, &fence);
            p.ResetCommandBuffer(cmd, 0);
            p.BeginCommandBuffer(cmd, &begin);
            p.CmdCopyImageToBuffer(cmd, hud_scene->image, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1, &region);
            p.EndCommandBuffer(cmd);
            p.QueueSubmit(queue, 1, &submit, fence);
            p.WaitForFences(p.handle, 1, &fence, VK_TRUE, 5'000'000'000ull);
            if (file) {
                std::fwrite(buffer.data, 1, hud_size / 2, file);
                std::fclose(file);
            }
        }
    }
    p.DestroyFence(p.handle, fence, nullptr);
    p.DestroyCommandPool(p.handle, pool, nullptr);
}

// ----------------------------------------------------------------------------- frame generation

constexpr uint32_t MAX_GENERATED = FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES;
/// The most generated frames a game frame gets: 3 (x4), or MAX_GENERATED (x8) with the experimental
/// setting (SC_FG_EXPERIMENTAL=1). Only that many outputs, command buffers and extra swapchain images
/// are made, so a launch without it uses no more memory than before the setting existed.
const uint32_t g_generated_cap = EnvFloat("SC_FG_EXPERIMENTAL", 0.0f) != 0.0f ? MAX_GENERATED : std::min(3u, MAX_GENERATED);
constexpr int COMMAND_SETS = 3;
constexpr int HUD_PASSES = 10; ///< the HUD passes' pipelines; one more follows them:
constexpr int UNMIX_PASS = 9;  ///< (the last of them) hud_unmix.comp: a scene worked out, see RecordUnmix
constexpr int PANEL_PASS = HUD_PASSES; ///< the performance panel's, made when the panel is first shown
/// The panel's descriptor sets in a command set: one per generated frame, then the real frame's.
constexpr uint32_t PANEL_SETS = 7 + MAX_GENERATED;
/// HUD content that moves over the screen (RecordMarkers): its pipelines follow the panel's and
/// are made when first needed; its descriptor sets in a command set, one for every pass that
/// RecordMarkers runs, follow the panel's (the composite uses the layer composite's).
constexpr int MARKER_PASS = PANEL_PASS + 1;
enum MarkerPass { // (in the order of PrepareMarkers' table)
    MARKER_MAPS,    ///< hud_moved.comp: the layer's alpha in cells of 4 and 16, what arrived and what left
    MARKER_BLOCKS,  ///< hud_blocks.comp: sums over blocks of 4x4 pixels
    MARKER_OWN,     ///< hud_motion.comp: each tile's own search
    MARKER_AROUND,  ///< hud_around.comp: the answers around a tile, tried on it
    MARKER_PIECES,  ///< hud_pieces.comp: tiles that touch are one piece; which pieces move
    MARKER_FIELD,   ///< hud_field.comp: a smooth field through a piece's motions
    MARKER_REFINE,  ///< hud_refine.comp: each tile against the field
    MARKER_REACH,   ///< hud_reach.comp: the motions that pass through a tile
    MARKER_WHICH,   ///< hud_which.comp: per pixel, whether its content moves
    MARKER_LAYER,   ///< hud_layer_moving.comp: the composite
    MARKER_PASSES
};
constexpr int ALL_PASSES = MARKER_PASS + MARKER_PASSES;
constexpr uint32_t MARKER_SETS = PANEL_SETS + MAX_GENERATED + 1;
constexpr uint32_t MARKER_SET_COUNT = 11; ///< pieces and field run twice
constexpr uint32_t UNMIX_SET = MARKER_SETS + MARKER_SET_COUNT; ///< RecordUnmix's descriptor set, the last
constexpr uint32_t HUD_SET_COUNT = UNMIX_SET + 1;

/// When the GPU is done with each hand-over's copy, seen from a thread of its own, while the display
/// is asked about its frames (timing=1): an empty submit with a fence behind the copy's, and the
/// thread waits for it. (Never destroyed: the thread must not be joinable when the process ends.)
struct DoneWatch {
    Device* device{};
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::pair<VkFence, uint64_t>> waiting; ///< in the order submitted
    std::vector<VkFence> spare;
    std::thread thread;
    std::atomic<bool> stop{};
    std::atomic<uint64_t> frame[64]{};
    std::atomic<double> time[64]{};
};
DoneWatch* const g_done_watch = new DoneWatch;

void WatchDone(Device& d, VkQueue queue, uint64_t frame) {
    DoneWatch& w = *g_done_watch;
    VkFence fence{};
    {
        std::lock_guard lock{w.mutex};
        if (w.stop || (w.device && w.device != &d) || w.waiting.size() > 16) {
            return;
        }
        if (!w.spare.empty()) {
            fence = w.spare.back();
            w.spare.pop_back();
        }
    }
    if (!fence) {
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (d.CreateFence(d.handle, &info, nullptr, &fence) != VK_SUCCESS) {
            return;
        }
    }
    VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const bool sent = d.QueueSubmit(queue, 1, &empty, fence) == VK_SUCCESS;
    {
        std::lock_guard lock{w.mutex};
        if (!sent) {
            w.spare.push_back(fence);
            return;
        }
        w.waiting.emplace_back(fence, frame);
        if (!w.device) {
            w.device = &d;
            w.thread = std::thread([&w] {
                prctl(PR_SET_NAME, "sc_fg-done", 0, 0, 0);
                std::unique_lock lock{w.mutex};
                while (true) {
                    w.wake.wait(lock, [&] { return w.stop || !w.waiting.empty(); });
                    if (w.waiting.empty()) {
                        return; // (stop)
                    }
                    const auto [fence, frame] = w.waiting.front();
                    w.waiting.erase(w.waiting.begin());
                    Device& dev = *w.device;
                    lock.unlock();
                    VkResult result;
                    do {
                        result = dev.WaitForFences(dev.handle, 1, &fence, VK_TRUE, 20'000'000ull);
                    } while (result == VK_TIMEOUT && !w.stop);
                    if (result == VK_SUCCESS) {
                        w.time[frame % 64].store(Now());
                        w.frame[frame % 64].store(frame);
                        dev.ResetFences(dev.handle, 1, &fence);
                    }
                    lock.lock();
                    w.spare.push_back(fence);
                }
            });
        }
    }
    w.wake.notify_one();
}

/// Its device goes: the thread ends, the fences go (the device is idle by then).
void StopDoneWatch(Device& d) {
    DoneWatch& w = *g_done_watch;
    {
        std::lock_guard lock{w.mutex};
        if (w.device != &d) {
            return;
        }
        w.stop = true;
    }
    w.wake.notify_all();
    if (w.thread.joinable()) {
        w.thread.join();
    }
    std::lock_guard lock{w.mutex};
    for (const auto& [fence, frame] : w.waiting) {
        d.DestroyFence(d.handle, fence, nullptr);
    }
    for (VkFence fence : w.spare) {
        d.DestroyFence(d.handle, fence, nullptr);
    }
    w.waiting.clear();
    w.spare.clear();
}

/// One present, kept for a while: what was linked to what, and what it took (EVENTS control file).
struct Event {
    enum : uint32_t {
        CHAINED = 1, DUPLICATE = 2, LAYER = 4, RESET = 8, REUSED = 16, NO_HUD = 32, PASSED = 64, MEASURED = 128,
        CHECKED = 256, GATED = 512, LATE = 1024, LOST = 2048, UNMIXED = 4096
    };
    double time{};                 ///< entry of the present hook
    uint64_t chain{}, hud{}, input{}, newest_hud{}, newest_input{};
    uint32_t handovers{}, hud_captures{}, evaluations{}, passes{}, frame_copies{}; ///< since the last linked frame
    float handover_ms{};           ///< from the hand-over to the present hook
    float slept_ms{}, gpu_ms{};    ///< the limiter's wait, the GPU's time for the game's frame
    float wait_ms{}, hook_ms{};    ///< the wait for this layer's GPU work, the whole hook
    float record_ms{}, ready_ms{}, paced_ms{}; ///< hook entry to its submit / to the generated frames going out; waits between frames
    float own_ms{}, guard_ms{};    ///< the game's own time for the frame; how long its submit was kept back (GuardHandover)
    float shown_ms{};              ///< from the real frame's present call to its reaching the display; 0: not known
    uint32_t multiplier{}, flags{};
    int shift{};
};
constexpr size_t EVENT_COUNT = 4096;

struct Fg {
    VkSwapchainKHR swapchain{};
    VkFormat format{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    bool modified{}; ///< created with the extra images and transfer usages
    bool presented{}; ///< its first present has been logged
    bool failed{};
    bool ready{};

    uint32_t generated{std::clamp<uint32_t>(static_cast<uint32_t>(EnvFloat("SC_FG_MULT", 2.0f)), 2, g_generated_cap + 1) - 1};
    uint32_t max_generated{generated}; ///< what was asked for; `generated` adapts below it to the frame rate
    double multiplier_changed{};
    uint32_t family{};
    Image backbuffer;
    Image outputs[MAX_GENERATED];
    Image zero_vectors, zero_depth;
    // FSR's scene-change detector (a histogram comparison) fires during fast camera turns and then
    // switches interpolation off for several frames. This all-zero stand-in says "no change".
    Image no_scene_change;
    bool scd_on{};
    bool no_cap{};      ///< NOCAP file: frames may go out faster than the display shows them
    double last_out{};  ///< when the last frame (generated or real) was handed to the display queue
    uint32_t held{};    ///< frames held back for the display's rate in this statistics span
    uint32_t window_held{}, window_out{}; ///< the same and all frames sent, since `window_start`
    double window_start{};
    bool binding{};     ///< most frames of the last second were held back: the game could go faster
    bool zero_cleared{};
    // HUD: the previous real frame, and the vectors with the HUD's taken out
    Image previous, masked_vectors, mask, grown[2]; ///< grown: this frame's and the last frame's
    Image checked_vectors, fixed_vectors; ///< the game's vectors after the check against the picture (two passes)
    Image vector_tiles, vector_reach; ///< per 8x8 block of them: a break in it; one near enough (mv_reach.comp)
    HostBuffer tile_lists;            ///< the blocks the two passes work on, with the arguments of their dispatches
    std::vector<HostBuffer> retired_buffers;
    bool check_failed{};
    uint32_t checked_frames{};
    uint32_t grown_index{};
    std::vector<Image> retired;
    struct Hud {
        bool ok{};
        /// detect, apply, composite, layer composite; the vector check: its two passes, the two before them,
        /// the one between them
        VkShaderModule shaders[ALL_PASSES]{};
        VkPipeline pipelines[ALL_PASSES]{};
        VkDescriptorSetLayout set_layout{};
        VkPipelineLayout layout{};
        VkDescriptorPool pool{};
        VkSampler sampler{};
        VkImageView current_view{}, previous_view{}, masked_view{}, mask_view{}, grown_view[2]{};
        VkImageView checked_view{}, fixed_view{}, tiles_view{}, reach_view{};
        VkImageView output_views[MAX_GENERATED]{};
        std::unordered_map<uint64_t, VkImageView> vector_views;
        std::unordered_map<uint64_t, VkImageView> layer_views; ///< of the imported scene and HUD layer copies
        std::vector<VkImageView> retired_views;
    } hud;
    bool hud_off{}, layer_off{}, layer_mode{};
    // Frames that came without a HUD capture of their own, one bit per frame (the newest lowest),
    // and how many came with one since. With gaps the layer is left alone (see Generate).
    uint32_t capture_gaps{}, capture_run{};
    bool capture_hold{};
    // The scene of a frame without a capture, worked out (RecordUnmix): two, as a run of such frames
    // goes from one to the other. `unmixed_last`: the frame before was one, its scene in unmixed[unmixed_index].
    Image unmixed[2];
    uint32_t unmixed_index{}, unmixed_frames{};
    bool unmixed_last{}, unmixed_failed{};
    // The game's final tone curve: presented value per scene value (measured, see MeasureCurve).
    bool curve_ok{}, curve_dirty{};
    double curve_time{};
    uint32_t curve_rejected{};
    double curve_logged{}; ///< when the curve was last written to the log
    int measure_set{-1}; ///< the command set whose probe read-back is still to be evaluated
    Image probe[6]; ///< every 8th pixel of scene, presented frame, HUD layer, the scenes before, after, two before
    HostBuffer probe_buffer[6];
    uint32_t probe_width{}, probe_height{};
    Image curve_image;
    VkImageView curve_view{};
    HostBuffer curve_staging;
    Image test_scene, test_layer; ///< tests only (SC_FG_TEST_LAYER): stand-ins for the game's copies
    uint32_t hud_frames{}, layer_frames{};
    VkCommandPool pool{};
    struct Set {
        VkCommandBuffer main{};
        VkCommandBuffer blit[MAX_GENERATED]{};
        /// detect, apply, one composite per output, the five passes of the vector check, the panel's,
        /// those of the moving HUD content (one for every pass RecordMarkers runs), RecordUnmix's
        VkDescriptorSet hud_sets[HUD_SET_COUNT]{};
        VkFence fence{};
        bool pending{};
    } sets[COMMAND_SETS];
    int set_index{};
    /// This layer's GPU time per frame, by step, while the display is asked about its frames (timing=1):
    /// timestamps around the steps of the interpolation, read when the set's work is done.
    struct GpuClock {
        static constexpr uint32_t MARKS = 7; ///< begin, prepare, flow, interpolation, HUD, end (and one before prepare)
        VkQueryPool pool{};
        bool failed{};
        bool written[COMMAND_SETS]{};
        double period_ms{};               ///< a tick in ms
        std::vector<float> parts[MARKS - 1];
    } clock;
    /// When the game's frame was done on the GPU (the hand-over's copy), against its hand-over and its
    /// present hook: what is left of the time between the two once the GPU is done (timing=1).
    uint64_t done_frame{};
    double done_handover{}, done_hook{};
    std::vector<float> done_after_handover_ms, done_before_hook_ms;
    double real_sent{};           ///< when the game's frame last went out
    /// The performance panel (hud_panel.h): frame rates and pacing, put over every frame that goes
    /// out while it is asked for (control file PANEL; sc-turbo fg-panel).
    struct Panel {
        static constexpr uint32_t GAPS = 512, FRAMES = 128;
        int step{};         ///< 0: not shown; 1 the shown rate, 2 and the game's, 3 and the pacing graph
        int corner{};       ///< 0 top left, 1 top right, 2 bottom left, 3 bottom right
        double checked{};   ///< when the control file was last looked at
        panel_key::Combo key;        ///< the key that steps it on, read in the game's own process (panel_key.h)
        const char* key_source = ""; ///< where it is read from, as last said in the log
        bool key_none_said{};
        int64_t stamp{-1};  ///< ... and what it was then: its time of change (0: no file)
        uint64_t inode{};
        bool ready{}, failed{};
        int scale{};        ///< which size of the letters (hud_font::SCALE)
        Image image;        ///< the picture, as large as the largest step's
        VkImageView view{};
        /// What was drawn for a command set's frame; the last one: for a frame that passes through.
        HostBuffer staging[COMMAND_SETS + 1];
        hud_panel::Canvas canvas, back; ///< the picture, and its empty box
        int back_step{};
        int x{}, y{}, width{}, height{}; ///< where the picture is in the frame now
        // Frames that pass through (no generated frames) get the panel from a command buffer of their own.
        VkCommandBuffer pass_cmd{};
        VkFence pass_fence{};
        VkDescriptorSet pass_set{};
        bool pass_pending{};
        // What it shows.
        float gaps[GAPS]{};         ///< the time between frames handed to the display, ms: a ring
        float frame_gaps[FRAMES]{}; ///< ... between the game's own frames
        uint32_t gap_count{}, frame_count{};
        double last_frame{};
        double rated{};             ///< when the numbers were last renewed
        float shown{}, base{}, middle{}, high{};
        int multiplier{1};
        // Tests: the panel's part of the frames as they go out (control file PANELDUMP).
        bool dump_next{};
        uint32_t dumps{};
        HostBuffer dump;
        int dump_x{}, dump_y{}, dump_width{}, dump_height{};
    } panel;
    /// HUD content that moves over the screen (world markers): where it was in the capture before
    /// (RecordMarkers), so that the generated frames show it on its way.
    struct Markers {
        bool ready{}, failed{};
        uint32_t tiles_w{}, tiles_h{};      ///< tiles of 64x64 pixels
        Image quarter, sixteenth;           ///< hud_moved.comp's maps
        Image blocks, cells;                ///< hud_blocks.comp's sums
        Image which;                        ///< hud_which.comp: per pixel, whether its content moves
        VkImageView quarter_view{}, sixteenth_view{}, blocks_view{}, cells_view{}, which_view{};
        HostBuffer tiles;                   ///< the passes' "Markers": counts, then per tile what the passes found (TILE_BYTES each)
        static constexpr VkDeviceSize TILE_BYTES = 128;
        HostBuffer counts[COMMAND_SETS];    ///< the counts as a command set's frame left them, read back
        bool counted[COMMAND_SETS]{};
        HostBuffer dump;                    ///< the whole of it, for a dump
        uint64_t previous_hud{};            ///< the capture that went with the frame in `previous`; 0: none
        uint32_t frames{}, seen{}, found{}, pieces{}, over{}, pictures{}; ///< since the last statistics line
    } markers;
    VkSemaphore real_ready{};
    VkSemaphore acquired[MAX_GENERATED]{};
    VkSemaphore blitted[MAX_GENERATED]{};
    HostBuffer dump_real, dump_generated, dump_vectors, dump_depth, dump_previous, dump_hud, dump_scene, dump_fixed;

    void* scratch{};
    FfxInterface backend{};
    FfxUInt32 shared_context{};
    FfxOpticalflowContext optical_flow{};
    FfxFrameInterpolationContext interpolation{};
    FfxResourceInternal of_vector{}, of_scd{}, dilated_depth{}, dilated_motion{}, previous_depth{};
    bool ffx_created{};

    uint64_t frame_id{};
    bool have_previous{};
    uint64_t input_serial{}, hud_used{};
    int input_adjust{}, hud_adjust{}; ///< pending correction of the hand-out (see NextInOrder)
    int sync_vote{}, sync_votes{};    ///< CheckSync: which neighbour fitted better, how often in a row
    bool probe_valid[3]{};            ///< the read-back has the scene before / after / two before
    float input_lag{}, hud_lag{};     ///< how many copies behind the newest the hand-out usually is
    uint32_t stale{}, stale_frames{}; ///< presents in a row / in this statistics span without a new copy
    uint32_t ahead_frames{};
    float measure_ms{};
    // The frame chain (see NoteHandover and LinkFrame).
    ChainFrame chain_frame;            ///< the hand-over of the last present that came with a new one
    uint64_t seen_input{}, seen_hud{}; ///< captures submitted by the time of the last present
    uint32_t chain_missed{};           ///< presents in a row with new captures and no hand-over
    int chain_shift{};       ///< found by picture: the presented frame is this many captures off the chain's
    bool measure_chained{};  ///< the probes being read back are of a chained frame
    bool measure_unshifted{}; ///< ... which got its own captures although the chain has a shift (nothing later to shift to)
    double sync_logged{};
    float sync_fit{}, sync_best{}; ///< CheckSync's last result: the scene used, the best other one
    uint32_t chained_frames{}, duplicate_frames{}, unlinked_frames{}, nohud_frames{}, multi_frames{};
    double ahead_sum{};         ///< how many captures the game was ahead at the presents of the span, summed
    std::vector<float> wait_ms; ///< waits for the GPU before the first generated frame
    // The limiter (see Limiter), read once a second; the sums are for the statistics span.
    double free_rate{};  ///< what the game could do unheld, frames a second, by what probes found; 0: not known
    double free_bound{}; ///< ... at the very most: what the thread that hands frames over and the GPU would allow
    // The multiplier's choice (see where it is made, in Generate).
    float interval_ring[128]{}; ///< the game's last frame times (seconds)
    uint32_t interval_count{};
    bool decide{};              ///< a second of the limiter's is over: the multiplier is looked at
    uint32_t votes_up{}, votes_down{}; ///< such seconds in a row that wanted more / fewer generated frames
    struct Probe {
        double time, period;
    } probes[4]{}; ///< what the last probes found (Limiter::probe_result)
    uint32_t probe_count{}, probe_seen{}, probe_stat{};
    double probe_wanted{};      ///< the frame time the last probe asked about
    double probe_wait{1.0}, probe_after{}; ///< seconds from one probe to the next; none before
    double free_last{};         ///< the last probe's answer as a rate, for the statistics
    double down_time{};  ///< when the multiplier last went down on what probes found
    uint32_t down_to{};  ///< ... and to what
    bool down_failed{};  ///< ... and had to come back up within moments
    double down_until{}, down_block{20.0}; ///< no such step before; how long the next block is
    /// From the present call to the generated frames being ready: what three in four of the last
    /// frames stayed under (single slow ones do not move it).
    double ready_seconds{};
    float ready_ring[32]{};
    uint32_t ready_count{};
    /// From the real frame going out to the end of the hook, the same level. Ready time, a
    /// refresh for each generated frame and this: the shortest frame time the hook keeps up with.
    double tail_seconds{};
    float tail_ring[32]{};
    uint32_t tail_count{};
    /// From hand-over to present hook when nothing is queued: the least of the last frames
    /// (it creeps up, so that it follows a GPU that needs longer).
    double handover_least{};
    double paced{};     ///< this hook's waits between frames so far
    bool hook_waited{}; ///< this hook waited for its generated frames
    double hook_last{}; ///< how long the last hook took that generated frames
    double room_until{}, room_block{30.0}; ///< no step up of the multiplier before; how long the next block is
    double auto_off_after{}, off_since{};
    uint32_t limit_frames{}, limit_held{}, limit_gpu_frames{}, limit_gpu_late{}, limit_gated{}, limit_own_frames{};
    double limit_slept{}, limit_gpu_seconds{}, limit_own_seconds{};
    uint32_t guard_waits{}, guard_timeouts{}, first_waits{}, first_ahead{};
    double guard_seconds{}, first_seconds{};
    std::vector<Event> events; ///< a ring of the last presents
    uint64_t event_count{};
    uint32_t event_files{};
    bool events_wanted{};
    double last_present{};
    double last_entry{};
    double period{};  ///< smoothed time between the game's presents, seconds
    bool too_fast{};  ///< the game alone is above SC_FG_MAX_BASE
    // statistics
    double stats_since{};
    uint32_t real_frames{}, generated_frames{}, passed_frames{};
    double hook_seconds{};
    /// When the game's frames reached the display, as the driver tells vkd3d-proton: what comes
    /// through VK_EXT_present_timing and the waits for presents, which this layer only looks at.
    /// Those calls come on vkd3d's waiting thread and the presents on its queue thread: one
    /// writes and the other reads, without a lock.
    struct Shown {
        static constexpr uint32_t SENT = 64, RING = 512;
        struct Sent { ///< a frame given to the display, under its present id
            std::atomic<uint64_t> id{};
            std::atomic<double> time{};
            std::atomic<uint64_t> event{}; ///< its line in the event log, plus one; 0: none
            std::atomic<uint32_t> multiplier{};
        } sent[SENT];
        struct Entry {
            uint64_t event;
            float delay_ms; ///< from the present call to the frame reaching the display
            float gap_ms;   ///< from the frame before it to this one, on the display; 0: not known
            uint32_t multiplier;
        } ring[RING]{};
        float waited_ms[RING]{}; ///< from the present call to the end of a wait for that present
        std::atomic<uint32_t> count{}, waited_count{}, unmatched{};
        // The waiting thread's own.
        uint64_t last_id{};
        double last_shown{};
        double offset{};         ///< added to a time on the display's clock (in seconds): Now()'s
        double calibrated{-1e9}; ///< when that was found
        uint64_t domain_id{};
        int domain{-1};
        uint32_t stage{};
        bool clock_ok{}, said{};
        // The presenting thread's own.
        uint32_t read{}, waited_read{};
        std::vector<float> delays, waits;
        uint32_t planned{}, closer{}, later{}, much_later{}; ///< frames by their distance on the display
        // This layer's own requests (PARAMS: timing), one for every frame it sends out, on the
        // presenting thread. Reports come in the order of the presents, but not all of them come
        // here: a report goes to whoever asks first, and something else in the game's process asks
        // now and then. So the game's frames carry an id after the first few, which says where a
        // report belongs. (Such an id stays below what vkd3d-proton would take for its own, should
        // the game go over to vsync: it counts the game's presents.)
        struct Mine {
            double time;
            uint64_t event, id, serial;
            uint32_t multiplier;
            bool real;
            float call_ms;    ///< how long its present call took
            uint8_t position; ///< its place among the frames sent for one game frame
        };
        static constexpr uint32_t MINE = 128, MINE_MOST = 48, IDS_AFTER = 16;
        Mine mine[MINE]{};
        /// What became of each frame sent while the requests are on (written by "sc-turbo fg-events",
        /// read by tools/display-timing.py: keep the two in step).
        struct Fate {
            double sent, shown; ///< this layer's clock, seconds; shown is 0 without a time
            float call_ms;
            uint8_t flags; ///< 1 the game's own frame, 2 replaced before it reached the display, 4 no report came here, 8 its report did not fit
            uint8_t position, multiplier, unused;
        };
        static constexpr uint32_t FATES = 16384;
        std::vector<Fate> fates;
        uint64_t fate_count{};
        uint64_t mine_sent{}, mine_read{}, own_real{};
        uint64_t own_reports{}, others_counted{}; ///< reports read here; those of others already in a statistics line
        std::atomic<uint64_t> others_read{}, own_last_id{}; ///< (whoever else reads reports looks at these)
        std::atomic<bool> own_active{}, others_said{};
        uint64_t own_domain{}, last_serial{};
        uint32_t own_stage{};
        int own_state{}; ///< 0: not tried yet, 1: ready, -1: not possible
        double last_any{};
        std::vector<float> generated_delays;
        uint32_t lost_real{}, lost_generated{}; ///< frames the display says it never showed: a later one replaced them
        uint32_t silent_real{}, silent_generated{}; ///< frames no report came here for
        uint32_t refreshes[4]{}; ///< frames on the display by the refreshes since the one before: 0, 1, 2, more
    } shown;
    VkSurfaceKHR surface{};
    VkSwapchainCreateFlagsKHR create_flags{};
    VkLatencySleepModeInfoNV sleep_mode{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV}; ///< as last asked for by vkd3d-proton
    bool latency_swapchain{}; ///< created for VK_NV_low_latency2
    bool boost_on{};          ///< the GPU has been asked to keep its speed up
    // What the present hook itself spends, since the last statistics line.
    double acquire_seconds{}, present_seconds{}; ///< in the driver: getting images, presenting
    uint32_t acquires{}, present_calls{};
    std::vector<float> real_out_ms; ///< from the present hook's start until the game's frame goes out
    std::vector<float> sleep_late_ms; ///< how much later than asked for its sleeps between frames ended
    std::vector<float> game_ms, output_ms, age_ms; ///< samples since the last statistics line
    double last_output{};
    uint64_t evaluations{};
    uint32_t game_inputs{}, blank_inputs{};
    double control_checked{-10.0};
    bool off{};
    bool dump_next{};
    uint32_t dump_more{}; ///< dumps still to follow the next one
    uint32_t dumps{};
};

/// What three in four of the last values stayed under (single high ones do not move it).
float Level(const float* ring, uint32_t count) {
    float sorted[32];
    const uint32_t n = std::min(count, 32u);
    if (!n) {
        return 0.0f;
    }
    std::copy(ring, ring + n, sorted);
    std::nth_element(sorted, sorted + n * 3 / 4, sorted + n);
    return sorted[n * 3 / 4];
}

/// Which captures belong to the frame being presented.
struct Link {
    bool alive{};     ///< frames are being handed over (the chain can be used)
    bool chained{};   ///< by the frame chain; else in the order of arrival
    bool duplicate{}; ///< nothing was rendered since the last present
    uint64_t input{}, hud{};
    uint64_t newest_input{}, newest_hud{}, recorded_input{};
    uint64_t submitted_input{}, submitted_hud{};
    ChainFrame frame;
};

Link LinkFrame(Device& d, Fg& fg, double now) {
    Link link;
    {
        std::lock_guard lock{g_chain_mutex};
        link.frame = d.chain;
    }
    std::lock_guard global{g_input_mutex};
    Device* r = g_input_device;
    if (!r) {
        return link;
    }
    Inputs& in = r->inputs;
    std::lock_guard lock{in.mutex};
    link.recorded_input = in.serial.load();
    link.submitted_input = in.submitted.load();
    link.submitted_hud = in.hud_submitted.load();
    link.newest_input = SubmittedCopy(link.recorded_input, link.submitted_input);
    link.newest_hud = SubmittedCopy(in.hud_serial.load(), link.submitted_hud);
    static const bool use_chain = EnvFloat("SC_FG_CHAIN", 1.0f) != 0.0f;
    const bool alive = link.alive = use_chain && g_live_chain.load() != 0.0f && r != &d && link.frame.number &&
                                    now - link.frame.time < 2.0;
    const int shift = fg.chain_shift + static_cast<int>(g_live_shift.load());
    const bool same = link.submitted_input == fg.seen_input && link.submitted_hud == fg.seen_hud;
    if (alive && link.frame.number != fg.chain_frame.number) {
        link.chained = true;
        fg.chain_missed = 0;
        const auto shifted = [&](uint64_t serial, uint64_t newest) -> uint64_t {
            const int64_t moved = static_cast<int64_t>(serial) + shift;
            return serial ? static_cast<uint64_t>(std::clamp<int64_t>(moved, 1, static_cast<int64_t>(std::max<uint64_t>(newest, 1))))
                          : 0;
        };
        link.hud = shifted(link.frame.hud, link.submitted_hud);
        link.input = shifted(link.frame.input, link.submitted_input);
    } else if (alive && (same || fg.chain_missed < 3)) {
        // No hand-over since the last present: the same frame once more. New captures do not make
        // it another one (the game may be a frame ahead); only when presents keep coming with
        // captures and without hand-overs is the chain left, until the next hand-over.
        fg.chain_missed += !same;
        link.chained = link.duplicate = true;
        link.hud = fg.hud_used;
        link.input = fg.input_serial;
    } else {
        link.hud = NextInOrder(fg.hud_used, link.newest_hud, &fg.hud_adjust, &fg.hud_lag);
        link.input = NextInOrder(fg.input_serial, link.newest_input, &fg.input_adjust, &fg.input_lag);
    }
    // The evaluation that was submitted with a HUD capture is the same frame's, whatever was counted.
    if (link.hud && !link.duplicate && (!link.chained || shift)) {
        const uint32_t slot = static_cast<uint32_t>((link.hud - 1) % Inputs::SLOTS);
        if (in.pair_hud[slot] == link.hud && in.pair_input[slot]) {
            link.input = in.pair_input[slot];
        }
    }
    return link;
}

/// The ring of the last presents, as text.
void WriteEvents(Fg& fg) {
    char path[600];
    std::snprintf(path, sizeof path, "%s/events-%d-%u.txt", Dir(), getpid(), fg.event_files++);
    FILE* file = fg.events.empty() ? nullptr : std::fopen(path, "w");
    if (!file) {
        return;
    }
    std::fprintf(file, "# time | chain hud input newest_hud newest_input | since the last linked frame: hand-overs "
                       "hud_captures evaluations hud_passes frame_copies | ms: hand-over_to_present limiter_sleep "
                       "game_gpu fg_gpu_wait hook hook_to_submit hook_to_first_frame_out paced_waits game_own "
                       "submit_kept_back present_to_display | multiplier shift flags (C chained, D duplicate, L layer, R reset, U reused "
                       "inputs, N no HUD capture, H its scene worked out from the frame, P passed untouched, M measured, "
                       "V vectors checked, G let on by the "
                       "gate, O copied out only after the game's next frame was submitted, X ... and finished: the wrong "
                       "picture)\n");
    const uint64_t count = std::min<uint64_t>(fg.event_count, EVENT_COUNT);
    for (uint64_t i = fg.event_count - count; i < fg.event_count; ++i) {
        const Event& e = fg.events[i % EVENT_COUNT];
        char flags[14]{};
        size_t n = 0;
        for (const auto& [bit, letter] : {std::pair{Event::CHAINED, 'C'}, {Event::DUPLICATE, 'D'}, {Event::LAYER, 'L'},
                                          {Event::RESET, 'R'},   {Event::REUSED, 'U'},    {Event::NO_HUD, 'N'},
                                          {Event::PASSED, 'P'},  {Event::MEASURED, 'M'}, {Event::CHECKED, 'V'},
                                          {Event::GATED, 'G'},   {Event::LATE, 'O'},     {Event::LOST, 'X'},
                                          {Event::UNMIXED, 'H'}}) {
            if (e.flags & bit) {
                flags[n++] = letter;
            }
        }
        std::fprintf(file, "%.4f | %llu %llu %llu %llu %llu | %u %u %u %u %u | %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f | x%u %d %s\n", e.time,
                     static_cast<unsigned long long>(e.chain), static_cast<unsigned long long>(e.hud),
                     static_cast<unsigned long long>(e.input), static_cast<unsigned long long>(e.newest_hud),
                     static_cast<unsigned long long>(e.newest_input), e.handovers, e.hud_captures, e.evaluations, e.passes,
                     e.frame_copies, e.handover_ms, e.slept_ms, e.gpu_ms, e.wait_ms, e.hook_ms, e.record_ms, e.ready_ms, e.paced_ms,
                     e.own_ms, e.guard_ms, e.shown_ms, e.multiplier, e.shift,
                     n ? flags : "-");
    }
    std::fclose(file);
    Log("fg: the last %llu presents written to %s", static_cast<unsigned long long>(count), path);
    const Fg::Shown& shown = fg.shown;
    if (!shown.fate_count) {
        return;
    }
    std::snprintf(path, sizeof path, "%s/presents-%d-%u.bin", Dir(), getpid(), fg.event_files - 1);
    if (FILE* out = std::fopen(path, "wb")) {
        const uint64_t fates = std::min<uint64_t>(shown.fate_count, Fg::Shown::FATES);
        const uint32_t size = sizeof(Fg::Shown::Fate);
        const double target = EnvFloat("SC_FG_TARGET", 120.0f);
        std::fwrite("SCFGPRS1", 1, 8, out);
        std::fwrite(&size, sizeof size, 1, out);
        std::fwrite(&size, sizeof size, 1, out); // (room for later use)
        std::fwrite(&fates, sizeof fates, 1, out);
        std::fwrite(&target, sizeof target, 1, out);
        for (uint64_t i = shown.fate_count - fates; i < shown.fate_count; ++i) {
            std::fwrite(&shown.fates[i % Fg::Shown::FATES], size, 1, out);
        }
        std::fclose(out);
        Log("fg: what became of the last %llu frames sent written to %s", static_cast<unsigned long long>(fates), path);
    }
}

bool CheckFfx(FfxErrorCode error, const char* what) {
    if (error != FFX_OK) {
        Log("fg: %s failed: 0x%x", what, error);
        return false;
    }
    return true;
}

FfxResource External(const Image& image, const wchar_t* name) {
    return ffxGetResourceVK(reinterpret_cast<void*>(image.image),
                            ffxGetImageResourceDescriptionVK(image.image, image.info,
                                                             FFX_RESOURCE_USAGE_READ_ONLY),
                            name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool CreateFfx(Device& d, Fg& fg) {
    g_ffx_device = &d;
    VkDeviceContext device_context{d.handle, d.physical, d.gdpa};
    const FfxDevice ffx_device = ffxGetDeviceVK(&device_context);
    constexpr size_t MAX_CONTEXTS = 4;
    const size_t scratch_size = ffxGetScratchMemorySizeVK(d.physical, MAX_CONTEXTS);
    const size_t scratch_alloc = (scratch_size + 63) / 64 * 64;
    fg.scratch = std::aligned_alloc(64, scratch_alloc);
    if (!fg.scratch) {
        return false;
    }
    std::memset(fg.scratch, 0, scratch_alloc);
    if (!CheckFfx(ffxGetInterfaceVK(&fg.backend, ffx_device, fg.scratch, scratch_size, MAX_CONTEXTS),
                  "ffxGetInterfaceVK") ||
        !CheckFfx(fg.backend.fpCreateBackendContext(&fg.backend, FFX_EFFECT_SHAREDRESOURCES, nullptr,
                                                    &fg.shared_context),
                  "shared backend context")) {
        return false;
    }
    FfxOpticalflowContextDescription of_description{};
    of_description.backendInterface = fg.backend;
    of_description.resolution = {fg.extent.width, fg.extent.height};
    if (!CheckFfx(ffxOpticalflowContextCreate(&fg.optical_flow, &of_description), "optical flow context")) {
        return false;
    }
    FfxFrameInterpolationContextDescription fi_description{};
    fi_description.backendInterface = fg.backend;
    fi_description.flags = static_cast<uint32_t>(EnvFloat(
        "SC_FG_FLAGS",
        FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE));
    fi_description.maxRenderSize = {fg.extent.width, fg.extent.height};
    fi_description.displaySize = {fg.extent.width, fg.extent.height};
    fi_description.backBufferFormat = ffxGetSurfaceFormatVK(fg.format);
    fi_description.previousInterpolationSourceFormat = fi_description.backBufferFormat;
    if (!CheckFfx(ffxFrameInterpolationContextCreate(&fg.interpolation, &fi_description),
                  "frame interpolation context")) {
        return false;
    }
    FfxOpticalflowSharedResourceDescriptions of_shared{};
    FfxFrameInterpolationSharedResourceDescriptions fi_shared{};
    if (!CheckFfx(ffxOpticalflowGetSharedResourceDescriptions(&fg.optical_flow, &of_shared), "of shared") ||
        !CheckFfx(ffxFrameInterpolationGetSharedResourceDescriptions(&fg.interpolation, &fi_shared), "fi shared") ||
        !CheckFfx(fg.backend.fpCreateResource(&fg.backend, &of_shared.opticalFlowVector, fg.shared_context, &fg.of_vector), "of vector") ||
        !CheckFfx(fg.backend.fpCreateResource(&fg.backend, &of_shared.opticalFlowSCD, fg.shared_context, &fg.of_scd), "of scd") ||
        !CheckFfx(fg.backend.fpCreateResource(&fg.backend, &fi_shared.dilatedDepth, fg.shared_context, &fg.dilated_depth), "dilated depth") ||
        !CheckFfx(fg.backend.fpCreateResource(&fg.backend, &fi_shared.dilatedMotionVectors, fg.shared_context, &fg.dilated_motion), "dilated vectors") ||
        !CheckFfx(fg.backend.fpCreateResource(&fg.backend, &fi_shared.reconstructedPrevNearestDepth, fg.shared_context, &fg.previous_depth), "previous depth")) {
        return false;
    }
    fg.ffx_created = true;
    return true;
}

VkImageView MakeView(Device& d, VkImage image, VkFormat format) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    return d.CreateImageView(d.handle, &info, nullptr, &view) == VK_SUCCESS ? view : VK_NULL_HANDLE;
}

/// Detect: bias, ratio, least motion, unused. Apply: up, down, level, grow. Read from the
/// environment and, while the game runs, from the control file PARAMS (lines "name=value").
float g_hud_parameters[8] = {EnvFloat("SC_FG_HUD_BIAS", 0.004f),  EnvFloat("SC_FG_HUD_RATIO", 0.6f),
                             EnvFloat("SC_FG_HUD_MOTION", 1.0f), 0.0f,
                             EnvFloat("SC_FG_HUD_UP", 0.34f),    EnvFloat("SC_FG_HUD_DOWN", 0.2f),
                             EnvFloat("SC_FG_HUD_LEVEL", 0.6f),  EnvFloat("SC_FG_HUD_GROW", 6.0f)};

void ReadParameters() {
    // The file is taken under a name of this process's own before it is read: a new one that
    // sc-turbo puts in place meanwhile is left for the next look, instead of being removed unread.
    char path[600], taken[620];
    std::snprintf(path, sizeof path, "%s/PARAMS", Dir());
    std::snprintf(taken, sizeof taken, "%s.layer-%d", path, static_cast<int>(getpid()));
    if (std::rename(path, taken) != 0) {
        return;
    }
    FILE* file = std::fopen(taken, "r");
    if (!file) {
        unlink(taken);
        return;
    }
    static const char* const names[8] = {"bias", "ratio", "motion", "", "up", "down", "level", "grow"};
    static const std::pair<const char*, std::atomic<float>*> live[] = {
        {"chain", &g_live_chain},     {"nohud", &g_live_nohud}, {"wait", &g_live_wait}, {"shift", &g_live_shift},
        {"vectors", &g_live_vectors}, {"gate", &g_live_gate},   {"guard", &g_live_guard}, {"first", &g_live_first},
        {"fill", &g_live_fill},       {"probe", &g_live_probe}, {"gpuwait", &g_live_lag}, {"markers", &g_live_markers},
        {"gapfill", &g_live_gapfill}, {"cross", &g_live_cross}, {"pictures", &g_live_pictures},
        {"boost", &g_live_boost}, {"priority", &g_live_priority}, {"timing", &g_live_timing},
        {"adapt", &g_live_adapt}};
    char name[64];
    float value = 0.0f;
    while (std::fscanf(file, " %63[^= \n]=%f", name, &value) == 2) {
        for (int i = 0; i < 8; ++i) {
            if (!std::strcmp(name, names[i])) {
                g_hud_parameters[i] = value;
            }
        }
        for (const auto& [key, target] : live) {
            if (!std::strcmp(name, key)) {
                *target = value;
            }
        }
    }
    std::fclose(file);
    unlink(taken);
    Log("fg: frame chain %s, frames without a HUD capture %s, wait for generated frames %s, chain shift by hand %d, "
        "vector check %.0f, limiter gate %s, guard %s, generated frames first %s, multiplier %s, probes %s, "
        "frames wait for the GPU %s, moving HUD content %s, pictures in the HUD %s, frames without a capture keep the HUD apart %s, "
        "captures from a frame copy apart from the HUD pass %s",
        g_live_chain.load() != 0.0f ? "on" : "off", g_live_nohud.load() != 0.0f ? "use the last capture" : "restart",
        g_live_wait.load() != 0.0f ? "on" : "off", static_cast<int>(g_live_shift.load()), g_live_vectors.load(),
        g_live_gate.load() != 0.0f ? "on" : "off", g_live_guard.load() != 0.0f ? "on" : "off",
        g_live_first.load() != 0.0f ? "on" : "off",
        g_live_fill.load() != 0.0f ? "fills the display" : "does not hold the game back",
        g_live_probe.load() != 0.0f ? "on" : "off", g_live_lag.load() != 0.0f ? "on" : "off",
        g_live_markers.load() != 0.0f ? "on its way in the generated frames" : "as in the frame that follows",
        g_live_pictures.load() != 0.0f ? "stay at the game's rate" : "are moved like the rest",
        g_live_gapfill.load() != 0.0f ? "on" : "off", g_live_cross.load() != 0.0f ? "on" : "off");
    Log("fg: the multiplier %s", g_live_adapt.load() != 0.0f ? "follows the game's rate" : "is held where it was asked for");
    Log("fg: HUD parameters bias=%g ratio=%g motion=%g up=%g down=%g level=%g grow=%g", g_hud_parameters[0],
        g_hud_parameters[1], g_hud_parameters[2], g_hud_parameters[4], g_hud_parameters[5], g_hud_parameters[6],
        g_hud_parameters[7]);
}

/// Layer composite: mode (0 blend, 1 hard, 2 show), alpha gain, difference floor. Control file LAYER.
float g_layer_parameters[4] = {0.0f, 1.0f, 1.5f / 255.0f, 0.0f};

/// The HUD passes (detect, apply, composite). Without them generation still works.
bool PrepareHud(Device& d, Fg& fg) {
    Fg::Hud& hud = fg.hud;
    if (!d.CreateComputePipelines || !CreateOwnImage(d, fg.previous, fg.format, fg.extent.width, fg.extent.height)) {
        return false;
    }
    // 0-2, 5 and 7-9 sampled, 3-4 written, 6 a buffer (the vector check's lists, the moving HUD content's tiles)
    VkDescriptorSetLayoutBinding bindings[10]{};
    for (uint32_t i = 0; i < 10; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == 6                      ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : i < 3 || i == 5 || i >= 7 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                 : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 10;
    set_layout.pBindings = bindings;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // (the command sets' own, and one for the panel on frames that pass through)
    constexpr uint32_t SETS = COMMAND_SETS * HUD_SET_COUNT + 1;
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7 * SETS},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * SETS},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = SETS;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    if (d.CreateDescriptorSetLayout(d.handle, &set_layout, nullptr, &hud.set_layout) != VK_SUCCESS ||
        d.CreateSampler(d.handle, &sampler, nullptr, &hud.sampler) != VK_SUCCESS ||
        d.CreateDescriptorPool(d.handle, &pool, nullptr, &hud.pool) != VK_SUCCESS) {
        return false;
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &hud.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    if (d.CreatePipelineLayout(d.handle, &layout, nullptr, &hud.layout) != VK_SUCCESS) {
        return false;
    }
    const std::pair<const uint32_t*, size_t> code[HUD_PASSES] = {{HUD_DETECT_SPV, sizeof HUD_DETECT_SPV},
                                                        {HUD_APPLY_SPV, sizeof HUD_APPLY_SPV},
                                                        {HUD_COMPOSITE_SPV, sizeof HUD_COMPOSITE_SPV},
                                                        {HUD_LAYER_SPV, sizeof HUD_LAYER_SPV},
                                                        {MV_CHECK_SPV, sizeof MV_CHECK_SPV},
                                                        {MV_SPREAD_SPV, sizeof MV_SPREAD_SPV},
                                                        {MV_TILES_SPV, sizeof MV_TILES_SPV},
                                                        {MV_REACH_SPV, sizeof MV_REACH_SPV},
                                                        {MV_MARKS_SPV, sizeof MV_MARKS_SPV},
                                                        {HUD_UNMIX_SPV, sizeof HUD_UNMIX_SPV}};
    for (int i = 0; i < HUD_PASSES; ++i) {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = code[i].second;
        shader.pCode = code[i].first;
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.pName = "main";
        pipeline.layout = hud.layout;
        if (d.CreateShaderModule(d.handle, &shader, nullptr, &hud.shaders[i]) != VK_SUCCESS ||
            (pipeline.stage.module = hud.shaders[i],
             d.CreateComputePipelines(d.handle, VK_NULL_HANDLE, 1, &pipeline, nullptr, &hud.pipelines[i])) != VK_SUCCESS) {
            return false;
        }
    }
    for (Fg::Set& set : fg.sets) {
        for (VkDescriptorSet& descriptor_set : set.hud_sets) {
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = hud.pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &hud.set_layout;
            if (d.AllocateDescriptorSets(d.handle, &allocate, &descriptor_set) != VK_SUCCESS) {
                return false;
            }
        }
    }
    hud.current_view = MakeView(d, fg.backbuffer.image, fg.format);
    hud.previous_view = MakeView(d, fg.previous.image, fg.format);
    bool ok = hud.current_view && hud.previous_view;
    for (uint32_t i = 0; i < g_generated_cap; ++i) {
        hud.output_views[i] = MakeView(d, fg.outputs[i].image, fg.format);
        ok = ok && hud.output_views[i];
    }
    return ok;
}

/// Binds `sampled` to bindings 0..2 and `stored` to 3..4 (null entries are left alone), then runs
/// pipeline `which` over width x height. `extra`: sampled, for binding 5. `lists`: a buffer, for
/// binding 6; with `indirect` the pass is started as the arguments at that place in the buffer
/// say, not over width x height. `more`: three more to sample, for bindings 7..9.
void RunHudPass(Device& d, Fg& fg, VkCommandBuffer cmd, VkDescriptorSet set, int which, const VkImageView sampled[3],
                const VkImageView stored[2], const float constants[4], uint32_t width, uint32_t height,
                VkImageView extra = VK_NULL_HANDLE, VkBuffer lists = VK_NULL_HANDLE,
                VkDeviceSize indirect = VK_WHOLE_SIZE, const VkImageView* more = nullptr) {
    Fg::Hud& hud = fg.hud;
    VkDescriptorImageInfo images[10]{};
    VkWriteDescriptorSet writes[10]{};
    uint32_t count = 0;
    const VkDescriptorBufferInfo whole_buffer{lists, 0, VK_WHOLE_SIZE};
    if (lists) {
        writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[count].dstSet = set;
        writes[count].dstBinding = 6;
        writes[count].descriptorCount = 1;
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[count].pBufferInfo = &whole_buffer;
        ++count;
    }
    for (uint32_t i = 0; i < 10; ++i) {
        const VkImageView view = i < 3    ? sampled[i]
                                 : i < 5  ? stored[i - 3]
                                 : i == 5 ? extra
                                 : i >= 7 && more ? more[i - 7] : VK_NULL_HANDLE;
        if (!view) {
            continue;
        }
        const bool is_sampled = i < 3 || i == 5 || i >= 7;
        images[i] = {is_sampled ? hud.sampler : VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[count].dstSet = set;
        writes[count].dstBinding = i;
        writes[count].descriptorCount = 1;
        writes[count].descriptorType =
            is_sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[count].pImageInfo = &images[i];
        ++count;
    }
    d.UpdateDescriptorSets(d.handle, count, writes, 0, nullptr);
    d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hud.pipelines[which]);
    d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hud.layout, 0, 1, &set, 0, nullptr);
    d.CmdPushConstants(cmd, hud.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, constants);
    if (indirect != VK_WHOLE_SIZE) {
        d.TraceCmdDispatchIndirect(cmd, lists, indirect);
    } else {
        d.CmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                            VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
}

/// Finds the HUD and takes its vectors out; `vectors` then points at the masked copy. False: the
/// passes could not run this frame.
bool RecordHud(Device& d, Fg& fg, Fg::Set& set, VkCommandBuffer cmd, Image*& vectors, uint32_t width, uint32_t height) {
    Fg::Hud& hud = fg.hud;
    if (fg.masked_vectors.info.extent.width != width || fg.masked_vectors.info.extent.height != height) {
        for (Image* image : {&fg.masked_vectors, &fg.mask, &fg.grown[0], &fg.grown[1]}) {
            if (image->image) {
                fg.retired.push_back(*image);
            }
            *image = {};
        }
        for (VkImageView* view : {&hud.masked_view, &hud.mask_view, &hud.grown_view[0], &hud.grown_view[1]}) {
            hud.retired_views.push_back(*view);
            *view = VK_NULL_HANDLE;
        }
        if (!CreateOwnImage(d, fg.masked_vectors, VK_FORMAT_R16G16_SFLOAT, width, height) ||
            !CreateOwnImage(d, fg.mask, VK_FORMAT_R8_UNORM, width, height) ||
            !CreateOwnImage(d, fg.grown[0], VK_FORMAT_R8G8_UNORM, width, height) ||
            !CreateOwnImage(d, fg.grown[1], VK_FORMAT_R8G8_UNORM, width, height) ||
            !(hud.masked_view = MakeView(d, fg.masked_vectors.image, VK_FORMAT_R16G16_SFLOAT)) ||
            !(hud.mask_view = MakeView(d, fg.mask.image, VK_FORMAT_R8_UNORM)) ||
            !(hud.grown_view[0] = MakeView(d, fg.grown[0].image, VK_FORMAT_R8G8_UNORM)) ||
            !(hud.grown_view[1] = MakeView(d, fg.grown[1].image, VK_FORMAT_R8G8_UNORM))) {
            hud.ok = false;
            return false;
        }
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image& image : fg.grown) {
            ToGeneral(d, cmd, image);
            d.CmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
    }
    VkImageView& vector_view = hud.vector_views[U64(vectors->image)];
    if (!vector_view && !(vector_view = MakeView(d, vectors->image, VK_FORMAT_R16G16_SFLOAT))) {
        hud.ok = false;
        return false;
    }
    ToGeneral(d, cmd, fg.masked_vectors);
    ToGeneral(d, cmd, fg.mask);
    fg.grown_index ^= 1;
    const VkImageView grown_now = hud.grown_view[fg.grown_index], grown_before = hud.grown_view[fg.grown_index ^ 1];
    const float* detect = g_hud_parameters;
    const float* apply = g_hud_parameters + 4;
    const VkImageView detect_in[3] = {hud.current_view, hud.previous_view, vector_view};
    const VkImageView detect_out[2] = {hud.mask_view, VK_NULL_HANDLE};
    RunHudPass(d, fg, cmd, set.hud_sets[0], 0, detect_in, detect_out, detect, width, height);
    const VkImageView apply_in[3] = {hud.mask_view, grown_before, vector_view};
    const VkImageView apply_out[2] = {hud.masked_view, grown_now};
    RunHudPass(d, fg, cmd, set.hud_sets[1], 1, apply_in, apply_out, apply, width, height);
    vectors = &fg.masked_vectors;
    ++fg.hud_frames;
    return true;
}

/// The game's vectors held against the picture (mv_check.comp, mv_spread.comp): where the previous
/// frame moved by a vector does not give this frame and moved by a neighbour's it does, the
/// neighbour's is used. Only near a break in the vector field can there be such a vector: two
/// small passes (mv_tiles.comp, mv_reach.comp) list the 8x8 blocks that are, a third (mv_marks.comp)
/// those where the first pass changed something, and the two passes are started for the listed
/// blocks only, which leaves most of every frame alone. `current` and `previous`: the same kind of
/// picture of this frame and the one before (the scene without the HUD, or the presented frame).
/// `vectors` then points at the checked copy. False: the passes could not be set up.
bool RecordVectorCheck(Device& d, Fg& fg, Fg::Set& set, VkCommandBuffer cmd, Image*& vectors, const Image& depth,
                       const Image& current, const Image& previous, uint32_t width, uint32_t height) {
    Fg::Hud& hud = fg.hud;
    const uint32_t tiles_w = (width + 7) / 8, tiles_h = (height + 7) / 8, tiles = tiles_w * tiles_h;
    if (tiles > 65535 || !d.CmdFillBuffer || !d.TraceCmdDispatchIndirect) {
        return false; // a pass is started with one work group per block, in one row: 65535 are certain to be allowed
    }
    if (fg.fixed_vectors.info.extent.width != width || fg.fixed_vectors.info.extent.height != height) {
        for (Image* image : {&fg.checked_vectors, &fg.fixed_vectors, &fg.vector_tiles, &fg.vector_reach}) {
            if (image->image) {
                fg.retired.push_back(*image);
            }
            *image = {};
        }
        for (VkImageView* view : {&hud.checked_view, &hud.fixed_view, &hud.tiles_view, &hud.reach_view}) {
            hud.retired_views.push_back(*view);
            *view = VK_NULL_HANDLE;
        }
        if (fg.tile_lists.buffer) {
            fg.retired_buffers.push_back(fg.tile_lists);
        }
        fg.tile_lists = {};
        // two dispatches' arguments (4 words each: the count, 1, 1, unused), then the two lists
        fg.tile_lists.size = (8 + 2 * static_cast<VkDeviceSize>(tiles)) * 4;
        if (!CreateOwnImage(d, fg.checked_vectors, VK_FORMAT_R16G16B16A16_SFLOAT, width, height) ||
            !CreateOwnImage(d, fg.fixed_vectors, VK_FORMAT_R16G16_SFLOAT, width, height) ||
            !CreateOwnImage(d, fg.vector_tiles, VK_FORMAT_R16G16B16A16_SFLOAT, tiles_w, tiles_h) ||
            !CreateOwnImage(d, fg.vector_reach, VK_FORMAT_R16G16B16A16_SFLOAT, tiles_w, tiles_h) ||
            !(hud.checked_view = MakeView(d, fg.checked_vectors.image, VK_FORMAT_R16G16B16A16_SFLOAT)) ||
            !(hud.fixed_view = MakeView(d, fg.fixed_vectors.image, VK_FORMAT_R16G16_SFLOAT)) ||
            !(hud.tiles_view = MakeView(d, fg.vector_tiles.image, VK_FORMAT_R16G16B16A16_SFLOAT)) ||
            !(hud.reach_view = MakeView(d, fg.vector_reach.image, VK_FORMAT_R16G16B16A16_SFLOAT)) ||
            !CreateBufferWithMemory(d, fg.tile_lists.size,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    false, fg.tile_lists.buffer, fg.tile_lists.memory, nullptr)) {
            return false;
        }
    }
    const auto view_of = [&](std::unordered_map<uint64_t, VkImageView>& views, const Image& image, VkFormat format) {
        VkImageView& view = views[U64(image.image)];
        if (!view) {
            view = MakeView(d, image.image, format);
        }
        return view;
    };
    const VkImageView vector_view = view_of(hud.vector_views, *vectors, VK_FORMAT_R16G16_SFLOAT);
    const VkImageView depth_view = view_of(hud.layer_views, depth, VK_FORMAT_R32_SFLOAT);
    const VkImageView current_view =
        &current == &fg.backbuffer ? hud.current_view : view_of(hud.layer_views, current, SCENE_FORMAT);
    const VkImageView previous_view =
        &previous == &fg.previous ? hud.previous_view : view_of(hud.layer_views, previous, SCENE_FORMAT);
    if (!vector_view || !depth_view || !current_view || !previous_view) {
        return false;
    }
    for (Image* image : {&fg.checked_vectors, &fg.fixed_vectors, &fg.vector_tiles, &fg.vector_reach}) {
        ToGeneral(d, cmd, *image);
    }
    // First pass: a neighbour's vector must explain the picture `factor` times better and by
    // `margin`, and at the neighbour itself to within 0.02; a vector that points off the screen
    // counts as a mismatch of 0.1. Second pass: the replacement may be 0.01 worse than the
    // pixel's own vector; vectors within 4 pixels or 15 % are the same object's.
    static const float check[4] = {EnvFloat("SC_FG_VECTOR_FACTOR", 2.0f), EnvFloat("SC_FG_VECTOR_MARGIN", 0.012f), 0.02f,
                                   0.10f};
    static const float spread[4] = {0.01f, 4.0f, 0.15f, 0.10f};
    static const float none[4]{};
    const VkImageView tiles_in[3] = {vector_view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkImageView tiles_out[2] = {hud.tiles_view, VK_NULL_HANDLE};
    const VkImageView reach_in[3] = {hud.tiles_view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkImageView reach_out[2] = {hud.reach_view, VK_NULL_HANDLE};
    const VkImageView check_in[3] = {vector_view, depth_view, current_view};
    const VkImageView check_out[2] = {hud.checked_view, VK_NULL_HANDLE};
    const VkImageView marks_in[3] = {hud.reach_view, hud.checked_view, VK_NULL_HANDLE};
    const VkImageView marks_out[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkImageView spread_in[3] = {vector_view, hud.checked_view, current_view};
    const VkImageView spread_out[2] = {hud.fixed_view, hud.reach_view};
    const VkImageCopy all = WholeCopy(width, height);
    VkMemoryBarrier filled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    filled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    const VkBuffer lists = fg.tile_lists.buffer;
    // tests only: everything several times over, to measure what it costs
    static const int times = std::max(1, static_cast<int>(EnvFloat("SC_FG_TEST_CHECKS", 1.0f)));
    for (int i = 0; i < times; ++i) {
        // Both lists start empty. The second pass writes only what it changes and what the first
        // one changed: the rest is the game's.
        for (const VkDeviceSize arguments : {0, 16}) {
            d.CmdFillBuffer(cmd, lists, arguments, 4, 0);
            d.CmdFillBuffer(cmd, lists, arguments + 4, 12, 1);
        }
        d.CmdCopyImage(cmd, vectors->image, VK_IMAGE_LAYOUT_GENERAL, fg.fixed_vectors.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &all);
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0,
                             nullptr, 0, nullptr);
        RunHudPass(d, fg, cmd, set.hud_sets[4 + MAX_GENERATED], 6, tiles_in, tiles_out, none, tiles_w, tiles_h);
        RunHudPass(d, fg, cmd, set.hud_sets[5 + MAX_GENERATED], 7, reach_in, reach_out, none, tiles_w, tiles_h,
                   VK_NULL_HANDLE, lists);
        RunHudPass(d, fg, cmd, set.hud_sets[2 + MAX_GENERATED], 4, check_in, check_out, check, 0, 0, previous_view, lists,
                   0);
        RunHudPass(d, fg, cmd, set.hud_sets[6 + MAX_GENERATED], 8, marks_in, marks_out, none, tiles_w, tiles_h,
                   VK_NULL_HANDLE, lists);
        RunHudPass(d, fg, cmd, set.hud_sets[3 + MAX_GENERATED], 5, spread_in, spread_out, spread, 0, 0, previous_view,
                   lists, 16);
    }
    vectors = &fg.fixed_vectors;
    ++fg.checked_frames;
    return true;
}

/// The timestamps of GpuClock, made when they are first asked for. False: none can be had.
bool PrepareGpuClock(Device& d, Fg& fg) {
    Fg::GpuClock& clock = fg.clock;
    if (clock.pool || clock.failed) {
        return clock.pool != VK_NULL_HANDLE;
    }
    clock.failed = true;
    VkPhysicalDeviceProperties properties{};
    if (!d.instance || !d.instance->GetPhysicalDeviceProperties || !d.CreateQueryPool || !d.CmdWriteTimestamp) {
        return false;
    }
    d.instance->GetPhysicalDeviceProperties(d.physical, &properties);
    if (!properties.limits.timestampComputeAndGraphics || properties.limits.timestampPeriod <= 0.0f) {
        return false;
    }
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = COMMAND_SETS * Fg::GpuClock::MARKS;
    if (d.CreateQueryPool(d.handle, &info, nullptr, &clock.pool) != VK_SUCCESS) {
        clock.pool = VK_NULL_HANDLE;
        return false;
    }
    clock.period_ms = properties.limits.timestampPeriod * 1e-6;
    clock.failed = false;
    return true;
}

/// What a set's timestamps say, its work being done.
void TakeGpuClock(Device& d, Fg& fg, int set_number) {
    Fg::GpuClock& clock = fg.clock;
    clock.written[set_number] = false;
    uint64_t ticks[Fg::GpuClock::MARKS]{};
    if (d.GetQueryPoolResults(d.handle, clock.pool, set_number * Fg::GpuClock::MARKS, Fg::GpuClock::MARKS, sizeof ticks,
                              ticks, sizeof ticks[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    for (uint32_t i = 0; i + 1 < Fg::GpuClock::MARKS; ++i) {
        clock.parts[i].push_back(ticks[i + 1] >= ticks[i] ? static_cast<float>((ticks[i + 1] - ticks[i]) * clock.period_ms)
                                                           : 0.0f);
    }
}

/// After interpolation: the real frame's HUD over every generated frame.
void RecordHudComposite(Device& d, Fg& fg, Fg::Set& set, VkCommandBuffer cmd) {
    static const float none[4]{};
    for (uint32_t i = 0; i < fg.generated; ++i) {
        const VkImageView in[3] = {fg.hud.current_view, fg.hud.grown_view[fg.grown_index], VK_NULL_HANDLE};
        const VkImageView out[2] = {fg.hud.output_views[i], VK_NULL_HANDLE};
        RunHudPass(d, fg, cmd, set.hud_sets[2 + i], 2, in, out, none, fg.extent.width, fg.extent.height);
    }
}

bool CreateProbes(Device& d, Fg& fg, uint32_t w, uint32_t h) {
    fg.probe_width = w / 8;
    fg.probe_height = h / 8;
    for (int i = 0; i < 6; ++i) {
        const VkDeviceSize size = static_cast<VkDeviceSize>(fg.probe_width) * fg.probe_height * (i == 2 ? 8 : 4);
        if (!CreateOwnImage(d, fg.probe[i], i == 2 ? HUD_FORMAT : SCENE_FORMAT, fg.probe_width, fg.probe_height) ||
            !CreateBufferWithMemory(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fg.probe_buffer[i].buffer,
                                    fg.probe_buffer[i].memory, &fg.probe_buffer[i].data)) {
            return false;
        }
    }
    return true;
}

/// How well a scene copy fits the presented frame: the spread of the presented values per scene
/// value, where the HUD layer is empty (small: one is a function of the other, the same picture).
double Agreement(const uint8_t* scene, const uint8_t* real, const uint16_t* layer, size_t pixels) {
    double sum[256]{}, squares[256]{}, count[256]{};
    for (size_t i = 0; i < pixels; ++i) {
        if (layer[i * 4 + 3] & 0x7fff) {
            continue;
        }
        const uint8_t value = scene[i * 4 + 1];
        const double shown = real[i * 4 + 1];
        sum[value] += shown;
        squares[value] += shown * shown;
        count[value] += 1.0;
    }
    double spread = 0.0, samples = 0.0;
    for (int v = 0; v < 256; ++v) {
        if (count[v] >= 8.0) {
            spread += squares[v] - sum[v] * sum[v] / count[v];
            samples += count[v];
        }
    }
    return samples > pixels / 4.0 ? std::sqrt(std::max(spread / samples, 0.0)) : 99.0;
}

/// Is the scene copy handed out at a present the one of the presented frame? The pictures say:
/// when the copy of the frame before (or after) fits the presented frame clearly better, twice
/// in a row, the hand-out moves by one. With the frame chain this is a check that should never
/// find anything; what it finds becomes a standing shift of the chain and is logged loudly.
/// Without the chain (captures handed out in the order of arrival) it is the only link there is.
void CheckSync(Fg& fg) {
    const size_t pixels = static_cast<size_t>(fg.probe_width) * fg.probe_height;
    const auto* real = static_cast<const uint8_t*>(fg.probe_buffer[1].data);
    const auto* layer = static_cast<const uint16_t*>(fg.probe_buffer[2].data);
    const auto fit = [&](int probe) { return Agreement(static_cast<const uint8_t*>(fg.probe_buffer[probe].data), real, layer, pixels); };
    const double used = fit(0), before = fg.probe_valid[0] ? fit(3) : 99.0, after = fg.probe_valid[1] ? fit(4) : 99.0;
    const double before2 = fg.probe_valid[2] ? fit(5) : 99.0;
    const double best = std::min({before, after, before2});
    fg.sync_fit = static_cast<float>(used);
    fg.sync_best = static_cast<float>(best);
    if (fg.measure_chained && fg.measure_unshifted && fg.chain_shift && used <= 3.0) {
        // The frame's own captures fit it: what the shift was found for is over.
        Log("fg: the frame fits its own captures again (fit %.1f): chain shift %d -> 0", used, fg.chain_shift);
        fg.chain_shift = 0;
    }
    const int vote = !(used > 3.0 && best < used * 0.5) ? 0 : best == before ? -1 : best == before2 ? -2 : 1;
    fg.sync_votes = vote && vote == fg.sync_vote ? fg.sync_votes + 1 : (vote ? 1 : 0);
    fg.sync_vote = vote;
    // Away from the chain's own link it takes two findings in a row; back towards it one is enough.
    const bool back = fg.measure_chained && vote && fg.chain_shift && (vote < 0) != (fg.chain_shift < 0);
    if (fg.sync_votes >= 2 || back) {
        fg.sync_votes = 0;
        if (fg.measure_chained) {
            const int shift = std::clamp(fg.chain_shift + vote, -3, 1);
            Log("fg: FRAME CHAIN OFF by %d: the presented frame fits the capture %s the linked one (fit %.1f; one "
                "before %.1f, two before %.1f, after %.1f): chain shift %d -> %d",
                vote, vote < 0 ? "before" : "after", used, before, before2, after, fg.chain_shift, shift);
            fg.chain_shift = shift;
            fg.events_wanted = true;
        } else {
            Log("fg: the copies were %d frame(s) %s the presented frame (fit %.1f; one before %.1f, two before "
                "%.1f, after %.1f): corrected",
                std::abs(vote), vote < 0 ? "ahead of" : "behind", used, before, before2, after);
            fg.input_adjust = fg.hud_adjust = vote;
        }
    } else if (used > 3.0 && Now() - fg.sync_logged > 5.0) {
        fg.sync_logged = Now();
        Log("fg: scene copy and presented frame differ (fit %.1f; one before %.1f, two before %.1f, after %.1f; %s)", used,
            before, before2, after, fg.measure_chained ? "chained" : "in order");
    }
}

/// Every 8th pixel of the scene without the HUD, of the presented frame and of the HUD layer has
/// been read back. Where the layer is empty the presented value is a function of the scene value,
/// the game's final tone curve; this measures it per channel. A read-back in which the two do not
/// agree that way (menus, loading screens) is not used.
void MeasureCurve(Fg& fg) {
    const uint32_t w = fg.probe_width, h = fg.probe_height;
    const auto* scene = static_cast<const uint8_t*>(fg.probe_buffer[0].data);
    const auto* real = static_cast<const uint8_t*>(fg.probe_buffer[1].data);
    const auto* layer = static_cast<const uint16_t*>(fg.probe_buffer[2].data);
    double sum[3][256]{}, squares[3][256]{}, count[3][256]{};
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        if (layer[i * 4 + 3] & 0x7fff) {
            continue; // the HUD is here
        }
        for (int c = 0; c < 3; ++c) {
            const uint8_t value = scene[i * 4 + c];
            const double shown = real[i * 4 + c];
            sum[c][value] += shown;
            squares[c][value] += shown * shown;
            count[c][value] += 1.0;
        }
    }
    float curve[3][256];
    bool known[3][256]{};
    uint32_t measured = 0;
    double spread = 0.0, samples = 0.0;
    for (int c = 0; c < 3; ++c) {
        for (int v = 0; v < 256; ++v) {
            if (count[c][v] >= 8.0) {
                const double mean = sum[c][v] / count[c][v];
                curve[c][v] = static_cast<float>(mean);
                known[c][v] = true;
                ++measured;
                spread += squares[c][v] - mean * mean * count[c][v];
                samples += count[c][v];
            }
        }
    }
    const double deviation = samples > 0.0 ? std::sqrt(std::max(spread / samples, 0.0)) : 99.0;
    static const bool test = EnvFloat("SC_FG_TEST_LAYER", 0.0f) != 0.0f;
    // A fade from or to black also agrees with itself: the bright end must come out bright.
    bool bright = false;
    for (int c = 0; c < 3; ++c) {
        for (int v = 200; v < 256; ++v) {
            bright = bright || (known[c][v] && curve[c][v] > 150.0f);
        }
    }
    const bool good = deviation < 2.0 && ((measured >= 3 * 50 && bright) || (test && measured));
    if (!good) {
        if (++fg.curve_rejected == 3) {
            Log("fg: scene and presented frame do not agree (deviation %.1f, %u values): tone curve kept as it is",
                deviation, measured);
        }
        return;
    }
    fg.curve_rejected = 0;
    uint8_t bytes[256 * 4];
    for (int c = 0; c < 3; ++c) {
        // the values not seen: between their measured neighbours
        float filled[256];
        int before = -1;
        for (int v = 0; v < 256; ++v) {
            if (!known[c][v]) {
                continue;
            }
            for (int u = before + 1; u < v; ++u) {
                filled[u] = before < 0 ? curve[c][v] * static_cast<float>(u) / static_cast<float>(v)
                                       : curve[c][before] + (curve[c][v] - curve[c][before]) * static_cast<float>(u - before) /
                                                                static_cast<float>(v - before);
            }
            filled[v] = curve[c][v];
            before = v;
        }
        for (int u = before + 1; u < 256; ++u) {
            filled[u] = before < 0 ? static_cast<float>(u)
                                   : curve[c][before] + (255.0f - curve[c][before]) * static_cast<float>(u - before) /
                                                            static_cast<float>(255 - before);
        }
        float highest = 0.0f;
        for (int v = 0; v < 256; ++v) {
            highest = std::max(highest, filled[v]); // never falling
            bytes[v * 4 + c] = static_cast<uint8_t>(std::clamp(highest + 0.5f, 0.0f, 255.0f));
        }
    }
    for (int v = 0; v < 256; ++v) {
        bytes[v * 4 + 3] = 255;
    }
    auto* out = static_cast<uint8_t*>(fg.curve_staging.data);
    int change = fg.curve_ok ? 0 : 99;
    for (int i = 0; i < 256 * 4; ++i) {
        change = std::max(change, std::abs(static_cast<int>(bytes[i]) - static_cast<int>(out[i])));
    }
    // (Its bright end is measured from a few pixels and moves a little with every measurement:
    // not a line for each of those.)
    if (const double now = Now(); change >= 16 || (change >= 3 && now - fg.curve_logged > 60.0)) {
        fg.curve_logged = now;
        Log("fg: tone curve (deviation %.2f, %u values measured): green 16>%u 64>%u 128>%u 192>%u 240>%u", deviation,
            measured, bytes[16 * 4 + 1], bytes[64 * 4 + 1], bytes[128 * 4 + 1], bytes[192 * 4 + 1], bytes[240 * 4 + 1]);
    }
    if (change >= 1) {
        std::memcpy(out, bytes, sizeof bytes);
        fg.curve_dirty = true;
    }
    fg.curve_ok = true;
}

/// More tiles than this with HUD content that arrived: the HUD as a whole is changing (a menu
/// opens), and nothing is said to move. Four tenths of the tiles (a ship's flight display is a
/// seventh of them); SC_FG_MARKER_CAP: a number of tiles instead.
float MarkerCap(const Fg& fg) {
    static const float cap = EnvFloat("SC_FG_MARKER_CAP", 0.0f);
    return cap > 0.0f ? cap : 0.4f * static_cast<float>(fg.markers.tiles_w * fg.markers.tiles_h);
}

/// What the passes for HUD content that moves over the screen need, made when they first run.
bool PrepareMarkers(Device& d, Fg& fg) {
    Fg::Markers& markers = fg.markers;
    if (markers.ready || markers.failed) {
        return markers.ready;
    }
    markers.failed = true; // until all of it is there
    Fg::Hud& hud = fg.hud;
    markers.tiles_w = (fg.extent.width + 63) / 64;
    markers.tiles_h = (fg.extent.height + 63) / 64;
    // The maps: cells of 16 pixels and of 4, with a border of 16 cells of 16 around the tiles.
    const uint32_t cells_w = markers.tiles_w * 4 + 32, cells_h = markers.tiles_h * 4 + 32;
    static const std::pair<const uint32_t*, size_t> code[MARKER_PASSES] = {
        {HUD_MOVED_SPV, sizeof HUD_MOVED_SPV},   {HUD_BLOCKS_SPV, sizeof HUD_BLOCKS_SPV},
        {HUD_MOTION_SPV, sizeof HUD_MOTION_SPV}, {HUD_AROUND_SPV, sizeof HUD_AROUND_SPV},
        {HUD_PIECES_SPV, sizeof HUD_PIECES_SPV}, {HUD_FIELD_SPV, sizeof HUD_FIELD_SPV},
        {HUD_REFINE_SPV, sizeof HUD_REFINE_SPV}, {HUD_REACH_SPV, sizeof HUD_REACH_SPV},
        {HUD_WHICH_SPV, sizeof HUD_WHICH_SPV},   {HUD_LAYER_MOVING_SPV, sizeof HUD_LAYER_MOVING_SPV}};
    bool ok = hud.ok && d.CmdFillBuffer && d.CmdCopyBuffer;
    for (int i = 0; ok && i < MARKER_PASSES; ++i) {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = code[i].second;
        shader.pCode = code[i].first;
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.pName = "main";
        pipeline.layout = hud.layout;
        ok = d.CreateShaderModule(d.handle, &shader, nullptr, &hud.shaders[MARKER_PASS + i]) == VK_SUCCESS &&
             (pipeline.stage.module = hud.shaders[MARKER_PASS + i],
              d.CreateComputePipelines(d.handle, VK_NULL_HANDLE, 1, &pipeline, nullptr, &hud.pipelines[MARKER_PASS + i])) ==
                 VK_SUCCESS;
    }
    // (the buffer: four counts, then per tile eight vectors of four)
    markers.tiles.size = 16 + static_cast<VkDeviceSize>(markers.tiles_w) * markers.tiles_h * Fg::Markers::TILE_BYTES;
    // The block sums: of the capture before at every pixel from three before the picture on; of this
    // capture per cell of 4 of the tiles. Whether a pixel's content moves: one byte a pixel.
    ok = ok && CreateOwnImage(d, markers.sixteenth, VK_FORMAT_R16G16B16A16_SFLOAT, cells_w, cells_h) &&
         CreateOwnImage(d, markers.quarter, VK_FORMAT_R16G16B16A16_SFLOAT, cells_w * 4, cells_h * 4) &&
         CreateOwnImage(d, markers.blocks, VK_FORMAT_R16_UINT, fg.extent.width + 3, fg.extent.height + 3) &&
         CreateOwnImage(d, markers.cells, VK_FORMAT_R16G16B16A16_UINT, markers.tiles_w * 16, markers.tiles_h * 16) &&
         CreateOwnImage(d, markers.which, VK_FORMAT_R8_UINT, fg.extent.width, fg.extent.height) &&
         (markers.sixteenth_view = MakeView(d, markers.sixteenth.image, VK_FORMAT_R16G16B16A16_SFLOAT)) &&
         (markers.quarter_view = MakeView(d, markers.quarter.image, VK_FORMAT_R16G16B16A16_SFLOAT)) &&
         (markers.blocks_view = MakeView(d, markers.blocks.image, VK_FORMAT_R16_UINT)) &&
         (markers.cells_view = MakeView(d, markers.cells.image, VK_FORMAT_R16G16B16A16_UINT)) &&
         (markers.which_view = MakeView(d, markers.which.image, VK_FORMAT_R8_UINT)) &&
         CreateBufferWithMemory(d, markers.tiles.size,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, markers.tiles.buffer, markers.tiles.memory, nullptr) &&
         CreateBufferWithMemory(d, markers.tiles.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, markers.dump.buffer,
                                markers.dump.memory, &markers.dump.data);
    for (HostBuffer& counts : markers.counts) {
        ok = ok && CreateBufferWithMemory(d, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, counts.buffer, counts.memory,
                                          &counts.data);
    }
    if (!ok) {
        Log("fg: the passes for moving HUD content could not be set up; it stays as in the frame that follows");
        return false; // (what was made goes with the swapchain)
    }
    Log("fg: moving HUD content: %ux%u tiles; nothing is said to move when more than %.0f of them changed",
        markers.tiles_w, markers.tiles_h, MarkerCap(fg));
    markers.failed = false;
    markers.ready = true;
    return true;
}

/// HUD content that moves over the screen (world markers, a ship's flight display): where the
/// content of this capture's HUD layer was in the capture before, as a smooth field per piece of
/// tiles that touch, and which pixels are content that moves; for the composite
/// (hud_layer_moving.comp) to put it where it is on its way in each generated frame. The passes
/// are those of proto/hud-motion/hudmotion.py, in its order. False: not this frame.
bool RecordMarkers(Device& d, Fg& fg, Fg::Set& set, int set_number, VkCommandBuffer cmd, const Image& layer,
                   const Image& layer_before) {
    if (!PrepareMarkers(d, fg)) {
        return false;
    }
    Fg::Markers& markers = fg.markers;
    VkImageView& layer_view = fg.hud.layer_views[U64(layer.image)];
    VkImageView& before_view = fg.hud.layer_views[U64(layer_before.image)];
    if ((!layer_view && !(layer_view = MakeView(d, layer.image, HUD_FORMAT))) ||
        (!before_view && !(before_view = MakeView(d, layer_before.image, HUD_FORMAT)))) {
        return false;
    }
    for (Image* image : {&markers.quarter, &markers.sixteenth, &markers.blocks, &markers.cells, &markers.which}) {
        ToGeneral(d, cmd, *image);
    }
    const VkBuffer tiles = markers.tiles.buffer;
    d.CmdFillBuffer(cmd, tiles, 0, 16, 0); // the counts; every tile's entry is written by the passes
    VkMemoryBarrier filled{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    filled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &filled, 0,
                         nullptr, 0, nullptr);
    const uint32_t w = markers.tiles_w, h = markers.tiles_h;
    const float first[4] = {static_cast<float>(w), static_cast<float>(h), MarkerCap(fg), 0.0f};
    const float second[4] = {first[0], first[1], first[2], g_live_pictures.load() != 0.0f ? 1.0f : 2.0f};
    const VkImageView layers[3] = {layer_view, before_view, VK_NULL_HANDLE};
    const VkImageView sums[3] = {markers.blocks_view, markers.cells_view, VK_NULL_HANDLE};
    const VkImageView sums_and_map[3] = {markers.blocks_view, markers.cells_view, markers.quarter_view};
    const VkImageView nothing[3]{};
    const VkImageView maps[2] = {markers.quarter_view, markers.sixteenth_view};
    const VkImageView block_sums[2] = {markers.blocks_view, markers.cells_view};
    const VkImageView moving[2] = {markers.which_view, VK_NULL_HANDLE};
    VkDescriptorSet* sets = &set.hud_sets[MARKER_SETS];
    // (a pass with one work group to a tile is given eight times the tiles: RunHudPass divides by eight)
    const auto run = [&](uint32_t which_set, MarkerPass pass, const VkImageView* in, const VkImageView* out,
                         const float* constants, uint32_t width, uint32_t height, VkImageView extra = VK_NULL_HANDLE) {
        RunHudPass(d, fg, cmd, sets[which_set], MARKER_PASS + pass, in, out, constants, width, height, extra, tiles);
    };
    // tests only: everything several times over, to measure what it costs
    static const int times = std::max(1, static_cast<int>(EnvFloat("SC_FG_TEST_MARKERS", 1.0f)));
    for (int i = 0; i < times; ++i) {
        run(0, MARKER_MAPS, layers, maps, first, w * 4 + 32, h * 4 + 32);
        run(1, MARKER_BLOCKS, layers, block_sums, first, w * 64 + 3, h * 64 + 3);
        run(2, MARKER_OWN, sums_and_map, nothing, first, w * 8, h * 8, markers.sixteenth_view);
        run(3, MARKER_AROUND, sums, nothing, first, w * 8, h * 8);
        run(4, MARKER_PIECES, nothing, nothing, first, 1, 1);
        run(5, MARKER_FIELD, nothing, nothing, first, w, h);
        run(6, MARKER_REFINE, sums, nothing, first, w * 8, h * 8);
        run(7, MARKER_PIECES, nothing, nothing, second, 1, 1);
        run(8, MARKER_FIELD, nothing, nothing, second, w, h);
        run(9, MARKER_REACH, nothing, nothing, first, w, h);
        run(10, MARKER_WHICH, layers, moving, first, fg.extent.width, fg.extent.height);
    }
    const VkBufferCopy counts{0, 0, 16};
    d.CmdCopyBuffer(cmd, tiles, markers.counts[set_number].buffer, 1, &counts);
    markers.counted[set_number] = true;
    return true;
}

/// Layer mode, after interpolation of the HUD-less scenes: the real frame's HUD contribution over
/// every generated frame. `layer_before`, `scene_before`: of the capture before, when
/// RecordMarkers ran for this frame: what moved is then put where it is on its way. False: a view
/// could not be made.
bool RecordLayerComposite(Device& d, Fg& fg, Fg::Set& set, VkCommandBuffer cmd, const Image& scene, const Image& layer,
                          const Image* layer_before = nullptr, const Image* scene_before = nullptr) {
    VkImageView& scene_view = fg.hud.layer_views[U64(scene.image)];
    VkImageView& layer_view = fg.hud.layer_views[U64(layer.image)];
    if ((!scene_view && !(scene_view = MakeView(d, scene.image, SCENE_FORMAT))) ||
        (!layer_view && !(layer_view = MakeView(d, layer.image, HUD_FORMAT)))) {
        return false;
    }
    VkImageView before[3]{};
    if (layer_before && scene_before) {
        VkImageView& layer_before_view = fg.hud.layer_views[U64(layer_before->image)];
        VkImageView& scene_before_view = fg.hud.layer_views[U64(scene_before->image)];
        if ((layer_before_view || (layer_before_view = MakeView(d, layer_before->image, HUD_FORMAT))) &&
            (scene_before_view || (scene_before_view = MakeView(d, scene_before->image, SCENE_FORMAT)))) {
            before[0] = layer_before_view;
            before[1] = fg.hud.previous_view; // the frame that went out before this one
            before[2] = scene_before_view;
        }
    }
    for (uint32_t i = 0; i < fg.generated; ++i) {
        const VkImageView in[3] = {fg.hud.current_view, scene_view, layer_view};
        const VkImageView out[2] = {fg.hud.output_views[i], before[0] ? fg.markers.which_view : VK_NULL_HANDLE};
        if (before[0]) {
            const float back = 1.0f - static_cast<float>(i + 1) / static_cast<float>(fg.generated + 1);
            const float constants[4] = {back, g_layer_parameters[1], g_layer_parameters[2],
                                        static_cast<float>(fg.markers.tiles_w)};
            RunHudPass(d, fg, cmd, set.hud_sets[2 + i], MARKER_PASS + MARKER_LAYER, in, out, constants, fg.extent.width,
                       fg.extent.height, fg.curve_view, fg.markers.tiles.buffer, VK_WHOLE_SIZE, before);
        } else {
            RunHudPass(d, fg, cmd, set.hud_sets[2 + i], 3, in, out, g_layer_parameters, fg.extent.width, fg.extent.height,
                       fg.curve_view);
        }
    }
    return true;
}

/// The GPU's own memory (its device-local heaps) that this process has in use, the game's and this
/// layer's together, and how much of it the driver offers the process now, in MB. False: the driver
/// does not say.
bool GpuMemory(const Device& d, double& used, double& budget) {
    used = budget = 0.0;
    const Instance* const in = d.instance;
    if (!in || !in->GetPhysicalDeviceMemoryProperties2 || !in->EnumerateDeviceExtensionProperties) {
        return false;
    }
    static const bool supported = [&] {
        uint32_t count = 0;
        in->EnumerateDeviceExtensionProperties(d.physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> all(count);
        in->EnumerateDeviceExtensionProperties(d.physical, nullptr, &count, all.data());
        for (const VkExtensionProperties& extension : all) {
            if (!std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                return true;
            }
        }
        return false;
    }();
    if (!supported) {
        return false;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT heaps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &heaps};
    in->GetPhysicalDeviceMemoryProperties2(d.physical, &properties);
    for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; ++i) {
        if (properties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            used += static_cast<double>(heaps.heapUsage[i]) / (1024.0 * 1024.0);
            budget += static_cast<double>(heaps.heapBudget[i]) / (1024.0 * 1024.0);
        }
    }
    return true;
}

/// The two images for the scenes RecordUnmix works out. False: there are none, and such frames go
/// the old way (no generated frames for them).
bool PrepareUnmix(Device& d, Fg& fg) {
    if (fg.unmixed[1].image) {
        return true;
    }
    if (!fg.unmixed_failed && (fg.unmixed[0].image || CreateOwnImage(d, fg.unmixed[0], SCENE_FORMAT, fg.extent.width, fg.extent.height)) &&
        CreateOwnImage(d, fg.unmixed[1], SCENE_FORMAT, fg.extent.width, fg.extent.height)) {
        return true;
    }
    if (!fg.unmixed_failed) {
        Log("fg: no memory for the scenes of frames without a HUD capture");
    }
    fg.unmixed_failed = true;
    return false;
}

/// A frame came without a HUD capture of its own. The picture it shows has the HUD in it, and
/// interpolating that would drag the HUD and what is around it along with the scene. So the
/// scene is worked out (hud_unmix.comp): `target` gets this frame without what the HUD added to
/// the frame before, from that frame (fg.previous), its scene `scene_before` and the last
/// captured layer. False: a view could not be made.
bool RecordUnmix(Device& d, Fg& fg, Fg::Set& set, VkCommandBuffer cmd, const Image& scene_before, const Image& layer,
                 Image& target) {
    Fg::Hud& hud = fg.hud;
    const auto view_of = [&](const Image& image, VkFormat format) {
        VkImageView& view = hud.layer_views[U64(image.image)];
        if (!view) {
            view = MakeView(d, image.image, format);
        }
        return view;
    };
    const VkImageView before = view_of(scene_before, SCENE_FORMAT), hud_layer = view_of(layer, HUD_FORMAT),
                      out = view_of(target, SCENE_FORMAT);
    if (!before || !hud_layer || !out) {
        return false;
    }
    ToGeneral(d, cmd, target);
    VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; // (this frame and the tone curve were copied just now)
    copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copied.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &copied, 0, nullptr,
                         0, nullptr);
    const VkImageView in[3] = {hud.current_view, before, hud_layer};
    const VkImageView written[2] = {out, VK_NULL_HANDLE};
    const VkImageView more[3] = {hud.previous_view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    RunHudPass(d, fg, cmd, set.hud_sets[UNMIX_SET], UNMIX_PASS, in, written, g_layer_parameters, fg.extent.width,
               fg.extent.height, fg.curve_view, VK_NULL_HANDLE, VK_WHOLE_SIZE, more);
    return true;
}

/// Everything a swapchain needs for frame generation, made at its first generated frame.
bool Prepare(Device& d, Fg& fg, uint32_t family) {
    const double start = Now();
    double memory_before = 0.0, memory_after = 0.0, memory_budget = 0.0;
    GpuMemory(d, memory_before, memory_budget);
    fg.family = family;
    uint32_t count = 0;
    d.GetSwapchainImagesKHR(d.handle, fg.swapchain, &count, nullptr);
    fg.images.resize(count);
    if (d.GetSwapchainImagesKHR(d.handle, fg.swapchain, &count, fg.images.data()) != VK_SUCCESS) {
        return false;
    }
    const uint32_t w = fg.extent.width, h = fg.extent.height;
    if (!CreateOwnImage(d, fg.backbuffer, fg.format, w, h) ||
        !CreateOwnImage(d, fg.zero_vectors, VK_FORMAT_R16G16_SFLOAT, w / 2, h / 2) ||
        !CreateOwnImage(d, fg.zero_depth, VK_FORMAT_R32_SFLOAT, w / 2, h / 2)) {
        return false;
    }
    for (uint32_t i = 0; i < g_generated_cap; ++i) {
        if (!CreateOwnImage(d, fg.outputs[i], fg.format, w, h)) {
            return false;
        }
    }
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    if (d.CreateCommandPool(d.handle, &pool_info, nullptr, &fg.pool) != VK_SUCCESS) {
        return false;
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (Fg::Set& set : fg.sets) {
        VkCommandBuffer buffers[1 + MAX_GENERATED]{};
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = fg.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1 + g_generated_cap;
        if (d.AllocateCommandBuffers(d.handle, &allocate, buffers) != VK_SUCCESS ||
            d.CreateFence(d.handle, &fence_info, nullptr, &set.fence) != VK_SUCCESS) {
            return false;
        }
        for (uint32_t i = 0; i < 1 + g_generated_cap; ++i) {
            // Command buffers made below the loader need its dispatch pointer set by hand.
            if (d.set_loader_data) {
                d.set_loader_data(d.handle, buffers[i]);
            }
        }
        set.main = buffers[0];
        for (uint32_t i = 0; i < g_generated_cap; ++i) {
            set.blit[i] = buffers[1 + i];
        }
    }
    if (d.CreateSemaphore(d.handle, &semaphore_info, nullptr, &fg.real_ready) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t i = 0; i < g_generated_cap; ++i) {
        if (d.CreateSemaphore(d.handle, &semaphore_info, nullptr, &fg.acquired[i]) != VK_SUCCESS ||
            d.CreateSemaphore(d.handle, &semaphore_info, nullptr, &fg.blitted[i]) != VK_SUCCESS) {
            return false;
        }
    }
    const VkDeviceSize dump_size = static_cast<VkDeviceSize>(w) * h * 4;
    if (!CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                fg.dump_real.buffer, fg.dump_real.memory, &fg.dump_real.data) ||
        !CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                fg.dump_generated.buffer, fg.dump_generated.memory,
                                &fg.dump_generated.data) ||
        !CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                fg.dump_vectors.buffer, fg.dump_vectors.memory, &fg.dump_vectors.data) ||
        !CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                fg.dump_depth.buffer, fg.dump_depth.memory, &fg.dump_depth.data)) {
        return false;
    }
    if (!CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fg.dump_previous.buffer,
                                fg.dump_previous.memory, &fg.dump_previous.data) ||
        !CreateBufferWithMemory(d, dump_size * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fg.dump_hud.buffer,
                                fg.dump_hud.memory, &fg.dump_hud.data) ||
        !CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fg.dump_scene.buffer,
                                fg.dump_scene.memory, &fg.dump_scene.data) ||
        !CreateBufferWithMemory(d, dump_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, fg.dump_fixed.buffer,
                                fg.dump_fixed.memory, &fg.dump_fixed.data)) {
        return false;
    }
    fg.hud.ok = EnvFloat("SC_FG_HUD", 1.0f) != 0.0f && fg.format == VK_FORMAT_R8G8B8A8_UNORM && PrepareHud(d, fg);
    Log("fg: HUD pass %s", fg.hud.ok ? "ready" : "not in use");
    if (!CreateFfx(d, fg)) {
        return false;
    }
    fg.ready = true;
    g_fg_ready_time = Now();
    GpuMemory(d, memory_after, memory_budget);
    Log("fg: ready for %ux%u format %d, %zu swapchain images, %u generated per frame%s, queue family %u "
        "(%.0f ms; GPU memory of the process %.0f -> %.0f MB, offered to it %.0f MB)",
        w, h, fg.format, fg.images.size(), fg.generated, g_generated_cap > 3 ? " (experimental: up to x8)" : "", family,
        (Now() - start) * 1000.0, memory_before, memory_after, memory_budget);
    return true;
}

void WritePpm(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height, bool bgra) {
    FILE* file = std::fopen(path, "wb");
    if (!file) {
        return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = pixels + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = source[x * 4 + (bgra ? 2 : 0)];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + (bgra ? 0 : 2)];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

struct Spread {
    float mean{}, p50{}, p95{}, p99{}, max{};
};
Spread Summarize(std::vector<float>& values) {
    Spread spread;
    if (values.empty()) {
        return spread;
    }
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const float value : values) {
        sum += value;
    }
    const auto at = [&](double q) { return values[static_cast<size_t>(q * static_cast<double>(values.size() - 1))]; };
    spread = {static_cast<float>(sum / static_cast<double>(values.size())), at(0.5), at(0.95), at(0.99), values.back()};
    values.clear();
    return spread;
}

/// A frame is handed to the display queue now.
void NoteOutput(Fg& fg) {
    const double now = Now();
    if (fg.last_output > 0.0 && now - fg.last_output < 0.5) {
        fg.output_ms.push_back(static_cast<float>((now - fg.last_output) * 1000.0));
    }
    if (fg.last_output > 0.0) { // (the panel's graph shows the long ones too)
        fg.panel.gaps[fg.panel.gap_count++ % Fg::Panel::GAPS] =
            static_cast<float>(std::min(now - fg.last_output, 1.0) * 1000.0);
    }
    fg.last_output = now;
}

/// The game's frame time lately: the middle one of the last `span` seconds' frame times; 0 with
/// too few of them. (Not their mean: a hitch is not a slower game.)
double MedianPeriod(const Fg& fg, double span) {
    float sorted[128];
    const uint32_t have = std::min(fg.interval_count, 128u);
    uint32_t n = 0;
    for (double sum = 0.0; n < have && sum < span; ++n) {
        sorted[n] = fg.interval_ring[(fg.interval_count - 1 - n) % 128];
        sum += sorted[n];
    }
    if (n < 8) {
        return 0.0;
    }
    std::nth_element(sorted, sorted + n / 2, sorted + n);
    return sorted[n / 2];
}

/// What has come in about frames that reached the display, taken over on the presenting thread:
/// into the event log, and for the statistics line.
void TakeShown(Fg& fg) {
    Fg::Shown& s = fg.shown;
    constexpr uint32_t RING = Fg::Shown::RING;
    const uint32_t count = s.count.load(std::memory_order_acquire);
    if (count - s.read > RING) {
        s.read = count - RING;
    }
    static const float refresh_ms = 1000.0f / EnvFloat("SC_FG_TARGET", 120.0f);
    for (; s.read != count; ++s.read) {
        const Fg::Shown::Entry& e = s.ring[s.read % RING];
        if (e.event && !fg.events.empty() && e.event - 1 + EVENT_COUNT > fg.event_count) {
            fg.events[(e.event - 1) % EVENT_COUNT].shown_ms = e.delay_ms;
        }
        s.delays.push_back(e.delay_ms);
        if (e.gap_ms > 0.0f) {
            // Real frames are meant to reach the display one multiplier's worth of refreshes apart.
            const int refreshes = static_cast<int>(std::lround(e.gap_ms / refresh_ms));
            const int meant = static_cast<int>(e.multiplier);
            ++(refreshes == meant ? s.planned : refreshes < meant ? s.closer : refreshes == meant + 1 ? s.later : s.much_later);
        }
    }
    const uint32_t waited = s.waited_count.load(std::memory_order_acquire);
    if (waited - s.waited_read > RING) {
        s.waited_read = waited - RING;
    }
    for (; s.waited_read != waited; ++s.waited_read) {
        s.waits.push_back(s.waited_ms[s.waited_read % RING]);
    }
}

void Statistics(Device& d, Fg& fg, double now) {
    if (fg.stats_since == 0.0) {
        fg.stats_since = now;
    }
    const double span = now - fg.stats_since;
    if (span < 10.0) {
        return;
    }
    const uint32_t real = fg.real_frames + fg.passed_frames;
    const Spread game = Summarize(fg.game_ms), output = Summarize(fg.output_ms), age = Summarize(fg.age_ms);
    const Spread wait = Summarize(fg.wait_ms);
    Fg::Shown& screen = fg.shown;
    const uint32_t reports = static_cast<uint32_t>(screen.delays.size());
    const Spread on_screen = Summarize(screen.delays), present_wait = Summarize(screen.waits);
    const Spread sleep_late = Summarize(fg.sleep_late_ms);
    const Spread generated_on_screen = Summarize(screen.generated_delays);
    const uint64_t others_read = screen.others_read.load(std::memory_order_relaxed) - screen.others_counted;
    const Spread real_out = Summarize(fg.real_out_ms);
    const Spread after_handover = Summarize(fg.done_after_handover_ms), before_hook = Summarize(fg.done_before_hook_ms);
    Spread parts[Fg::GpuClock::MARKS - 1];
    for (uint32_t i = 0; i + 1 < Fg::GpuClock::MARKS; ++i) {
        parts[i] = Summarize(fg.clock.parts[i]);
    }
    double advance = 0.0, least = 0.0;
    {
        std::lock_guard lock{g_limiter.gate_mutex};
        advance = g_limiter.advance;
        least = g_limiter.hook_least;
    }
    const uint32_t lag_waits = g_lag_waits.exchange(0);
    double memory_used = 0.0, memory_budget = 0.0;
    GpuMemory(d, memory_used, memory_budget);
    // "low" rates are what the slowest 1 % of frames amount to.
    Log("STAT x%u real=%.1f/s generated=%.1f/s shown=%.1f/s untouched=%u | game frame ms mean=%.1f p50=%.1f "
        "p95=%.1f p99=%.1f max=%.1f | shown frame ms mean=%.1f p50=%.1f p95=%.1f p99=%.1f max=%.1f | "
        "hook ms=%.2f hud=%u layer=%u | inputs game=%u blank=%u checked=%u evals/frame=%.2f age ms p50=%.1f p99=%.1f "
        "max=%.1f | "
        "ahead=%u stale=%u held=%u lag=%.1f/%.1f measure ms=%.1f | chain=%u dup=%u unlinked=%u nohud=%u multi=%u "
        "shift=%d ahead=%.2f fit=%.1f/%.1f | fg gpu wait ms p50=%.1f p99=%.1f max=%.1f ready=%.1f tail=%.1f | limit %u of %u sleep ms=%.1f "
        "game gpu ms=%.1f (late %u) free=%.0f/s of %.0f probes=%u | gate=%u advance ms=%.1f least ms=%.1f own ms=%.1f lead ms=%.1f guard=%u ms=%.1f "
        "failed=%u first=%u ms=%.1f ahead=%u late copies=%u lost=%u | "
        "latency calls sleep=%u marker=%u | kept back for the GPU %u ms=%.1f paced=%u | "
        "moving hud frames=%u tiles=%u found=%u pieces=%u over=%u pictures=%u | hud gaps filled=%u cross=%u | "
        "gpu memory used=%.0f of %.0f MB | "
        "present to display ms p50=%.1f p95=%.1f max=%.1f reports=%u unmatched=%u | "
        "real frames on the display as planned=%u closer=%u later=%u much later=%u | present wait ms p50=%.1f | boost=%d | "
        "per call ms: acquire=%.2f present=%.2f | sleeps ended late ms p50=%.2f p99=%.2f max=%.2f | priority=%d | "
        "generated frames to display ms p50=%.1f p95=%.1f | never on the display real=%u generated=%u | "
        "frames on the display after 0 refreshes=%u 1=%u 2=%u more=%u | no report here real=%u generated=%u | "
        "reports read by others=%llu | real frame goes out ms p50=%.1f p95=%.1f | "
        "game frame done on the GPU: after its hand-over ms p50=%.1f p95=%.1f, before the present hook ms p50=%.1f p95=%.1f | "
        "fg GPU ms by step: before=%.2f prepare=%.2f flow=%.2f interpolation=%.2f hud=%.2f after=%.2f",
        fg.too_fast ? 1 : fg.generated + 1, real / span, fg.generated_frames / span, (real + fg.generated_frames) / span,
        fg.passed_frames, game.mean, game.p50, game.p95, game.p99, game.max, output.mean, output.p50, output.p95,
        output.p99, output.max, real ? fg.hook_seconds / real * 1000.0 : 0.0, fg.hud_frames, fg.layer_frames, fg.game_inputs, fg.blank_inputs,
        fg.checked_frames, fg.real_frames ? static_cast<double>(fg.evaluations) / fg.real_frames : 0.0, age.p50, age.p99, age.max,
        fg.ahead_frames, fg.stale_frames, fg.held, fg.input_lag, fg.hud_lag, fg.measure_ms, fg.chained_frames,
        fg.duplicate_frames, fg.unlinked_frames, fg.nohud_frames, fg.multi_frames, fg.chain_shift,
        real ? fg.ahead_sum / real : 0.0, fg.sync_fit, fg.sync_best, wait.p50, wait.p99, wait.max, fg.ready_seconds * 1000.0,
fg.tail_seconds * 1000.0,
        fg.limit_held,
        fg.limit_frames, fg.limit_held ? fg.limit_slept / fg.limit_held * 1000.0 : 0.0,
        fg.limit_gpu_frames ? fg.limit_gpu_seconds / fg.limit_gpu_frames * 1000.0 : 0.0, fg.limit_gpu_late,
        fg.binding ? std::min(fg.free_last, fg.free_bound) : 0.0, fg.free_bound, std::exchange(fg.probe_stat, 0u),
        fg.limit_gated, advance * 1000.0, least * 1000.0,
        fg.limit_own_frames ? fg.limit_own_seconds / fg.limit_own_frames * 1000.0 : 0.0, g_limiter.lead.load() * 1000.0,
        fg.guard_waits,
        fg.guard_waits ? fg.guard_seconds / fg.guard_waits * 1000.0 : 0.0, fg.guard_timeouts, fg.first_waits,
        fg.first_waits ? fg.first_seconds / fg.first_waits * 1000.0 : 0.0, fg.first_ahead, g_late_copies.exchange(0),
        g_lost_copies.exchange(0),
        g_latency_sleeps.exchange(0), g_latency_markers.exchange(0), lag_waits,
        lag_waits ? g_lag_us.exchange(0) / 1000.0 / lag_waits : 0.0, g_limiter.gpu_paced.load(), fg.markers.frames,
        fg.markers.seen, fg.markers.found, fg.markers.pieces, fg.markers.over, fg.markers.pictures, fg.unmixed_frames,
        g_cross_captures.exchange(0), memory_used, memory_budget, on_screen.p50, on_screen.p95, on_screen.max, reports,
        screen.unmatched.exchange(0), screen.planned, screen.closer, screen.later, screen.much_later, present_wait.p50,
        fg.boost_on ? 1 : 0, fg.acquires ? fg.acquire_seconds / fg.acquires * 1000.0 : 0.0,
        fg.present_calls ? fg.present_seconds / fg.present_calls * 1000.0 : 0.0, sleep_late.p50, sleep_late.p99, sleep_late.max,
        static_cast<int>(g_live_priority.load()), generated_on_screen.p50, generated_on_screen.p95, screen.lost_real,
        screen.lost_generated, screen.refreshes[0], screen.refreshes[1], screen.refreshes[2], screen.refreshes[3],
        screen.silent_real, screen.silent_generated, static_cast<unsigned long long>(others_read), real_out.p50,
        real_out.p95, after_handover.p50, after_handover.p95, before_hook.p50, before_hook.p95,
        parts[0].p50, parts[1].p50, parts[2].p50, parts[3].p50, parts[4].p50, parts[5].p50);
    screen.lost_real = screen.lost_generated = screen.silent_real = screen.silent_generated = 0;
    screen.others_counted += others_read;
    screen.refreshes[0] = screen.refreshes[1] = screen.refreshes[2] = screen.refreshes[3] = 0;
    screen.planned = screen.closer = screen.later = screen.much_later = 0;
    fg.acquire_seconds = fg.present_seconds = 0.0;
    fg.acquires = fg.present_calls = 0;
    fg.unmixed_frames = 0;
    fg.markers.frames = fg.markers.seen = fg.markers.found = fg.markers.pieces = fg.markers.over = fg.markers.pictures = 0;
    fg.limit_gated = fg.limit_own_frames = fg.guard_waits = fg.guard_timeouts = fg.first_waits = fg.first_ahead = 0;
    fg.limit_own_seconds = fg.guard_seconds = fg.first_seconds = 0.0;
    fg.ahead_frames = fg.stale_frames = fg.held = 0;
    fg.chained_frames = fg.duplicate_frames = fg.unlinked_frames = fg.nohud_frames = fg.multi_frames = 0;
    fg.ahead_sum = 0.0;
    fg.limit_frames = fg.limit_held = fg.limit_gpu_frames = fg.limit_gpu_late = 0;
    fg.limit_slept = fg.limit_gpu_seconds = 0.0;
    fg.measure_ms = 0.0f;
    fg.stats_since = now;
    fg.real_frames = fg.generated_frames = fg.passed_frames = 0;
    fg.game_inputs = fg.blank_inputs = fg.checked_frames = 0;
    fg.hud_frames = fg.layer_frames = 0;
    fg.evaluations = 0;
    fg.hook_seconds = 0.0;
}

// ----------------------------------------------------------------------------- performance panel

/// The game presents a frame: for the panel's rate of the game's own frames.
void PanelNoteFrame(Fg& fg, double now) {
    Fg::Panel& panel = fg.panel;
    if (panel.last_frame > 0.0) {
        panel.frame_gaps[panel.frame_count++ % Fg::Panel::FRAMES] =
            static_cast<float>(std::min(now - panel.last_frame, 1.0) * 1000.0);
    }
    panel.last_frame = now;
}

/// Frames a second over the last second of a ring of frame gaps (ms), as far back as frames came
/// without a break; 0: too few to say.
float PanelRate(const float* ring, uint32_t count, uint32_t size) {
    double sum = 0.0;
    uint32_t n = 0;
    for (const uint32_t have = std::min(count, size); n < have && sum < 1000.0; ++n) {
        const float gap = ring[(count - 1 - n) % size];
        if (gap >= 500.0f) {
            break;
        }
        sum += gap;
    }
    return n >= 2 && sum > 0.0 ? static_cast<float>(n * 1000.0 / sum) : 0.0f;
}

/// The keyboard, for the panel's key (panel_key.h). It learns of the game's window when that is made
/// (the surface hooks below) and is read by the presenting thread.
panel_key::Reader g_panel_keys;

/// What is asked of the panel: the control file PANEL, "STEP [tl|tr|bl|br]" (sc-turbo fg-panel
/// writes it, and a desktop shortcut can be bound to that). Looked at up to twenty times a second,
/// so that a key press shows at once; read only when it has changed. It is left where it is: the
/// panel comes back as it was with the next launch.
void ReadPanelRequest(Fg& fg, double now) {
    // Only where it was asked for at launch (SC_FG_PANEL=1). Without that the control file is not
    // even looked at and the panel never comes into being: a frame-rate display that sits below
    // this layer (MangoHud) counts the generated frames as well and needs nothing from here.
    static const bool wanted = EnvFloat("SC_FG_PANEL", 0.0f) != 0.0f;
    if (!wanted) {
        return;
    }
    Fg::Panel& panel = fg.panel;
    static const char* const names[4] = {"top left", "top right", "bottom left", "bottom right"};
    static const char* const steps[4] = {"off", "the shown rate", "the shown rate and the game's",
                                         "both rates and the pacing graph"};
    char path[600];
    std::snprintf(path, sizeof path, "%s/PANEL", Dir());
    // The key: a press steps the panel on, and the control file is written anew so that it says what
    // is shown (the launcher and the next launch read it). Looked at with every frame: on Wayland
    // that hands out the key events that have arrived, which costs nothing when there are none.
    if (!panel.key.code) {
        const char* asked = std::getenv("SC_FG_PANEL_KEY");
        if (!asked || !panel_key::Parse(asked, panel.key)) {
            panel_key::Parse("Shift+7", panel.key);
        }
    }
    if (const int presses = g_panel_keys.Poll(panel.key)) {
        panel.step = (panel.step + presses) % 4;
        Log("fg: performance panel: %s%s%s (%s)", steps[panel.step], panel.step ? ", " : "",
            panel.step ? names[panel.corner] : "", panel.key.text);
        static const char* const corners[4] = {"tl", "tr", "bl", "br"};
        char fresh[620];
        std::snprintf(fresh, sizeof fresh, "%s.layer", path);
        if (FILE* file = std::fopen(fresh, "w")) {
            std::fprintf(file, "%d %s %s\n", panel.step, corners[panel.corner], panel.key.text);
            std::fclose(file);
            struct stat written {};
            if (std::rename(fresh, path) == 0 && stat(path, &written) == 0) {
                // (its own writing is not a request)
                panel.stamp = static_cast<int64_t>(written.st_mtim.tv_sec) * 1'000'000'000 + written.st_mtim.tv_nsec;
                panel.inode = static_cast<uint64_t>(written.st_ino);
            }
        }
    }
    if (const char* source = g_panel_keys.Source(); source != panel.key_source && *source) {
        panel.key_source = source;
        Log("fg: performance panel: %s steps it on, read from the game's own keyboard (%s)", panel.key.text, source);
    } else if (!*source && g_panel_keys.Failed() && !panel.key_none_said) {
        panel.key_none_said = true;
        Log("fg: performance panel: no keyboard can be read here; \"sc-turbo fg-panel next\" steps it on");
    }
    if (now - panel.checked < 0.05) {
        return;
    }
    panel.checked = now;
    struct stat status {};
    const bool there = stat(path, &status) == 0;
    const int64_t stamp =
        there ? static_cast<int64_t>(status.st_mtim.tv_sec) * 1'000'000'000 + status.st_mtim.tv_nsec : 0;
    const uint64_t inode = there ? static_cast<uint64_t>(status.st_ino) : 0;
    if (stamp == panel.stamp && inode == panel.inode) {
        return;
    }
    panel.stamp = stamp;
    panel.inode = inode;
    int step = 0;
    char where[8] = "", key[40] = "";
    if (there) {
        if (FILE* file = std::fopen(path, "r")) {
            if (std::fscanf(file, "%d %7s %39s", &step, where, key) < 1) {
                step = 0;
            }
            std::fclose(file);
        }
    }
    step = std::clamp(step, 0, 3);
    const int corner = !std::strcmp(where, "tr") ? 1 : !std::strcmp(where, "bl") ? 2 : !std::strcmp(where, "br") ? 3 : 0;
    if (step != panel.step || corner != panel.corner) {
        Log("fg: performance panel: %s%s%s", steps[step], step ? ", " : "", step ? names[corner] : "");
        panel.step = step;
        panel.corner = corner;
    }
    // (the key, where the file names one: "sc-turbo fg-panel key=..." and the launch write it)
    if (panel_key::Combo asked; *key && panel_key::Parse(key, asked) && std::strcmp(asked.text, panel.key.text)) {
        panel.key = asked;
        Log("fg: performance panel: the key is %s now", panel.key.text);
    } else if (*key && !panel_key::Parse(key, asked)) {
        static char refused[40] = "";
        if (std::strcmp(refused, key)) {
            std::snprintf(refused, sizeof refused, "%s", key);
            Log("fg: performance panel: \"%s\" is no key this layer knows; it stays %s", key, panel.key.text);
        }
    }
}

/// The panel's image and what puts it over a frame, made when it is first asked for (the frame
/// generation's own things have to be there). False: no panel on this swapchain.
bool PreparePanel(Device& d, Fg& fg) {
    Fg::Panel& panel = fg.panel;
    if (panel.ready || panel.failed) {
        return panel.ready;
    }
    panel.failed = true; // until all of it is there
    panel.scale = fg.extent.height > 1800 ? 1 : 0;
    int width = 0, height = 0;
    hud_panel::Size(3, panel.scale, width, height);
    // Its pass writes into this layer's copies of the frames: 8-bit RGBA, as the HUD passes need them.
    if (!fg.hud.ok || static_cast<uint32_t>(width) + 64 > fg.extent.width ||
        static_cast<uint32_t>(height) + 64 > fg.extent.height) {
        Log("fg: no performance panel on this swapchain (%s)",
            fg.hud.ok ? "too small" : "it needs the HUD passes: an 8-bit RGBA swapchain");
        return false;
    }
    Fg::Hud& hud = fg.hud;
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = sizeof PANEL_SPV;
    shader.pCode = PANEL_SPV;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.pName = "main";
    pipeline.layout = hud.layout;
    VkCommandBufferAllocateInfo buffers{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffers.commandPool = fg.pool;
    buffers.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffers.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sets.descriptorPool = hud.pool;
    sets.descriptorSetCount = 1;
    sets.pSetLayouts = &hud.set_layout;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    bool ok = d.CreateShaderModule(d.handle, &shader, nullptr, &hud.shaders[PANEL_PASS]) == VK_SUCCESS &&
              (pipeline.stage.module = hud.shaders[PANEL_PASS],
               d.CreateComputePipelines(d.handle, VK_NULL_HANDLE, 1, &pipeline, nullptr, &hud.pipelines[PANEL_PASS])) ==
                  VK_SUCCESS &&
              CreateOwnImage(d, panel.image, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(width),
                             static_cast<uint32_t>(height)) &&
              (panel.view = MakeView(d, panel.image.image, VK_FORMAT_R8G8B8A8_UNORM)) &&
              d.AllocateCommandBuffers(d.handle, &buffers, &panel.pass_cmd) == VK_SUCCESS &&
              d.CreateFence(d.handle, &fence, nullptr, &panel.pass_fence) == VK_SUCCESS &&
              d.AllocateDescriptorSets(d.handle, &sets, &panel.pass_set) == VK_SUCCESS &&
              CreateBufferWithMemory(d, 2 * static_cast<VkDeviceSize>(width + 32) * static_cast<VkDeviceSize>(height + 32) * 4,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, panel.dump.buffer, panel.dump.memory,
                                     &panel.dump.data);
    for (HostBuffer& staging : panel.staging) {
        ok = ok && CreateBufferWithMemory(d, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging.buffer,
                                          staging.memory, &staging.data);
    }
    if (!ok) {
        Log("fg: the performance panel could not be set up");
        return false; // (what was made goes with the swapchain)
    }
    if (d.set_loader_data) {
        d.set_loader_data(d.handle, panel.pass_cmd);
    }
    panel.failed = false;
    panel.ready = true;
    return true;
}

/// Draws the panel as it is to be shown now and records its way into its image. `slot`: whose
/// staging buffer (a command set's number, or COMMAND_SETS: a frame that passes through).
/// `multiplier`: shown frames per frame of the game's at the moment. False: no panel.
bool RecordPanel(Device& d, Fg& fg, VkCommandBuffer cmd, int slot, double now, int multiplier) {
    Fg::Panel& panel = fg.panel;
    if (panel.step <= 0 || !PreparePanel(d, fg)) {
        return false;
    }
    // The gaps the graph has room for, the newest last.
    float gaps[Fg::Panel::GAPS];
    const int count = static_cast<int>(std::min<uint32_t>(
        {panel.gap_count, Fg::Panel::GAPS, static_cast<uint32_t>(hud_panel::GraphColumns(panel.scale))}));
    for (int i = 0; i < count; ++i) {
        gaps[i] = panel.gaps[(panel.gap_count - static_cast<uint32_t>(count) + static_cast<uint32_t>(i)) % Fg::Panel::GAPS];
    }
    static const float target_ms = 1000.0f / EnvFloat("SC_FG_TARGET", 120.0f);
    hud_panel::Stats stats;
    stats.target_ms = target_ms;
    stats.gaps = gaps;
    stats.count = count;
    // The numbers are renewed four times a second: faster than that they cannot be read.
    if (panel.rated == 0.0 || now - panel.rated >= 0.25) {
        panel.rated = now;
        panel.shown = PanelRate(panel.gaps, panel.gap_count, Fg::Panel::GAPS);
        panel.base = PanelRate(panel.frame_gaps, panel.frame_count, Fg::Panel::FRAMES);
        panel.multiplier = multiplier;
        hud_panel::Levels(stats, panel.middle, panel.high);
    }
    stats.shown = panel.shown;
    stats.base = panel.base;
    stats.multiplier = panel.multiplier;
    stats.middle = panel.middle;
    stats.high = panel.high;
    if (panel.back_step != panel.step) {
        hud_panel::Back(panel.back, panel.step, panel.scale);
        panel.back_step = panel.step;
    }
    hud_panel::Draw(panel.canvas, panel.step, stats, panel.scale, &panel.back);
    panel.width = panel.canvas.w;
    panel.height = panel.canvas.h;
    const int margin = static_cast<int>(std::lround(18.0f * hud_font::SCALE[panel.scale]));
    const int frame_w = static_cast<int>(fg.extent.width), frame_h = static_cast<int>(fg.extent.height);
    panel.x = panel.corner & 1 ? frame_w - panel.width - margin : margin;
    panel.y = panel.corner & 2 ? frame_h - panel.height - margin : margin;
    std::memcpy(panel.staging[slot].data, panel.canvas.px.data(), panel.canvas.px.size());

    ToGeneral(d, cmd, panel.image);
    VkBufferImageCopy upload{};
    upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    upload.imageExtent = {static_cast<uint32_t>(panel.width), static_cast<uint32_t>(panel.height), 1};
    d.CmdCopyBufferToImage(cmd, panel.staging[slot].buffer, panel.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &upload);
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &written, 0,
                         nullptr, 0, nullptr);
    return true;
}

/// The panel over one of this layer's copies of a frame (`view`: 8-bit RGBA of the frame's size).
void RecordPanelOver(Device& d, Fg& fg, VkCommandBuffer cmd, VkDescriptorSet set, VkImageView view) {
    const Fg::Panel& panel = fg.panel;
    const VkImageView in[3] = {panel.view, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkImageView out[2] = {view, VK_NULL_HANDLE};
    const float place[4] = {static_cast<float>(panel.x), static_cast<float>(panel.y), static_cast<float>(panel.width),
                            static_cast<float>(panel.height)};
    RunHudPass(d, fg, cmd, set, PANEL_PASS, in, out, place, static_cast<uint32_t>(panel.width),
               static_cast<uint32_t>(panel.height));
}

/// The panel's part of a frame, between this layer's copy of the frame and the game's image.
VkImageCopy PanelPart(const Fg& fg) {
    const Fg::Panel& panel = fg.panel;
    VkImageCopy part{};
    part.srcSubresource = part.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    part.srcOffset = part.dstOffset = {panel.x, panel.y, 0};
    part.extent = {static_cast<uint32_t>(panel.width), static_cast<uint32_t>(panel.height), 1};
    return part;
}

/// Tests (PANELDUMP): the panel's part of `image` and a little around it into the dump buffer's
/// first or second half.
void RecordPanelDump(Device& d, Fg& fg, VkCommandBuffer cmd, VkImage image, int half) {
    Fg::Panel& panel = fg.panel;
    panel.dump_x = std::max(panel.x - 16, 0);
    panel.dump_y = std::max(panel.y - 16, 0);
    panel.dump_width = std::min(panel.width + 32, static_cast<int>(fg.extent.width) - panel.dump_x);
    panel.dump_height = std::min(panel.height + 32, static_cast<int>(fg.extent.height) - panel.dump_y);
    VkBufferImageCopy region{};
    region.bufferOffset = half ? static_cast<VkDeviceSize>(panel.dump_width) * static_cast<VkDeviceSize>(panel.dump_height) * 4 : 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {panel.dump_x, panel.dump_y, 0};
    region.imageExtent = {static_cast<uint32_t>(panel.dump_width), static_cast<uint32_t>(panel.dump_height), 1};
    d.CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, panel.dump.buffer, 1, &region);
}

void WritePanelDump(Fg& fg, const char* what, int half) {
    const Fg::Panel& panel = fg.panel;
    char path[600];
    std::snprintf(path, sizeof path, "%s/paneldump-%d-%03u-%s.ppm", Dir(), getpid(), panel.dumps, what);
    const size_t offset = half ? static_cast<size_t>(panel.dump_width) * static_cast<size_t>(panel.dump_height) * 4 : 0;
    WritePpm(path, static_cast<const uint8_t*>(panel.dump.data) + offset, static_cast<uint32_t>(panel.dump_width),
             static_cast<uint32_t>(panel.dump_height), false);
}

uint64_t PresentIdOf(const VkPresentInfoKHR& info);

#ifdef VK_EXT_present_timing
bool CalibrateShown(Device& d, Fg& fg, const VkPastPresentationTimingEXT& timing, uint32_t stage, double now);

/// A frame's request to be told when it reaches the display: goes into its present's chain.
struct OwnTiming {
    VkPresentTimingInfoEXT one{VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    VkPresentTimingsInfoEXT all{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT};
    VkPresentId2KHR ids{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    uint64_t id{};
};

/// Whether this layer can ask (the swapchain was made for it, which vkd3d-proton does): the first
/// time, the stage nearest the display that this surface can report and the clock to report it in.
bool PrepareOwnTiming(Device& d, Fg& fg) {
    Fg::Shown& s = fg.shown;
    if (s.own_state) {
        return s.own_state > 0;
    }
    s.own_state = -1;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR capabilities_of = nullptr;
    if (d.instance) {
        capabilities_of = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            d.instance->gipa(d.instance->handle, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    }
    if (!(fg.create_flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) ||
        !(fg.create_flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR) || !capabilities_of || !fg.surface ||
        !d.GetPastPresentationTimingEXT || !d.GetSwapchainTimeDomainPropertiesEXT || !d.SetSwapchainPresentTimingQueueSizeEXT) {
        Log("fg: this swapchain cannot report when frames reach the display (flags 0x%x)", fg.create_flags);
        return false;
    }
    VkPresentTimingSurfaceCapabilitiesEXT timing{VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
    VkSurfaceCapabilities2KHR capabilities{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &timing};
    VkPhysicalDeviceSurfaceInfo2KHR surface{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
    surface.surface = fg.surface;
    if (capabilities_of(d.physical, &surface, &capabilities) != VK_SUCCESS || !timing.presentTimingSupported) {
        Log("fg: this display cannot report when frames reach it");
        return false;
    }
    for (uint32_t bit = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT; bit && !s.own_stage; bit >>= 1) {
        s.own_stage = timing.presentStageQueries & bit;
    }
    VkTimeDomainKHR domains[8];
    uint64_t ids[8], counter = 0;
    VkSwapchainTimeDomainPropertiesEXT properties{VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
    properties.timeDomainCount = 8;
    properties.pTimeDomains = domains;
    properties.pTimeDomainIds = ids;
    bool found = false;
    if (s.own_stage && d.GetSwapchainTimeDomainPropertiesEXT(d.handle, fg.swapchain, &properties, &counter) >= 0) {
        for (uint32_t i = 0; i < properties.timeDomainCount && !found; ++i) {
            if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) {
                s.own_domain = ids[i];
                found = true;
            }
        }
    }
    if (!found || d.SetSwapchainPresentTimingQueueSizeEXT(d.handle, fg.swapchain, 2 * Fg::Shown::MINE_MOST) != VK_SUCCESS) {
        Log("fg: no clock to have the display's reports in (stage 0x%x)", s.own_stage);
        return false;
    }
    s.own_state = 1;
    s.fates.resize(Fg::Shown::FATES);
    s.own_active.store(true, std::memory_order_relaxed);
    Log("fg: every frame sent out asks when it reaches the display (stage 0x%x)", s.own_stage);
    return true;
}

/// Into the chain of a present, in front of `next`. False: not this one (too many unanswered).
bool RequestTiming(Fg& fg, OwnTiming& own, const void* next, bool real) {
    Fg::Shown& s = fg.shown;
    // A request holds a place in the driver's queue until someone has read its report.
    const uint64_t answered = s.own_reports + s.others_read.load(std::memory_order_relaxed);
    if (g_own_timing_failed.load() || s.mine_sent - s.mine_read >= Fg::Shown::MINE - 1 ||
        (s.mine_sent > answered && s.mine_sent - answered >= Fg::Shown::MINE_MOST)) {
        return false;
    }
    own.one.timeDomainId = s.own_domain;
    own.one.presentStageQueries = s.own_stage;
    own.all.swapchainCount = 1;
    own.all.pTimingInfos = &own.one;
    own.all.pNext = next;
    own.id = real && s.own_real >= Fg::Shown::IDS_AFTER ? s.own_real - Fg::Shown::IDS_AFTER + 1 : 0;
    if (own.id) {
        s.own_last_id.store(own.id, std::memory_order_relaxed);
        own.ids.pNext = next;
        own.ids.swapchainCount = 1;
        own.ids.pPresentIds = &own.id;
        own.all.pNext = &own.ids;
    }
    return true;
}

/// It has gone out, at `time`.
void NoteRequested(Fg& fg, const OwnTiming& own, VkResult result, double time, uint32_t multiplier, bool real, double call,
                   uint32_t position) {
    Fg::Shown& s = fg.shown;
    if (result < 0) {
        // (A window that changes size says "out of date": nothing to do with the request.)
        if (result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_ERROR_SURFACE_LOST_KHR && !g_own_timing_failed.exchange(true)) {
            Log("fg: a present that asked for its time failed (%d): no more such requests", result);
        }
        return;
    }
    s.own_real += real;
    s.mine[s.mine_sent++ % Fg::Shown::MINE] = {time, fg.events.empty() ? 0 : fg.event_count + 1, own.id, s.own_real, multiplier, real,
                                             static_cast<float>(call * 1000.0), static_cast<uint8_t>(position)};
}

void NoteFate(Fg::Shown& s, const Fg::Shown::Mine& frame, double shown, uint8_t flags) {
    if (!s.fates.empty()) {
        s.fates[s.fate_count++ % Fg::Shown::FATES] = {frame.time, shown, frame.call_ms, static_cast<uint8_t>(flags | (frame.real ? 1 : 0)),
                                                      frame.position, static_cast<uint8_t>(frame.multiplier), 0};
    }
}

/// What the display has reported about the frames sent out.
void TakeOwnTimings(Device& d, Fg& fg) {
    Fg::Shown& s = fg.shown;
    if (s.own_state <= 0 || s.mine_read == s.mine_sent) {
        return;
    }
    const double now = Now();
    const auto silent = [&](const Fg::Shown::Mine& frame) {
        ++(frame.real ? s.silent_real : s.silent_generated);
        NoteFate(s, frame, 0.0, 4);
    };
    // No report will come for these any more: someone else has had it.
    while (s.mine_read != s.mine_sent && now - s.mine[s.mine_read % Fg::Shown::MINE].time > 2.0) {
        silent(s.mine[s.mine_read++ % Fg::Shown::MINE]);
    }
    constexpr uint32_t MOST = 32;
    VkPastPresentationTimingEXT timings[MOST];
    VkPresentStageTimeEXT stages[MOST];
    for (uint32_t i = 0; i < MOST; ++i) {
        timings[i] = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT};
        timings[i].presentStageCount = 1;
        timings[i].pPresentStages = &stages[i];
    }
    VkPastPresentationTimingInfoEXT info{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
    info.swapchain = fg.swapchain;
    VkPastPresentationTimingPropertiesEXT properties{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
    properties.presentationTimingCount = MOST;
    properties.pPresentationTimings = timings;
    if (d.GetPastPresentationTimingEXT(d.handle, &info, &properties) < 0) {
        return;
    }
    static const double refresh = 1.0 / EnvFloat("SC_FG_TARGET", 120.0f);
    const auto lost = [&](const Fg::Shown::Mine& frame) {
        ++(frame.real ? s.lost_real : s.lost_generated);
        NoteFate(s, frame, 0.0, 2);
    };
    for (uint32_t i = 0; i < properties.presentationTimingCount; ++i) {
        const VkPastPresentationTimingEXT& timing = timings[i];
        if (!timing.reportComplete) {
            continue;
        }
        ++s.own_reports;
        // Whether it reached the display, and how long after the frame before: whichever frame it is.
        const bool on_display = timing.presentStageCount && stages[i].time;
        double shown = 0.0;
        if (on_display) {
            const bool clock = CalibrateShown(d, fg, timing, stages[i].stage, now);
            if (!s.said) {
                s.said = true;
                Log("fg: the display reports when frames reach it (stage 0x%x, time domain %d, its clock %s)", stages[i].stage,
                    static_cast<int>(timing.timeDomain), clock ? "compared with this layer's" : "not comparable: taken as the report's arrival");
            }
            shown = clock ? static_cast<double>(stages[i].time) * 1e-9 + s.offset : now;
            if (s.last_any > 0.0) {
                ++s.refreshes[std::clamp<long>(std::lround((shown - s.last_any) / refresh), 0, 3)];
            }
            s.last_any = shown;
        }
        // The frame it is about: the next one with its id. A report that fits none changes nothing;
        // frames passed over on the way to the right one had their reports go elsewhere.
        uint64_t at = s.mine_read;
        while (at != s.mine_sent && s.mine[at % Fg::Shown::MINE].id != timing.presentId) {
            ++at;
        }
        if (at == s.mine_sent) {
            s.unmatched.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (; s.mine_read != at; ++s.mine_read) {
            silent(s.mine[s.mine_read % Fg::Shown::MINE]);
        }
        const Fg::Shown::Mine* frame = &s.mine[s.mine_read++ % Fg::Shown::MINE];
        if (!on_display) {
            lost(*frame);
            continue;
        }
        if (shown < frame->time - 0.001 || shown > frame->time + 1.0) {
            s.unmatched.fetch_add(1, std::memory_order_relaxed);
            NoteFate(s, *frame, shown, 8);
            continue;
        }
        NoteFate(s, *frame, shown, 0);
        const auto delay = static_cast<float>((shown - frame->time) * 1000.0);
        if (!frame->real) {
            s.generated_delays.push_back(delay);
            continue;
        }
        const uint32_t count = s.count.load(std::memory_order_relaxed);
        Fg::Shown::Entry& entry = s.ring[count % Fg::Shown::RING];
        entry.event = frame->event;
        entry.delay_ms = delay;
        entry.gap_ms = s.last_serial + 1 == frame->serial ? static_cast<float>((shown - s.last_shown) * 1000.0) : 0.0f;
        entry.multiplier = frame->multiplier;
        s.count.store(count + 1, std::memory_order_release);
        s.last_serial = frame->serial;
        s.last_shown = shown;
    }
}

/// Whether this layer asks for the times of this present's frames: where it is switched on and the
/// game's own present neither has an id nor asks (then vkd3d-proton is reading them itself).
bool WantOwnTiming(Device& d, Fg& fg, const VkPresentInfoKHR& info) {
    if (g_live_timing.load() == 0.0f || g_own_timing_failed.load() || PresentIdOf(info)) {
        return false;
    }
    for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) {
            return false;
        }
    }
    return PrepareOwnTiming(d, fg);
}
#else
struct OwnTiming {
    VkBaseInStructure all{};
};
inline bool RequestTiming(Fg&, OwnTiming&, const void*, bool) { return false; }
inline void NoteRequested(Fg&, const OwnTiming&, VkResult, double, uint32_t, bool, double, uint32_t) {}
inline void TakeOwnTimings(Device&, Fg&) {}
inline bool WantOwnTiming(Device&, Fg&, const VkPresentInfoKHR&) { return false; }
#endif

/// The game presents image `index`. Returns false when the frame must simply pass through.
bool Generate(Device& d, Fg& fg, VkQueue queue, uint32_t family, const VkPresentInfoKHR& info,
              VkResult& result) {
    const double now = Now();
    if (now - fg.control_checked >= 1.0) {
        fg.control_checked = now;
        const bool off = ControlFile("OFF", false);
        if (off != fg.off) {
            Log("fg: %s", off ? "switched off (OFF file)" : "switched on");
            fg.off = off;
        }
        {
            char dump_path[600];
            std::snprintf(dump_path, sizeof dump_path, "%s/DUMP", Dir());
            if (FILE* file = std::fopen(dump_path, "r")) {
                unsigned frames = 1; // a number in the file: that many frames in a row (sc-turbo fg-dump N)
                if (std::fscanf(file, "%u", &frames) != 1) {
                    frames = 1;
                }
                std::fclose(file);
                unlink(dump_path);
                fg.dump_next = true;
                fg.dump_more = std::clamp(frames, 1u, 8u) - 1;
            }
        }
        if (ControlFile("PANELDUMP", true)) {
            fg.panel.dump_next = true;
        }
        ReadParameters();
        if (g_trace && ControlFile("HUDSCAN", true)) {
            g_scan_armed = true;
            g_trace_until = Now() + 0.4;
            Log("fg: scanning the display-size passes");
        }
        if (g_trace) {
            ScanWrite();
        }
        if (g_trace && ControlFile("TRACE", true)) {
            g_trace_until = Now() + 0.4;
            Log("fg: tracing the frame structure for 0.4 s");
        }
        const bool layer_off = ControlFile("HUDLAYEROFF", false);
        if (layer_off != fg.layer_off) {
            Log("fg: HUD layer use switched %s", layer_off ? "off (HUDLAYEROFF file)" : "on");
            fg.layer_off = layer_off;
        }
        if (ControlFile("LAYER", false)) {
            char path[600];
            std::snprintf(path, sizeof path, "%s/LAYER", Dir());
            if (FILE* file = std::fopen(path, "r")) {
                float v[3] = {g_layer_parameters[0], g_layer_parameters[1], g_layer_parameters[2]};
                if (std::fscanf(file, "%f %f %f", &v[0], &v[1], &v[2]) >= 1) {
                    std::copy(v, v + 3, g_layer_parameters);
                    Log("fg: layer composite mode=%.0f gain=%.2f floor=%.4f", v[0], v[1], v[2]);
                }
                std::fclose(file);
            }
            ControlFile("LAYER", true);
        }
        const bool no_cap = ControlFile("NOCAP", false);
        if (no_cap != fg.no_cap) {
            Log("fg: output %s", no_cap ? "not limited (NOCAP file)" : "limited to the display's rate");
            fg.no_cap = no_cap;
        }
        const bool scd_on = ControlFile("SCDON", false);
        if (scd_on != fg.scd_on) {
            Log("fg: FSR's scene-change detector %s", scd_on ? "in use (SCDON file)" : "ignored");
            fg.scd_on = scd_on;
        }
        const bool hud_off = ControlFile("HUDOFF", false);
        if (hud_off != fg.hud_off) {
            Log("fg: HUD pass switched %s", hud_off ? "off (HUDOFF file)" : "on");
            fg.hud_off = hud_off;
        }
        char path[600];
        std::snprintf(path, sizeof path, "%s/MULT", Dir());
        if (FILE* file = std::fopen(path, "r")) {
            unsigned multiplier = 0;
            if (std::fscanf(file, "%u", &multiplier) == 1 && multiplier >= 2 && multiplier <= MAX_GENERATED + 1) {
                // (above x4 only where the launch had the experimental setting: what it made room for)
                if (multiplier > g_generated_cap + 1) {
                    Log("fg: multiplier x%u asked for: this launch has room for x%u at most (the experimental "
                        "setting was off when it started)", multiplier, g_generated_cap + 1);
                    multiplier = g_generated_cap + 1;
                }
                // What is asked for is the limit. A multiplier that is held goes there at once; one
                // that follows the game's rate only comes down to it, and goes up by itself when
                // the rate asks for that (a jump to the limit would hold the game for the seconds
                // it takes to come back).
                const uint32_t asked = multiplier - 1;
                const uint32_t next = FollowsRate() ? std::min(fg.generated, asked) : asked;
                if (asked != fg.max_generated || next != fg.generated) {
                    fg.max_generated = asked;
                    if (next != fg.generated) {
                        fg.generated = next;
                        GateMultiplierChanged();
                    }
                    Log("fg: multiplier x%u asked for (%s), now x%u", multiplier,
                        FollowsRate() ? "the limit" : "held", fg.too_fast ? 1 : fg.generated + 1);
                }
            }
            std::fclose(file);
            unlink(path);
        }
        if (ControlFile("EVENTS", true) || (fg.events_wanted && fg.event_files < 8)) {
            WriteEvents(fg);
        }
        fg.events_wanted = false;
    }
    if (fg.last_entry > 0.0 && now - fg.last_entry < 0.5) {
        const double interval = now - fg.last_entry;
        fg.game_ms.push_back(static_cast<float>(interval * 1000.0));
        fg.period = fg.period > 0.0 ? fg.period * 0.9 + interval * 0.1 : interval;
        fg.interval_ring[fg.interval_count++ % 128] = static_cast<float>(interval);
    }
    fg.last_entry = now;
    // Above this base rate the display has no room for generated frames: they would only hold
    // the game back. (2560x1440 at 120 Hz: twice 60.)
    static const double max_base = EnvFloat("SC_FG_MAX_BASE", 62.0f);
    // The multiplier follows the frame rate: as many generated frames as the display has room for
    // (SC_FG_TARGET shown frames a second, the display's refresh rate), at most what was asked for.
    // A game launched with frame generation keeps it on (the user's rule), with one exception,
    // also theirs: when the game alone gets near the display's rate (above 92 % of it) generation
    // stops, and comes back below 80 %. At those rates a frame is held back only a few
    // milliseconds, so the switch itself is a small step. SC_FG_AUTO_OFF=0 forbids even that.
    // Every change shifts the delay of the picture and unsettles the pacing for a moment, so the
    // multiplier moves only on what lasts: the rate is the middle one of two seconds' frame times
    // (a hitch or a few slow frames are not a slower game), it is looked at once a second, and two
    // such seconds in a row have to ask for the same change.
    // Which way to round is a matter of taste (PARAMS fill): the fewest generated frames that
    // fill the display (1, the default: a game at 50 a second gets x3, is held at 40 and shows
    // 120, evenly), or the most that do not hold the game back (0: the same game gets x2 and
    // shows 100).
    static const bool auto_off = EnvFloat("SC_FG_AUTO_OFF", 1.0f) != 0.0f;
    constexpr double FILL_UP = 0.86, FILL_DOWN = 0.94;
    static const double target = EnvFloat("SC_FG_TARGET", 120.0f);
    static const double refresh = 1.0 / (target * 0.999); // a touch under the display's rate
    // (A probe needs the frame behind it kept back until the probe's copy is made: no probes
    // where both things that do that are switched off.)
    const bool probing = g_live_probe.load() != 0.0f && (g_live_first.load() != 0.0f || g_live_guard.load() != 0.0f);
    if (now - fg.window_start > 1.0) {
        // What the limiter did in the last second. A game that is being held shows the limiter's
        // rate, not what it could do; that follows from its own share of the frame time (the time
        // between frames less the wait) and from the GPU's time for a frame.
        uint32_t frames = 0, held = 0, gpu_frames = 0, gpu_late = 0;
        double slept = 0.0, gpu_seconds = 0.0;
        {
            std::lock_guard lock{g_limiter.mutex};
            frames = std::exchange(g_limiter.frames, 0u);
            held = std::exchange(g_limiter.held, 0u);
            gpu_frames = std::exchange(g_limiter.gpu_frames, 0u);
            gpu_late = std::exchange(g_limiter.gpu_late, 0u);
            slept = std::exchange(g_limiter.slept, 0.0);
            gpu_seconds = std::exchange(g_limiter.gpu_seconds, 0.0);
            fg.limit_gated += std::exchange(g_limiter.gated, 0u);
            fg.limit_own_frames += std::exchange(g_limiter.own_frames, 0u);
            fg.limit_own_seconds += std::exchange(g_limiter.own_seconds, 0.0);
        }
        {
            std::lock_guard lock{g_limiter.gate_mutex};
            fg.guard_waits += std::exchange(g_limiter.guard_waits, 0u);
            fg.guard_timeouts += std::exchange(g_limiter.guard_timeouts, 0u);
            fg.guard_seconds += std::exchange(g_limiter.guard_seconds, 0.0);
            fg.first_waits += std::exchange(g_limiter.first_waits, 0u);
            fg.first_ahead += std::exchange(g_limiter.first_ahead, 0u);
            fg.first_seconds += std::exchange(g_limiter.first_seconds, 0.0);
        }
        const bool limited = frames > 0 && held * 2 > frames;
        // Where frames are handed over the limiter is what holds the game; the present hook's own
        // limit is only a net there, and that it catches frames says nothing.
        const bool handed_over = fg.chain_frame.number && now - fg.chain_frame.time < 2.0;
        fg.binding = handed_over ? limited : fg.window_out > 0 && fg.window_held * 2 > fg.window_out;
        fg.free_bound = 0.0;
        if (limited && fg.period > 0.0) {
            const double own = fg.period - slept / frames;
            // The GPU's time is known when it was done with most frames before they were due;
            // otherwise the GPU is what holds the game, at about the rate it has now.
            const double gpu = gpu_frames * 2 > frames ? gpu_seconds / gpu_frames : fg.period;
            fg.free_bound = 1.0 / std::max({own, gpu, 0.002});
        }
        // That is a bound and no more: the thread that hands frames over is not the whole game
        // (in Star Citizen it needs 8 ms where the game needs 21, and going by it the multiplier
        // went down every few seconds, only to come back up). What the game could do is what
        // probes found (Limiter::probe_advance): the middle one of the last few.
        {
            std::lock_guard lock{g_limiter.mutex};
            if (g_limiter.probe_results != fg.probe_seen) {
                fg.probe_seen = g_limiter.probe_results;
                const double period = g_limiter.probe_result;
                fg.probes[fg.probe_count++ % 4] = {now, period};
                ++fg.probe_stat;
                fg.free_last = 1.0 / std::max(period, 0.002);
                // A game that does the rate asked about is asked again the next second, until
                // there is enough to go by. One that does not is asked less and less often (a
                // probe moves one frame in time, a little): every 16 s when it is far from that
                // rate, every 4 s when it is near.
                const bool does = fg.probe_wanted <= 0.0 || period <= fg.probe_wanted * 1.01;
                const bool far = period > fg.probe_wanted * 1.15;
                fg.probe_wait = does ? 1.0 : std::min(fg.probe_wait * 2.0, far ? 16.0 : 4.0);
                fg.probe_after = now + fg.probe_wait - 1.5; // (this is looked at once a second)
            }
        }
        fg.free_rate = 0.0;
        if (fg.binding && fg.free_bound > 0.0 && !probing) {
            fg.free_rate = fg.free_bound;
        } else if (fg.binding && fg.free_bound > 0.0) {
            // The last four, none of them old (a game that does the rate asked about is asked
            // every second, one that is near it every four at the most: with less than that
            // between them than this allows, a game near the rate was never found to do it),
            // and of those the second slowest: frame times differ, and a step down that has to
            // be taken back costs two changes.
            double periods[4];
            uint32_t n = 0;
            for (const Fg::Probe& probe : fg.probes) {
                if (probe.time > 0.0 && now - probe.time < 14.0) {
                    periods[n++] = probe.period;
                }
            }
            if (n == 4) {
                std::sort(periods, periods + 4);
                fg.free_rate = std::min(1.0 / std::max(periods[2], 0.002), fg.free_bound);
            }
        }
        fg.decide = true;
        fg.limit_frames += frames;
        fg.limit_held += held;
        fg.limit_gpu_frames += gpu_frames;
        fg.limit_gpu_late += gpu_late;
        fg.limit_slept += slept;
        fg.limit_gpu_seconds += gpu_seconds;
        fg.window_held = fg.window_out = 0;
        fg.window_start = now;
    }
    if (fg.decide && !FollowsRate()) {
        // Held (PARAMS adapt=0): the multiplier is what was asked for, whatever the game's rate,
        // and generation does not stop near the display's rate either. Where it was following
        // the rate until now, it goes back to what was asked for; what was measured belongs to
        // the multiplier that was.
        fg.decide = false;
        if (fg.too_fast || fg.generated != fg.max_generated) {
            Log("fg: multiplier x%u -> x%u, held there", fg.too_fast ? 1 : fg.generated + 1, fg.max_generated + 1);
            fg.multiplier_changed = now;
            fg.generated = fg.max_generated;
            fg.too_fast = false;
            GateMultiplierChanged();
            fg.ready_count = 0;
            fg.ready_seconds = 0.0;
            fg.binding = false;
            fg.window_held = fg.window_out = 0;
            fg.window_start = now;
            fg.votes_up = fg.votes_down = 0;
            fg.interval_count = 0;
            fg.free_rate = fg.free_last = 0.0;
            fg.probe_wait = 1.0;
            fg.probe_after = 0.0;
            for (Fg::Probe& probe : fg.probes) {
                probe = {};
            }
        }
    }
    if (fg.decide && max_base > 0.0) {
        fg.decide = false;
        const double median = MedianPeriod(fg, 2.0);
        const double measured = median > 0.0 ? 1.0 / median : 0.0;
        // A game the limiter holds shows the limiter's rate; what it could do is what probes found.
        const bool probed = fg.binding && fg.free_rate > measured;
        const double rate = probed ? fg.free_rate : measured;
        // The multiplier for a rate, from below and from above (between the two it stays as it
        // is). Not to hold the game back: the largest that shows no more than the display takes,
        // give or take 7 %. To fill the display: the smallest that shows 94 % of what it takes,
        // and one more only below 86 %.
        const bool fill = g_live_fill.load() != 0.0f;
        // A step down on what probes found is taken on trial. A probe asks about one frame, with
        // the GPU doing the work of the multiplier that was. One generated frame less and that
        // many more frames of the game's is another load, and a GPU near its limit does not get
        // there (seen at the game's size: 58 a second by probes at x3, 50 at x2, which showed
        // 100 frames where x3 showed 117; and 50 is between "one more" and "one less", so there
        // it stayed for 20 s). For its first seconds the game has to do the rate the step was
        // taken for, less 3 %, or the step is taken back, and none is tried again for a while.
        const bool trial = !fg.too_fast && fg.down_time > 0.0 && !fg.down_failed && now - fg.down_time < 8.0 &&
                           fg.generated + 1 == fg.down_to;
        const auto low_for = [&](double r) {
            if (trial) {
                return static_cast<uint32_t>(fill ? std::ceil(0.97 * FILL_DOWN * target / r) : 0.97 * target / (r * 0.93));
            }
            return static_cast<uint32_t>(fill ? std::ceil(FILL_UP * target / r) : target / (r * 1.07));
        };
        const auto high_for = [&](double r) {
            return static_cast<uint32_t>(fill ? std::ceil(FILL_DOWN * target / r) : target / (r * 0.93));
        };
        const bool may_stop = auto_off && now >= fg.auto_off_after;
        if (fg.down_time > 0.0 && !fg.down_failed && now - fg.down_time > 60.0) {
            fg.down_block = 20.0; // the last step down on probes has held
        }
        if (measured > 0.0) {
            const uint32_t current = fg.too_fast ? 1 : fg.generated + 1;
            // Held back without knowing what the game could do (no limiter: the present hook's own
            // limit): above x2, try one step less (more real frames for the same number shown).
            uint32_t low = low_for(rate);
            uint32_t high = fg.binding && fg.free_bound <= 0.0 && current > 2 ? current - 1 : high_for(rate);
            // The generated frames also have to fit into the game's frame time: they are ready after
            // ready_seconds and then need a refresh each. Where they do not fit, the game is being
            // held for them; one generated frame less instead, and no step up again for a while.
            // (Its frame time with this multiplier: what the display's rate leaves it, or its own
            // where probes have found that to be longer. Not the frame time it has: where it is
            // held for the generated frames, that is theirs. To fill the display some waiting
            // for them is accepted, 15 % of a frame; not to hold the game back, 7 %.)
            const double needed = fg.ready_seconds + fg.tail_seconds + (current - 1) * refresh + 0.0005;
            const double room = probed ? std::max(1.0 / rate, current * refresh) : current * refresh;
            const bool no_room = !fg.too_fast && current > 2 && high >= current &&
                                 fg.ready_seconds > 0.0 && fg.binding && needed > room * (fill ? 1.15 : 1.0 / 0.93);
            if (no_room) {
                high = std::min(high, current - 1);
            } else if (now < fg.room_until) {
                high = std::min(high, std::max(current, 2u));
            }
            low = std::min(low, high);
            // Whether to generate at all is one rule's alone, however the multiplier is rounded:
            // not above 92 % of the display's rate, and again below 80 %.
            const uint32_t least = may_stop && rate > target * (fg.too_fast ? 0.80 : 0.92) ? 1u : 2u;
            const uint32_t wanted =
                least == 1 ? 1u
                           : std::clamp(std::clamp(current, std::max(low, 2u), std::max(high, 2u)), 2u, fg.max_generated + 1);
            fg.votes_up = wanted > current ? fg.votes_up + 1 : 0;
            fg.votes_down = wanted < current ? fg.votes_down + 1 : 0;
            // (A step down on what probes found that had to be taken back: none for a while.)
            const bool on_probes = wanted < current && probed && !no_room;
            if (fg.votes_up >= 2 || (fg.votes_down >= 2 && !(on_probes && now < fg.down_until))) {
                fg.multiplier_changed = now;
                const char* const how = !probed ? "" : probing ? " (could be, by probes; held)" : " (could be; held)";
                if (no_room && wanted < current) {
                    Log("fg: base rate %.0f/s%s, x%u needs %.1f ms a frame (generated frames ready after %.1f): no room, "
                        "multiplier x%u -> x%u (no step up for %.0f s)",
                        rate, how, current, needed * 1000.0, fg.ready_seconds * 1000.0, current, wanted, fg.room_block);
                    fg.room_until = now + fg.room_block;
                    fg.room_block = std::min(fg.room_block * 2.0, 600.0);
                } else {
                    Log("fg: base rate %.0f/s%s, multiplier x%u -> x%u", rate, how, current, wanted);
                }
                if (wanted > current && fg.down_time > 0.0 && now - fg.down_time < 8.0 && !fg.down_failed) {
                    Log("fg: the step down %.0f s ago did not hold: none on what probes find for %.0f s",
                        now - fg.down_time, fg.down_block);
                    fg.down_failed = true;
                    fg.down_until = now + fg.down_block;
                    fg.down_block = std::min(fg.down_block * 2.0, 300.0);
                } else if (on_probes) {
                    fg.down_time = now;
                    fg.down_failed = false;
                    fg.down_to = wanted;
                }
                GateMultiplierChanged();
                fg.ready_count = 0; // the time they take changes with their number
                fg.ready_seconds = 0.0;
                if (fg.too_fast && now - fg.off_since < 5.0) {
                    // It did not hold: the estimate was too high. No new attempt for a minute.
                    fg.auto_off_after = now + 60.0;
                }
                if (wanted == 1) {
                    fg.off_since = now;
                }
                fg.too_fast = wanted == 1;
                fg.binding = false;
                fg.window_held = fg.window_out = 0;
                fg.window_start = now;
                if (wanted > 1) {
                    fg.generated = wanted - 1;
                }
                // What was measured belongs to the multiplier that was.
                fg.votes_up = fg.votes_down = 0;
                fg.interval_count = 0;
                fg.free_rate = fg.free_last = 0.0;
                fg.probe_wait = 1.0;
                fg.probe_after = 0.0;
                for (Fg::Probe& probe : fg.probes) {
                    probe = {};
                }
            }
        }
        // Would one generated frame less do, or none? The game is held, so its rate does not say.
        // One frame is let on early, as early as it would come at the rate from which that would
        // be the choice (Limiter::probe_advance), and what the game makes of it is the answer.
        // Only where the handing-over thread's own time and the GPU's leave room for that rate.
        const uint32_t multiplier = fg.too_fast ? 1 : fg.generated + 1;
        const bool handed_over = fg.chain_frame.number && now - fg.chain_frame.time < 2.0;
        if (handed_over && fg.binding && fg.free_bound > 0.0 && median > 0.0 && multiplier > 1 && now >= fg.down_until &&
            now >= fg.probe_after && probing) {
            const double threshold = multiplier > 2 ? (fill ? FILL_DOWN * target / (multiplier - 1) : target / (0.93 * multiplier))
                                     : may_stop    ? target * 0.92
                                                   : 0.0;
            const double wanted_period = threshold > 0.0 ? 1.0 / (threshold * 1.03) : 0.0;
            if (wanted_period > 0.0 && fg.free_bound > threshold * 1.03 && median - wanted_period > 0.0003) {
                std::lock_guard lock{g_limiter.mutex};
                g_limiter.probe_advance = std::min(median - wanted_period, 0.5 * median);
                fg.probe_wanted = wanted_period;
            }
        }
    }
    // The limiter (on the game's thread, see LimitFrame) holds the game to what the display has
    // room for: one frame of its own per `multiplier` shown, a touch under the display's rate.
    static const bool cap = EnvFloat("SC_FG_CAP", 1.0f) != 0.0f;
    {
        const bool generating = !fg.too_fast && fg.last_present > 0.0 && now - fg.last_present < 0.25;
        const double shown_per_frame = generating ? fg.generated + 1 : 1;
        // What the present hook needs for a frame: the time until the generated frames are ready,
        // then a refresh for each of them (closer together they cannot be shown). A game faster
        // than that is held to it; its frames would otherwise queue up behind the presents.
        // With the gate, which lets a frame on when the hook is about to return, that is the
        // limit taken from the hook itself. The timer then stays just under what a hook can do
        // at its quickest (see hold(): 0.97 of a refresh apart), out of the gate's way, for where
        // the gate does not hold: a GPU that is as slow as the hooks, with the game's frames
        // queued on it, gets to this layer's work sooner when the game is not let run ahead.
        const bool gate = g_live_gate.load() != 0.0f;
        const double hook_least = generating && fg.ready_seconds > 0.0
                                      ? fg.ready_seconds + fg.generated * refresh * (gate ? 0.97 : 1.0) + fg.tail_seconds +
                                            (gate ? 0.0 : 0.0005)
                                      : 0.0;
        const double wanted = cap && !fg.no_cap && !fg.off && !fg.failed && fg.modified
                                    ? std::max(shown_per_frame * refresh, hook_least)
                                    : 0.0;
        g_limiter.interval = wanted > 0.0 && wanted < 1.0 ? wanted : 0.0; // (and no number that is none)
        g_limiter.published = now;
    }
    const bool fallback = EnvFloat("SC_FG_FALLBACK", 0.0f) != 0.0f;
    // The captures of the frame being presented.
    const Link link = LinkFrame(d, fg, now);
    if (link.chained && !link.duplicate) {
        // For the limiter's gate: this frame's hook has begun, and how long the last one like it took.
        const bool generating = !fg.too_fast && !fg.off && fg.have_previous && fg.last_present > 0.0 &&
                                now - fg.last_present < 0.25;
        {
            // How long before a hook's return the next frame is let on (Limiter::advance). This
            // hook began `idle` after the last one returned (less the way from one to the next).
            Limiter& l = g_limiter;
            std::lock_guard lock{l.gate_mutex};
            const bool known = !l.in_hook && l.hook_free > 0.0 && now - l.hook_free < 0.25;
            const double idle = known ? now - l.hook_free - 0.0003 : 0.0;
            if (idle <= 0.00025) {
                ++l.waiting; // its frame was there, waiting or just in time
            } else if (!link.frame.gated) {
                l.waiting = 0; // the game is the slower one
            }
            // What this frame needed: the game began it when the frame before was let on,
            // `lead_before` ahead of the hook's return then; it had to be here, done, by the
            // return of the hook after that one.
            if (known && l.lead_before >= 0.0 && idle < 0.1) {
                const bool late = idle > 0.00025; // the hook had to wait for it
                const bool seen = !late && link.frame.gpu_done > 0.0 && link.frame.gpu_done < l.hook_free;
                // In time, and no more known. At least what the game's own time for it leaves
                // of a hook's; where the gate let the frame before on, a little less than that
                // gave it, to find out. (A frame the timer let on early says nothing about what
                // it needed, and must not be taken to have needed all of that.)
                const double least = l.return_before > 0.0
                                         ? (link.frame.own_ms + link.frame.guard_ms) * 0.001 - (l.hook_free - l.return_before)
                                         : 0.0;
                const double less = l.lead_before - std::max(0.0002, 0.02 * l.lead_before);
                const double need = late     ? l.lead_before + idle
                                    : seen   ? l.lead_before - (l.hook_free - link.frame.gpu_done)
                                    : l.lead_gated ? std::max(least, less)
                                                   : std::min(least, l.lead_before);
                // More lead than this no frame can use. Its submit waits for the generated frames
                // of the hook before it (WaitForGenerated): let on earlier than its own time, up
                // to that submit, less the time those take, the game only waits there instead.
                // And lead is delay, the frame is that much older when it is shown; taken from
                // frames that were late because the game was slow, or the GPU, it grew to a
                // frame's time and stayed.
                //
                // That is for a game that hands a frame over when it has submitted it. One that
                // does it the other way round (the time between its submit and its coming to the
                // limiter is a good part of its own time for a frame) is let on with the frame's
                // GPU work still to come: what that takes is what it needs, and its own time
                // says nothing about it.
                //
                // Nor does that wait take its time where the GPU is what the frames wait for
                // (Limiter::gpu_paced): there a frame's submit goes on when the GPU is done with
                // the frame before, and all of the game's own time for it can be of use. Frames
                // let on with less came after the GPU had run out of work whenever the game
                // took longer over one than over most (measured, for a game whose own time
                // differs much from frame to frame: the GPU without work for 1.3 ms of every
                // frame instead of 0.2, and 1 to 3 % fewer frames).
                const bool first = g_live_first.load() != 0.0f && now >= g_first_off_until.load();
                const bool paced = l.gpu_paced.load() >= 4;
                const double own = link.frame.own_ms * 0.001, lead = l.lead.load();
                const double usable = own > 0.0 && lead < 0.5 * own
                                          ? std::max(own - (first ? (paced ? 0.0 : l.hook_ready) + lead : 0.0), 0.0)
                                          : 0.05;
                l.need_ring[l.need_count++ % 32] = static_cast<float>(std::clamp(std::min(need, usable), -0.05, 0.05));
                // The most any of them needed, but for the odd one (a hitch is not made up for
                // by letting every frame on early).
                float sorted[32];
                const uint32_t n = std::min(l.need_count, 32u);
                std::copy_n(l.need_ring, n, sorted);
                std::sort(sorted, sorted + n, std::greater<float>());
                l.advance = std::clamp(static_cast<double>(sorted[n >= 8 ? 2 : 0]), Limiter::LEAD, 0.05);
            }
            l.return_before = known ? l.hook_free : 0.0;
            l.lead_before = known && l.hook_free > link.frame.time ? l.hook_free - link.frame.time : -1.0;
            l.lead_gated = link.frame.gated;
        }
        GateEnter(link.frame.number, now + (generating ? fg.hook_last : 0.0), generating);
    }
    if (fg.events.empty()) {
        fg.events.resize(EVENT_COUNT);
    }
    Event& event = fg.events[fg.event_count % EVENT_COUNT];
    event = {};
    event.time = now;
    event.chain = link.frame.number;
    event.hud = link.hud;
    event.input = link.input;
    event.newest_hud = link.newest_hud;
    event.newest_input = link.newest_input;
    event.multiplier = fg.too_fast ? 1 : fg.generated + 1;
    event.shift = fg.chain_shift;
    event.flags = (link.chained ? Event::CHAINED : 0u) | (link.duplicate ? Event::DUPLICATE : 0u);
    fg.seen_input = link.submitted_input;
    fg.seen_hud = link.submitted_hud;
    fg.ahead_sum += link.hud ? static_cast<double>(link.newest_hud - std::min(link.hud, link.newest_hud))
                             : static_cast<double>(link.newest_input - std::min(link.input, link.newest_input));
    if (link.duplicate) {
        ++fg.duplicate_frames;
        return false; // the same frame once more: it goes out as it is, and nothing is forgotten
    }
    if (link.chained) {
        const ChainFrame& last = fg.chain_frame;
        if (last.number) {
            event.handovers = static_cast<uint32_t>(link.frame.number - last.number);
            event.hud_captures = static_cast<uint32_t>(link.frame.hud - last.hud);
            event.evaluations = static_cast<uint32_t>(link.frame.input - last.input);
            event.passes = static_cast<uint32_t>(link.frame.passes - last.passes);
            event.frame_copies = static_cast<uint32_t>(link.frame.frame_copies - last.frame_copies);
            fg.multi_frames += event.hud_captures > 1 || event.evaluations > 1;
        }
        event.handover_ms = static_cast<float>((now - link.frame.time) * 1000.0);
        // The frame before this one: when the GPU was done with its hand-over's copy (by now it is).
        if (fg.done_frame && g_done_watch->frame[fg.done_frame % 64].load() == fg.done_frame) {
            const double done = g_done_watch->time[fg.done_frame % 64].load();
            fg.done_after_handover_ms.push_back(static_cast<float>((done - fg.done_handover) * 1000.0));
            fg.done_before_hook_ms.push_back(static_cast<float>((fg.done_hook - done) * 1000.0));
        }
        fg.done_frame = g_live_timing.load() != 0.0f && !link.duplicate ? link.frame.number : 0;
        fg.done_handover = link.frame.time;
        fg.done_hook = now;
        const double waited_for_hook = now - link.frame.time;
        // (From a small beginning: the very first frames wait for much that has nothing to do with it,
        // and a hook that takes that for the usual way does not hurry when frames are waiting.)
        fg.handover_least =
            std::min(waited_for_hook, (fg.handover_least > 0.0 ? fg.handover_least : 0.0005) * 1.02 + 0.0001);
        g_limiter.queued = std::max(waited_for_hook - fg.handover_least, 0.0);
        event.slept_ms = link.frame.slept_ms;
        event.gpu_ms = link.frame.gpu_ms;
        event.own_ms = link.frame.own_ms;
        event.guard_ms = link.frame.guard_ms;
        event.flags |= (link.frame.gated ? Event::GATED : 0u) | (link.frame.late ? Event::LATE : 0u) |
                       (link.frame.lost ? Event::LOST : 0u);

        ++fg.chained_frames;
        fg.chain_frame = link.frame;
    } else if (link.alive) {
        ++fg.unlinked_frames;
    }
    const uint64_t serial = link.input;
    fg.ahead_frames += link.recorded_input != link.newest_input;
    const bool inputs_new = serial != fg.input_serial;
    // A present or two without a new copy (the submit came a moment late) reuses the last one: the
    // vectors are a frame old, which shows far less than a frame without generation. Longer than
    // that and DLSS is not running (menus, loading): frames pass through.
    fg.stale = inputs_new ? 0 : fg.stale + 1;
    const bool reuse = !inputs_new && fg.stale <= 2 && fg.input_serial && fg.have_previous;
    fg.stale_frames += reuse;
    if (fg.failed || !fg.modified || fg.off || fg.too_fast || (!inputs_new && !reuse && !fallback)) {
        fg.have_previous = false;
        return false;
    }
    if (!fg.ready && !Prepare(d, fg, family)) {
        Log("fg: could not be set up; frames pass through");
        fg.failed = true;
        return false;
    }
    if (family != fg.family) {
        Log("fg: the game presents on another queue family (%u); frames pass through", family);
        fg.failed = true;
        return false;
    }
    fg.evaluations += serial > fg.input_serial ? serial - fg.input_serial : 0;
    fg.input_serial = serial;
    const uint32_t index = info.pImageIndices[0];
    const uint32_t w = fg.extent.width, h = fg.extent.height;

    Stage(2);
    const int set_number = static_cast<int>(fg.set_index);
    Fg::Set& set = fg.sets[fg.set_index];
    fg.set_index = (fg.set_index + 1) % COMMAND_SETS;
    if (set.pending) {
        d.WaitForFences(d.handle, 1, &set.fence, VK_TRUE, UINT64_MAX);
        d.ResetFences(d.handle, 1, &set.fence);
        set.pending = false;
    }
    Stage(3);
    if (fg.clock.written[set_number]) {
        TakeGpuClock(d, fg, set_number);
    }
    if (fg.markers.counted[set_number]) { // what this set's passes for moving HUD content counted
        const auto* counts = static_cast<const uint32_t*>(fg.markers.counts[set_number].data);
        ++fg.markers.frames;
        fg.markers.seen += counts[0];
        fg.markers.found += counts[1];
        fg.markers.pieces += counts[2];
        fg.markers.pictures += counts[3];
        fg.markers.over += static_cast<float>(counts[0]) > MarkerCap(fg);
        fg.markers.counted[set_number] = false;
    }
    if (fg.measure_set == set_number) {
        // the probes this set read back a few frames ago are long finished: no waiting for them
        const double before = Now();
        CheckSync(fg);
        MeasureCurve(fg);
        fg.measure_ms = std::max(fg.measure_ms, static_cast<float>((Now() - before) * 1000.0));
        fg.measure_set = -1;
    }

    // Which vectors and depth: the copies made when DLSS ran, or blanks.
    Image* vectors = &fg.zero_vectors;
    Image* depth = &fg.zero_depth;
    uint32_t render_w = w / 2, render_h = h / 2;
    if (inputs_new || reuse) {
        const Picked pick = PickInputs(d, true, serial);
        if (pick.vectors && pick.width <= w && pick.height <= h) {
            vectors = pick.vectors;
            depth = pick.depth;
            render_w = pick.width;
            render_h = pick.height;
            fg.age_ms.push_back(static_cast<float>(pick.age * 1000.0));
        }
    }
    ++(vectors == &fg.zero_vectors ? fg.blank_inputs : fg.game_inputs);
    uint32_t hud_w = 0, hud_h = 0;
    Image* hud_scene = nullptr;
    Image* hud_neighbours[3]{};
    // A chained frame that came without a HUD capture of its own would be handed the last frame's
    // again: a scene copy of another frame, which must not be used. No generated frame this time,
    // and the interpolation starts over with the next frame.
    const bool hud_gap = link.chained && link.hud && link.hud == fg.hud_used && !link.duplicate;
    const bool hud_missing =
        link.chained && link.hud && link.hud == fg.hud_used && fg.layer_mode && g_live_nohud.load() == 0.0f;
    // Not every frame comes with a capture. In play a few do not (one in two hundred, in short
    // runs): for those the scene is worked out from the frame and the last capture (RecordUnmix),
    // and the HUD stays apart from what is interpolated. In the game's menu about every second one
    // comes without (the HUD is drawn, the frame is not set aside): where that many are missing,
    // or a run goes on for long, the layer is left alone and the frames are interpolated as they
    // are presented; it is taken up again once every frame has had its capture for a while.
    // (With gapfill=0 a few are enough for that, and starting over at each of the others leaves
    // fewer generated frames.)
    const bool gap_fill = g_live_gapfill.load() != 0.0f && !fg.unmixed_failed;
    fg.capture_gaps = (fg.capture_gaps << 1) | (hud_gap ? 1u : 0u);
    fg.capture_run = hud_gap ? 0 : fg.capture_run + 1;
    if (!fg.capture_hold && __builtin_popcount(fg.capture_gaps) >= (gap_fill ? 12 : 3) && g_live_nohud.load() == 0.0f) {
        fg.capture_hold = true;
        Log("fg: HUD captures come with gaps (%d of the last 32 frames without one): frames are interpolated as "
            "presented until every frame has one again",
            __builtin_popcount(fg.capture_gaps));
    } else if (fg.capture_hold && fg.capture_run >= (gap_fill ? 16u : 45u)) {
        fg.capture_hold = false;
        Log("fg: every frame has its HUD capture again");
    }
    Image* scene_before = nullptr;
    Image* layer_before = nullptr;
    uint64_t hud_capture = 0; // the one handed out (not always the one asked for)
    Image* hud_layer = PickHud(d, hud_w, hud_h, &hud_scene, link.hud, hud_neighbours, &scene_before, &layer_before,
                               &hud_capture, hud_missing && gap_fill ? 1.0 : 0.5);
    if (!hud_layer) {
        hud_capture = 0;
    }
    fg.hud_used = link.hud;
    static const bool test_layer = EnvFloat("SC_FG_TEST_LAYER", 0.0f) != 0.0f;
    if (test_layer && (fg.test_scene.image || (CreateOwnImage(d, fg.test_scene, SCENE_FORMAT, w, h) &&
                                               CreateOwnImage(d, fg.test_layer, HUD_FORMAT, w, h)))) {
        hud_scene = &fg.test_scene;
        hud_layer = &fg.test_layer;
        hud_w = w;
        hud_h = h;
    }
    if (hud_layer && (hud_w != w || hud_h != h || !hud_scene || !fg.hud.ok || fg.layer_off || fg.hud_off ||
                      fg.capture_hold)) {
        hud_layer = nullptr;
    }
    // The tone curve is measured from a read-back now and then; until it is known the layer is not used.
    Image* const curve_layer = hud_layer;
    Image* const curve_scene = hud_scene;
    bool measure = false;
    if (hud_layer) {
        if (!fg.curve_image.image) {
            if (!CreateOwnImage(d, fg.curve_image, SCENE_FORMAT, 256, 1) ||
                !(fg.curve_view = MakeView(d, fg.curve_image.image, SCENE_FORMAT)) ||
                !CreateBufferWithMemory(d, 256 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, fg.curve_staging.buffer,
                                        fg.curve_staging.memory, &fg.curve_staging.data) ||
                !CreateProbes(d, fg, w, h)) {
                fg.layer_off = true;
                Log("fg: no tone curve image, HUD layer not used");
            }
            PrepareUnmix(d, fg); // (here, not in the middle of play at the first frame that needs them)
        }
        measure = !fg.layer_off && !hud_missing && fg.measure_set < 0 && now - fg.curve_time > (fg.curve_ok ? 1.0 : 0.5);
        if (!fg.curve_ok || fg.layer_off) {
            hud_layer = nullptr;
        }
    }
    if ((hud_layer != nullptr) != fg.layer_mode) {
        fg.layer_mode = hud_layer != nullptr;
        fg.have_previous = false; // the interpolation source changes: start over
        Log("fg: HUD layer mode %s", fg.layer_mode ? "on (scene and HUD separated exactly)" : "off");
    }
    if (hud_layer) {
        ++fg.layer_frames;
    }
    fg.nohud_frames += hud_missing;
    // A frame without a capture, between frames in layer mode: its scene is worked out, below.
    const bool unmix = hud_missing && gap_fill && hud_layer && hud_layer != &fg.test_layer && hud_scene && fg.hud.ok &&
                       fg.have_previous && now - fg.last_present <= 0.5 && PrepareUnmix(d, fg);
    const bool reset = !fg.have_previous || now - fg.last_present > 0.5 || (hud_missing && !unmix);
    // A frame without generated ones in the middle of a run of them: the real frame still goes
    // out when it would have, so that the real frames keep their even spacing.
    const bool keep_cadence = reset && fg.last_present > 0.0 && now - fg.last_present < 0.1;
    event.flags |= (hud_layer ? Event::LAYER : 0u) | (reset ? Event::RESET : 0u) | (reuse ? Event::REUSED : 0u) |
                   (hud_missing ? Event::NO_HUD : 0u) | (measure ? Event::MEASURED : 0u) | (unmix ? Event::UNMIXED : 0u);
    const float frame_ms = fg.last_present > 0.0 ? static_cast<float>((now - fg.last_present) * 1000.0) : 33.3f;
    const bool dump = fg.dump_next && !reset;
    // HUD content that moves over the screen is looked for when the capture before this one is
    // still there and is the one that went with the frame kept in `previous`.
    const bool moving = hud_layer && hud_layer != &fg.test_layer && !reset && g_live_markers.load() != 0.0f &&
                        g_layer_parameters[0] == 0.0f && !fg.markers.failed && layer_before && layer_before->image &&
                        scene_before && scene_before->image && hud_capture &&
                        fg.markers.previous_hud + 1 == hud_capture;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmd = set.main;
    d.ResetCommandBuffer(cmd, 0);
    d.BeginCommandBuffer(cmd, &begin);
    // This layer's GPU time by step (timing=1; frames interpolated in one command buffer only).
    const bool clocked = g_live_timing.load() != 0.0f && !reset && PrepareGpuClock(d, fg);
    const auto mark = [&](uint32_t which) {
        if (clocked) {
            d.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg.clock.pool,
                                set_number * Fg::GpuClock::MARKS + which);
        }
    };
    if (clocked) {
        d.CmdResetQueryPool(cmd, fg.clock.pool, set_number * Fg::GpuClock::MARKS, Fg::GpuClock::MARKS);
    }
    mark(0);
    ToGeneral(d, cmd, fg.backbuffer);
    for (uint32_t i = 0; i < g_generated_cap; ++i) {
        ToGeneral(d, cmd, fg.outputs[i]);
    }
    if (!fg.zero_cleared) {
        ToGeneral(d, cmd, fg.zero_vectors);
        ToGeneral(d, cmd, fg.zero_depth);
        const VkClearColorValue zero{};
        if (fg.no_scene_change.image || CreateOwnImage(d, fg.no_scene_change, VK_FORMAT_R32_UINT, 3, 1)) {
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ToGeneral(d, cmd, fg.no_scene_change);
            d.CmdClearColorImage(cmd, fg.no_scene_change.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue test{};
        test.float32[0] = EnvFloat("SC_FG_TEST_MV", 0.0f); // tests only: a constant vector field
        d.CmdClearColorImage(cmd, fg.zero_vectors.image, VK_IMAGE_LAYOUT_GENERAL, &test, 1, &range);
        d.CmdClearColorImage(cmd, fg.zero_depth.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        fg.zero_cleared = true;
    }
    // The game's finished frame.
    ImageBarrier(d, cmd, fg.images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageCopy whole = WholeCopy(w, h);
    d.CmdCopyImage(cmd, fg.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fg.backbuffer.image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    ImageBarrier(d, cmd, fg.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (fg.curve_dirty && fg.curve_image.image) {
        ToGeneral(d, cmd, fg.curve_image);
        VkBufferImageCopy lut{};
        lut.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        lut.imageExtent = {256, 1, 1};
        d.CmdCopyBufferToImage(cmd, fg.curve_staging.buffer, fg.curve_image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &lut);
        fg.curve_dirty = false;
    }
    if (test_layer && curve_layer == &fg.test_layer) {
        // the frame itself as the scene, and a half-transparent layer over the left half
        ToGeneral(d, cmd, fg.test_scene);
        ToGeneral(d, cmd, fg.test_layer);
        d.CmdCopyImage(cmd, fg.backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, fg.test_scene.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &whole);
        VkClearColorValue half{}; // SC_FG_TEST_LAYER=2: nothing but HUD, the curve can then not be measured
        half.float32[3] = EnvFloat("SC_FG_TEST_LAYER", 0.0f) > 1.5f ? 0.5f : 0.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        d.CmdClearColorImage(cmd, fg.test_layer.image, VK_IMAGE_LAYOUT_GENERAL, &half, 1, &range);
    }

    // The frame time the frames are spread over: the game's, as measured. But a frame the limiter
    // held back came when this hook's predecessor let it (the gate): there the measured frame
    // time is the hooks' own doing, and spreading the frames over it would keep it as long as it
    // happens to be. Such a frame gets what the display has room for.
    const bool held = link.chained && link.frame.slept_ms > 1.0f && g_live_gate.load() != 0.0f && cap && !fg.no_cap;
    const double period = held ? (fg.generated + 1) * refresh : fg.period < 0.1 ? fg.period : 0.1;
    const double step = fg.period > 0.0 ? period / (fg.generated + 1) : 0.0;
    bool hud_ran = false;
    Image* const game_vectors = vectors;
    bool checked = false;
    bool unmixed = false;
    if (fg.hud.ok) {
        ToGeneral(d, cmd, fg.previous);
        if (unmix) {
            // This frame's scene takes the place of the capture's, and the scene before it is what
            // the vectors point into. A run of such frames goes on from the scene worked out last.
            Image& target = fg.unmixed[fg.unmixed_index ^ 1u];
            Image* const before = fg.unmixed_last ? &fg.unmixed[fg.unmixed_index] : hud_scene;
            unmixed = RecordUnmix(d, fg, set, cmd, *before, *hud_layer, target);
            if (unmixed) {
                scene_before = before;
                hud_scene = &target;
                fg.unmixed_index ^= 1u;
                ++fg.unmixed_frames;
            }
        } else if (fg.unmixed_last) {
            scene_before = &fg.unmixed[fg.unmixed_index]; // the frame before had no capture to go by
        }
        // The game's vectors against the picture: with the HUD layer the scene without the HUD
        // of this capture and of the one before, otherwise (tests, or asked for) the shown frames.
        const float check = g_live_vectors.load();
        if (check != 0.0f && !reset && !fg.check_failed && vectors != &fg.zero_vectors) {
            const bool separated = hud_layer && hud_layer != &fg.test_layer;
            const Image* const picture = separated ? hud_scene : &fg.backbuffer;
            const Image* const picture_before = separated ? scene_before : &fg.previous;
            if (picture_before && picture_before->image && (hud_layer || check >= 2.0f)) {
                checked = RecordVectorCheck(d, fg, set, cmd, vectors, *depth, *picture, *picture_before, render_w,
                                            render_h);
                if (!checked) {
                    fg.check_failed = true;
                    Log("fg: the vector check could not be set up; the game's vectors are used as they are");
                }
            }
        }
        event.flags |= checked ? Event::CHECKED : 0u;
        static const bool test_vectors = EnvFloat("SC_FG_TEST_MV", 0.0f) != 0.0f;
        if (!hud_layer && !reset && !fg.hud_off && (game_vectors != &fg.zero_vectors || test_vectors)) {
            hud_ran = RecordHud(d, fg, set, cmd, vectors, render_w, render_h);
        }
    }

    g_ffx_device = &d;
    const FfxCommandList command_list = ffxGetCommandListVK(cmd);
    FfxFrameInterpolationPrepareDescription prepare{};
    prepare.commandList = command_list;
    prepare.renderSize = {render_w, render_h};
    prepare.jitterOffset = {0.0f, 0.0f};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = frame_ms;
    prepare.cameraNear = EnvFloat("SC_FG_NEAR", 0.1f);
    prepare.cameraFar = 3.402823466e+38f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.cameraFovAngleVertical = EnvFloat("SC_FG_FOV", 1.0f);
    prepare.depth = External(*depth, L"Depth");
    prepare.motionVectors = External(*vectors, L"MotionVectors");
    prepare.frameID = fg.frame_id;
    prepare.dilatedDepth = fg.backend.fpGetResource(&fg.backend, fg.dilated_depth);
    prepare.dilatedMotionVectors = fg.backend.fpGetResource(&fg.backend, fg.dilated_motion);
    prepare.reconstructedPrevDepth = fg.backend.fpGetResource(&fg.backend, fg.previous_depth);

    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = command_list;
    flow.color = External(hud_layer ? *hud_scene : fg.backbuffer, L"Backbuffer");
    flow.opticalFlowVector = fg.backend.fpGetResource(&fg.backend, fg.of_vector);
    flow.opticalFlowSCD = fg.backend.fpGetResource(&fg.backend, fg.of_scd);
    flow.reset = reset;
    flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    flow.minMaxLuminance = {0.0f, 1.0f};

    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = command_list;
    dispatch.displaySize = {w, h};
    dispatch.renderSize = {render_w, render_h};
    dispatch.currentBackBuffer = External(hud_layer ? *hud_scene : fg.backbuffer, L"Backbuffer");
    dispatch.output = External(fg.outputs[0], L"Output");
    dispatch.generatedFrameCount = fg.generated;
    for (uint32_t i = 0; i < fg.generated; ++i) {
        dispatch.generatedFrames[i] = External(fg.outputs[i], L"Output");
        dispatch.generatedFrameTimes[i] = static_cast<float>(i + 1) / static_cast<float>(fg.generated + 1);
    }
    dispatch.interpolationRect = {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
    dispatch.opticalFlowVector = flow.opticalFlowVector;
    dispatch.opticalFlowSceneChangeDetection = flow.opticalFlowSCD;
    if (!fg.scd_on && fg.no_scene_change.image && !reset) {
        dispatch.opticalFlowSceneChangeDetection = External(fg.no_scene_change, L"NoSceneChange");
    }
    dispatch.opticalFlowBlockSize = 8;
    dispatch.opticalFlowScale = {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h)};
    dispatch.cameraNear = prepare.cameraNear;
    dispatch.cameraFar = prepare.cameraFar;
    dispatch.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.frameTimeDelta = frame_ms;
    dispatch.reset = reset;
    dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dispatch.minMaxLuminance[0] = 0.0f;
    dispatch.minMaxLuminance[1] = 1.0f;
    dispatch.frameID = fg.frame_id;
    dispatch.dilatedDepth = prepare.dilatedDepth;
    dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
    dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth;

    bool ffx_ok = true;
    mark(1);
    ffx_ok = CheckFfx(ffxFrameInterpolationPrepare(&fg.interpolation, &prepare), "prepare");
    mark(2);
    ffx_ok = ffx_ok && CheckFfx(ffxOpticalflowContextDispatch(&fg.optical_flow, &flow), "optical flow");
    mark(3);
    ffx_ok = ffx_ok && CheckFfx(ffxFrameInterpolationDispatch(&fg.interpolation, &dispatch), "interpolation");
    mark(4);
    if (hud_ran && fg.hud.ok && ffx_ok) {
        RecordHudComposite(d, fg, set, cmd);
    }
    bool markers_ran = false;
    if (hud_layer && ffx_ok) {
        markers_ran = moving && RecordMarkers(d, fg, set, set_number, cmd, *hud_layer, *layer_before);
        RecordLayerComposite(d, fg, set, cmd, *hud_scene, *hud_layer, markers_ran ? layer_before : nullptr, scene_before);
    }
    mark(5);
    if (measure) {
        VkBufferImageCopy full{};
        full.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        full.imageExtent = {w, h, 1};
        full.imageExtent = {fg.probe_width, fg.probe_height, 1};
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(fg.probe_width * 8), static_cast<int32_t>(fg.probe_height * 8), 1};
        blit.dstOffsets[1] = {static_cast<int32_t>(fg.probe_width), static_cast<int32_t>(fg.probe_height), 1};
        fg.probe_valid[0] = hud_neighbours[0] && hud_neighbours[0]->image;
        fg.probe_valid[1] = hud_neighbours[1] && hud_neighbours[1]->image;
        fg.probe_valid[2] = hud_neighbours[2] && hud_neighbours[2]->image;
        const VkImage sources[6] = {curve_scene->image, fg.backbuffer.image, curve_layer->image,
                                    fg.probe_valid[0] ? hud_neighbours[0]->image : curve_scene->image,
                                    fg.probe_valid[1] ? hud_neighbours[1]->image : curve_scene->image,
                                    fg.probe_valid[2] ? hud_neighbours[2]->image : curve_scene->image};
        for (int i = 0; i < 6; ++i) {
            ToGeneral(d, cmd, fg.probe[i]);
            d.TraceCmdBlitImage(cmd, sources[i], VK_IMAGE_LAYOUT_GENERAL, fg.probe[i].image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                           VK_FILTER_NEAREST);
        }
        VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &written, 0, nullptr,
                             0, nullptr);
        for (int i = 0; i < 6; ++i) {
            d.CmdCopyImageToBuffer(cmd, fg.probe[i].image, VK_IMAGE_LAYOUT_GENERAL, fg.probe_buffer[i].buffer, 1, &full);
        }
    }
    if (dump) {
        VkBufferImageCopy full{};
        full.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        full.imageExtent = {w, h, 1};
        d.CmdCopyImageToBuffer(cmd, (fg.hud.ok ? fg.previous : fg.backbuffer).image, VK_IMAGE_LAYOUT_GENERAL,
                               fg.dump_previous.buffer, 1, &full);
        if (hud_layer) {
            full.imageExtent = {hud_w, hud_h, 1};
            d.CmdCopyImageToBuffer(cmd, hud_layer->image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_hud.buffer, 1, &full);
            d.CmdCopyImageToBuffer(cmd, hud_scene->image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_scene.buffer, 1, &full);
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        d.CmdCopyImageToBuffer(cmd, fg.backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_real.buffer, 1, &region);
        d.CmdCopyImageToBuffer(cmd, fg.outputs[0].image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_generated.buffer, 1, &region);
        region.imageExtent = {render_w, render_h, 1};
        d.CmdCopyImageToBuffer(cmd, game_vectors->image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_vectors.buffer, 1, &region);
        d.CmdCopyImageToBuffer(cmd, depth->image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_depth.buffer, 1, &region);
        if (checked) {
            d.CmdCopyImageToBuffer(cmd, fg.fixed_vectors.image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_fixed.buffer, 1,
                                   &region);
        }
        if (markers_ran) {
            const VkBufferCopy all{0, 0, fg.markers.tiles.size};
            d.CmdCopyBuffer(cmd, fg.markers.tiles.buffer, fg.markers.dump.buffer, 1, &all);
        }
    }
    if (fg.hud.ok) {
        d.CmdCopyImage(cmd, fg.backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, fg.previous.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &whole);
    }
    // (a frame that came without a capture of its own was handed another frame's)
    fg.markers.previous_hud = fg.hud.ok && !hud_missing ? hud_capture : 0;
    // The performance panel, over every frame that goes out: the generated ones, and the game's own
    // by way of this layer's copy of it (which nothing reads after this; the interpolation and the
    // HUD passes have had the frame without the panel).
    const bool panel_on = RecordPanel(d, fg, cmd, set_number, now, static_cast<int>(fg.generated + 1));
    const bool panel_dump = panel_on && fg.panel.dump_next && ffx_ok && !reset;
    if (panel_on) {
        if (ffx_ok && !reset) {
            for (uint32_t i = 0; i < fg.generated; ++i) {
                RecordPanelOver(d, fg, cmd, set.hud_sets[PANEL_SETS + i], fg.hud.output_views[i]);
            }
        }
        RecordPanelOver(d, fg, cmd, set.hud_sets[PANEL_SETS + MAX_GENERATED], fg.hud.current_view);
        const VkImageCopy part = PanelPart(fg);
        ImageBarrier(d, cmd, fg.images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        d.CmdCopyImage(cmd, fg.backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, fg.images[index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &part);
        ImageBarrier(d, cmd, fg.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        if (panel_dump) {
            RecordPanelDump(d, fg, cmd, fg.outputs[0].image, 0);
            RecordPanelDump(d, fg, cmd, fg.backbuffer.image, 1);
        }
    }
    mark(6);
    fg.clock.written[set_number] = clocked;
    d.EndCommandBuffer(cmd);

    // Submitted in the place of the game's present: it waits for what the present waited for.
    std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info.waitSemaphoreCount;
    submit.pWaitSemaphores = info.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &fg.real_ready;
    if (d.QueueSubmit(queue, 1, &submit, set.fence) != VK_SUCCESS) {
        Log("fg: submit failed; frames pass through");
        fg.failed = true;
        return false; // the wait semaphores are untouched: the game's present goes on as it was
    }
    set.pending = true;
    event.record_ms = static_cast<float>((Now() - now) * 1000.0);
    if (!ffx_ok) {
        fg.failed = true; // this frame still has to be presented after real_ready
    }

    // A generated frame cannot be shown before the GPU has made it. Handed over at once, the first
    // one becomes ready together with the real frame behind it whenever the GPU is busy, and the
    // display then shows only the latter: a generated frame lost and the motion uneven. So: wait
    // for the work, then send the frames one step apart.
    static const bool wait_gpu = EnvFloat("SC_FG_WAIT_GPU", 1.0f) != 0.0f;
    const bool waited = wait_gpu && g_live_wait.load() != 0.0f && ffx_ok && (!reset || keep_cadence);
    if (waited) {
        Stage(2);
        const double before = Now();
        d.WaitForFences(d.handle, 1, &set.fence, VK_TRUE, 50'000'000ull);
        static const double test_wait = EnvFloat("SC_FG_TEST_WAIT_MS", 0.0f) * 0.001; // tests only: a busy GPU
        if (test_wait > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(test_wait));
        }
        event.wait_ms = static_cast<float>((Now() - before) * 1000.0);
        fg.wait_ms.push_back(event.wait_ms);
        if (link.chained && !link.duplicate) {
            GateGenerated();
        }
        // How long a frame takes from the present call until its generated frames can go out:
        // the level three in four of the last frames stayed under.
        fg.ready_ring[fg.ready_count++ % 32] = static_cast<float>(Now() - now);
        fg.ready_seconds = Level(fg.ready_ring, fg.ready_count);
        fg.hook_waited = true;
        // They go out at that level, not each at its own moment: the frames keep their distance
        // when the GPU's time for them wavers.
        const double left = now + fg.ready_seconds - Now();
        if (left > 0.0 && left < 0.02) {
            std::this_thread::sleep_for(std::chrono::duration<double>(left));
        }
        event.ready_ms = static_cast<float>((Now() - now) * 1000.0);
    }

    uint32_t presented = 0;
    // Pacing: the generated frames and then the game's frame go out one step apart, a step being
    // the game's frame time divided by the multiplier. The wait happens here, in the game's
    // present call.
    static const bool pace = EnvFloat("SC_FG_PACE", 1.0f) != 0.0f;
    // The generated frames go out from now on, the real one last, at `end`: one step apart when
    // there is time for that. The real frame has to be out before the next one comes, or frames
    // queue up behind this hook and the presents hold the game again: so not later than a frame
    // time after this hook began (less what follows the last present), and, once the next frame
    // has been handed over, not later than it gets here.
    // No more frames than the display shows (SC_FG_TARGET a second): what goes out faster is dropped
    // by the compositor at uneven moments, which is seen as judder in every pan. Where the frames
    // are handed over between devices the limiter sees to that on the game's side, and this is
    // only a net under it for frames that arrive bunched. Without a hand-over (one device) this
    // is the limit itself; holding frames here then also holds the game.
    const double min_gap = !cap ? 0.0 : link.chained ? 0.97 / (target * 0.999) : 0.985 / target;
    double out = Now();
    double end = out + step * fg.generated;
    if (waited) {
        end = std::min(end, now + period - fg.tail_seconds - 0.0005);
    }
    if (link.chained) {
        // For the limiter's gate: when this hook returns, and what a hook like it takes when it
        // hurries. The frames are not sent closer together than `min_gap`, whatever `end` says.
        const bool generating = ffx_ok && !reset;
        const bool paced = pace && step > 0.0 && (generating || keep_cadence);
        const double closest = generating && !fg.no_cap ? fg.generated * min_gap : 0.0;
        const double last = paced ? std::max({end, out, out + closest}) : out;
        GatePlan(last + fg.tail_seconds,
                 generating ? (waited ? fg.ready_seconds : out - now) + closest + fg.tail_seconds + 0.0003 : 0.0,
                 generating ? out - now : 0.0);
    }
    const auto wait_for = [&](uint32_t gaps) { // gaps: from the last frame sent to the real one
        if (!pace || step <= 0.0 || !gaps) {
            return;
        }
        if (link.chained && waited) {
            ChainFrame next;
            {
                std::lock_guard lock{g_chain_mutex};
                next = g_chain_newest;
            }
            if (next.number > link.frame.number && !next.gated) {
                end = std::min(end, next.time + fg.handover_least - fg.tail_seconds - 0.0005);
            }
        }
        out += std::max(end - out, 0.0) / gaps;
        const double before = Now();
        if (out > before) {
            Stage(5);
            std::this_thread::sleep_for(std::chrono::duration<double>(out - before));
            const double woke = Now();
            fg.paced += woke - before;
            fg.sleep_late_ms.push_back(static_cast<float>((woke - out) * 1000.0));
        }
    };
    const auto hold = [&] {
        const double t = Now(), due = fg.last_out + min_gap;
        if (min_gap > 0.0 && !fg.no_cap && due > t + 0.0003 && due - t < 0.05) {
            Stage(5);
            std::this_thread::sleep_for(std::chrono::duration<double>(due - t));
            ++fg.held;
            ++fg.window_held;
            const double woke = Now();
            fg.paced += woke - t;
            fg.sleep_late_ms.push_back(static_cast<float>((woke - due) * 1000.0));
        }
        ++fg.window_out;
        fg.last_out = Now();
    };
    const bool own_timing = WantOwnTiming(d, fg, info);
    const uint32_t shown_multiplier = fg.too_fast ? 1 : fg.generated + 1;
    // The game's frame, now waiting for this layer's work in place of the game's semaphores.
    const auto send_real = [&] {
        VkPresentInfoKHR real = info;
        real.waitSemaphoreCount = 1;
        real.pWaitSemaphores = &fg.real_ready;
        OwnTiming own_real;
        const bool real_asks = own_timing && RequestTiming(fg, own_real, info.pNext, true);
        if (real_asks) {
            real.pNext = &own_real.all;
        }
        hold();
        NoteOutput(fg);
        Stage(7);
        fg.real_sent = fg.last_out;
        fg.real_out_ms.push_back(static_cast<float>((fg.last_out - now) * 1000.0));
        const double before_present = Now();
        result = d.QueuePresentKHR(queue, &real);
        const double present_call = Now() - before_present;
        fg.present_seconds += present_call;
        ++fg.present_calls;
        if (real_asks) {
            NoteRequested(fg, own_real, result, fg.last_out, shown_multiplier, true, present_call, presented);
        }
    };
    // A generated frame goes out. False: there is no swapchain image for it.
    const VkImageCopy copy = WholeCopy(w, h);
    const auto send_generated = [&](uint32_t i) {
        uint32_t extra = 0;
        Stage(4);
        const double before_acquire = Now();
        const VkResult acquire = d.AcquireNextImageKHR(d.handle, fg.swapchain, 100'000'000ull,
                                                       fg.acquired[i], VK_NULL_HANDLE, &extra);
        fg.acquire_seconds += Now() - before_acquire;
        ++fg.acquires;
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            if (fg.frame_id % 600 == 0) {
                Log("fg: no swapchain image for a generated frame (%d)", acquire);
            }
            return false;
        }
        VkCommandBuffer blit = set.blit[i];
        d.ResetCommandBuffer(blit, 0);
        d.BeginCommandBuffer(blit, &begin);
        ImageBarrier(d, blit, fg.images[extra], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        d.CmdCopyImage(blit, fg.outputs[i].image, VK_IMAGE_LAYOUT_GENERAL, fg.images[extra],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(d, blit, fg.images[extra], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        d.EndCommandBuffer(blit);
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo blit_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        blit_submit.waitSemaphoreCount = 1;
        blit_submit.pWaitSemaphores = &fg.acquired[i];
        blit_submit.pWaitDstStageMask = &stage;
        blit_submit.commandBufferCount = 1;
        blit_submit.pCommandBuffers = &blit;
        blit_submit.signalSemaphoreCount = 1;
        blit_submit.pSignalSemaphores = &fg.blitted[i];
        d.QueueSubmit(queue, 1, &blit_submit, VK_NULL_HANDLE);
        VkPresentInfoKHR extra_present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        extra_present.waitSemaphoreCount = 1;
        extra_present.pWaitSemaphores = &fg.blitted[i];
        extra_present.swapchainCount = 1;
        extra_present.pSwapchains = &fg.swapchain;
        extra_present.pImageIndices = &extra;
        OwnTiming own;
        const bool asks = own_timing && RequestTiming(fg, own, nullptr, false);
        if (asks) {
            extra_present.pNext = &own.all;
        }
        hold();
        NoteOutput(fg);
        Stage(6);
        const double before_present = Now();
        const VkResult sent = d.QueuePresentKHR(queue, &extra_present);
        const double present_call = Now() - before_present;
        fg.present_seconds += present_call;
        ++fg.present_calls;
        if (asks) {
            NoteRequested(fg, own, sent, fg.last_out, shown_multiplier, false, present_call, i);
        }
        ++presented;
        return true;
    };
    if (ffx_ok && !reset) {
        for (uint32_t i = 0; i < fg.generated; ++i) {
            if (i) {
                wait_for(fg.generated - i + 1);
            }
            if (!send_generated(i)) {
                break;
            }
        }
    }
    if (presented || keep_cadence) {
        wait_for(1);
    }
    send_real();

    if (measure) {
        fg.measure_set = set_number;
        fg.curve_time = now;
        fg.measure_chained = link.chained;
        fg.measure_unshifted = link.chained && !link.duplicate && fg.chain_shift != 0 && link.hud == link.frame.hud;
    }
    if (dump) {
        Stage(9);
        d.WaitForFences(d.handle, 1, &set.fence, VK_TRUE, UINT64_MAX);
        const bool bgra = fg.format == VK_FORMAT_B8G8R8A8_UNORM || fg.format == VK_FORMAT_B8G8R8A8_SRGB;
        char path[600];
        if (hud_layer) { // the HUD layer (RGBA16F) and the frame without it
            std::snprintf(path, sizeof path, "%s/dump-%d-%03u-hud-%ux%u.bin", Dir(), getpid(), fg.dumps, hud_w, hud_h);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(fg.dump_hud.data, 1, static_cast<size_t>(hud_w) * hud_h * 8, file);
                std::fclose(file);
            }
            std::snprintf(path, sizeof path, "%s/dump-%d-%03u-scene.ppm", Dir(), getpid(), fg.dumps);
            WritePpm(path, static_cast<const uint8_t*>(fg.dump_scene.data), w, h, false);
        }
        std::snprintf(path, sizeof path, "%s/dump-%d-%03u-previous.ppm", Dir(), getpid(), fg.dumps);
        WritePpm(path, static_cast<const uint8_t*>(fg.dump_previous.data), w, h, bgra);
        std::snprintf(path, sizeof path, "%s/dump-%d-%03u-real.ppm", Dir(), getpid(), fg.dumps);
        WritePpm(path, static_cast<const uint8_t*>(fg.dump_real.data), w, h, bgra);
        std::snprintf(path, sizeof path, "%s/dump-%d-%03u-generated.ppm", Dir(), getpid(), fg.dumps);
        WritePpm(path, static_cast<const uint8_t*>(fg.dump_generated.data), w, h, bgra);
        // the game's vectors: RG16F, then depth: R32F, then (when they were checked) the vectors the
        // interpolation got: RG16F; all render_w x render_h (sizes are in the log line)
        std::snprintf(path, sizeof path, "%s/dump-%d-%03u-inputs-%ux%u.bin", Dir(), getpid(), fg.dumps, render_w, render_h);
        if (FILE* file = std::fopen(path, "wb")) {
            const size_t bytes = static_cast<size_t>(render_w) * render_h * 4;
            std::fwrite(fg.dump_vectors.data, 1, bytes, file);
            std::fwrite(fg.dump_depth.data, 1, bytes, file);
            if (checked) {
                std::fwrite(fg.dump_fixed.data, 1, bytes, file);
            }
            std::fclose(file);
        }
        if (markers_ran) { // moving HUD content: four counts, then per tile what the passes found (tools/hud-motion-check.py)
            std::snprintf(path, sizeof path, "%s/dump-%d-%03u-markers-%ux%u.bin", Dir(), getpid(), fg.dumps,
                          fg.markers.tiles_w, fg.markers.tiles_h);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fwrite(fg.markers.dump.data, 1, fg.markers.tiles.size, file);
                std::fclose(file);
            }
        }
        Log("fg: dump %u written (render %ux%u, %s inputs%s%s%s, HUD capture %llu)", fg.dumps, render_w, render_h,
            game_vectors == &fg.zero_vectors ? "blank" : "game", checked ? ", vectors checked" : "",
            markers_ran ? ", moving HUD content looked for" : "",
            unmixed ? ", no capture of its own: scene worked out" : "", static_cast<unsigned long long>(hud_capture));
        ++fg.dumps;
        fg.dump_next = fg.dump_more > 0; // (the frames of a run of dumps follow each other without a gap)
        fg.dump_more -= fg.dump_more > 0;
    }

    if (panel_dump) {
        Stage(9);
        d.WaitForFences(d.handle, 1, &set.fence, VK_TRUE, UINT64_MAX);
        WritePanelDump(fg, "generated", 0);
        WritePanelDump(fg, "real", 1);
        Log("fg: panel dump %u written (a generated frame and the game's)", fg.panel.dumps);
        ++fg.panel.dumps;
        fg.panel.dump_next = false;
    }

    fg.have_previous = !hud_missing || unmixed;
    fg.unmixed_last = unmixed;
    fg.last_present = now;
    ++fg.frame_id;
    ++fg.real_frames;
    fg.generated_frames += presented;
    return true;
}

/// A frame that goes out as the game made it (menus, loading, generation switched off or not
/// needed), with the panel over it: a small command buffer in the place of the game's present, as
/// for the frames that are generated from, and the present then waits for it. False: the frame
/// is to go out untouched.
bool PassWithPanel(Device& d, Fg& fg, VkQueue queue, uint32_t family, const VkPresentInfoKHR& info, VkResult& result) {
    Fg::Panel& panel = fg.panel;
    if (panel.step <= 0 || panel.failed || fg.failed || !fg.modified) {
        return false;
    }
    if (!fg.ready && !Prepare(d, fg, family)) {
        Log("fg: could not be set up; frames pass through");
        fg.failed = true;
        return false;
    }
    if (family != fg.family || !PreparePanel(d, fg)) {
        return false;
    }
    if (panel.pass_pending) {
        if (d.WaitForFences(d.handle, 1, &panel.pass_fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) {
            return false;
        }
        d.ResetFences(d.handle, 1, &panel.pass_fence);
        panel.pass_pending = false;
    }
    const VkImage image = fg.images[info.pImageIndices[0]];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkCommandBuffer cmd = panel.pass_cmd;
    d.ResetCommandBuffer(cmd, 0);
    d.BeginCommandBuffer(cmd, &begin);
    ToGeneral(d, cmd, fg.backbuffer);
    if (!RecordPanel(d, fg, cmd, COMMAND_SETS, Now(), 1)) {
        d.EndCommandBuffer(cmd);
        return false;
    }
    // The panel's part of the frame into this layer's copy, the panel over it, and back. (Of that
    // copy only this part is touched, and the next frame that is generated from renews all of it.)
    const VkImageCopy part = PanelPart(fg);
    ImageBarrier(d, cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    d.CmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fg.backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, 1, &part);
    ImageBarrier(d, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    RecordPanelOver(d, fg, cmd, panel.pass_set, fg.hud.current_view);
    d.CmdCopyImage(cmd, fg.backbuffer.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &part);
    ImageBarrier(d, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    const bool dump = panel.dump_next;
    if (dump) {
        RecordPanelDump(d, fg, cmd, fg.backbuffer.image, 0);
    }
    d.EndCommandBuffer(cmd);

    const std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info.waitSemaphoreCount;
    submit.pWaitSemaphores = info.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &fg.real_ready;
    if (d.QueueSubmit(queue, 1, &submit, panel.pass_fence) != VK_SUCCESS) {
        Log("fg: the performance panel's submit failed; no panel");
        panel.failed = true;
        return false; // the wait semaphores are untouched: the game's present goes on as it was
    }
    panel.pass_pending = true;
    VkPresentInfoKHR real = info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &fg.real_ready;
    result = d.QueuePresentKHR(queue, &real);
    if (dump) {
        d.WaitForFences(d.handle, 1, &panel.pass_fence, VK_TRUE, UINT64_MAX);
        WritePanelDump(fg, "passed", 0);
        Log("fg: panel dump %u written (a frame that passed through)", panel.dumps);
        ++panel.dumps;
        panel.dump_next = false;
    }
    return true;
}

// ----------------------------------------------------------------------------- hooks

VKAPI_ATTR void VKAPI_CALL HookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    IN_HOOK("GetDeviceQueue");
    Device* d = GetDevice(Key(device));
    d->GetDeviceQueue(device, family, index, queue);
    if (*queue) {
        std::lock_guard lock{g_tables_mutex};
        g_queues[U64(*queue)] = {d, family};
    }
}

VKAPI_ATTR void VKAPI_CALL HookGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    IN_HOOK("GetDeviceQueue2");
    Device* d = GetDevice(Key(device));
    d->GetDeviceQueue2(device, info, queue);
    if (*queue) {
        std::lock_guard lock{g_tables_mutex};
        g_queues[U64(*queue)] = {d, info->queueFamilyIndex};
    }
}

VKAPI_ATTR VkResult VKAPI_CALL HookAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                                          VkCommandBuffer* buffers) {
    IN_HOOK("AllocCmdBuffers");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->AllocateCommandBuffers(device, info, buffers);
    if (result == VK_SUCCESS) {
        std::lock_guard lock{g_tables_mutex};
        for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
            g_command_buffers[U64(buffers[i])] = d;
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL HookFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
                                                  const VkCommandBuffer* buffers) {
    IN_HOOK("FreeCmdBuffers");
    Device* d = GetDevice(Key(device));
    if (buffers) {
        {
            std::lock_guard lock{g_tables_mutex};
            for (uint32_t i = 0; i < count; ++i) {
                g_command_buffers.erase(U64(buffers[i]));
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            ForgetHudPass(buffers[i]);
        }
    }
    d->FreeCommandBuffers(device, pool, count, buffers);
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateImage(VkDevice device, const VkImageCreateInfo* info,
                                               const VkAllocationCallbacks* allocator, VkImage* image) {
    IN_HOOK("CreateImage");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->CreateImage(device, info, allocator, image);
    if (result == VK_SUCCESS && info->imageType == VK_IMAGE_TYPE_2D) {
        std::lock_guard lock{g_records_mutex};
        bool shared = false;
        for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext) {
            shared |= next->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        }
        g_images[U64(*image)] = {info->format, info->extent.width, info->extent.height, shared, false};
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL HookDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    IN_HOOK("DestroyImage");
    Device* d = GetDevice(Key(device));
    {
        std::lock_guard lock{g_records_mutex};
        g_images.erase(U64(image));
    }
    if (image) {
        ForgetImage(*d, image);
    }
    d->DestroyImage(device, image, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                   const VkAllocationCallbacks* allocator, VkImageView* view) {
    IN_HOOK("CreateImageView");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->CreateImageView(device, info, allocator, view);
    if (result == VK_SUCCESS) {
        std::lock_guard lock{g_records_mutex};
        g_views[U64(*view)] = {U64(info->image), info->format, info->subresourceRange.aspectMask};
    }
    if (result == VK_SUCCESS && g_test_inputs) {
        NoteView(*d, *view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, false);
        if (info->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
            std::lock_guard lock{d->inputs.mutex};
            d->inputs.fresh = true;
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL HookDestroyImageView(VkDevice device, VkImageView view,
                                                const VkAllocationCallbacks* allocator) {
    IN_HOOK("DestroyImageView");
    Device* d = GetDevice(Key(device));
    {
        std::lock_guard lock{g_records_mutex};
        g_views.erase(U64(view));
    }
    d->DestroyImageView(device, view, allocator);
}

/// Self-test only: an empty barrier stands in for DLSS's kernel launch.
VKAPI_ATTR void VKAPI_CALL HookCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags source,
                                                  VkPipelineStageFlags destination, VkDependencyFlags flags,
                                                  uint32_t memory_count, const VkMemoryBarrier* memory,
                                                  uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                                                  uint32_t image_count, const VkImageMemoryBarrier* images) {
    IN_HOOK("PipelineBarrier");
    Device* d = DeviceOfCommandBuffer(cmd);
    if (!memory_count && !buffer_count && !image_count) {
        CopyInputs(*d, cmd);
    }
    d->CmdPipelineBarrier(cmd, source, destination, flags, memory_count, memory, buffer_count, buffers,
                          image_count, images);
}

VKAPI_ATTR uint32_t VKAPI_CALL HookGetImageViewHandleNVX(VkDevice device, const VkImageViewHandleInfoNVX* info) {
    IN_HOOK("ViewHandle32");
    Device* d = GetDevice(Key(device));
    NoteView(*d, info->imageView, info->descriptorType, true);
    STEP(); // (from here on it is the driver's)
    return d->GetImageViewHandleNVX(device, info);
}

VKAPI_ATTR uint64_t VKAPI_CALL HookGetImageViewHandle64NVX(VkDevice device, const VkImageViewHandleInfoNVX* info) {
    IN_HOOK("ViewHandle64");
    Device* d = GetDevice(Key(device));
    NoteView(*d, info->imageView, info->descriptorType, false);
    STEP();
    return d->GetImageViewHandle64NVX(device, info);
}

VKAPI_ATTR void VKAPI_CALL HookCmdCuLaunchKernelNVX(VkCommandBuffer cmd, const VkCuLaunchInfoNVX* info) {
    IN_HOOK("LaunchKernel");
    Device* d = DeviceOfCommandBuffer(cmd);
    CopyInputs(*d, cmd);
    TraceCount(cmd, 2);
    if (const HeldFor lock{d->inputs.mutex, PATIENCE}; lock) {
        d->inputs.launched = true;
        ++d->inputs.n_launch;
    }
    STEP();
    d->CmdCuLaunchKernelNVX(cmd, info);
}

// VK_NV_low_latency2 (Reflex): passed on untouched and counted. Whether and where the game asks for
// its latency sleep says where a limiter could hold it without a frame of delay: before it reads
// its input, not after the frame is drawn.
Fg* FindFg(Device& d, VkSwapchainKHR swapchain) {
    std::lock_guard lock{d.fg_mutex};
    const auto it = d.swapchains.find(U64(swapchain));
    return it == d.swapchains.end() ? nullptr : it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL HookSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
                                                         const VkLatencySleepModeInfoNV* info) {
    IN_HOOK("LatencySleepMode");
    Device* d = GetDevice(Key(device));
    // (What is asked for stays as it is, with the GPU's boost added where that is switched on.)
    VkLatencySleepModeInfoNV changed{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    if (info) {
        changed = *info;
        if (Fg* fg = FindFg(*d, swapchain)) {
            fg->sleep_mode = *info;
            fg->sleep_mode.pNext = nullptr;
            fg->latency_swapchain = true;
            changed.lowLatencyBoost |= fg->boost_on ? VK_TRUE : VK_FALSE;
        }
    }
    static std::atomic<uint32_t> logged{};
    if (logged.fetch_add(1) < 16) {
        if (info) {
            Log("low latency: sleep mode set on device %u: %s, boost %s, minimum interval %u us (thread %d)", d->index,
                info->lowLatencyMode ? "on" : "off", info->lowLatencyBoost ? "on" : "off", info->minimumIntervalUs,
                static_cast<int>(syscall(SYS_gettid)));
        } else {
            Log("low latency: sleep mode reset on device %u", d->index);
        }
    }
    return d->SetLatencySleepModeNV(device, swapchain, info ? &changed : nullptr);
}

/// The boost switched on or off (PARAMS: boost): the GPU keeps its speed up, whatever the game's
/// load. It needs nothing of the game: without low-latency mode the boost is still a hint to the
/// GPU, and that mode stays as vkd3d-proton has it.
void ApplyBoost(Device& d, Fg& fg) {
    const bool boost = g_live_boost.load() != 0.0f;
    if (boost == fg.boost_on || !fg.latency_swapchain || !d.SetLatencySleepModeNV) {
        return;
    }
    fg.boost_on = boost;
    VkLatencySleepModeInfoNV mode = fg.sleep_mode;
    mode.lowLatencyBoost = boost || fg.sleep_mode.lowLatencyBoost ? VK_TRUE : VK_FALSE;
    const VkResult result = d.SetLatencySleepModeNV(d.handle, fg.swapchain, &mode);
    Log("fg: the GPU's boost is %s (result %d)", boost ? "on" : "off", result);
}

/// The priority of the thread that presents (PARAMS: priority), set on that thread as it comes by.
void ApplyPriority() {
    static thread_local int applied = 0, before = 0;
    const int wanted = std::clamp(static_cast<int>(g_live_priority.load()), 0, 20);
    if (wanted == applied) {
        return;
    }
    const auto thread = static_cast<id_t>(syscall(SYS_gettid));
    if (!applied) {
        errno = 0;
        const int now = getpriority(PRIO_PROCESS, thread);
        before = errno ? 0 : now;
    }
    applied = wanted;
    const int nice_value = wanted ? -wanted : before;
    const int result = setpriority(PRIO_PROCESS, thread, nice_value);
    Log("fg: the presenting thread's nice value is now %d%s%s", result ? getpriority(PRIO_PROCESS, thread) : nice_value,
        result ? ": not allowed, " : "", result ? std::strerror(errno) : "");
}

/// The present id of a present (vkd3d-proton gives every frame of the game one); 0: none.
uint64_t PresentIdOf(const VkPresentInfoKHR& info) {
    for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext) {
#ifdef VK_KHR_present_id2
        if (next->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) {
            const auto* ids = reinterpret_cast<const VkPresentId2KHR*>(next);
            return ids->swapchainCount && ids->pPresentIds ? ids->pPresentIds[0] : 0;
        }
#endif
        if (next->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
            const auto* ids = reinterpret_cast<const VkPresentIdKHR*>(next);
            return ids->swapchainCount && ids->pPresentIds ? ids->pPresentIds[0] : 0;
        }
    }
    return 0;
}

/// The game's frame has gone to the display at `time`: for when word of it comes back.
void NoteSent(Fg& fg, const VkPresentInfoKHR& info, double time, uint32_t multiplier) {
    const uint64_t id = PresentIdOf(info);
    if (!id) {
        return;
    }
    Fg::Shown::Sent& sent = fg.shown.sent[id % Fg::Shown::SENT];
    sent.id.store(0, std::memory_order_relaxed);
    sent.time.store(time, std::memory_order_relaxed);
    sent.multiplier.store(multiplier, std::memory_order_relaxed);
    sent.event.store(fg.events.empty() ? 0 : fg.event_count, std::memory_order_relaxed);
    sent.id.store(id, std::memory_order_release);
}

bool FindSent(Fg& fg, uint64_t id, double& time, uint64_t& event, uint32_t& multiplier) {
    const Fg::Shown::Sent& sent = fg.shown.sent[id % Fg::Shown::SENT];
    if (!id || sent.id.load(std::memory_order_acquire) != id) {
        return false;
    }
    time = sent.time.load(std::memory_order_relaxed);
    event = sent.event.load(std::memory_order_relaxed);
    multiplier = sent.multiplier.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return sent.id.load(std::memory_order_relaxed) == id;
}

/// A wait for a present has ended: the frame has been shown, or nearly. The report of the moment
/// itself (below) is the better figure; this one is there when the driver gives none.
void NoteWaited(Device& d, VkSwapchainKHR swapchain, uint64_t id) {
    Fg* fg = FindFg(d, swapchain);
    double time = 0.0;
    uint64_t event = 0;
    uint32_t multiplier = 0;
    if (!fg || !FindSent(*fg, id, time, event, multiplier)) {
        return;
    }
    Fg::Shown& s = fg->shown;
    const uint32_t count = s.waited_count.load(std::memory_order_relaxed);
    s.waited_ms[count % Fg::Shown::RING] = static_cast<float>((Now() - time) * 1000.0);
    s.waited_count.store(count + 1, std::memory_order_release);
}

VKAPI_ATTR VkResult VKAPI_CALL HookWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t id,
                                                     uint64_t timeout) {
    IN_HOOK("WaitForPresent");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->WaitForPresentKHR(device, swapchain, id, timeout);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        NoteWaited(*d, swapchain, id);
    }
    return result;
}

#ifdef VK_EXT_present_timing
VKAPI_ATTR VkResult VKAPI_CALL HookWaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain,
                                                      const VkPresentWait2InfoKHR* info) {
    IN_HOOK("WaitForPresent2");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->WaitForPresent2KHR(device, swapchain, info);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        NoteWaited(*d, swapchain, info->presentId);
    }
    return result;
}

/// The display's clock against Now(), asked for now and then. A single reading of its clock
/// between two of ours does: the two readings are a few microseconds apart, or it is tried again.
bool CalibrateShown(Device& d, Fg& fg, const VkPastPresentationTimingEXT& timing, uint32_t stage, double now) {
    Fg::Shown& s = fg.shown;
    const int domain = static_cast<int>(timing.timeDomain);
    if (s.domain == domain && s.domain_id == timing.timeDomainId && s.stage == stage && now - s.calibrated < 5.0) {
        return s.clock_ok;
    }
    s.domain = domain;
    s.domain_id = timing.timeDomainId;
    s.stage = stage;
    s.calibrated = now;
    s.clock_ok = false;
    if (!d.GetCalibratedTimestampsKHR) {
        return false;
    }
    VkSwapchainCalibratedTimestampInfoEXT local{VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT};
    local.swapchain = fg.swapchain;
    local.presentStage = stage;
    local.timeDomainId = timing.timeDomainId;
    VkCalibratedTimestampInfoKHR info{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR};
    info.timeDomain = timing.timeDomain;
    if (timing.timeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT || timing.timeDomain == VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT) {
        info.pNext = &local;
    }
    double best = 1.0;
    for (int attempt = 0; attempt < 8 && best > 0.0001; ++attempt) {
        uint64_t stamp = 0, deviation = 0;
        const double before = Now();
        if (d.GetCalibratedTimestampsKHR(d.handle, 1, &info, &stamp, &deviation) != VK_SUCCESS) {
            break;
        }
        const double after = Now();
        if (after - before < best) {
            best = after - before;
            s.offset = 0.5 * (before + after) - static_cast<double>(stamp) * 1e-9;
            s.clock_ok = true;
        }
    }
    return s.clock_ok;
}

/// What vkd3d-proton has just been told about frames that went out: looked at as it passes.
void NoteTimings(Device& d, Fg& fg, const VkPastPresentationTimingPropertiesEXT& properties) {
    Fg::Shown& s = fg.shown;
    const double now = Now();
    for (uint32_t i = 0; i < properties.presentationTimingCount; ++i) {
        const VkPastPresentationTimingEXT& timing = properties.pPresentationTimings[i];
        if (!timing.reportComplete) {
            continue;
        }
        // About a frame this layer asked after: the caller has it now, TakeOwnTimings will not get it.
        if (s.own_active.load(std::memory_order_relaxed) && timing.presentId <= s.own_last_id.load(std::memory_order_relaxed)) {
            s.others_read.fetch_add(1, std::memory_order_relaxed);
            if (!s.others_said.exchange(true, std::memory_order_relaxed)) {
                Log("fg: thread %s reads the display's reports as well: the frames it gets them for have no time here", ThreadName());
            }
            continue;
        }
        if (!timing.pPresentStages) {
            continue;
        }
        // The last stage reported is the one nearest the display (the bits go in that order).
        uint64_t stamp = 0;
        uint32_t stage = 0;
        for (uint32_t j = 0; j < timing.presentStageCount; ++j) {
            if (timing.pPresentStages[j].time && timing.pPresentStages[j].stage > stage) {
                stage = timing.pPresentStages[j].stage;
                stamp = timing.pPresentStages[j].time;
            }
        }
        if (!stamp) {
            continue;
        }
        const bool clock = CalibrateShown(d, fg, timing, stage, now);
        if (!s.said) {
            s.said = true;
            Log("fg: the display reports when frames reach it (stage 0x%x, time domain %d, its clock %s)", stage,
                static_cast<int>(timing.timeDomain), clock ? "compared with this layer's" : "not comparable: taken as the report's arrival");
        }
        const double shown = clock ? static_cast<double>(stamp) * 1e-9 + s.offset : now;
        double sent = 0.0;
        uint64_t event = 0;
        uint32_t multiplier = 0;
        if (!FindSent(fg, timing.presentId, sent, event, multiplier) || shown < sent - 0.001 || shown > sent + 1.0) {
            s.unmatched.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint32_t count = s.count.load(std::memory_order_relaxed);
        Fg::Shown::Entry& entry = s.ring[count % Fg::Shown::RING];
        entry.event = event;
        entry.delay_ms = static_cast<float>((shown - sent) * 1000.0);
        entry.gap_ms = s.last_id + 1 == timing.presentId ? static_cast<float>((shown - s.last_shown) * 1000.0) : 0.0f;
        entry.multiplier = multiplier;
        s.count.store(count + 1, std::memory_order_release);
        s.last_id = timing.presentId;
        s.last_shown = shown;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL HookGetPastPresentationTimingEXT(VkDevice device, const VkPastPresentationTimingInfoEXT* info,
                                                                VkPastPresentationTimingPropertiesEXT* properties) {
    IN_HOOK("PastPresentTime");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->GetPastPresentationTimingEXT(device, info, properties);
    if (result >= 0 && properties->pPresentationTimings) {
        if (Fg* fg = FindFg(*d, info->swapchain)) {
            NoteTimings(*d, *fg, *properties);
        }
    }
    return result;
}
#endif

/// For the log, once per swapchain: what the game's present brings along.
void LogPresentChain(const VkPresentInfoKHR& info) {
    std::string names;
    for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext) {
        char name[64];
        std::snprintf(name, sizeof name, "%d ", static_cast<int>(next->sType));
        names += name;
        if (next->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR) {
            const auto* modes = reinterpret_cast<const VkSwapchainPresentModeInfoKHR*>(next);
            if (modes->swapchainCount && modes->pPresentModes) {
                std::snprintf(name, sizeof name, "(present mode %d) ", static_cast<int>(modes->pPresentModes[0]));
                names += name;
            }
        }
#ifdef VK_EXT_present_timing
        if (next->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) {
            const auto* timings = reinterpret_cast<const VkPresentTimingsInfoEXT*>(next);
            if (timings->swapchainCount && timings->pTimingInfos) {
                std::snprintf(name, sizeof name, "(timing: stages 0x%x, flags 0x%x) ",
                              timings->pTimingInfos[0].presentStageQueries, timings->pTimingInfos[0].flags);
                names += name;
            }
        }
#endif
    }
    Log("swapchain: the first present brings along: %s", names.empty() ? "nothing" : names.c_str());
}

VKAPI_ATTR VkResult VKAPI_CALL HookLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkLatencySleepInfoNV* info) {
    IN_HOOK("LatencySleep");
    Device* d = GetDevice(Key(device));
    if (g_latency_sleeps.fetch_add(1) == 0) {
        static std::atomic<uint32_t> logged{};
        if (logged.fetch_add(1) < 4) {
            Log("low latency: the game asks for its latency sleep (device %u, thread %d)", d->index,
                static_cast<int>(syscall(SYS_gettid)));
        }
    }
    return d->LatencySleepNV(device, swapchain, info);
}

VKAPI_ATTR void VKAPI_CALL HookSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkSetLatencyMarkerInfoNV* info) {
    IN_HOOK("LatencyMarker");
    Device* d = GetDevice(Key(device));
    g_latency_markers.fetch_add(1);
    static std::atomic<uint32_t> seen{};
    if (info && static_cast<uint32_t>(info->marker) < 32) {
        const uint32_t bit = 1u << static_cast<uint32_t>(info->marker);
        if (!(seen.fetch_or(bit) & bit)) {
            Log("low latency: marker %d first set (device %u, thread %d)", static_cast<int>(info->marker), d->index,
                static_cast<int>(syscall(SYS_gettid)));
        }
    }
    d->SetLatencyMarkerNV(device, swapchain, info);
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                      const VkAllocationCallbacks* allocator,
                                                      VkSwapchainKHR* swapchain) {
    IN_HOOK("CreateSwapchain");
    Device* d = GetDevice(Key(device));
    // (SC_FG_KEEP_SWAPCHAIN=1, a diagnosis: every swapchain is created as asked for, which leaves no
    // room for generated frames)
    static const bool keep = EnvFloat("SC_FG_KEEP_SWAPCHAIN", 0.0f) != 0.0f;
    // With a runner that ships vkd3d-proton the game presents through Direct3D 12, and the swapchain
    // is vkd3d-proton's. Without it the game's renderer makes a swapchain of its own, and the game
    // does not survive a change to that one: it closes a few seconds later, while starting up
    // (LUG's runners without "experimental" in their name, Alpha 4.10). That one is left as it is.
    const bool own = d->instance && !std::strcmp(d->instance->engine, "StarEngine");
    const bool wanted = !keep && !own && (d->nvx || g_test_inputs || EnvFloat("SC_FG_FALLBACK", 0.0f) != 0.0f);
    VkSwapchainCreateInfoKHR changed = *info;
    if (wanted) {
        // Room for the generated frames, and the right to copy from and into the images. No more
        // images than the surface takes: with 5x to 8x (the game's own and eight more) the request went
        // past that and NVIDIA's driver crashed in vkCreateSwapchainKHR instead of failing. A generated
        // frame takes its image just before it is shown, so fewer is a smaller buffer, not a stop.
        changed.minImageCount += g_generated_cap + 1;
        const auto capabilities_of = d->instance ? reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                                                       d->instance->gipa(d->instance->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
                                                 : nullptr;
        VkSurfaceCapabilitiesKHR capabilities{};
        if (capabilities_of && capabilities_of(d->physical, info->surface, &capabilities) == VK_SUCCESS &&
            capabilities.maxImageCount != 0 && changed.minImageCount > capabilities.maxImageCount) {
            Log("swapchain: %u images wanted, the surface takes %u at most", changed.minImageCount, capabilities.maxImageCount);
            changed.minImageCount = std::max(info->minImageCount, capabilities.maxImageCount);
        }
        changed.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    VkResult result = d->CreateSwapchainKHR(device, &changed, allocator, swapchain);
    bool modified = wanted && result == VK_SUCCESS;
    if (wanted && result != VK_SUCCESS) {
        Log("swapchain: the changed request failed (%d); created as the game asked, no frame generation", result);
        result = d->CreateSwapchainKHR(device, info, allocator, swapchain);
    }
    if (result == VK_SUCCESS) {
        if (g_trace) {
            uint32_t count = 0;
            d->GetSwapchainImagesKHR(device, *swapchain, &count, nullptr);
            std::vector<VkImage> images(count);
            d->GetSwapchainImagesKHR(device, *swapchain, &count, images.data());
            std::lock_guard lock{g_records_mutex};
            for (VkImage image : images) {
                g_images[U64(image)] = {info->imageFormat, info->imageExtent.width, info->imageExtent.height, false, true};
            }
        }
        Fg* fg = new Fg;
        fg->swapchain = *swapchain;
        fg->format = info->imageFormat;
        fg->extent = info->imageExtent;
        fg->modified = modified;
        fg->surface = info->surface;
        fg->create_flags = info->flags;
        for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV) {
                fg->latency_swapchain = reinterpret_cast<const VkSwapchainLatencyCreateInfoNV*>(next)->latencyModeEnable;
            }
        }
        Log("swapchain: flags 0x%x, made for low latency: %s", info->flags, fg->latency_swapchain ? "yes" : "no");
        std::lock_guard lock{d->fg_mutex};
        d->swapchains[U64(*swapchain)] = fg;
        Log("swapchain: %ux%u format %d present mode %d min images %u -> %u%s (device %u, thread %s%s)",
            info->imageExtent.width, info->imageExtent.height, info->imageFormat, info->presentMode,
            info->minImageCount, modified ? changed.minImageCount : info->minImageCount, modified ? "" : " (untouched)",
            d->index, ThreadName(), info->oldSwapchain ? ", replaces one" : "");
        if (static std::atomic<bool> said{}; own && !said.exchange(true)) {
            Log("swapchain: this one is the game's own, which it does not let a layer change: no frame generation. The "
                "runner has no vkd3d-proton, with which the game presents through Direct3D 12 (the LUG's experimental "
                "runners have it)");
        }
    }
    return result;
}

void DestroyFg(Device& d, Fg* fg) {
    if (fg->ready) {
        for (Fg::Set& set : fg->sets) {
            if (set.pending) {
                d.WaitForFences(d.handle, 1, &set.fence, VK_TRUE, 2'000'000'000ull);
            }
        }
    }
    Fg::Panel& panel = fg->panel;
    if (panel.pass_pending) {
        d.WaitForFences(d.handle, 1, &panel.pass_fence, VK_TRUE, 2'000'000'000ull);
    }
    if (panel.pass_fence) {
        d.DestroyFence(d.handle, panel.pass_fence, nullptr);
    }
    if (fg->ffx_created) {
        g_ffx_device = &d;
        ffxFrameInterpolationContextDestroy(&fg->interpolation);
        ffxOpticalflowContextDestroy(&fg->optical_flow);
        for (FfxResourceInternal resource :
             {fg->of_vector, fg->of_scd, fg->dilated_depth, fg->dilated_motion, fg->previous_depth}) {
            fg->backend.fpDestroyResource(&fg->backend, resource, fg->shared_context);
        }
        fg->backend.fpDestroyBackendContext(&fg->backend, fg->shared_context);
    }
    std::free(fg->scratch);
    Fg::Hud& hud = fg->hud;
    for (auto& [image, view] : hud.vector_views) {
        hud.retired_views.push_back(view);
    }
    for (auto& [image, view] : hud.layer_views) {
        hud.retired_views.push_back(view);
    }
    hud.retired_views.push_back(fg->curve_view);
    hud.retired_views.push_back(panel.view);
    for (VkImageView view : {fg->markers.quarter_view, fg->markers.sixteenth_view, fg->markers.blocks_view,
                             fg->markers.cells_view, fg->markers.which_view}) {
        hud.retired_views.push_back(view);
    }
    for (VkImageView view : {hud.current_view, hud.previous_view, hud.masked_view, hud.mask_view, hud.grown_view[0],
                             hud.grown_view[1], hud.checked_view, hud.fixed_view, hud.tiles_view, hud.reach_view,
                             hud.output_views[0], hud.output_views[1], hud.output_views[2]}) {
        hud.retired_views.push_back(view);
    }
    for (VkImageView view : hud.retired_views) {
        if (view) {
            d.DestroyImageView(d.handle, view, nullptr);
        }
    }
    for (int i = 0; i < ALL_PASSES; ++i) {
        if (hud.pipelines[i]) {
            d.DestroyPipeline(d.handle, hud.pipelines[i], nullptr);
        }
        if (hud.shaders[i]) {
            d.DestroyShaderModule(d.handle, hud.shaders[i], nullptr);
        }
    }
    if (hud.layout) {
        d.DestroyPipelineLayout(d.handle, hud.layout, nullptr);
    }
    if (hud.pool) {
        d.DestroyDescriptorPool(d.handle, hud.pool, nullptr);
    }
    if (hud.sampler) {
        d.DestroySampler(d.handle, hud.sampler, nullptr);
    }
    if (hud.set_layout) {
        d.DestroyDescriptorSetLayout(d.handle, hud.set_layout, nullptr);
    }
    DestroyOwnImage(d, fg->previous);
    if (fg->clock.pool) {
        d.DestroyQueryPool(d.handle, fg->clock.pool, nullptr);
    }
    DestroyOwnImage(d, fg->unmixed[0]);
    DestroyOwnImage(d, fg->unmixed[1]);
    DestroyOwnImage(d, fg->curve_image);
    DestroyOwnImage(d, panel.image);
    for (Image* image : {&fg->markers.quarter, &fg->markers.sixteenth, &fg->markers.blocks, &fg->markers.cells,
                         &fg->markers.which}) {
        DestroyOwnImage(d, *image);
    }
    fg->retired_buffers.push_back(fg->markers.tiles);
    fg->retired_buffers.push_back(fg->markers.dump);
    for (const HostBuffer& buffer : fg->markers.counts) {
        fg->retired_buffers.push_back(buffer);
    }
    fg->retired_buffers.push_back(panel.dump);
    for (const HostBuffer& buffer : panel.staging) {
        fg->retired_buffers.push_back(buffer);
    }
    DestroyOwnImage(d, fg->no_scene_change);
    for (Image& probe : fg->probe) {
        DestroyOwnImage(d, probe);
    }
    DestroyOwnImage(d, fg->test_scene);
    DestroyOwnImage(d, fg->test_layer);
    DestroyOwnImage(d, fg->mask);
    DestroyOwnImage(d, fg->grown[0]);
    DestroyOwnImage(d, fg->grown[1]);
    DestroyOwnImage(d, fg->masked_vectors);
    DestroyOwnImage(d, fg->checked_vectors);
    DestroyOwnImage(d, fg->fixed_vectors);
    DestroyOwnImage(d, fg->vector_tiles);
    DestroyOwnImage(d, fg->vector_reach);
    fg->retired_buffers.push_back(fg->tile_lists);
    for (const HostBuffer& buffer : fg->retired_buffers) {
        if (buffer.buffer) {
            d.DestroyBuffer(d.handle, buffer.buffer, nullptr);
            d.FreeMemory(d.handle, buffer.memory, nullptr);
        }
    }
    for (Image& image : fg->retired) {
        DestroyOwnImage(d, image);
    }
    for (Fg::Set& set : fg->sets) {
        if (set.fence) {
            d.DestroyFence(d.handle, set.fence, nullptr);
        }
    }
    if (fg->pool) {
        d.DestroyCommandPool(d.handle, fg->pool, nullptr);
    }
    if (fg->real_ready) {
        d.DestroySemaphore(d.handle, fg->real_ready, nullptr);
    }
    for (uint32_t i = 0; i < MAX_GENERATED; ++i) {
        if (fg->acquired[i]) {
            d.DestroySemaphore(d.handle, fg->acquired[i], nullptr);
        }
        if (fg->blitted[i]) {
            d.DestroySemaphore(d.handle, fg->blitted[i], nullptr);
        }
        DestroyOwnImage(d, fg->outputs[i]);
    }
    DestroyOwnImage(d, fg->backbuffer);
    DestroyOwnImage(d, fg->zero_vectors);
    DestroyOwnImage(d, fg->zero_depth);
    for (HostBuffer* buffer :
         {&fg->dump_real, &fg->dump_generated, &fg->dump_vectors, &fg->dump_depth, &fg->dump_previous,
          &fg->dump_hud, &fg->dump_scene, &fg->dump_fixed, &fg->curve_staging, &fg->probe_buffer[0], &fg->probe_buffer[1],
          &fg->probe_buffer[2], &fg->probe_buffer[3], &fg->probe_buffer[4],
          &fg->probe_buffer[5]}) {
        if (buffer->buffer) {
            d.DestroyBuffer(d.handle, buffer->buffer, nullptr);
            d.FreeMemory(d.handle, buffer->memory, nullptr);
        }
    }
    delete fg;
}

VKAPI_ATTR void VKAPI_CALL HookDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks* allocator) {
    IN_HOOK("DestroySwapchain");
    Device* d = GetDevice(Key(device));
    Fg* fg = nullptr;
    {
        std::lock_guard lock{d->fg_mutex};
        if (const auto it = d->swapchains.find(U64(swapchain)); it != d->swapchains.end()) {
            fg = it->second;
            d->swapchains.erase(it);
        }
    }
    if (fg) {
        Log("swapchain: %ux%u destroyed (device %u, thread %s)", fg->extent.width, fg->extent.height, d->index,
            ThreadName());
        DestroyFg(*d, fg);
    }
    d->DestroySwapchainKHR(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL HookQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    IN_HOOK("QueuePresent");
    Device* d = nullptr;
    uint32_t family = 0;
    {
        std::lock_guard lock{g_tables_mutex};
        if (const auto it = g_queues.find(U64(queue)); it != g_queues.end()) {
            d = it->second.first;
            family = it->second.second;
        } else if (const auto device = g_devices.find(Key(queue)); device != g_devices.end()) {
            d = device->second;
        }
    }
    Fg* fg = nullptr;
    if (info->swapchainCount == 1) {
        std::lock_guard lock{d->fg_mutex};
        if (const auto it = d->swapchains.find(U64(info->pSwapchains[0])); it != d->swapchains.end()) {
            fg = it->second;
        }
    }
    TracePresent(d->index, queue);
    if (!fg) {
        return d->QueuePresentKHR(queue, info);
    }
    if (!fg->presented) {
        fg->presented = true;
        Log("swapchain: first present of %ux%u (device %u, thread %s)", fg->extent.width, fg->extent.height, d->index,
            ThreadName());
        LogPresentChain(*info);
    }
    ApplyBoost(*d, *fg);
    ApplyPriority();
    const double start = Now();
    VkResult result = VK_SUCCESS;
    Stage(1);
    fg->paced = 0.0;
    fg->hook_waited = false;
    ReadPanelRequest(*fg, start);
    PanelNoteFrame(*fg, start);
    const bool passed = !Generate(*d, *fg, queue, family, *info, result);
    if (passed) {
        NoteOutput(*fg);
        Stage(8);
        if (!PassWithPanel(*d, *fg, queue, family, *info, result)) {
            result = d->QueuePresentKHR(queue, info);
        }
        ++fg->passed_frames;
    }
    if (info->pResults) {
        info->pResults[0] = result;
    }
    Stage(0);
    const double end = Now();
    GateLeave();
    fg->hook_seconds += end - start;
    if (!passed) {
        fg->hook_last = end - start;
    }
    if (!passed && fg->hook_waited) {
        fg->tail_ring[fg->tail_count++ % 32] = static_cast<float>(end - fg->last_out);
        fg->tail_seconds = Level(fg->tail_ring, fg->tail_count);
    }
    if (!fg->events.empty()) {
        Event& event = fg->events[fg->event_count++ % EVENT_COUNT];
        event.hook_ms = static_cast<float>((end - start) * 1000.0);
        event.paced_ms = static_cast<float>(fg->paced * 1000.0);
        event.flags |= passed ? Event::PASSED : 0u;
    }
    NoteSent(*fg, *info, passed ? end : fg->real_sent, passed || fg->too_fast ? 1 : fg->generated + 1);
    TakeOwnTimings(*d, *fg);
    TakeShown(*fg);
    Statistics(*d, *fg, end);
    return result;
}

// ----------------------------------------------------------------------------- instance, device

VKAPI_ATTR VkResult VKAPI_CALL HookCreateInstance(const VkInstanceCreateInfo* info,
                                                  const VkAllocationCallbacks* allocator, VkInstance* instance) {
    IN_HOOK("CreateInstance");
    static const bool tested = [] {
        if (EnvFloat("SC_FG_TEST_LOCKS", 0.0f) != 0.0f) {
            TestLocks();
        }
        return true;
    }();
    (void)tested;
    auto* link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO)) {
        link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    }
    if (!link) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS) {
        return result;
    }
    auto* in = new Instance;
    in->handle = *instance;
    in->gipa = gipa;
    {
        const VkApplicationInfo* app = info->pApplicationInfo;
        std::snprintf(in->engine, sizeof in->engine, "%s", app && app->pEngineName ? app->pEngineName : "?");
        Log("instance created: application \"%s\", engine \"%s\", %u extensions (thread %s)",
            app && app->pApplicationName ? app->pApplicationName : "?", in->engine, info->enabledExtensionCount,
            ThreadName());
    }
#define LOAD(name) in->name = reinterpret_cast<PFN_vk##name>(gipa(*instance, "vk" #name))
    LOAD(DestroyInstance);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(GetPhysicalDeviceMemoryProperties2);
    LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceProperties2);
    LOAD(GetPhysicalDeviceFeatures);
    LOAD(GetPhysicalDeviceFeatures2);
    LOAD(EnumerateDeviceExtensionProperties);
#undef LOAD
    std::lock_guard lock{g_tables_mutex};
    g_instances[Key(*instance)] = in;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL HookDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    IN_HOOK("DestroyInstance");
    if (!instance) {
        return;
    }
    Instance* in = nullptr;
    {
        std::lock_guard lock{g_tables_mutex};
        if (const auto it = g_instances.find(Key(instance)); it != g_instances.end()) {
            in = it->second;
            g_instances.erase(it);
        }
    }
    if (in) {
        in->DestroyInstance(instance, allocator);
        delete in;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                                const VkAllocationCallbacks* allocator, VkDevice* device) {
    IN_HOOK("CreateDevice");
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkSetDeviceLoaderData set_loader_data{};
    for (auto* link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext)); link;
         link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext))) {
        if (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            continue;
        }
        if (link->function == VK_LAYER_LINK_INFO && !gdpa) {
            gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
            link->u.pLayerInfo = link->u.pLayerInfo->pNext;
        } else if (link->function == VK_LOADER_DATA_CALLBACK) {
            set_loader_data = link->u.pfnSetDeviceLoaderData;
        }
    }
    if (!gdpa) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Instance* in = GetInstance(Key(physical));
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(in ? in->handle : VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create) {
        create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    }
    if (!create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS) {
        return result;
    }
    auto* d = new Device;
    d->handle = *device;
    d->physical = physical;
    d->instance = in;
    d->gdpa = gdpa;
    d->set_loader_data = set_loader_data;
#define X(name) d->name = reinterpret_cast<PFN_vk##name>(gdpa(*device, "vk" #name));
    DEVICE_FUNCTIONS(X)
#undef X
#define X(name)                                                                                    \
    d->Trace##name = reinterpret_cast<PFN_vk##name>(gdpa(*device, "vk" #name));                    \
    if (!d->Trace##name) {                                                                         \
        d->Trace##name = reinterpret_cast<PFN_vk##name>(gdpa(*device, "vk" #name "KHR"));          \
    }
    TRACE_FUNCTIONS(X)
#undef X
    static std::atomic<uint32_t> device_count{};
    d->index = device_count.fetch_add(1);
    if (in && in->GetPhysicalDeviceMemoryProperties) {
        in->GetPhysicalDeviceMemoryProperties(physical, &d->memory);
    }
    bool view_handle = false, binary_import = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        view_handle |= !std::strcmp(info->ppEnabledExtensionNames[i], "VK_NVX_image_view_handle");
        binary_import |= !std::strcmp(info->ppEnabledExtensionNames[i], "VK_NVX_binary_import");
    }
    d->nvx = view_handle && binary_import && in;
    Log("device created: %p, %u extensions, DLSS path %s (device %u, engine \"%s\", thread %s)", static_cast<void*>(d),
        info->enabledExtensionCount, d->nvx ? "available" : "absent", d->index, in ? in->engine : "?", ThreadName());
    {
        // (which extensions: tells the game's renderer from what else makes a device in its process)
        std::string names;
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
            const char* name = info->ppEnabledExtensionNames[i];
            names += std::strncmp(name, "VK_", 3) != 0 ? name : name + 3;
            names += ' ';
            if (names.size() > 160 || i + 1 == info->enabledExtensionCount) {
                Log("device created: device %u has %s", d->index, names.c_str());
                names.clear();
            }
        }
    }
    std::lock_guard lock{g_tables_mutex};
    g_devices[Key(*device)] = d;
    for (DeviceSlot& slot : g_device_slots) {
        if (!slot.key.load(std::memory_order_relaxed)) {
            slot.device.store(d, std::memory_order_release);
            slot.key.store(Key(*device), std::memory_order_release);
            break;
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL HookDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    IN_HOOK("DestroyDevice");
    if (!device) {
        return;
    }
    if (g_selftest) {
        if (Device* reader = GetDevice(Key(device))) {
            SelftestImport(*reader);
        }
    }
    Device* d = nullptr;
    {
        std::lock_guard lock{g_tables_mutex};
        if (const auto it = g_devices.find(Key(device)); it != g_devices.end()) {
            d = it->second;
            g_devices.erase(it);
        }
        for (DeviceSlot& slot : g_device_slots) {
            if (slot.key.load(std::memory_order_relaxed) == Key(device)) {
                slot.key.store(nullptr, std::memory_order_release);
                slot.device.store(nullptr, std::memory_order_release);
            }
        }
        std::erase_if(g_queues, [d](const auto& entry) { return entry.second.first == d; });
        std::erase_if(g_command_buffers, [d](const auto& entry) { return entry.second == d; });
    }
    if (!d) {
        return;
    }
    StopDoneWatch(*d);
    Log("device destroyed: device %u (thread %s)", d->index, ThreadName());
    {
        std::lock_guard global{g_input_mutex};
        if (g_input_device == d) {
            g_input_device = nullptr;
            g_input_device_hint = nullptr;
        }
    }
    for (auto& [handle, fg] : d->swapchains) {
        DestroyFg(*d, fg);
    }
    Inputs& in = d->inputs;
    if (g_selftest && in.self_vectors.data) {
        char path[600];
        std::snprintf(path, sizeof path, "%s/selftest-%ux%u.bin", Dir(), in.copy_width, in.copy_height);
        if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(in.self_vectors.data, 1, in.self_vectors.size, file);
            std::fwrite(in.self_depth.data, 1, in.self_depth.size, file);
            std::fclose(file);
        }
    }
    for (HostBuffer* buffer : {&in.self_vectors, &in.self_depth}) {
        if (buffer->buffer) {
            d->DestroyBuffer(d->handle, buffer->buffer, nullptr);
            d->FreeMemory(d->handle, buffer->memory, nullptr);
        }
    }
    for (uint32_t i = 0; i < Inputs::SLOTS; ++i) {
        DestroyOwnImage(*d, in.vector_copy[i]);
        DestroyOwnImage(*d, in.depth_copy[i]);
        DestroyOwnImage(*d, d->imports.vectors[i]);
        DestroyOwnImage(*d, d->imports.depth[i]);
        DestroyOwnImage(*d, in.hud_copy[i]);
        DestroyOwnImage(*d, d->imports.hud[i]);
        DestroyOwnImage(*d, in.scene_copy[i]);
        DestroyOwnImage(*d, d->imports.scene[i]);
    }
    for (std::vector<Image>* list : {&in.retired, &d->imports.retired}) {
        for (Image& image : *list) {
            DestroyOwnImage(*d, image);
        }
    }
    if (in.depth_hop) {
        d->DestroyBuffer(d->handle, in.depth_hop, nullptr);
        d->FreeMemory(d->handle, in.depth_hop_memory, nullptr);
    }
    for (VkFence fence : d->mark_fences) {
        if (fence) {
            d->WaitForFences(d->handle, 1, &fence, VK_TRUE, 1'000'000'000ull);
            d->DestroyFence(d->handle, fence, nullptr);
        }
    }
    if (g_ffx_device == d) {
        g_ffx_device = nullptr;
    }
    const PFN_vkDestroyDevice destroy = d->DestroyDevice;
    delete d;
    destroy(device, allocator);
}


// ----------------------------------------------------------------------------- trace
// What the game does in a frame, written to trace-<pid>.log for a moment after the control file
// TRACE appears: render passes with their targets and draw counts, copies between images, compute
// work, submits and presents. Nothing is changed; every hook passes its call on.

Lock g_trace_mutex{LOCK_TRACE};
FILE* g_trace_file{};
struct CbTrace {
    std::string pass;
    uint32_t draws{}, dispatches{}, kernels{};
    bool in_pass{};
};
std::unordered_map<uint64_t, CbTrace> g_cb_trace;
std::unordered_map<uint64_t, uint32_t> g_short_ids;
std::unordered_map<uint64_t, std::vector<std::pair<VkFormat, VkAttachmentLoadOp>>> g_render_passes;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_framebuffers;

bool Tracing() {
    return g_trace && Now() < g_trace_until.load();
}

// The functions below are called with g_trace_mutex held.
void TraceLine(const char* format, ...) {
    if (!g_trace_file) {
        char path[600];
        std::snprintf(path, sizeof path, "%s/trace-%d.log", Dir(), getpid());
        g_trace_file = std::fopen(path, "a");
        if (!g_trace_file) {
            return;
        }
    }
    std::fprintf(g_trace_file, "%10.4f t%-7ld ", Now(), syscall(SYS_gettid));
    va_list args;
    va_start(args, format);
    std::vfprintf(g_trace_file, format, args);
    va_end(args);
    std::fputc('\n', g_trace_file);
    std::fflush(g_trace_file);
}

uint32_t ShortId(uint64_t handle) {
    const auto [it, added] = g_short_ids.try_emplace(handle, static_cast<uint32_t>(g_short_ids.size()));
    return it->second;
}

std::string DescribeImage(uint64_t image) {
    ImageRecord record;
    bool known = false;
    {
        std::lock_guard lock{g_records_mutex};
        if (const auto it = g_images.find(image); it != g_images.end()) {
            record = it->second;
            known = true;
        }
    }
    char text[96];
    if (known) {
        std::snprintf(text, sizeof text, "img%u(f%d %ux%u%s%s)", ShortId(image), record.format, record.width,
                      record.height, record.shared ? " SHARED" : "", record.swapchain ? " SWAPCHAIN" : "");
    } else {
        std::snprintf(text, sizeof text, "img%u(?)", ShortId(image));
    }
    return text;
}

std::string DescribeView(uint64_t view) {
    uint64_t image = 0;
    {
        std::lock_guard lock{g_records_mutex};
        if (const auto it = g_views.find(view); it != g_views.end()) {
            image = it->second.image;
        }
    }
    return image ? DescribeImage(image) : std::string("view?");
}

const char* LoadName(VkAttachmentLoadOp op) {
    return op == VK_ATTACHMENT_LOAD_OP_LOAD ? "load" : op == VK_ATTACHMENT_LOAD_OP_CLEAR ? "clear" : "dontcare";
}

void TraceFlush(VkCommandBuffer cmd, CbTrace& cb) {
    if (cb.dispatches || cb.kernels) {
        TraceLine("cb%u compute dispatches=%u dlss_kernels=%u", ShortId(U64(cmd)), cb.dispatches, cb.kernels);
        cb.dispatches = cb.kernels = 0;
    }
}

/// kind: 0 draw, 1 dispatch, 2 DLSS kernel
void TraceCount(VkCommandBuffer cmd, int kind) {
    if (!Tracing()) {
        return;
    }
    std::lock_guard lock{g_trace_mutex};
    CbTrace& cb = g_cb_trace[U64(cmd)];
    ++(kind == 0 ? cb.draws : kind == 1 ? cb.dispatches : cb.kernels);
}

void TracePresent(uint32_t device, VkQueue queue) {
    if (Tracing()) {
        std::lock_guard lock{g_trace_mutex};
        TraceLine("PRESENT device%u queue%u", device, ShortId(U64(queue)));
    }
}

void TraceBeginPass(VkCommandBuffer cmd, std::string pass) {
    std::lock_guard lock{g_trace_mutex};
    CbTrace& cb = g_cb_trace[U64(cmd)];
    TraceFlush(cmd, cb);
    cb.pass = std::move(pass);
    cb.draws = 0;
    cb.in_pass = true;
}

void TraceEndPass(VkCommandBuffer cmd) {
    std::lock_guard lock{g_trace_mutex};
    CbTrace& cb = g_cb_trace[U64(cmd)];
    if (cb.in_pass) {
        TraceLine("cb%u PASS %s draws=%u", ShortId(U64(cmd)), cb.pass.c_str(), cb.draws);
    }
    cb.in_pass = false;
    cb.draws = 0;
}

void TraceTransfer(VkCommandBuffer cmd, const char* what, uint64_t source, uint64_t destination) {
    if (!Tracing()) {
        return;
    }
    std::lock_guard lock{g_trace_mutex};
    TraceFlush(cmd, g_cb_trace[U64(cmd)]);
    TraceLine("cb%u %s %s -> %s", ShortId(U64(cmd)), what, source ? DescribeImage(source).c_str() : "buffer",
              destination ? DescribeImage(destination).c_str() : "buffer");
}

// One-shot scan (control file HUDSCAN, trace launches only): the colour target of every pass that
// draws into a display-size RGBA16F image is saved before and after the pass, to find the pass
// that draws the HUD.
struct ScanEntry {
    HostBuffer before, after;
    uint32_t width{}, height{};
    uint64_t cmd{};
    long thread{};
    double time{};
    bool depth{};
    bool ended{};
};
constexpr size_t SCAN_PASSES = 32;
Lock g_scan_mutex{LOCK_SCAN};
std::vector<ScanEntry> g_scan;
std::unordered_map<uint64_t, size_t> g_scan_open; ///< command buffer -> entry of its open pass
std::atomic<bool> g_scan_armed{};
Device* g_scan_device{};
double g_scan_last{};

void ScanBegin(Device& d, VkCommandBuffer cmd, uint64_t view, VkAttachmentLoadOp load, bool depth) {
    if (!g_scan_armed.load() || load != VK_ATTACHMENT_LOAD_OP_LOAD) {
        return;
    }
    ImageRecord record;
    uint64_t image = 0;
    {
        std::lock_guard lock{g_records_mutex};
        const auto view_it = g_views.find(view);
        if (view_it == g_views.end() || view_it->second.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
            return;
        }
        image = view_it->second.image;
        const auto image_it = g_images.find(image);
        if (image_it == g_images.end()) {
            return;
        }
        record = image_it->second;
    }
    if (record.width < 1280 || record.width <= d.inputs.width) {
        return; // not display size
    }
    std::lock_guard lock{g_scan_mutex};
    if (g_scan.size() >= SCAN_PASSES) {
        g_scan_armed = false;
        return;
    }
    ScanEntry entry;
    const VkDeviceSize size = static_cast<VkDeviceSize>(record.width) * record.height * 8;
    if (!CreateBufferWithMemory(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, entry.before.buffer, entry.before.memory,
                                &entry.before.data) ||
        !CreateBufferWithMemory(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, entry.after.buffer, entry.after.memory,
                                &entry.after.data)) {
        g_scan_armed = false;
        return;
    }
    entry.width = record.width;
    entry.height = record.height;
    entry.cmd = U64(cmd);
    entry.thread = syscall(SYS_gettid);
    entry.time = Now();
    entry.depth = depth;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {record.width, record.height, 1};
    d.CmdCopyImageToBuffer(cmd, reinterpret_cast<VkImage>(static_cast<uintptr_t>(image)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.before.buffer, 1, &region);
    g_scan_open[U64(cmd)] = g_scan.size();
    g_scan.push_back(entry);
    g_scan_device = &d;
    g_scan_last = Now();
    // the image for the copy after the pass
    g_scan.back().before.size = image;
}

void ScanEnd(Device& d, VkCommandBuffer cmd) {
    std::lock_guard lock{g_scan_mutex};
    const auto it = g_scan_open.find(U64(cmd));
    if (it == g_scan_open.end()) {
        return;
    }
    ScanEntry& entry = g_scan[it->second];
    g_scan_open.erase(it);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {entry.width, entry.height, 1};
    d.CmdCopyImageToBuffer(cmd, reinterpret_cast<VkImage>(static_cast<uintptr_t>(entry.before.size)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.after.buffer, 1, &region);
    entry.ended = true;
    g_scan_last = Now();
}

/// Called once a second from the presenting side: writes a finished scan to files.
void ScanWrite() {
    std::lock_guard lock{g_scan_mutex};
    if (g_scan.empty() || Now() - g_scan_last < 2.0) {
        return;
    }
    g_scan_armed = false;
    Device& d = *g_scan_device;
    for (size_t i = 0; i < g_scan.size(); ++i) {
        ScanEntry& entry = g_scan[i];
        const size_t bytes = static_cast<size_t>(entry.width) * entry.height * 8;
        for (int k = 0; k < 2; ++k) {
            HostBuffer& buffer = k ? entry.after : entry.before;
            if (!k || entry.ended) {
                char path[600];
                std::snprintf(path, sizeof path, "%s/scan-%d-%02zu-%s-%ux%u.bin", Dir(), getpid(), i,
                              k ? "after" : "before", entry.width, entry.height);
                if (FILE* file = std::fopen(path, "wb")) {
                    std::fwrite(buffer.data, 1, bytes, file);
                    std::fclose(file);
                }
            }
            d.DestroyBuffer(d.handle, buffer.buffer, nullptr);
            d.FreeMemory(d.handle, buffer.memory, nullptr);
        }
        Log("scan %02zu: %ux%u depth=%d thread=%ld time=%.4f ended=%d", i, entry.width, entry.height, entry.depth,
            entry.thread, entry.time, entry.ended);
    }
    g_scan.clear();
    g_scan_open.clear();
}

struct HudPass {
    VkImage image{};
    uint32_t width{}, height{};
};
Lock g_hud_pass_mutex{LOCK_HUD_PASS};
std::unordered_map<uint64_t, HudPass> g_hud_passes; ///< command buffer -> the HUD layer its open pass draws into
/// The last capture's layer and the frame copy that went with it (under g_hud_pass_mutex). The game
/// does not always record the two into one command buffer: the same copy without a HUD pass before
/// it in its own is the same thing, with that layer (see NoteFrameCopy).
struct HudCopy {
    VkImage layer{};
    uint64_t source{}, destination{};
    uint32_t width{}, height{};
} g_hud_copy;

/// A pass begins. The HUD layer is a display-size RGBA16F target drawn into without depth; the last
/// such pass of a command buffer before the frame is copied aside is the finished layer.
void NotePassTarget(VkCommandBuffer cmd, uint64_t view, VkAttachmentLoadOp load, size_t attachment_count) {
    if (attachment_count != 1 || load != VK_ATTACHMENT_LOAD_OP_LOAD) {
        return;
    }
    ImageRecord record;
    uint64_t image = 0;
    {
        std::lock_guard lock{g_records_mutex};
        const auto view_it = g_views.find(view);
        if (view_it == g_views.end() || view_it->second.format != HUD_FORMAT) {
            return;
        }
        image = view_it->second.image;
        const auto image_it = g_images.find(image);
        if (image_it == g_images.end()) {
            return;
        }
        record = image_it->second;
    }
    if (record.width < 1280 || record.height < 720) {
        return;
    }
    ++g_hud_pass_count;
    std::lock_guard lock{g_hud_pass_mutex};
    g_hud_passes[U64(cmd)] = {reinterpret_cast<VkImage>(static_cast<uintptr_t>(image)), record.width, record.height};
}

/// What is known about a command buffer's open HUD pass holds for one recording of it. A command
/// buffer that is begun again (or freed) starts with none: a pass whose frame copy never came (in
/// the game's menus, with the HUD hidden, in some cameras) would otherwise wait for a copy in some
/// later frame, and hand that frame an image the game may long have destroyed.
void ForgetHudPass(VkCommandBuffer cmd) {
    std::lock_guard lock{g_hud_pass_mutex};
    if (!g_hud_passes.empty()) {
        g_hud_passes.erase(U64(cmd));
    }
}

std::atomic<uint32_t> g_stale_passes{};

/// The game destroys an image: nothing kept here may name it any longer. (Copies are recorded
/// from what is kept: from a destroyed image that is a fault inside the driver, in the middle
/// of a call of the game's.)
void ForgetImage(Device& d, VkImage image) {
    {
        std::lock_guard lock{g_hud_pass_mutex};
        if (!g_hud_passes.empty()) {
            std::erase_if(g_hud_passes, [image](const auto& entry) { return entry.second.image == image; });
        }
        if (g_hud_copy.layer == image || g_hud_copy.source == U64(image) || g_hud_copy.destination == U64(image)) {
            g_hud_copy = {};
        }
    }
    Inputs& in = d.inputs;
    std::lock_guard lock{in.mutex};
    if (in.depth == U64(image)) {
        in.depth = 0;
    }
    for (Candidate& candidate : in.vectors) {
        if (candidate.image == U64(image)) {
            candidate.image = 0;
        }
    }
}

/// A pass has ended (the end is already recorded).
void PassEnded(Device& d, VkCommandBuffer cmd) {
    if (g_trace) {
        ScanEnd(d, cmd);
    }
}

/// The game copies one RGBA8 image to another. When both are display size and the command buffer
/// has just drawn the HUD layer, this is the frame being set aside before the HUD goes over it.
void NoteFrameCopy(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout, VkImage destination) {
    if (!g_hud_capture) {
        return;
    }
    HudPass pass;
    bool stale = false, cross = false;
    {
        std::lock_guard lock{g_hud_pass_mutex};
        const auto it = g_hud_passes.find(U64(cmd));
        if (it != g_hud_passes.end()) {
            pass = it->second;
        } else if (g_live_cross.load() != 0.0f && g_hud_copy.layer && g_hud_copy.source == U64(source) &&
                   g_hud_copy.destination == U64(destination)) {
            // No HUD pass in this command buffer, but the frame copy that follows it: the pass went
            // into another one, which runs before this.
            pass = {g_hud_copy.layer, g_hud_copy.width, g_hud_copy.height};
            cross = true;
        } else {
            return;
        }
    }
    {
        std::lock_guard lock{g_records_mutex};
        const auto from = g_images.find(U64(source)), to = g_images.find(U64(destination));
        if (from == g_images.end() || to == g_images.end()) {
            return;
        }
        for (const ImageRecord* record : {&from->second, &to->second}) {
            if (record->format != SCENE_FORMAT || record->width != pass.width || record->height != pass.height ||
                record->shared || record->swapchain) {
                return;
            }
        }
        // (The layer itself must still be what the pass drew into.)
        const auto layer = g_images.find(U64(pass.image));
        stale = layer == g_images.end() || layer->second.format != HUD_FORMAT || layer->second.width != pass.width ||
                layer->second.height != pass.height;
    }
    {
        std::lock_guard lock{g_hud_pass_mutex};
        g_hud_passes.erase(U64(cmd));
    }
    if (stale) {
        if (g_stale_passes.fetch_add(1) < 8) {
            Log("fg: a frame copy met a HUD pass whose layer no longer exists: not captured");
        }
        return;
    }
    ++g_frame_copy_count;
    Device& d = *DeviceOfCommandBuffer(cmd);
    // (A capture that is not on its way yet may be this frame's, and a second one would put the
    // frames and their captures out of step.)
    if (cross && d.inputs.hud_serial.load() != d.inputs.hud_submitted.load()) {
        return;
    }
    if (pass.width != d.inputs.width || pass.height != d.inputs.height) {
        CopyHud(d, cmd, pass.image, source, source_layout, pass.width, pass.height);
        if (cross) {
            g_cross_captures.fetch_add(1);
        } else {
            std::lock_guard lock{g_hud_pass_mutex};
            g_hud_copy = {pass.image, U64(source), U64(destination), pass.width, pass.height};
        }
    }
}

void PassAttachments(const VkRenderPassBeginInfo* begin, std::vector<uint64_t>& views,
                     std::vector<std::pair<VkFormat, VkAttachmentLoadOp>>& attachments);

void ScanRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* begin) {
    if (!g_scan_armed.load()) {
        return;
    }
    std::vector<uint64_t> views;
    std::vector<std::pair<VkFormat, VkAttachmentLoadOp>> attachments;
    PassAttachments(begin, views, attachments);
    if (!views.empty() && !attachments.empty()) {
        ScanBegin(*DeviceOfCommandBuffer(cmd), cmd, views[0], attachments[0].second, views.size() > 1);
    }
}

void NoteRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* begin) {
    std::vector<uint64_t> views;
    std::vector<std::pair<VkFormat, VkAttachmentLoadOp>> attachments;
    PassAttachments(begin, views, attachments);
    if (views.size() == 1 && attachments.size() == 1) {
        NotePassTarget(cmd, views[0], attachments[0].second, 1);
    }
}

void PassAttachments(const VkRenderPassBeginInfo* begin, std::vector<uint64_t>& views,
                     std::vector<std::pair<VkFormat, VkAttachmentLoadOp>>& attachments) {
    {
        std::lock_guard lock{g_trace_mutex};
        if (const auto it = g_framebuffers.find(U64(begin->framebuffer)); it != g_framebuffers.end()) {
            views = it->second;
        }
        if (const auto it = g_render_passes.find(U64(begin->renderPass)); it != g_render_passes.end()) {
            attachments = it->second;
        }
    }
    for (auto* next = static_cast<const VkBaseInStructure*>(begin->pNext); next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const auto* info = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);
            views.clear();
            for (uint32_t i = 0; i < info->attachmentCount; ++i) {
                views.push_back(U64(info->pAttachments[i]));
            }
        }
    }
}

std::string PassOfRenderPass(const VkRenderPassBeginInfo* begin) {
    std::vector<uint64_t> views;
    std::vector<std::pair<VkFormat, VkAttachmentLoadOp>> attachments;
    PassAttachments(begin, views, attachments);
    std::string pass = "[";
    std::lock_guard lock{g_trace_mutex};
    for (size_t i = 0; i < views.size(); ++i) {
        pass += DescribeView(views[i]);
        pass += i < attachments.size() ? std::string(" ") + LoadName(attachments[i].second) : std::string();
        pass += i + 1 < views.size() ? ", " : "";
    }
    return pass + "]";
}

VKAPI_ATTR VkResult VKAPI_CALL TraceCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
                                                     const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    IN_HOOK("RenderPassNew");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->TraceCreateRenderPass(device, info, allocator, pass);
    if (result == VK_SUCCESS) {
        std::lock_guard lock{g_trace_mutex};
        auto& list = g_render_passes[U64(*pass)];
        list.clear();
        for (uint32_t i = 0; i < info->attachmentCount; ++i) {
            list.emplace_back(info->pAttachments[i].format, info->pAttachments[i].loadOp);
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
                                                      const VkAllocationCallbacks* allocator, VkRenderPass* pass) {
    IN_HOOK("RenderPassNew2");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->TraceCreateRenderPass2(device, info, allocator, pass);
    if (result == VK_SUCCESS) {
        std::lock_guard lock{g_trace_mutex};
        auto& list = g_render_passes[U64(*pass)];
        list.clear();
        for (uint32_t i = 0; i < info->attachmentCount; ++i) {
            list.emplace_back(info->pAttachments[i].format, info->pAttachments[i].loadOp);
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                                      const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    IN_HOOK("FramebufferNew");
    Device* d = GetDevice(Key(device));
    const VkResult result = d->TraceCreateFramebuffer(device, info, allocator, framebuffer);
    if (result == VK_SUCCESS) {
        std::lock_guard lock{g_trace_mutex};
        auto& list = g_framebuffers[U64(*framebuffer)];
        list.clear();
        if (!(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
            for (uint32_t i = 0; i < info->attachmentCount; ++i) {
                list.push_back(U64(info->pAttachments[i]));
            }
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL TraceCmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* begin,
                                                   VkSubpassContents contents) {
    IN_HOOK("BeginRenderPass");
    if (Tracing()) {
        TraceBeginPass(cmd, PassOfRenderPass(begin));
    }
    NoteRenderPass(cmd, begin);
    ScanRenderPass(cmd, begin);
    DeviceOfCommandBuffer(cmd)->TraceCmdBeginRenderPass(cmd, begin, contents);
}

VKAPI_ATTR void VKAPI_CALL TraceCmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* begin,
                                                    const VkSubpassBeginInfo* subpass) {
    IN_HOOK("BeginRenderPass2");
    if (Tracing()) {
        TraceBeginPass(cmd, PassOfRenderPass(begin));
    }
    NoteRenderPass(cmd, begin);
    ScanRenderPass(cmd, begin);
    DeviceOfCommandBuffer(cmd)->TraceCmdBeginRenderPass2(cmd, begin, subpass);
}

VKAPI_ATTR void VKAPI_CALL TraceCmdEndRenderPass(VkCommandBuffer cmd) {
    IN_HOOK("EndRenderPass");
    if (g_trace) {
        TraceEndPass(cmd);
    }
    Device* d = DeviceOfCommandBuffer(cmd);
    d->TraceCmdEndRenderPass(cmd);
    PassEnded(*d, cmd);
}

VKAPI_ATTR void VKAPI_CALL TraceCmdEndRenderPass2(VkCommandBuffer cmd, const VkSubpassEndInfo* end) {
    IN_HOOK("EndRenderPass2");
    if (g_trace) {
        TraceEndPass(cmd);
    }
    Device* d = DeviceOfCommandBuffer(cmd);
    d->TraceCmdEndRenderPass2(cmd, end);
    PassEnded(*d, cmd);
}

VKAPI_ATTR void VKAPI_CALL TraceCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
    IN_HOOK("BeginRendering");
    if (Tracing()) {
        std::string pass = "[";
        {
            std::lock_guard lock{g_trace_mutex};
            for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
                const VkRenderingAttachmentInfo& attachment = info->pColorAttachments[i];
                pass += attachment.imageView ? DescribeView(U64(attachment.imageView)) + " " + LoadName(attachment.loadOp)
                                             : std::string("none");
                pass += ", ";
            }
            if (info->pDepthAttachment && info->pDepthAttachment->imageView) {
                pass += "depth " + DescribeView(U64(info->pDepthAttachment->imageView)) + " " +
                        LoadName(info->pDepthAttachment->loadOp);
            }
        }
        TraceBeginPass(cmd, pass + "]");
    }
    if (info->colorAttachmentCount == 1 && info->pColorAttachments[0].imageView &&
        !(info->pDepthAttachment && info->pDepthAttachment->imageView)) {
        NotePassTarget(cmd, U64(info->pColorAttachments[0].imageView), info->pColorAttachments[0].loadOp, 1);
    }
    if (g_scan_armed.load() && info->colorAttachmentCount >= 1 && info->pColorAttachments[0].imageView) {
        ScanBegin(*DeviceOfCommandBuffer(cmd), cmd, U64(info->pColorAttachments[0].imageView),
                  info->pColorAttachments[0].loadOp, info->pDepthAttachment && info->pDepthAttachment->imageView);
    }
    DeviceOfCommandBuffer(cmd)->TraceCmdBeginRendering(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL TraceCmdEndRendering(VkCommandBuffer cmd) {
    IN_HOOK("EndRendering");
    if (g_trace) {
        TraceEndPass(cmd);
    }
    Device* d = DeviceOfCommandBuffer(cmd);
    d->TraceCmdEndRendering(cmd);
    PassEnded(*d, cmd);
}

VKAPI_ATTR void VKAPI_CALL TraceCmdDraw(VkCommandBuffer cmd, uint32_t a, uint32_t b, uint32_t c, uint32_t e) {
    TraceCount(cmd, 0);
    DeviceOfCommandBuffer(cmd)->TraceCmdDraw(cmd, a, b, c, e);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDrawIndexed(VkCommandBuffer cmd, uint32_t a, uint32_t b, uint32_t c, int32_t e,
                                               uint32_t f) {
    TraceCount(cmd, 0);
    DeviceOfCommandBuffer(cmd)->TraceCmdDrawIndexed(cmd, a, b, c, e, f);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t count,
                                                uint32_t stride) {
    TraceCount(cmd, 0);
    DeviceOfCommandBuffer(cmd)->TraceCmdDrawIndirect(cmd, buffer, offset, count, stride);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                       uint32_t count, uint32_t stride) {
    TraceCount(cmd, 0);
    DeviceOfCommandBuffer(cmd)->TraceCmdDrawIndexedIndirect(cmd, buffer, offset, count, stride);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDrawIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                     VkBuffer count_buffer, VkDeviceSize count_offset, uint32_t max,
                                                     uint32_t stride) {
    TraceCount(cmd, 0);
    DeviceOfCommandBuffer(cmd)->TraceCmdDrawIndirectCount(cmd, buffer, offset, count_buffer, count_offset, max, stride);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                            VkBuffer count_buffer, VkDeviceSize count_offset, uint32_t max,
                                                            uint32_t stride) {
    TraceCount(cmd, 0);
    DeviceOfCommandBuffer(cmd)->TraceCmdDrawIndexedIndirectCount(cmd, buffer, offset, count_buffer, count_offset, max,
                                                                 stride);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    TraceCount(cmd, 1);
    DeviceOfCommandBuffer(cmd)->CmdDispatch(cmd, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset) {
    TraceCount(cmd, 1);
    DeviceOfCommandBuffer(cmd)->TraceCmdDispatchIndirect(cmd, buffer, offset);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdCopyImage(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout,
                                             VkImage destination, VkImageLayout destination_layout, uint32_t count,
                                             const VkImageCopy* regions) {
    IN_HOOK("CopyImage");
    TraceTransfer(cmd, "COPY", U64(source), U64(destination));
    Device* d = DeviceOfCommandBuffer(cmd);
    d->CmdCopyImage(cmd, source, source_layout, destination, destination_layout, count, regions);
    NoteFrameCopy(cmd, source, source_layout, destination);
    NoteHandover(*d, cmd, source, destination);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdCopyImage2(VkCommandBuffer cmd, const VkCopyImageInfo2* info) {
    IN_HOOK("CopyImage2");
    TraceTransfer(cmd, "COPY", U64(info->srcImage), U64(info->dstImage));
    Device* d = DeviceOfCommandBuffer(cmd);
    d->TraceCmdCopyImage2(cmd, info);
    NoteFrameCopy(cmd, info->srcImage, info->srcImageLayout, info->dstImage);
    NoteHandover(*d, cmd, info->srcImage, info->dstImage);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdBlitImage(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout,
                                             VkImage destination, VkImageLayout destination_layout, uint32_t count,
                                             const VkImageBlit* regions, VkFilter filter) {
    TraceTransfer(cmd, "BLIT", U64(source), U64(destination));
    DeviceOfCommandBuffer(cmd)->TraceCmdBlitImage(cmd, source, source_layout, destination, destination_layout, count,
                                                  regions, filter);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdBlitImage2(VkCommandBuffer cmd, const VkBlitImageInfo2* info) {
    TraceTransfer(cmd, "BLIT", U64(info->srcImage), U64(info->dstImage));
    DeviceOfCommandBuffer(cmd)->TraceCmdBlitImage2(cmd, info);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdResolveImage(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout,
                                                VkImage destination, VkImageLayout destination_layout, uint32_t count,
                                                const VkImageResolve* regions) {
    TraceTransfer(cmd, "RESOLVE", U64(source), U64(destination));
    DeviceOfCommandBuffer(cmd)->TraceCmdResolveImage(cmd, source, source_layout, destination, destination_layout, count,
                                                     regions);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage source, VkImageLayout layout,
                                                     VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions) {
    TraceTransfer(cmd, "READBACK", U64(source), 0);
    DeviceOfCommandBuffer(cmd)->CmdCopyImageToBuffer(cmd, source, layout, buffer, count, regions);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage destination,
                                                     VkImageLayout layout, uint32_t count,
                                                     const VkBufferImageCopy* regions) {
    TraceTransfer(cmd, "UPLOAD", 0, U64(destination));
    DeviceOfCommandBuffer(cmd)->CmdCopyBufferToImage(cmd, buffer, destination, layout, count, regions);
}
VKAPI_ATTR void VKAPI_CALL TraceCmdExecuteCommands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer* buffers) {
    if (Tracing()) {
        std::lock_guard lock{g_trace_mutex};
        std::string list;
        for (uint32_t i = 0; i < count; ++i) {
            TraceFlush(buffers[i], g_cb_trace[U64(buffers[i])]);
            list += " cb" + std::to_string(ShortId(U64(buffers[i])));
        }
        TraceLine("cb%u EXECUTES%s", ShortId(U64(cmd)), list.c_str());
    }
    DeviceOfCommandBuffer(cmd)->TraceCmdExecuteCommands(cmd, count, buffers);
}
VKAPI_ATTR VkResult VKAPI_CALL TraceBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info) {
    IN_HOOK("BeginCmdBuffer");
    Device* d = DeviceOfCommandBuffer(cmd);
    ForgetHudPass(cmd);
    if (Tracing()) {
        std::lock_guard lock{g_trace_mutex};
        g_cb_trace[U64(cmd)] = {};
        TraceLine("cb%u BEGIN device%u", ShortId(U64(cmd)), d->index);
    }
    return d->BeginCommandBuffer(cmd, info);
}

void TraceSubmit(VkQueue queue, const std::vector<VkCommandBuffer>& buffers) {
    std::lock_guard lock{g_trace_mutex};
    std::string list;
    for (VkCommandBuffer buffer : buffers) {
        TraceFlush(buffer, g_cb_trace[U64(buffer)]);
        list += " cb" + std::to_string(ShortId(U64(buffer)));
    }
    TraceLine("SUBMIT queue%u:%s", ShortId(U64(queue)), list.c_str());
}

Device* DeviceOfQueue(VkQueue queue) {
    std::lock_guard lock{g_tables_mutex};
    if (const auto it = g_queues.find(U64(queue)); it != g_queues.end()) {
        return it->second.first;
    }
    const auto it = g_devices.find(Key(queue));
    return it == g_devices.end() ? nullptr : it->second;
}

/// A submit, with all its command buffers: what it carries (captures, a hand-over), and after it
/// has gone on, the mark that tells when the GPU is done with a frame.
VkResult Submitted(VkQueue queue, const std::vector<VkCommandBuffer>& buffers, const auto& forward) {
    Device& d = *DeviceOfQueue(queue);
    const bool frame = NoteSubmitted(d, buffers.data(), buffers.size());
    const uint64_t chain_before = d.chain.number;
    NoteChainSubmitted(d, buffers.data(), buffers.size());
    const uint64_t chain_after = d.chain.number;
    if (frame) {
        g_submit_thread = static_cast<int>(syscall(SYS_gettid));
        if (!WaitForGenerated(d)) {
            WaitForGpu(d); // (where that wait is off, or had nothing to wait for)
        }
        GuardHandover(d);
        double none = 0.0;
        g_frame_going.compare_exchange_strong(none, Now()); // (for Limiter::lead)
    }
    if (Tracing()) {
        TraceSubmit(queue, buffers);
    }
    const VkResult result = forward(d);
    if (result == VK_SUCCESS && chain_after > chain_before && g_live_timing.load() != 0.0f) {
        WatchDone(d, queue, chain_after); // (a hand-over's copy is in this submit)
    }
    if (frame && result == VK_SUCCESS) {
        const uint64_t after = g_chain_handed.load();
        g_chain_drawn_after = after;
        MarkGpu(d, queue);
        {
            std::lock_guard lock{d.mark_mutex};
            d.drawn[after % 8] = {after, d.mark_last, d.mark_last >= 0 ? d.mark_serial[d.mark_last] : 0};
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    IN_HOOK("QueueSubmit");
    thread_local std::vector<VkCommandBuffer> buffers;
    buffers.clear();
    for (uint32_t i = 0; i < count; ++i) {
        buffers.insert(buffers.end(), submits[i].pCommandBuffers, submits[i].pCommandBuffers + submits[i].commandBufferCount);
    }
    return Submitted(queue, buffers, [&](Device& d) { return d.QueueSubmit(queue, count, submits, fence); });
}
VKAPI_ATTR VkResult VKAPI_CALL TraceQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                                 VkFence fence) {
    IN_HOOK("QueueSubmit2");
    thread_local std::vector<VkCommandBuffer> buffers;
    buffers.clear();
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k) {
            buffers.push_back(submits[i].pCommandBufferInfos[k].commandBuffer);
        }
    }
    return Submitted(queue, buffers, [&](Device& d) { return d.TraceQueueSubmit2(queue, count, submits, fence); });
}

struct TraceHook {
    const char* name;
    PFN_vkVoidFunction function;
    bool always{}; ///< also without SC_FG_TRACE: the hooks that find the HUD layer
};
#define TRACE_HOOK(name, function) {name, reinterpret_cast<PFN_vkVoidFunction>(function), false}
#define PASS_HOOK(name, function) {name, reinterpret_cast<PFN_vkVoidFunction>(function), true}
const TraceHook TRACE_HOOKS[] = {
    PASS_HOOK("vkCreateRenderPass", TraceCreateRenderPass),
    PASS_HOOK("vkCreateRenderPass2", TraceCreateRenderPass2),
    PASS_HOOK("vkCreateRenderPass2KHR", TraceCreateRenderPass2),
    PASS_HOOK("vkCreateFramebuffer", TraceCreateFramebuffer),
    PASS_HOOK("vkCmdBeginRenderPass", TraceCmdBeginRenderPass),
    PASS_HOOK("vkCmdBeginRenderPass2", TraceCmdBeginRenderPass2),
    PASS_HOOK("vkCmdBeginRenderPass2KHR", TraceCmdBeginRenderPass2),
    PASS_HOOK("vkCmdEndRenderPass", TraceCmdEndRenderPass),
    PASS_HOOK("vkCmdEndRenderPass2", TraceCmdEndRenderPass2),
    PASS_HOOK("vkCmdEndRenderPass2KHR", TraceCmdEndRenderPass2),
    PASS_HOOK("vkCmdBeginRendering", TraceCmdBeginRendering),
    PASS_HOOK("vkCmdBeginRenderingKHR", TraceCmdBeginRendering),
    PASS_HOOK("vkCmdEndRendering", TraceCmdEndRendering),
    PASS_HOOK("vkCmdEndRenderingKHR", TraceCmdEndRendering),
    TRACE_HOOK("vkCmdDraw", TraceCmdDraw),
    TRACE_HOOK("vkCmdDrawIndexed", TraceCmdDrawIndexed),
    TRACE_HOOK("vkCmdDrawIndirect", TraceCmdDrawIndirect),
    TRACE_HOOK("vkCmdDrawIndexedIndirect", TraceCmdDrawIndexedIndirect),
    TRACE_HOOK("vkCmdDrawIndirectCount", TraceCmdDrawIndirectCount),
    TRACE_HOOK("vkCmdDrawIndirectCountKHR", TraceCmdDrawIndirectCount),
    TRACE_HOOK("vkCmdDrawIndexedIndirectCount", TraceCmdDrawIndexedIndirectCount),
    TRACE_HOOK("vkCmdDrawIndexedIndirectCountKHR", TraceCmdDrawIndexedIndirectCount),
    TRACE_HOOK("vkCmdDispatch", TraceCmdDispatch),
    TRACE_HOOK("vkCmdDispatchIndirect", TraceCmdDispatchIndirect),
    PASS_HOOK("vkCmdCopyImage", TraceCmdCopyImage),
    PASS_HOOK("vkCmdCopyImage2", TraceCmdCopyImage2),
    PASS_HOOK("vkCmdCopyImage2KHR", TraceCmdCopyImage2),
    TRACE_HOOK("vkCmdBlitImage", TraceCmdBlitImage),
    TRACE_HOOK("vkCmdBlitImage2", TraceCmdBlitImage2),
    TRACE_HOOK("vkCmdBlitImage2KHR", TraceCmdBlitImage2),
    TRACE_HOOK("vkCmdResolveImage", TraceCmdResolveImage),
    TRACE_HOOK("vkCmdCopyImageToBuffer", TraceCmdCopyImageToBuffer),
    TRACE_HOOK("vkCmdCopyBufferToImage", TraceCmdCopyBufferToImage),
    TRACE_HOOK("vkCmdExecuteCommands", TraceCmdExecuteCommands),
    PASS_HOOK("vkBeginCommandBuffer", TraceBeginCommandBuffer),
    PASS_HOOK("vkQueueSubmit", TraceQueueSubmit),
    PASS_HOOK("vkQueueSubmit2", TraceQueueSubmit2),
    PASS_HOOK("vkQueueSubmit2KHR", TraceQueueSubmit2),
};

struct Hook {
    const char* name;
    PFN_vkVoidFunction function;
    bool needs_next; ///< only stands in when the driver has the function
};
#define HOOK(name, needs) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(Hook##name), needs}
const Hook DEVICE_HOOKS[] = {
    HOOK(DestroyDevice, false),         HOOK(GetDeviceQueue, false),
    HOOK(GetDeviceQueue2, true),        HOOK(AllocateCommandBuffers, false),
    HOOK(FreeCommandBuffers, false),    HOOK(CreateImage, false),
    HOOK(DestroyImage, false),          HOOK(CreateImageView, false),
    HOOK(DestroyImageView, false),      HOOK(GetImageViewHandleNVX, true),
    HOOK(GetImageViewHandle64NVX, true), HOOK(CmdCuLaunchKernelNVX, true),
    HOOK(CreateSwapchainKHR, true),     HOOK(DestroySwapchainKHR, true),
    HOOK(QueuePresentKHR, true),        HOOK(SetLatencySleepModeNV, true),
    HOOK(LatencySleepNV, true),         HOOK(SetLatencyMarkerNV, true),
    HOOK(WaitForPresentKHR, true),
#ifdef VK_EXT_present_timing
    HOOK(WaitForPresent2KHR, true),     HOOK(GetPastPresentationTimingEXT, true),
#endif
};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookGetDeviceProcAddr);
    }
    Device* d = device ? GetDevice(Key(device)) : nullptr;
    if (!d) {
        return nullptr;
    }
    const PFN_vkVoidFunction next = d->gdpa(device, name);
    if (g_test_inputs && !std::strcmp(name, "vkCmdPipelineBarrier")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookCmdPipelineBarrier);
    }
    for (const Hook& hook : DEVICE_HOOKS) {
        if (!std::strcmp(name, hook.name)) {
            return hook.needs_next && !next ? nullptr : hook.function;
        }
    }
    if (next) {
        for (const TraceHook& hook : TRACE_HOOKS) {
            if ((g_trace || (hook.always && g_hud_capture)) && !std::strcmp(name, hook.name)) {
                return hook.function;
            }
        }
    }
    return next;
}

// The game's window being given to Vulkan: where the panel was asked for at launch (SC_FG_PANEL=1),
// the layer notes which display connection it is on, so that the panel's key can be read there.
// Nothing else is done with it, and without the panel these hooks are not even handed out.
struct PlatformSurfaceInfo { // (the head of VkWaylandSurfaceCreateInfoKHR: sType, pNext, flags, display, surface)
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* display;
    void* surface;
};
using PFN_CreatePlatformSurface = VkResult(VKAPI_PTR*)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);

VkResult CreatePlatformSurface(const char* name, VkInstance instance, const void* info, const VkAllocationCallbacks* allocator,
                               VkSurfaceKHR* surface) {
    Instance* in = GetInstance(Key(instance));
    const auto next = in ? reinterpret_cast<PFN_CreatePlatformSurface>(in->gipa(instance, name)) : nullptr;
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return next(instance, info, allocator, surface);
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateWaylandSurface(VkInstance instance, const void* info,
                                                        const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    IN_HOOK("CreateSurface");
    const VkResult result = CreatePlatformSurface("vkCreateWaylandSurfaceKHR", instance, info, allocator, surface);
    if (result == VK_SUCCESS && info) {
        g_panel_keys.SeenWayland(static_cast<const PlatformSurfaceInfo*>(info)->display);
        Log("fg: the game's window is on Wayland");
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateXlibSurface(VkInstance instance, const void* info,
                                                     const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    IN_HOOK("CreateSurface");
    const VkResult result = CreatePlatformSurface("vkCreateXlibSurfaceKHR", instance, info, allocator, surface);
    if (result == VK_SUCCESS) {
        g_panel_keys.SeenX11();
        Log("fg: the game's window is on X11");
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL HookCreateXcbSurface(VkInstance instance, const void* info,
                                                    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    IN_HOOK("CreateSurface");
    const VkResult result = CreatePlatformSurface("vkCreateXcbSurfaceKHR", instance, info, allocator, surface);
    if (result == VK_SUCCESS) {
        g_panel_keys.SeenX11();
        Log("fg: the game's window is on X11");
    }
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL HookGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!std::strncmp(name, "vkCreate", 8) && std::strstr(name, "SurfaceKHR") && EnvFloat("SC_FG_PANEL", 0.0f) != 0.0f) {
        struct SurfaceHook {
            const char* name;
            PFN_CreatePlatformSurface function;
        };
        static const SurfaceHook hooks[] = {{"vkCreateWaylandSurfaceKHR", HookCreateWaylandSurface},
                                            {"vkCreateXlibSurfaceKHR", HookCreateXlibSurface},
                                            {"vkCreateXcbSurfaceKHR", HookCreateXcbSurface}};
        for (const SurfaceHook& hook : hooks) {
            if (!std::strcmp(name, hook.name)) {
                // (only where the instance has it: the window system's extension has to be on.
                // Otherwise the usual way below, which hands back whatever is there.)
                Instance* in = instance ? GetInstance(Key(instance)) : nullptr;
                if (in && in->gipa(instance, name)) {
                    return reinterpret_cast<PFN_vkVoidFunction>(hook.function);
                }
                break;
            }
        }
    }
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookGetInstanceProcAddr);
    }
    if (!std::strcmp(name, "vkCreateInstance")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookCreateInstance);
    }
    if (!std::strcmp(name, "vkDestroyInstance")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookDestroyInstance);
    }
    if (!std::strcmp(name, "vkCreateDevice")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookCreateDevice);
    }
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
        return reinterpret_cast<PFN_vkVoidFunction>(HookGetDeviceProcAddr);
    }
    Instance* in = instance ? GetInstance(Key(instance)) : nullptr;
    const PFN_vkVoidFunction next = in ? in->gipa(instance, name) : nullptr;
    for (const Hook& hook : DEVICE_HOOKS) {
        if (!std::strcmp(name, hook.name)) {
            return hook.needs_next && !next ? nullptr : hook.function;
        }
    }
    if (next) {
        for (const TraceHook& hook : TRACE_HOOKS) {
            if ((g_trace || (hook.always && g_hud_capture)) && !std::strcmp(name, hook.name)) {
                return hook.function;
            }
        }
    }
    return next;
}

bool ProcessMatches() {
    const char* want = std::getenv("SC_FG_MATCH");
    if (!want || !*want) {
        want = "StarCitizen.exe";
    }
    char buffer[4096];
    FILE* file = std::fopen("/proc/self/cmdline", "r");
    if (!file) {
        return false;
    }
    const size_t size = std::fread(buffer, 1, sizeof buffer - 1, file);
    std::fclose(file);
    for (size_t i = 0; i < size; ++i) {
        if (!buffer[i]) {
            buffer[i] = ' ';
        }
    }
    buffer[size] = 0;
    return strcasestr(buffer, want) != nullptr;
}

} // namespace

// AMD's Vulkan backend calls these by name where it should go through the device's own table. In
// a layer they must reach the next layer, not the loader's entry points (which would run this
// layer again, and take handles the loader has not wrapped). Hidden: nothing outside this
// library sees them.
extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    return g_ffx_device->gdpa(device, name);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                              const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    return g_ffx_device->CreateBuffer(device, info, allocator, buffer);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical,
                                                               VkPhysicalDeviceMemoryProperties* properties) {
    g_ffx_device->instance->GetPhysicalDeviceMemoryProperties(physical, properties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physical,
                                                         VkPhysicalDeviceProperties* properties) {
    g_ffx_device->instance->GetPhysicalDeviceProperties(physical, properties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical,
                                                          VkPhysicalDeviceProperties2* properties) {
    g_ffx_device->instance->GetPhysicalDeviceProperties2(physical, properties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physical,
                                                       VkPhysicalDeviceFeatures* features) {
    g_ffx_device->instance->GetPhysicalDeviceFeatures(physical, features);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical,
                                                        VkPhysicalDeviceFeatures2* features) {
    g_ffx_device->instance->GetPhysicalDeviceFeatures2(physical, features);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char* layer,
                                                                    uint32_t* count,
                                                                    VkExtensionProperties* properties) {
    return g_ffx_device->instance->EnumerateDeviceExtensionProperties(physical, layer, count, properties);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!ProcessMatches()) {
        return VK_ERROR_INITIALIZATION_FAILED; // the loader then leaves this layer out
    }
    if (version->loaderLayerInterfaceVersion > 2) {
        version->loaderLayerInterfaceVersion = 2;
    }
    version->pfnGetInstanceProcAddr = HookGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = HookGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
}
