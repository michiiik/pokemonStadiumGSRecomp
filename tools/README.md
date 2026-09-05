# Project tools

These tools are intentionally small and repository-relative. They expect the
Stadium 2 repository to be checked out in the documented workspace layout so
the public decompilation submodule is available at `../../decomp/pokestadiumgs`.

- `verify_rom.ps1` checks the local ignored `baserom.z64`.
- `derive_recomp_metadata.py` derives fragment and function-size metadata from
  the ELF built by the `pokestadiumgs` submodule.
- `build_ps2_aspmain.py` derives the Stadium 2 audio microcode configuration
  from a local ROM. Use `--rom` and `--output` to override its defaults.

Generated files and ROMs remain local and are excluded by `.gitignore`.
