#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""硬件前只读检查。不会联网、刷机、注册设备或输出配置凭据。"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASELINE = "6a512ea186f29c94fa53e33181a83d01564205ae"
REQUIRED_DOCS = ("README.md", "CODEBASE.md", "THIRD_PARTY_NOTICES.md", "demo/README.md",
    "docs/product.md", "docs/architecture.md", "docs/hardware.md", "docs/acceptance.md",
    "docs/demo.md", "docs/submission.md", "docs/linx-setup.md", "docs/build.md", "docs/preparation-status.md")


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def check_links():
    files = [ROOT / "README.md", ROOT / "demo/README.md", *(ROOT / "docs").glob("*.md")]
    for path in files:
        for target in re.findall(r"\]\(([^)]+)\)", path.read_text()):
            if "://" not in target and not target.startswith("#"):
                if not (path.parent / target.split("#")[0]).exists():
                    raise ValueError("失效文档链接: " + str(path.relative_to(ROOT)) + " -> " + target)


def check_repository():
    if git("rev-parse", "v0^{commit}") != BASELINE:
        raise ValueError("v0与已记录基线不同，请检查，不能自动移动tag")
    if (ROOT / "firmware/.git").exists():
        raise ValueError("firmware不能含独立.git")
    tracked = git("ls-files", "-z").split("\0")
    blocked = ("firmware/build/", "firmware/managed_components/", ".local-backups/")
    for name in tracked:
        if not name:
            continue
        if name.startswith(blocked) or name in ("firmware/sdkconfig", "firmware/sdkconfig.old", ".env"):
            raise ValueError("生成配置/缓存不应被跟踪: " + name)
        path = ROOT / name
        if not path.is_file() or path.stat().st_size > 2_000_000:
            continue
        try:
            content = path.read_text()
        except UnicodeError:
            continue
        # 只报告位置，不输出可能存在的凭据内容；这不是全面秘密扫描保证。
        if re.search(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{30,}", content):
            raise ValueError("疑似私钥/token，请人工核对文件: " + name)
    subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-build", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        for tool in ("git", "bash", os.environ.get("CXX", "c++"), os.environ.get("CC", "cc"), "node"):
            if shutil.which(tool) is None:
                raise ValueError("缺少主机工具: " + tool)
        idf = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v5.5"))
        for name in ("export.sh", "tools/idf.py", "components/json/cJSON/cJSON.c"):
            if not (idf / name).is_file():
                raise ValueError("缺少ESP-IDF文件: " + name + "；请先安装v5.5.1或设置IDF_PATH")
        for name in REQUIRED_DOCS:
            if not (ROOT / name).is_file() or not (ROOT / name).stat().st_size:
                raise ValueError("缺少交付文档: " + name)
        check_links()
        check_repository()
        spec = importlib.util.spec_from_file_location("hardware_config", ROOT / "tools/configure-hardware.py")
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        config = ROOT / "firmware/sdkconfig"
        values = module.merge_profile({}, module.read_sdkconfig(config))
        if args.require_build:
            build = ROOT / "firmware/build"
            binary = build / "intelligent_robot_car.bin"
            effective = json.loads((build / "config/sdkconfig.json").read_text())
            if not binary.is_file() or binary.stat().st_size == 0 or effective.get("IDF_TARGET") != "esp32s3":
                raise ValueError("缺少有效ESP32-S3构建，请先./tools/build.sh")
            for key, value in values.items():
                if effective.get(key, module.DEFAULTS[key]) != value:
                    raise ValueError("当前配置与构建配置不同，需要重新构建: " + key)
        report = {"preflight": "passed", "commit": git("rev-parse", "HEAD"), "v0": BASELINE,
            "working_tree_dirty": bool(git("status", "--porcelain")), "turret_enabled": values["CAR_TURRET_ENABLED"],
            "build_presence_checked": args.require_build, "hardware": "pending_physical_validation",
            "cloud_binding": "pending_real_account_and_device", "demo_video": "pending_real_recording"}
        if args.json:
            print(json.dumps(report, ensure_ascii=False, indent=2))
        else:
            print("PASS: host tools, IDF files, docs/links, v0, tracked-file hygiene, hardware config")
            print("预检查不替代编译/测试；实机、真实云端绑定和视频仍待验证。")
            print("commit=" + report["commit"] + " dirty=" + str(report["working_tree_dirty"]))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, "预检查失败: " + str(error) + "\n")


if __name__ == "__main__":
    main()
