# Pokémon Stadium 2 Recompilation

This project is a work-in-progress native recompilation of **Pokémon Stadium 2
(US)**. It aims to make the game build and run as a native application while
preserving the original game behavior.

The project combines:

- Stadium 2 decompilation work from the nested [pokestadiumgs](https://github.com/michiiik/pokestadiumgs) fork
- code generation with the nested [N64Recomp](https://github.com/michiiik/N64Recomp) fork
- nested native runtime, renderer, and launcher dependencies

The repository contains game-specific source, configuration, build scripts, and
documentation. Dependencies are pinned as Git submodules inside this repository and provide
the conventional self-contained source layout after the initialization described below.

## ROM policy

A legally obtained Pokémon Stadium 2 ROM is required for local development.
ROMs, saves, generated output, binaries, and machine-specific files must remain
local and must not be committed.

## Build from source

The repository uses nested, pinned submodules. Do not use `--recursive` on the
initial clone: two optional Ares gitlinks currently point to commits that are
not available from the public Ares remote. The default build uses placeholder
mode and does not require Ares.

### Prerequisites

- All platforms: Git, Python 3, CMake, Ninja, and enough disk space for the
  nested dependencies and generated C.
- Windows: Visual Studio 2022 with C++ tools for `N64Recomp`, LLVM/Clang
  (`clang-cl`, `llvm-rc`) and Ninja for the native runner, plus WSL for the
  Stadium 2 disassembly build.
- Linux: LLVM Clang, Make, SDL2 development files, `pkg-config`, Python 3, and
  MIPS binutils (normally commands prefixed `mips-linux-gnu-`).
- macOS: Xcode Command Line Tools (Apple Clang), CMake, Ninja, SDL2,
  `pkg-config`, Python 3, Make, and MIPS binutils. Set
  `MIPS_BINUTILS_PREFIX` if they use a different prefix.

The Linux commands below are source-derived and have not been executed in the
current validation environment. Its WSL Ubuntu installation did not yet have
CMake, Ninja, `clang++`, `pkg-config`/SDL2 metadata, or MIPS binutils installed.

### 1. Clone and initialize dependencies

PowerShell:

```powershell
git clone https://github.com/michiiik/pokemonStadiumGSRecomp.git
Set-Location pokemonStadiumGSRecomp
git submodule update --init
git -C lib/N64ModernRuntime submodule update --init -- N64Recomp
git -C n64recomp config submodule.ares-bridge/third_party/ares.update none
git -C lib/N64ModernRuntime/N64Recomp config submodule.ares-bridge/third_party/ares.update none
git submodule update --init --recursive
```

Linux and macOS:

```bash
git clone https://github.com/michiiik/pokemonStadiumGSRecomp.git
cd pokemonStadiumGSRecomp
git submodule update --init
git -C lib/N64ModernRuntime submodule update --init -- N64Recomp
git -C n64recomp config submodule.ares-bridge/third_party/ares.update none
git -C lib/N64ModernRuntime/N64Recomp config submodule.ares-bridge/third_party/ares.update none
git submodule update --init --recursive
```

The two local `update none` settings skip only the unavailable optional Ares
pins. They do not modify tracked files. All dependencies required by the
default placeholder build are still initialized recursively.

### 2. Build the disassembly ELF

Place a legally obtained, exact Pokémon Stadium 2 US ROM at `baserom.z64`,
then copy it to the disassembly's ignored input location. The expected MD5 is
`1561c75d11cedf356a8ddb1a4a5f9d5d`.

The Stadium 2 disassembly does not support native Windows. Run this stage in
WSL from the repository mounted under `/mnt`:

```bash
mkdir -p disasm/baseroms/us
cp baserom.z64 disasm/baseroms/us/baserom.z64
make -C disasm init
make -C disasm -j"$(nproc)"
test -f disasm/build/pokestadiumgs-us.elf
```

Linux uses the same commands. On macOS, replace `$(nproc)` with
`$(sysctl -n hw.logicalcpu)`. The required output is
`disasm/build/pokestadiumgs-us.elf`.

### 3. Build the N64Recomp CLI

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S n64recomp -B n64recomp/build-vs -G "Visual Studio 17 2022" -A x64
cmake --build n64recomp/build-vs --config Release --target N64RecompCLI
```

The Windows executable is
`n64recomp/build-vs/Release/N64Recomp.exe`.

Linux:

```bash
cmake -S n64recomp -B n64recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build n64recomp/build --target N64RecompCLI
```

macOS:

```bash
cmake -S n64recomp -B n64recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(xcrun --find clang)" \
  -DCMAKE_CXX_COMPILER="$(xcrun --find clang++)"
cmake --build n64recomp/build --target N64RecompCLI
```

The Linux/macOS executable is `n64recomp/build/N64Recomp`.

### 4. Generate the recompiled C sources

PowerShell:

```powershell
& .\n64recomp\build-vs\Release\N64Recomp.exe game.toml
if ($LASTEXITCODE -ne 0) { throw "N64Recomp generation failed" }
```

Linux and macOS:

```bash
./n64recomp/build/N64Recomp game.toml
```

This writes the ignored `generated/` directory. Generation is currently known
to exit nonzero because hook `Memmap_RelocateFragment` is missing. If
N64Recomp exits nonzero, any files it left in `generated/` are incomplete and
must not be treated as valid build input.

### 5. Configure and build the native runner

Windows, from a shell where `clang-cl`, `llvm-rc`, Ninja, and the Visual Studio
linker environment are available:

```powershell
cmake -S . -B build-native -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_RC_COMPILER=llvm-rc
cmake --build build-native --target PokemonStadiumGSRecomp --parallel 2
```

The intended Windows executable is `build-native/PokemonStadiumGSRecomp.exe`.

Linux:

```bash
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-native --target PokemonStadiumGSRecomp --parallel 2
```

macOS:

```bash
cmake -S . -B build-native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$(xcrun --find clang)" \
  -DCMAKE_CXX_COMPILER="$(xcrun --find clang++)"
cmake --build build-native --target PokemonStadiumGSRecomp --parallel 2
```

The intended Linux/macOS executable is `build-native/PokemonStadiumGSRecomp`.

### Android

Install Android Studio or equivalent command-line tools with SDK Platform 35,
NDK `27.2.12479018`, CMake `3.22.1`, JDK 17 or newer, and Gradle 8.9. No Gradle
wrapper is committed, so `gradle` must be available on `PATH` (or invoke the
matching installation directly).

After the dependency initialization above, run from the repository root in
PowerShell:

```powershell
gradle -p android assembleDebug -PpsrRom=C:\path\to\legally-obtained-stadium2-us.z64
```

The APK output is
`android/app/build/outputs/apk/debug/app-debug.apk`. The target is
`arm64-v8a`, with minimum SDK 26 and target SDK 35. The ROM is staged into APK
assets locally for this build and must never be committed.

Measured Android validation passed resource, asset, and manifest processing and
reached the arm64 CMake configure using NDK Clang 18. It currently stops because
RT64's `nativefiledialog` requests host `PkgConfig` during Android configure.
No device test was needed or performed because the APK was not produced.

### Current validation status (September 2026)

These are measured results, not a claim that the full source pipeline succeeds:

| Stage | Measured result |
|-------|-----------------|
| Nested dependencies, Windows | Top-level pins and every required non-Ares recursive module initialized successfully. |
| Nested dependencies, macOS | Fresh clones reproduced the same result; only the two deliberately skipped Ares paths remained uninitialized. |
| Linux | Commands are source-derived and unexecuted; the available WSL environment lacks the prerequisites listed above. |
| N64Recomp CLI, Windows | Built successfully with Visual Studio 2022. |
| Code generation | Exits nonzero because hook `Memmap_RelocateFragment` is missing; partial `generated/` output is invalid. |
| Native configure, Windows | CMake/Ninja configure passed with clang-cl 22. |
| Native build, Windows clang-cl | Stops in pinned fmt consteval handling; no full build success is claimed. |
| Native configure, macOS | Passed with Apple Clang 21, CMake 4.4, Ninja, SDL2, and a generated snapshot. |
| Native build, macOS | Stops around target 83-87 in pinned `fmt/src/os.cc` consteval errors (first seen at lines 172, 218, and 287). No later blocker was reached. |
| Android | Gradle 8.9 reached arm64 CMake with NDK Clang 18, then stopped at RT64 `nativefiledialog` host `PkgConfig`; no APK or device test. |

## License

The project code is distributed under GPL-3.0; see `COPYING`. Bundled fonts,
controller mappings, and inherited launcher artwork are documented in
`THIRD_PARTY.md` and `licenses/`.

No ROMs, saves, extracted game data, or generated recompiler output are included.
