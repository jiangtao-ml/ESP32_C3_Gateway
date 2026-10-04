#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
两端口 485 互测（**唯一有效的回环方法**）

背景：单端口短接 A/B 做回环是无效的 —— 两线半双工 485 短接 A/B 后差分 V_AB ≡ 0，
      接收器判据是 |V_AB| > 200mV，0V 落在判定窗口正中间，输出不定（标准 fail-safe 器件
      输出不确定，full-fail-safe 器件恒输出高）→ 健康模块同样不回显。
      依据：MaxLinear XR33156 AN、Renesas KB「fail-safe short」、Industrial Monitor Direct。

正确做法：两个物理 485 端口背靠背，A↔A、B↔B（GND 共地，同插一台电脑时已天然共地）。

接线：模块A 的 A — 模块B 的 A ；模块A 的 B — 模块B 的 B
用法：python two_port_loopback.py COMx COMy
"""
import sys
import time

import serial

A = sys.argv[1]
B = sys.argv[2]
PAT = b"abc"
TRIES = 5


def openp(port):
    return serial.Serial(port, 9600, bytesize=8, parity="N", stopbits=1, timeout=0.4)


def one_way(tx, rx, label):
    got_all = []
    ok = 0
    for i in range(1, TRIES + 1):
        rx.reset_input_buffer()
        tx.write(PAT)
        tx.flush()
        time.sleep(0.25)
        got = rx.read(64)
        got_all.append(got)
        if got == PAT:
            ok += 1
        print("  [{}] 第 {} 次  收 {}   {}".format(
            label, i, got.hex(" ").upper() or "(空)", "OK" if got == PAT else ""), flush=True)
    print("  [{}] 结果 {}/{}".format(label, ok, TRIES), flush=True)
    return ok


def main():
    pa = openp(A)
    pb = openp(B)
    time.sleep(0.2)
    print("[two] 已打开 {} 与 {}  @9600 8N1".format(A, B), flush=True)
    print("[two] 方向 1：{} 发送 -> {} 接收".format(A, B), flush=True)
    n1 = one_way(pa, pb, "{}->{}".format(A, B))
    print("[two] 方向 2：{} 发送 -> {} 接收".format(B, A), flush=True)
    n2 = one_way(pb, pa, "{}->{}".format(B, A))
    pa.close()
    pb.close()

    print("[two] ===== 汇总 =====", flush=True)
    print("[two] {} -> {} : {}/{}".format(A, B, n1, TRIES), flush=True)
    print("[two] {} -> {} : {}/{}".format(B, A, n2, TRIES), flush=True)
    if n1 and n2:
        print("[two] ✅ 双向互通：两个模块的收发都正常", flush=True)
    elif n1 or n2:
        print("[two] ⚠️ 只有一个方向通：对调那两根线再看（可能是 A/B 接反），或其中一个方向坏", flush=True)
    else:
        print("[two] ❌ 双向都不通：检查 A↔A / B↔B 是否真的接通、两个模块是否都已插好", flush=True)


if __name__ == "__main__":
    main()
