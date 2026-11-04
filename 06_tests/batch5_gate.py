#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""批次 5 台架闸门（G5-1 ~ G5-7）—— 从采集库直接算，不靠人工对表。

设计依据：04_ai_prompts/17_批次5_50ms与加密_2026-11-04.md §6
          02_docs/22 §3.2（对账恒等式）· 02_docs/24（验收判据）

为什么要有这个脚本
------------------
G5-1「两包间隔 50 ms」、G5-2「每轮 udp_tx +2」、G5-3「同轮 ts 一致」这三条，
如果靠人眼看串口或拿 Excel 比，一定会在"差不多对上了"的地方放过去。
而这三条恰好是本批**最容易静默出错**的三处（状态机拆分 / 复位块后移 / ts 重算）。
所以全部落成可复算的判据：**同一份库，谁跑都得同一个结论**。

用法
----
    # 明文臂（enc_on=0）台架跑完后
    python batch5_gate.py --db ..\\03_logs\\g5_plain.db --arm plain

    # 密文臂（enc_on=1）
    python batch5_gate.py --db ..\\03_logs\\g5_enc.db --arm enc

    # 也接受 --report 用来自动判定放行
    python batch5_gate.py --db ..\\03_logs\\g5_enc.db --arm enc --strict

退出码：0 = 全部 PASS（或该臂不适用的项记 NA）；1 = 有 FAIL。

只读打开
--------
`mode=ro` —— 绝不在归档库上留下任何写入。要写东西请先 copy 副本。
"""

import argparse
import os
import sqlite3
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

try:
    import udp_collect as U          # 复用已通过自检的对账引擎，不另写一套
except Exception as e:               # pragma: no cover
    sys.stderr.write("无法导入 udp_collect.py（应与本脚本同目录）：%s\n" % e)
    raise SystemExit(2)


# ---------------------------------------------------------------- 小工具

def open_ro(path):
    uri = "file:%s?mode=ro" % os.path.abspath(path).replace("\\", "/")
    conn = sqlite3.connect(uri, uri=True, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


def has_col(conn, table, col):
    try:
        return any(r["name"] == col for r in conn.execute("PRAGMA table_info(%s)" % table))
    except sqlite3.DatabaseError:
        return False


def pct(vals, q):
    """最近秩百分位（不引 numpy，避免旧电脑装依赖）。"""
    if not vals:
        return None
    s = sorted(vals)
    k = int(round((len(s) - 1) * q))
    return s[min(max(k, 0), len(s) - 1)]


def fmt(v, nd=1):
    if v is None:
        return "—"
    return ("%%.%df" % nd) % v


# ---------------------------------------------------------------- 取轮次视图

def load_rounds(conn):
    """每轮一行：hb 的到达时刻 + 它的**下一条**(seq+1)的到达时刻 / 类型 / ts。

    ⚠️ 配对规则是 **hb 在 seq，第二行在 seq+1**（hb 先发、每发一行 seq+1）。
       绝不能按 seq 分组 —— 那是两行各自的 seq，分组后就永远配不上对
       （第一版就栽在这里：groups 翻倍、G5-1 全 NA、G5-2 半轮 100%）。
    """
    has_ts = has_col(conn, "udp_packet", "ts")
    ts_sel = ", MIN(ts) AS ts" if has_ts else ", NULL AS ts"
    sql = """
    SELECT epoch_id, seq, ptype,
           MIN(recv_mono) AS t,
           MIN(udp_tx)    AS tx,
           MIN(poll_ok)   AS ok,
           MIN(poll_err)  AS er
           %s
    FROM udp_packet
    WHERE seq IS NOT NULL AND ptype IN ('hb','data','err')
    GROUP BY epoch_id, seq, ptype
    """ % ts_sel

    hb, sec = {}, {}
    for r in conn.execute(sql):
        key = (r["epoch_id"], r["seq"])
        if r["ptype"] == "hb":
            hb[key] = r
        else:
            sec[key] = r

    out = []
    for (eid, s), h in sorted(hb.items(), key=lambda kv: (kv[1]["t"], kv[0][1])):
        n = sec.get((eid, s + 1))
        out.append({
            "epoch_id": eid, "seq": s,
            "t_hb": h["t"], "tx_hb": h["tx"],
            "t_2": (n["t"] if n else None),
            "second_kind": (n["ptype"] if n else None),
            "ts2": (n["ts"] if n else None),
        })
    orphan = sum(1 for (eid, s) in sec if (eid, s - 1) not in hb)
    return out, has_ts, orphan


# ---------------------------------------------------------------- 各判据

def g5_1(rounds, orphan=0):
    """两包间隔：同一轮 第二行 − hb 的到达时刻差。"""
    gaps = [r["t_2"] - r["t_hb"] for r in rounds
            if r["t_hb"] is not None and r["t_2"] is not None]
    half = sum(1 for r in rounds if r["t_hb"] is not None and r["t_2"] is None)
    if not gaps:
        return {"ok": None, "note": "没有可配对的轮次（一包都没成对）"}
    med, p99, mx = pct(gaps, 0.5), pct(gaps, 0.99), max(gaps)
    ok = (0.040 <= med <= 0.080) and (p99 < 0.20)
    return {
        "ok": ok,
        "med": med, "p99": p99, "max": mx, "n": len(gaps),
        "half": half, "orphan": orphan,
        "note": "中位数应落在 40~80 ms（50 ms 目标 + 一个 loop 周期），p99 < 200 ms",
    }


def g5_2(rounds):
    """节拍：同一 epoch 内，相邻 hb 的 udp_tx 必须恰好 +2。"""
    bad, pairs, deltas = 0, 0, {}
    for eid in sorted({r["epoch_id"] for r in rounds}):
        hbs = [r for r in rounds if r["epoch_id"] == eid and r["t_hb"] is not None
               and r["tx_hb"] is not None]
        hbs.sort(key=lambda r: r["t_hb"])
        for a, b in zip(hbs, hbs[1:]):
            d = b["tx_hb"] - a["tx_hb"]
            pairs += 1
            deltas[d] = deltas.get(d, 0) + 1
            if d != 2:
                bad += 1
    half = sum(1 for r in rounds if r["t_hb"] is not None and r["t_2"] is None)
    total = sum(1 for r in rounds if r["t_hb"] is not None)
    half_ratio = (half / total) if total else None
    ok = (pairs > 0 and bad == 0 and (half_ratio is not None and half_ratio < 0.001))
    return {
        "ok": ok, "pairs": pairs, "bad": bad, "deltas": deltas,
        "half": half, "half_ratio": half_ratio,
        "note": "相邻 hb 的 udp_tx 必须 +2；半轮占比 < 0.1%",
    }


def g5_3(rounds, has_ts):
    """同轮 ts 一致。库里没有 ts 列时，只能记 NA 并说明结构依据。"""
    if not has_ts:
        return {"ok": None,
                "note": "本库无 ts 列 ⇒ 无法事后比对；结构上已核对 sendSecondLine() 只读 g_roundTs"}
    # 用 ptype<>'hb' 的 ts 与…… 库里 hb 不存 payload，故只有第二行的 ts
    # ⇒ 只能做"每轮 ts 单调且增量≈1 s"的旁证
    ts_list = [(r["t_hb"], r["ts2"]) for r in rounds if r["ts2"]]
    if len(ts_list) < 3:
        return {"ok": None, "note": "ts 样本不足 3 条"}
    step = {"INCR1": 0, "SAME": 0, "BACK": 0, "JUMP": 0}
    prev = None
    for _, ts in ts_list:
        if prev is not None:
            if ts == prev:
                step["SAME"] += 1
            elif ts > prev:
                secs = _iso_diff(prev, ts)
                step["INCR1" if secs == 1 else "JUMP"] += 1
            else:
                step["BACK"] += 1
        prev = ts
    ok = (step["BACK"] == 0)
    return {"ok": ok, "step": step, "n": len(ts_list),
            "note": "只能做旁证（库内无 hb.ts）；BACK 必须为 0"}


def _iso_diff(a, b):
    """两个 ISO 时间串（YYYY-MM-DDTHH:MM:SS）的秒差；解析失败返回 None。"""
    try:
        import datetime as dt
        fa = dt.datetime.strptime(a[:19].replace(" ", "T"), "%Y-%m-%dT%H:%M:%S")
        fb = dt.datetime.strptime(b[:19].replace(" ", "T"), "%Y-%m-%dT%H:%M:%S")
        return int((fb - fa).total_seconds())
    except Exception:
        return None


def g5_4(conn, arm):
    """解密健康度：decrypt_fail 必须 0；密文臂 data 行 enc=1 必须 100%。"""
    n_fail = conn.execute(
        "SELECT COUNT(*) c FROM udp_packet WHERE parse_status='decrypt_fail'").fetchone()["c"]
    if has_col(conn, "udp_packet", "enc"):
        q = ("SELECT COALESCE(enc,0) e, COUNT(*) c FROM udp_packet "
             "WHERE ptype='data' GROUP BY COALESCE(enc,0)")
        dist = {r["e"]: r["c"] for r in conn.execute(q)}
    else:
        dist = {}
    n_data = sum(dist.values())
    enc_ratio = (dist.get(1, 0) / n_data) if n_data else None
    if arm == "enc":
        ok = (n_fail == 0 and n_data > 0 and dist.get(0, 0) == 0)
    else:
        ok = (n_fail == 0 and (n_data == 0 or dist.get(1, 0) == 0))
    return {"ok": ok, "decrypt_fail": n_fail, "dist": dist,
            "n_data": n_data, "enc_ratio": enc_ratio,
            "note": "密文臂要求 decrypt_fail=0 且 data 行 enc=1 占 100%"}


def g5_5(conn, arm):
    """包长：payload_len（原始报文）与 payload（已存明文）之差，密文臂应恒为 29。"""
    if not has_col(conn, "udp_packet", "payload_len"):
        return {"ok": None, "note": "无 payload_len 列"}
    rows = conn.execute(
        "SELECT ptype, payload_len, LENGTH(payload) AS plen, COALESCE(enc,0) AS e "
        "FROM udp_packet WHERE payload IS NOT NULL AND payload_len IS NOT NULL").fetchall()
    if not rows:
        return {"ok": None, "note": "没有同时含 payload 与 payload_len 的行"}
    want = 29 if arm == "enc" else 0
    bad = [r for r in rows if (r["payload_len"] - r["plen"]) != want]
    mx = max(r["payload_len"] for r in rows)
    ok = (not bad) and (mx < 1472)
    return {"ok": ok, "n": len(rows), "bad": len(bad), "want": want, "max_len": mx,
            "note": "差值必须恒为 %d B；max(payload_len) < 1472（否则 IP 分片）" % want}


def g5_6(conn):
    """对账恒等式：期望行 = 收到 + 未发出 + 网络丢失，残差必须 0。"""
    tot = {"expected": 0, "distinct": 0, "not_sent": 0, "lost": 0,
           "rounds": 0, "dup": 0}
    for ep in conn.execute("SELECT * FROM device_epoch ORDER BY epoch_id"):
        st = U._epoch_stats(conn, ep)
        if st is None:
            continue
        for k in tot:
            tot[k] += st.get(k, 0) or 0
    resid = tot["expected"] - (tot["distinct"] + tot["not_sent"] + tot["lost"])
    return {"ok": (resid == 0), "resid": resid, **tot,
            "note": "残差必须 0（公式见 02_docs/22 §3.2）"}


def g5_7(conn):
    """丢包不对称比：data_lost / hb_lost。基线明文臂约 15:1。"""
    cls = U.classify_loss(conn, U.infer_err_row_mode(conn))
    d, h = cls["data_only"], cls["hb_only"]
    ratio = (d / h) if h else None
    return {"ok": None, "data_only": d, "hb_only": h, "ratio": ratio,
            "full": cls["full"], "err_only": cls["err_only"],
            "note": "只报不判：基线约 15:1；密文臂若 > 20:1 ⇒ +29 B 有实质影响，须写进报告"}


# ---------------------------------------------------------------- 主流程

def main(argv=None):
    ap = argparse.ArgumentParser(description="批次 5 台架闸门 G5-1~G5-7")
    ap.add_argument("--db", required=True, help="采集库路径（只读打开）")
    ap.add_argument("--arm", choices=["plain", "enc"], default="plain",
                    help="plain=明文臂(enc_on=0) / enc=密文臂(enc_on=1)")
    ap.add_argument("--strict", action="store_true", help="有 FAIL 时退出码 1")
    args = ap.parse_args(argv)

    if not os.path.exists(args.db):
        sys.stderr.write("库不存在：%s\n" % args.db)
        return 2

    conn = open_ro(args.db)
    try:
        try:
            meta = conn.execute("SELECT * FROM collect_run ORDER BY run_id DESC LIMIT 1").fetchone()
            n_rows = conn.execute("SELECT COUNT(*) c FROM udp_packet").fetchone()["c"]
            n_ep = conn.execute("SELECT COUNT(*) c FROM device_epoch").fetchone()["c"]
        except sqlite3.OperationalError as e:
            sys.stderr.write(
                "这个库里没有 batch-5 需要的表（%s）。\n"
                "确认 --db 指向的是 udp_collect.py 采集出来的库，不是原型/示例库。\n" % e)
            return 2

        print("=" * 74)
        print("批次 5 台架闸门  arm=%s" % args.arm)
        print("  库      %s" % os.path.abspath(args.db))
        if meta:
            print("  会话    run_id=%s  fw=%s  bin=%s"
                  % (meta["run_id"], meta["fw_ver"] or "[未提供]",
                     (meta["bin_sha256"] or "[未提供]")[:16]))
        print("  规模    %d 行 / %d 个世代" % (n_rows, n_ep))
        print("=" * 74)

        rounds, has_ts, orphan = load_rounds(conn)
        print("  轮次视图：%d 轮（按 hb 计）%s"
              % (len(rounds), ("，孤儿第二行 %d" % orphan) if orphan else ""))
        print("")

        r1 = g5_1(rounds, orphan)
        r2 = g5_2(rounds)
        r3 = g5_3(rounds, has_ts)
        r4 = g5_4(conn, args.arm)
        r5 = g5_5(conn, args.arm)
        r6 = g5_6(conn)
        r7 = g5_7(conn)

        def verdict(d):
            return {True: "PASS", False: "FAIL", None: " NA "}[d["ok"]]

        print("G5-1 两包间隔        %s" % verdict(r1))
        if r1["ok"] is not None:
            print("      中位数 %s s / p99 %s s / max %s s / n=%d"
                  % (fmt(r1["med"], 4), fmt(r1["p99"], 4), fmt(r1["max"], 4), r1["n"]))
            if r1["half"] or r1["orphan"]:
                print("      半轮 %d / 孤儿 %d" % (r1["half"], r1["orphan"]))
        print("      判据：%s" % r1["note"])

        print("")
        print("G5-2 节拍与轮完整性  %s" % verdict(r2))
        if r2["pairs"]:
            print("      相邻 hb 数 %d，非 +2 的 %d；增量分布 %s"
                  % (r2["pairs"], r2["bad"],
                     ", ".join("%+d:%d" % (k, v) for k, v in sorted(r2["deltas"].items()))))
            print("      半轮 %d（占比 %s）" % (r2["half"], fmt(
                (r2["half_ratio"] or 0) * 100, 3) + "%"))
        print("      判据：%s" % r2["note"])

        print("")
        print("G5-3 同轮 ts 一致     %s" % verdict(r3))
        if r3.get("step"):
            print("      %s" % r3["step"])
        print("      判据：%s" % r3["note"])

        print("")
        print("G5-4 解密健康度      %s" % verdict(r4))
        print("      decrypt_fail=%d  data 行 enc 分布 %s"
              % (r4["decrypt_fail"], r4["dist"] or "{}"))
        print("      判据：%s" % r4["note"])

        print("")
        print("G5-5 包长与分片      %s" % verdict(r5))
        if r5.get("n"):
            print("      样本 %d，差值≠%d 的 %d 条；max(payload_len)=%d"
                  % (r5["n"], r5["want"], r5["bad"], r5["max_len"]))
        print("      判据：%s" % r5["note"])

        print("")
        print("G5-6 对账闭合        %s" % verdict(r6))
        print("      期望 %d = 收到 %d + 未发 %d + 丢 %d ⇒ 残差 %d"
              % (r6["expected"], r6["distinct"], r6["not_sent"], r6["lost"], r6["resid"]))
        print("      判据：%s" % r6["note"])

        print("")
        print("G5-7 丢包不对称      %s（只报不判）" % verdict(r7))
        print("      仅丢 data %d / 仅丢 hb %d ⇒ 比值 %s"
              % (r7["data_only"], r7["hb_only"],
                 ("%.1f:1" % r7["ratio"]) if r7["ratio"] else "—"))
        print("      判据：%s" % r7["note"])

        print("")
        print("=" * 74)
        fails = [n for n, d in (("G5-1", r1), ("G5-2", r2), ("G5-3", r3), ("G5-4", r4),
                                ("G5-5", r5), ("G5-6", r6)) if d["ok"] is False]
        nas = [n for n, d in (("G5-1", r1), ("G5-2", r2), ("G5-3", r3), ("G5-4", r4),
                              ("G5-5", r5), ("G5-6", r6)) if d["ok"] is None]
        if fails:
            print("结论：**不可放行**，FAIL = %s" % ", ".join(fails))
        else:
            print("结论：**可放行**（NA = %s%s）"
                  % (", ".join(nas) if nas else "无",
                     "；NA 项须在报告里说明依据" if nas else ""))
        print("=" * 74)
        return 1 if (fails and args.strict) else (0 if not fails else 1)
    finally:
        conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
