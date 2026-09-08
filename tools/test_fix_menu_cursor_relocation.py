#!/usr/bin/env python3
"""Regression tests for the narrowly scoped menu cursor relocation repair."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from fix_menu_cursor_relocation import SITES, fix

SCRIPT = Path(__file__).with_name("fix_menu_cursor_relocation.py")


def fixture(ending="\n"):
    lines = ["// unrelated", "int untouched = 1;"]
    for pc in ("8220AA20", "8220AA20", "8220AA24"):
        assembly, old, _ = SITES[pc]
        lines.extend([f"    // 0x{pc}: {assembly}", f"    {old}"])
    return ending.join(lines) + ending


class CursorRepairTests(unittest.TestCase):
    def test_aliases_line_endings_and_idempotence(self):
        for ending in ("\n", "\r\n"):
            original = fixture(ending) * 2
            patched, changed, counts = fix(original, Path("fixture.c"))
            self.assertEqual(changed, 6)
            self.assertEqual(counts, {"8220AA20": 4, "8220AA24": 2})
            self.assertEqual(patched.count(ending), original.count(ending))
            self.assertIn("int untouched = 1;", patched)
            self.assertEqual(fix(patched, Path("fixture.c"))[:2], (patched, 0))

    def test_partially_fixed_input(self):
        original = fixture().replace(SITES["8220AA20"][1], SITES["8220AA20"][2], 1)
        self.assertEqual(fix(original, Path("fixture.c"))[1], 2)

    def test_unexpected_and_incomplete_sites(self):
        for original in (fixture().replace("MEM_W", "ADD32"),
                         fixture().replace("lw $a0", "lw $a1"),
                         fixture().replace("0X9B4", "0X9B8"),
                         "// 0x8220AA24: lw $a0, 0x9B4($a0)"):
            with self.assertRaises(ValueError):
                fix(original, Path("fixture.c"))

    def test_cli_check_and_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "funcs_1.c"
            target.write_bytes(fixture("\r\n").encode())
            before = target.read_bytes()
            def run(*args):
                return subprocess.run([sys.executable, str(SCRIPT), str(root), *args],
                                      capture_output=True, text=True)
            self.assertEqual(run("--check").returncode, 1)
            self.assertEqual(target.read_bytes(), before)
            invalid = root / "funcs_2.c"
            invalid.write_bytes(fixture().replace("MEM_W", "ADD32").encode())
            self.assertEqual(run().returncode, 2)
            self.assertEqual(target.read_bytes(), before)
            invalid.unlink()
            self.assertEqual(run().returncode, 0)
            patched = target.read_bytes()
            self.assertEqual(patched.count(b"\r\n"), before.count(b"\r\n"))
            self.assertEqual(run("--check").returncode, 0)
            self.assertEqual(run().returncode, 0)
            self.assertEqual(target.read_bytes(), patched)
            target.write_text("int unrelated;\n")
            self.assertEqual(run().returncode, 2)


if __name__ == "__main__":
    unittest.main()
