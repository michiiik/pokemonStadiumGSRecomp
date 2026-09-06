# Pokémon Stadium 2 Recompilation

This project is a work-in-progress native recompilation of **Pokémon Stadium 2
(US)**. It aims to make the game build and run as a native application while
preserving the original game behavior.

The project combines:

- Stadium 2 decompilation work from the nested [pokestadiumgs](https://github.com/michiiik/pokestadiumgs) fork
- code generation with the nested [N64Recomp](https://github.com/michiiik/N64Recomp) fork
- nested native runtime, renderer, and launcher dependencies

The repository contains game-specific source, configuration, build scripts, and
documentation. Dependencies are pinned as Git submodules inside this repository
so a recursive clone provides the conventional self-contained source layout.

## ROM policy

A legally obtained Pokémon Stadium 2 ROM is required for local development.
ROMs, saves, generated output, binaries, and machine-specific files must remain
local and must not be committed.

## Build instructions

Clone recursively and run the setup check from the repository root:

```bash
git clone --recursive https://github.com/michiiik/pokemonStadiumGSRecomp.git
cd pokemonStadiumGSRecomp
./setup.sh
```

If the repository was cloned without submodules, initialize them with:

```bash
git submodule update --init --recursive
```

Generated recompiler sources are intentionally not tracked. On a fresh clone,
run the documented generation pipeline before configuring CMake, or provide a
local `generated/` tree; otherwise configuration fails because
`generated/lookup.cpp` is missing.

### Windows

Install Git, CMake 3.22 or newer, Ninja, and a supported C/C++ compiler
(Visual Studio 2022 or Clang on Windows). From the repository root:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PokemonStadiumGSRecomp --parallel 2
```

The executable is written below `build`.

### Android

Install Android Studio with the Android SDK, SDK Platform 35, NDK
27.2.12479018, CMake, and a JDK 17 or newer. Set `ANDROID_HOME` or
`ANDROID_SDK_ROOT`, initialize the recursive submodules, and build from the
Android project:

```powershell
git submodule update --init --recursive
cd android
gradle assembleDebug -PpsrRom=C:\path\to\pokemon-stadium-2.z64
```

The debug APK is produced under `android/app/build/outputs/apk`. If Gradle
cannot find the SDK, create `android/local.properties` with an `sdk.dir=`
entry pointing to the installed SDK. The ROM is passed to Gradle and is never
copied into the repository.

### macOS

Install Xcode Command Line Tools, Git, CMake, Ninja, and the Vulkan SDK. From
the repository root:

```bash
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target PokemonStadiumGSRecomp --parallel 2
```

The macOS build requires the platform libraries used by RT64. Supply the ROM at
launch; do not add it to the repository.

This project is under active development; Android and macOS remain porting
targets while the Windows native build is brought up.

## Validation status (2026-09-06)

The following results were recorded against merge commit
`e02da8142920994b055d50d5f7850f25f5364726`:

- On a clean Windows clone, `setup.bat` passed with all seven required
  dependencies. The optional Ares checkout was skipped.
- Canonical Windows CMake configuration reached MSVC detection but was blocked
  only because CMake could not download SDL2 from GitHub in the restricted
  environment. Offline configuration with a local SDL2 checkout passed. Full
  builds were not completed: the MSVC build failed at an unguarded
  `-Wno-unused-parameter`, and the ClangCL build failed with fmt `consteval`
  errors.
- Android `assembleDebug` was attempted with the external US Stadium 2 ROM
  (MD5 `1561c75d11cedf356a8ddb1a4a5f9d5d`), Gradle 8.9, JDK 21, and NDK
  `27.2.12479018`. AGP/network dependency resolution blocked the first
  attempt; the offline retry then failed because `native-platform.dll` was
  missing. No Android build success is claimed.
- The Mac mini was not rerun because remote SSH/Tailscale access was
  unavailable in this session. From the repository root, rerun:

  ```sh
  git submodule update --init --recursive
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target PokemonStadiumGSRecomp --parallel 2
  ```

## License

The project code is distributed under GPL-3.0; see `COPYING`. Bundled fonts,
controller mappings, and inherited launcher artwork are documented in
`THIRD_PARTY.md` and `licenses/`.

No ROMs, saves, extracted game data, or generated recompiler output are included.
