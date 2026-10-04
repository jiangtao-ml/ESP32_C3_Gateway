#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
485 总线注入探针（仅发送）—— 用来「反向」验证接收通路

用途：从电脑侧 USB 转 485 往总线灌字节，看 ESP32 串口是否打印 RX>0。
      该通路的走线为：
          PC → USB转485(TX) → A/B 总线 → MAX13487(接收器 RO) → 分压 → GPIO20 → ESP32
      **与「ESP32 GPIO21 → 模块 DI」那根线完全无关**，
      因此能把「去程（ESP32→PC）」与「回程（PC→ESP32）」两个方向彻底分开。

发的是无害的单字节 0x55（不是任何 Modbus 命令，总线上也没有真机，仅 MAX13487 + USB转485）。

用法：python bus_probe_tx.py COM7 [字节HEX] [间隔毫秒] [总秒数]
默认： COM7 / 0x55 / 250ms / 45s
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BYTE = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x55
GAP_MS = int(sys.argv[3]) if len(sys.argv) > 3 else 250
SECS = float(sys.argv[4]) if len(sys.argv) > 4 else 45.0

ser = serial.Serial(PORT, 9600, bytesize=8, parity="N", stopbits=1, timeout=0.05)
print("[probe] 打开 {} @9600，每 {}ms 注入 1 字节 0x{:02X}，持续 {:.0f}s".format(
    PORT, GAP_MS, BYTE, SECS), flush=True)

t0 = time.time()
n = 0
try:
    while time.time() - t0 < SECS:
        ser.write(bytes([BYTE]))
        ser.flush()
        n += 1
        time.sleep(GAP_MS / 1000.0)
except KeyboardInterrupt:
    pass

print("[probe] 结束，共注入 {} 字节".format(n), flush=True)
ser.close()
