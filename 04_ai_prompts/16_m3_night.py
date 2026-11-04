# -*- coding: utf-8 -*-
"""m3_night.py —— 从 168h 库复算 M3 的夜间窗口值与逐小时分布。

只读打开数据库，绝不写库、绝不 ALTER（归档指纹必须保持不变）。

M3 = 去重收到行 / 应唯一发送行
  · 分母 = Δudp_tx + 1（固件 hb 里的 udp_tx 是「发送这一行之前」的累计值）
  · 分子按 (device_id, epoch_id, seq) 去重（重包不能算两次送达）
  · 夜间窗口 = 22:00–07:59 本地时间，完整 10 h；跨零点按「归属前一晚」合并
  · 小时内若 udp_tx 回落（固件重启）⇒ 该小时增量不可算，标 N/A 并剔除

用法：
    python m3_night.py <run168h.db>
输出：
    m3_by_hour.csv  逐小时
    m3_by_night.csv 逐夜
    控制台摘要（含判定用的两个数）
"""
import csv
import os
import sqlite3
import sys

# ---------------------------------------------------------------- SQL
# ① 逐小时：分母的起止 udp_tx 取该小时首尾两条 hb
SQL_HOUR = """
WITH hb AS (
    SELECT substr(recv_time,1,13) AS hr,
           udp_tx, epoch_id,
           ROW_NUMBER() OVER (PARTITION BY substr(recv_time,1,13) ORDER BY recv_mono ASC)  AS rn_a,
           ROW_NUMBER() OVER (PARTITION BY substr(recv_time,1,13) ORDER BY recv_mono DESC) AS rn_z
    FROM udp_packet
    WHERE ptype='hb' AND udp_tx IS NOT NULL
),
tx AS (
    SELECT hr,
           MAX(CASE WHEN rn_a=1 THEN udp_tx END) AS tx_a,
           MAX(CASE WHEN rn_z=1 THEN udp_tx END) AS tx_z,
           COUNT(DISTINCT epoch_id)              AS ep_n
    FROM hb GROUP BY hr
),
rx AS (
    SELECT substr(recv_time,1,13) AS hr,
           COUNT(DISTINCT device_id || '|' || COALESCE(epoch_id,-1) || '|' || seq) AS rx_rows
    FROM udp_packet
    WHERE ptype IN ('data','hb','err') AND seq IS NOT NULL
    GROUP BY hr
)
SELECT rx.hr                  AS hr,
       rx.rx_rows             AS rx_rows,
       (tx.tx_z - tx.tx_a + 1) AS tx_rows,
       tx.ep_n                AS epochs_in_hour,
       CASE WHEN tx.tx_z < tx.tx_a THEN 1 ELSE 0 END AS rebooted
FROM rx JOIN tx ON rx.hr = tx.hr
ORDER BY rx.hr
"""

# ② 逐夜：22:00 起算，归属到 22 点所在的那一天
SQL_NIGHT = """
WITH hb AS (
    SELECT CASE WHEN CAST(strftime('%H', recv_time) AS INTEGER) >= 22
                THEN date(recv_time) ELSE date(recv_time,'-1 day') END AS night_id,
           recv_time, recv_mono, udp_tx, epoch_id,
           ROW_NUMBER() OVER (
               PARTITION BY CASE WHEN CAST(strftime('%H', recv_time) AS INTEGER) >= 22
                                 THEN date(recv_time) ELSE date(recv_time,'-1 day') END
               ORDER BY recv_mono ASC)  AS rn_a,
           ROW_NUMBER() OVER (
               PARTITION BY CASE WHEN CAST(strftime('%H', recv_time) AS INTEGER) >= 22
                                 THEN date(recv_time) ELSE date(recv_time,'-1 day') END
               ORDER BY recv_mono DESC) AS rn_z
    FROM udp_packet
    WHERE ptype='hb' AND udp_tx IS NOT NULL
      AND (CAST(strftime('%H', recv_time) AS INTEGER) >= 22
           OR CAST(strftime('%H', recv_time) AS INTEGER) < 8)
),
tx AS (
    SELECT night_id,
           MAX(CASE WHEN rn_a=1 THEN udp_tx END) AS tx_a,
           MAX(CASE WHEN rn_z=1 THEN udp_tx END) AS tx_z,
           MIN(recv_time) AS t_first,
           MAX(recv_time) AS t_last,
           COUNT(DISTINCT substr(recv_time,1,13)) AS hours_seen,
           COUNT(DISTINCT epoch_id) AS ep_n
    FROM hb GROUP BY night_id
),
rx AS (
    SELECT CASE WHEN CAST(strftime('%H', recv_time) AS INTEGER) >= 22
                THEN date(recv_time) ELSE date(recv_time,'-1 day') END AS night_id,
           COUNT(DISTINCT device_id || '|' || COALESCE(epoch_id,-1) || '|' || seq) AS rx_rows
    FROM udp_packet
    WHERE ptype IN ('data','hb','err') AND seq IS NOT NULL
      AND (CAST(strftime('%H', recv_time) AS INTEGER) >= 22
           OR CAST(strftime('%H', recv_time) AS INTEGER) < 8)
    GROUP BY night_id
)
SELECT tx.night_id            AS night_id,
       tx.t_first             AS t_first,
       tx.t_last              AS t_last,
       tx.hours_seen          AS hours_seen,
       rx.rx_rows             AS rx_rows,
       (tx.tx_z - tx.tx_a + 1) AS tx_rows,
       tx.ep_n                AS epochs_in_night,
       CASE WHEN tx.tx_z < tx.tx_a THEN 1 ELSE 0 END AS rebooted
FROM rx JOIN tx ON rx.night_id = tx.night_id
ORDER BY tx.night_id
"""

# ③ 全天分母：必须**按世代分段求和**。
#    ⚠️ 2026-11-04 修 bug（旧电脑在 168h 库上实跑发现）：原实现用「末条 udp_tx − 首条 udp_tx + 1」
#    跨全库算，而固件重启会把 g_udpTx 归零 ⇒ 一次重启就吃掉前面全部增量。
#    168h 里 10-29 有 3 次重启，导致 C-b 被算成 131.66%（物理不可能：收到比发出还多）。
#    正解：按 epoch_id 分段，每段 max−min+1 再求和（段内单调，故 max=末、min=首）。
SQL_TX_EPOCH = """
SELECT COALESCE(epoch_id,-1) AS ep,
       MIN(udp_tx) AS tx_a,
       MAX(udp_tx) AS tx_z,
       COUNT(*)    AS n_hb
FROM udp_packet
WHERE ptype='hb' AND udp_tx IS NOT NULL
GROUP BY COALESCE(epoch_id,-1)
"""

# 段内单调性校验：若同一 epoch 内 udp_tx 出现回落，说明 epoch 切分漏了重启 ⇒ 该段不可算
SQL_TX_NONMONO = """
WITH s AS (
    SELECT COALESCE(epoch_id,-1) AS ep, udp_tx,
           LAG(udp_tx) OVER (PARTITION BY COALESCE(epoch_id,-1) ORDER BY recv_mono) AS prev
    FROM udp_packet WHERE ptype='hb' AND udp_tx IS NOT NULL
)
SELECT ep, COUNT(*) AS drops FROM s WHERE prev IS NOT NULL AND udp_tx < prev GROUP BY ep
"""


def pct(a, b):
    return (100.0 * a / b) if b else None


def main(db_path):
    uri = "file:%s?mode=ro" % db_path.replace("\\", "/")
    conn = sqlite3.connect(uri, uri=True)
    conn.row_factory = sqlite3.Row
    out_dir = os.path.dirname(os.path.abspath(db_path))

    # ---- 逐小时
    hours = []
    for r in conn.execute(SQL_HOUR):
        tx_rows = r["tx_rows"]
        # rx > tx 物理不可能（收到比发出多）⇒ 该小时内有 udp_tx 回卷没被 rebooted 抓到
        ok = ((not r["rebooted"]) and tx_rows and tx_rows > 0
              and r["rx_rows"] <= tx_rows)
        if r["rebooted"]:
            note = "REBOOT_IN_HOUR"
        elif not (tx_rows and tx_rows > 0):
            note = "NO_TX"
        elif r["rx_rows"] > tx_rows:
            note = "RX_GT_TX"
        else:
            note = ""
        hours.append({
            "hr": r["hr"], "rx_rows": r["rx_rows"], "tx_rows": tx_rows,
            "m3_pct": round(pct(r["rx_rows"], tx_rows), 4) if ok else None,
            "epochs_in_hour": r["epochs_in_hour"],
            "note": note,
        })
    p1 = os.path.join(out_dir, "m3_by_hour.csv")
    with open(p1, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=["hr", "rx_rows", "tx_rows", "m3_pct",
                                          "epochs_in_hour", "note"])
        w.writeheader()
        w.writerows(hours)

    # ---- 逐夜
    # ⚠️ 2026-11-04 修口径（旧电脑实跑发现）：原来只判「10 个时钟小时都有心跳」，
    #    但 168h 的起跑夜 22:44 才开跑 ⇒ 22 点只有 16 分钟，仍被当成完整夜。
    #    实测该夜 tx=66,684 行 = 33,342 秒 = 9.26 h，比 10 h 缺 44 分钟。
    #    「完整 10 小时」是本判据的**预声明定义**，必须真的覆盖满 ⇒ 补首末包时刻校验。
    def _hms(s):
        return s[11:19] if len(s) >= 19 else ""

    hearts = {}
    for r in conn.execute(SQL_TX_NONMONO):
        hearts[r["ep"]] = r["drops"]

    nights = []
    for r in conn.execute(SQL_NIGHT):
        tx_rows = r["tx_rows"]
        first_ok = "22:00:00" <= _hms(r["t_first"]) <= "22:05:00"
        last_ok = "07:55:00" <= _hms(r["t_last"]) <= "08:00:00"
        ok = (not r["rebooted"]) and tx_rows and tx_rows > 0 and r["rx_rows"] <= tx_rows
        if r["rebooted"]:
            note = "REBOOT_IN_NIGHT"
        elif not first_ok:
            note = "PARTIAL_START(%s)" % _hms(r["t_first"])
        elif not last_ok:
            note = "PARTIAL_END(%s)" % _hms(r["t_last"])
        elif not (tx_rows and tx_rows > 0):
            note = "NO_TX"
        elif r["rx_rows"] > tx_rows:
            note = "RX_GT_TX"
        else:
            note = ""
        nights.append({
            "night_id": r["night_id"], "t_first": r["t_first"], "t_last": r["t_last"],
            "hours_seen": r["hours_seen"], "rx_rows": r["rx_rows"], "tx_rows": tx_rows,
            "m3_pct": round(pct(r["rx_rows"], tx_rows), 4) if ok else None,
            # 完整夜 = 10 个时钟小时都有 hb **且** 首包 ≤22:05 **且** 末包 ≥07:55 且无重启
            "complete": 1 if (r["hours_seen"] == 10 and first_ok and last_ok
                              and not r["rebooted"]) else 0,
            "note": note,
        })
    p2 = os.path.join(out_dir, "m3_by_night.csv")
    with open(p2, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=["night_id", "t_first", "t_last", "hours_seen",
                                          "rx_rows", "tx_rows", "m3_pct", "complete", "note"])
        w.writeheader()
        w.writerows(nights)

    # ---- 汇总：只算「完整且无重启」的夜（不许挑，全部计入）
    good_n = [n for n in nights if n["complete"] == 1]
    good_h = [h for h in hours if h["m3_pct"] is not None]
    all_rx = conn.execute(
        "SELECT COUNT(DISTINCT device_id || '|' || COALESCE(epoch_id,-1) || '|' || seq) "
        "FROM udp_packet WHERE ptype IN ('data','hb','err') AND seq IS NOT NULL"
    ).fetchone()[0]

    # 全天分母：按世代分段求和（见 SQL_TX_EPOCH 的注释）
    all_tx = 0
    seg_bad = []
    seg_n = 0
    for s in conn.execute(SQL_TX_EPOCH):
        seg_n += 1
        if hearts.get(s["ep"], 0) > 0:
            seg_bad.append(s["ep"])      # 段内有回卷 ⇒ epoch 切分漏了重启，该段不可信
        all_tx += (s["tx_z"] - s["tx_a"] + 1)

    lines = []
    A = lines.append
    A("=" * 74)
    A("M3 夜间窗口复算（只读）")
    A("  库  %s" % db_path)
    A("=" * 74)
    A("")
    A("【1】逐夜（22:00–07:59，完整夜 = 10 个时钟小时都在库内）")
    A("%-12s %-21s %-21s %6s %10s %10s %10s" %
      ("night_id", "首包", "末包", "小时", "收到行", "发送行", "M3%"))
    for n in nights:
        A("%-12s %-21s %-21s %6d %10d %10d %10s" % (
            n["night_id"], n["t_first"], n["t_last"], n["hours_seen"],
            n["rx_rows"], n["tx_rows"],
            "N/A" if n["m3_pct"] is None else "%.4f" % n["m3_pct"]))
    A("")
    A("  完整夜 %d 个 / 共 %d 个夜段" % (len(good_n), len(nights)))
    if len(good_n) != len(nights):
        bad = ["%s[%s]" % (n["night_id"], n["note"] or "?")
               for n in nights if n["complete"] != 1]
        A("  ⚠️ 非完整夜已剔除，不参与 C-a：%s" % ", ".join(bad))
        A("     （判据要求真的覆盖满 22:00–08:00；起跑/收尾残段不算）")
    A("")
    if good_n:
        sr = sum(n["rx_rows"] for n in good_n)
        st = sum(n["tx_rows"] for n in good_n)
        A("【2】★ C-a 判据值：完整夜合计 %d 行 / %d 行 = **%.4f%%**" % (sr, st, pct(sr, st)))
        A("    阈值 ≥99%% ⇒ %s" % ("**成立**" if pct(sr, st) >= 99.0 else "**不成立（裁定作废，M3 回落 F）**"))
        # 稳健性对照：把被剔除但 tx 仍然有效的夜也算进去，看结论会不会翻转
        loose = [n for n in nights if n["m3_pct"] is not None]
        if len(loose) != len(good_n):
            lr = sum(n["rx_rows"] for n in loose)
            lt = sum(n["tx_rows"] for n in loose)
            A("    稳健性对照（把上面剔除的夜也算进来，共 %d 夜）= %.4f%% ⇒ 结论%s"
              % (len(loose), pct(lr, lt),
                 "一致" if (pct(lr, lt) >= 99.0) == (pct(sr, st) >= 99.0) else "**不一致，须人工裁定**"))
    else:
        A("【2】没有完整夜 ⇒ C-a 无法判定")
    A("")
    A("【3】C-b 归档值：全天 %d 行 / %d 行 = **%.4f%%**（分母按 %d 个世代分段求和）"
      % (all_rx, all_tx, pct(all_rx, all_tx), seg_n))
    if seg_bad:
        A("    ⚠️ 世代 %s 段内 udp_tx 仍有回落 ⇒ epoch 切分可能漏了重启，C-b 会偏大"
          % ", ".join(str(x) for x in seg_bad))
    A("")
    A("【4】逐小时有效 %d 个（有 udp_tx 增量且无重启）" % len(good_h))
    A("    ⚠️ 逐小时值有窗口边缘效应：分母取该小时「首条/末条**收到的** hb」的 udp_tx，")
    A("       若该小时开头有成片丢包，首条 hb 会偏晚 ⇒ 分母偏小 ⇒ M3 偏乐观。")
    A("       **仅用于看趋势，不参与判据**；判据只认【2】的整夜值（边缘偏差摊到 3.6 万行 ≈0.005%）。")
    if good_h:
        lo = min(good_h, key=lambda x: x["m3_pct"])
        hi = max(good_h, key=lambda x: x["m3_pct"])
        A("    最差小时 %s = %.4f%%   最好小时 %s = %.4f%%" %
          (lo["hr"], lo["m3_pct"], hi["hr"], hi["m3_pct"]))
        A("    夜间小时（22,23,0-7）平均 M3 = %.4f%%" % (
            sum(h["m3_pct"] for h in good_h
                if int(h["hr"][11:13]) >= 22 or int(h["hr"][11:13]) < 8) /
            max(1, len([h for h in good_h
                        if int(h["hr"][11:13]) >= 22 or int(h["hr"][11:13]) < 8]))))
        A("    白天小时（8-21）平均 M3   = %.4f%%" % (
            sum(h["m3_pct"] for h in good_h
                if 8 <= int(h["hr"][11:13]) < 22) /
            max(1, len([h for h in good_h if 8 <= int(h["hr"][11:13]) < 22]))))
    A("")
    A("输出：%s" % p1)
    A("      %s" % p2)
    txt = "\n".join(lines)
    print(txt)
    p3 = os.path.join(out_dir, "m3_summary.txt")
    with open(p3, "w", encoding="utf-8") as f:
        f.write(txt + "\n")
    A_out = None
    conn.close()
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法：python m3_night.py <run168h.db>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
