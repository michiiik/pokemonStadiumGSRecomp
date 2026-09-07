cmake_minimum_required(VERSION 3.20)

# Use the classic flag-based MSVC runtime selection (CMP0091 OLD) so the static
# CRT can be forced reliably under direct clang-cl + Ninja. Must be set before
# project(), which initializes the per-config flag variables we rewrite below.
# See the static-CRT block further down for the rationale.
if (WIN32)
    cmake_policy(SET CMP0091 OLD)
endif()

project(PokemonStadiumGSRecomp C CXX)

# Clang 22 rejects fmt's consteval checker in the pinned runtime revision.
# Keep the workspace build compatible with the local clang-cl toolchain.
add_compile_definitions(FMT_USE_CONSTEVAL=0)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything -Wall")
    endif()
endif()
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20.0)
    add_compile_options(-Wno-uninitialized-const-pointer)
endif()
if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# Statically link the MSVC C/C++ runtime so the shipped executable does not
# depend on the Visual C++ redistributable being installed (no MSVCP140.dll /
# VCRUNTIME140.dll to bundle — the #1 cause of "missing DLL" errors on a clean
# machine).
#
# The CMAKE_MSVC_RUNTIME_LIBRARY abstraction (CMP0091 NEW) is not reliably
# honored for the direct clang-cl + Ninja configuration this project uses, so we
# use the classic, bulletproof approach: with CMP0091 OLD (set before project()
# above), the runtime flag lives in CMAKE_*_FLAGS_<CONFIG>; rewrite the
# dynamic-CRT flag (-MD) to the static one (/MT) in every flag variable. Because
# dependencies inherit these flag variables, every target links the static CRT.
if (WIN32)
    foreach(flag_var
            CMAKE_C_FLAGS   CMAKE_C_FLAGS_DEBUG   CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO   CMAKE_C_FLAGS_MINSIZEREL
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        string(REGEX REPLACE "[-/]MDd" "/MTd" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "[-/]MD"  "/MT"  ${flag_var} "${${flag_var}}")
    endforeach()
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Release/windowed build: no console window. Off by default (the console is handy
# for dev — stderr logs). Configure with -DPSR_NO_CONSOLE=ON for the release exe.
option(PSR_NO_CONSOLE "Build a windowed release with no console window" OFF)

# ---- ROM hash check ---------------------------------------------------------
set(EXPECTED_BASEROM_MD5 "1561c75d11cedf356a8ddb1a4a5f9d5d")
if(EXISTS "${CMAKE_SOURCE_DIR}/baserom.z64")
    file(MD5 "${CMAKE_SOURCE_DIR}/baserom.z64" ACTUAL_BASEROM_MD5)
    if(NOT ACTUAL_BASEROM_MD5 STREQUAL EXPECTED_BASEROM_MD5)
        message(WARNING "baserom.z64 MD5 mismatch.\n  expected: ${EXPECTED_BASEROM_MD5}  (US v1.0)\n  actual:   ${ACTUAL_BASEROM_MD5}")
    else()
        message(STATUS "baserom.z64 MD5 OK (US v1.0)")
    endif()
endif()

# ---- RT64 (graphics renderer) ----------------------------------------------
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)

if (ANDROID)
    # Android provides the window, input, and audio host through SDL2. Keep
    # SDL static here; the Android app packages the resulting native library.
    add_compile_definitions(RT64_SDL_WINDOW_VULKAN)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    set(PSR_SDL2_DIR "${CMAKE_SOURCE_DIR}/lib/SDL2" CACHE PATH
        "Path to the SDL2 checkout used for Android builds")
    if (NOT EXISTS "${PSR_SDL2_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Android builds require SDL2 at ${PSR_SDL2_DIR}. "
            "Initialize the lib/SDL2 submodule before configuring Android.")
    endif()
    add_subdirectory(${PSR_SDL2_DIR} ${CMAKE_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# Clang-cl 22 already provides _m_prefetch; skip SDL 2.x's legacy redeclaration.
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" AND TARGET rt64)
    target_compile_definitions(rt64 PRIVATE __PRFCHWINTRIN_H)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # RT64 uses a raw-pointer __try/__except helper; disable C++ object
    # unwinding for this target to avoid MSVC C2712 under /EHsc.
    target_compile_options(rt64 PRIVATE /EHs-c-)
endif()

# hlslpp's scalar backend calls labs() without including <stdlib.h>. AppleClang
# does not make that declaration available through <math.h>, so expose the
# standard declaration only to RT64 on Apple builds.
if (APPLE AND TARGET rt64)
    target_compile_options(rt64 PRIVATE -include stdlib.h)
endif()

if (ANDROID)
    set(PSR_ADRENOTOOLS_DIR "${CMAKE_SOURCE_DIR}/lib/adrenotools" CACHE PATH
        "Path to the libadrenotools checkout")
    if (NOT EXISTS "${PSR_ADRENOTOOLS_DIR}/include/adrenotools/driver.h" OR
        NOT EXISTS "${PSR_ADRENOTOOLS_DIR}/lib/linkernsbypass/CMakeLists.txt")
        message(FATAL_ERROR
            "Android custom graphics drivers require the lib/adrenotools submodule and its nested submodule. "
            "Run git submodule update --init --recursive before configuring Android.")
    endif()
    add_subdirectory(${PSR_ADRENOTOOLS_DIR} ${CMAKE_BINARY_DIR}/adrenotools)
endif()

# ---- UI framework: shared recomp-ui pre-boot ImGui launcher -----------------
# The launcher is wired into the runner target below via
# recomp_target_launcher_ui() (see recomp-ui/recomp_ui.cmake), replacing the old
# vendored RmlUi + lunasvg overlay. Keep static linkage for the engine subdirs.
set(BUILD_SHARED_LIBS OFF)

# ---- N64ModernRuntime (nested submodule) -----------------------------------
set(N64MODERN_RUNTIME_ROOT "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime" CACHE PATH
    "Path to the N64ModernRuntime checkout used by this game")
add_subdirectory(${N64MODERN_RUNTIME_ROOT} ${CMAKE_BINARY_DIR}/lib/N64ModernRuntime)

# The runtime keeps GCC/Clang warning spellings on its portable targets. Plain
# MSVC rejects -Wno-unused-parameter as an invalid numeric /W option (D8021),
# so remove that one inherited flag at the game boundary. Clang-cl accepts the
# Clang spelling and does not need this filter.
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    foreach(runtime_target ultramodern librecomp)
        if (TARGET ${runtime_target})
            get_target_property(runtime_options ${runtime_target} COMPILE_OPTIONS)
            if (runtime_options)
                list(FILTER runtime_options EXCLUDE REGEX "^-Wno-unused-parameter$")
                set_property(TARGET ${runtime_target} PROPERTY COMPILE_OPTIONS
                    "${runtime_options}")
            endif()
            target_compile_options(${runtime_target} PRIVATE
                "/FI${CMAKE_SOURCE_DIR}/include/msvc_compat.h")
        endif()
    endforeach()
endif()
get_target_property(LIBRECOMP_SOURCES librecomp SOURCES)
list(REMOVE_ITEM LIBRECOMP_SOURCES
    "${N64MODERN_RUNTIME_ROOT}/librecomp/src/pi.cpp"
    "${N64MODERN_RUNTIME_ROOT}/librecomp/src/ai.cpp")
set_property(TARGET librecomp PROPERTY SOURCES "${LIBRECOMP_SOURCES}")
target_sources(librecomp PRIVATE
    ${CMAKE_SOURCE_DIR}/mac_pi.cpp
    ${CMAKE_SOURCE_DIR}/mac_ai.cpp)
get_target_property(ULTRAMODERN_SOURCES ultramodern SOURCES)
list(FILTER ULTRAMODERN_SOURCES EXCLUDE REGEX "/mesgqueue\\.cpp$")
set_property(TARGET ultramodern PROPERTY SOURCES "${ULTRAMODERN_SOURCES}")
target_sources(ultramodern PRIVATE ${CMAKE_SOURCE_DIR}/mac_mesgqueue.cpp)

target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

# ---- ares-bridge (oracle for divergence diff against Ares) -----------------
# The ares-bridge subdir is added transitively by librecomp's
# add_subdirectory of N64Recomp, so we don't need our own
# add_subdirectory here. Pass -DWITH_ARES_BRIDGE=ON to opt into the
# real Ares-backed implementation (the placeholder is always built).
# We just declare the include path + link dependency below.

# ---- RecompiledFuncs library (the recompiled game C) -----------------------
add_library(RecompiledFuncs STATIC)
option(PSR_UNITY_BUILD "Build generated game C sources in unity batches" ON)
set_target_properties(RecompiledFuncs PROPERTIES
    UNITY_BUILD ${PSR_UNITY_BUILD}
    UNITY_BUILD_BATCH_SIZE 4
)
if (MSVC)
    # Native MSVC and clang-cl both use COFF objects; the generated unity
    # units can exceed the default section-count limit.
    target_compile_options(RecompiledFuncs PRIVATE
        /bigobj
    )
endif()
if (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(RecompiledFuncs PRIVATE
        /wd4101  # unreferenced local variable in generated C
        /wd4013  # implicit declaration in generated C
    )
else()
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        -Wno-implicit-function-declaration
    )
endif()
target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${N64MODERN_RUNTIME_ROOT}/ultramodern/include
    ${N64MODERN_RUNTIME_ROOT}/librecomp/include
    ${N64MODERN_RUNTIME_ROOT}/N64Recomp/include
)
file(GLOB FUNC_C_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES})

# NOTE: the register-quit/GB-Tower TRACE_ENTRY/RETURN hooks (PKMNSTADIUM_TRACE_HOOKS)
# were removed 2026-06 — Issues #7/#11 are resolved and the per-function [jrdiag]
# stderr spew was the dominant cost of the launcher->game boot stutter. The engine
# still emits TRACE_ENTRY()/TRACE_RETURN() in generated/ (game.toml trace_mode=true),
# but include/trace.h compiles them to no-ops without this define. A future regen
# with trace_mode=false purges the emitted calls entirely.
#
# Opt-in re-enable for crash RE (issues #9/#16 gallery null-vtable): turning
# PSR_TRACE_HOOKS ON defines PKMNSTADIUM_TRACE_HOOKS=1 for the recompiled funcs
# so trace_recent (the guest function-entry ring) records again. The [jrdiag]
# stderr block is now gated to 6 specific function names, so there is no flood.
# Default OFF — release builds stay no-op. Pair with the enlarged TRACE_RING_CAP
# in extras.c so the ring spans multiple frames, not <1 frame.
option(PSR_TRACE_HOOKS "Compile the recompiled funcs with TRACE_ENTRY ring hooks (crash RE)" OFF)
if(PSR_TRACE_HOOKS)
    target_compile_definitions(RecompiledFuncs PRIVATE PKMNSTADIUM_TRACE_HOOKS=1)
    message(STATUS "PSR_TRACE_HOOKS=ON — recompiled funcs record the trace_recent ring")
endif()

# Issue #9 geo probe: compile ONLY funcs_85.c (which defines process_geo_layout)
# with the TRACE_ENTRY hook so extras.c can observe each process_geo_layout entry
# and log its segptr (a1) + caller — splitting Case A (bad reload: first call gets
# a graph-pool segptr) from Case B (double-processing: 2nd call gets the overwritten
# pool ptr). One-file recompile; default OFF. (TRACE_ENTRY in funcs_85.c expands to
# the ring hook only when PKMNSTADIUM_TRACE_HOOKS is defined for that TU.)
option(PSR_GEO_PROBE "Trace-hook funcs_85.c to log process_geo_layout segptrs (#9 RE)" OFF)
if(PSR_GEO_PROBE)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/generated/funcs_85.c
        PROPERTIES COMPILE_DEFINITIONS PKMNSTADIUM_TRACE_HOOKS=1)
    message(STATUS "PSR_GEO_PROBE=ON — funcs_85.c traces process_geo_layout segptrs")
endif()

# ---- Runner executable ------------------------------------------------------
set(PSR_RECOMPUI_SOURCE src/main/recompui_launcher.cpp)
set(PSR_DEBUG_SERVER_SOURCE src/main/debug_server.cpp)
if (APPLE)
    set(PSR_DEBUG_SERVER_SOURCE ${CMAKE_SOURCE_DIR}/debug_server_macos.cpp)
endif()
set(PSR_POST_MORTEM_SOURCE src/main/post_mortem.cpp)
if (NOT WIN32)
    set(PSR_POST_MORTEM_SOURCE ${CMAKE_SOURCE_DIR}/mac_post_mortem_stub.cpp)
endif()
if (ANDROID)
    # The desktop launcher uses native file dialogs and an OpenGL host window.
    # Android uses an Activity-backed launcher later in the port. These
    # minimal host implementations keep the native runner linkable while the
    # Activity integration is added.
    set(PSR_RECOMPUI_SOURCE src/main/recompui_android.cpp)
    set(PSR_DEBUG_SERVER_SOURCE src/main/debug_server_android.cpp)
    set(PSR_POST_MORTEM_SOURCE src/main/post_mortem_android.cpp)
endif()

set(PSR_RUNNER_SOURCES
    src/main/main.cpp
    ${PSR_POST_MORTEM_SOURCE}
    src/main/rt64_render_context.cpp
    ${PSR_DEBUG_SERVER_SOURCE}
    src/main/ares_worker.cpp
    src/main/register_overlays.cpp
    src/main/rsp_aspmain_hook.cpp
    src/main/aspmain_replay.cpp
    src/main/aspmain_refint.cpp
    src/main/transfer_pak.cpp
    src/main/gb_audio.cpp
    src/main/input_bindings.cpp
    src/main/audio_host_probe.cpp
    extras_macos.c
    # Generated lookup table (provides get_entrypoint_address etc).
    generated/lookup.cpp
    # Pokemon Stadium's own aspMain (recompiled from baserom @ 0x68020, 0xC60 bytes).
    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
    ${CMAKE_SOURCE_DIR}/rsp/aspMain_ps2.cpp
    # GB Tower display/copy microcode from fragment 1 (selected by ucode boot signature).
    ${CMAKE_SOURCE_DIR}/rsp/gbTowerMain.cpp
    ${CMAKE_SOURCE_DIR}/rsp/gbTowerColorMain.cpp
    # njpgdspMain is the standard libultra JPEG ucode (vendored from Zelda64Recomp's recompiled output).
    ${CMAKE_SOURCE_DIR}/rsp/njpgdspMain.cpp

    # ---- Pre-boot launcher host glue ---------------------------------------
    # Wraps the shared recomp-ui C ABI (recomp_launcher_run_window); the launcher
    # itself is added to this target by recomp_target_launcher_ui() below.
    ${PSR_RECOMPUI_SOURCE}
)

if (ANDROID)
    add_library(PokemonStadiumGSRecomp SHARED ${PSR_RUNNER_SOURCES})
    target_sources(PokemonStadiumGSRecomp PRIVATE
        src/main/android_battle_companion.cpp
        src/main/android_vulkan_driver.cpp)
    # SDLActivity loads the native runtime as libmain.so.
    set_target_properties(PokemonStadiumGSRecomp PROPERTIES OUTPUT_NAME "main")
else()
    add_executable(PokemonStadiumGSRecomp ${PSR_RUNNER_SOURCES})
    target_sources(PokemonStadiumGSRecomp PRIVATE ${CMAKE_SOURCE_DIR}/mac_link_shims.c)
endif()

target_include_directories(PokemonStadiumGSRecomp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/main
    ${N64MODERN_RUNTIME_ROOT}/ultramodern/include
    ${N64MODERN_RUNTIME_ROOT}/librecomp/include
    ${N64MODERN_RUNTIME_ROOT}/N64Recomp/include
    ${N64MODERN_RUNTIME_ROOT}/thirdparty/concurrentqueue
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_CURRENT_BINARY_DIR}
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # The native MSVC compiler does not understand Clang's -f spellings.
else()
    target_compile_options(PokemonStadiumGSRecomp PRIVATE
        -fno-strict-aliasing
        -fms-extensions
    )
endif()
if (NOT APPLE AND NOT ANDROID AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(PokemonStadiumGSRecomp PRIVATE
        -march=nehalem  # Required by RT64's RSP simulator (SSSE3, SSE4.1, SSE4.2)
    )
endif()

# The runner sources (main.cpp, rt64_render_context.cpp) include RT64's hlslpp
# via Windows headers; without NOMINMAX the min/max macros break hlslpp's min()/
# max() declarations. Scoped to the runner target so the sub-builds are untouched.
target_compile_definitions(PokemonStadiumGSRecomp PRIVATE NOMINMAX)

# Clang-cl 22 already provides _m_prefetch; skip SDL 2.x's legacy redeclaration.
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_definitions(PokemonStadiumGSRecomp PRIVATE __PRFCHWINTRIN_H)
endif()

# Emit PDBs in Release for the runner + librecomp/ultramodern so the SEH
# crash filter's stack walk produces real symbol names. clang-cl needs
# /Z7 (embed CodeView in objects) on the compiler side and /DEBUG on the
# linker side; /OPT:REF + /OPT:NOICF keep dead-code stripping but stop
# IDENTICAL COMDAT folding from collapsing different functions to the
# same address (which would corrupt the stack-walk symbol mapping).
if (MSVC)
    target_compile_options(PokemonStadiumGSRecomp PRIVATE /Z7)
    # Generated C is intentionally left without CodeView by default; embedding
    # symbols in every large translation unit exhausts MSVC memory.
    option(PSR_GENERATED_DEBUG "Embed CodeView symbols in generated C" OFF)
    if (PSR_GENERATED_DEBUG)
        target_compile_options(RecompiledFuncs PRIVATE /Z7)
    endif()
    target_link_options(PokemonStadiumGSRecomp PRIVATE /DEBUG /OPT:REF /OPT:NOICF)
    if (TARGET librecomp)
        target_compile_options(librecomp PRIVATE /Z7)
    endif()
    if (TARGET ultramodern)
        target_compile_options(ultramodern PRIVATE /Z7)
    endif()
endif()

# ---- SDL2 (built from the checked-out submodule on Windows) ----------------
if (WIN32)
    set(PSR_SDL2_DIR "${CMAKE_SOURCE_DIR}/lib/SDL2" CACHE PATH
        "Path to the SDL2 checkout used for Windows builds")
    if (NOT EXISTS "${PSR_SDL2_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Windows builds require SDL2 at ${PSR_SDL2_DIR}. "
            "Initialize the lib/SDL2 submodule before configuring.")
    endif()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${PSR_SDL2_DIR} ${CMAKE_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
        CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # SDL2's clang-cl link can otherwise retain the dynamic CRT defaults
        # even though this project rewrites its compile flags to /MT.
        target_link_options(SDL2 PRIVATE
            "$<$<CONFIG:Debug>:/DEFAULTLIB:libcmtd.lib>"
            "$<$<CONFIG:Debug>:/DEFAULTLIB:libucrtd.lib>"
            "$<$<NOT:$<CONFIG:Debug>>:/DEFAULTLIB:libcmt.lib>"
            "$<$<NOT:$<CONFIG:Debug>>:/DEFAULTLIB:libucrt.lib>"
        )
        target_link_options(PokemonStadiumGSRecomp PRIVATE
            "$<$<CONFIG:Debug>:/DEFAULTLIB:libcmtd.lib>"
            "$<$<CONFIG:Debug>:/DEFAULTLIB:libucrtd.lib>"
            "$<$<NOT:$<CONFIG:Debug>>:/DEFAULTLIB:libcmt.lib>"
            "$<$<NOT:$<CONFIG:Debug>>:/DEFAULTLIB:libucrt.lib>"
        )
    endif()
    target_include_directories(PokemonStadiumGSRecomp PRIVATE ${PSR_SDL2_DIR}/include)

    add_custom_command(TARGET PokemonStadiumGSRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2>"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:PokemonStadiumGSRecomp>
    )

    # No SDL2main: recomp-ui builds with SDL_MAIN_HANDLED, so our real main() is
    # the entry point (SDL.h does not redirect it to SDL_main). opengl32 backs the
    # launcher's GL window (imgui_impl_opengl3 + launcher_gl.c). comdlg32/ole32
    # also back tinyfiledialogs' native file pickers.
    target_link_libraries(PokemonStadiumGSRecomp PRIVATE SDL2 opengl32 Winmm.lib dbghelp.lib comdlg32.lib ole32.lib avrt.lib)
elseif (ANDROID)
    target_link_libraries(PokemonStadiumGSRecomp PRIVATE SDL2-static log android adrenotools)
    add_dependencies(PokemonStadiumGSRecomp
        main_hook file_redirect_hook gsl_alloc_hook hook_impl)
endif()

target_link_libraries(PokemonStadiumGSRecomp PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    ares_bridge
)

if (ANDROID)
    # Android 15/16 devices may use 16 KiB memory pages. NDK r27 does not
    # enable 16 KiB ELF segment alignment by default.
    target_link_options(PokemonStadiumGSRecomp PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384"
    )
endif()

# Force-include all of ares_bridge.lib so the static initializer that
# installs the RSP per-instruction hook (HookInstaller in
# ares_rsp_ring.cpp) is not dropped by /OPT:REF. Without this, lld-link
# strips the unreferenced global and the hook never fires, leaving
# ares_rsp_trace_count stuck at 0 even while Ares runs.
if (MSVC)
    target_link_options(PokemonStadiumGSRecomp PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE:ares_bridge>"
    )
endif()

# Expose the real-Ares oracle's debug-server commands only when the real bridge
# is configured. Without this define the runner references none of the Ares
# trace symbols (which live in the real ares_bridge), so the default
# (placeholder) build links cleanly with the static CRT.
if (WITH_ARES_BRIDGE)
    target_compile_definitions(PokemonStadiumGSRecomp PRIVATE PSR_WITH_ARES=1)
endif()

# Windowed (no-console) release. With SDL_MAIN_HANDLED there is no SDL2main
# WinMain, so force the CRT's console startup (which calls our main()) as the
# entry for the WINDOWS subsystem — this gives a no-console window while keeping
# main() the entry point. stderr still routes to a redirected handle if one is
# set, so logging/diagnostics aren't lost when launched from a terminal.
if (WIN32 AND PSR_NO_CONSOLE)
    target_link_options(PokemonStadiumGSRecomp PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

target_include_directories(PokemonStadiumGSRecomp PRIVATE
    ${N64MODERN_RUNTIME_ROOT}/N64Recomp/ares-bridge/include
)

# ---- Shared recomp-ui pre-boot launcher (Dear ImGui) -----------------------
# Adds the launcher sources (ImGui + tinyfiledialogs + launcher core), include
# dirs, and the RECOMP_LAUNCHER / SDL_MAIN_HANDLED defines to the runner, and
# stages the launcher's assets (fonts/img) next to the exe. See
# recomp-ui/recomp_ui.cmake.
# PSR links rt64 statically, and rt64 already compiles Dear ImGui (its debug
# inspector). Two ImGui copies in one binary collide at link, so tell recomp-ui
# to use rt64's ImGui instead of bundling its own: it compiles only its launcher
# UI against rt64's ImGui headers and links rt64's already-built ImGui core +
# SDL2 backend. rt64 ships but does NOT compile the OpenGL3 backend (it renders
# via Vulkan/D3D12), and recomp-ui's pre-boot window is OpenGL — so we compile
# that one backend here, against rt64's (1.90.x) ImGui, matching its core.
if (NOT ANDROID)
    # The launcher uses RT64's single host ImGui copy to avoid duplicate symbols.
set(RECOMP_UI_HOST_IMGUI ON)
set(RECOMP_UI_HOST_IMGUI_INCLUDE
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui/backends)
    include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
    recomp_target_launcher_ui(PokemonStadiumGSRecomp CONSOLE n64)
    target_sources(PokemonStadiumGSRecomp PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui/backends/imgui_impl_opengl3.cpp)
    # recomp-ui pins the target to C++17; PSR's runner needs C++20.
    set_target_properties(PokemonStadiumGSRecomp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if (MSVC)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/recomp-ui/src/common/backends/imgui/launcher_imgui.cpp PROPERTIES COMPILE_OPTIONS "/Zc:char8_t-")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set_source_files_properties(${CMAKE_SOURCE_DIR}/recomp-ui/src/common/backends/imgui/launcher_imgui.cpp PROPERTIES COMPILE_OPTIONS "-fno-char8_t")
endif()
endif()

# ---- UI assets (fonts, etc.) next to the executable ------------------------
add_custom_command(TARGET PokemonStadiumGSRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:PokemonStadiumGSRecomp>/assets"
)

# The overlay runtime JIT loads TinyCC dynamically. Stage the vendored Windows
# toolchain beside the executable so the runtime can resolve libtcc.dll and its
# libtcc1-64.a/include files without relying on PATH or a developer checkout.
if (WIN32)
    set(PSR_TCC_TOOLCHAIN_DIR "${N64MODERN_RUNTIME_ROOT}/N64Recomp/lib/tcc")
    if (EXISTS "${PSR_TCC_TOOLCHAIN_DIR}/libtcc.dll")
        add_custom_command(TARGET PokemonStadiumGSRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PSR_TCC_TOOLCHAIN_DIR}"
                "$<TARGET_FILE_DIR:PokemonStadiumGSRecomp>/tcc"
            COMMENT "Staging vendored TinyCC toolchain"
        )
    else()
        message(WARNING
            "Bundled TinyCC was not found at ${PSR_TCC_TOOLCHAIN_DIR}; "
            "runtime overlay JIT will be unavailable until the recursive submodules are initialized.")
    endif()
endif()
