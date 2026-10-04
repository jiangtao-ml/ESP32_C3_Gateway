# -*- coding: utf-8 -*-
"""独立复核 M5 真机帧与 3 段读取帧的 CRC16/Modbus（低字节先发）。"""


def crc16(data: bytes) -> bytes:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return bytes([crc & 0xFF, (crc >> 8) & 0xFF])


CASES = [
    ("M5 读全部 39 寄存器", "01 04 00 00 00 27", "B0 10"),
    ("E-02 S1 读 0x0000..0x000F (16)", "01 04 00 00 00 10", "F1 C6"),
    ("E-02 S2 读 0x0010..0x001F (16)", "01 04 00 10 00 10", "F0 03"),
    ("E-02 S3 读 0x0020..0x0026 (7)", "01 04 00 20 00 07", "B0 02"),
    ("M5 第1步探针：读 A 相电流 (1 reg)", "01 04 00 09 00 01", "E1 C8"),
    ("M5 第1步探针：读 N 相温度 (1 reg)", "01 04 00 06 00 01", "D1 CB"),
    ("M3/M4 回环帧（对照）", "01 03 00 00 00 02", "C4 0B"),
]

for name, body_hex, crc_hex in CASES:
    body = bytes.fromhex(body_hex.replace(" ", ""))
    calc = crc16(body)
    doc = bytes.fromhex(crc_hex.replace(" ", ""))
    ok = "OK  " if calc == doc else "MISMATCH"
    print("[{}] {:<36} 计算 {} | 文档 {}  =>  {}".format(
        ok, name, calc.hex(" ").upper(), doc.hex(" ").upper(),
        (body + calc).hex(" ").upper()))

# 响应帧长度核算
for name, nreg in [("读全部 39", 39), ("S1 16", 16), ("S2 16", 16), ("S3 7", 7)]:
    n = 1 + 1 + 1 + nreg * 2 + 2
    print("响应帧长 {:<12} {} 字节".format(name, n))
