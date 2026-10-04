#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
串口抓取器（只读，不发送任何数据）

用途：把 ESP32 的串口监视器输出抓到本地，省去人工复制粘贴。
注意：同一 COM 口不能同时被 Arduino IDE 的串口监视器占用 —— 抓之前请先关掉串口监视器。

用法：python serial_capture.py COM8 [波特率] [抓取秒数]
"""
import sys
import time

import serial

PORT = sys.argv[1]
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

ser = serial.Serial(PORT, BAUD, timeout=0.2)
print("[cap] 打开 {} @ {}，抓取 {:.0f}s".format(PORT, BAUD, SECS), flush=True)

buf = b""
t0 = time.time()
while time.time() - t0 < SECS:
    buf += ser.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        print(line.decode("utf-8", "replace").rstrip("\r"), flush=True)

if buf:
    print(buf.decode("utf-8", "replace").rstrip("\r"), flush=True)
print("[cap] 结束", flush=True)
