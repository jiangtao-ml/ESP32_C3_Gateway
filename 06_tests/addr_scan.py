#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STB3-125 从站地址扫描器 / 请求帧生成器（**只读**，绝不发送任何写命令）

为什么需要它：
  固件把从站地址硬编码在 4 条请求帧常量里（config.h:148-156），第一个字节就是 0x01；
  而 RS485_SLAVE_ADDR 宏（config.h:122）**并未被任何帧引用** —— 改那个宏不起作用。
  改地址必须改帧首字节 **并重算 CRC**（Modbus CRC 覆盖整帧，含地址字节），
  只改首字节不重算 => 从站判 CRC 错直接丢弃 => 现象仍是 RX=0，极易误判成"没改成功"。

用法：
  python addr_scan.py COM7             # 扫描地址 1~32（常见范围，约 7 秒）
  python addr_scan.py COM7 --all       # 扫描 1~247（约 50 秒）
  python addr_scan.py COM7 --gen 3     # 为地址 3 生成 4 条请求帧（含正确 CRC），直接贴进 config.h

安全边界：
  只发功能码 0x04（读输入寄存器）。**不含** 0x03/0x05/0x06/0x0F/0x10；
  **不含广播地址 0**（项目铁律：广播一律禁止）。每个地址只发一次，不重试。

依赖：pyserial。若系统 python 报 ModuleNotFoundError，用 PlatformIO 的解释器：
  "C:\\Users\\17352\\.platformio\\penv\\Scripts\\python.exe" addr_scan.py COM7
"""
import sys
import time

try:
    import serial
except ModuleNotFoundError:
    print("[ERR] 缺少 pyserial。请改用：")
    print('  "C:\\Users\\17352\\.platformio\\penv\\Scripts\\python.exe" addr_scan.py <COM>')
    sys.exit(2)

READ_INPUT = 0x04
BAUD = 9600


def crc16_modbus(data: bytes) -> int:
    """CRC16/Modbus：初值 0xFFFF，多项式 0xA001（反向），低字节先发送。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_frame(addr: int, start: int, qty: int, func: int = READ_INPUT) -> bytes:
    body = bytes([addr & 0xFF, func,
                  (start >> 8) & 0xFF, start & 0xFF,
                  (qty >> 8) & 0xFF, qty & 0xFF])
    c = crc16_modbus(body)
    return body + bytes([c & 0xFF, (c >> 8) & 0xFF])


# ---- 自检：与项目里已独立复核过的帧逐字节比对，算法不对就立刻退出 ----
assert build_frame(0x01, 0x0000, 2, 0x03) == bytes(
    [0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B]), "CRC 自检失败（M3 回环帧）"
assert build_frame(0x01, 0x0000, 39) == bytes(
    [0x01, 0x04, 0x00, 0x00, 0x00, 0x27, 0xB0, 0x10]), "CRC 自检失败（一帧读全）"

LAYOUT = [
    ("QI_FULL_FRAME", 0x0000, 39),
    ("QI_SEG1_FRAME", 0x0000, 16),
    ("QI_SEG2_FRAME", 0x0010, 16),
    ("QI_SEG3_FRAME", 0x0020, 7),
]


def gen_frames(addr: int) -> None:
    print("// ---- 地址 %d 的 4 条请求帧（CRC 已重算，可直接替换 config.h:148-156）----" % addr)
    for name, start, qty in LAYOUT:
        f = build_frame(addr, start, qty)
        hexs = ", ".join("0x%02X" % b for b in f)
        print("#define %-14s {%s}" % (name, hexs))
    print("// 同时把 config.h:122 的 RS485_SLAVE_ADDR 改成 %d（仅作文档一致性，代码不引用）" % addr)
    print("// 改完必须重新编译烧录；只改源码不烧录不会生效。")


def scan(port: str, max_addr: int) -> None:
    ser = serial.Serial(port, BAUD, timeout=0.2)
    print("[scan] %s @ %d 8N1，地址 1~%d（只读 0x04，每地址一次）" % (port, BAUD, max_addr))
    hits = []
    t0 = time.time()
    for addr in range(1, max_addr + 1):
        frame = build_frame(addr, 0x0000, 39)
        ser.reset_input_buffer()
        ser.write(frame)
        ser.flush()
        resp = ser.read(120)                 # 83 B @9600 ≈ 87 ms，0.2 s 超时足够
        if not resp:
            continue
        if resp[0] != addr:
            print("  地址 %3d: 收到不属于该地址的字节 %02X（忽略）" % (addr, resp[0]))
            continue
        is_exc = (resp[1] & 0x80) != 0
        if is_exc:
            code = resp[2] if len(resp) > 2 else 0
            print("  地址 %3d: 异常响应 0x%02X 0x%02X（地址对，但读 39 个被拒，"
                  "可能是寄存器数上限）" % (addr, resp[1], code))
        else:
            print("  地址 %3d: 正常响应 %d 字节" % (addr, len(resp)))
        hits.append(addr)
        if len(hits) >= 3:
            print("  已命中 3 个，停止扫描（正常总线应只有 1 个从站）")
            break
    ser.close()
    dt = time.time() - t0
    print("-" * 60)
    if hits:
        print("结果：有回应的地址 = %s（耗时 %.1f s）" % (hits, dt))
        if len(hits) > 1:
            print("⚠️ 多于一个地址有回应：检查是否总线上并了别的设备，或地址冲突")
        print("下一步：用 --gen %d 生成正确 CRC 的帧" % hits[0])
    else:
        print("结果：1~%d 全部无回应（耗时 %.1f s）" % (max_addr, dt))
        print("可能原因（按概率排）：")
        print("  1. 这对线不是 RS485 信号线（或接的不是 STB3-125 那一路）")
        print("  2. A/B 接反了 —— 先对调再扫一次")
        print("  3. 没有 GND 共模参考，信号幅度不足")
        print("  4. 设备波特率不是 9600")
        print("  5. 板子侧发送通路断（W4/模块）—— 用 m5_slave_04.py 单向验证")


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    port = sys.argv[1]
    if "--gen" in sys.argv:
        gen_frames(int(sys.argv[sys.argv.index("--gen") + 1]))
    elif "--all" in sys.argv:
        scan(port, 247)
    else:
        scan(port, 32)


if __name__ == "__main__":
    main()
