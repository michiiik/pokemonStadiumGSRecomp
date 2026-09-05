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

# This game uses the shared sibling workspace layout. Keep shared build
# dependencies in workspace submodules so the checkout remains portable and
# does not depend on a developer's old staging directory.
get_filename_component(PSR_WORKSPACE_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
set(PSR_TOOLCHAIN_ROOT "${PSR_WORKSPACE_ROOT}/toolchain")

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
    set(PSR_SDL2_DIR "${PSR_TOOLCHAIN_ROOT}/SDL2" CACHE PATH
        "Path to the SDL2 checkout used for Android builds")
    if (NOT EXISTS "${PSR_SDL2_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Android builds require SDL2 at ${PSR_SDL2_DIR}. "
            "Add the workspace SDL2 dependency before configuring Android.")
    endif()
    add_subdirectory(${PSR_SDL2_DIR} ${CMAKE_BINARY_DIR}/SDL2 EXCLUDE_FROM_ALL)
endif()

add_subdirectory(${PSR_TOOLCHAIN_ROOT}/rt64 ${CMAKE_BINARY_DIR}/rt64)

if (ANDROID)
    set(PSR_ADRENOTOOLS_DIR "${PSR_TOOLCHAIN_ROOT}/adrenotools" CACHE PATH
        "Path to the libadrenotools checkout")
    if (NOT EXISTS "${PSR_ADRENOTOOLS_DIR}/include/adrenotools/driver.h" OR
        NOT EXISTS "${PSR_ADRENOTOOLS_DIR}/lib/linkernsbypass/CMakeLists.txt")
        message(FATAL_ERROR
            "Android custom graphics drivers require the workspace toolchain/adrenotools checkout and its nested submodule. "
            "Initialize the workspace dependency before configuring Android.")
    endif()
    add_subdirectory(${PSR_ADRENOTOOLS_DIR} ${CMAKE_BINARY_DIR}/adrenotools)
endif()

# ---- UI framework: shared recomp-ui pre-boot ImGui launcher -----------------
# The launcher is wired into the runner target below via
# recomp_target_launcher_ui() (see recomp-ui/recomp_ui.cmake), replacing the old
# vendored RmlUi + lunasvg overlay. Keep static linkage for the engine subdirs.
set(BUILD_SHARED_LIBS OFF)

# ---- N64ModernRuntime (workspace submodule) --------------------------------
set(N64MODERN_RUNTIME_ROOT "${PSR_TOOLCHAIN_ROOT}/N64ModernRuntime" CACHE PATH
    "Path to the N64ModernRuntime checkout used by this game")
add_subdirectory(${N64MODERN_RUNTIME_ROOT} ${CMAKE_BINARY_DIR}/lib/N64ModernRuntime)
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
set_target_properties(RecompiledFuncs PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 4
)
target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)
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
set(PSR_DEBUG_SERVER_SOURCE ${CMAKE_SOURCE_DIR}/debug_server_macos.cpp)
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
    ${PSR_TOOLCHAIN_ROOT}/rt64/src
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/hlslpp/include
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/dxc/inc
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/rhi
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/render
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_options(PokemonStadiumGSRecomp PRIVATE
    -fno-strict-aliasing
    -fms-extensions
)
if (NOT APPLE AND NOT ANDROID)
    target_compile_options(PokemonStadiumGSRecomp PRIVATE
        -march=nehalem  # Required by RT64's RSP simulator (SSSE3, SSE4.1, SSE4.2)
    )
endif()

# The runner sources (main.cpp, rt64_render_context.cpp) include RT64's hlslpp
# via Windows headers; without NOMINMAX the min/max macros break hlslpp's min()/
# max() declarations. Scoped to the runner target so the sub-builds are untouched.
target_compile_definitions(PokemonStadiumGSRecomp PRIVATE NOMINMAX)

# Emit PDBs in Release for the runner + librecomp/ultramodern so the SEH
# crash filter's stack walk produces real symbol names. clang-cl needs
# /Z7 (embed CodeView in objects) on the compiler side and /DEBUG on the
# linker side; /OPT:REF + /OPT:NOICF keep dead-code stripping but stop
# IDENTICAL COMDAT folding from collapsing different functions to the
# same address (which would corrupt the stack-walk symbol mapping).
if (MSVC)
    target_compile_options(PokemonStadiumGSRecomp PRIVATE /Z7)
    # The 1007 generated funcs dominate compile memory/time. Their /Z7 CodeView
    # only feeds the host SEH stack-walk, which is already unsymbolized for the
    # recompiled code and irrelevant to the guest trace_recent ring. In a
    # PSR_TRACE_HOOKS build we diagnose from the guest ring, so skip /Z7 here —
    # it roughly halves per-object memory and avoids clang-cl thrashing/OOM.
    if (NOT PSR_TRACE_HOOKS)
        target_compile_options(RecompiledFuncs   PRIVATE /Z7)
    endif()
    target_link_options(PokemonStadiumGSRecomp PRIVATE /DEBUG /OPT:REF /OPT:NOICF)
    if (TARGET librecomp)
        target_compile_options(librecomp PRIVATE /Z7)
    endif()
    if (TARGET ultramodern)
        target_compile_options(ultramodern PRIVATE /Z7)
    endif()
endif()

# ---- SDL2 (fetched on Windows; standard system package on Linux/Mac) -------
if (WIN32)
    include(FetchContent)
    if (DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    target_include_directories(PokemonStadiumGSRecomp PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(PokemonStadiumGSRecomp PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)

    add_custom_command(TARGET PokemonStadiumGSRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
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
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/imgui
    ${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/imgui/backends)
    include(${PSR_TOOLCHAIN_ROOT}/recomp-ui/recomp_ui.cmake)
    recomp_target_launcher_ui(PokemonStadiumGSRecomp CONSOLE n64)
    target_sources(PokemonStadiumGSRecomp PRIVATE
        ${PSR_TOOLCHAIN_ROOT}/rt64/src/contrib/imgui/backends/imgui_impl_opengl3.cpp)
    # recomp-ui pins the target to C++17; PSR's runner needs C++20.
    set_target_properties(PokemonStadiumGSRecomp PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
set_source_files_properties(${PSR_TOOLCHAIN_ROOT}/recomp-ui/src/common/backends/imgui/launcher_imgui.cpp PROPERTIES COMPILE_OPTIONS "/Zc:char8_t-")
endif()

# ---- UI assets (fonts, etc.) next to the executable ------------------------
add_custom_command(TARGET PokemonStadiumGSRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:PokemonStadiumGSRecomp>/assets"
)
