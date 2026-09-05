// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Framework-level libultra-call ring instrumentation
//     (LIBRECOMP_ULTRA_TRACE) at osAi entry points for runner-side
//     boot sequencing diagnostics.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include "recomp.h"
#include "librecomp/ultra_trace.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <string>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#define VI_NTSC_CLOCK 48681812

// ── AI_LEN_REG emulation (audio pacing back-pressure) ───────────────
// The N64 Audio Interface length register (kseg1 0xA4500004) reports how
// many bytes are still queued for playback. libultra audio managers read
// it directly via HW_REG(AI_LEN_REG) to pace generation: when it shows
// the AI buffer is full they emit a SHORTER audio frame; when it's low,
// a full frame. That feedback loop is what keeps the game producing
// audio at exactly the playback rate on hardware.
//
// This runtime maps the MMIO region to zero-initialized pages (see
// librecomp/addresses.hpp), so absent emulation AI_LEN_REG always reads
// 0. The game then believes the AI buffer is perpetually empty, ALWAYS
// generates full frames, and over-produces audio relative to real-time
// output. Downstream that manifested as the music-rate periodic click:
// the host SDL queue creeps up and main.cpp's queue_samples() decimation
// valve drops samples mid-chunk to drain the overflow — a discontinuity
// (click) at every drop, ~4/s.
//
// Fix: publish the live remaining-byte count into the register after each
// buffer submission so the game's own hardware-designed feedback loop
// works. The queue then self-stabilizes at a low, bounded depth and no
// samples are ever dropped. This is generic across every libultra audio
// game, not Stadium-specific.
//
// Address note: kseg1 0xA4500004 sign-extends to the 64-bit gpr
// 0xFFFFFFFFA4500004; MEM_W reduces that to the same host offset
// (0x24500004 within the mapped region) that the recompiled HW_REG read
// resolves to, so the write and the read alias the same byte.
static constexpr uint64_t AI_LEN_REG_ADDR = 0xFFFFFFFFA4500004ull;

static inline void publish_ai_len(uint8_t* rdram) {
    // get_remaining_audio_bytes() returns the queued byte count already
    // lag-adjusted (it subtracts a fractional refresh); the game does
    // HW_REG(AI_LEN_REG) >> 2 to recover stereo frames.
    MEM_W(0, AI_LEN_REG_ADDR) = (int32_t)ultramodern::get_remaining_audio_bytes();
}

// Exported so high-frequency wrappers (osRecvMesg) can keep the raw
// register fresh: games that read HW_REG(AI_LEN_REG) directly (Stadium's
// audio frame builder) otherwise see a value frozen at the LAST osAi*
// call — one full frame stale — where hardware drains it continuously.
// With the virtual AI DAC in ultramodern this refresh is cheap
// (steady-clock read + subtract) and makes the register hardware-fresh
// as of the message receipt that wakes the audio thread.
extern "C" void recomp_publish_ai_len(uint8_t* rdram) {
    publish_ai_len(rdram);
}

extern "C" void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);

    // Stadium 2's startup path can reach audio setup before its video-mode
    // initializer. This field is the selected VI rate (50 or 60); the US ROM
    // is NTSC, so the hardware-consistent fallback is 60 Hz.
    constexpr uint32_t kStadium2ViRate = 0x8011EB94;
    if (MEM_W(0, kStadium2ViRate) == 0) {
        MEM_W(0, kStadium2ViRate) = 60;
    }
    // The audio frame scheduler divides by this halfword immediately after
    // osAiSetFrequency. A missing producer leaves it zero during early boot.
    if (MEM_H(0, 0x8011EA6C) == 0) {
        MEM_H(0, 0x8011EA6C) = 1;
    }

    uint32_t freq = ctx->r4;
    // This makes actual audio frequency more accurate to console, but may not be desirable
    //uint32_t dacRate = (uint32_t)(((float)VI_NTSC_CLOCK / freq) + 0.5f);
    //freq = VI_NTSC_CLOCK / dacRate;
    ctx->r2 = freq;
    ultramodern::set_audio_frequency(freq);
}

extern "C" void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ultramodern::queue_audio_buffer(rdram, ctx->r4, ctx->r5);
    // Refresh AI_LEN_REG so the next audio-frame's HW_REG read sees the
    // real remaining-byte count and paces generation accordingly.
    publish_ai_len(rdram);
    ctx->r2 = 0;
}

// Stadium 2 has no osAiSetNextBuffer: it stores to AI_DRAM_ADDR_REG and
// AI_LEN_REG itself, and a guest store to 0xA4500000 lands in the mapping's
// slack where nothing reads it. Its store site calls this instead, so the
// buffer reaches the host queue on the same path osAiSetNextBuffer would use.
extern "C" void recomp_ai_submit_buffer(uint8_t* rdram, uint32_t guest_addr,
                                        uint32_t byte_count) {
    static uint32_t direct_submit_log_count = 0;
    if (byte_count == 0) {
        return;
    }
    if (direct_submit_log_count < 8) {
        std::fprintf(stderr, "[PSR][audio] direct AI submit guest=0x%08X bytes=%u\n",
                     guest_addr, byte_count);
        ++direct_submit_log_count;
    }
    ultramodern::queue_audio_buffer(rdram, (PTR(int16_t))guest_addr, byte_count);
    // Leave AI_LEN holding the remaining-byte count the game reads back.
    publish_ai_len(rdram);
}

extern "C" void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // Keep the register consistent for games that mix the API and the
    // raw register read.
    publish_ai_len(rdram);
    ctx->r2 = ultramodern::get_remaining_audio_bytes();
}

extern "C" void osAiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 0x00000000; // Pretend the audio DMAs finish instantly
}
