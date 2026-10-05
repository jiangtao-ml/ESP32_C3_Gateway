#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NTP 三源改造 —— 烧录后验收脚本
================================

用途：抓 ESP32 串口启动日志，自动判定 NTP 三源改造是否生效，并回答核心问题：
      **「实际生效源」是内网 172.16.0.2，还是落到了公网备源？**

背景（2026-10-04）：
  - 导师提供内网 chrony 源 172.16.0.2（实测 stratum 2 / RTT 3.4ms，公网对照 41ms）。
  - config.h 已由两源改三源：主 172.16.0.2 → 备1 ntp.aliyun.com → 备2 203.107.6.88。
  - 固件新增 logNtpEffectiveSource()，用 sntp_getserver() 打印**实际回包的源 IP**。
  - ⚠️ 172.16.0.0/12 是私网段，与 ESP32 所在 192.168.1.0/24 不同网段 ⇒
    「电脑能到」不等于「ESP32 能到」，**必须在设备侧实测确认**。本脚本就是做这件事的。

用法：
    python ntp_verify.py COM5          # 默认抓 45 秒
    python ntp_verify.py COM5 60

注意：
  1. 关掉 Arduino IDE 的串口监视器（同一端口不能被两个程序打开）。
  2. 打开串口通常会触发 ESP32-C3 复位，从而打印完整启动日志；
     若没看到 `[WIFI] 静态配置` 之类的启动行，**手动按一下板上的 RST 键**。
  3. 本脚本**只读串口，不发送任何数据**。
"""

import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
BAUD = 115200

# 期望值（与 config.h / 00_context.md 对齐）
EXP_MAIN = "172.16.0.2"
EXP_ALT1 = "ntp.aliyun.com"
EXP_ALT2 = "203.107.6.88"
EXP_TZ = "CST-8"
EXP_ESP_IP = "192.168.1.200"

RE_ACTUAL = re.compile(r"\[NTP\]\s*实际生效源：\s*(\S+)")
RE_TRIPLE = re.compile(r"\[NTP\]\s*三源配置\s*(.*)")
RE_SYNCED = re.compile(r"\[NTP\]\s*时间已同步：本地\s*(\S+ \S+)\s*(\S+)")
RE_LOST = re.compile(r"\[NTP\]\s*时间同步丢失")
RE_WIFI = re.compile(r"\[WIFI\]\s*已连接\s*SSID=(.*)")
RE_UDP = re.compile(r"\[UDP -> (\S+?)\]")


def main() -> int:
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    print("[{}] 已打开 {} @{}，抓取 {:.0f} 秒……".format(
        time.strftime("%H:%M:%S"), PORT, BAUD, SECONDS), flush=True)

    raw_path = "03_logs/NTP验收_{}.log".format(time.strftime("%Y%m%d_%H%M%S"))
    raw = open(raw_path, "w", encoding="utf-8")
    buf = ""
    lines = []
    t0 = time.time()
    while (time.time() - t0) < SECONDS:
        data = ser.read(4096)
        if not data:
            continue
        buf += data.decode("utf-8", errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip("\r")
            raw.write(line + "\n")
            if line.strip():
                lines.append(line)
                print("[esp] {}".format(line), flush=True)
    ser.close()
    raw.close()

    text = "\n".join(lines)

    # ---------------- 判定 ----------------
    triple = RE_TRIPLE.search(text)
    triple_ok = bool(triple) and all(k in triple.group(1)
                                     for k in (EXP_MAIN, EXP_ALT1, EXP_ALT2, EXP_TZ))

    synced = RE_SYNCED.search(text)
    sync_ok = bool(synced)
    tz_ok = bool(synced) and synced.group(2) == "+0800"

    actual = RE_ACTUAL.search(text)
    actual_ip = actual.group(1) if actual else None

    wifi = RE_WIFI.search(text)
    udp_targets = sorted(set(RE_UDP.findall(text)))

    out = []
    out.append("")
    out.append("=" * 66)
    out.append("NTP 三源改造 —— 烧录验收报告")
    out.append("=" * 66)
    out.append("抓取端口 / 时长     : {} / {:.0f} s".format(PORT, SECONDS))
    out.append("串口总行数          : {}".format(len(lines)))
    out.append("")
    out.append("[1] setup 启动打印三源配置 : {}".format("PASS" if triple_ok else "FAIL"))
    if triple:
        out.append("    实测内容：{}".format(triple.group(1).strip()))
    else:
        out.append("    ⚠️ 未抓到该行 —— 可能没看到启动日志，请按 RST 后重跑")
    out.append("")
    out.append("[2] 三源配置含全部期望值   : {}".format(
        "PASS" if triple_ok else "FAIL（期望包含 {} / {} / {} / {}）".format(
            EXP_MAIN, EXP_ALT1, EXP_ALT2, EXP_TZ)))
    out.append("")
    out.append("[3] WiFi 已连接            : {}".format("PASS" if wifi else "FAIL"))
    if wifi:
        out.append("    实测：SSID={}".format(wifi.group(1).strip()))
    out.append("")
    out.append("[4] 时间已同步             : {}".format("PASS" if sync_ok else "FAIL"))
    if synced:
        out.append("    本地时间：{}  {}".format(synced.group(1), synced.group(2)))
    out.append("")
    out.append("[5] 时区为东八区 (+0800)   : {}".format("PASS" if tz_ok else "FAIL"))
    out.append("")
    if actual_ip:
        if actual_ip == EXP_MAIN:
            verdict = "PASS —— ✅ 实际生效源是内网 chrony ⇒ 首次在设备侧确认 172.16.0.2 可达"
        elif actual_ip in (EXP_ALT1, EXP_ALT2):
            verdict = ("FAIL（但结论有效）—— ⚠️ 落到了公网备源 ⇒ "
                       "ESP32 → 172.16.0.2 不通，应把内网源降级为可选、仍以公网为主")
        else:
            verdict = "⚠️ 无法归类，实际生效源 = {}".format(actual_ip)
    else:
        verdict = "FAIL —— 未抓到「实际生效源」行（核心交付未验证）"
    out.append("[6] ★核心★ 实际生效源      : {}".format(verdict))
    out.append("")
    out.append("[7] UDP 目标（参考，与 NTP 无关）: {}".format(
        ", ".join(udp_targets) if udp_targets else "未抓到"))
    out.append("    注：电脑 IP 会漂移。若此处与当前 `ipconfig /all` 的 WLAN 地址不一致，")
    out.append("        NetAssist 收不到数据 —— 但**不影响本脚本的 NTP 判定**。")
    out.append("")
    out.append("原始日志：{}".format(raw_path))
    out.append("=" * 66)

    text_out = "\n".join(out)
    print(text_out, flush=True)
    with open("03_logs/NTP验收_report.txt", "w", encoding="utf-8") as f:
        f.write(text_out + "\n")

    return 0 if (triple_ok and sync_ok and tz_ok and actual_ip) else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[ntp_verify] 手工中断", flush=True)
        sys.exit(130)
    except serial.SerialException as exc:
        print("[ntp_verify] 串口异常：{}".format(exc), flush=True)
        sys.exit(1)
