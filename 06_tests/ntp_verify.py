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
RE_ACTUAL_FALLBACK = re.compile(r"\[NTP\]\s*实际生效源：暂未能")
RE_TRIPLE = re.compile(r"\[NTP\]\s*三源配置\s*(.*)")
RE_SYNCED = re.compile(r"\[NTP\]\s*时间已同步：本地\s*(\S+ \S+)\s*(\S+)")
RE_LOST = re.compile(r"\[NTP\]\s*时间同步丢失")
RE_WIFI = re.compile(r"\[WIFI\]\s*已连接\s*SSID=(.*)")
RE_UDP = re.compile(r"\[UDP -> (\S+?)\]")
RE_UDP_ERR = re.compile(r"\[UDP\]\[ERR\]\s*发送失败")
RE_BOOT_MARK = re.compile(r"\[BOOT\]|\[WIFI\] 静态配置|\[485\] UART1 就绪")
# 「WiFi 已连上 -> 触发 SNTP 对时」= 本次会话真正开始对时的时刻。
# 若「时间已同步」出现在它【之前】，说明判定过早（复位后 RTC 保留时间会导致
# getLocalTime() 立即为真，于是 TZ 未生效时就被判为"已同步"）—— 见 04_ai_prompts/02。
RE_NTP_TRIGGER = re.compile(r"\[NTP\]\s*WiFi 已连上\s*->\s*触发 SNTP 对时")


def _open(stamp_on_fail: bool = True):
    """尝试打开端口；失败返回 None（板子复位后 USB 会重新枚举，需要等它回来）。"""
    try:
        return serial.Serial(PORT, BAUD, timeout=0.2)
    except serial.SerialException:
        return None


def main() -> int:
    ser = None
    for _ in range(40):                      # 最多等 12 秒让端口出现
        ser = _open()
        if ser is not None:
            break
        time.sleep(0.3)
    if ser is None:
        print("[ntp_verify] 无法打开 {} —— 是否被串口监视器占用？".format(PORT), flush=True)
        return 1

    print("[{}] 已打开 {} @{}，抓取 {:.0f} 秒……".format(
        time.strftime("%H:%M:%S"), PORT, BAUD, SECONDS), flush=True)
    print("        ⚠️ 若要抓**开机日志**，请现在按一下板上的 RST 键"
          "（本脚本会自动等端口重新枚举并续抓）。", flush=True)

    raw_path = "03_logs/NTP验收_{}.log".format(time.strftime("%Y%m%d_%H%M%S"))
    raw = open(raw_path, "w", encoding="utf-8")
    buf = ""
    lines = []
    reconnects = 0
    deadline = time.time() + SECONDS

    def feed(chunk_text: str):
        nonlocal buf
        buf += chunk_text
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip("\r")
            raw.write(line + "\n")
            if line.strip():
                lines.append(line)
                print("[esp] {}".format(line), flush=True)

    while time.time() < deadline:
        try:
            data = ser.read(4096)
        except (serial.SerialException, OSError) as exc:
            # ESP32-C3 原生 USB CDC：芯片复位会导致 USB 重新枚举 → 端口掉线。
            # 这是**预期行为**（用户按 RST / 看门狗复位都会触发），不是错误。
            reconnects += 1
            print("\n[{}] ⚠️ 端口掉线（第 {} 次）——板子应已复位，等待重新枚举……".format(
                time.strftime("%H:%M:%S"), reconnects), flush=True)
            try:
                ser.close()
            except Exception:
                pass
            t_wait = time.time()
            ser = None
            while time.time() < deadline:
                ser = _open()
                if ser is not None:
                    break
                time.sleep(0.3)
            if ser is None:
                print("[ntp_verify] 等待端口超时，提前结束", flush=True)
                break
            waited = time.time() - t_wait
            deadline += waited                       # 补回掉线等待时间
            print("[{}] ✅ 端口已恢复（等待 {:.1f}s），继续抓取\n".format(
                time.strftime("%H:%M:%S"), waited), flush=True)
            continue
        if not data:
            continue
        feed(data.decode("utf-8", errors="replace"))

    if ser is not None:
        try:
            ser.close()
        except Exception:
            pass
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
    actual_is_fallback = bool(RE_ACTUAL_FALLBACK.search(text))

    # 顺序判定：真正开始对时 vs 打印「已同步」
    trig_idx = sync_idx = None
    for i, ln in enumerate(lines):
        if trig_idx is None and RE_NTP_TRIGGER.search(ln):
            trig_idx = i
        if sync_idx is None and RE_SYNCED.search(ln):
            sync_idx = i
    premature = (trig_idx is not None and sync_idx is not None and sync_idx < trig_idx)

    wifi = RE_WIFI.search(text)
    udp_targets = sorted(set(RE_UDP.findall(text)))
    udp_ok_n = len(RE_UDP.findall(text))
    udp_err_n = len(RE_UDP_ERR.findall(text))
    saw_boot = bool(RE_BOOT_MARK.search(text))

    out = []
    out.append("")
    out.append("=" * 66)
    out.append("NTP 三源改造 —— 烧录验收报告")
    out.append("=" * 66)
    out.append("抓取端口 / 时长     : {} / {:.0f} s".format(PORT, SECONDS))
    out.append("串口总行数          : {}".format(len(lines)))
    out.append("端口掉线/重连次数   : {}".format(reconnects))
    if reconnects:
        out.append("    注：ESP32-C3 原生 USB CDC 在芯片复位时会重新枚举 ⇒ 按 RST 必然掉线一次，属正常。")
    if not saw_boot:
        out.append("")
        out.append("⚠️ 未捕获到启动标志行（[BOOT] / [WIFI] 静态配置 / [485] UART1 就绪）⇒")
        out.append("   本次抓的是「运行中途」的日志，不是开机日志。")
        out.append("   **请按一下板上的 RST 键重抓**（本脚本运行时按即可）。")
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
    if not tz_ok and synced:
        out.append("    ⚠️ 读到 {}（非 +0800）。若本次是**复位后**抓取，很可能是**判定时机过早**：".format(
            synced.group(2)))
        out.append("       复位后 RTC 保留时间 ⇒ getLocalTime() 立即为真 ⇒ 在 configTzTime 之前就报\"已同步\"。")
        out.append("       ⚠️ 但**时区本身可能没坏** —— 请对照本文件下面几条 [UDP -> ...] 行的时间戳，")
        out.append("          若它们是**正确的本地时间**，则时区正常，问题只在判定时机。")
    out.append("")
    out.append("[5b] 同步行是否早于「触发 SNTP 对时」行 : {}".format(
        "⚠️ 是 —— 判定过早（异常）" if premature else ("否（正常）" if trig_idx is not None else "无法判定（未抓到对应行）")))
    if premature:
        out.append("    ⇒ 根因与修法见 `04_ai_prompts/02_Codex提示词_NTP同步判定修正.md`（改用 SNTP 回包状态判定）")
    out.append("")
    if actual_is_fallback:
        verdict = ("FAIL —— ⚠️ **走了兜底分支**（该时刻 SNTP 尚未收到任何回包）。"
                   "最常见原因就是上面的「判定过早」：日志在真正对时之前就调用了 logNtpEffectiveSource()。")
    elif actual_ip:
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
    rate = (100.0 * udp_err_n / (udp_ok_n + udp_err_n)) if (udp_ok_n + udp_err_n) else 0.0
    out.append("[8] UDP 发送失败（新观察项）    : 成功 {} 次 / 失败 {} 次（失败率 {:.0f}%）".format(
        udp_ok_n, udp_err_n, rate))
    if udp_err_n:
        out.append("    ⚠️ `[UDP][ERR] 发送失败（beginPacket/endPacket 未返回 1）` 有出现。")
        out.append("       **实测经验（2026-10-05）**：该错误集中在**开机/网络收敛期**，跑稳后归零 ——")
        out.append("         抓取1(刚开机) 45 成功/18 失败；抓取2 60/19；**抓取3(跑稳) 100/0**。")
        out.append("       ⇒ 多为暂态，**不是稳定丢包率**。但若长时间持续出现，才需要排查：")
        out.append("         ① 目标 IP 是否存在于本网段（ARP 能否解析）；② WiFi RSSI；③ lwIP 缓冲。")
        out.append("       ⚠️ 注意：**串口报「发送成功」≠ 数据送达**（UDP 无连接，目标不存在时可能静默丢弃）")
        out.append("          ⇒ 必须用 `06_tests/udp_listen.py` 在电脑侧确认真的收到。")
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
