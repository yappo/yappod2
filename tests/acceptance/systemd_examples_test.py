#!/usr/bin/env python3

import configparser
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class SystemdExamplesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(sys.argv[1]).resolve()
        cls.core = cls.load_unit("yappod-core.service")
        cls.front = cls.load_unit("yappod-front.service")

    @classmethod
    def load_unit(cls, name):
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        path = cls.root / "examples" / "systemd" / name
        with path.open(encoding="utf-8") as unit_file:
            parser.read_file(unit_file)
        return parser

    def test_core_runs_the_server_in_foreground(self):
        service = self.core["Service"]
        self.assertEqual(service["Type"], "exec")
        self.assertEqual(
            service["ExecStart"],
            "/usr/local/bin/yappod_core --foreground --config /etc/yappod/application.toml",
        )
        self.assertNotIn("PIDFile", service)
        self.assertEqual(service["Restart"], "on-failure")

    def test_front_runs_after_core_without_sharing_its_lifecycle(self):
        unit = self.front["Unit"]
        service = self.front["Service"]
        self.assertIn("yappod-core.service", unit["Wants"].split())
        self.assertIn("yappod-core.service", unit["After"].split())
        self.assertNotIn("Requires", unit)
        self.assertNotIn("BindsTo", unit)
        self.assertEqual(service["Type"], "exec")
        self.assertEqual(
            service["ExecStart"],
            "/usr/local/bin/yappod_front --foreground --config /etc/yappod/application.toml",
        )
        self.assertNotIn("PIDFile", service)
        self.assertEqual(service["Restart"], "on-failure")

    def test_units_use_the_documented_service_identity_and_protection(self):
        for unit in (self.core, self.front):
            service = unit["Service"]
            self.assertEqual(service["User"], "yappod")
            self.assertEqual(service["Group"], "yappod")
            self.assertEqual(service["NoNewPrivileges"], "true")
            self.assertEqual(service["ProtectSystem"], "strict")
            self.assertEqual(service["ProtectHome"], "true")
            self.assertEqual(service["StandardOutput"], "journal")
            self.assertEqual(service["StandardError"], "journal")
            self.assertEqual(unit["Install"]["WantedBy"], "multi-user.target")

    @unittest.skipUnless(shutil.which("systemd-analyze"), "systemd-analyze is not installed")
    def test_units_pass_systemd_analyze_verify(self):
        source_dir = self.root / "examples" / "systemd"
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            unit_paths = []
            for name in ("yappod-core.service", "yappod-front.service"):
                source = (source_dir / name).read_text(encoding="utf-8")
                source = source.replace("/usr/local/bin/yappod_core", "/bin/true")
                source = source.replace("/usr/local/bin/yappod_front", "/bin/true")
                source = source.replace("User=yappod", "User=root")
                source = source.replace("Group=yappod", "Group=root")
                destination = temporary / name
                destination.write_text(source, encoding="utf-8")
                unit_paths.append(str(destination))
            completed = subprocess.run(
                ["systemd-analyze", "verify", *unit_paths],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                completed.returncode,
                0,
                msg=f"systemd-analyze verify failed:\n{completed.stdout}{completed.stderr}",
            )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
