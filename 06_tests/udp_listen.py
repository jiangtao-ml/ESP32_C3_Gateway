#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC 侧 UDP 接收监听（验证 ESP32 数据是否真的到达电脑）
=====================================================

用途：在电脑上监听 UDP 8888，确认 ESP32 的上行数据**真的到了电脑**。
      —— 这一点必须单独验证，因为**串口打印「发送成功」不等于数据送达**：
      UDP 是无连接的，目标 IP 不存在时 lwIP 仍可能返回成功（静默丢弃）。

用法：
    python udp_listen.py            # 监听 0.0.0.0:8888，直到 Ctrl+C
    python udp_listen.py 8888 120   # 监听 8888 端口 120 秒

判读：
    - 每秒 1 行、内容形如 `2026-10-05 14:40:01 | HEARTBEAT` ⇒ **上行链路完好**
    - 一条都没有 ⇒ 数据没到电脑。此时按顺序查：
        ① ESP32 的 `config.h` 里 `UDP_TARGET_IP` 是否等于**本机当前 WLAN IPv4**；
        ② ESP32 串口是否出现 `[UDP][ERR] 发送失败`；
        ③ 电脑防火墙是否拦了 UDP 8888（首次监听时 Windows 常会弹窗询问）。
"""

import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8888
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0     # 0 = 一直听

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))
s.settimeout(1.0)

print("[udp_listen] 正在监听 0.0.0.0:{} ……{}".format(
    PORT, "（Ctrl+C 停止）" if SECONDS == 0 else "（{:.0f} 秒）".format(SECONDS)), flush=True)

n = 0
senders = {}
t0 = time.time()
try:
    while SECONDS == 0 or (time.time() - t0) < SECONDS:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        n += 1
        senders[addr[0]] = senders.get(addr[0], 0) + 1
        text = data.decode("utf-8", errors="replace").rstrip("\r\n")
        print("[{:>4}] {} <- {}:{}".format(n, text, addr[0], addr[1]), flush=True)
except KeyboardInterrupt:
    pass
finally:
    s.close()

print("\n[udp_listen] 共收到 {} 个包，来源：{}".format(
    n, senders if senders else "（无）"), flush=True)
sys.exit(0 if n else 2)
