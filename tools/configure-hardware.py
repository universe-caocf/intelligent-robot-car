#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""预览/校验炮塔配置；显式 --apply 才备份并原子更新 sdkconfig。"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = json.loads((ROOT / "tools/hardware-profile.example.json").read_text())
BOUNDS = {
    "CAR_TURRET_FLYWHEEL_GPIO": (-1, 48), "CAR_TURRET_PUSH_GPIO": (-1, 48),
    "CAR_TURRET_CONFIRM_GPIO": (-1, 48), "CAR_TURRET_TILT_GPIO": (-1, 48),
    "CAR_TURRET_REST_US": (500, 2500), "CAR_TURRET_PUSH_US": (500, 2500),
    "CAR_TURRET_SPIN_MS": (100, 3000), "CAR_TURRET_PUSH_MS": (100, 1000),
    "CAR_TURRET_RECOVER_MS": (100, 1000),
    "CAR_TURRET_TILT_MIN_US": (500, 2500), "CAR_TURRET_TILT_MAX_US": (500, 2500),
    "CAR_TURRET_TILT_MIN_ANGLE": (0, 180), "CAR_TURRET_TILT_MAX_ANGLE": (0, 180),
}
GPIO_KEYS = ("CAR_TURRET_FLYWHEEL_GPIO", "CAR_TURRET_PUSH_GPIO", "CAR_TURRET_CONFIRM_GPIO", "CAR_TURRET_TILT_GPIO")
AVAILABLE_PINS = {8, 9, 10, 11, 17, 38}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("JSON存在重复字段: " + key)
        result[key] = value
    return result


def read_sdkconfig(path):
    values = {}
    if not path.exists():
        return values
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"(?:CONFIG_([A-Z0-9_]+)=(.*)|# CONFIG_([A-Z0-9_]+) is not set)", line)
        if not match:
            continue
        key = match[1] or match[3]
        if key not in DEFAULTS:
            continue
        if key in values:
            raise ValueError("sdkconfig存在重复字段: " + key)
        raw = match[2]
        if key == "CAR_TURRET_ENABLED":
            if raw not in (None, "y", "n"):
                raise ValueError("启用开关必须为y/n")
            values[key] = raw == "y"
        else:
            if raw is None or not re.fullmatch(r"-?[0-9]+", raw):
                raise ValueError("配置必须为整数: " + key)
            values[key] = int(raw)
    return values


def validate(values):
    if set(values) != set(DEFAULTS):
        raise ValueError("未知或缺少配置字段")
    if type(values["CAR_TURRET_ENABLED"]) is not bool:
        raise ValueError("CAR_TURRET_ENABLED必须是JSON布尔值")
    for key, (minimum, maximum) in BOUNDS.items():
        value = values[key]
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(key + "必须是范围内的整数")
    if values["CAR_TURRET_REST_US"] == values["CAR_TURRET_PUSH_US"]:
        raise ValueError("推弹rest和push脉宽不能相同")
    for suffix in ("US", "ANGLE"):
        if values["CAR_TURRET_TILT_MIN_" + suffix] >= values["CAR_TURRET_TILT_MAX_" + suffix]:
            raise ValueError("俯仰最小值必须小于最大值")
    if values["CAR_TURRET_ENABLED"]:
        pins = [values[key] for key in GPIO_KEYS]
        if any(pin not in AVAILABLE_PINS for pin in pins[:3]) or (pins[3] != -1 and pins[3] not in AVAILABLE_PINS):
            raise ValueError("启用时必填GPIO需在候选白名单中，可选俯仰允许-1")
        used = [pin for pin in pins if pin >= 0]
        if len(used) != len(set(used)):
            raise ValueError("机构和确认键不能共用GPIO")
    return values


def merge_profile(profile, current=None):
    if not isinstance(profile, dict) or set(profile) - set(DEFAULTS):
        raise ValueError("配置必须是只含已知字段的JSON对象")
    return validate({**DEFAULTS, **(current or {}), **profile})


def apply_profile(path, values, backup_root):
    validate(values)
    if not path.is_file():
        raise ValueError("先完成一次构建生成sdkconfig，再应用配置")
    original = path.read_text()
    updated = original
    for key, value in values.items():
        if type(value) is bool:
            replacement = "CONFIG_" + key + "=y" if value else "# CONFIG_" + key + " is not set"
        else:
            replacement = "CONFIG_" + key + "=" + str(value)
        pattern = r"^(?:CONFIG_" + key + r"=.*|# CONFIG_" + key + r" is not set)$"
        updated, count = re.subn(pattern, replacement, updated, flags=re.M)
        if count == 0:
            updated = updated.rstrip("\n") + "\n" + replacement + "\n"
    if updated == original:
        return None
    backup_root.mkdir(parents=True, exist_ok=True)
    backup = Path(tempfile.mkdtemp(prefix="hardware-", dir=backup_root)) / path.name
    shutil.copy2(path, backup)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(updated)
        temporary.chmod(path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return backup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?", type=Path)
    parser.add_argument("--apply", action="store_true", help="显式备份并更新本机sdkconfig")
    parser.add_argument("--sdkconfig", type=Path, default=ROOT / "firmware/sdkconfig")
    parser.add_argument("--backup-dir", type=Path, default=ROOT / ".local-backups/hardware-config")
    parser.add_argument("--check-sdkconfig", type=Path, help="只校验生成配置，无写入")
    args = parser.parse_args()
    try:
        if args.check_sdkconfig:
            if args.profile or args.apply or not args.check_sdkconfig.is_file():
                raise ValueError("检查模式需要现有sdkconfig，不能同时应用profile")
            merge_profile({}, read_sdkconfig(args.check_sdkconfig))
            print("PASS: hardware configuration syntax and conflict checks (not physical verification)")
            return
        if args.profile is None:
            parser.error("请提供JSON配置文件或--check-sdkconfig")
        profile = json.loads(args.profile.read_text(), object_pairs_hook=unique_object)
        values = merge_profile(profile, read_sdkconfig(args.sdkconfig))
        print(json.dumps(values, ensure_ascii=False, indent=2))
        if args.apply:
            backup = apply_profile(args.sdkconfig, values, args.backup_dir)
            print("配置未变化" if backup is None else "已更新；原配置备份: " + str(backup))
            print("尚未编译/烧录；先核对实物，再运行构建入口")
        else:
            print("仅预览，无文件写入；应用需显式--apply")
    except (OSError, ValueError) as error:
        parser.exit(2, "配置检查失败: " + str(error) + "\n")


if __name__ == "__main__":
    main()
