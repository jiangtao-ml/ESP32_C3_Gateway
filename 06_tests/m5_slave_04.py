#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M5 台面预演 · 电脑端 0x04 真机帧从站模拟器（手写帧组装，不依赖第三方 Modbus 库）

定位：在**不接真机**的前提下，先把 M5 的固件改动（功能码 0x03→0x04、默认 3 段读取、
      RS485_CHAR_TIMEOUT_MS 25→4~8）整条链路验证一遍，从而把「代码问题」与「真机问题」分开。

⚠️ 本脚本里的 39 个寄存器是**合成值**，只保证与 `02_docs/06_QI协议说明.md` §5.3 的换算表自洽、
   便于人眼核对；**不来自任何真机**，不得当作真机数据。真机数据必须按
   `06_tests/M5_真机联调操作卡.md` §4 的 Q-08 判据另行核对。

协议：从站地址 1 / 9600 / 8N1 / 无校验 / CRC16-Modbus（0xFFFF, 0xA001, 低字节先）

用法：
  python m5_slave_04.py COM7               # 接受任意 qty（模拟「真机接受一帧读全」）
  python m5_slave_04.py COM7 --limit16     # qty > 16 → 0x84/0x02（模拟「原文上限 16 属实」）
  python m5_slave_04.py COM7 --short N     # 每个请求最多只回 N 个寄存器（**短帧**，见下）

--short N（短响应对照从站）用于验证 P0-1「帧长必须与本段请求的寄存器数一致」：
  它回的帧**自洽**（byte count 与实收长度一致、CRC 正确、地址功能码都对），只与"请求"不符。
  例：一帧读全要 39 个、--short 30 只回 30 个 ⇒ 固件收到 byteCount=60 / rxLen=65，而本段期望 83。
  · 判据未生效（P0-1 之前）：该帧被判成功 → 发数据行，且 raw 尾部 18 字节保留**上一轮**残留 ⇒ 静默掺假
  · 判据生效（P0-1 之后）：`[M4] 帧长与请求不符 期望 83 收到 65（byte count=60）` + 该轮改发 err 行
  ⚠️ 合成寄存器**每轮不变**，长稳观察时看不出"尾部冻结"；要判"是否掺假"看固件串口那行日志即可。
  ⚠️ --short 会让连续失败累计到 3 轮且本会话从未成功时触发**自动降级 3 段**（批次 2 既有逻辑）：
     降级后每段只要 16/16/7 个，30 ≥ 16 ⇒ 又全部成功。这是**预期行为**，不是 bug。

与固件两种编译分支的对应（见操作卡 §5 E-02 决策树）：
  · 默认模式   ← 固件 QI_READ_SPLIT_3SEG = 0，一次发 `01 04 00 00 00 27 B0 10`
  · --limit16  ← 固件 QI_READ_SPLIT_3SEG = 1，分 3 段 00 10 / 10 10 / 20 07
两种模式都应得到「解析 OK」，可据此确认固件两条分支都工作正常，再上真机。

安全：台面阶段 485 另一端是 USB 转 485（与电脑同接地）⇒ **USB 供电即可，不需要充电宝**。
注意：同一 COM 口不能被两个程序同时占用（运行前关掉 Arduino IDE 串口监视器）。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
BAUD = 9600
SLAVE_ID = 1
LIMIT16 = "--limit16" in sys.argv
SHORT = int(sys.argv[sys.argv.index("--short") + 1]) if "--short" in sys.argv else None

REQ_LEN = 8          # 0x04 请求固定 8 字节：addr func startH startL qtyH qtyL crcL crcH
MAX_QTY = 39         # 30001~30039
IDLE_END = 0.05
READ_TIMEOUT = 0.005

# 39 个输入寄存器（30001~30039）合成值；换算口径见 02_docs/06_QI协议说明.md §5.3
_REGS = [0] * MAX_QTY
_REGS[0] = 0x00F0     # 开关状态：无锁 + 合闸
_REGS[4] = 500        # 频率    raw/10     -> 50.00 Hz
_REGS[5] = 123        # 漏电    raw/1000   -> 0.123 A
_REGS[6] = 65         # N 相温度 raw-40    -> 25 °C
_REGS[7] = 68         # A 相温度           -> 28 °C
_REGS[8] = 230        # A 相电压 1:1       -> 230 V
_REGS[9] = 523        # A 相电流 raw/100   -> 5.23 A
_REGS[10] = 98        # A 相功率因数 ×0.01 -> 0.98
_REGS[11] = 1178      # A 相有功功率       -> 1178 W
_REGS[14] = 1         # A 相总电量 H
_REGS[15] = 2345      # A 相总电量 L       -> 65.536 + 2345 = 2410.536 度
_REGS[16] = 66        # B 相温度           -> 26 °C
_REGS[17] = 231       # B 相电压
_REGS[18] = 510       # B 相电流           -> 5.10 A
_REGS[19] = 97        # B 相功率因数       -> 0.97
_REGS[20] = 1142      # B 相有功功率
_REGS[23] = 0         # B 相总电量 H
_REGS[24] = 1876      # B 相总电量 L
_REGS[25] = 64        # C 相温度           -> 24 °C
_REGS[26] = 229       # C 相电压
_REGS[27] = 498       # C 相电流           -> 4.98 A
_REGS[28] = 99        # C 相功率因数       -> 0.99
_REGS[29] = 1129      # C 相有功功率
_REGS[32] = 0         # C 相总电量 H
_REGS[33] = 1544      # C 相总电量 L
_REGS[37] = 1         # 总电量 H
_REGS[38] = 5765      # 总电量 L           -> 65.536 + 5765 = 5830.536 度


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


def exc_resp(addr: int, func: int, code: int) -> bytes:
    body = bytes([addr, func | 0x80, code])
    return body + crc16(body)


def handle(ser: serial.Serial, frame: bytes) -> None:
    print("[{}] [slave] RX  {}".format(stamp(), frame.hex(" ").upper()), flush=True)
    body, got = frame[:-2], frame[-2:]
    if crc16(body) != got:
        print("[{}] [slave] CRC 校验失败（收到 {}，应为 {}），丢弃".format(
            stamp(), got.hex(" ").upper(), crc16(body).hex(" ").upper()), flush=True)
        return

    addr, func = body[0], body[1]
    if addr != SLAVE_ID:
        print("[{}] [slave] 地址不匹配（{} != {}），不响应".format(stamp(), addr, SLAVE_ID), flush=True)
        return

    if func != 0x04:
        resp = exc_resp(addr, func, 0x01)
        ser.write(resp)
        ser.flush()
        tag = "!! M5 禁止再把 0x03 回环帧带来轮询，" if func == 0x03 else ""
        print("[{}] [slave] TX  {}  （{}功能码 0x{:02X} 不支持，回异常码 0x01）".format(
            stamp(), resp.hex(" ").upper(), tag, func), flush=True)
        return

    start = (body[2] << 8) | body[3]
    qty = (body[4] << 8) | body[5]

    if qty == 0 or start + qty > MAX_QTY:
        resp = exc_resp(addr, func, 0x02)
        ser.write(resp)
        ser.flush()
        print("[{}] [slave] TX  {}  （地址/数量越界 start={} qty={} → 异常码 0x02）".format(
            stamp(), resp.hex(" ").upper(), start, qty), flush=True)
        return

    if LIMIT16 and qty > 16:
        resp = exc_resp(addr, func, 0x02)
        ser.write(resp)
        ser.flush()
        print("[{}] [slave] TX  {}  （--limit16：qty={} > 16 → 异常码 0x02，模拟 E-02 命中）".format(
            stamp(), resp.hex(" ").upper(), qty), flush=True)
        return

    qty_ret = qty if SHORT is None else min(qty, SHORT)   # --short N：故意少回，帧仍自洽（P0-1 判据的靶子）
    vals = _REGS[start:start + qty_ret]
    payload = bytes([addr, func, qty_ret * 2]) + b"".join(v.to_bytes(2, "big") for v in vals)
    resp = payload + crc16(payload)
    ser.write(resp)
    ser.flush()
    tag = "" if qty_ret == qty else "  ★--short：要 {} 只回 {}（byte count={}）".format(qty, qty_ret, qty_ret * 2)
    print("[{}] [slave] TX  {}  （reg[{}..{}] qty={} → 共 {} 字节，期望响应长 {}{}）".format(
        stamp(), resp.hex(" ").upper(), start, start + qty_ret - 1, qty, len(resp), 5 + qty * 2, tag), flush=True)


def extract(ser: serial.Serial, buf: bytearray) -> None:
    """在缓冲里扫描合法帧并处理（USB 转 485 会按 USB 帧批量交付，故不要求一次收满）。"""
    i = 0
    n = len(buf)
    while i < n:
        if buf[i] != SLAVE_ID:
            i += 1
            continue
        if n - i < REQ_LEN:
            break                       # 可能是被切开的帧头，留着等后续字节
        cand = bytes(buf[i:i + REQ_LEN])
        if crc16(cand[:-2]) == cand[-2:]:
            handle(ser, cand)
            i += REQ_LEN
        else:
            i += 1                      # 错位起帧，右移一字节重试
    if i > 0:
        noise = bytes(buf[:i])
        print("[{}] [slave] 噪声 {} 字节：{}".format(
            stamp(), len(noise), noise.hex(" ").upper()), flush=True)
        del buf[:i]


def main() -> None:
    ser = serial.Serial(PORT, BAUD, bytesize=8, parity="N", stopbits=1, timeout=READ_TIMEOUT)
    print("[{}] [slave] 已打开 {} {} 8N1 | SlaveID={} | 模式 = {}".format(
        stamp(), PORT, BAUD, SLAVE_ID,
        ("limit16（qty>16 → 0x84/0x02）" if LIMIT16 else "接受任意 qty（含一帧读全 39）")
        + ("" if SHORT is None else " + **--short {}：每请求最多回 {} 个（短帧，P0-1 靶子）**".format(SHORT, SHORT))),
        flush=True)
    print("[{}] [slave] 合成寄存器就绪：reg0=0x{:04X}(合闸) reg4={}(50.00Hz) reg8={}(230V) "
          "reg9={}(5.23A) reg37/38={}/{}".format(
              stamp(), _REGS[0], _REGS[4], _REGS[8], _REGS[9], _REGS[37], _REGS[38]), flush=True)
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
