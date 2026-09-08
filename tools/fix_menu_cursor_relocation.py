#!/usr/bin/env python3
"""Repair the menu cursor's known absolute resource load, including aliases.

The writer uses relocated object storage, but this load was emitted literally
because its target lies beyond the fragment image extent. Validate all files
before writing; preserve line endings and leave unrelated instructions alone.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SITES = {
    "8220AA20": (
        "lui $a0, 0x8221",
        "ctx->r4 = S32(U32(0X8221) << 16);",
        "ctx->r4 = S32(U32(RELOC_HI16(56, 0X109B4)) << 16);",
    ),
    "8220AA24": (
        "lw $a0, 0x9B4($a0)",
        "ctx->r4 = MEM_W(ctx->r4, 0X9B4);",
        "ctx->r4 = MEM_W(ctx->r4, (int16_t)RELOC_LO16(56, 0X109B4));",
    ),
}
PC = re.compile(r"^[ \t]*//[ \t]*0[xX](8220AA20|8220AA24):", re.IGNORECASE)


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text).lower()


def fix(text: str, path: Path) -> tuple[str, int, dict[str, int]]:
    counts = dict.fromkeys(SITES, 0)
    if "8220AA" not in text.upper():
        return text, 0, counts
    lines = text.splitlines(keepends=True)
    changed = 0
    for index, line in enumerate(lines):
        match = PC.match(line)
        if not match:
            continue
        pc = match.group(1).upper()
        assembly, old, new = SITES[pc]
        counts[pc] += 1
        if compact(line[match.end():]) != compact(assembly) or index + 1 >= len(lines):
            raise ValueError(f"{path}: unexpected or incomplete instruction at {pc}")
        code = lines[index + 1]
        if compact(code) == compact(new):
            continue
        if compact(code) != compact(old):
            raise ValueError(f"{path}: unexpected generated C at {pc}")
        indent = code[:len(code) - len(code.lstrip(" \t"))]
        ending = "\r\n" if code.endswith("\r\n") else "\n" if code.endswith("\n") else ""
        lines[index + 1] = indent + new + ending
        changed += 1
    # The HI instruction occurs both in the branch's delay-slot expansion and
    # its fall-through path. Each full generated callback has two HI sites.
    if counts["8220AA20"] != 2 * counts["8220AA24"]:
        raise ValueError(f"{path}: unmatched cursor HI/LO sites")
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
    counts = dict.fromkeys(SITES, 0)
    total = 0
    for path in files:
        with path.open(encoding="utf-8", newline="") as source:
            fixed, changed, seen = fix(source.read(), path)
        if changed:
            pending.append((path, fixed))
        total += changed
        for pc, count in seen.items():
            counts[pc] += count
    if not all(counts.values()):
        raise ValueError("missing menu cursor target sites")
    if not args.check:
        for path, fixed in pending:
            with path.open("w", encoding="utf-8", newline="") as output:
                output.write(fixed)
    action = "need repair" if args.check else "changed"
    print(f"menu cursor: {sum(counts.values())} sites validated; {total} sites {action}")
    return 1 if args.check and total else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
