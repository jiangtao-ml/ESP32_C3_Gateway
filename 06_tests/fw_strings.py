#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
固件 flash dump 分析：从 ESP32 固件镜像里提取字符串、发现协议字段与配置页
================================================================================

用途：手里只有一台**已烧好固件**的板子、没有源码时，用 `esptool read_flash` 把整片 flash
      dump 出来，再用本脚本把里面的**可读信息**捞出来 —— 常见收获：
       - 内置 Web 配置页的 HTML / JS（能看出可改哪些参数、有没有鉴权）
       - 上报 JSON 的**字段名**（对齐格式用）
       - 各种状态码 / 错误码枚举（如 `quality` 的取值）
       - WiFi / NTP / 端口等默认值（不含密钥；密钥通常是运行时输入的）

⚠️ 边界：这是**别人的固件**。读出来做**格式对齐**是正常工程手段；但如果打算**修改或复用**，
   更该做的是**向作者要源码** —— 逆向只能拿到字符串，优化、宏、隐藏逻辑都看不到。

用法：
    python fw_strings.py <dump.bin>                 # 打印分类报告
    python fw_strings.py <dump.bin> --raw           # 额外导出全部字符串到 <dump>_strings.txt
    python fw_strings.py <dump.bin> --grep 关键词    # 只列含该关键词的字符串
"""
import os
import re
import sys

# 关注的关键词（按用途分组，命中即高亮）
WATCH = {
    "格式/字段": ["quality", "openState", "record", "recor", "frequent", "leakage",
                  "voltage", "current", "_temp", "_Pf", "_power", "seq", "dev", "ts_src"],
    "配置/配网": ["ssid", "password", "AP", "ap_", "ESP485", "esp32", "config", "save",
                  "chrony", "ntp", "udp", "port", "server", "poll"],
    "Web/HTTP": ["<html", "<!DOCTYPE", "<title", "<form", "Content-Type", "HTTP/1.1",
                 "GET ", "POST ", "/status", "/config", "/save", "/wifi", "text/html",
                 "application/json", "charset"],
    "错误/质量": ["err", "ERR", "fail", "FAIL", "timeout", "TIMEOUT", "crc", "CRC",
                  "dropped", "queue", "ok", "OK"],
    "固件信息": ["fw", "version", "v1.", "ESP-IDF", "arduino-esp32", "boot", "partition"],
}

ASCII_RE = re.compile(rb"[\x20-\x7e]{4,}")
# 允许 ASCII 与 UTF-8 中文字节混排的长串（用于捞中文界面文字）
MIXED_RE = re.compile(rb"(?:[\x20-\x7e]|[\xe4-\xe9][\x80-\xbf]{2}|[\xe3][\x80-\xbf]{2}){4,}")


def decode(b: bytes) -> str:
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("latin-1")


def extract(data: bytes):
    ascii_strs, mixed_strs = [], []
    for m in ASCII_RE.finditer(data):
        ascii_strs.append((m.start(), m.group().decode("latin-1")))
    for m in MIXED_RE.finditer(data):
        raw = m.group()
        if all(0x20 <= c <= 0x7e for c in raw):
            continue                      # 纯 ASCII 已收
        s = decode(raw)
        mixed_strs.append((m.start(), s))
    return ascii_strs, mixed_strs


def main() -> int:
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    path = a[0]
    if not os.path.exists(path):
        print("找不到文件：{}".format(path))
        return 1
    data = open(path, "rb").read()
    print("文件：{}  大小：{:.2f} MB".format(path, len(data) / 1048576.0))
    print()

    ascii_strs, mixed_strs = extract(data)
    print("可打印 ASCII 串：{} 条；含非 ASCII（可能中文）串：{} 条".format(len(ascii_strs), len(mixed_strs)))
    print()

    if "--grep" in a:
        kw = a[a.index("--grep") + 1]
        hits = [(o, s) for o, s in ascii_strs + mixed_strs if kw.lower() in s.lower()]
        print("含「{}」的串：{} 条".format(kw, len(hits)))
        for o, s in hits[:200]:
            print("  0x{:06X}  {}".format(o, s[:160]))
        return 0

    print("=" * 78)
    print("按用途分类命中（每条最多列 12 个，去重）")
    print("=" * 78)
    for group, kws in WATCH.items():
        seen, rows = set(), []
        for o, s in ascii_strs + mixed_strs:
            low = s.lower()
            for k in kws:
                if k.lower() in low:
                    key = s.strip()[:120]
                    if key and key not in seen:
                        seen.add(key)
                        rows.append((o, key))
                    break
        print()
        print("【{}】命中 {} 个不同串".format(group, len(rows)))
        for o, s in rows[:12]:
            print("  0x{:06X}  {}".format(o, s))
        if len(rows) > 12:
            print("  … 另有 {} 个".format(len(rows) - 12))

    # JSON 键名（形如 "xxx": ）
    print()
    print("=" * 78)
    keys = set()
    for _, s in ascii_strs + mixed_strs:
        for m in re.finditer(r'"([A-Za-z_][A-Za-z0-9_]{1,24})"\s*:', s):
            keys.add(m.group(1))
    print("疑似 JSON 键名（{} 个）：".format(len(keys)))
    print("  " + ", ".join(sorted(keys)))

    # 内置 HTML 页
    print()
    print("=" * 78)
    html_chunks = []
    for o, s in mixed_strs + ascii_strs:
        low = s.lower()
        if any(t in low for t in ("<html", "<!doctype", "<form", "<title", "<script", "<style",
                                  "text/html", "<meta", "<body", "<input")):
            html_chunks.append((o, s))
    print("疑似 HTML / 前端片段：{} 条".format(len(html_chunks)))
    for o, s in html_chunks[:40]:
        print("  0x{:06X}  {}".format(o, s[:170]))

    if "--raw" in a:
        out = os.path.splitext(path)[0] + "_strings.txt"
        with open(out, "w", encoding="utf-8") as f:
            for o, s in sorted(ascii_strs + mixed_strs):
                f.write("0x{:06X}\t{}\n".format(o, s))
        print()
        print("已导出全部字符串：{}".format(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
