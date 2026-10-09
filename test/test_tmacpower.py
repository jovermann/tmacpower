#!/usr/bin/env python3
#
# Copyright (c) 2026 Johannes Overmann
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

import os
import pty
import re
import select
import subprocess
import time
import unittest
from pathlib import Path


PROGRAM = str(Path(__file__).resolve().parents[1] / "tmacpower")


class TmacpowerTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([PROGRAM, *args], capture_output=True, text=True, timeout=10)

    def test_help_and_version(self):
        help_result = self.run_cli("--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertLess(help_result.stdout.index("Examples:"), help_result.stdout.index("Options:"))
        self.assertTrue(help_result.stdout.endswith("\n"))
        self.assertFalse(help_result.stdout.endswith("\n\n"))
        self.assertIn("Copyright (c) 2026 Johannes Overmann", help_result.stdout)
        self.assertIn("--no-ui", help_result.stdout)

        version_result = self.run_cli("--version")
        self.assertEqual(version_result.returncode, 0)
        self.assertRegex(version_result.stdout, r"version 0\.1\.0")

    def test_invalid_interval_and_nonterminal_ui(self):
        invalid = self.run_cli("--interval=0.1")
        self.assertEqual(invalid.returncode, 1)
        self.assertIn("--interval must be between", invalid.stdout)

        nonterminal = self.run_cli()
        self.assertEqual(nonterminal.returncode, 1)
        self.assertIn("use --no-ui", nonterminal.stdout)

    def test_plain_text_power_table(self):
        result = self.run_cli("--no-ui")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("\x1b[", result.stdout)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 12)
        self.assertTrue(lines[0].startswith("Metric"))
        self.assertEqual(set(lines[1]), {"-"})
        labels = [
            "Power source", "Supply delivered", "Machine consumed",
            "Battery charging", "Battery discharging", "Adapter capacity",
            "Battery charge", "Battery voltage", "Battery current", "Charging state",
        ]
        self.assertEqual([line[:19].rstrip() for line in lines[2:]], labels)
        self.assertEqual(len({line.index("  W") for line in lines[3:8]}), 1)
        for line in lines[3:8]:
            self.assertRegex(line, r"(?:\d+\.\d|unavailable)  W$")
        if "Battery" in lines[2]:
            self.assertRegex(lines[3], r"0\.0  W$")

    def test_tui_colors_timestamp_and_quit(self):
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [PROGRAM, "--interval=0.2"], stdin=slave, stdout=slave,
            stderr=subprocess.PIPE, start_new_session=True,
        )
        os.close(slave)
        output = bytearray()
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.2)
                if ready:
                    chunk = os.read(master, 65536)
                    if not chunk:
                        break
                    output.extend(chunk)
                times = set(re.findall(rb"tmacpower  \|  live power  \|  (\d\d:\d\d:\d\d)", output))
                if len(times) >= 2 and b"q / Ctrl-C: quit" in output:
                    break
            self.assertGreaterEqual(len(times), 2, output.decode(errors="replace"))
            os.write(master, b"q")
            deadline = time.monotonic() + 5
            while process.poll() is None and time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.2)
                if ready:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output.extend(chunk)
            self.assertEqual(process.wait(timeout=1), 0)
            while select.select([master], [], [], 0)[0]:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                output.extend(chunk)
            self.assertIn(b"\x1b[44;37m", output)  # blue title and footer
            self.assertIn(b"\x1b[100;37m", output)  # dark table header
            self.assertIn(b"\x1b[32m", output)  # green data rows
            self.assertIn(b"\x1b[?1049l", output)  # original screen restored
            first_frame = bytes(output).split(b"\x1b[H\x1b[2J", 2)[1]
            self.assertNotIn(b"---------------------------------------", first_frame)
            self.assertNotIn(b"\r\n\r\n", first_frame)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            process.stderr.close()
            os.close(master)


if __name__ == "__main__":
    unittest.main()
