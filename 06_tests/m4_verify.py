#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M4 一键联测：同时观察「ESP32 串口」与「一个或多个 485 端口的只读监听」

为什么需要它：M4 的判定要同时拿到三个观察点，分三次跑会来回折腾、还会错过时序。
  1. ESP32 串口（COM5，115200）：看 `[M4] 解析 OK` / `超时` / `CRC 失败`
  2. 485 端口（只读，**不发送任何数据**）：总线上到底有没有 ESP32 发出的请求帧

只读监听的意义：只要总线上出现 `01 03 00 00 00 02 C4 0B`，就证明 **MAX13487 在正常发送**；
两个端口都空，就证明它没在驱动总线。

用法：python m4_verify.py <ESP32端口> <秒数> <485端口> [<485端口> ...]
例：  python m4_verify.py COM5 30 COM6 COM7

注意：运行前请关掉 Arduino IDE 的串口监视器（同一端口不能同时被两个程序打开）。
"""
import sys
import threading
import time

import serial

ESP = sys.argv[1]
SECS = float(sys.argv[2])
RS_PORTS = sys.argv[3:]

esp_lines = []
rs_data = {p: [] for p in RS_PORTS}
stop = threading.Event()
FRAME = "01 03 00 00 00 02 C4 0B"


def sniff(port):
    try:
        s = serial.Serial(port, 9600, bytesize=8, parity="N", stopbits=1, timeout=0.1)
    except Exception as exc:
        rs_data[port].append(("ERR", "打不开：{}".format(exc)))
        return
    buf = bytearray()
    last = time.time()
    t0 = time.time()
    while not stop.is_set() and (time.time() - t0) < SECS:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            last = time.time()
        elif buf and (time.time() - last) > 0.05:
            rs_data[port].append((time.strftime("%H:%M:%S"), bytes(buf)))
            buf.clear()
        time.sleep(0.003)
    if buf:
        rs_data[port].append((time.strftime("%H:%M:%S"), bytes(buf)))
    s.close()


def cap_esp():
    try:
        s = serial.Serial(ESP, 115200, timeout=0.2)
    except Exception as exc:
        esp_lines.append("[打不开 {}：{}]".format(ESP, exc))
        return
    buf = b""
    t0 = time.time()
    while not stop.is_set() and (time.time() - t0) < SECS:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            esp_lines.append(line.decode("utf-8", "replace").rstrip("\r"))
    s.close()


def main():
    print("[verify] ESP32={} 485端口={} 时长={:.0f}s".format(ESP, RS_PORTS, SECS), flush=True)
    threads = [threading.Thread(target=cap_esp, daemon=True)]
    for p in RS_PORTS:
        threads.append(threading.Thread(target=sniff, args=(p,), daemon=True))
    for t in threads:
        t.start()
    time.sleep(SECS + 0.5)
    stop.set()
    for t in threads:
        t.join(timeout=2)

    ok = sum(1 for l in esp_lines if "解析 OK" in l)
    to = sum(1 for l in esp_lines if "超时" in l)
    crc = sum(1 for l in esp_lines if "CRC 失败" in l)
    flen = sum(1 for l in esp_lines if "帧长异常" in l)

    print("\n===== ESP32 侧（{}） =====".format(ESP), flush=True)
    print("  行数={}  解析OK={}  超时={}  CRC失败={}  帧长异常={}".format(
        len(esp_lines), ok, to, crc, flen), flush=True)
    for l in esp_lines[:6]:
        print("  | " + l, flush=True)
    if len(esp_lines) > 6:
        print("  | ...（共 {} 行）".format(len(esp_lines)), flush=True)

    print("\n===== 485 总线侧（只读监听） =====", flush=True)
    for p in RS_PORTS:
        batches = rs_data[p]
        total = sum(len(d) for _, d in batches if isinstance(d, bytes))
        hit = any(isinstance(d, bytes) and FRAME.replace(" ", "").lower() in d.hex() for _, d in batches)
        print("  {}  批次={}  总字节={}  收到请求帧={}".format(
            p, len(batches), total, "★ 是" if hit else "否"), flush=True)
        for ts, d in batches[:5]:
            if isinstance(d, bytes):
                print("      [{}] {}  {}".format(ts, len(d), d.hex(" ").upper()), flush=True)
            else:
                print("      {}".format(d), flush=True)

    print("\n===== 判定 =====", flush=True)
    any_bus = any(any(isinstance(d, bytes) and FRAME.replace(" ", "").lower() in d.hex()
                      for _, d in rs_data[p]) for p in RS_PORTS)
    if any_bus:
        print("  ✅ 总线上出现了 ESP32 的请求帧 ⇒ MAX13487 发送正常", flush=True)
    elif to and not any_bus:
        print("  ❌ 总线上没有任何数据，但 ESP32 在持续发送 ⇒ MAX13487 未驱动总线", flush=True)
    else:
        print("  ⚠️ 数据不完整，请人工看上面的明细", flush=True)


if __name__ == "__main__":
    main()
