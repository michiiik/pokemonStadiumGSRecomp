/*
 * register_overlays.cpp — feed the generated section/overlay table
 * into librecomp's recomp::overlays::register_overlays().
 *
 * Adapted from Zelda64Recomp's src/main/register_overlays.cpp. The
 * generated/recomp_overlays.inl defines:
 *   SectionTableEntry section_table[]      (every code section)
 *   uint16_t          overlay_sections_by_index[]  (overlay slots)
 *   size_t            num_sections          (total sections)
 *
 * Without this registration, librecomp's func_map stays empty for
 * every section beyond patches — so any LOOKUP_FUNC at runtime fails
 * with "Failed to find function at 0x...". This is the missing piece
 * that lets the resident kernel + 77 fragments resolve at runtime.
 */

#include <cstdint>
#include <cstddef>

#include "librecomp/overlays.hpp"
#include "recomp.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

#include "../../generated/recomp_overlays.inl"
#include "../../generated/funcs.h"

static void ps2_finish_guest_return(recomp_context* ctx) {
    const uint32_t target = static_cast<uint32_t>(ctx->r31);
    if (target != ctx->host_return_target) {
        recomp_request_tailcall(ctx, static_cast<gpr>(static_cast<int32_t>(target)));
    }
}

static void ps2_dynamic_fragment_helper(uint8_t* rdram, recomp_context* ctx) {
    MEM_W(0x4, ctx->r29) = ctx->r5;
    ctx->r3 = 0;
    ctx->r2 = (ctx->r4 == 0)
        ? static_cast<gpr>(static_cast<int32_t>(0x80685C80u))
        : 0;
    ps2_finish_guest_return(ctx);
}

extern "C" void recomp_ultra_trace_record(const char* name, uint32_t pc, uint32_t a0,
                                         uint32_t a1, uint32_t a2, uint32_t a3);

// Diagnostic shim: 0x801451A0 is a *runtime* address (wherever the main pool
// happened to place a fragment this run), so this hardcoded mapping can only
// be correct by accident. Record every dispatch through it with the live
// argument registers so calls can be correlated against the fragment actually
// loaded there.
static void ps2_tramp_801451A0(uint8_t* rdram, recomp_context* ctx) {
    recomp_ultra_trace_record("~ps2_tramp_801451A0", 0x801451A0u, (uint32_t)ctx->r4,
                              (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r31);
    func_83F002F4(rdram, ctx);
}

static void ps2_set_callback_guard(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t callback = static_cast<uint32_t>(ctx->r4);
    if (callback == 0 ||
        (callback >= 0x80000000u && callback < 0x90000000u)) {
        MEM_W(-0x590, 0x80090000u) = callback;
    }
    ps2_finish_guest_return(ctx);
}

namespace pokestadium {
    void register_overlays();
    void register_runtime_trampolines();
}

void pokestadium::register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections     = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len   = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);

    // Stadium 2 emits these two runtime trampolines outside normal function
    // entries. Register their native destinations so they never fall through
    // to the interpreter and lose the guest return address.
    // Band-aid, not a fix: 0x801451A0 is wherever the main pool happened to
    // place a fragment. It is correct only while section 15 (link
    // 0x83F00000) is resident there. Removing it just turns the failure
    // into an explicit lookup miss at the same address. The real fix is a
    // Memmap_RelocateFragment registration hook in game.toml (see the
    // handoff) so every fragment registers its own entry.
    recomp::overlays::add_loaded_function(0x801451A0, ps2_tramp_801451A0);
    recomp::overlays::add_loaded_function(0x806788E0, ps2_dynamic_fragment_helper);
}

void pokestadium::register_runtime_trampolines() {
    // Band-aid, not a fix: 0x801451A0 is wherever the main pool happened to
    // place a fragment. It is correct only while section 15 (link
    // 0x83F00000) is resident there. Removing it just turns the failure
    // into an explicit lookup miss at the same address. The real fix is a
    // Memmap_RelocateFragment registration hook in game.toml (see the
    // handoff) so every fragment registers its own entry.
    recomp::overlays::add_loaded_function(0x801451A0, ps2_tramp_801451A0);
    recomp::overlays::add_loaded_function(0x806788E0, ps2_dynamic_fragment_helper);
    recomp::overlays::add_loaded_function(0x80015A40, ps2_set_callback_guard);
}
