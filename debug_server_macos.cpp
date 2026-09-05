// macOS debug-server stand-in.
//
// The full TCP debug server (src/main/debug_server.cpp) is Windows-only in
// this checkout (winsock2 / dbghelp / tlhelp32); the Stadium 1 "rental"
// checkout has a cross-platform variant that could be ported later. Until
// then this provides the one thing the macOS boot investigation actually
// needs: the built-in progress counters, reported on a timer.
//
// These counters are already incremented by live code -- g_send_dl_count and
// g_update_screen_count in src/main/rt64_render_context.cpp, g_vi_ticks and
// g_frame_count in src/main/main.cpp -- so this is pure observation with no
// behavior change. They answer "is the game actually reaching the point of
// producing frames?", which is otherwise invisible in a headless/agent
// environment where screen capture is unavailable.
//
// Set PSR_PROGRESS=0 to silence.

#include "debug_server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace pkmnstadium::dbg {

std::atomic<bool> g_fast_forward{false};
std::atomic<bool> g_enable_instant_present_request{false};
std::atomic<uint64_t> g_vi_ticks{0};
std::atomic<uint64_t> g_frame_count{0};
std::atomic<bool> g_input_override_active{false};
std::atomic<uint16_t> g_buttons_override{0};
std::atomic<int> g_stick_x_override{0};
std::atomic<int> g_stick_y_override{0};
std::atomic<float> g_audio_volume{0.0f};
std::atomic<uint64_t> g_send_dl_count{0};
std::atomic<uint64_t> g_update_screen_count{0};
std::atomic<uint64_t> g_send_dl_audio_count{0};
std::atomic<uint64_t> g_send_dl_gfx_count{0};
std::atomic<uint64_t> g_send_dl_other_count{0};

extern "C" uint64_t ultramodern_submit_gfx_count(void);
extern "C" uint64_t ultramodern_submit_audio_count(void);
extern "C" uint64_t ultramodern_submit_other_count(void);
extern "C" uint64_t ultramodern_sp_complete_count(void);
extern "C" uint64_t ultramodern_dp_complete_count(void);

namespace {
std::atomic<bool> g_progress_exit{false};
std::thread g_progress_thread;

void progress_func() {
    uint64_t last_dl = 0, last_screen = 0, last_frames = 0;
    for (int tick = 0; !g_progress_exit.load(std::memory_order_relaxed); tick++) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (g_progress_exit.load(std::memory_order_relaxed)) break;

        const uint64_t dl     = g_send_dl_count.load(std::memory_order_relaxed);
        const uint64_t screen = g_update_screen_count.load(std::memory_order_relaxed);
        const uint64_t frames = g_frame_count.load(std::memory_order_relaxed);
        const uint64_t vi     = g_vi_ticks.load(std::memory_order_relaxed);

        // Only speak up when something changed, or every ~20s as a heartbeat,
        // so a stalled boot is obvious without flooding the log.
        const bool moved = (dl != last_dl) || (screen != last_screen) ||
                           (frames != last_frames);
        if (moved || (tick % 10) == 0) {
            fprintf(stderr,
                "[progress] display_lists=%llu screen_updates=%llu frames=%llu "
                "vi_ticks=%llu | submit gfx=%llu aud=%llu other=%llu "
                "| complete sp=%llu dp=%llu%s\n",
                (unsigned long long)dl, (unsigned long long)screen,
                (unsigned long long)frames, (unsigned long long)vi,
                (unsigned long long)ultramodern_submit_gfx_count(),
                (unsigned long long)ultramodern_submit_audio_count(),
                (unsigned long long)ultramodern_submit_other_count(),
                (unsigned long long)ultramodern_sp_complete_count(),
                (unsigned long long)ultramodern_dp_complete_count(),
                moved ? "" : "  (no change)");
            fflush(stderr);
        }
        last_dl = dl; last_screen = screen; last_frames = frames;
    }
}
} // namespace

void start(int) {
    const char* e = std::getenv("PSR_PROGRESS");
    if (e != nullptr && e[0] == '0') {
        return;
    }
    g_progress_exit.store(false, std::memory_order_relaxed);
    g_progress_thread = std::thread{progress_func};
}

void shutdown() {
    g_progress_exit.store(true, std::memory_order_relaxed);
    if (g_progress_thread.joinable()) {
        g_progress_thread.join();
    }
}

} // namespace pkmnstadium::dbg
