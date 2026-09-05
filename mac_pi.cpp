// N64ModernRuntime — modifications in this file by Matthew Stanley
// (mstan fork). Per GPL-3.0 §5(a), changes are noted below; original
// file copyright remains with the original authors. See COPYING.
//
// Modified 2026 by Matthew Stanley:
//   - Bounds-check do_rom_read; zero-fill on out-of-range DMA.
//   - Host backtrace on OOB ROM read for diagnostics.
//   - Mirror ROM into kseg1 region of rdram (PokemonStadium copy
//     protection reads cart vaddrs directly).
//   - HLE-correct PI bus modeling for absent devices.
//   - HAL fragment trampoline synthesis hooks at load time.
//   - Framework-level libultra ring integration.
//
// Copyright (c) 2026 Matthew Stanley
//
// ---------------------------------------------------------------------

#include <memory>
#include <fstream>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/files.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/ultra_trace.hpp"
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

static std::vector<uint8_t> rom;

// ── PI ROM-fill ring ─────────────────────────────────────────────────
// Always-on ring of every ROM→RDRAM PI DMA (dram, rom, size, ms).
// This is the CPU-side half of the audio stream-buffer forensics: each
// RSP voice-input read (rspdma ring) maps back to the PI fill that
// last wrote its RDRAM range, giving the expected ROM offset without
// content searching — and a fill that lands on a range between two
// reads that expected stability is the clobber, attributed by time.
// PI ROM reads are low-rate (~10^2/s in game), so 262144 slots
// (8 MiB) cover tens of minutes. Query via recomp_pi_fill_dump.
namespace {
struct PiFillEvent {
    uint64_t seq;
    uint64_t ms;
    uint32_t dram;   // physical RDRAM address
    uint32_t rom;    // physical cart address
    uint32_t size;
    uint32_t pad;
};
constexpr uint32_t kPiFillCap = 262144;
PiFillEvent g_pi_fills[kPiFillCap]{};
std::atomic<uint64_t> g_pi_fill_seq{0};
const std::chrono::steady_clock::time_point g_pi_fill_t0 =
    std::chrono::steady_clock::now();

void record_pi_fill(uint32_t dram, uint32_t rom_addr, uint32_t size) {
    uint64_t seq = g_pi_fill_seq.fetch_add(1, std::memory_order_relaxed);
    PiFillEvent& ev = g_pi_fills[seq % kPiFillCap];
    ev.seq = seq;
    ev.ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_pi_fill_t0).count());
    ev.dram = dram;
    ev.rom = rom_addr;
    ev.size = size;
    ev.pad = 0;
}
}

// Streaming whole-ring dump, oldest first. Same tolerance model as
// recomp_rsp_dma_dump: the write index is snapshotted up front.
// Returns events written, or -1 on I/O failure.
extern "C" int64_t recomp_pi_fill_dump(const char* path) {
    if (path == nullptr) {
        return -1;
    }
    uint64_t write_index = g_pi_fill_seq.load(std::memory_order_acquire);
    uint64_t count = std::min<uint64_t>(write_index, kPiFillCap);
    uint64_t start = write_index - count;

    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return -1;
    }
    constexpr uint32_t kChunk = 8192;
    std::vector<PiFillEvent> chunk(kChunk);
    uint64_t written = 0;
    while (written < count) {
        uint32_t n = (uint32_t)std::min<uint64_t>(kChunk, count - written);
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = g_pi_fills[(start + written + i) % kPiFillCap];
        }
        if (std::fwrite(chunk.data(), sizeof(PiFillEvent), n, f) != n) {
            std::fclose(f);
            return -1;
        }
        written += n;
    }
    std::fclose(f);
    return (int64_t)written;
}

bool recomp::is_rom_loaded() {
    return !rom.empty();
}

void recomp::set_rom_contents(std::vector<uint8_t>&& new_rom) {
    rom = std::move(new_rom);
}

// Mirror ROM into the kseg1 region of rdram so that direct MIPS
// reads of cart vaddrs (e.g. `lw $t1, 0xB0000E38`) return the
// correct ROM bytes. Some games read ROM via cart vaddrs without
// going through osPiStartDma — most notably copy-protection /
// CIC-checksum routines that read a known ROM word and compare
// against an expected value (e.g. Pokemon Stadium's
// Game_DoCopyProtection at *(u32*)0xB0000E38).
//
// Without this mirror, MEM_W of a cart vaddr reads rdram bytes
// that were never written, returning garbage. The check trips
// and the game falls into a copy-protection error path
// (e.g. state = -0x10 in Stadium, which then bypasses the
// state-machine switch and the game appears to "stutter back to
// intro").
//
// MEM_W formula: rdram + (vaddr - 0xFFFFFFFF80000000). For
// kseg1 cart base 0xB0000000, that's rdram + 0x30000000.
// We use the recompiler's BE-byte-swapped storage convention
// (XOR-3 byte index) so that a host-native int32_t read of the
// mirrored bytes returns the BE word that Stadium expects.
//
// Cost: one-time copy at startup, ~32 MiB worst case (size of
// the ROM image). Negligible vs. the rest of the recompile.
void recomp::mirror_rom_to_kseg1(uint8_t* rdram) {
    if (rom.empty()) {
        fprintf(stderr,
            "[mirror] mirror_rom_to_kseg1 called with empty rom — "
            "kseg1 cart reads will return garbage\n");
        fflush(stderr);
        return;
    }
    constexpr size_t kKseg1RomOffset = 0x30000000;
    const size_t n = rom.size();
    for (size_t i = 0; i < n; i++) {
        rdram[(kKseg1RomOffset + i) ^ 3] = rom[i];
    }
}

std::span<const uint8_t> recomp::get_rom() {
    return rom;
}

constexpr uint32_t k1_to_phys(uint32_t addr) {
    return addr & 0x1FFFFFFF;
}

constexpr uint32_t phys_to_k1(uint32_t addr) {
    return addr | 0xA0000000;
}

void save_write_ptr(const void* in, uint32_t offset, uint32_t count);
void save_clear(uint32_t start, uint32_t size, char value);

namespace {
    constexpr uint32_t flash_size = 0x20000;
    constexpr uint32_t flash_page_size = 128;
    constexpr uint32_t flash_pages_per_sector = 128;
    constexpr uint32_t flash_sector_size = flash_page_size * flash_pages_per_sector;
    constexpr uint32_t flash_cmd_reg = 0x10000;
    constexpr uint32_t flash_cmd_chip_erase = 0x3C000000;
    constexpr uint32_t flash_cmd_sector_erase = 0x4B000000;
    constexpr uint32_t flash_cmd_execute_erase = 0x78000000;
    constexpr uint32_t flash_cmd_program_page = 0xA5000000;
    constexpr uint32_t flash_cmd_page_program = 0xB4000000;
    constexpr uint32_t flash_cmd_status = 0xD2000000;
    constexpr uint32_t flash_cmd_id = 0xE1000000;
    constexpr uint32_t flash_cmd_read_array = 0xF0000000;
    // MX_B/D makes libultra use 0x80-byte Flash page addressing, matching
    // the runtime's 128-byte page program/read granularity.
    constexpr uint32_t flash_id_type = 0x11118001;
    constexpr uint32_t flash_id_maker = 0x00C2001D;

    enum class FlashMode {
        ReadArray,
        Status,
        Id,
        PageProgram,
        SectorErase,
        ChipErase,
    };

    FlashMode flash_mode = FlashMode::ReadArray;
    uint8_t flash_status = 0;
    uint32_t flash_pending_erase_page = 0;
    std::array<char, flash_page_size> flash_write_buffer{};

    bool flash_region_contains(uint32_t physical_addr, uint32_t size) {
        if (!recomp::flashram_allowed()) {
            return false;
        }
        if (physical_addr < recomp::sram_base) {
            return false;
        }
        const uint32_t offset = physical_addr - recomp::sram_base;
        return offset <= flash_size && size <= flash_size - offset;
    }

    uint32_t flash_page_offset(uint32_t page_num) {
        return page_num * flash_page_size;
    }

    void flash_program_page(uint32_t page_num) {
        const uint32_t offset = flash_page_offset(page_num);
        if (offset + flash_page_size <= flash_size) {
            save_write_ptr(flash_write_buffer.data(), offset, flash_page_size);
            flash_status = 0x04;
        }
        else {
            flash_status = 0;
        }
        flash_mode = FlashMode::Status;
    }

    void flash_erase_sector(uint32_t page_num) {
        const uint32_t sector_page = (page_num / flash_pages_per_sector) * flash_pages_per_sector;
        const uint32_t offset = flash_page_offset(sector_page);
        if (offset + flash_sector_size <= flash_size) {
            save_clear(offset, flash_sector_size, static_cast<char>(0xFF));
            flash_status = 0x08;
        }
        else {
            flash_status = 0;
        }
        flash_mode = FlashMode::Status;
    }

    void flash_erase_chip() {
        save_clear(0, flash_size, static_cast<char>(0xFF));
        flash_status = 0x08;
        flash_mode = FlashMode::Status;
    }
}

extern "C" void __osPiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void __osPiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

// The SI access guards are the serial-interface counterpart to the PI guards.
// Stadium uses them around controller/PIF transfers; the host-side SI path is
// already serialized by the runtime, so the reconstructed ABI entry points
// only need to preserve the call boundary and tracing contract.
extern "C" void __osSiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void __osSiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
}

extern "C" void osCartRomInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    OSPiHandle* handle = TO_PTR(OSPiHandle, recomp::cart_handle);
    handle->type = 0; // cart
    handle->baseAddress = phys_to_k1(recomp::rom_base);
    handle->domain = 0;

    ctx->r2 = (gpr)recomp::cart_handle;
}

extern "C" void osDriveRomInit_recomp(uint8_t * rdram, recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    OSPiHandle* handle = TO_PTR(OSPiHandle, recomp::drive_handle);
    handle->type = 1; // bulk
    handle->baseAddress = phys_to_k1(recomp::drive_base);
    handle->domain = 0;

    ctx->r2 = (gpr)recomp::drive_handle;
}

extern "C" void osCreatePiManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    osCreateMesgQueue(rdram, (int32_t)ctx->r5, (int32_t)ctx->r6, (s32)ctx->r7);

    // Stadium 2's streamed-ROM loader uses this private PI completion queue.
    // The shared runtime intentionally leaves the PI manager unimplemented,
    // so establish the queue here as part of the manager initialization.
    osCreateMesgQueue(rdram, 0x800CD000, 0x800CD018, 1);
}

void recomp::do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes) {
    // TODO: widen the copy to word units where alignment allows.

    // TODO: support transfers that aren't naturally aligned.
    assert((physical_addr & 0x1) == 0 && "Only PI DMA from aligned ROM addresses is currently supported");
    assert((ram_address & 0x7) == 0 && "Only PI DMA to aligned RDRAM addresses is currently supported");

    // Bounds check: if physical_addr is past end of ROM (audio engine
    // sometimes does this when wave_list[i].base is corrupted), don't
    // crash the runner. Log the bad DMA, zero-fill the destination,
    // and continue. This is a runtime defensive measure to keep
    // diagnostic data flowing — the underlying pointer corruption
    // still needs root-cause investigation.
    const uint32_t rom_off = physical_addr - recomp::rom_base;
    if (rom_off >= rom.size() || rom_off + num_bytes > rom.size()) {
        static int s_logged = 0;
        if (s_logged < 8) {
            s_logged++;
            fprintf(stderr,
                "[do_rom_read] OUT-OF-BOUNDS phys=0x%08X off=0x%X size=0x%zX "
                "(rom_size=0x%zX) — zero-filling dst=0x%08X\n",
                physical_addr, rom_off, num_bytes, rom.size(),
                (uint32_t)(int32_t)ram_address);
#ifdef _WIN32
            HANDLE proc = GetCurrentProcess();
            SymInitialize(proc, NULL, TRUE);
            void* frames[24];
            USHORT n = CaptureStackBackTrace(0, 24, frames, NULL);
            char symbuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            IMAGEHLP_LINE64 line; memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            for (USHORT i = 0; i < n && i < 18; i++) {
                DWORD64 disp64 = 0; DWORD disp32 = 0;
                const char* name = "?"; const char* file = "?"; DWORD lineno = 0;
                if (SymFromAddr(proc, (DWORD64)frames[i], &disp64, sym)) name = sym->Name;
                if (SymGetLineFromAddr64(proc, (DWORD64)frames[i], &disp32, &line)) {
                    file = line.FileName; lineno = line.LineNumber;
                }
                fprintf(stderr, "  #%02u %s (%s:%lu)\n", i, name, file, lineno);
            }
#endif
            fflush(stderr);
        }
        for (size_t i = 0; i < num_bytes; i++) {
            MEM_B(i, ram_address) = 0;
        }
        return;
    }

    uint8_t* rom_addr = rom.data() + rom_off;
    for (size_t copied = 0; copied < num_bytes; ++copied) {
        MEM_B(copied, ram_address) = rom_addr[copied];
    }
}

void recomp::do_rom_pio(uint8_t* rdram, gpr ram_address, uint32_t physical_addr) {
    assert((physical_addr & 0x3) == 0 && "PIO not 4-byte aligned in device, currently unsupported");
    assert((ram_address & 0x3) == 0 && "PIO not 4-byte aligned in RDRAM, currently unsupported");
    const uint8_t* src = rom.data() + (physical_addr - recomp::rom_base);
    for (uint32_t i = 0; i < sizeof(uint32_t); ++i) {
        MEM_B(i, ram_address) = src[i];
    }
}

struct {
    std::vector<char> save_buffer;
    std::thread saving_thread;
    std::filesystem::path save_file_path;
    moodycamel::LightweightSemaphore write_sempahore;
    // Used to tell the saving thread that a file swap is pending.
    moodycamel::LightweightSemaphore swap_file_pending_sempahore;
    // Used to tell the consumer thread that the saving thread is ready for a file swap.
    moodycamel::LightweightSemaphore swap_file_ready_sempahore;
    std::mutex save_buffer_mutex;
} save_context;

const std::u8string save_folder = u8"saves";

extern std::filesystem::path config_path;

std::filesystem::path ultramodern::get_save_file_path() {
    return save_context.save_file_path;
}

void set_save_file_path(const std::u8string& subfolder, const std::u8string& name) {
    std::filesystem::path dir = config_path / save_folder;
    if (!subfolder.empty()) {
        dir /= subfolder;
    }
    save_context.save_file_path = dir / (name + u8".bin");
}

void update_save_file() {
    bool saving_failed = false;
    {
        std::ofstream save_file = recomp::open_output_file_with_backup(ultramodern::get_save_file_path(), std::ios_base::binary);

        if (save_file.good()) {
            std::lock_guard lock{ save_context.save_buffer_mutex };
            save_file.write(save_context.save_buffer.data(), save_context.save_buffer.size());
        }
        else {
            saving_failed = true;
        }
    }
    if (!saving_failed) {
        saving_failed = !recomp::finalize_output_file_with_backup(ultramodern::get_save_file_path());
    }
    if (saving_failed) {
        ultramodern::error_handling::message_box("Failed to write to the save file. Check your file permissions and whether the save folder has been moved to Dropbox or similar, as this can cause issues.");
    }
}

extern std::atomic_bool exited;

void saving_thread_func(RDRAM_ARG1) {
    // Block this long for the next write, and fold at most this many
    // writes into one flush so a never-ending write stream can't keep
    // the save buffer from ever reaching disk.
    constexpr int64_t write_wait_us = 10000;
    constexpr int max_coalesced = 128;

    while (!exited) {
        bool dirty = false;
        int folded = 0;

        // Drain incoming writes, merging consecutive ones into a single
        // file flush, up to the coalescing cap.
        while (save_context.write_sempahore.wait(write_wait_us) && folded < max_coalesced) {
            dirty = true;
            folded++;
        }

        // Only touch the file when at least one write landed this round.
        if (dirty) {
            update_save_file();
        }

        if (save_context.swap_file_pending_sempahore.tryWait()) {
            save_context.swap_file_ready_sempahore.signal();
        }
    }
}

void save_write_ptr(const void* in, uint32_t offset, uint32_t count) {
    assert(offset + count <= save_context.save_buffer.size());

    {
        std::lock_guard lock{ save_context.save_buffer_mutex };
        std::memcpy(save_context.save_buffer.data() + offset, in, count);
    }

    save_context.write_sempahore.signal();
}

void save_write(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count) {
    assert(offset + count <= save_context.save_buffer.size());

    {
        std::lock_guard lock{ save_context.save_buffer_mutex };
        for (uint32_t pos = 0; pos < count; ++pos) {
            save_context.save_buffer[offset + pos] = MEM_B(pos, rdram_address);
        }
    }

    save_context.write_sempahore.signal();
}

void save_read(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count) {
    assert(offset + count <= save_context.save_buffer.size());

    std::lock_guard lock{ save_context.save_buffer_mutex };
    for (uint32_t pos = 0; pos < count; ++pos) {
        MEM_B(pos, rdram_address) = save_context.save_buffer[offset + pos];
    }
}

void save_clear(uint32_t start, uint32_t size, char value) {
    assert(start + size <= save_context.save_buffer.size());

    {
        std::lock_guard lock{ save_context.save_buffer_mutex };
        auto first = save_context.save_buffer.begin() + start;
        std::fill(first, first + size, value);
    }

    save_context.write_sempahore.signal();
}

size_t get_save_size(recomp::SaveType save_type) {
    switch (save_type) {
        case recomp::SaveType::AllowAll:
        case recomp::SaveType::Flashram:
            return 0x20000;
        case recomp::SaveType::Sram:
            return 0x8000;
        case recomp::SaveType::Eep16k:
            return 0x800;
        case recomp::SaveType::Eep4k:
            return 0x200;
        case recomp::SaveType::None:
            return 0;
    }
    return 0;
}

void read_save_file() {
    std::filesystem::path save_file_path = ultramodern::get_save_file_path();

    // Ensure the save file directory exists.
    std::filesystem::create_directories(save_file_path.parent_path());

    const char blank_value =
        recomp::get_save_type() == recomp::SaveType::Flashram ? static_cast<char>(0xFF) : 0;
    std::fill(save_context.save_buffer.begin(), save_context.save_buffer.end(), blank_value);

    // Read the save file if it exists.
    std::ifstream save_file = recomp::open_input_file_with_backup(save_file_path, std::ios_base::binary);
    if (save_file.good()) {
        save_file.read(save_context.save_buffer.data(), save_context.save_buffer.size());
    }
}

void ultramodern::init_saving(RDRAM_ARG1) {
    set_save_file_path(u8"", recomp::current_game_id());

    save_context.save_buffer.resize(get_save_size(recomp::get_save_type()));

    read_save_file();

    save_context.saving_thread = std::thread{saving_thread_func, PASS_RDRAM};
}

void ultramodern::change_save_file(const std::u8string& subfolder, const std::u8string& name) {
    // Tell the saving thread that a file swap is pending.
    save_context.swap_file_pending_sempahore.signal();
    // Wait until the saving thread indicates it's ready to swap files.
    save_context.swap_file_ready_sempahore.wait();
    // Perform the save file swap.
    set_save_file_path(subfolder, name);
    read_save_file();
}

void ultramodern::join_saving_thread() {
    if (save_context.saving_thread.joinable()) {
        save_context.saving_thread.join();
    }
}

// PI DMA completion: post the done-message to the request's return queue and
// record the delivery in the always-on ring ("~pi_dma_done": a0=mq, a1=phys,
// a2=send result, a3=size — see ultramodern/ultra_trace.hpp event classes).
static void dma_complete(RDRAM_ARG PTR(OSMesgQueue) mq, uint32_t physical_addr, uint32_t size) {
    s32 sent = osSendMesg(PASS_RDRAM mq, 0, OS_MESG_NOBLOCK);
    recomp_ultra_trace_record("~pi_dma_done", 0, (uint32_t)mq, physical_addr, (uint32_t)sent, size);
}

void do_dma(RDRAM_ARG PTR(OSMesgQueue) mq, gpr rdram_address, uint32_t physical_addr, uint32_t size, uint32_t direction) {
    // TODO asynchronous transfer
    // TODO implement unaligned DMA correctly
    if (direction == 0) {
        if (physical_addr >= recomp::rom_base) {
            // read cart rom
            recomp::do_rom_read(rdram, rdram_address, physical_addr, size);
            record_pi_fill((uint32_t)rdram_address & 0x1FFFFFFFu,
                           physical_addr, size);

            // Register any recompiled code sections that fall within
            // this DMA range into func_map.
            uint32_t rom_offset = physical_addr - recomp::rom_base;
            // Per-DMA trace is opt-in via N64MR_PI_DEBUG; without it
            // this fires for every overlay load and floods stderr.
            static const bool s_pi_debug = getenv("N64MR_PI_DEBUG") != nullptr;
            if (s_pi_debug) {
                fprintf(stderr, "[pi] load_overlays(rom=0x%08X, ram=0x%08X, size=0x%X)\n",
                    rom_offset, (uint32_t)(int32_t)rdram_address, size);
                fflush(stderr);
            }
            load_overlays(rom_offset, (int32_t)rdram_address, size);
            // Stadium-style fragment trampoline scan — registers
            // synthetic func_map entries for the textbin J/nop slots
            // between fragment headers and their first decompiled C
            // function. No-op for sections that don't have the
            // "FRAGMENT" magic (Zelda + most other games).
            recomp::overlays::scan_loaded_fragment_trampolines(rdram,
                rom_offset, (int32_t)rdram_address, size);
            if (s_pi_debug) {
                fprintf(stderr, "[pi] load_overlays done\n"); fflush(stderr);
            }

            // Send a message to the mq to indicate that the transfer completed
            dma_complete(rdram, mq, physical_addr, size);
        } else if (flash_region_contains(physical_addr, size)) {
            const uint32_t offset = physical_addr - recomp::sram_base;
            if (flash_mode == FlashMode::Id) {
                if (size >= sizeof(uint32_t)) {
                    MEM_W(0, rdram_address) = flash_id_type;
                }
                if (size >= sizeof(uint32_t) * 2) {
                    MEM_W(sizeof(uint32_t), rdram_address) = flash_id_maker;
                }
                for (uint32_t i = sizeof(uint32_t) * 2; i < size; i++) {
                    MEM_B(i, rdram_address) = 0;
                }
            } else {
                save_read(rdram, rdram_address, offset, size);
            }
            dma_complete(rdram, mq, physical_addr, size);
        } else if (physical_addr >= recomp::sram_base) {
            if (!recomp::sram_allowed()) {
                // SRAM region is also used by the 64DD's diagnostic/RAM
                // areas (~0x08000000-0x0FFFFFFF). When the game configured
                // EEPROM/Flash but libleo polls this range to detect the
                // drive, the correct behavior on a system without a
                // 64DD/SRAM device is "PI bus open — read returns the
                // open-bus pattern 0xFF". libleo and SRAM-probing init
                // code interpret all-0xFF as "no device responds";
                // returning all-zeros leaves it ambiguous (could mean
                // "blank but valid storage") and games like Pokemon
                // Stadium then sector-scan the entire region looking
                // for non-zero data to confirm presence.
                static thread_local int log_count = 0;
                if (log_count++ < 16) {
                    fprintf(stderr, "[pi] Drive/SRAM read 0x%08X size=0x%X — no-device, returning open-bus 0xFF\n",
                        physical_addr, size);
                }
                for (uint32_t i = 0; i < size; i++) {
                    MEM_B(i, rdram_address) = (int8_t)0xFF;
                }
                dma_complete(rdram, mq, physical_addr, size);
            } else {
                // read sram
                save_read(rdram, rdram_address, physical_addr - recomp::sram_base, size);
                dma_complete(rdram, mq, physical_addr, size);
            }
        } else if (physical_addr >= recomp::drive_base) {
            // 64DD region (0x06000000-0x07FFFFFF). No drive attached →
            // return zeros and complete cleanly so libleo proceeds along
            // its no-disk path instead of hanging on a missing DMA done.
            fprintf(stderr, "[pi] Drive read 0x%08X size=0x%X — no drive, returning zeros\n", physical_addr, size);
            dma_complete(rdram, mq, physical_addr, size);
        } else {
            fprintf(stderr, "[WARN] PI DMA read from unknown region, phys address 0x%08X\n", physical_addr);
        }
    } else {
        if (physical_addr >= recomp::rom_base) {
            // write cart rom
            throw std::runtime_error("ROM DMA write unimplemented");
        } else if (flash_region_contains(physical_addr, size)) {
            if (flash_mode == FlashMode::PageProgram) {
                const uint32_t copy_size = std::min<uint32_t>(size, flash_page_size);
                for (uint32_t i = 0; i < copy_size; i++) {
                    flash_write_buffer[i] = MEM_B(i, rdram_address);
                }
                for (uint32_t i = copy_size; i < flash_page_size; i++) {
                    flash_write_buffer[i] = static_cast<char>(0xFF);
                }
            } else {
                static int log_count = 0;
                if (log_count++ < 8) {
                    fprintf(stderr,
                        "[pi] FlashRAM DMA write in mode %d phys=0x%08X size=0x%X -- ignoring\n",
                        static_cast<int>(flash_mode), physical_addr, size);
                }
            }
            dma_complete(rdram, mq, physical_addr, size);
        } else if (physical_addr >= recomp::sram_base) {
            if (!recomp::sram_allowed()) {
                // Same reasoning as the read path — drop drive writes
                // when the configured save type is not SRAM.
                fprintf(stderr, "[pi] Drive/SRAM write 0x%08X size=0x%X — no-device, dropping\n", physical_addr, size);
                dma_complete(rdram, mq, physical_addr, size);
            } else {
                save_write(rdram, rdram_address, physical_addr - recomp::sram_base, size);
                dma_complete(rdram, mq, physical_addr, size);
            }
        } else if (physical_addr >= recomp::drive_base) {
            fprintf(stderr, "[pi] Drive write 0x%08X size=0x%X — no drive, dropping\n", physical_addr, size);
            dma_complete(rdram, mq, physical_addr, size);
        } else {
            fprintf(stderr, "[WARN] PI DMA write to unknown region, phys address 0x%08X\n", physical_addr);
        }
    }
}

extern "C" void osPiStartDma_recomp(RDRAM_ARG recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    uint32_t mb = ctx->r4;
    uint32_t pri = ctx->r5;
    uint32_t direction = ctx->r6;
    uint32_t devAddr = ctx->r7 | recomp::rom_base;
    gpr dramAddr = MEM_W(0x10, ctx->r29);
    uint32_t size = MEM_W(0x14, ctx->r29);
    PTR(OSMesgQueue) mq = MEM_W(0x18, ctx->r29);
    uint32_t physical_addr = k1_to_phys(devAddr);

    debug_printf("[pi] DMA from 0x%08X into 0x%08X of size 0x%08X\n", devAddr, dramAddr, size);

    do_dma(PASS_RDRAM mq, dramAddr, physical_addr, size, direction);

    ctx->r2 = 0;
}

extern "C" void osPiReadIo_recomp(RDRAM_ARG recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // s32 osPiReadIo(u32 devAddr, u32* data). Unlike osEPiReadIo, this
    // interface implicitly targets the cartridge ROM PI handle.
    uint32_t devAddr = ctx->r4 | recomp::rom_base;
    gpr dramAddr = ctx->r5;
    uint32_t physical_addr = k1_to_phys(devAddr);

    if (physical_addr >= recomp::rom_base) {
        recomp::do_rom_pio(PASS_RDRAM dramAddr, physical_addr);
    } else {
        fprintf(stderr,
            "[pi] osPiReadIo from unknown phys 0x%08X -- returning 0\n",
            physical_addr);
        MEM_W(0, dramAddr) = 0;
    }

    ctx->r2 = 0;
}

extern "C" void osEPiStartDma_recomp(RDRAM_ARG recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    OSPiHandle* handle = TO_PTR(OSPiHandle, ctx->r4);
    OSIoMesg* mb = TO_PTR(OSIoMesg, ctx->r5);
    uint32_t direction = ctx->r6;
    uint32_t devAddr = handle->baseAddress | mb->devAddr;
    gpr dramAddr = mb->dramAddr;
    uint32_t size = mb->size;
    PTR(OSMesgQueue) mq = mb->hdr.retQueue;
    uint32_t physical_addr = k1_to_phys(devAddr);

    debug_printf("[pi] DMA from 0x%08X into 0x%08X of size 0x%08X\n", devAddr, dramAddr, size);

    do_dma(PASS_RDRAM mq, dramAddr, physical_addr, size, direction);

    ctx->r2 = 0;
}

extern "C" void osEPiReadIo_recomp(RDRAM_ARG recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    OSPiHandle* handle = TO_PTR(OSPiHandle, ctx->r4);
    uint32_t devAddr = handle->baseAddress | ctx->r5;
    gpr dramAddr = ctx->r6;
    uint32_t physical_addr = k1_to_phys(devAddr);

    if (physical_addr >= recomp::rom_base) {
        // cart rom
        recomp::do_rom_pio(PASS_RDRAM dramAddr, physical_addr);
    } else if (flash_region_contains(physical_addr, sizeof(uint32_t))) {
        uint32_t value = 0;
        if (flash_mode == FlashMode::Status) {
            value = flash_status;
        } else if (flash_mode == FlashMode::Id) {
            value = flash_id_type;
        }
        MEM_W(0, dramAddr) = static_cast<int32_t>(value);
        ctx->r2 = 0;
        return;
    } else if (physical_addr >= 0x05000000 && physical_addr < 0x06000000) {
        // 64DD ASIC register space. With no drive attached the status
        // register MUST report "no disk inserted" so libleo's poll loop
        // bails out instead of scanning the diagnostic RAM forever.
        // The LEO_STATUS register lives at offset 0x508 inside ASIC
        // space; bit 0x800000 set = "DISK_NOT_INSERTED" indicator.
        // We report the no-disk pattern for ALL ASIC reads — libleo
        // gates on the status bits and treats anything else as
        // ignored hardware-quirk noise.
        uint32_t no_disk_status = 0x00800000u;
        MEM_W(0, dramAddr) = (int32_t)no_disk_status;
        ctx->r2 = 0;
        return;
    } else if (physical_addr >= recomp::sram_base) {
        // SRAM/64DD diagnostic RAM PIO read. Same reasoning as the DMA
        // path: when the configured save type is not SRAM, return zeros
        // (already-zero rdram region; just write 0).
        MEM_W(0, dramAddr) = 0;
        ctx->r2 = 0;
        return;
    } else {
        // Genuinely unknown address — keep this loud, it's a missing
        // device handler.
        fprintf(stderr,
            "[pi] osEPiReadIo from unknown phys 0x%08X — returning 0\n",
            physical_addr);
        MEM_W(0, dramAddr) = 0;
    }

    ctx->r2 = 0;
}

extern "C" void osEPiWriteIo_recomp(RDRAM_ARG recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    // s32 osEPiWriteIo(OSPiHandle* handle, u32 devAddr, u32 data)
    // Single-word PIO write to a PI device. ROM is read-only, so writes
    // there are silently dropped. Writes to 64DD register space
    // (0x05000000+) target a peripheral that isn't present in this
    // environment — dropping the write models "no device responding,"
    // which is the behavior libleo expects when polling.
    OSPiHandle* handle = TO_PTR(OSPiHandle, ctx->r4);
    uint32_t devAddr = handle->baseAddress | ctx->r5;
    uint32_t physical_addr = k1_to_phys(devAddr);
    uint32_t data = static_cast<uint32_t>(ctx->r6);

    if (recomp::flashram_allowed() && physical_addr == recomp::sram_base + flash_cmd_reg) {
        switch (data & 0xFF000000) {
        case flash_cmd_chip_erase:
            flash_mode = FlashMode::ChipErase;
            break;
        case flash_cmd_sector_erase:
            flash_pending_erase_page = data & 0x0000FFFF;
            flash_mode = FlashMode::SectorErase;
            break;
        case flash_cmd_execute_erase:
            if (flash_mode == FlashMode::ChipErase) {
                flash_erase_chip();
            } else if (flash_mode == FlashMode::SectorErase) {
                flash_erase_sector(flash_pending_erase_page);
            }
            break;
        case flash_cmd_program_page:
            flash_program_page(data & 0x0000FFFF);
            break;
        case flash_cmd_page_program:
            flash_mode = FlashMode::PageProgram;
            break;
        case flash_cmd_status:
            flash_mode = FlashMode::Status;
            break;
        case flash_cmd_id:
            flash_mode = FlashMode::Id;
            break;
        case flash_cmd_read_array:
            flash_mode = FlashMode::ReadArray;
            break;
        default:
            break;
        }
        ctx->r2 = 0;
        return;
    }

    if (recomp::flashram_allowed() && physical_addr == recomp::sram_base && data == 0) {
        flash_status = 0;
        flash_mode = FlashMode::ReadArray;
        ctx->r2 = 0;
        return;
    }

    ctx->r2 = 0;
}

extern "C" void osPiGetStatus_recomp(RDRAM_ARG recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = 0;
}

extern "C" void osPiRawStartDma_recomp(RDRAM_ARG recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ultramodern::error_handling::message_box(
        "Stub `osPiRawStartDma_recomp` function called!\n"
        "Most games do not call this function directly, which means the libultra function\n"
        "that uses this function was not properly named.\n"
        "\n"
        "If you triggered this message, please make sure you have properly identified\n"
        "every libultra function on your recompiled game. If you are sure every libultra\n"
        "function has been identified and you still get this problem then open an issue on\n"
        "the N64ModernRuntime Github repository mentioning the game you are trying to\n"
        "recompile and steps to reproduce the issue.\n"
        "\n"
        "The application will close now, bye and good luck!"
    );
    ULTRAMODERN_QUICK_EXIT();
}

extern "C" void osEPiRawStartDma_recomp(RDRAM_ARG recomp_context * ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ultramodern::error_handling::message_box(
        "Stub `osEPiRawStartDma_recomp` function called!\n"
        "Most games do not call this function directly, which means the libultra function\n"
        "that uses this function was not properly named.\n"
        "\n"
        "If you triggered this message, please make sure you have properly identified\n"
        "every libultra function on your recompiled game. If you are sure every libultra\n"
        "function has been identified and you still get this problem then open an issue on\n"
        "the N64ModernRuntime Github repository mentioning the game you are trying to\n"
        "recompile and steps to reproduce the issue.\n"
        "\n"
        "The application will close now, bye and good luck!"
    );
    ULTRAMODERN_QUICK_EXIT();
}
