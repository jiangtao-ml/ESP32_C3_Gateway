#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PlatformIO 国内离线引导 v2.0 —— 支持 arduino-esp32 2.x 与 pioarduino 3.x
================================================================================

背景（2026-10-05/06 实测）：
  - PlatformIO 官方 CDN 在国内 **≈14 KB/s**；`github.com` 直连**超时**（本机实测 0 字节）
    ⇒ 工具链/framework 是 GB 量级，正常路径基本下不动。
  - 两条可用通道（`02_docs/20` 归纳，均已实测）：
      规则 A：`github.com/espressif/<rest>`  →  `dl.espressif.cn/github_assets/espressif/<rest>`
      规则 B：`github.com/<其它组织>/<rest>` →  `https://gh-proxy.com/https://github.com/<其它组织>/<rest>`
  - ⚠️ 乐鑫给的是 **Espressif 原版包**（包根缺 `package.json`），PlatformIO 会报
    `MissingPackageManifestError` ⇒ 必须 **解压后自己补 `package.json` + `.piopm`**。
  - ⚠️ pioarduino 3.x 的 `toolchain-*` / `tool-*` 包在 `pioarduino/registry` 里**只是"清单桩"**
    （zip 内只有 `package.json` + `tools.json`，1~2 KB）；**真实下载地址写在 `tools.json` 里**，
    且指向 `github.com/espressif/...` ⇒ **规则 A 适用**。本脚本会自动解析。

v2.0 相对 v1.0 的变化：
  1. **平台驱动**：不再硬编码包清单 —— 读 `~/.platformio/platforms/espressif32/platform.json`
     的 `packages` 段，自动解析每个包的"声明 URL"。
  2. **桩包自动解析**：下载桩 → 读 `tools.json` → 取 `win64`（或 `win32`）真实地址与体积。
  3. **双通道 URL 改写**（规则 A/B）。
  4. **支持 `tar.xz`**（3.x 的 framework / espidf 都换成了 tar.xz）。
  5. **平台自身也由本脚本落盘**（含 `.piopm`），并把 `platform.json` 里"URL 形式的版本"
     回写成**具体版本号**，使 PlatformIO 认账。
  6. 原有的 2.0.17 路径保留为 `--set legacy2x`。

用法：
    python pio_cn_seed.py --set c3                 # 3.x：预置 ESP32-C3 + arduino 所需包
    python pio_cn_seed.py --set c3 --only framework-espidf,tool-esptoolpy
    python pio_cn_seed.py --set c3 --list          # 只解析、不下载
    python pio_cn_seed.py --set legacy2x           # 旧的 2.0.17 路径

⚠️ 本脚本会写入 `~/.platformio/`（用户级目录），**不碰仓库里的任何源码**。
⚠️ `--rebuild` 会先把同名旧包目录**改名为 `*.pio2.bak`**（不删除），以便随时回退。
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

# ---------------------------------------------------------------- 常量
CDN = "https://dl.espressif.cn/github_assets/"
PROXY = "https://gh-proxy.com/"
GH = "https://github.com/"

PIO = os.path.expanduser("~/.platformio")
PKGS = os.path.join(PIO, "packages")
PLATFORMS = os.path.join(PIO, "platforms")
DL = os.path.join(PIO, ".cache", "downloads")

DEFAULT_PLATFORM = "55.03.311"      # pioarduino → Arduino 3.3.11 / IDF 5.5.5
PLATFORM_NAME = "espressif32"
MIN_OK = 128                        # 下载成功的最小体积（桩包只有 ~1 KB，别把门槛设成 1 KB）

# 3.x（pioarduino）下 ESP32-C3 + arduino 所需包
C3_SET = [
    "framework-arduinoespressif32",
    "framework-arduinoespressif32-libs",
    "framework-espidf",
    "toolchain-riscv32-esp",
    "tool-esptoolpy",
    "tool-esp_install",
    "tool-scons",
    "tool-cmake",
    "tool-ninja",
    "tool-esp-rom-elfs",
]

# 2.x 旧路径（保留）
LEGACY2X = [
    dict(pkg="toolchain-riscv32-esp", pio_type="tool", pio_owner="espressif",
         url=CDN + "espressif/crosstool-NG/releases/download/esp-2021r2-patch5/"
                   "riscv32-esp-elf-gcc8_4_0-esp-2021r2-patch5-win64.zip",
         desc="ESP32-C3 RISC-V 工具链（2.0.17 时代）"),
    dict(pkg="framework-arduinoespressif32", pio_type="framework", pio_owner="platformio",
         url=CDN + "espressif/arduino-esp32/releases/download/2.0.17/esp32-2.0.17.zip",
         desc="arduino-esp32 2.0.17 框架源码"),
]


# ---------------------------------------------------------------- 工具
def rewrite(url):
    """两条通道规则。"""
    if url.startswith(GH + "espressif/"):
        return CDN + url[len(GH):]
    if url.startswith(GH):
        return PROXY + url
    return url


def cache_path(url):
    return os.path.join(DL, hashlib.sha1(url.encode()).hexdigest())


def human(n):
    return "%.1f MB" % (n / 1048576.0) if n else "?"


def park(path, suffix="pio2.bak"):
    """把已存在的目录**改名**归档（绝不删除；删除在沙箱里会被安全策略拦截）。
    返回归档路径，或 None（路径不存在）。"""
    if not os.path.isdir(path):
        return None
    i = 0
    while True:
        cand = "%s.%s%s" % (path, suffix, "" if i == 0 else ".%d" % i)
        if not os.path.exists(cand):
            shutil.move(path, cand)
            print("    旧内容已归档 → %s" % os.path.basename(cand))
            return cand
        i += 1


def _curl_fetch(url, dest):
    """优先用系统 curl（跟重定向、可重试，实测比 urllib 稳）。"""
    exe = shutil.which("curl")
    if not exe:
        return False
    cmd = [exe, "-L", "--fail", "--silent", "--show-error",
           "--retry", "4", "--retry-delay", "3", "--retry-all-errors",
           "--connect-timeout", "20", "--max-time", "1800",
           "-o", dest, url]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True)
    except Exception as e:
        print("      curl 异常：%s" % e)
        return False
    if p.returncode != 0:
        if p.stderr:
            print("      curl 返回 %d：%s" % (p.returncode, p.stderr.strip()[:160]))
        return False
    return os.path.exists(dest) and os.path.getsize(dest) >= MIN_OK


def _urllib_fetch(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
        while True:
            b = r.read(524288)
            if not b:
                break
            f.write(b)
    return os.path.exists(dest) and os.path.getsize(dest) >= MIN_OK


def variants(url):
    """同一资产的多个可达入口（代理挂了能换一个）。"""
    if url.startswith(PROXY + GH):
        inner = url[len(PROXY):]                    # https://github.com/...
        return [url, "https://ghfast.top/" + inner, inner]
    return [url]


def download(url, expect_size=None, label=""):
    """下载到 PlatformIO 缓存目录（同名复用）。curl 优先，urllib 兜底，多代理轮试。"""
    os.makedirs(DL, exist_ok=True)
    dest = cache_path(url)
    if os.path.exists(dest) and os.path.getsize(dest) >= MIN_OK:
        sz = os.path.getsize(dest)
        if not expect_size or abs(sz - expect_size) < 4096:
            print("    [缓存] %s（%s）" % (os.path.basename(dest)[:12], human(sz)))
            return dest
    print("    [下载] %s" % (label or url.split("/")[-1]))
    tmp = dest + ".part"
    last = None
    ok = False
    for u in variants(url):
        for attempt in (1, 2):
            try:
                if os.path.exists(tmp):
                    os.remove(tmp)
                if _curl_fetch(u, tmp) or _urllib_fetch(u, tmp):
                    ok = True
                    break
            except Exception as e:
                last = e
                print("      异常：%s" % e)
            print("      第 %d 次失败（%s），重试…" % (attempt, u.split("/")[2]))
            time.sleep(2)
        if ok:
            break
    if not ok:
        raise RuntimeError("下载失败：%s（%s）" % (url, last))
    got = os.path.getsize(tmp)
    if expect_size and abs(got - expect_size) > 4096:
        os.remove(tmp)
        raise RuntimeError("体积不符：%s（期望 %s）" % (human(got), human(expect_size)))
    if os.path.exists(dest):
        os.remove(dest)
    os.replace(tmp, dest)
    print("    [完成] %s" % human(got))
    return dest


def detect_format(path):
    """按魔数判断压缩格式（缓存文件名是 sha1，没有后缀，不能靠扩展名）。"""
    with open(path, "rb") as f:
        head = f.read(6)
    if head[:4] == b"PK\x03\x04":
        return "zip"
    if head[:2] == b"\x1f\x8b":            # gzip
        return "tar"
    if head[:6] == b"\xfd7zXZ\x00":        # xz
        return "tar"
    if head[:3] == b"BZh":                 # bzip2
        return "tar"
    return "zip"


def extract(archive, outdir):
    """解压到 outdir；若只有一层顶层目录则拍平。格式按魔数嗅探（zip / tar.xz / tar.gz）。"""
    is_tar = detect_format(archive) == "tar"
    if is_tar:
        with tarfile.open(archive) as t:
            names = [m.name for m in t.getmembers() if m.isfile()]
            tops = {n.split("/")[0] for n in names if "/" in n}
            single = len(tops) == 1 and all("/" in n for n in names)
            prefix = (list(tops)[0] + "/") if single else ""
            print("    解压(tar)：%d 个文件，顶层 %s ⇒ %s" % (
                len(names), list(tops)[:3], "拍平 1 层" if single else "保持原状"))
            members = t.getmembers()
            for m in members:
                name = m.name
                if prefix:
                    if not name.startswith(prefix):
                        continue
                    name = name[len(prefix):]
                if not name:
                    continue
                dst = os.path.join(outdir, name.replace("/", os.sep))
                if m.isdir():
                    os.makedirs(dst, exist_ok=True)
                elif m.isfile():
                    os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
                    src = t.extractfile(m)
                    if src is None:
                        continue
                    with src, open(dst, "wb") as f:
                        shutil.copyfileobj(src, f)
    else:
        with zipfile.ZipFile(archive) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            tops = {n.split("/")[0] for n in names if "/" in n}
            single = len(tops) == 1 and all("/" in n for n in names)
            prefix = (list(tops)[0] + "/") if single else ""
            print("    解压(zip)：%d 个文件，顶层 %s ⇒ %s" % (
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


def write_manifests(outdir, pkg, ver, pio_type, pio_owner, desc=""):
    spec = {"owner": pio_owner, "id": 0, "name": pkg, "requirements": None, "uri": None}
    pj = {"name": pkg, "version": ver, "description": desc or pkg,
          "system": ["windows_amd64", "windows_arm64", "linux_x86_64",
                     "linux_aarch64", "darwin_x86_64", "darwin_arm64"],
          "spec": spec}
    with open(os.path.join(outdir, "package.json"), "w", encoding="utf-8") as f:
        json.dump(pj, f, ensure_ascii=False, indent=2)
    with open(os.path.join(outdir, ".piopm"), "w", encoding="utf-8") as f:
        json.dump({"type": pio_type, "name": pkg, "version": ver, "spec": spec},
                  f, ensure_ascii=False)


def guess_ver(url, fallback="0.0.0"):
    m = re.search(r"[-_v](\d+\.\d+\.\d+)", os.path.basename(url))
    return m.group(1) if m else fallback


_TYPES = {"framework": "framework", "toolchain": "toolchain", "tool": "tool",
          "debugger": "tool", "uploader": "tool", "platform": "platform"}


def norm_type(t):
    """PlatformIO 的 .piopm type 只认 tool/toolchain/framework/platform/library。"""
    return _TYPES.get(str(t), "tool")


def pkgjson_ver(outdir):
    """解压后目录里若带 package.json，取它的 version。"""
    p = os.path.join(outdir, "package.json")
    try:
        return json.load(open(p, encoding="utf-8")).get("version")
    except Exception:
        return None


# ---------------------------------------------------------------- 桩包
def read_stub(zpath, pkg):
    """若 zip 是 pioarduino 的"清单桩"（只有 package.json+tools.json），返回 (ver, real_url, size)。"""
    try:
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
            if "tools.json" not in names or len(names) > 4:
                return None
            pj = json.loads(z.read("package.json").decode("utf-8")) if "package.json" in names else {}
            tj = json.loads(z.read("tools.json").decode("utf-8"))
    except Exception:
        return None
    for tool in tj.get("tools", []):
        if tool.get("name") != pkg:
            continue
        vers = tool.get("versions") or []
        if not vers:
            continue
        v0 = vers[0]
        for key in ("win64", "win32"):
            entry = v0.get(key)
            if isinstance(entry, dict) and entry.get("url"):
                return pj.get("version"), entry["url"], entry.get("size")
    return None


# ---------------------------------------------------------------- 平台
def ensure_platform(ver):
    """把 pioarduino 平台落盘到 ~/.platformio/platforms/espressif32（旧版整体归档，不删除）。
    版本已正确时**不重装**（重装由版本差异触发，与包的 --rebuild 解耦）。"""
    pdir = os.path.join(PLATFORMS, PLATFORM_NAME)
    pjson = os.path.join(pdir, "platform.json")
    cur = None
    if os.path.exists(pjson):
        try:
            cur = json.load(open(pjson, encoding="utf-8")).get("version")
        except Exception:
            cur = None
    if cur is not None and str(cur) == str(ver):
        print("[平台] 已是 %s，跳过" % ver)
        return pdir
    if os.path.isdir(pdir):
        print("[平台] 现有版本 %s ≠ %s ⇒ 归档后重装" % (cur, ver))
        park(pdir)

    url = (PROXY + GH + "pioarduino/platform-espressif32/releases/download/%s/"
           "platform-espressif32.zip" % ver)
    print("[平台] 获取 pioarduino %s" % ver)
    z = download(url, label="platform-espressif32-%s.zip" % ver)
    os.makedirs(pdir, exist_ok=True)
    extract(z, pdir)
    write_manifests(pdir, PLATFORM_NAME, ver, "platform", "pioarduino",
                    desc="pioarduino espressif32 platform %s" % ver)
    return pdir


def load_packages(pdir):
    with open(os.path.join(pdir, "platform.json"), encoding="utf-8") as f:
        return json.load(f).get("packages") or {}


def patch_platform_json(pdir, versions):
    """把 platform.json 里"URL 形式的 version"回写成具体版本号，使 PlatformIO 认账。"""
    pj = os.path.join(pdir, "platform.json")
    orig = pj + ".orig"
    if not os.path.exists(orig):
        shutil.copy2(pj, orig)
    with open(pj, encoding="utf-8") as f:
        d = json.load(f)
    changed = 0
    for k, v in (d.get("packages") or {}).items():
        if isinstance(v, dict) and isinstance(v.get("version"), str) and v["version"].startswith("http"):
            if k in versions:
                v["version"] = versions[k]
                changed += 1
    with open(pj, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    print("[平台] platform.json 已回写 %d 个包的版本（原件 .orig）" % changed)


# ---------------------------------------------------------------- 主流程
def seed_pkg(pkg, spec, rebuild, listonly):
    """预置单个包。返回具体版本号。

    版本优先级：platform.json 的 `package-version` → 桩包自带 → 解压后 package.json → URL 推导。
    """
    declared = spec.get("version") if isinstance(spec, dict) else spec
    pv = spec.get("package-version") if isinstance(spec, dict) else None
    pio_type = (spec or {}).get("type", "tool") if isinstance(spec, dict) else "tool"
    pio_owner = (spec or {}).get("owner", "espressif") if isinstance(spec, dict) else "espressif"

    print("[%s]" % pkg)
    print("    声明：%s" % str(declared)[:110])
    ch = "A(dl.espressif.cn)" if str(declared).startswith(GH + "espressif/") \
        else "B(gh-proxy)" if str(declared).startswith(GH) else "直接"
    print("    通道：%s    package-version=%s" % (ch, pv))
    out = os.path.join(PKGS, pkg)
    if listonly:
        return None
    if os.path.isdir(out) and not rebuild:
        print("    已存在，跳过（--rebuild 可覆盖）")
        return None
    if os.path.isdir(out) and rebuild:
        park(out)

    stub = download(rewrite(declared), label=os.path.basename(str(declared)))
    parsed = read_stub(stub, pkg)
    ver = pv
    if parsed:
        stub_ver, real_url, size = parsed
        print("    桩包 → 真实地址：%s" % rewrite(real_url))
        payload = download(rewrite(real_url), expect_size=size, label=real_url.split("/")[-1])
        ver = ver or stub_ver
    else:
        payload = stub
    os.makedirs(out, exist_ok=True)
    extract(payload, out)
    if not ver:
        ver = pkgjson_ver(out) or guess_ver(str(declared))
    write_manifests(out, pkg, ver, norm_type(pio_type), pio_owner)
    print("    ✅ %s @ %s" % (pkg, ver))
    return ver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", choices=["c3", "legacy2x"], default="c3")
    ap.add_argument("--platform", default=DEFAULT_PLATFORM)
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    a = ap.parse_args()

    os.makedirs(PKGS, exist_ok=True)
    os.makedirs(DL, exist_ok=True)

    if a.set == "legacy2x":
        print("== 旧路径（arduino-esp32 2.0.17）==")
        for t in LEGACY2X:
            out = os.path.join(PKGS, t["pkg"])
            if a.list:
                print("[%s] %s" % (t["pkg"], t["url"]))
                continue
            if os.path.isdir(out) and not a.rebuild:
                print("[%s] 已存在，跳过" % t["pkg"])
                continue
            if os.path.isdir(out) and a.rebuild:
                park(out)
            z = download(t["url"], label=t["url"].split("/")[-1])
            os.makedirs(out, exist_ok=True)
            extract(z, out)
            write_manifests(out, t["pkg"], "0.0.0", t["pio_type"], t["pio_owner"], t["desc"])
        return 0

    # ---- 3.x（pioarduino）----
    pdir = ensure_platform(a.platform) if not a.list else None
    if a.list:
        # 仅解析已落盘的 platform.json
        pdir = os.path.join(PLATFORMS, PLATFORM_NAME)
    pkgs = load_packages(pdir)
    only = [s for s in a.only.split(",") if s]
    targets = [p for p in (only or C3_SET) if p in pkgs] or C3_SET

    print("\n== 目标包（%d）==" % len(targets))
    versions = {}
    for pkg in targets:
        v = seed_pkg(pkg, pkgs.get(pkg, {}), a.rebuild, a.list)
        if v:
            versions[pkg] = v
    if versions and not a.list:
        patch_platform_json(pdir, versions)

    print("\n下一步：`pio run -d 01_firmware/pio_verify`（首次编译会补齐剩余 optional 包）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
