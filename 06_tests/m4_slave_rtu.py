#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M4 电脑端 Modbus RTU 从站模拟器（手写帧组装，不依赖第三方 Modbus 库）

定位：替代 Modbus Slave GUI 的备选方案（见 01_firmware/esp32c3_gateway/README.md §M4）。
      好处：帧序列可直接看到、无共享版 10 分钟限时、可复现。

协议参数：从站地址 1 / 9600 / 8N1 / 无校验 / CRC16-Modbus（0xFFFF, 0xA001, 低字节先）
数据：保持寄存器 [0] = 10，[1] = 20（对应固件预期输出 REG: [0]=10 [1]=20）

用法：python m4_slave_rtu.py COM7 [波特率]

容错设计（v1.1）：
  - 不要求一次收满 8 字节 —— USB 转 485 按 USB 帧批量交付，帧内会出现数毫秒真空隙。
  - 每收到一批字节就在缓冲里「扫描有效帧」：以从站地址开头 + 8 字节 + CRC 通过 才受理，
    因此碎帧重拼、噪声混入、错位起帧都能自愈。
  - 空闲超过 IDLE_END 仍无法拼出合法帧 → 按噪声报出并清空，不静默丢失证据。

注意：同一 COM 口不能同时被 Modbus Slave 和本脚本占用，运行前请断开 Modbus Slave。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
SLAVE_ID = 1
REGS = {0: 10, 1: 20}

REQ_LEN_03 = 8          # 0x03 请求固定 8 字节
IDLE_END = 0.05         # 兜底：静默 50ms 仍拼不出合法帧 → 判为噪声并清空
READ_TIMEOUT = 0.005


def crc16(data: bytes) -> bytes:
    """CRC16/Modbus，返回线上字节序（低字节在前）。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def stamp() -> str:
    return time.strftime("%H:%M:%S")


def handle(ser: serial.Serial, frame: bytes) -> None:
    print("[{}] [slave] RX {}".format(stamp(), frame.hex(" ").upper()), flush=True)
    body, got = frame[:-2], frame[-2:]
    if crc16(body) != got:
        print("[{}] [slave] CRC 校验失败（收到 {}，应为 {}），丢弃".format(
            stamp(), got.hex(" ").upper(), crc16(body).hex(" ").upper()), flush=True)
        return

    addr, func = body[0], body[1]
    if addr != SLAVE_ID:
        print("[{}] [slave] 地址不匹配（{} != {}），不响应".format(stamp(), addr, SLAVE_ID), flush=True)
        return

    if func == 0x03:
        start = (body[2] << 8) | body[3]
        qty = (body[4] << 8) | body[5]
        vals = [REGS.get(start + i, 0) for i in range(qty)]
        payload = bytes([addr, func, qty * 2]) + b"".join(v.to_bytes(2, "big") for v in vals)
        resp = payload + crc16(payload)
        ser.write(resp)
        ser.flush()
        print("[{}] [slave] TX {}  (reg[{}..{}]={})".format(
            stamp(), resp.hex(" ").upper(), start, start + qty - 1, vals), flush=True)
    else:
        exc = bytes([addr, func | 0x80, 0x01])
        resp = exc + crc16(exc)
        ser.write(resp)
        ser.flush()
        print("[{}] [slave] TX {}  (不支持的功能码 0x{:02X}，回异常码 0x01)".format(
            stamp(), resp.hex(" ").upper(), func), flush=True)


def extract(ser: serial.Serial, buf: bytearray) -> bytearray:
    """在缓冲里扫描合法帧并处理，返回剩余未消化的字节。"""
    n = len(buf)
    i = 0
    while i < n:
        if buf[i] != SLAVE_ID:
            i += 1
            continue
        if n - i < REQ_LEN_03:
            break                       # 可能是被切开的帧头，留着等后续字节
        cand = bytes(buf[i:i + REQ_LEN_03])
        if crc16(cand[:-2]) == cand[-2:]:
            handle(ser, cand)
            i += REQ_LEN_03
        else:
            i += 1                      # 错位起帧，右移一字节重试
    if i > 0:
        noise = bytes(buf[:i])
        print("[{}] [slave] 噪声 {} 字节：{}".format(
            stamp(), len(noise), noise.hex(" ").upper()), flush=True)
        del buf[:i]
    return buf


def main() -> None:
    ser = serial.Serial(PORT, BAUD, bytesize=8, parity="N", stopbits=1, timeout=READ_TIMEOUT)
    print("[{}] [slave] 已打开 {} {} 8N1 | SlaveID={} | 寄存器 {}".format(
        stamp(), PORT, BAUD, SLAVE_ID, REGS), flush=True)
    buf = bytearray()
    last = time.time()
    while True:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            last = time.time()
            extract(ser, buf)
        elif buf and (time.time() - last) > IDLE_END:
            print("[{}] [slave] 残帧（{} 字节，未通过 CRC 扫描）：{}".format(
                stamp(), len(buf), buf.hex(" ").upper()), flush=True)
            buf.clear()
        time.sleep(0.001)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[slave] 已停止", flush=True)
    except serial.SerialException as exc:
        print("[slave] 串口异常：{}".format(exc), flush=True)
        sys.exit(1)
