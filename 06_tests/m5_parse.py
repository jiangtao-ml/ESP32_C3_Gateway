#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M5 数据核对：把 UDP 上行的一行文本解析成 39 个字段（表驱动，含工程量换算与质量判定）
====================================================================================

字段字典来源（**唯一事实来源**）：
  - `05_hardware/STB3-125_Database_Fields.md`（= 用户提供的《数据库说明.docx》，
    定义字段名 / 类型 / 量程 / 精度 / 电量公式）
  - `02_docs/06_QI协议说明.md` §5.3（寄存器地址与换算）
  - `06_tests/M5_真机联调操作卡.md` §9（换算速查）

关键结论：**39 个输入寄存器（30001~30039）与字段表里的 39 个字段一一对应、顺序一致**。
本脚本的 FIELDS 表就是这条映射的机器可读版本。

⚠️ 不发明协议：表里没有的一律标 `?`；带「推测」字样的解码需人复核后再定案。

用法：
    python m5_parse.py <抓包日志文件> [行号]        # 行号默认 1
    python m5_parse.py --line "<时间戳> | <HEX>"
    python m5_parse.py --selftest                  # 用合成值验证换算表
    python m5_parse.py --dict                      # 打印字段字典（39 行对照表）
"""
import re
import sys

REG_COUNT = 39

# 分闸原因编码（字段表原文）：F=无 1=过流 2=漏电 3=过温 4=过载 5=过压 6=欠压 7=远程 8=模组 9=失压 B=限电
TRIP = {"F": "无", "1": "过流", "2": "漏电", "3": "过温", "4": "过载",
        "5": "过压", "6": "欠压", "7": "远程", "8": "模组", "9": "失压", "B": "限电"}

# 字段分级：ok = 有明确判据；warn = 有明确判据但需业务判读；?
KIND = {}


def _kv(idx, name, cn, unit, lo, hi, dec):
    KIND[idx] = dict(name=name, cn=cn, unit=unit, lo=lo, hi=hi, dec=dec)


# idx: (DB字段名, 中文, 单位, 量程下限, 量程上限, 小数位)
_kv(0, "openState", "开关分合闸状态", "", None, None, 0)
_kv(1, "record1", "分闸记录1（前第9-12次）", "", None, None, 0)
_kv(2, "record2", "分闸记录2（前第5-8次）", "", None, None, 0)
_kv(3, "record3", "分闸记录3（前第1-4次）", "", None, None, 0)
_kv(4, "frequent", "频率 F", "Hz", 0, 100, 1)
_kv(5, "leakage_current", "漏电电流 L", "A", 0, 1, 3)
_kv(6, "N_temp", "板载温度", "℃", -40, 160, 0)
_kv(7, "A_temp", "A 相接线柱温度", "℃", -40, 160, 0)
_kv(8, "A_voltage", "A 相电压", "V", 0, 600, 0)
_kv(9, "A_current", "A 相电流", "A", 0, 655.35, 2)
_kv(10, "A_Pf", "A 相功率因数", "", 0, 1, 2)
_kv(11, "A_power", "A 相有功功率 P", "W", 0, 65535, 0)
_kv(12, "A_Q", "A 相无功功率 Q", "VAR", 0, 65535, 0)
_kv(13, "A_S", "A 相视在功率 S", "VA", 0, 65535, 0)
_kv(14, "A_H", "A 相有功电量 高字 H", "", 0, 999999999, 0)
_kv(15, "A_L", "A 相有功电量 低字 L", "度", 0, 65535, 3)
_kv(16, "B_temp", "B 相接线柱温度", "℃", -40, 160, 0)
_kv(17, "B_voltage", "B 相电压", "V", 0, 600, 0)
_kv(18, "B_current", "B 相电流", "A", 0, 655.35, 2)
_kv(19, "B_Pf", "B 相功率因数", "", 0, 1, 2)
_kv(20, "B_power", "B 相有功功率 P", "W", 0, 65535, 0)
_kv(21, "B_Q", "B 相无功功率 Q", "VAR", 0, 65535, 0)
_kv(22, "B_S", "B 相视在功率 S", "VA", 0, 65535, 0)
_kv(23, "B_H", "B 相有功电量 高字 H", "", 0, 999999999, 0)
_kv(24, "B_L", "B 相有功电量 低字 L", "度", 0, 65535, 3)
_kv(25, "C_temp", "C 相接线柱温度", "℃", -40, 160, 0)
_kv(26, "C_voltage", "C 相电压", "V", 0, 600, 0)
_kv(27, "C_current", "C 相电流", "A", 0, 655.35, 2)
_kv(28, "C_Pf", "C 相功率因数", "", 0, 1, 2)
_kv(29, "C_power", "C 相有功功率 P", "W", 0, 65535, 0)
_kv(30, "C_Q", "C 相无功功率 Q", "VAR", 0, 65535, 0)
_kv(31, "C_S", "C 相视在功率 S", "VA", 0, 65535, 0)
_kv(32, "C_H", "C 相有功电量 高字 H", "", 0, 999999999, 0)
_kv(33, "C_L", "C 相有功电量 低字 L", "度", 0, 65535, 3)
_kv(34, "power", "合相有功功率 P", "W", 0, 65535, 0)
_kv(35, "Q", "合相无功功率 Q", "VAR", 0, 65535, 0)
_kv(36, "S", "合相视在功率 S", "VA", 0, 65535, 0)
_kv(37, "H", "合相有功电量 高字 H", "", 0, 999999999, 0)
_kv(38, "L", "合相有功电量 低字 L", "度", 0, 65535, 3)

ENERGY = {15: (14, "A 相总电量"), 24: (23, "B 相总电量"),
          33: (32, "C 相总电量"), 38: (37, "合相总电量")}


def fmt_status(raw: int) -> str:
    hi, lo = (raw >> 8) & 0xFF, raw & 0xFF
    lock = {0x01: "本地锁止", 0x00: "无锁"}.get(hi, "锁位未知(0x%02X)" % hi)
    sw = {0x0F: "分闸", 0xF0: "合闸"}.get(lo, "分合位未知(0x%02X)" % lo)
    return "{} / {}".format(lock, sw)


def fmt_trip(raw: int) -> str:
    """分闸记录：字段表定义为 varchar（4 条记录/字段）。此处按半字节解码（**推测，待复核**）。"""
    nib = "%04X" % raw
    out = []
    for ch in nib:
        out.append(TRIP.get(ch, "空位" if ch == "0" else "?0x%s" % ch))
    return "{}  [半字节解码，推测]".format(" / ".join(out))


def value_of(idx: int, regs):
    """返回 (显示值, 是否无效, 说明)。"""
    raw = regs[idx]
    if idx == 0:
        return fmt_status(raw), False, ""
    if idx in (1, 2, 3):
        return fmt_trip(raw), False, ""
    if idx in ENERGY:
        hi_i, label = ENERGY[idx]
        v = (regs[hi_i] * 65536) / 1000.0 + raw
        return "{:.3f} 度   ← {}".format(v, label), False, ""
    if idx in (14, 23, 32, 37):
        return "{}（高字，与下一行合成电量）".format(raw), False, ""
    meta = KIND[idx]
    dec = meta["dec"]
    v = raw / 100.0 if idx in (9, 18, 27) else \
        raw / 1000.0 if idx == 5 else \
        raw / 10.0 if idx == 4 else \
        raw - 40 if idx in (6, 7, 16, 25) else \
        raw * 0.01 if idx in (10, 19, 28) else float(raw)
    txt = ("{:.%df}" % dec).format(v)
    if meta["unit"]:
        txt += " " + meta["unit"]
    # 语义层校验：量程 + 无效值伪装
    bad, note = False, ""
    if idx in (7, 16, 25) and v == -40:
        bad, note = True, "= 量程下限，疑似「无数据」伪装（该相未装接线柱温度传感器）"
    if not bad and (v < meta["lo"] or v > meta["hi"]):
        bad, note = True, "越界（量程 {}~{}）".format(meta["lo"], meta["hi"])
    return txt, bad, note


def parse_hex(text: str):
    if "|" not in text:
        raise ValueError("没有 ' | ' 分隔符")
    ts, payload = text.split("|", 1)
    hexs = re.findall(r"\b[0-9A-Fa-f]{2}\b", payload)
    if len(hexs) != REG_COUNT * 2:
        raise ValueError("HEX 字节数 {} != {}".format(len(hexs), REG_COUNT * 2))
    return ts.strip(), bytes(int(h, 16) for h in hexs)


def dump(ts: str, data: bytes) -> None:
    regs = [(data[i * 2] << 8) | data[i * 2 + 1] for i in range(REG_COUNT)]
    print("时间戳：{}".format(ts))
    print("数据域：{} 字节 = {} 寄存器（S1 0-15 / S2 16-31 / S3 32-38）".format(len(data), REG_COUNT))
    print()
    print("{:>3}  {:>6}  {:<14} {:<22} {}".format("#", "HEX", "DB 字段", "中文", "值 / 判读"))
    print("{}  {}  {}  {}  {}".format("-" * 3, "-" * 6, "-" * 14, "-" * 22, "-" * 34))
    bad_n = 0
    for i, raw in enumerate(regs):
        meta = KIND[i]
        txt, bad, note = value_of(i, regs)
        bad_n += 1 if bad else 0
        line = "{:>3}  0x{:04X}  {:<14} {:<22} {}".format(i, raw, meta["name"], meta["cn"], txt)
        if bad:
            line += "   <<< 无效"
        print(line)
        if bad:
            print("        ^^ 无效原因：{}".format(note))
    print()
    print("字段级判定：{} 项无效 / 共 {} 项".format(bad_n, REG_COUNT))
    print("⚠️ 主判据仍是「原始寄存器值逐项相等」（Q-08）；本表换算与判定供人眼复核。")


def print_dict() -> None:
    print("39 寄存器 ↔ DB 字段 对照表（顺序一一对应）")
    print("{:>3}  {:<14} {:<22} {:<8} {}".format("#", "DB 字段", "中文", "单位", "量程"))
    for i in range(REG_COUNT):
        m = KIND[i]
        rng = "{}~{}".format(m["lo"], m["hi"]) if m["lo"] is not None else "—"
        print("{:>3}  {:<14} {:<22} {:<8} {}".format(i, m["name"], m["cn"], m["unit"] or "—", rng))


def selftest() -> int:
    cases = [
        (0, 0x00F0, "无锁 / 合闸"), (4, 500, "50.0 Hz"), (5, 123, "0.123 A"),
        (6, 65, "25 ℃"), (8, 222, "222 V"), (9, 523, "5.23 A"),
        (10, 98, "0.98"), (11, 1178, "1178 W"), (27, 498, "4.98 A"),
    ]
    bad = 0
    regs = [0] * REG_COUNT
    for idx, raw, want in cases:
        regs[idx] = raw
        got, _, _ = value_of(idx, regs)
        ok = got == want
        bad += 0 if ok else 1
        print("[{}] reg{:<2} raw={:<6} -> {:<10} (期望 {})".format("OK " if ok else "BAD", idx, raw, got, want))
    regs[7] = 0
    _, isbad, _ = value_of(7, regs)
    print("[{}] reg7 raw=0 -> 判为无效值（期望 无效）".format("OK " if isbad else "BAD"))
    bad += 0 if isbad else 1
    regs[14], regs[15] = 4, 58831
    got, _, _ = value_of(15, regs)
    print("[{}] A 相电量 H=4 L=58831 -> {} (期望 59093.144 度)".format(
        "OK " if got == "59093.144 度" else "BAD", got))
    print("总体：", "ALL_OK" if bad == 0 else "{} 项不符".format(bad))
    return 0 if bad == 0 else 1


def main() -> int:
    a = sys.argv[1:]
    if a and a[0] == "--selftest":
        return selftest()
    if a and a[0] == "--dict":
        print_dict()
        return 0
    if len(a) >= 2 and a[0] == "--line":
        ts, data = parse_hex(a[1])
        dump(ts, data)
        return 0
    if not a:
        print(__doc__)
        return 1
    want = int(a[1]) if len(a) > 1 else 1
    n = 0
    with open(a[0], "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "|" not in line or "FAIL:" in line or "udp_listen" in line:
                continue
            try:
                ts, data = parse_hex(line)
            except ValueError:
                continue
            n += 1
            if n == want:
                dump(ts, data)
                return 0
    print("没找到第 {} 条数据行（共匹配 {} 条）".format(want, n))
    return 1


if __name__ == "__main__":
    sys.exit(main())
