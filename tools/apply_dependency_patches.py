#!/usr/bin/env python3
"""Apply third-party dependency patches for the PokemonStadiumGSRecomp Windows build.

The runtime/engine fixes below live inside third-party submodules that
re-clone to their pinned commit on every `git submodule update`, so the deltas
cannot be committed into them from this repository (and one of them is an
upstream tree we deliberately do not touch). This script re-applies them and
is safe to re-run.

The repo-owned fixes that used to be separate patches (register_overlays.cpp
trampolines, main.cpp Windows watchpage, CMakeLists Stadium-2 wiring, the
aspMain_ps2 audio ucode) are already committed in this repository, so they are
intentionally absent here -- only submodule-targeted deltas remain.

Run from the repo root (setup.bat invokes this):
    py -3 tools/apply_dependency_patches.py
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

_COMBINED = r'''diff --git a/src/decompressed.cpp b/src/decompressed.cpp
index 05ab587..50c3ba7 100644
--- a/src/decompressed.cpp
+++ b/src/decompressed.cpp
@@ -796,7 +796,14 @@ size_t add_decompressed_section(Context& context,
 
     // Cumulative synthetic-rom counter. Aligned to 4 bytes so MIPS
     // instruction reads are always aligned.
-    static uint64_t next_synthetic_rom = 0xFE000000ull;
+    // Sandbox/low-RAM: pool base lowered from 0xFE000000 to 0x10000000.
+    // context.rom is a dense std::vector; starting the synthetic pool at
+    // 0xFE000000 forces a ~4.3 GB resize on the first variant (and a ~2x
+    // realloc transient on every growth), which bad_allocs on 8 GB
+    // machines (incl. default WSL2). 0x10000000 (256 MB) is still above
+    // every real ROM offset (<= 64 MB) and nothing keys on the 0xFE
+    // prefix. Changes only synthetic rom_addr metadata in generated/.
+    static uint64_t next_synthetic_rom = 0x10000000ull;
     const uint32_t synthetic_rom = uint32_t(next_synthetic_rom);
     next_synthetic_rom += (uint64_t(reloc_offset) + 3u) & ~uint64_t(3u);
     if (next_synthetic_rom > 0xFFFFFFFFull) {
diff --git a/src/elf.cpp b/src/elf.cpp
index f8776dd..41840ee 100644
--- a/src/elf.cpp
+++ b/src/elf.cpp
@@ -139,9 +139,46 @@ bool parse_section_relocs(N64Recomp::Context& context, const ELFIO::elfio& elf_f
             }
         }
         else if (rel_symbol_section_index == ELFIO::SHN_ABS) {
+            // Hand-port of pokemonStadiumGSRecompPrivate
+            // patches/n64recomp-abs-bss-fragment-reloc.patch to the refactored
+            // engine: a fragment's bss can be absent from the linker script
+            // (splat yamls that declare bss_size: 0x0 while the fragment
+            // header's sizeInRam - fileSize says otherwise). Symbols living
+            // there fall outside every section and arrive as SHN_ABS, which
+            // would drop the relocation and bake the link-time literal -
+            // fatal when the symbol is a loop bound compared against a
+            // relocated pointer. If the value sits in a relocatable section's
+            // 1 MB fragment slot, relocate against that section instead.
+            // Genuine absolutes (RSP/DP hardware registers and the like)
+            // never share a slot with a fragment's link address, and
+            // non-relocatable sections are left alone.
+            size_t abs_owner_section = (size_t)-1;
+            uint32_t abs_value = (uint32_t)rel_symbol_value;
+            for (size_t abs_i = 0; abs_i < context.sections.size(); abs_i++) {
+                const auto& abs_sec = context.sections[abs_i];
+                if (!abs_sec.relocatable) {
+                    continue;
+                }
+                uint32_t abs_sec_vram = (uint32_t)abs_sec.ram_addr;
+                if (abs_sec_vram == 0) {
+                    continue;
+                }
+                if ((abs_sec_vram & 0xFFF00000u) == (abs_value & 0xFFF00000u) &&
+                    abs_value >= abs_sec_vram) {
+                    abs_owner_section = abs_i;
+                    break;
+                }
+            }
+
             reloc_out.reference_symbol = false;
-            reloc_out.target_section = N64Recomp::SectionAbsolute;
-            rel_section_vram = 0;
+            if (abs_owner_section != (size_t)-1) {
+                reloc_out.target_section = (uint16_t)abs_owner_section;
+                rel_section_vram = context.sections[abs_owner_section].ram_addr;
+            }
+            else {
+                reloc_out.target_section = N64Recomp::SectionAbsolute;
+                rel_section_vram = 0;
+            }
         }
         else {
             reloc_out.reference_symbol = false;
diff --git a/src/main.cpp b/src/main.cpp
index 8a9a705..7059d5e 100644
--- a/src/main.cpp
+++ b/src/main.cpp
@@ -1910,9 +1910,18 @@ int main(int argc, char** argv) {
 
     std::vector<std::vector<uint32_t>> static_funcs_by_section{ context.sections.size() };
     seed_static_entrypoints_from_code_relocs(context, static_funcs_by_section);
-    seed_static_entrypoints_from_pointer_tables(context, static_funcs_by_section);
-    seed_static_entrypoints_from_code_relocs(context, static_funcs_by_section);
-    seed_static_branch_continuations_from_indirect_dispatch_entries(context, static_funcs_by_section);
+
+    // Pokemon Stadium 2 has large tables containing values that resemble code
+    // pointers. Aggressively materializing every inferred interior entry emits
+    // gigabytes of duplicate function bodies. The runtime's lookup-miss tier
+    // can discover these entries on demand, so allow this offline expansion to
+    // be disabled for the Stadium 2 build without changing the default.
+    if (std::getenv("N64RECOMP_SKIP_AGGRESSIVE_STATIC_SEEDS") == nullptr) {
+        seed_static_entrypoints_from_pointer_tables(context, static_funcs_by_section);
+        seed_static_entrypoints_from_code_relocs(context, static_funcs_by_section);
+        seed_static_branch_continuations_from_indirect_dispatch_entries(context, static_funcs_by_section);
+    }
+
     seed_static_entrypoints_from_code_relocs(context, static_funcs_by_section);
 
     fmt::print("Working dir: {}\n", std::filesystem::current_path().string());
diff --git a/src/recompilation.cpp b/src/recompilation.cpp
index a395a96..f0abca2 100644
--- a/src/recompilation.cpp
+++ b/src/recompilation.cpp
@@ -128,6 +128,9 @@ JalResolutionResult resolve_jal(const N64Recomp::Context& context, size_t cur_se
         }
         // No symbol there — synthesize a static function at that address.
         else {
+            if (std::getenv("N64RECOMP_SKIP_AGGRESSIVE_STATIC_SEEDS") != nullptr) {
+                return JalResolutionResult::Ambiguous;
+            }
             return JalResolutionResult::CreateStatic;
         }
     }
@@ -174,6 +177,9 @@ JalResolutionResult resolve_jal(const N64Recomp::Context& context, size_t cur_se
                 }
 
                 static_section_index = containing_section;
+                if (std::getenv("N64RECOMP_SKIP_AGGRESSIVE_STATIC_SEEDS") != nullptr) {
+                    return JalResolutionResult::Ambiguous;
+                }
                 return JalResolutionResult::CreateStatic;
             }
 
'''

_RUNTIME = r'''--- a/librecomp/src/pi.cpp
+++ b/librecomp/src/pi.cpp
@@ -282,7 +282,12 @@
 
 extern "C" void osCreatePiManager_recomp(uint8_t* rdram, recomp_context* ctx) {
     LIBRECOMP_ULTRA_TRACE(ctx);
-    ;
+    osCreateMesgQueue(rdram, (int32_t)ctx->r5, (int32_t)ctx->r6, (s32)ctx->r7);
+
+    // Stadium 2's streamed-ROM loader uses this private PI completion queue.
+    // The shared runtime intentionally leaves the PI manager unimplemented,
+    // so establish the queue here as part of the manager initialization.
+    osCreateMesgQueue(rdram, 0x800CD000, 0x800CD018, 1);
 }
 
 void recomp::do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes) {
--- a/librecomp/src/ai.cpp
+++ b/librecomp/src/ai.cpp
@@ -72,6 +72,19 @@
 
 extern "C" void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
     LIBRECOMP_ULTRA_TRACE(ctx);
+
+    // Stadium 2's startup path can reach audio setup before its video-mode
+    // initializer. This field is the selected VI rate (50 or 60); the US ROM
+    // is NTSC, so the hardware-consistent fallback is 60 Hz.
+    constexpr uint32_t kStadium2ViRate = 0x8011EB94;
+    if (MEM_W(0, kStadium2ViRate) == 0) {
+        MEM_W(0, kStadium2ViRate) = 60;
+    }
+    // The audio frame scheduler divides by this halfword immediately after
+    // osAiSetFrequency. A missing producer leaves it zero during early boot.
+    if (MEM_H(0, 0x8011EA6C) == 0) {
+        MEM_H(0, 0x8011EA6C) = 1;
+    }
     uint32_t freq = ctx->r4;
     // This makes actual audio frequency more accurate to console, but may not be desirable
     //uint32_t dacRate = (uint32_t)(((float)VI_NTSC_CLOCK / freq) + 0.5f);
--- a/librecomp/src/ultra_translation.cpp
+++ b/librecomp/src/ultra_translation.cpp
@@ -176,19 +176,37 @@
     ;
 }
 
+// Hand-port of pokemonStadiumGSRecompPrivate
+// patches/librecomp-int-mask-return-value.patch: interrupts are not
+// simulated (guest execution is cooperative), so the mask itself has no
+// effect. The RETURN VALUE does matter: osSetIntMask and __osDisableInt are
+// documented to return the previous mask, and guest code stores that value
+// and passes it back to restore a critical section. Leaving v0 untouched
+// returned whatever the previous call left there, which the game can then
+// use as a pointer or a count. Track the mask per guest thread (each guest
+// thread owns a host thread, so thread_local is per-OSThread) and return
+// the real previous value.
+static constexpr uint32_t OS_IM_NONE_VALUE = 0x00000001u;
+static constexpr uint32_t OS_IM_ALL_VALUE = 0x003FFF01u;
+static thread_local uint32_t guest_int_mask = OS_IM_ALL_VALUE;
+
 extern "C" void osSetIntMask_recomp(uint8_t * rdram, recomp_context * ctx) {
     LIBRECOMP_ULTRA_TRACE(ctx);
-    ;
+    const uint32_t previous = guest_int_mask;
+    guest_int_mask = (uint32_t)ctx->r4;
+    ctx->r2 = (int32_t)previous;
 }
 
 extern "C" void __osDisableInt_recomp(uint8_t * rdram, recomp_context * ctx) {
     LIBRECOMP_ULTRA_TRACE(ctx);
-    ;
+    const uint32_t previous = guest_int_mask;
+    guest_int_mask = OS_IM_NONE_VALUE;
+    ctx->r2 = (int32_t)previous;
 }
 
 extern "C" void __osRestoreInt_recomp(uint8_t * rdram, recomp_context * ctx) {
     LIBRECOMP_ULTRA_TRACE(ctx);
-    ;
+    guest_int_mask = (uint32_t)ctx->r4;
 }
 
 extern "C" void __osSetFpcCsr_recomp(uint8_t * rdram, recomp_context * ctx) {
--- a/ultramodern/src/mesgqueue.cpp
+++ b/ultramodern/src/mesgqueue.cpp
@@ -692,6 +692,13 @@
 }
 
 extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
+    // Do not form a host pointer until the guest queue address is validated.
+    // do_send performs the deeper queue-state validation below.
+    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
+        fprintf(stderr, "[osSendMesg] invalid guest queue 0x%08X; dropping message\n", uint32_t(mq_));
+        return -1;
+    }
+
     OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
     bool jam = false;
 
@@ -720,6 +727,13 @@
 }
 
 extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
+    // Do not form a host pointer until the guest queue address is validated.
+    // do_send performs the deeper queue-state validation below.
+    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
+        fprintf(stderr, "[osJamMesg] invalid guest queue 0x%08X; dropping message\n", uint32_t(mq_));
+        return -1;
+    }
+
     OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
     bool jam = true;
 
@@ -743,6 +757,13 @@
 }
 
 extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
+    // Do not form a host pointer until the guest queue address is validated.
+    // do_recv performs the deeper queue-state validation below.
+    if (uint32_t(mq_) < 0x80000000u || uint32_t(mq_) >= 0x80800000u) {
+        fprintf(stderr, "[osRecvMesg] invalid guest queue 0x%08X; dropping receive\n", uint32_t(mq_));
+        return -1;
+    }
+
     OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
 
     assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");
'''

_FRAGMENT_BASE = r'''diff --git a/include/recomp.h b/include/recomp.h
index 1f474f2..d48adc9 100644
--- a/include/recomp.h
+++ b/include/recomp.h
@@ -590,6 +590,16 @@ struct recomp_context {
     // prologue. _target carries the rejected entry PC (interpreter + telemetry).
     uint32_t dispatch_entry_rejected;
     uint32_t dispatch_entry_rejected_target;
+    // Stadium 2 fragment-runtime base (pokemonStadiumGSRecompPrivate): the
+    // runtime fragment's load base while a fragment body executes, saved /
+    // cleared by the Memmap_RelocateFragment / Memmap_GetFragmentVaddr
+    // hooks in game.toml and consumed by librecomp's overlay resolution
+    // (overlays.cpp). 0 when no runtime fragment is active.
+    uint32_t fragment_runtime_base;
+    // Section index of the active runtime fragment (same lifecycle as
+    // fragment_runtime_base): recomp_section_address returns the live base
+    // only for this section while the fragment body executes.
+    uint32_t fragment_section_index;
 };
 
 // Asserts the FPR index is legal: either even, or any index once the context
'''

_RECOMP_UI_U8 = r'''diff --git a/src/common/backends/imgui/launcher_imgui.cpp b/src/common/backends/imgui/launcher_imgui.cpp
index 048ac8a..ea171d9 100644
--- a/src/common/backends/imgui/launcher_imgui.cpp
+++ b/src/common/backends/imgui/launcher_imgui.cpp
@@ -5473,7 +5473,7 @@ static void draw_lobby_seat_row(LauncherModel* m,
                 const bool pressed =
                     ImGui::Button("##kick", ImVec2(kick_btn, kick_btn));
                 {
-                    const char* boot = u8"\U0001F97E";
+                    const char* boot = "\xF0\x9F\xA5\xBE"; // U+1F97E boot emoji, UTF-8 bytes (u8 literal would be char8_t in C++20)
                     const ImVec2 rmin = ImGui::GetItemRectMin();
                     const ImVec2 rmax = ImGui::GetItemRectMax();
                     const ImVec2 ts = ImGui::CalcTextSize(boot);
@@ -5494,7 +5494,7 @@ static void draw_lobby_seat_row(LauncherModel* m,
                 const ImVec2 rmax = ImGui::GetItemRectMax();
                 dl->AddRect(rmin, rmax, ImGui::GetColorU32(ImGuiCol_Border),
                             ImGui::GetStyle().FrameRounding);
-                const char* boot = u8"\U0001F97E";
+                const char* boot = "\xF0\x9F\xA5\xBE"; // U+1F97E boot emoji, UTF-8 bytes (u8 literal would be char8_t in C++20)
                 const ImVec2 ts = ImGui::CalcTextSize(boot);
                 dl->AddText(ImVec2((rmin.x + rmax.x - ts.x) * 0.5f,
                                    (rmin.y + rmax.y - ts.y) * 0.5f),
@@ -5913,7 +5913,7 @@ void draw_netplay_room_modal(LauncherModel* m, const LauncherTheme& th) {
                            ? "Waiting for every player to install this "
                              "lobby's mods"
                            : nullptr);
-            if (ImGui::Button(u8"\u25B6 Play", ImVec2(play_w, btn_h))) {
+            if (ImGui::Button("\xE2\x96\xB6 Play", ImVec2(play_w, btn_h))) {
                 /* Ensure engine match_caps.rollback matches UI before start. */
                 if (np->rollback_set)
                     (void)np->rollback_set(np->ctx, m->netplay_rollback ? 1 : 0);
'''

_RT64_IMGUI = r'''diff --git a/src/gui/rt64_inspector.cpp b/src/gui/rt64_inspector.cpp
index 15fdce6..59fa8b9 100644
--- a/src/gui/rt64_inspector.cpp
+++ b/src/gui/rt64_inspector.cpp
@@ -107,7 +107,10 @@ namespace RT64 {
         case UserConfiguration::GraphicsAPI::Vulkan: {
             VulkanDevice *interfaceDevice = static_cast<VulkanDevice *>(device);
             const VulkanSwapChain *interfaceSwapChain = static_cast<const VulkanSwapChain *>(swapChain);
-            ImGui_ImplVulkan_LoadFunctions([](const char *functionName, void *vulkanInstance) {
+            // imgui 1.91.x LoadFunctions takes the Vulkan api_version first;
+            // this path uses a render pass (not dynamic rendering), so 1.0 core
+            // functions are all the backend loads from it.
+            ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, [](const char *functionName, void *vulkanInstance) {
                 return vkGetInstanceProcAddr(*(reinterpret_cast<VkInstance *>(vulkanInstance)), functionName);
             }, &interfaceDevice->renderInterface->instance);
 
diff --git a/src/imgui/imgui_impl_sdl2_custom.cpp b/src/imgui/imgui_impl_sdl2_custom.cpp
index 3edf457..0853a1a 100644
--- a/src/imgui/imgui_impl_sdl2_custom.cpp
+++ b/src/imgui/imgui_impl_sdl2_custom.cpp
@@ -148,7 +148,7 @@ static void ImGui_ImplSDL2_SetClipboardText(void*, const char* text)
 }
 
 // Note: native IME will only display if user calls SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1") _before_ SDL_CreateWindow().
-static void ImGui_ImplSDL2_SetPlatformImeData(ImGuiViewport*, ImGuiPlatformImeData* data)
+static void ImGui_ImplSDL2_SetPlatformImeData(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data)
 {
     if (data->WantVisible)
     {
@@ -418,7 +418,7 @@ static bool ImGui_ImplSDL2_Init(SDL_Window* window, SDL_Renderer* renderer)
     io.SetClipboardTextFn = ImGui_ImplSDL2_SetClipboardText;
     io.GetClipboardTextFn = ImGui_ImplSDL2_GetClipboardText;
     io.ClipboardUserData = nullptr;
-    io.SetPlatformImeDataFn = ImGui_ImplSDL2_SetPlatformImeData;
+    ImGui::GetPlatformIO().Platform_SetImeDataFn = ImGui_ImplSDL2_SetPlatformImeData;
 
     // Load mouse cursors
     bd->MouseCursors[ImGuiMouseCursor_Arrow] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
'''


# (submodule path relative to repo root, human-readable label, unified diff)
PATCHES = [
    (
        "n64recomp",
        "N64Recomp engine: synthetic ROM pool base, SHN_ABS fragment relocation, "
        "static-seeds gate, JAL lookup",
        _COMBINED,
    ),
    (
        "lib/N64ModernRuntime",
        "N64ModernRuntime: PI queue, AI VI-rate guard, int-mask return value, "
        "mesg-queue guard",
        _RUNTIME,
    ),
    (
        "lib/N64ModernRuntime/N64Recomp",
        "nested N64Recomp: fragment_runtime_base / fragment_section_index on "
        "recomp_context",
        _FRAGMENT_BASE,
    ),
    (
        "lib/rt64",
        "rt64: imgui 1.91.x LoadFunctions / IME-hook compatibility",
        _RT64_IMGUI,
    ),
    (
        "recomp-ui",
        "recomp-ui: rewrite u8 string literals to UTF-8 byte escapes",
        _RECOMP_UI_U8,
    ),
]


def apply_patch(submodule_rel, label, patch_text):
    target = REPO_ROOT / submodule_rel
    if not (target / ".git").exists():
        print("[skip] %s: not a checked-out repo (missing .git)" % submodule_rel)
        return False

    with tempfile.NamedTemporaryFile(
        "w", suffix=".patch", delete=False, encoding="utf-8"
    ) as fh:
        fh.write(patch_text)
        tmp = fh.name

    try:
        rev = subprocess.run(
            ["git", "-C", str(target), "apply", "--check", "--reverse",
             "--whitespace=nowarn", tmp],
            capture_output=True, text=True,
        )
        if rev.returncode == 0:
            print("[ok   ] %-38s already applied  (%s)" % (submodule_rel, label))
            return True

        fwd = subprocess.run(
            ["git", "-C", str(target), "apply", "--whitespace=nowarn", tmp],
            capture_output=True, text=True,
        )
        if fwd.returncode == 0:
            print("[apply] %-38s applied        (%s)" % (submodule_rel, label))
            return True

        print("[FAIL ] %-38s   (%s)" % (submodule_rel, label))
        sys.stderr.write((fwd.stderr or "").strip() + "\n")
        return False
    finally:
        Path(tmp).unlink(missing_ok=True)


def main():
    ok = True
    for rel, label, patch_text in PATCHES:
        if not apply_patch(rel, label, patch_text):
            ok = False
    print()
    if ok:
        print("All dependency patches present.")
        return 0
    print("One or more patches failed to apply (context drift or partial "
          "application). See the messages above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
