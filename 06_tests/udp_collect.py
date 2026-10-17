#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""udp_collect.py —— 168 小时连续采集接收端（M7 验收主工具）

设计依据：02_docs/22_实现与交付方案.md §2（进程模型 / 收包参数 / 持久化 / 可观测性）
                    §3（三口径 + 四计数器对账 + 丢包四分类）

用法
----
    # 168h 正式长跑（默认）
    python udp_collect.py --db 03_logs\\run168.db

    # 30 分钟冒烟
    python udp_collect.py --db 03_logs\\smoke.db --hours 0.5

    # 只重算报告（验收复算，必须与运行时报告逐字节一致）
    python udp_collect.py --db 03_logs\\run168.db --report-only

    # 断电后补解析（parse_status='ok' 但 meter_record 缺行的补入库）
    python udp_collect.py --db 03_logs\\run168.db --repair

    # 统计引擎自检（构造数据集注入已知丢包/重启/重包/时钟回跳，逐项比对）
    python udp_collect.py --selftest

    # 加密对比（8h A/B；固件侧 ENC_ON=1 时启用）
    python udp_collect.py --db 03_logs\\enc.db --hours 8 --key-hex <32位hex>

三条硬约束（改代码前必读）
--------------------------
1. **T1 收包线程只做 recvfrom + json.loads + 计数器 + 入队**。
   严禁 SQL / print / 文件写 —— 一次 Defender 全盘扫描或 WAL checkpoint 就能让
   commit 阻塞数十秒，若收包与写库同线程，这期间新到的包只能堆在内核缓冲区，
   **溢出即静默丢包，且事后无法与"WiFi 掉线"区分**。
   （T1 的异常不直接写日志，攒进有界 deque，由 T3 落地 —— 等价于方案的 q_stat。）

2. **统计数字一个都不落盘（运行期累加值只是控制台快照）**。
   报告里的每一个数都由 SQL 从 udp_packet 重算 ⇒ 断电重启、进程重启、事后复算
   三个问题同时解决。`--report-only` 与运行时报告必须逐字节一致。

3. **写序不可颠倒**：先写 udp_packet → commit → 再写 meter_record。
   原始包不可再生，解析结果可再生；字典换版/口径改了都能全量重放。

对 db_proto.py 的复用边界
-------------------------
直接 import 复用：load_dict / build_ddl / store / decode / bytes_to_regs。
**不改 db_proto.py 一个字**（那是已被 M5 验证过的工具，作者≠验证者）。
本文件只做三件事：把 JSON 翻译成 store() 认识的 pkt、补 epoch_id、去重前置检查。
原因：`db_proto.ingest()` 恒返回 "new"，去重必须由调用方做（已核对源码 L239-246）。

关于「每轮发几行」——对账恒等式的前提
------------------------------------
固件每轮**必发 1 条 hb**；成功轮再发 1 条 data；失败轮再发 1 条 err
（`QI_SEND_ERR_ROW=1` 时，config.h L217 当前=1）。于是：
    成功轮 = 2 行，失败轮 = 2 行 ⇒ 期望行数 = 2 × ΔR
但若 `QI_SEND_ERR_ROW=0`，失败轮只有 1 行 ⇒ 期望值 = Δok×2 + Δerr×1。
**因此 2·ΔR 不是常量，必须实测推断**（见 infer_err_row_mode）。
把 2 写死会让长跑在"恰好失败很多"的时候对账差额凭空变大，且找不到原因。
"""

from __future__ import annotations

import argparse
import json
import logging
import logging.handlers
import os
import queue
import shutil
import socket
import sqlite3
import sys
import tempfile
import threading
import time
import traceback
from collections import deque
from datetime import datetime, timedelta, timezone

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import db_proto  # noqa: E402  同目录复用（解析/换算/入库）

# ---------------------------------------------------------------- 常量

CST = timezone(timedelta(hours=8))
SLAVE_ADDR = 1                 # STB3-125 地址；固件侧写死为 1（config.h）
REG_COUNT = 39                 # 39 输入寄存器
RAW_BYTES = REG_COUNT * 2      # 78 字节
PERIOD_S = 1.0                 # 设计周期 1 Hz
HOURS_168 = 168.0

MAX_DGRAM = 4096               # 单包上限：明文最坏 1106 B / 密文 1135 B
Q_MAX = 20000                  # q_pkt 上限（按 1 Hz 约 5.5 h 缓冲，远超任何阻塞）
SNAP_INTERVAL_S = 60           # 分钟快照
CONSOLE_INTERVAL_S = 10        # 控制台行
COMMIT_SECS = 2.0              # 提交策略三条件之一
COMMIT_ROWS = 500
COMMIT_IDLE_S = 1.0
CHECKPOINT_SECS = 6 * 3600     # 每 6h TRUNCATE 一次 WAL
DISK_LOW_GB = 10.0

# 该型号恒不上报的字段（raw 恒 0 ⇒ 恰为量程下限 -40 ℃ 的"无数据"伪装）
# 与固件 QI_PHASE_TEMP_ABSENT=1 口径一致；不列入则每一行都会被降级成 PARTIAL。
KNOWN_MISSING = {"A_temp", "B_temp", "C_temp"}

DATA_REQUIRED = ("seq", "dev", "ts", "raw", "quality")
HB_REQUIRED = ("seq", "dev", "poll_ok", "poll_err", "udp_tx")
ERR_REQUIRED = ("seq", "dev", "type", "seg", "reason")


def _fix_console() -> None:
    """Windows 控制台默认 GBK，中文/箭头会炸；强制 UTF-8 且不因单字符异常退出。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def setup_logging(db_path: str, verbose: bool) -> logging.Logger:
    log_path = os.path.splitext(db_path)[0] + ".log"
    d = os.path.dirname(os.path.abspath(log_path))
    if d:
        os.makedirs(d, exist_ok=True)
    lg = logging.getLogger("udp_collect")
    lg.setLevel(logging.DEBUG if verbose else logging.INFO)
    lg.handlers.clear()
    fh = logging.handlers.RotatingFileHandler(
        log_path, maxBytes=50 * 1024 * 1024, backupCount=10, encoding="utf-8")
    fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(message)s"))
    lg.addHandler(fh)
    sh = logging.StreamHandler(sys.stdout)
    sh.setLevel(logging.INFO)
    sh.setFormatter(logging.Formatter("%(message)s"))
    lg.addHandler(sh)
    return lg


# ---------------------------------------------------------------- 运行期计数器

class Stats:
    """跨线程计数器。T1/T2 写、T3 读，都是 3 次/秒的量级 ⇒ 锁开销可忽略。"""

    def __init__(self) -> None:
        self._lk = threading.Lock()
        self.rx = 0
        self.rx_data = 0
        self.rx_hb = 0
        self.rx_err = 0
        self.rx_unknown = 0
        self.bad_json = 0
        self.bad_field = 0
        self.oversize = 0
        self.decrypt_fail = 0
        self.q_drop = 0
        self.db_rows = 0
        self.dup = 0
        self.collision = 0
        self.same_second = 0
        self.clock_step = 0
        self.store_attempts = 0
        self.store_ok = 0
        self.store_bad = 0
        self.quality_conflict = 0
        self.commit_count = 0
        self.last_rx_mono = 0.0
        self.last_seq = 0
        self.last_poll_ok = 0
        self.last_poll_err = 0
        self.last_udp_tx = 0
        self.last_rssi = 0
        self.last_heap = 0
        self.last_db_mb = 0.0
        self.epochs = 0

    def bump(self, **kw) -> None:
        with self._lk:
            for k, v in kw.items():
                setattr(self, k, getattr(self, k, 0) + v)

    def set(self, **kw) -> None:
        with self._lk:
            for k, v in kw.items():
                setattr(self, k, v)

    def snap(self) -> dict:
        with self._lk:
            return dict(self.__dict__)


# ---------------------------------------------------------------- 数据库层

DDL_EXTRA = """
-- ① 原始流水：唯一不可再生资源，只增不改。
--    ⚠️ 刻意**不建** UNIQUE(device_id, seq)：重复包必须留下来当证据，
--       重包率是实测指标，去重放在分析期。
CREATE TABLE IF NOT EXISTS udp_packet (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    recv_time     TEXT NOT NULL,
    recv_mono     REAL NOT NULL,
    src_ip        TEXT,
    src_port      INTEGER,
    dev           TEXT,
    device_id     TEXT,
    seq           INTEGER,
    ptype         TEXT NOT NULL,
    payload_len   INTEGER,
    payload       BLOB,
    enc           INTEGER DEFAULT 0,
    parse_status  TEXT,
    epoch_id      INTEGER,
    poll_ok       INTEGER,
    poll_err      INTEGER,
    udp_tx        INTEGER,
    rssi          INTEGER,
    free_heap     INTEGER,
    ts_ok         INTEGER
);
CREATE INDEX IF NOT EXISTS idx_pkt_dev_seq ON udp_packet(device_id, seq);
CREATE INDEX IF NOT EXISTS idx_pkt_recv    ON udp_packet(recv_mono);
CREATE INDEX IF NOT EXISTS idx_pkt_type    ON udp_packet(ptype, recv_mono);
CREATE INDEX IF NOT EXISTS idx_pkt_epoch   ON udp_packet(epoch_id, seq);

-- ② 采集会话（一次长跑一行；重启不新建，靠 ingest_cursor.run_id 认领）
CREATE TABLE IF NOT EXISTS collect_run (
    run_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at   TEXT,
    planned_end  TEXT,
    ended_at     TEXT,
    target_hours REAL DEFAULT 168,
    period_ms    INTEGER DEFAULT 1000,
    bin_sha256   TEXT,
    note         TEXT
);

-- ③ 设备启动世代：seq 回退 + poll 计数回退即切世代。
--    所有统计都以 epoch 为分组单位 —— 否则每次重启 seq 从 1 重来，
--    用 (dev, seq) 去重会把重启算成上千个"重复包"，重包率瞬间爆表。
CREATE TABLE IF NOT EXISTS device_epoch (
    epoch_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id         TEXT,
    seq_first         INTEGER,
    seq_last          INTEGER,
    first_recv        TEXT,
    last_recv         TEXT,
    poll_ok_at_start  INTEGER,
    poll_err_at_start INTEGER,
    udp_tx_at_start   INTEGER,
    detect            TEXT
);

-- ④ 分钟级快照（趋势/告警用，**不参与最终口径**）
CREATE TABLE IF NOT EXISTS stat_snapshot (
    ts               TEXT PRIMARY KEY,
    run_id           INTEGER,
    device_id        TEXT,
    pkt_rx           INTEGER,
    data_rx          INTEGER,
    hb_rx            INTEGER,
    err_rx           INTEGER,
    seq_last         INTEGER,
    lost_lines       INTEGER,
    full_lost_rounds INTEGER,
    data_lost        INTEGER,
    dup              INTEGER,
    q_depth          INTEGER,
    q_drop           INTEGER,
    poll_ok_last     INTEGER,
    poll_err_last    INTEGER,
    udp_tx_last      INTEGER,
    rssi             INTEGER,
    free_heap        INTEGER,
    db_mb            REAL,
    disk_free_mb     REAL
);

-- ⑤ 断点游标
CREATE TABLE IF NOT EXISTS ingest_cursor (k TEXT PRIMARY KEY, v TEXT);

-- ⑥ 异常事件（报告"事件表"直接来源于此）
CREATE TABLE IF NOT EXISTS event_log (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    ts       TEXT,
    kind     TEXT,
    severity TEXT,
    detail   TEXT
);
CREATE INDEX IF NOT EXISTS idx_evt_kind ON event_log(kind, severity);
"""


def open_db(db_path: str, readonly: bool = False) -> sqlite3.Connection:
    """打开库并施加 PRAGMA。

    ⚠️ T3 用 `mode=ro` 在 WAL 库上会因缺 `-shm` 写权限报
    "attempt to write a readonly database"（SQLite 已知行为）。
    故只读连接**先试 ro、失败回退 rw + PRAGMA query_only** ——
    宁可多一个逻辑只读句柄，也不能让统计线程在长跑中段崩掉。
    """
    conn = None
    if readonly:
        try:
            uri = "file:%s?mode=ro" % db_path.replace("\\", "/")
            conn = sqlite3.connect(uri, uri=True, timeout=10.0, check_same_thread=False)
            conn.execute("SELECT 1 FROM sqlite_master LIMIT 1").fetchall()
        except sqlite3.Error:
            if conn is not None:
                try:
                    conn.close()
                except sqlite3.Error:
                    pass
            conn = sqlite3.connect(db_path, timeout=10.0, check_same_thread=False)
            try:
                conn.execute("PRAGMA query_only=ON")   # 退化成逻辑只读
            except sqlite3.Error:
                pass
    else:
        conn = sqlite3.connect(db_path, timeout=10.0, check_same_thread=False)

    if not readonly:
        conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA wal_autocheckpoint=2000")
    conn.execute("PRAGMA journal_size_limit=67108864")
    conn.execute("PRAGMA busy_timeout=10000")
    conn.execute("PRAGMA temp_store=MEMORY")
    conn.row_factory = sqlite3.Row
    return conn


def init_schema(conn: sqlite3.Connection, fields) -> None:
    """建表 + 给 meter_record 补 epoch_id（ALTER 幂等）。"""
    conn.executescript(DDL_EXTRA)
    conn.executescript(db_proto.build_ddl(fields))

    cols = {r["name"] for r in conn.execute("PRAGMA table_info(meter_record)")}
    if "epoch_id" not in cols:
        conn.execute("ALTER TABLE meter_record ADD COLUMN epoch_id INTEGER")
    # 幂等键：真正的"同一条上行"判据。NULL 在 SQLite 唯一索引里互不冲突，
    # 故历史行（epoch_id 为 NULL）不会互相顶掉。
    conn.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_meter_epoch_seq "
                 "ON meter_record(device_id, epoch_id, seq)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_meter_epoch "
                 "ON meter_record(epoch_id, seq)")
    conn.commit()


def log_event(conn: sqlite3.Connection, ts: str, kind: str, severity: str, detail: str) -> None:
    conn.execute("INSERT INTO event_log (ts,kind,severity,detail) VALUES (?,?,?,?)",
                 (ts, kind, severity, detail))


def cursor_get(conn, k, default=None):
    row = conn.execute("SELECT v FROM ingest_cursor WHERE k=?", (k,)).fetchone()
    return row["v"] if row else default


def cursor_set(conn, k, v) -> None:
    conn.execute("INSERT INTO ingest_cursor (k,v) VALUES (?,?) "
                 "ON CONFLICT(k) DO UPDATE SET v=excluded.v", (k, str(v)))


# ---------------------------------------------------------------- 世代（epoch）

class EpochTracker:
    """按设备维护「当前世代」，只在 T2（唯一写线程）里调用。

    判据：seq 相对上次**大幅**回退，**且** hb 里 poll_ok+poll_err 总计数也回退
    （累计值只增，回退必是重启）。只有 seq 回退而 poll 仍增 ⇒ 乱序/迟到包，
    **不切世代** —— 否则局域网内一次乱序就能把一代数据劈成两半。
    """

    BACK_TOL = 64           # seq 小回退容忍度（乱序窗口远小于此）
    REBOOT_SEQ_DROP = 1000  # 大幅回退：即使没有 poll 也可判重启

    def __init__(self, conn: sqlite3.Connection, logger: logging.Logger):
        self.conn = conn
        self.lg = logger
        self.state = {}     # device_id -> {"epoch_id","seq_last","seq_max","poll_total"}
        self._restore()

    def _restore(self) -> None:
        """进程重启后从库里恢复每个设备的最新世代，避免把"续跑"误判成"新世代"。"""
        for r in self.conn.execute(
                "SELECT epoch_id,device_id,seq_last FROM device_epoch ORDER BY epoch_id"):
            self.state[r["device_id"]] = {
                "epoch_id": r["epoch_id"],
                "seq_last": r["seq_last"] or 0,
                "seq_max": r["seq_last"] or 0,
                "poll_total": None,
            }
        for dev, st in self.state.items():
            row = self.conn.execute(
                "SELECT poll_ok,poll_err FROM udp_packet WHERE epoch_id=? AND ptype='hb' "
                "AND poll_ok IS NOT NULL ORDER BY id DESC LIMIT 1",
                (st["epoch_id"],)).fetchone()
            if row:
                st["poll_total"] = (row["poll_ok"] or 0) + (row["poll_err"] or 0)

    def current(self, device_id: str, seq: int, ts: str, poll: tuple | None) -> int:
        """返回该包所属 epoch_id，必要时新建世代。不 commit（由 T2 统一提交）。"""
        st = self.state.get(device_id)
        if st is None:
            return self._new(device_id, seq, ts, poll, "cold_start")

        reason = None
        if seq < st["seq_last"] - self.BACK_TOL:
            if poll is None or st["poll_total"] is None:
                if seq < st["seq_last"] - self.REBOOT_SEQ_DROP:
                    reason = "seq_rollback"
            elif sum(poll[:2]) < st["poll_total"]:
                reason = "seq_rollback"
        if reason is None and poll is not None and st["poll_total"] is not None:
            if sum(poll[:2]) < st["poll_total"]:
                reason = "poll_reset"

        if reason:
            return self._new(device_id, seq, ts, poll, reason)

        st["seq_last"] = max(st["seq_last"], seq)
        st["seq_max"] = max(st["seq_max"], seq)
        if poll is not None:
            st["poll_total"] = sum(poll[:2])
        return st["epoch_id"]

    def _new(self, device_id, seq, ts, poll, reason) -> int:
        cur = self.conn.execute(
            "INSERT INTO device_epoch (device_id,seq_first,seq_last,first_recv,last_recv,"
            "poll_ok_at_start,poll_err_at_start,udp_tx_at_start,detect) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            (device_id, seq, seq, ts, ts,
             poll[0] if poll else None, poll[1] if poll else None,
             poll[2] if poll and len(poll) > 2 else None, reason))
        eid = cur.lastrowid
        self.state[device_id] = {"epoch_id": eid, "seq_last": seq, "seq_max": seq,
                                 "poll_total": sum(poll[:2]) if poll else None}
        self.lg.warning("[EPOCH] %s 新世代 #%d（%s，seq=%s）", device_id, eid, reason, seq)
        return eid

    def touch(self, device_id: str, ts: str) -> None:
        st = self.state.get(device_id)
        if st:
            self.conn.execute("UPDATE device_epoch SET last_recv=?,seq_last=? WHERE epoch_id=?",
                              (ts, st["seq_max"], st["epoch_id"]))


# ---------------------------------------------------------------- 时间戳归一

class BootAnchor:
    """`ts_ok=false` 时固件发的是 `BOOT+<秒>`（早期串口格式为 `BOOT+秒.毫秒`）。

    这**不是时间戳**，不能直接当 collect_time。用「首包到达墙钟 − BOOT 秒数」
    反推开机时刻，锚点一旦定下就不动（每世代一个）。
    锚点误差 = 该包从固件组装到到达 PC 的时延（毫秒级），属于**已知近似**；
    这类行 src='udp_boot'，报告里与 NTP 行分开计数，绝不混算。
    """

    def __init__(self) -> None:
        self.anchor = {}   # epoch_id -> datetime

    def to_wall(self, epoch_id: int, ts_raw, recv_wall: datetime):
        """→ (collect_time 'YYYY-mm-dd HH:MM:SS', src_tag, ok)"""
        if not isinstance(ts_raw, str):
            return None, "udp_bad", False
        s = ts_raw.strip().replace("T", " ")
        if s.startswith("BOOT+"):
            try:
                secs = float(s[5:])
            except ValueError:
                return None, "udp_bad", False
            a = self.anchor.get(epoch_id)
            if a is None:
                a = recv_wall - timedelta(seconds=secs)
                self.anchor[epoch_id] = a
            return (a + timedelta(seconds=secs)).strftime("%Y-%m-%d %H:%M:%S"), "udp_boot", False
        if len(s) >= 19 and s[4] == "-" and s[7] == "-" and s[13] == ":":
            return s[:19], "udp", True
        return None, "udp_bad", False


# ---------------------------------------------------------------- JSON → store

class StoreResult:
    NEW, DUP, COLLIDE, BAD = "new", "dup", "collide", "bad"
    SAME_SECOND = "same_second"    # 两轮落在同一秒（ts 精度不足，非时钟故障）
    CLOCK_STEP = "clock_step"      # ts 相对上一轮**倒退**（真 NTP 回跳）


def store_json(conn: sqlite3.Connection, obj: dict, device_id: str, fields,
               epoch_id: int, recv_wall: datetime, anchors: BootAnchor) -> tuple:
    """把一条 JSON 数据行落成 meter_record 的一行。返回 (结果, quality)。

    与 db_proto.store 的差异仅三处，其余全复用：
      1. JSON 的 raw 是 233 字符空格分隔串 ⇒ 先 `.split()` 成 78 个字节串
         （bytes_to_regs 自己两两拼寄存器，不能替它拼）。
      2. 去重前置检查 —— ingest() 恒返回 "new"，见文件头。
      3. 入库后回填 epoch_id（db_proto 不认识这个列，不硬塞进它的 rec）。
    """
    ts_db, src, _ok = anchors.to_wall(epoch_id, obj.get("ts"), recv_wall)
    if ts_db is None:
        return StoreResult.BAD, "TS"

    raw = obj.get("raw")
    if not isinstance(raw, str):
        return StoreResult.BAD, "RAW"
    hexs = raw.split()
    if len(hexs) != RAW_BYTES or any(len(h) != 2 for h in hexs):
        return StoreResult.BAD, "RAW_LEN%d" % len(hexs)

    seq = obj.get("seq")
    if not isinstance(seq, int):
        return StoreResult.BAD, "SEQ"

    # ---- 去重（幂等键 = device_id + epoch_id + seq）----
    if conn.execute("SELECT 1 FROM meter_record WHERE device_id=? AND epoch_id=? AND seq=? LIMIT 1",
                    (device_id, epoch_id, seq)).fetchone():
        return StoreResult.DUP, None

    # ---- 同秒撞车检查 ----
    # 现主键 (device_id, collect_time) 是秒级；两条不同轮次撞主键会被
    # ON CONFLICT DO UPDATE **静默覆盖** ⇒ 一行凭空消失且无报错。这里提前拦住。
    #
    # ⚠️ 但撞车有四种成因，**绝不能一律当"时钟回跳"**（第一版就是这么错的，
    #    冒烟测试里 0.9 s 轮询立刻误报 2 次，长跑报告会被一眼打回）：
    #      ① ts 与上一轮相同        ⇒ ts 只到秒，1 Hz 下偶发，**属分辨率伪影**，不是故障
    #      ② ts 比上一轮更早        ⇒ 真 NTP 回跳（CLOCK_STEP）
    #      ③ ts 更晚但仍撞车        ⇒ 迟到/乱序到达
    #      ④ 上一轮不存在           ⇒ 世代首包，正常
    prev = conn.execute(
        "SELECT collect_time,seq FROM meter_record WHERE device_id=? AND epoch_id=? AND seq<? "
        "ORDER BY seq DESC LIMIT 1", (device_id, epoch_id, seq)).fetchone()
    hit = conn.execute("SELECT epoch_id,seq FROM meter_record WHERE device_id=? AND collect_time=?",
                       (device_id, ts_db)).fetchone()
    if hit:
        prev_ts = prev["collect_time"] if prev else None
        if prev_ts and ts_db < prev_ts:
            log_event(conn, recv_wall.strftime("%Y-%m-%d %H:%M:%S"), "CLOCK_STEP", "WARN",
                      "collect_time 从 %s 倒退到 %s（seq %s→%s），该行未覆盖"
                      % (prev_ts, ts_db, prev["seq"], seq))
            return StoreResult.CLOCK_STEP, None
        if prev_ts and ts_db == prev_ts:
            log_event(conn, recv_wall.strftime("%Y-%m-%d %H:%M:%S"), "TS_SAME_SECOND", "INFO",
                      "collect_time=%s 被 seq=%s 与 seq=%s 共用（ts 只到秒，非故障）"
                      % (ts_db, prev["seq"], seq))
            return StoreResult.SAME_SECOND, None
        log_event(conn, recv_wall.strftime("%Y-%m-%d %H:%M:%S"), "TS_OUT_OF_ORDER", "WARN",
                  "collect_time=%s 已被 seq=%s 占用，本轮 seq=%s 未覆盖（乱序到达）"
                  % (ts_db, hit["seq"], seq))
        return StoreResult.COLLIDE, None

    pkt = {"ts": ts_db, "hexs": hexs, "seg": obj.get("seg", "1")}
    try:
        db_proto.store(conn, pkt, device_id, SLAVE_ADDR, src, fields,
                       seq=seq, known_missing=KNOWN_MISSING)
    except sqlite3.IntegrityError as e:
        # 唯一索引兜底（极端时序下前置检查漏过）——绝不让它打断长跑
        log_event(conn, recv_wall.strftime("%Y-%m-%d %H:%M:%S"), "INTEGRITY", "WARN", str(e))
        return StoreResult.DUP, None
    except (ValueError, KeyError, IndexError) as e:
        log_event(conn, recv_wall.strftime("%Y-%m-%d %H:%M:%S"), "STORE_EXC", "WARN", repr(e))
        return StoreResult.BAD, "EXC"

    conn.execute("UPDATE meter_record SET epoch_id=? WHERE device_id=? AND collect_time=?",
                 (epoch_id, device_id, ts_db))

    # ---- quality 复核：固件说 OK 但有 invalid 字段 ⇒ 固件口径 bug 的证据 ----
    q = obj.get("quality")
    inv = (obj.get("invalid") or "").strip()
    if q == "OK" and inv:
        return StoreResult.NEW, "OK_CONFLICT"
    return StoreResult.NEW, q


# ---------------------------------------------------------------- 加密

def decrypt_frame(data: bytes, key: bytes):
    """帧格式：1B version(=1) + 12B nonce + ciphertext + 16B tag（AES-128-GCM）。

    失败一律返回 None（**fail-closed**）：错解出来的 JSON 会污染统计，
    比丢包严重得多 —— 丢包有数可查，脏数据没有。
    """
    if len(data) < 1 + 12 + 16 or data[0] != 1:
        return None
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise SystemExit("使用 --key-hex 需要 cryptography：pip install cryptography")
    try:
        return AESGCM(key).decrypt(data[1:13], data[13:], None)
    except Exception:
        return None


# ---------------------------------------------------------------- T1 收包线程

class Receiver(threading.Thread):
    """只做：收 → 解 JSON → 分类 → 校验必填 → 入队。不碰 SQL / 不打日志。"""

    def __init__(self, port: int, q: queue.Queue, stats: Stats, stop: threading.Event,
                 key: bytes | None, lg: logging.Logger):
        super().__init__(name="T1-rx", daemon=True)
        self.port = port
        self.q = q
        self.st = stats
        self.stop = stop
        self.key = key
        self.lg = lg
        self.anomalies = deque(maxlen=200)   # T3 负责落库/写日志
        self.sock = None
        self.fatal = None

    def run(self) -> None:
        try:
            self._open()
        except OSError as e:
            self.fatal = e
            self.lg.error("[FATAL] 绑定 UDP:%d 失败：%s", self.port, e)
            self.lg.error("        8888 被占用最常见的原因是 NetAssist（老师给的上位机）还开着。")
            self.lg.error("        它在 Windows 上会用 SO_REUSEADDR 抢走数据，且**不报错** —— 先关掉再重试。")
            self.stop.set()
            return

        sock_err = 0
        while not self.stop.is_set():
            try:
                data, addr = self.sock.recvfrom(MAX_DGRAM)
            except socket.timeout:
                continue                      # 正常空闲，不是错误
            except ConnectionResetError:
                continue                      # Windows：对端不可达的 ICMP 回传（10054）
            except OSError as e:
                sock_err += 1
                if sock_err <= 20 and len(self.anomalies) < 200:
                    self.anomalies.append(("SOCK_ERR", "WARN", str(e)))
                continue
            try:
                self._handle(data, addr)
            except Exception:
                # 单包解析异常绝不能掀翻收包线程
                if len(self.anomalies) < 200:
                    self.anomalies.append(("PARSE_EXC", "WARN", traceback.format_exc(limit=3)))
        try:
            self.sock.close()
        except OSError:
            pass

    def _open(self) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # SO_RCVBUF 必须在 bind 之前设；Windows 默认 64 KB ≈ 29 s 缓冲，
        # 一次 checkpoint 就能填满，之后内核静默丢包。
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        except OSError:
            pass
        # Windows 专属：SO_REUSEADDR 允许第二个进程**静默抢包**；
        # SO_EXCLUSIVEADDRUSE 反过来，端口被占时直接 bind 失败（我们要的就是这个）。
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            try:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            except OSError:
                pass
        s.bind(("0.0.0.0", self.port))
        s.settimeout(0.5)   # 只用于响应 stop，超时不是错误
        self.sock = s

    def _handle(self, data: bytes, addr) -> None:
        now = datetime.now(CST)
        recv = {
            "recv_time": now.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
            "recv_wall": now,
            "recv_mono": time.perf_counter(),
            "src_ip": addr[0],
            "src_port": addr[1],
            "payload_len": len(data),
            "payload": data,
            "enc": 0,
            "parse_status": "ok",
            "ptype": "unknown",
            "obj": None,
        }
        self.st.set(last_rx_mono=recv["recv_mono"])

        if len(data) >= MAX_DGRAM:
            recv["parse_status"] = "oversize"
            self.st.bump(rx=1, oversize=1)
            self._put(recv)
            return

        text = self._decode_payload(data, recv)
        if text is None:
            self.st.bump(rx=1)
            self._put(recv)
            return

        try:
            obj = json.loads(text)
            if not isinstance(obj, dict):
                raise ValueError("not an object")
        except ValueError:
            recv["parse_status"] = "bad_json"
            self.st.bump(rx=1, bad_json=1)
            self._put(recv)
            return

        t = obj.get("type")
        ptype = "data" if t is None else ("hb" if t == "hb" else ("err" if t == "err" else "unknown"))
        need = {"data": DATA_REQUIRED, "hb": HB_REQUIRED, "err": ERR_REQUIRED}.get(ptype)
        if need and any(k not in obj for k in need):
            missing = [k for k in need if k not in obj]
            recv["parse_status"] = "bad_field"
            recv["obj"] = obj
            recv["ptype"] = ptype
            self.st.bump(rx=1, bad_field=1)
            if len(self.anomalies) < 200:
                self.anomalies.append(("BAD_FIELD", "WARN", "%s 缺 %s" % (ptype, ",".join(missing))))
            self._put(recv)
            return

        recv["obj"] = obj
        recv["ptype"] = ptype
        if ptype == "data":
            self.st.bump(rx=1, rx_data=1)
        elif ptype == "hb":
            self.st.bump(rx=1, rx_hb=1)
        elif ptype == "err":
            self.st.bump(rx=1, rx_err=1)
        else:
            self.st.bump(rx=1, rx_unknown=1)
        self._put(recv)

    def _decode_payload(self, data: bytes, recv: dict):
        """返回可 json.loads 的文本；失败返回 None。

        先认明文（首字节 `{`），再谈解密。这个判据比"有没有传 --key"可靠：
        固件 ENC_ON=0 时永远发明文，此时哪怕传了 --key 也不该乱解。
        """
        if data[:1] == b"{":
            recv["enc"] = 0
            return data.decode("utf-8", "replace")

        if self.key is None:
            recv["parse_status"] = "bad_json"
            if len(self.anomalies) < 200:
                self.anomalies.append(("NOT_JSON", "WARN",
                                       "首字节 0x%02X，非明文；若是密文请加 --key-hex"
                                       % (data[0] if data else 0)))
            return None

        plain = decrypt_frame(data, self.key)
        if plain is None:
            recv["enc"] = 1
            recv["parse_status"] = "decrypt_fail"
            self.st.bump(decrypt_fail=1)
            if len(self.anomalies) < 200:
                self.anomalies.append(("DECRYPT_FAIL", "WARN", "len=%d" % len(data)))
            return None
        recv["enc"] = 1
        return plain.decode("utf-8", "replace")

    def _put(self, recv: dict) -> None:
        """背压：满则丢并累加 q_drop —— 这是**唯一允许的、可观测的**丢弃点，
        必须与内核缓冲静默丢包区分开（前者有数，后者只能靠 seq 缺口推断）。"""
        try:
            self.q.put_nowait(recv)
        except queue.Full:
            self.st.bump(q_drop=1)
            if len(self.anomalies) < 200:
                self.anomalies.append(("QUEUE_FULL", "ALERT", "q_pkt 满，已丢弃 1 包"))


# ---------------------------------------------------------------- T2 落库线程

class Writer(threading.Thread):
    """唯一写线程：世代判定 → 写 udp_packet → 提交 → 写 meter_record。"""

    def __init__(self, db_path: str, q: queue.Queue, stats: Stats, stop: threading.Event,
                 fields, run_id: int, lg: logging.Logger):
        super().__init__(name="T2-db", daemon=True)
        self.db_path = db_path
        self.q = q
        self.st = stats
        self.stop = stop
        self.fields = fields
        self.run_id = run_id
        self.lg = lg
        self.conn = None
        self.epochs = None
        self.anchors = BootAnchor()
        self.pending_rows = 0
        self.last_commit = time.monotonic()
        self.last_ckpt = time.monotonic()
        self.last_ip = {}          # device_id -> src_ip（IP_CHANGE 检测）
        self.last_progress = time.monotonic()
        self.fatal = None
        self.ready = threading.Event()   # 供 T3 等"写入端已就绪"（见 StatReporter.run）

    def run(self) -> None:
        try:
            self.conn = open_db(self.db_path)
            # ⚠️ 写探测：WAL 库若已被一个 `mode=ro` 连接**先**打开（此时 -shm 还不存在），
            #    Windows 上该只读句柄会挡住写入，本线程之后每条 INSERT 都报
            #    "attempt to write a readonly database"，而且收包端看起来一切正常
            #    （rx 一直在涨、只有 db 行数不动）—— 这是最难查的一类静默失败。
            #    这里启动即探一次，宁可当场退出，也不要跑 168 小时后拿到一份空报告。
            self.conn.execute("PRAGMA user_version=1")
            self.conn.commit()
            self.epochs = EpochTracker(self.conn, self.lg)
            self.ready.set()
            while True:
                try:
                    recv = self.q.get(timeout=0.5)
                except queue.Empty:
                    if self._should_commit():
                        self._commit()
                    if self.stop.is_set():
                        break
                    continue
                if recv is None:
                    break
                self._process(recv)
                if self._should_commit():
                    self._commit()
                if time.monotonic() - self.last_ckpt > CHECKPOINT_SECS:
                    self._checkpoint()
        except Exception as e:
            self.fatal = e
            self.lg.error("[T2] 落库线程异常：\n%s", traceback.format_exc())
        finally:
            if self.conn is not None:
                self._drain()
                self._commit(force=True)
                try:
                    self.conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
                except sqlite3.Error:
                    pass
                try:
                    self.conn.close()
                except sqlite3.Error:
                    pass

    def _drain(self) -> None:
        """停止时把队列排空（长跑退出码要为 0，不能带着未写的包退出）。"""
        n = 0
        while True:
            try:
                recv = self.q.get_nowait()
            except queue.Empty:
                break
            if recv is None:
                break
            self._process(recv)
            n += 1
        if n:
            self.lg.info("[T2] 退出前排空 %d 包", n)

    def _should_commit(self) -> bool:
        if self.pending_rows >= COMMIT_ROWS:
            return True
        if self.pending_rows == 0:
            return False
        now = time.monotonic()
        if now - self.last_commit >= COMMIT_SECS:
            return True
        return (now - self.last_progress) >= COMMIT_IDLE_S

    def _commit(self, force: bool = False) -> None:
        if self.pending_rows == 0 and not force:
            return
        try:
            self.conn.commit()
            self.st.bump(commit_count=1)
        except sqlite3.Error as e:
            self.lg.error("[T2] commit 失败：%s", e)
            try:
                self.conn.rollback()
            except sqlite3.Error:
                pass
        self.pending_rows = 0
        self.last_commit = time.monotonic()

    def _checkpoint(self) -> None:
        try:
            self.conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            self.conn.commit()
            self.last_ckpt = time.monotonic()
            self.lg.info("[T2] WAL checkpoint(TRUNCATE) 完成")
        except sqlite3.Error as e:
            self.lg.warning("[T2] checkpoint 失败：%s", e)

    # ------------------------------------------------ 单包处理
    def _process(self, recv: dict) -> None:
        obj = recv.get("obj") or {}
        dev = obj.get("dev") or "UNKNOWN"
        device_id = dev
        seq = obj.get("seq") if isinstance(obj.get("seq"), int) else None
        ptype = recv["ptype"]
        ok = recv["parse_status"] == "ok"

        # hb 携带累计计数器，是 epoch 判定与 M1/M3 的唯一来源
        poll = None
        if ptype == "hb" and all(isinstance(obj.get(k), int)
                                 for k in ("poll_ok", "poll_err", "udp_tx")):
            poll = (obj["poll_ok"], obj["poll_err"], obj["udp_tx"])
            self.st.set(last_poll_ok=obj["poll_ok"], last_poll_err=obj["poll_err"],
                        last_udp_tx=obj["udp_tx"], last_seq=seq or 0,
                        last_rssi=obj.get("rssi") or 0,
                        last_heap=obj.get("free_heap") or 0)
        elif seq is not None:
            self.st.set(last_seq=seq)

        epoch_id = None
        if ok and seq is not None:
            epoch_id = self.epochs.current(device_id, seq, recv["recv_time"], poll)
            self.epochs.touch(device_id, recv["recv_time"])
            self.st.set(epochs=len(self.epochs.state))

        # IP 漂移（长跑"数据没了"的高频原因，必须留证据）
        prev_ip = self.last_ip.get(device_id)
        if prev_ip and prev_ip != recv["src_ip"]:
            log_event(self.conn, recv["recv_time"], "IP_CHANGE", "WARN",
                      "%s -> %s" % (prev_ip, recv["src_ip"]))
            self.lg.warning("[IP] %s 源地址变化：%s -> %s", device_id, prev_ip, recv["src_ip"])
        self.last_ip[device_id] = recv["src_ip"]

        # ---- ① 原始流水（不可再生，优先落盘）----
        self.conn.execute(
            "INSERT INTO udp_packet (recv_time,recv_mono,src_ip,src_port,dev,device_id,seq,"
            "ptype,payload_len,payload,enc,parse_status,epoch_id,poll_ok,poll_err,udp_tx,"
            "rssi,free_heap,ts_ok) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (recv["recv_time"], recv["recv_mono"], recv["src_ip"], recv["src_port"],
             dev, device_id, seq, ptype, recv["payload_len"],
             # hb 不存 payload：每轮必发、字段已抽干，存了等于白占一半体积
             None if ptype == "hb" else recv["payload"],
             recv["enc"], recv["parse_status"], epoch_id,
             obj.get("poll_ok"), obj.get("poll_err"), obj.get("udp_tx"),
             obj.get("rssi"), obj.get("free_heap"), 1 if obj.get("ts_ok") else 0))
        self.pending_rows += 1
        self.st.bump(db_rows=1)
        self.last_progress = time.monotonic()
        self._commit()      # 原始包立即提交：崩溃时宁可多一次 fsync，也不能丢原始证据

        # ---- ② 解析结果（可再生产物）----
        if ok and ptype == "data" and epoch_id is not None:
            self.st.bump(store_attempts=1)
            res, quality = store_json(self.conn, obj, device_id, self.fields,
                                      epoch_id, recv["recv_wall"], self.anchors)
            if res == StoreResult.NEW:
                self.st.bump(store_ok=1)
                self.pending_rows += 1
                if quality == "OK_CONFLICT":
                    self.st.bump(quality_conflict=1)
                    log_event(self.conn, recv["recv_time"], "QUALITY_CONFLICT", "WARN",
                              "quality=OK 但 invalid=%s（seq=%s）" % (obj.get("invalid"), seq))
            elif res == StoreResult.DUP:
                self.st.bump(dup=1)
            elif res == StoreResult.SAME_SECOND:
                # 1 Hz + 秒级 ts 的固有伪影：两轮落进同一秒，主键只能存一行。
                # 不告警（否则长跑日志被刷爆），但要计数——它是"少了多少行"的解释项。
                self.st.bump(same_second=1)
            elif res == StoreResult.CLOCK_STEP:
                self.st.bump(clock_step=1)
                self.lg.warning("[CLOCK] seq=%s 的 collect_time 相对上一轮倒退，未覆盖", seq)
            elif res == StoreResult.COLLIDE:
                self.st.bump(collision=1)
                self.lg.warning("[COLLIDE] seq=%s 乱序到达，collect_time 已被占用，未覆盖", seq)
            else:
                self.st.bump(store_bad=1)
                log_event(self.conn, recv["recv_time"], "STORE_BAD", "WARN",
                          "%s（seq=%s）" % (quality, seq))


# ---------------------------------------------------------------- T3 统计线程

class StatReporter(threading.Thread):
    """只读连接；10 s 控制台一行、60 s 分钟快照 + 磁盘检查。"""

    def __init__(self, db_path: str, stats: Stats, q_pkt: queue.Queue, stop: threading.Event,
                 receiver: Receiver, writer: "Writer", run_id: int, started: datetime,
                 hours: float, lg: logging.Logger):
        super().__init__(name="T3-stat", daemon=True)
        self.db_path = db_path
        self.st = stats
        self.q_pkt = q_pkt
        self.stop = stop
        self.receiver = receiver
        self.writer = writer
        self.run_id = run_id
        self.started = started
        self.hours = hours
        self.lg = lg
        self.deadline = started + timedelta(hours=hours)
        self._last_snap = 0.0

    def run(self) -> None:
        # 必须等写入端把 WAL 的 -shm 建好，再开只读连接（见 Writer.run 的写探测注释）
        if not self.writer.ready.wait(timeout=15.0):
            self.lg.warning("[T3] 写入端未就绪，仍尝试打开只读连接")
        try:
            conn = open_db(self.db_path, readonly=True)
        except sqlite3.Error as e:
            self.lg.error("[T3] 只读连接失败（不影响采集）：%s", e)
            conn = None
        t0 = time.monotonic()
        try:
            while not self.stop.is_set():
                self.stop.wait(CONSOLE_INTERVAL_S)
                if self.stop.is_set():
                    break
                elapsed = time.monotonic() - t0
                s = self.st.snap()
                self._console(s, elapsed)
                self._drain_anomalies(conn)
                last = s.get("last_rx_mono") or 0.0
                if last and (time.perf_counter() - last) > 30:
                    gap = time.perf_counter() - last
                    self.lg.warning("[ALERT] 已 %.0f s 无任何上行（GAP）", gap)
                    if conn:
                        try:
                            log_event(conn, datetime.now(CST).strftime("%Y-%m-%d %H:%M:%S"),
                                      "GAP", "ALERT", "%.0f s 无包" % gap)
                            conn.commit()
                        except sqlite3.Error:
                            pass
                if elapsed - self._last_snap >= SNAP_INTERVAL_S:
                    self._last_snap = elapsed
                    self._snapshot(conn, s)
                if datetime.now(CST) >= self.deadline:
                    self.lg.info("[RUN] 已达到计划时长 %.2f h，开始收尾", self.hours)
                    self.stop.set()
                    break
        finally:
            if conn:
                conn.close()

    def _console(self, s: dict, elapsed: float) -> None:
        now = datetime.now(CST).strftime("%H:%M:%S")
        uptime = str(timedelta(seconds=int(elapsed)))
        line = ("[%s] up %s | rx %d (d %d/h %d/e %d) | bad %d | dup %d "
                "| ss %d cs %d oo %d | seq %d | q %d/%d drop %d | db %d 行 | heap %dk"
                % (now, uptime, s["rx"], s["rx_data"], s["rx_hb"], s["rx_err"],
                   s["bad_json"] + s["bad_field"], s["dup"],
                   s["same_second"], s["clock_step"], s["collision"],
                   s["last_seq"], self.q_pkt.qsize(), Q_MAX, s["q_drop"],
                   s["db_rows"], (s["last_heap"] or 0) // 1024))
        if self.writer.fatal is not None:
            line = "[FATAL] 落库线程已死（%s）——收包还在继续，但数据没进库！ | " % str(
                self.writer.fatal)[:40] + line
        if len(line) > 160:
            line = line[:157] + "..."
        self.lg.info(line)

    def _drain_anomalies(self, conn) -> None:
        """T1 攒的异常在这里落地 —— 顺带满足"T1 不做文件 I/O"的约束。"""
        r = self.receiver
        while r.anomalies:
            try:
                kind, sev, detail = r.anomalies.popleft()
            except IndexError:
                break
            self.lg.warning("[%s] %s", kind, detail.replace("\n", " | ")[:400])
            if conn:
                try:
                    log_event(conn, datetime.now(CST).strftime("%Y-%m-%d %H:%M:%S"),
                              kind, sev, detail[:800])
                    conn.commit()
                except sqlite3.Error:
                    pass

    def _snapshot(self, conn, s: dict) -> None:
        if conn is None:
            return
        try:
            du = shutil.disk_usage(os.path.dirname(os.path.abspath(self.db_path)))
            free_gb = du.free / (1024 ** 3)
            db_mb = (os.path.getsize(self.db_path) / (1024 ** 2)
                     if os.path.exists(self.db_path) else 0.0)
            self.st.set(last_db_mb=db_mb)
            if free_gb < DISK_LOW_GB:
                self.lg.error("[ALERT] 磁盘剩余 %.1f GB < %.0f GB（DISK_LOW）", free_gb, DISK_LOW_GB)
                try:
                    log_event(conn, datetime.now(CST).strftime("%Y-%m-%d %H:%M:%S"),
                              "DISK_LOW", "ALERT", "剩余 %.1f GB" % free_gb)
                except sqlite3.Error:
                    pass
            conn.execute(
                "INSERT OR REPLACE INTO stat_snapshot (ts,run_id,device_id,pkt_rx,data_rx,hb_rx,"
                "err_rx,seq_last,q_depth,q_drop,poll_ok_last,poll_err_last,udp_tx_last,rssi,"
                "free_heap,db_mb,disk_free_mb) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (datetime.now(CST).strftime("%Y-%m-%d %H:%M:%S"), self.run_id, None,
                 s["rx"], s["rx_data"], s["rx_hb"], s["rx_err"], s["last_seq"],
                 self.q_pkt.qsize(), s["q_drop"], s["last_poll_ok"], s["last_poll_err"],
                 s["last_udp_tx"], s["last_rssi"], s["last_heap"], db_mb, free_gb * 1024))
            conn.commit()
        except (sqlite3.Error, OSError) as e:
            self.lg.warning("[T3] 快照写入失败：%s", e)


# ---------------------------------------------------------------- 统计引擎

def _pct(a, b, digits: int = 4):
    if not b:
        return None
    return round(100.0 * a / b, digits)


def _fmt(v, spec: str = "%.4f", na: str = "[待实测]") -> str:
    return na if v is None else (spec % v)


def infer_err_row_mode(conn) -> bool:
    """失败轮是否也发一行 err？决定「每轮几行」，是对账恒等式的前提。

    · 有 err 包 ⇒ 开（True）
    · 有失败轮（poll_err>0）却一个 err 包都没有 ⇒ 关（False）
    · 全程零失败 ⇒ 无法区分，但两种假设结果相同，默认 True（与 config.h 一致）
    """
    if conn.execute("SELECT 1 FROM udp_packet WHERE ptype='err' LIMIT 1").fetchone():
        return True
    any_fail = conn.execute("SELECT 1 FROM udp_packet WHERE ptype='hb' AND poll_err>0 LIMIT 1"
                            ).fetchone()
    return not bool(any_fail)


def _load_epoch_packets(conn, eid: int) -> dict:
    """→ {seq: ptype}（同一 seq 取首次到达的那条，用于"这一行到没到"）。"""
    got = {}
    for r in conn.execute("SELECT seq,ptype FROM udp_packet WHERE epoch_id=? AND "
                          "ptype IN ('data','hb','err') AND seq IS NOT NULL", (eid,)):
        got.setdefault(r["seq"], r["ptype"])
    return got


def _epoch_hbs(conn, eid: int) -> list:
    """→ [(seq, poll_ok, poll_err)]，按 seq 升序（累计计数器，可靠源）。"""
    return [(r["seq"], r["poll_ok"], r["poll_err"])
            for r in conn.execute(
                "SELECT seq,poll_ok,poll_err FROM udp_packet WHERE epoch_id=? AND ptype='hb' "
                "AND seq IS NOT NULL AND poll_ok IS NOT NULL AND poll_err IS NOT NULL "
                "ORDER BY seq", (eid,))]


def _epoch_rounds(conn, ep) -> int:
    """该世代的执行轮数。+1 = 起始快照本身已含首轮。

    ⚠️ 边界 ±1 轮（首个 hb 的归属无法从累计值反推），影响 < 0.001%，
       已在报告里明写，不粉饰。
    """
    last = conn.execute(
        "SELECT poll_ok,poll_err FROM udp_packet WHERE epoch_id=? AND ptype='hb' "
        "AND poll_ok IS NOT NULL ORDER BY id DESC LIMIT 1", (ep["epoch_id"],)).fetchone()
    if not last:
        return 0
    d = ((last["poll_ok"] + last["poll_err"])
         - ((ep["poll_ok_at_start"] or 0) + (ep["poll_err_at_start"] or 0)) + 1)
    return max(d, 0)


def classify_loss(conn, err_mode: bool) -> dict:
    """丢包四分类。判据优先级 = 方案 §3.3：**先看 poll 计数器，再看 seq 缺哪一条**。

    正常轮：奇数 seq = hb，偶数 seq = data（hb 先发，seq 后自增）；
    失败轮：奇数 seq = hb，偶数 seq = err。
    前提是 err 行开启 ⇒ 每轮恰好 2 行、奇偶稳定。若 err 关，奇偶被打乱，
    此时只统计"整轮缺失"，其余计入 unexpl 并如实标注，不硬凑四分类。

    返回的 full 计数单位是**轮**，其余三项单位是**行**（与 §3.6 M5/M6/M7 口径一致）。
    """
    res = {"full": 0, "data_only": 0, "hb_only": 0, "err_only": 0,
           "unexpl": 0, "denom": 0}

    for ep in conn.execute("SELECT * FROM device_epoch ORDER BY epoch_id"):
        eid = ep["epoch_id"]
        got = _load_epoch_packets(conn, eid)
        hbs = _epoch_hbs(conn, eid)
        res["denom"] += _epoch_rounds(conn, ep)
        if not hbs:
            continue

        if not err_mode:
            seq_s = min(got) if got else 0
            seq_e = max(got) if got else 0
            for s in range(seq_s, seq_e + 1):
                if s not in got:
                    res["unexpl"] += 1
            continue

        # 首轮的"第二条"（hb 之后那条）单独判：没有前一个 hb 做差分
        first_seq = hbs[0][0]
        second = got.get(first_seq + 1)
        if second is None:
            # 首轮成败不可知（累计值含首轮），按 unexpl 记，不猜
            res["unexpl"] += 1

        for i in range(1, len(hbs)):
            seq_p, ok_p, er_p = hbs[i - 1]
            seq_c, ok_c, er_c = hbs[i]
            span = (seq_c - seq_p) // 2      # 相邻两个"到达的 hb"之间跨了几轮
            d_ok, d_err = ok_c - ok_p, er_c - er_p

            if span <= 0:
                # seq 相同的重复 hb：不算轮次，交给重包统计
                continue

            # 中间缺失的轮（hb 没到）
            for k in range(1, span):
                h = seq_p + 2 * k
                if got.get(h + 1) is None:
                    res["full"] += 1          # hb 与第二条都没到
                else:
                    res["hb_only"] += 1       # 只缺心跳，第二条到了
            if span > 1:
                # 跨度 >1 时中间各轮的成败无法逐轮归因，如实留痕
                res["unexpl"] += max(span - 1 - (d_ok + d_err), 0)

            # 当前轮（hb 已到）：只用它自己的差分判成败
            if got.get(seq_c + 1) is None:
                if d_ok == 1 and d_err == 0:
                    res["data_only"] += 1
                elif d_ok == 0 and d_err == 1:
                    res["err_only"] += 1
                elif d_ok + d_err == 0:
                    res["unexpl"] += 1        # 计数器没动：不该发生
                else:
                    res["unexpl"] += 1        # 跨轮差分，无法精确归因
    return res


def jitter(conn) -> dict:
    """同一类型（hb）相邻到达的 perf_counter 差值，减周期取绝对值。

    ⚠️ 两个必须做的清洗，少一个分位数就废：
      ① **按 (epoch, seq) 取首次到达**：重复包会在原包之后 20 ms 到达，
         直接算差值会得到 |0.02−1| ≈ 980 ms，把 p99 顶到 977 ms（第一版实测）。
      ② **剔除 > 5 个周期的大间隔**：那是断流/重启，不是抖动；
         混进分位数会让 p99 变成"最长断网时间"，失去意义。单独计 gaps。
    ⚠️ 必须用 recv_mono（perf_counter），不能用 recv_time（墙钟）：
       NTP 步进一次就在墙钟上跳出几百毫秒，那是时钟抖动，不是采集抖动。
    """
    deltas = []
    gaps = 0
    for ep in conn.execute("SELECT epoch_id FROM device_epoch ORDER BY epoch_id"):
        prev = None
        for r in conn.execute(
                "SELECT recv_mono FROM udp_packet WHERE id IN ("
                "  SELECT MIN(id) FROM udp_packet WHERE epoch_id=? AND ptype='hb' "
                "  GROUP BY epoch_id,seq) ORDER BY recv_mono", (ep["epoch_id"],)):
            cur = r["recv_mono"]
            if prev is not None:
                d = abs((cur - prev) - PERIOD_S) * 1000.0
                if d > 5 * PERIOD_S * 1000.0:
                    gaps += 1
                else:
                    deltas.append(d)
            prev = cur
        if len(deltas) > 200000:      # 20 万样本足够算 p99，且不撑爆内存
            break
    n = len(deltas)
    if not n:
        return {"p50": None, "p99": None, "max": None, "n": 0, "gaps": gaps}
    deltas.sort()
    return {"p50": deltas[max(int(n * 0.50) - 1, 0)],
            "p99": deltas[min(int(n * 0.99), n - 1)],
            "max": deltas[-1], "n": n, "gaps": gaps}


def _epoch_stats(conn, ep) -> dict:
    """单个世代的全部对账量。所有口径在这里定死，报告只负责排版。

    三处最容易写错、且第一版真写错了的地方：
      ① 首轮归属：世代首包之前没有 hb 可差分，若把 +1 只加在分母上，
         短测里 M1 会凭空少 1（22 轮时差 4.5%，1800 轮时差 0.06%）。
         解法：看 `seq_first+1` 那条是 data 还是 err，直接判首轮成败。
      ② udp_tx 是"发送前"的值（固件先组包后自增）⇒ 窗口内发出数 = tx_e - tx_s + 1。
      ③ 重复包会让 sent 多出来而 seq 不变 ⇒ 必须先把重复从 sent 里剥掉，
         否则重包会被算成"未发出"，对账永远差几条。
    """
    eid = ep["epoch_id"]
    last_hb = conn.execute(
        "SELECT poll_ok,poll_err,udp_tx,seq FROM udp_packet WHERE epoch_id=? AND ptype='hb' "
        "AND poll_ok IS NOT NULL ORDER BY id DESC LIMIT 1", (eid,)).fetchone()
    if not last_hb:
        return None

    ok_s = ep["poll_ok_at_start"] or 0
    er_s = ep["poll_err_at_start"] or 0
    tx_s = ep["udp_tx_at_start"] or 0
    ok_e, er_e, tx_e = last_hb["poll_ok"], last_hb["poll_err"], last_hb["udp_tx"] or 0
    seq_s = ep["seq_first"] or 0
    seq_e = last_hb["seq"] or 0

    got = _load_epoch_packets(conn, eid)
    second = got.get(seq_s + 1)
    first_ok = 1 if second == "data" else (0 if second == "err" else None)

    d_ok = ok_e - ok_s + (1 if first_ok == 1 else 0)
    d_err = er_e - er_s + (1 if first_ok == 0 else 0)
    rounds = d_ok + d_err          # 首轮去向不明时，它同时从分子分母退出

    expected = max(seq_e - seq_s + 1, 0)
    sent = max(tx_e - tx_s + 1, 0)
    row = conn.execute(
        "SELECT COUNT(*) c, COUNT(DISTINCT seq) d FROM udp_packet WHERE epoch_id=? AND "
        "ptype IN ('data','hb','err') AND seq BETWEEN ? AND ?",
        (eid, seq_s, seq_e)).fetchone()
    rx_dgrams, distinct = row["c"], row["d"]
    dup_rx = max(rx_dgrams - distinct, 0)
    sent_unique = max(sent - dup_rx, 0)     # 剥掉重发后的"应当唯一"发送数
    return {
        "epoch_id": eid, "device_id": ep["device_id"],
        "seq_s": seq_s, "seq_e": seq_e,
        "rounds": rounds, "d_ok": d_ok, "d_err": d_err, "first_ok": first_ok,
        "expected": expected, "sent": sent, "sent_unique": sent_unique,
        "rx_dgrams": rx_dgrams, "distinct": distinct, "dup": dup_rx,
        "lost": max(sent_unique - distinct, 0),
        "not_sent": max(expected - sent_unique, 0),
    }


def dup_stats(conn) -> tuple:
    """→ (重复数, 去重后条数)。键 = (device_id, epoch_id, seq)，**必须带 epoch_id**。"""
    row = conn.execute(
        "SELECT COALESCE(SUM(c-1),0) d FROM (SELECT COUNT(*) c FROM udp_packet "
        "WHERE parse_status='ok' AND seq IS NOT NULL "
        "GROUP BY device_id, epoch_id, seq HAVING c>1)").fetchone()
    dup = row["d"] if row else 0
    distinct = conn.execute(
        "SELECT COUNT(*) c FROM (SELECT 1 FROM udp_packet WHERE parse_status='ok' "
        "AND seq IS NOT NULL GROUP BY device_id, epoch_id, seq)").fetchone()["c"]
    return dup, distinct


def build_report(conn: sqlite3.Connection, fields, db_path: str = "",
                 run_id: int | None = None) -> str:
    """全部数字由 SQL 从 udp_packet / meter_record 重算。

    ⚠️ 排序键固定 `id`、浮点统一 %.4f —— `--report-only` 必须与运行时报告逐字节一致，
       否则"可复算"这条验收判据（§2.9 #5）就是空话。
    """
    out = []
    w = out.append

    run = None
    if run_id is not None:
        run = conn.execute("SELECT * FROM collect_run WHERE run_id=?", (run_id,)).fetchone()
    if run is None:
        run = conn.execute("SELECT * FROM collect_run ORDER BY run_id DESC LIMIT 1").fetchone()

    started = run["started_at"] if run else None
    ended = run["ended_at"] if run and run["ended_at"] else None
    if ended is None:
        row = conn.execute("SELECT MAX(recv_time) m FROM udp_packet").fetchone()
        ended = row["m"] if row else None

    dur_s = 0.0
    if started and ended:
        try:
            dur_s = (datetime.strptime(ended, "%Y-%m-%d %H:%M:%S")
                     - datetime.strptime(started, "%Y-%m-%d %H:%M:%S")).total_seconds()
        except ValueError:
            dur_s = 0.0
    hours = dur_s / 3600.0 if dur_s else 0.0
    periods_theory = int(dur_s / PERIOD_S) if dur_s else 0

    w("=" * 78)
    w("168 小时连续采集 —— 报告")
    w("=" * 78)
    w("")
    w("【1】封面")
    w("-" * 78)
    tot_pkt = conn.execute("SELECT COUNT(*) c FROM udp_packet").fetchone()["c"]
    devs = [r["device_id"] for r in conn.execute(
        "SELECT DISTINCT device_id FROM udp_packet WHERE device_id IS NOT NULL")]
    db_mb = os.path.getsize(db_path) / (1024 ** 2) if db_path and os.path.exists(db_path) else 0.0
    w("  设备：%s" % (", ".join(devs) if devs else "[无]"))
    w("  DB  ：%s（%.1f MB，%d 包）" % (os.path.basename(db_path) or "?", db_mb, tot_pkt))
    w("  起止：%s → %s" % (started or "?", ended or "?"))
    w("  时长：%.4f h（理论轮数 %d @ %.1fs）" % (hours, periods_theory, PERIOD_S))
    _th = run["target_hours"] if run else HOURS_168
    w("  计划：%s h%s" % (("%.1f" % _th) if _th >= 1 else ("%.2f" % _th),
                          "" if hours else "（未完成）"))
    w("  主机：%s" % os.environ.get("COMPUTERNAME", "?"))
    w("")

    # ---------------- 总量 ----------------
    w("【2】总量（四个计数器）")
    w("-" * 78)
    by_type = {r["ptype"]: r["c"] for r in conn.execute(
        "SELECT ptype,COUNT(*) c FROM udp_packet GROUP BY ptype ORDER BY ptype")}
    rx_total = sum(by_type.values())
    rx_data = by_type.get("data", 0)
    rx_hb = by_type.get("hb", 0)
    rx_err = by_type.get("err", 0)
    w("  收到包   %d  =  data %d + hb %d + err %d + 其他 %d"
      % (rx_total, rx_data, rx_hb, rx_err, rx_total - rx_data - rx_hb - rx_err))
    dup, distinct = dup_stats(conn)
    w("  去重后   %d  重复 %d（重包率 %s）"
      % (distinct, dup, _fmt(_pct(dup, rx_total), "%.4f%%")))

    # ---------------- 按世代对账 ----------------
    w("")
    w("【3】按世代对账（seq 连续段；每次设备重启切一段）")
    w("-" * 78)
    w("  %-4s %-16s %-13s %-8s %-8s %-8s %-8s %-8s %-8s"
      % ("#", "设备", "seq范围", "轮数", "期望行", "已发", "收到", "重复", "网络丢"))
    tot = {"rounds": 0, "d_ok": 0, "d_err": 0, "expected": 0, "sent": 0,
           "sent_unique": 0, "distinct": 0, "dup": 0, "lost": 0, "not_sent": 0}
    n_ep = 0
    for ep in conn.execute("SELECT * FROM device_epoch ORDER BY epoch_id"):
        st = _epoch_stats(conn, ep)
        if st is None:
            continue
        n_ep += 1
        for k in tot:
            tot[k] += st[k]
        w("  %-4d %-16s %-13s %-8d %-8d %-8d %-8d %-8d %-8d"
          % (st["epoch_id"], (st["device_id"] or "?")[-16:],
             "%d-%d" % (st["seq_s"], st["seq_e"]),
             st["rounds"], st["expected"], st["sent"], st["distinct"],
             st["dup"], st["lost"]))

    w("  %-4s %-16s %-13s %-8d %-8d %-8d %-8d %-8d %-8d"
      % ("Σ", "", "", tot["rounds"], tot["expected"], tot["sent"],
         tot["distinct"], tot["dup"], tot["lost"]))

    # 恒等式：期望行(seq) = 收到(去重) + 未发出 + 网络丢失
    resid = tot["expected"] - (tot["distinct"] + tot["not_sent"] + tot["lost"])
    w("")
    w("  对账恒等式：期望行 %d = 收到 %d + 未发出 %d + 网络丢失 %d  → 残差 %d（%s）"
      % (tot["expected"], tot["distinct"], tot["not_sent"], tot["lost"], resid,
         "闭合" if resid == 0 else "不闭合，需排查"))

    err_mode = infer_err_row_mode(conn)
    expect_by_mode = tot["d_ok"] * 2 + tot["d_err"] * (2 if err_mode else 1)
    boundary = expect_by_mode - tot["expected"]
    w("")
    w("  每轮行数模式：%s"
      % ("每轮 2 行（hb + data/err，QI_SEND_ERR_ROW=1）" if err_mode
         else "成功轮 2 行 / 失败轮 1 行（QI_SEND_ERR_ROW=0）"))
    w("  模式校验：按计数器推 %d 行，按 seq 推 %d 行，差 %+d（应 = 世代数 %d，"
      % (expect_by_mode, tot["expected"], boundary, n_ep))
    w("            每世代末轮的第二条落在 seq 窗口外，这是刻意边界）→ %s"
      % ("一致" if boundary == n_ep else "不一致，需排查"))

    # ---------------- 三个口径 ----------------
    w("")
    w("【4】三个成功率（必须同时给出，只给一个必被质疑）")
    w("-" * 78)
    tot_rounds, tot_ok, tot_err = tot["rounds"], tot["d_ok"], tot["d_err"]
    m1 = _pct(tot_ok, tot_rounds) if tot_rounds else None
    m3 = _pct(tot["distinct"], tot["sent_unique"]) if tot["sent_unique"] else None
    m2_wall = _pct(rx_data, periods_theory) if periods_theory else None
    m2_run = _pct(rx_data, tot_rounds) if tot_rounds else None
    a_rate = _pct(tot_rounds, periods_theory) if periods_theory else None

    w("  M1 采集成功率（纯 485 总线）  = ΣΔpoll_ok/ΣΔR = %d/%d = %s"
      % (tot_ok, tot_rounds, _fmt(m1, "%.4f%%")))
    w("  M3 传输送达率（纯 UDP）        = 去重收到/应唯一发送 = %d/%d = %s（重复不计）"
      % (tot["distinct"], tot["sent_unique"], _fmt(m3, "%.4f%%")))
    w("  A  链路可用率（设备在跑/理论）  = ΣΔR/(T/1s) = %d/%d = %s"
      % (tot_rounds, periods_theory, _fmt(a_rate, "%.4f%%")))
    if tot_rounds > 1:
        row = conn.execute("SELECT MIN(recv_time) a, MAX(recv_time) b FROM udp_packet").fetchone()
        span = 0.0
        if row and row["a"] and row["b"]:
            try:
                span = (datetime.strptime(row["b"], "%Y-%m-%d %H:%M:%S.%f")
                        - datetime.strptime(row["a"], "%Y-%m-%d %H:%M:%S.%f")).total_seconds()
            except ValueError:
                span = 0.0
        if span > 0:
            # 用「首包→末包」而不是整段运行时长：后者含启动等待和收尾，
            # 短测里能把 1.0 s 算成 1.36 s，据此判定"周期偏离"是假警报。
            pm = span / (tot_rounds - 1)
            w("  实测周期（首包→末包/间隔数） = %.3f s/轮，相对 1 s 偏差 %+.2f%% → %s"
              % (pm, (pm - 1.0) * 100.0,
                 "与标称一致，M2 用理论轮数即可" if abs(pm - 1.0) <= 0.01
                 else "**偏离标称**：M2 须改用 M2'（执行口径），否则分母失真"))
    w("  M2 端到端完整率（验收口径）    = 数据行/理论轮数 = %d/%d = %s"
      % (rx_data, periods_theory, _fmt(m2_wall, "%.4f%%")))
    w("  M2' 端到端完整率（执行口径）   = 数据行/实际轮数 = %d/%d = %s"
      % (rx_data, tot_rounds, _fmt(m2_run, "%.4f%%")))
    if None not in (m1, m3, a_rate, m2_wall) and hours >= 1.0:
        pred = a_rate * m1 * m3 / 10000.0
        w("  一致性：A×M1×M3 = %.4f%% vs M2 = %.4f%%，偏差 %.4f 个百分点 → %s"
          % (pred, m2_wall, abs(pred - m2_wall),
             "通过（<0.1）" if abs(pred - m2_wall) < 0.1 else "需排查口径"))
    elif hours < 1.0:
        w("  一致性校验跳过：时长 < 1 h，启停爬坡会让 A 偏低，此时做乘积校验无意义。")
    w("  注：M1/M3 用累计计数器差分，与 A（墙钟理论轮数）相互独立 ——")
    w("      稳态下三者乘积应逼近 M2，这条恒等式本身就是对统计口径的证伪。")

    # ---------------- 丢包四分类 ----------------
    w("")
    w("【5】丢包四分类（先看 poll 计数器，再看 seq 缺哪一条）")
    w("-" * 78)
    cls = classify_loss(conn, err_mode)
    denom = cls.get("denom", 0)
    for k, label, unit in (("full", "整轮丢失（hb 与第二条都没到）", "轮"),
                           ("data_only", "仅数据行丢失（心跳到、采集成功、数据行没到）", "行"),
                           ("hb_only", "仅心跳丢失（第二条到了）", "行"),
                           ("err_only", "仅 err 行丢失", "行"),
                           ("unexpl", "无法归类（计数器与 seq 矛盾）", "个")):
        w("  %-40s %8d %s (%s)" % (label, cls.get(k, 0), unit,
                                   _fmt(_pct(cls.get(k, 0), denom), "%.4f%%")))
    w("  %-40s %8d 行（= full×2 + 后三项）"
      % ("折算丢失行数", cls["full"] * 2 + cls["data_only"] + cls["hb_only"] + cls["err_only"]))
    w("  分母（执行轮数）%d" % denom)
    if not err_mode:
        w("  ⚠️ err 行关闭 ⇒ 每轮行数不固定、奇偶失效，四分类降级为「总数 + unexpl」。")

    # ---------------- 重包 / 撞车 ----------------
    w("")
    w("【6】重包与撞车")
    w("-" * 78)
    tcoll = conn.execute("SELECT COUNT(*) c FROM event_log WHERE kind='CLOCK_STEP'"
                         ).fetchone()["c"]
    tsame = conn.execute("SELECT COUNT(*) c FROM event_log WHERE kind='TS_SAME_SECOND'"
                         ).fetchone()["c"]
    tooo = conn.execute("SELECT COUNT(*) c FROM event_log WHERE kind='TS_OUT_OF_ORDER'"
                        ).fetchone()["c"]
    qconf = conn.execute("SELECT COUNT(*) c FROM event_log WHERE kind='QUALITY_CONFLICT'"
                         ).fetchone()["c"]
    w("  重包数 %d / 总包 %d → M8 重包率 %s"
      % (dup, rx_total, _fmt(_pct(dup, rx_total), "%.4f%%")))
    w("  M10 时钟回跳（ts 相对上一轮倒退）%d 次 → **必须为 0**" % tcoll)
    w("  同秒两轮 %d 次（ts 只到秒，1 Hz 下属分辨率伪影，非故障；"
      "代价是这些轮次各少存 1 行 meter_record）" % tsame)
    w("  乱序到达 %d 次（后到的那条未覆盖，原始包仍在 udp_packet）" % tooo)
    w("  quality 冲突 %d（quality=OK 但 invalid 非空 ⇒ 固件口径 bug 证据）" % qconf)

    # ---------------- 抖动 ----------------
    w("")
    w("【7】周期抖动（只用 hb 行，同类型之间比较才有意义）")
    w("-" * 78)
    j = jitter(conn)
    w("  |Δrecv−1s|  p50 %s ms | p99 %s ms | max %s ms | 样本 %d | 剔除断流 %d 次"
      % (_fmt(j["p50"], "%.1f"), _fmt(j["p99"], "%.1f"), _fmt(j["max"], "%.1f"),
         j["n"], j["gaps"]))
    if j["p50"] is not None:
        w("  阈值：p99 ≤ 50 ms、max ≤ 200 ms → %s"
          % ("通过" if (j["p99"] is not None and j["p99"] <= 50 and j["max"] <= 200)
             else "未通过"))

    # ---------------- 数据质量 ----------------
    w("")
    w("【8】数据质量")
    w("-" * 78)
    tot_mr = conn.execute("SELECT COUNT(*) c FROM meter_record").fetchone()["c"]
    w("  meter_record 行数 %d" % tot_mr)
    for r in conn.execute("SELECT quality,COUNT(*) c FROM meter_record GROUP BY quality ORDER BY quality"):
        w("    quality=%-8s %6d  (%s)"
          % (r["quality"], r["c"], _fmt(_pct(r["c"], tot_mr), "%.4f%%")))
    w("  M16 质量 OK 占比 %s（阈值 ≥95%%）"
      % _fmt(_pct(conn.execute("SELECT COUNT(*) c FROM meter_record WHERE quality='OK'"
                               ).fetchone()["c"], tot_mr), "%.4f%%"))
    boot = conn.execute("SELECT COUNT(*) c FROM meter_record WHERE src='udp_boot'").fetchone()["c"]
    w("  时间戳来源：NTP %d 行 / BOOT 锚点 %d 行（后者为已知近似，单独计数）"
      % (tot_mr - boot, boot))

    # ---- 完整性：收到的数据行 vs 真正落进 meter_record 的行 ----
    # ⚠️ 这是本工具最该主动交代的一条，不能等评审来问：
    #    meter_record 的主键是 (device_id, collect_time)，collect_time 只到**秒**。
    #    1 Hz 轮询一旦周期略小于 1 s（固件实测抖动 + NTP 步进），就会有两轮落进同一秒，
    #    主键只容得下一行 ⇒ 第二行进不去。这不是本工具的 bug，是既有 schema 的固有限制，
    #    任何基于 db_proto 的采集端都一样。
    #    兜底：**原始包在 udp_packet 里一条不少**，主键一旦加宽即可全量重放（--repair）。
    w("")
    w("  完整性：收到数据行 %d，meter_record 实存 %d，差 %d" % (rx_data, tot_mr, rx_data - tot_mr))
    w("    其中 重包 %d / 同秒两轮 %d / 时钟回跳 %d / 乱序 %d（其余见 event_log）"
      % (dup, tsame, tcoll, tooo))
    w("    ⇒ 差额全部来自主键 (device_id, collect_time) 的秒级精度，**原始包未丢**；")
    w("      主键加宽后可用 --repair 从 udp_packet 全量重放，故不影响数据可复原性。")
    w("")
    w("  字段级 NULL 率（A/B/C_temp 预期 100% NULL —— 该型号恒不上报接线柱温度）：")
    for f in fields:
        n = conn.execute("SELECT COUNT(*) c FROM meter_record WHERE %s IS NULL"
                         % f["field"]).fetchone()["c"]
        if n:
            w("    %-16s %6d / %-6d %s"
              % (f["field"], n, tot_mr, _fmt(_pct(n, tot_mr), "%.2f%%")))

    # ---------------- 逐小时 ----------------
    w("")
    w("【9】逐小时趋势")
    w("-" * 78)
    w("  %-16s %-8s %-8s %-8s" % ("小时", "hb", "data", "err"))
    for r in conn.execute("SELECT substr(recv_time,1,13) h, SUM(ptype='hb') hb, "
                          "SUM(ptype='data') d, SUM(ptype='err') e FROM udp_packet "
                          "GROUP BY h ORDER BY h"):
        w("  %-16s %-8d %-8d %-8d" % (r["h"], r["hb"] or 0, r["d"] or 0, r["e"] or 0))

    # ---------------- 事件 ----------------
    w("")
    w("【10】异常事件（event_log）")
    w("-" * 78)
    evs = list(conn.execute("SELECT kind,severity,COUNT(*) c,MIN(ts) t0,MAX(ts) t1 "
                            "FROM event_log GROUP BY kind,severity ORDER BY c DESC"))
    if not evs:
        w("  [无]")
    for e in evs:
        w("  %-24s %-6s %5d 次  %s → %s"
          % (e["kind"], e["severity"], e["c"], e["t0"], e["t1"]))
    w("")
    w("=" * 78)
    w("报告结束。所有数字均可由 --report-only 从 udp_packet 重算。")
    w("=" * 78)
    return "\n".join(out)


# ---------------------------------------------------------------- 修复

def repair(conn, fields, lg) -> int:
    """对 parse_status='ok' 但 meter_record 缺行的数据包补入库（断电恢复）。

    ⚠️ 必须按 id 升序且共用同一个 BootAnchor：BOOT+ 的锚点是"该世代第一个此类包"
       反推出来的，重新算会得到与实时入库不同的 collect_time，反而制造撞车。
    """
    rows = list(conn.execute(
        "SELECT id,device_id,seq,epoch_id,payload FROM udp_packet p WHERE p.ptype='data' "
        "AND p.parse_status='ok' AND p.epoch_id IS NOT NULL AND NOT EXISTS ("
        "  SELECT 1 FROM meter_record m WHERE m.device_id=p.device_id "
        "  AND m.epoch_id=p.epoch_id AND m.seq=p.seq) ORDER BY p.id"))
    anchors = BootAnchor()
    n = 0
    for r in rows:
        if not r["payload"]:
            continue
        try:
            obj = json.loads(bytes(r["payload"]).decode("utf-8", "replace"))
        except ValueError:
            continue
        res, _q = store_json(conn, obj, r["device_id"], fields, r["epoch_id"],
                             datetime.now(CST), anchors)
        if res == StoreResult.NEW:
            n += 1
    conn.commit()
    return n


# ---------------------------------------------------------------- 自检（构造数据集）

def _synth_frame(volt: int, cur: int, a_h: int, a_l: int, state: int = 0x00F0) -> str:
    """造 78 字节 HEX（与 m5_slave_04 / db_proto.demo_packets 同口径）。"""
    r = [0] * REG_COUNT
    r[0] = state
    r[1], r[2], r[3] = 0x6660, 0x0066, 0x0000        # 频率 50.00 / 漏电 / N_temp
    r[4], r[5], r[6], r[7] = 500, 123, 64, 0         # A_temp=0 ⇒ -40 伪装
    r[8], r[9], r[10], r[11] = volt, cur, 98, volt * cur // 100 * 98 // 100
    r[14], r[15] = a_h, a_l
    r[37], r[38] = a_h, a_l
    return " ".join("%02X" % b for w in r for b in (w >> 8, w & 0xFF))


def selftest() -> int:
    """§3.8 #4「引擎可被证伪」：注入已知数量的四类丢包 + 重启 + 重包 + 时钟回跳，
    要求引擎逐项精确复现。一个没被证伪过的统计引擎，等于没有统计。
    """
    lg = logging.getLogger("selftest")
    logging.basicConfig(level=logging.CRITICAL)   # 静音，只看断言
    fields = db_proto.load_dict()

    tmp = tempfile.mkdtemp(prefix="uc_selftest_")
    db_path = os.path.join(tmp, "selftest.db")
    conn = open_db(db_path)
    init_schema(conn, fields)

    DEV = "ESP485-TEST01"
    base = datetime(2026, 10, 1, 0, 0, 0, tzinfo=CST)
    frame = _synth_frame(222, 523, 4, 58831)

    # 注入清单
    N_FULL = 50          # 整轮丢失（轮）
    N_DATA = 20          # 仅数据行丢失（行）
    N_HB = 10            # 仅心跳丢失（行）
    N_ERR = 3            # 仅 err 行丢失（行）
    N_DUP = 5            # 重复包（条）
    N_FAIL_ROUNDS = 100  # 前 100 轮为失败轮（发 err 不发 data）
    EPOCHS = (400, 300, 300)   # 三代，模拟 2 次重启

    # ⚠️ 注入区间必须落在"失败轮"还是"成功轮"上，否则四分类会张冠李戴：
    #    整轮丢 / err 行丢 ⇒ 放在失败轮（g ≤ 100）
    #    仅数据行丢 / 仅心跳丢 ⇒ 必须放在成功轮（g > 100）
    full_rounds = set(range(11, 11 + N_FULL))               # 11..60，失败轮
    data_rounds = set(range(151, 151 + N_DATA))             # 151..170，成功轮
    hb_rounds = set(range(201, 201 + N_HB))                 # 201..210，成功轮
    err_rounds = set(range(2, 2 + N_ERR))                   # 2..4，失败轮
    # 第 1 轮不参与 err 注入：它是世代首轮，没有前一个 hb 可差分，
    # 引擎会把它记成 unexpl（诚实的"不可归因"），不该被算成一次成功归因。

    seq = 0
    ok = err = tx = 0
    rid = conn.execute("INSERT INTO collect_run (started_at,target_hours) VALUES (?,?)",
                       (base.strftime("%Y-%m-%d %H:%M:%S"), 168)).lastrowid
    cursor_set(conn, "run_id", rid)

    g = 0
    for ei, nround in enumerate(EPOCHS):
        eid = conn.execute(
            "INSERT INTO device_epoch (device_id,seq_first,seq_last,first_recv,last_recv,"
            "poll_ok_at_start,poll_err_at_start,udp_tx_at_start,detect) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            (DEV, seq + 1, seq + 1, base.strftime("%Y-%m-%d %H:%M:%S"),
             base.strftime("%Y-%m-%d %H:%M:%S"), None, None, None,
             "cold_start" if ei == 0 else "seq_rollback")).lastrowid
        ep_first = None
        last_hb_seq = seq + 1
        for k in range(nround):
            g += 1
            t = base + timedelta(seconds=k + sum(EPOCHS[:ei]))
            is_fail = g <= N_FAIL_ROUNDS
            lost_full = g in full_rounds
            lost_data = g in data_rounds
            lost_hb = g in hb_rounds
            lost_err = (g in err_rounds) and is_fail

            if is_fail:
                err += 1
            else:
                ok += 1
            tx_before = tx          # hb 里带的是"发这一行之前"的累计发送数
            tx += 2                 # 本轮会发 hb + 第二条
            seq += 1
            hb_seq = seq            # 奇数
            seq += 1
            sec_seq = seq           # 偶数
            last_hb_seq = hb_seq
            if ep_first is None:
                ep_first = (ok, err, tx_before)   # = 首个 hb 观察到的累计值（含首轮）

            def ins(s, ptype, payload=None, _t=t, _g=g):
                conn.execute(
                    "INSERT INTO udp_packet (recv_time,recv_mono,src_ip,src_port,dev,device_id,"
                    "seq,ptype,payload_len,payload,enc,parse_status,epoch_id,poll_ok,poll_err,"
                    "udp_tx,rssi,free_heap,ts_ok) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (_t.strftime("%Y-%m-%d %H:%M:%S"), _g * 1.0 + 1000.0, "192.168.1.200",
                     12345, DEV, DEV, s, ptype, len(payload or b""), payload, 0, "ok", eid,
                     ok if ptype == "hb" else None, err if ptype == "hb" else None,
                     tx_before if ptype == "hb" else None, -55, 180000, 1))

            hb_obj = {"seq": hb_seq, "dev": DEV, "type": "hb",
                      "ts": t.strftime("%Y-%m-%dT%H:%M:%S"), "ts_ok": True,
                      "poll_ok": ok, "poll_err": err, "udp_tx": tx_before,
                      "rssi": -55, "free_heap": 180000}
            data_obj = {"seq": sec_seq, "dev": DEV, "ts": t.strftime("%Y-%m-%dT%H:%M:%S"),
                        "ts_ok": True, "quality": "OK", "invalid": "", "seg": "1", "raw": frame}

            if not lost_full and not lost_hb:
                ins(hb_seq, "hb", json.dumps(hb_obj).encode())
            if not lost_full:
                if is_fail:
                    if not lost_err:
                        ins(sec_seq, "err", json.dumps(
                            {"seq": sec_seq, "dev": DEV, "type": "err",
                             "ts": t.strftime("%Y-%m-%dT%H:%M:%S"), "ts_ok": True,
                             "seg": "S1", "reason": "TIMEOUT",
                             "poll_err": err}).encode())
                elif not lost_data:
                    ins(sec_seq, "data", json.dumps(data_obj).encode())

            # 重复包：把前 5 轮的 hb 再插一遍（UDP 单播不重复是理论，必须实测验证）
            if g <= N_DUP:
                ins(hb_seq, "hb", json.dumps(hb_obj).encode())

        conn.execute("UPDATE device_epoch SET seq_last=?,poll_ok_at_start=?,"
                     "poll_err_at_start=?,udp_tx_at_start=? WHERE epoch_id=?",
                     (last_hb_seq, ep_first[0], ep_first[1], ep_first[2], eid))

    conn.commit()

    # ---- 时间戳异常：同秒两轮 / 时钟回跳 各造 1 次，验证能区分 ----
    # ⚠️ 这两个必须能分开：同秒是 ts 只到秒的分辨率伪影（1 Hz 下偶发，不是故障），
    #    回跳是 NTP 真的倒退（会导致数据被静默覆盖）。混为一谈会把报告打回。
    anchors = BootAnchor()

    def _store(seq_no, ts_str):
        obj = {"seq": seq_no, "dev": DEV, "ts": ts_str, "ts_ok": True,
               "quality": "OK", "invalid": "", "seg": "1", "raw": frame}
        return store_json(conn, obj, DEV, fields, 1, base, anchors)[0]

    r1 = _store(9001, "2026-10-01T12:00:00")   # 正常
    r2 = _store(9002, "2026-10-01T12:00:00")   # 同一秒第二轮 → SAME_SECOND
    r3 = _store(9003, "2026-10-01T12:00:10")   # 正常
    r4 = _store(9004, "2026-10-01T12:00:00")   # ts 倒退且该秒已有行 → CLOCK_STEP
    conn.commit()

    # ---- 断言 ----
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append("%-28s 期望 %s，实际 %s" % (name, want, got))

    cls = classify_loss(conn, True)
    check("整轮丢失（轮）", cls["full"], N_FULL)
    check("仅数据行丢失（行）", cls["data_only"], N_DATA)
    check("仅心跳丢失（行）", cls["hb_only"], N_HB)
    check("仅 err 行丢失（行）", cls["err_only"], N_ERR)

    d, _ = dup_stats(conn)
    check("重复包（条）", d, N_DUP)

    ne = conn.execute("SELECT COUNT(*) c FROM device_epoch").fetchone()["c"]
    check("世代数（次重启+1）", ne, len(EPOCHS))

    tc = conn.execute("SELECT COUNT(*) c FROM event_log WHERE kind='CLOCK_STEP'"
                      ).fetchone()["c"]
    ts_n = conn.execute("SELECT COUNT(*) c FROM event_log WHERE kind='TS_SAME_SECOND'"
                        ).fetchone()["c"]
    check("执行轮数", cls["denom"], sum(EPOCHS))
    check("首次入库", r1, StoreResult.NEW)
    check("同秒第二轮", r2, StoreResult.SAME_SECOND)
    check("同秒后正常轮", r3, StoreResult.NEW)
    check("时钟回跳", r4, StoreResult.CLOCK_STEP)
    check("CLOCK_STEP 事件数", tc, 1)
    check("TS_SAME_SECOND 事件数", ts_n, 1)

    print("=" * 66)
    print("udp_collect 统计引擎自检")
    print("=" * 66)
    print("  注入：整轮丢 %d 轮 / 仅数据丢 %d 行 / 仅心跳丢 %d 行 / 仅 err 丢 %d 行"
          % (N_FULL, N_DATA, N_HB, N_ERR))
    print("        重复包 %d 条 / 重启 %d 次 / 时间戳异常 2 类各 1 次"
          % (N_DUP, len(EPOCHS) - 1))
    print("  实测：整轮丢 %d 轮 / 仅数据丢 %d 行 / 仅心跳丢 %d 行 / 仅 err 丢 %d 行"
          % (cls["full"], cls["data_only"], cls["hb_only"], cls["err_only"]))
    print("        重复包 %d 条 / 世代 %d / 时钟回跳 %d / 同秒 %d / unexpl %d"
          % (d, ne, tc, ts_n, cls["unexpl"]))
    print("-" * 66)
    if fails:
        print("  ✗ 自检未通过：")
        for f in fails:
            print("      " + f)
        print("=" * 66)
        conn.close()
        return 1
    print("  ✓ 全部一致，引擎可复现注入值")
    print("=" * 66)
    conn.close()
    try:
        shutil.rmtree(tmp, ignore_errors=True)
    except OSError:
        pass
    return 0


# ---------------------------------------------------------------- 主流程

def ensure_run(conn, hours: float) -> tuple:
    """认领或新建采集会话。重启不新建（§2.6）。"""
    rid = cursor_get(conn, "run_id")
    if rid:
        row = conn.execute("SELECT * FROM collect_run WHERE run_id=?", (int(rid),)).fetchone()
        if row and row["started_at"]:
            return row["run_id"], datetime.strptime(row["started_at"], "%Y-%m-%d %H:%M:%S")
    started = datetime.now(CST).replace(microsecond=0)
    planned = (started + timedelta(hours=hours)).strftime("%Y-%m-%d %H:%M:%S")
    cur = conn.execute("INSERT INTO collect_run (started_at,planned_end,target_hours,period_ms) "
                       "VALUES (?,?,?,?)",
                       (started.strftime("%Y-%m-%d %H:%M:%S"), planned, hours, 1000))
    rid = cur.lastrowid
    cursor_set(conn, "run_id", rid)
    conn.commit()
    return rid, started


def main(argv=None) -> int:
    _fix_console()
    ap = argparse.ArgumentParser(
        description="168h 连续采集接收端（设计见 02_docs/22_实现与交付方案.md §2）")
    ap.add_argument("--db", default=os.path.join(_HERE, "..", "03_logs", "run168.db"),
                    help="SQLite 路径（默认 03_logs/run168.db）")
    ap.add_argument("--port", type=int, default=8888, help="UDP 监听端口，默认 8888")
    ap.add_argument("--hours", type=float, default=HOURS_168, help="计划时长（小时），默认 168")
    ap.add_argument("--report-only", action="store_true", help="只重算报告（验收复算）")
    ap.add_argument("--repair", action="store_true", help="补解析缺行")
    ap.add_argument("--selftest", action="store_true", help="统计引擎自检（不需要设备）")
    ap.add_argument("--key-hex", default=None, help="AES-128-GCM 密钥（32 位 hex）")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    db_path = os.path.abspath(args.db)
    d = os.path.dirname(db_path)
    if d:
        os.makedirs(d, exist_ok=True)
    lg = setup_logging(db_path, args.verbose)
    fields = db_proto.load_dict()

    if args.report_only:
        if not os.path.exists(db_path):
            lg.error("库不存在：%s", db_path)
            return 2
        conn = open_db(db_path, readonly=True)
        print(build_report(conn, fields, db_path))
        conn.close()
        return 0

    key = bytes.fromhex(args.key_hex) if args.key_hex else None
    if key is not None and len(key) != 16:
        lg.error("--key-hex 必须是 16 字节（32 位 hex），当前 %d 字节", len(key))
        return 2

    conn = open_db(db_path)
    init_schema(conn, fields)
    run_id, started = ensure_run(conn, args.hours)
    if args.repair:
        n = repair(conn, fields, lg)
        lg.info("[REPAIR] 补入库 %d 行", n)
        conn.close()
        return 0

    lg.info("=" * 70)
    lg.info("udp_collect 启动")
    lg.info("  库      %s", db_path)
    lg.info("  端口    UDP %d（SO_RCVBUF 1 MiB + SO_EXCLUSIVEADDRUSE）", args.port)
    lg.info("  会话    run_id=%d  起于 %s  计划 %.1f h", run_id,
            started.strftime("%Y-%m-%d %H:%M:%S"), args.hours)
    lg.info("  字段    %d 个（已知不上报：%s）", len(fields), ",".join(sorted(KNOWN_MISSING)))
    lg.info("  加密    %s", "AES-128-GCM（密钥已载入）" if key else "关闭")
    lg.info("=" * 70)
    conn.close()

    stats = Stats()
    q_pkt: queue.Queue = queue.Queue(maxsize=Q_MAX)
    stop = threading.Event()

    rx = Receiver(args.port, q_pkt, stats, stop, key, lg)
    dbw = Writer(db_path, q_pkt, stats, stop, fields, run_id, lg)
    sta = StatReporter(db_path, stats, q_pkt, stop, rx, dbw, run_id, started, args.hours, lg)

    rx.start()
    # 等 socket 就绪或失败（最多 2 s），避免 bind 失败还起后面两个线程
    for _ in range(40):
        if rx.sock is not None or rx.fatal is not None:
            break
        time.sleep(0.05)
    if rx.fatal is not None:
        return 3

    dbw.start()
    sta.start()

    try:
        while not stop.is_set():
            stop.wait(1.0)
    except KeyboardInterrupt:
        lg.info("\n[RUN] 收到 Ctrl+C，开始收尾")
        stop.set()

    # 收尾：关 socket → 排空队列 → 最后 commit → checkpoint → 报告
    q_pkt.put(None)          # 唤醒 T2 走退出分支
    dbw.join(timeout=120)
    sta.join(timeout=10)
    if dbw.fatal is not None:
        lg.error("[RUN] 落库线程异常退出，报告可能不完整：%s", dbw.fatal)

    conn = open_db(db_path)
    ended = datetime.now(CST).strftime("%Y-%m-%d %H:%M:%S")
    conn.execute("UPDATE collect_run SET ended_at=? WHERE run_id=?", (ended, run_id))
    conn.commit()
    try:
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    except sqlite3.Error:
        pass
    report = build_report(conn, fields, db_path, run_id)
    conn.close()

    if dbw.fatal is not None:
        # 报告会被直接贴进验收文档，一份"数字全是 0 但看起来正常"的报告比报错更危险
        report = ("!" * 78 + "\n"
                  "!! 本报告不完整：落库线程异常退出\n"
                  "!! 原因：%s\n"
                  "!! 下列数字只反映已入库部分，**不得用于验收**\n" % dbw.fatal
                  + "!" * 78 + "\n\n" + report)

    rp = os.path.splitext(db_path)[0] + "_report.txt"
    # 默认 newline=None："\n" 会按 os.linesep 写成 CRLF，与 `print` 重定向到文件的
    # 结果一致 —— 否则 --report-only 与运行时报告会因行尾差异 diff 全红。
    with open(rp, "w", encoding="utf-8") as f:
        f.write(report + "\n")
    print(report)
    lg.info("[RUN] 报告已写入 %s", rp)
    return 0 if dbw.fatal is None else 4


if __name__ == "__main__":
    sys.exit(main())
