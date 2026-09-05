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

## License

The project code is distributed under GPL-3.0; see `COPYING`. Bundled fonts,
controller mappings, and inherited launcher artwork are documented in
`THIRD_PARTY.md` and `licenses/`.

No ROMs, saves, extracted game data, or generated recompiler output are included.
