# Project tools

These tools are intentionally small and repository-relative. They expect the
public decompilation fork to be initialized as the nested `disasm` submodule.

- `verify_rom.ps1` checks the local ignored `baserom.z64`.
- `derive_recomp_metadata.py` derives fragment and function-size metadata from
  the ELF built by the `pokestadiumgs` submodule.
- `build_ps2_aspmain.py` derives the Stadium 2 audio microcode configuration
  from a local ROM. Use `--rom` and `--output` to override its defaults.

Generated files and ROMs remain local and are excluded by `.gitignore`.

`fix_fragment20_bounds.py generated` applies the specific fragment-20 bound
correction used during Windows validation. CMake runs it during configuration
when generated C files exist. After regenerating, rerun CMake before building.
It is intentionally not a general BSS-relocation heuristic.

Run its ROM-free regression tests with:

```sh
python tools/test_fix_fragment20_bounds.py
```

Use `python tools/fix_fragment20_bounds.py generated --check` for a read-only
validation; unpatched or unexpected input produces a nonzero exit code.
