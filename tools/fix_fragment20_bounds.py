#!/usr/bin/env python3
"""Repair six known fragment-20 bound sites, including generated aliases.

This is intentionally not a general relocation repair. Validate the entire
directory before writing, preserve line endings, and leave unchanged files alone.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PAIRS = (("82604120", "82604130"), ("82604250", "82604254"),
         ("82604304", "82604308"))
SITES = {
    pc: (reg, kind, asm)
    for pair, reg, asm in zip(PAIRS, ("r17", "r17", "r18"), ("s1", "s1", "s2"))
    for pc, kind in zip(pair, ("hi", "lo"))
}
PC = re.compile(r"^[ \t]*//[ \t]*0[xX](" + "|".join(SITES) + r"):")


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text).lower()


def fix(text: str, path: Path) -> tuple[str, int, dict[str, int]]:
    lines = text.splitlines(keepends=True)
    counts = dict.fromkeys(SITES, 0)
    changed = 0
    for index, line in enumerate(lines):
        match = PC.match(line)
        if not match:
            continue
        pc = match.group(1)
        reg, kind, asm = SITES[pc]
        counts[pc] += 1
        instruction = line[match.end():]
        expected_asm = (f"lui ${asm}, 0x8262" if kind == "hi"
                        else f"addiu ${asm}, ${asm}, -0x5B78")
        if compact(instruction) != compact(expected_asm) or index + 1 >= len(lines):
            raise ValueError(f"{path}: unexpected or incomplete instruction at {pc}")
        code = lines[index + 1]
        if kind == "hi":
            old = f"ctx->{reg} = S32(U32(0x8262) << 16);"
            new = f"ctx->{reg} = S32(U32(RELOC_HI16(65, 0x1A488)) << 16);"
        else:
            old = f"ctx->{reg} = ADD32(ctx->{reg}, -0x5B78);"
            new = f"ctx->{reg} = ADD32(ctx->{reg}, (int16_t)RELOC_LO16(65, 0x1A488));"
        if compact(code) == compact(new):
            continue
        if compact(code) != compact(old):
            raise ValueError(f"{path}: unexpected generated C at {pc}")
        indent = code[:len(code) - len(code.lstrip(" \t"))]
        ending = "\r\n" if code.endswith("\r\n") else "\n" if code.endswith("\n") else ""
        lines[index + 1] = indent + new + ending
        changed += 1
    for hi, lo in PAIRS:
        if counts[hi] != counts[lo]:
            raise ValueError(f"{path}: unmatched bound sites {hi}/{lo}")
    return "".join(lines), changed, counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", type=Path)
    parser.add_argument("--check", action="store_true", help="read-only; exit 1 if repair needed")
    args = parser.parse_args()
    files = sorted(args.generated.glob("funcs_*.c"))
    if not files:
        raise ValueError(f"no funcs_*.c files in {args.generated}")
    pending = []
    total = 0
    counts = dict.fromkeys(SITES, 0)
    for path in files:
        with path.open(encoding="utf-8", newline="") as source:
            fixed, changed, seen = fix(source.read(), path)
        if changed:
            pending.append((path, fixed))
        total += changed
        for pc, count in seen.items():
            counts[pc] += count
    missing = [pc for pc, count in counts.items() if not count]
    if missing:
        raise ValueError("missing target sites: " + ", ".join(missing))
    if not args.check:
        for path, fixed in pending:
            with path.open("w", encoding="utf-8", newline="") as output:
                output.write(fixed)
    sites = sum(counts.values())
    action = "need repair" if args.check else "changed"
    print(f"fragment20 bounds: {sites // 2} pairs / {sites} sites validated; {total} sites {action}")
    return 1 if args.check and total else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
