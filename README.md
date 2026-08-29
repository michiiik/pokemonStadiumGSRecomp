# Pokémon Stadium 2 Recompilation

This project is a work-in-progress native recompilation of **Pokémon Stadium 2
(US)**. It aims to make the game build and run as a native application while
preserving the original game behavior.

The project combines:

- Stadium 2 decompilation work based on [`pokestadiumgs`](https://github.com/michiiik/pokestadiumgs)
- Code generation with [`N64Recomp`](https://github.com/michiiik/N64Recomp)
- A native runtime and renderer maintained in the workspace toolchain

The repository contains project-specific source, configuration, build scripts,
and documentation. Dependencies are managed separately as Git submodules by
the private [`PokemonStadiumWorkspace`](https://github.com/michiiik/PokemonStadiumWorkspace)
repository.

## ROM policy

A legally obtained Pokémon Stadium 2 ROM is required for local development.
ROMs, saves, generated output, binaries, and machine-specific files must remain
local and must not be committed.

## Status

This project is under active development. Build instructions and supported
targets will be documented here as the native build is brought up from the
clean base.

