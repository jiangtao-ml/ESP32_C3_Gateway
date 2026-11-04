#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M5 数据落库原型：解析 → 四层校验（L0~L3）→ 质量打标 → 幂等入库（SQLite）
================================================================================

定位：把 `02_docs/16_数据落地与商业化设计.md` 的设计**落成能跑的东西**。
      **不依赖真机、不依赖 ESP32** —— 用合成数据或已有的 UDP 抓包即可端到端验证。

设计要点（对应 02_docs/16）：
  · 字段字典是**唯一事实来源**（`05_hardware/STB3-125_字段字典.csv`），本脚本**不硬编码字段**
  · **「错误的数据不要」= 整行入库 + 无效字段置 NULL + 打质量标记**，**不是丢整条**
    （否则按实测 A/B/C_temp 恒为 −40，会把所有数据丢光）
  · 保留 `raw_hex`（原始 78 字节）⇒ 公式可演进、历史可重放
  · `PRIMARY KEY (device_id, collect_time)` ⇒ **补传/重发天然幂等**

四层校验：
  L0 传输层 —— CRC 由**固件**做（任一段失败即整轮不上传）；本层只认 `FAIL:` 行
  L1 结构层 —— 78 字节 / 39 字段 / H 与 L 成对
  L2 语义层 —— 量程越界、**无效值伪装（−40）**、状态一致性、电量单调性、时间合理性
  L3 业务层 —— 连续失败计数、设备级缺失率

用法：
    python db_proto.py --ddl                      # 打印建表 DDL
    python db_proto.py --demo                     # 用合成数据端到端跑一遍
    python db_proto.py --log <udp_listen 抓包>     # 用真实/历史抓包入库
    python db_proto.py --log <抓包> --again        # 再入一次，验证幂等（行数不变）

✅ 电量公式**已定案**（2026-10-27 砚青，D-10，见 `02_docs/31_电量公式勘误_2026-10-27.md`）：
   正确式 = `(H*65536 + L) / 1000`，结果单位**度**（kWh），因为 raw 的物理单位是 **Wh**。
   旧式 `(H*65536)/1000 + L` 把低字 L 当"度"直接相加 ⇒ 整体放大 1000 倍；
   且 L 由 65535 -> 0（H+1，**正常进位**）时会暴跌 65469.464 度。
   本脚本 / `05_hardware/STB3-125_字段字典.csv` / `02_docs/06` / 固件 已全部同步为此式。
   ⚠️ 定案前的历史库若需重算，请用 `raw_hex` 走本脚本重放，**不要直接改 energy 列**。
"""
import argparse
import csv
import os
import re
import sqlite3
import sys
from datetime import datetime, timedelta, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICT_CSV = os.path.join(ROOT, "05_hardware", "STB3-125_字段字典.csv")
REG_COUNT = 39
CST = timezone(timedelta(hours=8))

META_COLS = [
    ("device_id", "TEXT NOT NULL"),
    ("slave_addr", "INTEGER NOT NULL"),
    ("collect_time", "TEXT NOT NULL"),
    ("save_time", "TEXT NOT NULL"),
    ("quality", "TEXT NOT NULL"),
    ("seg_ok", "TEXT"),
    ("invalid_fields", "TEXT"),
    ("err_reason", "TEXT"),
    ("src", "TEXT NOT NULL"),
    ("seq", "INTEGER"),
    ("raw_hex", "TEXT"),
]


def load_dict():
    with open(DICT_CSV, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != REG_COUNT:
        raise SystemExit("字段字典应为 %d 行，实际 %d 行：%s" % (REG_COUNT, len(rows), DICT_CSV))
    for r in rows:
        r["reg_index"] = int(r["reg_index"])
        r["decimals"] = int(r["decimals"])
        r["raw_min"] = float(r["raw_min"]) if r["raw_min"] else None
        r["raw_max"] = float(r["raw_max"]) if r["raw_max"] else None
    return rows


def sql_type(f):
    if f["field"] in ("openState", "record1", "record2", "record3"):
        return "TEXT"
    if f["conv"] == "高字":
        return "INTEGER"
    return "REAL" if f["decimals"] > 0 else "INTEGER"


def build_ddl(fields):
    lines = ["-- 由 06_tests/db_proto.py 依 05_hardware/STB3-125_字段字典.csv 生成",
             "-- 单位/量程见字段字典；QUALITY: OK / PARTIAL / BAD",
             "CREATE TABLE IF NOT EXISTS meter_record ("]
    for c, t in META_COLS:
        lines.append("    %-16s %s," % (c, t))
    for f in fields:
        lines.append("    %-16s %s," % (f["field"], sql_type(f)))
    lines.append("    PRIMARY KEY (device_id, collect_time)")
    lines.append(");")
    lines.append("CREATE INDEX IF NOT EXISTS idx_meter_time ON meter_record(collect_time);")
    lines.append("CREATE INDEX IF NOT EXISTS idx_meter_quality ON meter_record(device_id, quality);")
    return "\n".join(lines)


# ---------------------------------------------------------------- 换算
def raw_to_value(f, regs):
    """按字段字典把原始寄存器换算成工程量。返回 (显示值, 是否无效, 原因)。"""
    i = f["reg_index"]
    raw = regs[i]
    name, conv = f["field"], f["conv"]

    if name == "openState":
        hi, lo = (raw >> 8) & 0xFF, raw & 0xFF
        lock = {0x01: "本地锁止", 0x00: "无锁"}.get(hi, "锁位未知")
        sw = {0x0F: "分闸", 0xF0: "合闸"}.get(lo, "分合位未知")
        return "%s/%s" % (lock, sw), False, None
    if name in ("record1", "record2", "record3"):
        return "%04X" % raw, False, None
    if conv == "高字":
        return float(raw), False, None
    if conv == "(H*65536+L)/1000":
        # D-10 定案（2026-10-27）：高低字拼成 32 位累加器整体除 1000（raw 单位 Wh => 度）。
        hi_i = f["reg_index"] - 1
        return (regs[hi_i] * 65536 + raw) / 1000.0, False, None

    v = {"raw/10": raw / 10.0, "raw/100": raw / 100.0, "raw/1000": raw / 1000.0,
         "raw-40": raw - 40.0, "raw*0.01": raw * 0.01}.get(conv, float(raw))

    # ---- L2 语义：量程 + 无效值伪装 ----
    if name.endswith("_temp") and name != "N_temp" and v == -40:
        return v, True, "= 量程下限，疑似『无数据』伪装（该相未装接线柱温度传感器）"
    lo, hi = f["raw_min"], f["raw_max"]
    if lo is not None and hi is not None:
        # ⚠️ 字段字典的 raw_min/raw_max 列虽然叫 "raw"，实际存的是【换算后工程量范围】：
        #   见 05_hardware/STB3-125_字段字典.csv —— temp 列 = -40..160（℃）、frequency = 0..100（Hz）。
        #   故此处一律直接按换算后的值比较，**不得再对 raw-40 字段二次减 40**。
        #   旧代码 (lo-40, hi-40) 把温度算成 -80..120 ℃，与固件（-40..160）矛盾，
        #   已于 2026-10-06 修正（闭合 PROGRESS.md §8.1 第 3 条）。
        lo_c, hi_c = lo, hi
        if v < lo_c or v > hi_c:
            return v, True, "越界（量程 %s~%s %s）" % (lo_c, hi_c, f["unit"])
    return v, False, None


# ---------------------------------------------------------------- 解析
LINE_RE = re.compile(r"^\s*\[?\s*\d*\]?\s*(\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2})\s*\|\s*(.+?)\s*(?:<-.*)?$")
TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}$")


def parse_line(line):
    """→ dict(ts=..., hexs=... | fail='Sx:REASON') 或 None（非数据行）

    ⚠️ 必须校验左边确实是**时间戳**：否则日志里的前置行（如串口列表
    `COM4 | 蓝牙链接上的标准串行 (COM4) | ...`）会被当成数据行收进来。
    """
    line = line.strip()
    if not line or "|" not in line or line.startswith("[udp_listen]") or "共收到" in line:
        return None
    ts, payload = line.split("|", 1)
    ts = re.sub(r"^\[?\s*\d*\]?\s*", "", ts).strip()
    if not TS_RE.match(ts):
        return None
    payload = payload.split("<-")[0].strip()
    if payload.startswith("FAIL:"):
        return {"ts": ts, "fail": payload[5:]}
    hexs = re.findall(r"\b[0-9A-Fa-f]{2}\b", payload)
    if len(hexs) != REG_COUNT * 2:
        return {"ts": ts, "fail": "LEN:HEX%d" % len(hexs)}
    return {"ts": ts, "hexs": hexs}


def bytes_to_regs(hexs):
    """78 个【字节】 → 39 个【16 位寄存器】（大端）。

    ⚠️ 必须显式两两拼接：`hexs` 是字节序列，直接 `regs[i]` 会取到第 i 个**字节**，
    造成整体错位（症状：A_voltage 拿到某个高字节、A_current 拿到相邻低字节）。
    """
    b = [int(h, 16) for h in hexs]
    return [(b[i * 2] << 8) | b[i * 2 + 1] for i in range(REG_COUNT)]


def decode(regs, fields, known_missing=()):
    """→ (vals, invalid_all, known_missing_hit)

    ⚠️ 关键设计：把「该型号**已知不上报**的字段」与「**本次异常**」分开。
       否则按实测 A/B/C_temp 恒为 −40，**每一行都会降级成 PARTIAL**，
       `quality` 就彻底失去区分度（永远没有 OK）。
    """
    vals, invalid, known = {}, {}, {}
    for f in fields:
        v, bad, why = raw_to_value(f, regs)
        if bad:
            vals[f["field"]] = None
            if f["field"] in known_missing:
                known[f["field"]] = why
            else:
                invalid[f["field"]] = why
        else:
            vals[f["field"]] = v
    return vals, invalid, known


# ---------------------------------------------------------------- 校验
def l2_consistency(vals):
    """整行级语义：状态一致性。返回 [(code, 说明)]"""
    out = []
    st = vals.get("openState") or ""
    if "合闸" in st:
        vs = [vals.get(k) or 0 for k in ("A_voltage", "B_voltage", "C_voltage")]
        if max(vs) == 0:
            out.append(("STATE", "openState=合闸 但三相电压全 0，可疑"))
    return out


def l2_energy_monotonic(conn, device_id, collect_time, vals):
    """电量单调性：与同设备上一条比较，只增不减。"""
    row = conn.execute(
        "SELECT A_H,A_L,H,L,collect_time FROM meter_record WHERE device_id=? AND collect_time<? "
        "ORDER BY collect_time DESC LIMIT 1", (device_id, collect_time)).fetchone()
    if not row:
        return []
    out = []
    for label, cur_h, cur_l, prev_h, prev_l in (
            ("A 相电量", vals.get("A_H"), vals.get("A_L"), row[0], row[1]),
            ("合相电量", vals.get("H"), vals.get("L"), row[2], row[3])):
        if None in (cur_h, cur_l, prev_h, prev_l):
            continue
        if cur_h * 65536 + cur_l < prev_h * 65536 + prev_l:
            out.append(("ENERGY", "%s 回退（32 位口径 %s < %s）" % (
                label, int(cur_h * 65536 + cur_l), int(prev_h * 65536 + prev_l))))
    return out


def l2_time_sanity(collect_time, save_time):
    out = []
    try:
        c = datetime.strptime(collect_time, "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return [("TIME", "collect_time 格式非法：%s" % collect_time)]
    s = datetime.strptime(save_time, "%Y-%m-%d %H:%M:%S")
    if abs((s - c).total_seconds()) > 86400:
        out.append(("TIME", "采集时刻与入库时刻相差 > 24h"))
    return out


# ---------------------------------------------------------------- 入库
def ingest(conn, rec, fields):
    """幂等写入。返回 'new' / 'dup' / 'bad'"""
    names = [c for c, _ in META_COLS] + [f["field"] for f in fields]
    ph = ",".join("?" * len(names))
    sql = "INSERT INTO meter_record (%s) VALUES (%s) ON CONFLICT(device_id,collect_time) DO UPDATE SET %s" % (
        ",".join(names), ph, ",".join("%s=excluded.%s" % (n, n) for n in names[2:]))
    conn.execute(sql, [rec[n] for n in names])
    return "new"


def store(conn, pkt, device_id, addr, src, fields, seq=None, known_missing=()):
    """一条上行 → 一行（或 BAD 行）。"""
    now = datetime.now(CST).strftime("%Y-%m-%d %H:%M:%S")
    if "fail" in pkt:
        parts = pkt["fail"].split(":", 1)
        seg, reason = (parts[0], parts[1]) if len(parts) == 2 else ("?", pkt["fail"])
        rec = {c: None for c, _ in META_COLS}
        rec.update(device_id=device_id, slave_addr=addr, collect_time=pkt["ts"], save_time=now,
                   quality="BAD", seg_ok="000", invalid_fields="", src=src,
                   err_reason="%s:%s" % (seg, reason), seq=seq)
        for f in fields:
            rec[f["field"]] = None
        return ingest(conn, rec, fields), "BAD"

    regs = bytes_to_regs(pkt["hexs"])
    if len(regs) != REG_COUNT:
        return "bad", "LEN"
    vals, invalid, known = decode(regs, fields, known_missing)

    flags = l2_consistency(vals)
    flags += l2_energy_monotonic(conn, device_id, pkt["ts"], vals)
    flags += l2_time_sanity(pkt["ts"], now)

    quality = "OK" if (not invalid and not flags) else "PARTIAL"
    notes = ["%s:%s" % (c, m) for c, m in flags]
    if known:
        notes.append("KNOWN_MISSING:" + ",".join(sorted(known)))
    rec = {c: None for c, _ in META_COLS}
    # seg 适配批次 2（2026-10-07 砚青）：数据行 seg="1"（一帧读全）/ "3"（三段）。
    # seg_ok 按实际读取模式记：一帧 "1"；三段沿用 "111"（3 段全成功）。
    # 失败行维持 "000"（无完整数据），段 tag 在 err_reason 里（"F:..." / "S2:..."）。
    rec.update(device_id=device_id, slave_addr=addr, collect_time=pkt["ts"], save_time=now,
               quality=quality, seg_ok=("1" if pkt.get("seg") == "1" else "111"),
               invalid_fields=",".join(sorted(invalid)),
               err_reason=";".join(notes) or None,
               src=src, seq=seq, raw_hex=" ".join(pkt["hexs"]))
    rec.update(vals)
    ingest(conn, rec, fields)
    # 幂等由 PRIMARY KEY(device_id, collect_time) + UPSERT 保证：
    # 同一 (设备, 采集时刻) 重复入库只会覆盖，不会新增行。
    return "new", quality


# ---------------------------------------------------------------- 演示数据
def demo_packets(n=6):
    """合成上行：模拟 m5_slave_04 的合成寄存器，构造 n 条合法 + 1 条 FAIL。"""
    def frame(volt, cur, a_h, a_l, st=0x00F0):
        r = [0] * REG_COUNT
        r[0] = st
        r[1], r[2], r[3] = 0x6660, 0x0066, 0x0000
        r[4], r[5], r[6], r[7] = 500, 123, 64, 0          # 频率/漏电/N_temp/A_temp(0→-40 伪装)
        r[8], r[9], r[10], r[11] = volt, cur, 98, volt * cur // 100 * 98 // 100
        r[14], r[15] = a_h, a_l
        r[37], r[38] = a_h, a_l
        return " ".join("%02X" % b for w in r for b in (w >> 8, w & 0xFF))
    out = []
    for i in range(n):
        ts = (datetime(2026, 10, 6, 20, 0, 0) + timedelta(seconds=i)).strftime("%Y-%m-%d %H:%M:%S")
        out.append({"ts": ts, "hexs": frame(222, 523 + i, 4, 58831 + i).split(" ")})
    out.append({"ts": "2026-10-06 20:00:10", "fail": "S2:TIMEOUT"})
    return out


# ---------------------------------------------------------------- 报告
def report(conn, fields):
    print()
    print("=" * 74)
    print("入库结果")
    print("=" * 74)
    total = conn.execute("SELECT COUNT(*) FROM meter_record").fetchone()[0]
    print("  总行数：%d" % total)
    for q, c in conn.execute("SELECT quality,COUNT(*) FROM meter_record GROUP BY quality ORDER BY quality"):
        print("    quality=%-8s %d" % (q, c))
    print()
    print("  按设备：")
    for did, c, ok, part, bad in conn.execute(
            "SELECT device_id,COUNT(*),"
            "SUM(quality='OK'),SUM(quality='PARTIAL'),SUM(quality='BAD') "
            "FROM meter_record GROUP BY device_id"):
        print("    %-20s 共 %-4d OK=%-4d PARTIAL=%-4d BAD=%-4d  （缺失率 %.1f%%）"
              % (did, c, ok or 0, part or 0, bad or 0, 100.0 * (bad or 0) / c))
    print()
    ic = [f["field"] for f in fields if f["field"].endswith("_temp") and f["field"] != "N_temp"]
    print("  字段级无效统计（示例：接线柱温度）：")
    for fn in ic:
        n = conn.execute("SELECT COUNT(*) FROM meter_record WHERE %s IS NULL" % fn).fetchone()[0]
        print("    %-10s NULL %d / %d" % (fn, n, total))
    print()
    print("  最近一行（关键字段）：")
    row = conn.execute(
        "SELECT collect_time,quality,openState,frequent,A_voltage,A_current,N_temp,A_temp,"
        "A_H,A_L,err_reason,invalid_fields,substr(raw_hex,1,23)||' ...' FROM meter_record "
        "ORDER BY quality DESC, collect_time DESC LIMIT 1").fetchone()
    if row:
        for k, v in zip(("collect_time", "quality", "openState", "frequent", "A_voltage",
                         "A_current", "N_temp", "A_temp", "A_H", "A_L",
                         "err_reason", "invalid_fields", "raw_hex"), row):
            print("    %-14s %s" % (k, v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ddl", action="store_true")
    ap.add_argument("--demo", action="store_true")
    ap.add_argument("--log")
    ap.add_argument("--again", action="store_true", help="重复入库，验证幂等")
    ap.add_argument("--db", default=os.path.join(ROOT, ".workbuddy", "tmp", "meter.db"))
    ap.add_argument("--device", default="48f6ee159b88")
    ap.add_argument("--src", default="sim")
    ap.add_argument("--known-missing", default="A_temp,B_temp,C_temp",
                    help="该型号【已知不上报】的字段：仍置 NULL，但不把整行降级为 PARTIAL")
    a = ap.parse_args()

    fields = load_dict()
    if a.ddl:
        print(build_ddl(fields))
        return 0

    os.makedirs(os.path.dirname(a.db), exist_ok=True)
    conn = sqlite3.connect(a.db)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(build_ddl(fields))

    pkts = []
    if a.demo:
        pkts = demo_packets()
        a.src = "sim"
        print("合成数据：%d 条（含 1 条 FAIL）" % len(pkts))
    elif a.log:
        for line in open(a.log, encoding="utf-8", errors="replace"):
            p = parse_line(line)
            if p:
                pkts.append(p)
        a.src = "real" if a.src == "sim" else a.src
        print("抓包文件：%s → 解析出 %d 条" % (a.log, len(pkts)))
    else:
        ap.print_help()
        return 1

    before = conn.execute("SELECT COUNT(*) FROM meter_record").fetchone()[0]
    known_missing = {s.strip() for s in a.known_missing.split(",") if s.strip()}
    print("已知不上报字段（置 NULL 但不降级）：%s" % (sorted(known_missing) or "无"))
    stat = {}
    for i, p in enumerate(pkts):
        r, q = store(conn, p, a.device, 1, a.src, fields, seq=i + 1, known_missing=known_missing)
        stat[q] = stat.get(q, 0) + 1
    conn.commit()
    after = conn.execute("SELECT COUNT(*) FROM meter_record").fetchone()[0]
    print("本次处理：%s；新增 %d 行（%d → %d）" % (stat, after - before, before, after))
    if a.again:
        print("（--again：与上次同样的数据再次入库，行数应不变）")
    report(conn, fields)
    conn.close()
    print()
    print("数据库：%s" % a.db)
    return 0


if __name__ == "__main__":
    sys.exit(main())
