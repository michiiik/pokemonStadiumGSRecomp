/* Android host implementation for the optional desktop TCP debug server. */

#include "debug_server.h"

namespace pkmnstadium::dbg {

std::atomic<bool>     g_fast_forward{false};
std::atomic<bool>     g_enable_instant_present_request{false};
std::atomic<uint64_t> g_vi_ticks{0};
std::atomic<uint64_t> g_frame_count{0};
std::atomic<bool>     g_input_override_active{false};
std::atomic<uint16_t> g_buttons_override{0};
std::atomic<int>      g_stick_x_override{0};
std::atomic<int>      g_stick_y_override{0};
std::atomic<float>    g_audio_volume{0.0f};
std::atomic<uint64_t> g_send_dl_count{0};
std::atomic<uint64_t> g_update_screen_count{0};
std::atomic<uint64_t> g_send_dl_audio_count{0};
std::atomic<uint64_t> g_send_dl_gfx_count{0};
std::atomic<uint64_t> g_send_dl_other_count{0};

void start(int) {}
void shutdown() {}

} // namespace pkmnstadium::dbg
