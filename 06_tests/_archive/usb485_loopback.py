#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
USB 转 485 模块「A/B 自环」测试 —— 本项目规定的「换硬件后第一个动作」

做法：把模块自己的 A 与 B 用一根杜邦线短接，从本口发送 abc，看能否原样收回。
      该测试**不依赖 ESP32、不依赖 MAX13487**，用于单独判定
      「USB 转 485 模块自身收发是否完好」，从而把故障域一刀切开。

前置：把该模块的 A/B 从面包板上拔下来，只短接它自己的 A—B。

用法：python usb485_loopback.py COM7 [测试次数]
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 5
PATTERN = b"abc"

ser = serial.Serial(PORT, 9600, bytesize=8, parity="N", stopbits=1, timeout=0.3)
print("[loop] 打开 {} @9600 8N1".format(PORT), flush=True)
time.sleep(0.2)
ser.reset_input_buffer()

ok = 0
for i in range(1, N + 1):
    ser.reset_input_buffer()
    ser.write(PATTERN)
    ser.flush()
    time.sleep(0.15)
    got = ser.read(64)
    hit = got == PATTERN
    ok += 1 if hit else 0
    print("[loop] 第 {} 次  发送 {}  收到 {}   {}".format(
        i, PATTERN.decode(), got.hex(" ").upper() or "(空)",
        "OK" if hit else "不匹配"), flush=True)

print("[loop] 结果：{}/{} 次回显正确 → {}".format(
    ok, N,
    "模块自身收发正常" if ok == N else "模块未能自环：疑模块硬件 / 驱动 / 短接线问题"), flush=True)
ser.close()
