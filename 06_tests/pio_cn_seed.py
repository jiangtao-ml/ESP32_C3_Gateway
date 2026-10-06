#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PlatformIO 国内离线引导：从乐鑫 CN CDN 取包 → 拍平目录 → 补 PlatformIO 清单
================================================================================

背景（2026-10-05 实测）：
  - PlatformIO 官方 CDN 在国内 **≈14 KB/s**，工具链 + framework 是 GB 量级 ⇒ 基本下不动。
  - 乐鑫 CN CDN `dl.espressif.cn` **实测 2.1 MB/s（快 150 倍以上）**，但它**只镜像 GitHub release 资产**，
    给的是 **Espressif 原版包**，而 PlatformIO 要求包根目录带 `package.json` 清单
    ⇒ 直接 `platform_packages = ... @ <乐鑫 URL>` 会报
       `MissingPackageManifestError: Could not find one of 'package.json' manifest files in the package`。
  - 所以本脚本做三件事：① 下载/复用缓存；② 解压并**拍平多余的一层顶层目录**；③ 写 `package.json` + `.piopm`，
    让 PlatformIO 认为该包已安装（版本号取自 `~/.platformio/platforms/<platform>/platform.json`）。

用法：
    python pio_cn_seed.py                     # 引导本工程需要的两个大包（toolchain + framework）
    python pio_cn_seed.py --list              # 只看计划，不下载
    python pio_cn_seed.py --rebuild           # 已存在的包目录先删掉再放（慎用）

⚠️ 该脚本会写入 `~/.platformio/packages/`（用户级目录），不碰仓库里的任何源码。
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import urllib.request
import zipfile

CN = "https://dl.espressif.cn/github_assets"
PIO = os.path.expanduser("~/.platformio")
PKGS = os.path.join(PIO, "packages")
DL = os.path.join(PIO, ".cache", "downloads")

# 目标：本工程（espressif32@7.1.3 + esp32c3 + arduino）需要的大包
TARGETS = [
    dict(
        pkg="toolchain-riscv32-esp",
        pio_type="tool", pio_owner="espressif",
        url=CN + "/espressif/crosstool-NG/releases/download/esp-2021r2-patch5/"
                  "riscv32-esp-elf-gcc8_4_0-esp-2021r2-patch5-win64.zip",
        desc="ESP32-C3 RISC-V 工具链（Espressif crosstool-NG 原版 win64）",
    ),
    dict(
        pkg="framework-arduinoespressif32",
        pio_type="framework", pio_owner="platformio",
        url=CN + "/espressif/arduino-esp32/releases/download/2.0.17/esp32-2.0.17.zip",
        desc="arduino-esp32 2.0.17 框架源码（= platform 声明的 4.20017.0 那一代）",
    ),
]


def platform_versions() -> dict:
    """从 platform.json 读平台声明的包版本（用于写清单里的 version）。"""
    out = {}
    pdir = os.path.join(PIO, "platforms")
    if not os.path.isdir(pdir):
        return out
    for name in sorted(os.listdir(pdir)):
        pj = os.path.join(pdir, name, "platform.json")
        if not os.path.exists(pj):
            continue
        try:
            j = json.load(open(pj, encoding="utf-8"))
        except Exception:
            continue
        for k, v in (j.get("packages") or {}).items():
            ver = str((v or {}).get("version") or "")
            ver = re.sub(r"^[~^]=?", "", ver).strip() or None
            out.setdefault(k, ver)
    return out


def cache_path(url: str) -> str:
    """PlatformIO 的下载缓存按 URL 的 SHA1 命名。"""
    return os.path.join(DL, hashlib.sha1(url.encode()).hexdigest())


def fetch(url: str, dest: str) -> str:
    """优先复用 PlatformIO 已缓存的同名文件；否则从 CN CDN 下载。"""
    c = cache_path(url)
    if os.path.exists(c) and os.path.getsize(c) > 1024 * 1024:
        print("    [缓存] 复用 PlatformIO 已下载：{}（{:.1f} MB）".format(
            os.path.basename(c), os.path.getsize(c) / 1048576))
        return c
    print("    [下载] {}".format(url.split("/")[-1]))
    tmp = dest + ".part"
    got, total = 0, None
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "pio-seed"}), timeout=30) as r:
        total = int(r.headers.get("Content-Length") or 0)
        with open(tmp, "wb") as f:
            while True:
                b = r.read(262144)
                if not b:
                    break
                f.write(b)
                got += len(b)
    if total and got != total:
        raise RuntimeError("下载不完整：{}/{}".format(got, total))
    if os.path.exists(dest):
        os.remove(dest)
    os.replace(tmp, dest)
    print("    [完成] {:.1f} MB".format(got / 1048576))
    return dest


def extract_flatten(zpath: str, outdir: str) -> str:
    """解压到 outdir；若压缩包只有一层顶层目录，则拍平（PlatformIO 期望的布局）。"""
    with zipfile.ZipFile(zpath) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        tops = {n.split("/")[0] for n in names if "/" in n}
        single = len(tops) == 1 and all("/" in n for n in names)
        prefix = (list(tops)[0] + "/") if single else ""
        print("    解压：{} 个文件，顶层目录 {} ⇒ {}".format(
            len(names), list(tops)[:3], "拍平 1 层" if single else "保持原状"))
        for m in z.infolist():
            name = m.filename
            if prefix:
                if not name.startswith(prefix):
                    continue
                name = name[len(prefix):]
            if not name:
                continue
            dst = os.path.join(outdir, name.replace("/", os.sep))
            if m.is_dir():
                os.makedirs(dst, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
                with z.open(m) as src, open(dst, "wb") as f:
                    shutil.copyfileobj(src, f)
        return prefix or "(无)"


def write_manifests(outdir: str, t: dict, ver: str) -> None:
    spec = {"owner": t["pio_owner"], "id": 0, "name": t["pkg"],
            "requirements": None, "uri": None}
    pj = {"name": t["pkg"], "version": ver, "description": t["desc"],
          "system": ["windows_amd64", "windows_arm64", "linux_x86_64",
                     "linux_aarch64", "darwin_x86_64", "darwin_arm64"],
          "spec": spec}
    piopm = {"name": t["pkg"], "version": ver, "spec": spec, "type": t["pio_type"]}
    with open(os.path.join(outdir, "package.json"), "w", encoding="utf-8") as f:
        json.dump(pj, f, ensure_ascii=False, indent=2)
    with open(os.path.join(outdir, ".piopm"), "w", encoding="utf-8") as f:
        json.dump(piopm, f, ensure_ascii=False)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    a = ap.parse_args()

    vers = platform_versions()
    os.makedirs(PKGS, exist_ok=True)
    os.makedirs(DL, exist_ok=True)
    print("platform.json 声明的版本：{}".format(
        {t["pkg"]: vers.get(t["pkg"]) for t in TARGETS}))
    print()

    for t in TARGETS:
        ver = vers.get(t["pkg"]) or "0.0.0"
        out = os.path.join(PKGS, t["pkg"])
        print("[{}]  版本={}  目标={}".format(t["pkg"], ver, out))
        print("       {}".format(t["desc"]))
        if a.list:
            print("       URL = {}".format(t["url"]))
            continue
        if os.path.isdir(out) and not a.rebuild:
            print("       已存在，跳过（--rebuild 可覆盖）")
            continue
        if os.path.isdir(out) and a.rebuild:
            shutil.rmtree(out, ignore_errors=True)
        z = fetch(t["url"], os.path.join(DL, os.path.basename(t["url"])))
        os.makedirs(out, exist_ok=True)
        extract_flatten(z, out)
        write_manifests(out, t, ver)
        print("       ✅ 完成（已写入 package.json / .piopm）")
    print()
    print("下一步：把 platformio.ini 里的 platform_packages 覆盖**注释掉**，再跑 pio run。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
