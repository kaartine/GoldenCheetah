#!/usr/bin/env python3

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
CHECKER = REPOSITORY_ROOT / "appveyor" / "check-unconfigured-oauth.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("unconfigured_oauth", CHECKER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load OAuth artifact checker")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class UnconfiguredOAuthGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = load_checker()

    def make_binary(self, root, status, exit_code=0):
        source = root / "build-status.c"
        binary = root / ("GoldenCheetah.exe" if os.name == "nt" else "GoldenCheetah")
        source.write_text(
            "#include <stdio.h>\n"
            "int main(int argc, char **argv) {\n"
            "  (void)argv;\n"
            "  if (argc != 2) return 64;\n"
            f"  fputs({json.dumps(status)}, stdout);\n"
            f"  return {exit_code};\n"
            "}\n",
            encoding="ascii",
        )
        if os.name == "nt":
            command = ["cl", "/nologo", str(source), f"/Fe{binary}"]
        else:
            command = [os.environ.get("CC", "cc"), str(source), "-o", str(binary)]
        result = subprocess.run(command, capture_output=True, text=True, cwd=root)
        if result.returncode != 0:
            raise AssertionError(f"native fixture compile failed: {result.stderr}")
        return binary

    def run_checker(self, binary):
        return subprocess.run(
            [sys.executable, str(CHECKER), str(binary)],
            capture_output=True,
            text=True,
        )

    def test_only_exact_unconfigured_build_status_is_accepted(self):
        valid = (
            "goldencheetah_build_status=1\n"
            "application=GoldenCheetah\n"
            "strava_support=enabled\n"
            "strava_oauth=unavailable\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            accepted = self.run_checker(self.make_binary(root, valid))
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            link = root / "linked-GoldenCheetah"
            try:
                link.symlink_to(root / "GoldenCheetah")
            except OSError:
                pass
            else:
                self.assertNotEqual(self.run_checker(link).returncode, 0)

            for index, hostile in enumerate(
                (
                    valid.replace("unavailable", "configured"),
                    valid + "secret=leaked\n",
                    valid.replace("application=GoldenCheetah\n", ""),
                )
            ):
                binary = self.make_binary(root, hostile)
                binary.rename(root / f"hostile-{index}")
                rejected = self.run_checker(root / f"hostile-{index}")
                self.assertNotEqual(rejected.returncode, 0, hostile)

    def test_native_executable_formats_are_identified_structurally(self):
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[16:18] = (3).to_bytes(2, "little")

        pe = bytearray(128)
        pe[:2] = b"MZ"
        pe[0x3C:0x40] = (64).to_bytes(4, "little")
        pe[64:68] = b"PE\x00\x00"

        macho = struct.pack(
            "<IiiIIIII",
            0xFEEDFACF,
            0x01000007,
            3,
            2,
            0,
            0,
            0,
            0,
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = (("elf", elf), ("pe", pe), ("mach-o", macho))
            for name, payload in expected:
                with self.subTest(name=name):
                    path = root / name
                    path.write_bytes(payload)
                    self.assertEqual(self.checker.binary_format(path), name)

    def test_dynamic_loader_injection_variables_are_removed(self):
        environment = self.checker.build_environment(
            "/isolated-home",
            {
                "PATH": "/usr/bin",
                "LD_PRELOAD": "/untrusted/preload.so",
                "LD_LIBRARY_PATH": "/untrusted/lib",
                "DYLD_INSERT_LIBRARIES": "/untrusted/insert.dylib",
                "DYLD_LIBRARY_PATH": "/untrusted/lib",
                "DYLD_FRAMEWORK_PATH": "/untrusted/frameworks",
            },
        )
        self.assertEqual(environment["HOME"], "/isolated-home")
        self.assertEqual(environment["PATH"], "/usr/bin")
        self.assertFalse(
            any(
                name in environment
                for name in (
                    "LD_PRELOAD",
                    "LD_LIBRARY_PATH",
                    "DYLD_INSERT_LIBRARIES",
                    "DYLD_LIBRARY_PATH",
                    "DYLD_FRAMEWORK_PATH",
                )
            )
        )


if __name__ == "__main__":
    unittest.main()
