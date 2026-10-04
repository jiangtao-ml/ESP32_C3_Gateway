#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
485 总线只读监听器（**不发送任何东西**）

用途：把两个 USB 转 485 模块同时挂在同一条总线上，一人一个端口各自只读，
      用来交叉验证「到底有没有数据上总线」以及「哪个模块的线接对了行」。
      只读不写，因此不会与从站脚本争抢总线（真机上也不会误发控制命令）。

用法：python bus_sniff.py COMx [监听秒数]
"""
import sys
import time

import serial

PORT = sys.argv[1]
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

ser = serial.Serial(PORT, 9600, bytesize=8, parity="N", stopbits=1, timeout=0.1)
print("[sniff] {} 只读监听 {:.0f}s @9600 8N1".format(PORT, SECS), flush=True)

buf = bytearray()
last = time.time()
t0 = time.time()
n = 0
while time.time() - t0 < SECS:
    chunk = ser.read(256)
    if chunk:
        buf += chunk
        last = time.time()
    elif buf and (time.time() - last) > 0.05:
        n += 1
        print("[sniff] {} 收到 {} 字节：{}".format(
            time.strftime("%H:%M:%S"), len(buf), bytes(buf).hex(" ").upper()), flush=True)
        buf.clear()
    time.sleep(0.005)

print("[sniff] 结束（共 {} 批数据）".format(n), flush=True)
