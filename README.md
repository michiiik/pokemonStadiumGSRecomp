# Pokémon Stadium 2 Recompilation

This project is a work-in-progress native recompilation of **Pokémon Stadium 2
(US)**. It aims to make the game build and run as a native application while
preserving the original game behavior.

The project combines:

- Stadium 2 decompilation work based on [`pokestadiumgs`](https://github.com/michiiik/pokestadiumgs)
- Code generation with [`N64Recomp`](https://github.com/michiiik/N64Recomp)
- A native runtime and renderer maintained in the workspace toolchain

The repository contains project-specific source, configuration, build scripts,
and documentation. The current build layout expects the shared public
dependency repositories to be checked out as Git submodules in the sibling
`decomp/` and `toolchain/` directories of the workspace. Run `setup.sh` from
this repository to verify those paths without creating nested dependency copies.

## ROM policy

A legally obtained Pokémon Stadium 2 ROM is required for local development.
ROMs, saves, generated output, binaries, and machine-specific files must remain
local and must not be committed.

## Build instructions

Run these commands from the workspace root (the directory containing `games/`
and `toolchain/`). ROMs, saves, APKs, and build output stay outside Git.

### Windows

Install Git, CMake 3.22 or newer, Ninja, and a supported C/C++ compiler
(Visual Studio 2022 or Clang on Windows). Initialize the workspace
dependencies, then configure and build:

```powershell
git submodule update --init --recursive
cmake -S games/pokemonStadiumGSRecomp -B build/games/stadium2 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/games/stadium2 --target PokemonStadiumGSRecomp --parallel 2
```

The executable is written below `build\games\stadium2`.

### Android

Install Android Studio with the Android SDK, SDK Platform 35, NDK
27.2.12479018, CMake, and a JDK 17 or newer. Set `ANDROID_HOME` (or
`ANDROID_SDK_ROOT`) and make sure Gradle is available, then initialize the
workspace dependencies and build from the Android project:

```powershell
git submodule update --init --recursive
cd games/pokemonStadiumGSRecomp/android
gradle assembleDebug -PpsrRom=C:\path\to\pokemon-stadium-2.z64
```

The debug APK is produced under `android/app/build/outputs/apk`. If Gradle
cannot find the SDK, create `android/local.properties` with an `sdk.dir=`
entry pointing to the installed SDK. The ROM is passed to Gradle and is never
copied into the repository.

### macOS

Install Xcode Command Line Tools, Git, CMake, Ninja, and the Vulkan SDK. Then
initialize the same submodules and use the native CMake toolchain:

```bash
git submodule update --init --recursive
cmake -S games/pokemonStadiumGSRecomp -B build/macos/stadium2 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos/stadium2 --target PokemonStadiumGSRecomp --parallel 2
```

The macOS build requires a compatible C/C++ compiler, CMake, Ninja, Vulkan,
and the platform libraries used by RT64. Supply the ROM at launch; do not add
it to the repository.

This project is under active development; Android and macOS remain porting
targets while the Windows native build is brought up.

## License

The project code is distributed under GPL-3.0; see COPYING. Bundled fonts,
controller mappings, and inherited launcher artwork are documented in
THIRD_PARTY.md and licenses/.

No ROMs, saves, extracted game data, or generated recompiler output are included.
