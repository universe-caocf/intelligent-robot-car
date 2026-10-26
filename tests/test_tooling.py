# SPDX-License-Identifier: GPL-3.0-or-later
"""只用临时目录验证配置工具，不触碰真实sdkconfig或备份。"""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/configure-hardware.py"
spec = importlib.util.spec_from_file_location("hardware_config", SCRIPT)
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class HardwareConfigTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.sdk = self.root / "sdkconfig"
        self.profile = self.root / "profile.json"
        self.backups = self.root / "backups"
        self.original = '# Original configuration\nCONFIG_UNRELATED_SECRET="placeholder-not-a-real-secret"\n# CONFIG_CAR_TURRET_ENABLED is not set\nCONFIG_CAR_TURRET_SPIN_MS=900\n'
        self.sdk.write_text(self.original)
        self.sdk.chmod(0o600)

    def cli(self, *arguments):
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, arguments)],
                              capture_output=True, text=True)

    def test_defaults_and_kconfig_match(self):
        self.assertFalse(config.merge_profile({})["CAR_TURRET_ENABLED"])
        text = (ROOT / "firmware/main/Kconfig.projbuild").read_text()
        for key, value in config.DEFAULTS.items():
            block = re.search(r"^config " + key + r"\n(.*?)(?=^config |^endmenu|\Z)", text, re.M | re.S)[1]
            default = re.search(r"^    default (\S+)", block, re.M)[1]
            expected = ("y" if value else "n") if type(value) is bool else str(value)
            self.assertEqual(default, expected, key)

    def test_invalid_json_fields_and_types(self):
        for profile in ([], None, {"UNKNOWN": 1}, {"CAR_TURRET_ENABLED": 1},
                        {"CAR_TURRET_SPIN_MS": True}, {"CAR_TURRET_SPIN_MS": 700.5},
                        {"CAR_TURRET_SPIN_MS": "700"}):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                config.merge_profile(profile)
        with self.assertRaises(ValueError):
            json.loads('{"CAR_TURRET_ENABLED":false,"CAR_TURRET_ENABLED":true}', object_pairs_hook=config.unique_object)

    def test_numeric_bounds(self):
        for key, (minimum, maximum) in config.BOUNDS.items():
            for value in (minimum - 1, maximum + 1):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    config.merge_profile({key: value})

    def test_cross_field_validation(self):
        for profile in ({"CAR_TURRET_REST_US": 1500},
                        {"CAR_TURRET_TILT_MIN_US": 2500},
                        {"CAR_TURRET_TILT_MIN_ANGLE": 120},
                        {"CAR_TURRET_TILT_MAX_ANGLE": 59}):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                config.merge_profile(profile)

    def test_enabled_gpio_whitelist_and_conflicts(self):
        enabled = {"CAR_TURRET_ENABLED": True, "CAR_TURRET_FLYWHEEL_GPIO": 8,
                   "CAR_TURRET_PUSH_GPIO": 9, "CAR_TURRET_CONFIRM_GPIO": 11}
        self.assertEqual(config.merge_profile(enabled)["CAR_TURRET_TILT_GPIO"], -1)
        for key in config.GPIO_KEYS[:3]:
            for pin in (-1, 12, 19, 26, 48):
                with self.subTest(key=key, pin=pin), self.assertRaises(ValueError):
                    config.merge_profile({**enabled, key: pin})
        for profile in ({"CAR_TURRET_PUSH_GPIO": 8}, {"CAR_TURRET_TILT_GPIO": 11}):
            with self.assertRaises(ValueError):
                config.merge_profile({**enabled, **profile})
        self.assertEqual(config.merge_profile({"CAR_TURRET_ENABLED": False})["CAR_TURRET_PUSH_GPIO"], -1)

    def test_partial_profile_preserves_existing_values(self):
        current = config.read_sdkconfig(self.sdk)
        values = config.merge_profile({"CAR_TURRET_PUSH_MS": 350}, current)
        self.assertEqual(values["CAR_TURRET_SPIN_MS"], 900)
        self.assertEqual(values["CAR_TURRET_PUSH_MS"], 350)
        self.assertNotIn("UNRELATED_SECRET", values)

    def test_apply_backup_preservation_and_noop(self):
        values = config.merge_profile({}, config.read_sdkconfig(self.sdk))
        backup = config.apply_profile(self.sdk, values, self.backups)
        self.assertEqual(backup.read_text(), self.original)
        self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        self.assertIn('CONFIG_UNRELATED_SECRET="placeholder-not-a-real-secret"', self.sdk.read_text())
        self.assertEqual(self.sdk.stat().st_mode & 0o777, 0o600)
        self.assertEqual(config.read_sdkconfig(self.sdk), values)
        before = self.sdk.stat().st_mtime_ns
        self.assertIsNone(config.apply_profile(self.sdk, values, self.backups))
        self.assertEqual(self.sdk.stat().st_mtime_ns, before)
        self.assertEqual(len(list(self.backups.iterdir())), 1)

    def test_invalid_apply_does_not_write_or_backup(self):
        values = {**config.DEFAULTS, "CAR_TURRET_SPIN_MS": 0}
        with self.assertRaises(ValueError):
            config.apply_profile(self.sdk, values, self.backups)
        self.assertEqual(self.sdk.read_text(), self.original)
        self.assertFalse(self.backups.exists())

    def test_cli_preview_apply_and_secret_output(self):
        self.profile.write_text('{"CAR_TURRET_PUSH_MS":350}')
        arguments = [self.profile, "--sdkconfig", self.sdk, "--backup-dir", self.backups]
        preview = self.cli(*arguments)
        self.assertEqual(preview.returncode, 0, preview.stderr)
        self.assertNotIn("placeholder-not-a-real-secret", preview.stdout + preview.stderr)
        self.assertEqual(self.sdk.read_text(), self.original)
        self.assertFalse(self.backups.exists())
        applied = self.cli(*arguments, "--apply")
        self.assertEqual(applied.returncode, 0, applied.stderr)
        self.assertEqual(config.read_sdkconfig(self.sdk)["CAR_TURRET_PUSH_MS"], 350)
        self.assertEqual(len(list(self.backups.iterdir())), 1)

    def test_cli_missing_sdkconfig_and_invalid_profile(self):
        self.profile.write_text('{}')
        missing = self.cli(self.profile, "--sdkconfig", self.root / "missing", "--apply")
        self.assertNotEqual(missing.returncode, 0)
        for content in ('{', '{"CAR_TURRET_SPIN_MS":700,"CAR_TURRET_SPIN_MS":900}',
                        '{"CAR_TURRET_ENABLED":true}'):
            self.profile.write_text(content)
            result = self.cli(self.profile, "--sdkconfig", self.sdk, "--apply", "--backup-dir", self.backups)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.sdk.read_text(), self.original)
        self.assertFalse(self.backups.exists())

    def test_check_mode_is_readonly(self):
        result = self.cli("--check-sdkconfig", self.sdk)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.sdk.read_text(), self.original)
        self.profile.write_text('{}')
        self.assertNotEqual(self.cli(self.profile, "--check-sdkconfig", self.sdk).returncode, 0)
        self.assertNotEqual(self.cli("--check-sdkconfig", self.sdk, "--apply").returncode, 0)

    def test_sdkconfig_duplicate_or_invalid_rejected(self):
        for text in ('CONFIG_CAR_TURRET_ENABLED=bad\n', 'CONFIG_CAR_TURRET_PUSH_MS=no\n',
                     'CONFIG_CAR_TURRET_PUSH_MS=200\nCONFIG_CAR_TURRET_PUSH_MS=300\n'):
            self.sdk.write_text(text)
            with self.assertRaises(ValueError):
                config.read_sdkconfig(self.sdk)


if __name__ == "__main__":
    unittest.main()
