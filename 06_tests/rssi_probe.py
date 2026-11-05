# -*- coding: utf-8 -*-
"""RSSI / 包体积 / 分类丢包率 三联对照 —— 独立脚本，只依赖 sqlite3。

用途：判定「密文臂 50% 数据行丢失」是空口链路（RF）问题，还是 +29 B 加密开销问题。

依据：固件每个包都带 rssi（esp32c3_gateway.ino:1016），接收端抽成独立列
      udp_packet.rssi，payload_len 保存原始 UDP 长度（密文 = 明文 + 29）。

判读：
  - 若密文臂 RSSI 明显低于明文臂（≥6 dB）⇒ 链路变了（位置/遮挡/距离），与加密无关
  - 若两臂 RSSI 相同而密文臂丢得多 ⇒ 才轮到「大包更脆弱」的解释
  - 表内 5 分钟粒度丢包率若与 RSSI 同步恶化 ⇒ RF；若平RSSI 却突发 ⇒ 空口拥塞

用法: python rssi_probe.py <db1> [<db2> ...]
"""
import sqlite3
import sys


def pctl(v, q):
    if not v:
        return None
    i = int(round(q * (len(v) - 1)))
    return v[min(len(v) - 1, max(0, i))]


def load(db):
    uri = "file:%s?mode=ro" % db.replace("\\", "/")
    c = sqlite3.connect(uri, uri=True)
    c.row_factory = sqlite3.Row
    rows = [dict(r) for r in c.execute(
        "SELECT epoch_id, recv_mono, seq, ptype, payload_len, rssi, poll_ok, poll_err "
        "FROM udp_packet WHERE ptype IN ('hb','data','err') ORDER BY recv_mono")]
    c.close()
    return rows


def analyze(db):
    rows = load(db)
    if not rows:
        print("  [空库] %s" % db)
        return
    ok0 = rows[0]["poll_ok"] or 0
    er0 = rows[0]["poll_err"] or 0
    ok1 = rows[-1]["poll_ok"] or 0
    er1 = rows[-1]["poll_err"] or 0
    d_ok = ok1 - ok0
    d_er = er1 - er0
    rounds = d_ok + d_er

    by = {}
    for r in rows:
        by.setdefault(r["ptype"], []).append(r)

    print("=" * 78)
    print("库: %s" % db)
    print("  包 %d | 设备执行 %d 轮（poll_ok %d / poll_err %d）| 墙钟 %.1f s"
          % (len(rows), rounds, d_ok, d_er, rows[-1]["recv_mono"] - rows[0]["recv_mono"]))
    print("  类型  %-6s %-8s %-10s %-24s %s" % ("收到", "应发", "丢失", "RSSI p10/p50/p90", "payload 中位/最大"))
    for t in ("hb", "data", "err"):
        rs = by.get(t, [])
        sent = rounds if t == "hb" else (d_ok if t == "data" else d_er)
        loss = sent - len(rs)
        rssi = sorted([r["rssi"] for r in rs if r["rssi"] is not None])
        ln = sorted([r["payload_len"] for r in rs if r["payload_len"]])
        rate = (100.0 * loss / sent) if sent else 0.0
        print("  %-5s %-6d %-8d %-10s %-24s %s"
              % (t, len(rs), sent,
                 ("%d(%.1f%%)" % (loss, rate)),
                 ("%d/%d/%d" % (pctl(rssi, .1), pctl(rssi, .5), pctl(rssi, .9))) if rssi else "无数据",
                 ("%d/%d" % (pctl(ln, .5), ln[-1])) if ln else "-"))

    # 分类丢包（正确口径：只用计数器做分母，不用四分类的"仅"格）
    if d_ok and "data" in by and "hb" in by:
        ld = 100.0 * (d_ok - len(by["data"])) / d_ok
        lh = 100.0 * (rounds - len(by["hb"])) / rounds
        print("  ⇒ 真实丢包率  data %.2f%% / hb %.2f%%  = 比值 %.2f : 1"
              % (ld, lh, (ld / lh) if lh else 0))
        ln = sorted([r["payload_len"] for r in by["data"] if r["payload_len"]])
        hn = sorted([r["payload_len"] for r in by["hb"] if r["payload_len"]])
        if ln and hn:
            print("  ⇒ 体积比 data %d B / hb %d B = %.2f : 1（碰撞概率按空口时长走，丢包比应与之同量级）"
                  % (pctl(ln, .5), pctl(hn, .5), float(pctl(ln, .5)) / pctl(hn, .5)))

    # 5 分钟粒度：丢包率 vs RSSI
    t0 = rows[0]["recv_mono"]
    buckets = {}
    for r in rows:
        k = int((r["recv_mono"] - t0) // 300)
        b = buckets.setdefault(k, {"n": 0, "hb": 0, "data": 0, "ok": [], "er": [], "rssi": []})
        b["n"] += 1
        b["hb" if r["ptype"] == "hb" else "data"] += 1
        b["ok"].append(r["poll_ok"] or 0)
        b["er"].append(r["poll_err"] or 0)
        if r["rssi"] is not None:
            b["rssi"].append(r["rssi"])
    print("  %-8s %-6s %-5s %-5s %-8s %s" % ("分钟段", "轮数", "hb", "data", "丢包率", "RSSI p50"))
    keys = sorted(buckets)
    skip = 0
    for k in keys:
        b = buckets[k]
        rd = (max(b["ok"]) + max(b["er"])) - (min(b["ok"]) + min(b["er"]))
        if rd < 20:            # 首尾不完整桶不做判读（分母太小会出负数）
            skip += 1
            continue
        exp = rd * 2
        loss = 100.0 * (exp - b["n"]) / exp if exp else 0.0
        rs = sorted(b["rssi"])
        print("  %-8s %-6d %-5d %-5d %-8s %s"
              % ("%d-%dmin" % (k * 5, k * 5 + 5), rd, b["hb"], b["data"],
                 "%.1f%%" % loss, pctl(rs, .5) if rs else "-"))
    if skip:
        print("  （首尾 %d 个不满 5 分钟的桶已略过）" % skip)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for db in sys.argv[1:]:
        try:
            analyze(db)
        except Exception as e:
            print("  [失败] %s -> %r" % (db, e))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
