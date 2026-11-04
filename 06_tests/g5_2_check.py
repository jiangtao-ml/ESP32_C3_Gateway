# -*- coding: utf-8 -*-
"""G5-2 复算（修正判据）—— 独立小脚本，不依赖 batch5_gate.py / udp_collect.py。

原判据「相邻 hb 的 udp_tx 必须 +2」无法区分「设备少发」和「网络整轮丢包」：
整轮两包都丢时 udp_tx 就是 +4，会被误判为固件缺陷。
本脚本用 hb 自带的 poll 计数器做分母：Δudp_tx 必须 == 2 × Δ(poll_ok+poll_err)。
丢包不破坏该恒等式（丢的是"收到"不是"发出"），只有少发/半轮才破坏。
奇数 Δtx 才是设备半轮（!g_linkUp 中途断链）的唯一信号。

用法: python g5_2_check.py <db 路径> [周期秒,默认1]
"""
import sqlite3
import sys


def main():
    db = sys.argv[1]
    period = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
    uri = "file:%s?mode=ro" % db.replace("\\", "/")
    c = sqlite3.connect(uri, uri=True)
    c.row_factory = sqlite3.Row

    pairs = viol = odd = 0
    deltas = {}
    rnd = 0
    elapsed = 0.0
    samples = []

    for ep in c.execute("SELECT DISTINCT epoch_id FROM udp_packet WHERE ptype='hb'"):
        rows = [dict(r) for r in c.execute(
            "SELECT seq, recv_mono, udp_tx, poll_ok, poll_err FROM udp_packet "
            "WHERE ptype='hb' AND epoch_id IS ? AND udp_tx IS NOT NULL "
            "AND poll_ok IS NOT NULL AND poll_err IS NOT NULL ORDER BY recv_mono",
            (ep["epoch_id"],))]
        if len(rows) < 2:
            continue
        rnd += (rows[-1]["poll_ok"] + rows[-1]["poll_err"]) - \
               (rows[0]["poll_ok"] + rows[0]["poll_err"])
        elapsed += rows[-1]["recv_mono"] - rows[0]["recv_mono"]
        for a, b in zip(rows, rows[1:]):
            dt = b["udp_tx"] - a["udp_tx"]
            dr = (b["poll_ok"] + b["poll_err"]) - (a["poll_ok"] + a["poll_err"])
            pairs += 1
            deltas[dt] = deltas.get(dt, 0) + 1
            if dt % 2 != 0:
                odd += 1
            if dt != 2 * dr:
                viol += 1
            if (dt % 2 != 0 or dt != 2 * dr) and len(samples) < 8:
                samples.append("seq %d->%d  Δtx=%d  Δ轮=%d" % (a["seq"], b["seq"], dt, dr))

    rate = (rnd / elapsed) if elapsed > 0 else 0.0
    want = 1.0 / period
    rate_ok = (want * 0.98) <= rate <= (want * 1.02)
    ok = pairs > 0 and viol == 0 and odd == 0 and rate_ok

    print("G5-2 节拍与设备侧完整性（修正判据）")
    print("  相邻 hb 对 %d" % pairs)
    print("  Δtx≠2·Δ轮 的 %d 处" % viol)
    print("  奇数 Δtx %d 处（=设备半轮）" % odd)
    print("  增量分布 %s" % ", ".join("%+d:%d" % (k, deltas[k]) for k in sorted(deltas)))
    print("  设备执行 %d 轮 / 墙钟 %.1f s ⇒ %.4f 轮/秒（期望 %.4f，容差 ±2%%）"
          % (rnd, elapsed, rate, want))
    for s in samples:
        print("    异常样本：%s" % s)
    print("  结论：%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
