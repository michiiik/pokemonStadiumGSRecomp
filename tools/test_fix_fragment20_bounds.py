"""ROM-free regression tests for the scoped generated-source repair."""
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("fix_fragment20_bounds.py")
PAIRS = (("82604120", "82604130", "17", "s1"),
         ("82604250", "82604254", "17", "s1"),
         ("82604304", "82604308", "18", "s2"))


def fixture():
    return "".join(
        f"    // 0x{hi}: lui         ${asm}, 0x8262\n"
        f"    ctx->r{reg} = S32(U32(0X8262) << 16);\n"
        "    // unrelated intervening instruction\n"
        "    ctx->r2 = 0;\n"
        f"    // 0x{lo}: addiu       ${asm}, ${asm}, -0x5B78\n"
        f"    ctx->r{reg} = ADD32(ctx->r{reg}, -0X5B78);\n"
        for hi, lo, reg, asm in PAIRS)


def run(root, *args):
    return subprocess.run([sys.executable, str(SCRIPT), str(root), *args],
                          capture_output=True, text=True)


class BoundsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "funcs_1.c"
        self.source.write_bytes(fixture().encode())

    def test_check_aliases_crlf_and_idempotence(self):
        self.source.write_bytes((fixture() * 2).replace("\n", "\r\n").encode())
        unchanged = self.root / "funcs_2.c"
        unchanged.write_bytes(b"// unrelated\n")
        stamp = unchanged.stat().st_mtime_ns
        before = self.source.read_bytes()
        self.assertEqual(run(self.root, "--check").returncode, 1)
        self.assertEqual(before, self.source.read_bytes())
        result = run(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        fixed = self.source.read_bytes()
        self.assertEqual(fixed.count(b"RELOC_HI16"), 6)
        self.assertEqual(fixed.count(b"RELOC_LO16"), 6)
        self.assertEqual(fixed.count(b"\n"), fixed.count(b"\r\n"))
        self.assertEqual(unchanged.stat().st_mtime_ns, stamp)
        stamp = self.source.stat().st_mtime_ns
        self.assertEqual(run(self.root).returncode, 0)
        self.assertEqual(run(self.root, "--check").returncode, 0)
        self.assertEqual(fixed, self.source.read_bytes())
        self.assertEqual(self.source.stat().st_mtime_ns, stamp)
        self.source.write_bytes(fixed.replace(b"0x1A488", b"0X1a488"))
        self.assertEqual(run(self.root, "--check").returncode, 0)

    def test_malformed_directory_never_partially_written(self):
        original = self.source.read_bytes()
        bad = self.root / "funcs_9.c"
        variants = [
            fixture().replace("ADD32(ctx->r17, -0X5B78)", "ADD32(ctx->r17, -0X5B74)", 1),
            fixture().replace("lui         $s1", "lui         $s2", 1),
            fixture().replace("// 0x82604130:", "// 0x82604134:", 1),
            fixture().replace("// 0x82604120:", "// 0x82604124:", 1),
            "// 0x82604120: lui $s1, 0x8262\n",
        ]
        for variant in variants:
            with self.subTest(variant=variant):
                bad.write_bytes(variant.encode())
                self.assertEqual(run(self.root).returncode, 2)
                self.assertEqual(self.source.read_bytes(), original)
                self.assertEqual(bad.read_bytes(), variant.encode())

    def test_missing_sites(self):
        self.source.write_bytes(b"// no target sites\n")
        self.assertEqual(run(self.root).returncode, 2)

    @unittest.skipUnless(shutil.which("cmake"), "CMake not on PATH")
    def test_cmake_configure_hook(self):
        repo = SCRIPT.parent.parent
        cmake = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
        start = cmake.index("if(FUNC_C_SOURCES)")
        end = cmake.index("target_sources(RecompiledFuncs", start)
        block = cmake[start:end].replace(
            '${CMAKE_SOURCE_DIR}/generated', self.root.as_posix())
        test_script = self.root / "check.cmake"
        test_script.write_bytes((
            f'set(CMAKE_SOURCE_DIR "{repo.resolve().as_posix()}")\n'
            f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
            'set(FUNC_C_SOURCES "fixture")\n' + block).encode())
        command = ["cmake", "-P", str(test_script)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(run(self.root, "--check").returncode, 0)
        self.source.write_bytes(b"// malformed generated directory\n")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Fragment-20 bound validation failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
