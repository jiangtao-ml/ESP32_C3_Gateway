#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NTP 可达性探针（PC 侧）
========================

用途：在把 ESP32 的 NTP 源改成内网服务器之前，先在电脑上确认
      ① 该服务器是否真的在应答 NTP（UDP/123）；
      ② 与公网 NTP 相比的往返延迟与时间偏差。

背景：导师（张振亚）2026-10-02 起在群里通知，**172.16.0.2 上有 chrony 服务**。
      ⚠️ 注意 172.16.0.0/12 是 RFC1918 私有网段，与电脑所在的 192.168.1.0/24 不同网段 ——
      能否连通取决于校园网是否路由该网段。**必须在电脑侧实测**，不能假定。

用法：
    python ntp_probe.py 172.16.0.2
    python ntp_probe.py 172.16.0.2 ntp.aliyun.com 203.107.6.88

判读：
    - 打印出 stratum / 服务器时间 ⇒ 该源可用（chrony 已配置 allow，且 UDP/123 通）。
    - `超时` ⇒ 不可用：可能被路由隔断、被防火墙拦、或 chrony 只做上游同步而未开放服务。
"""

import socket
import struct
import sys
import time

NTP_EPOCH_DELTA = 2208988800          # 1900-01-01 与 1970-01-01 的秒差
LI_VN_MODE_CLIENT = 0x1B              # LI=0 VN=3 Mode=3(client)
STRATUM_NAME = {0: "unspecified/kiss-o'-death", 1: "一级（原子钟）",
                2: "二级", 3: "三级", 16: "unsynchronized"}


def query(host: str, timeout: float = 3.0):
    """返回 (服务器时间戳, stratum, 往返毫秒, 与本地时钟偏差秒)；失败返回 None。"""
    pkt = bytes([LI_VN_MODE_CLIENT]) + 47 * b"\x00"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        t0 = time.time()
        s.sendto(pkt, (host, 123))
        data, addr = s.recvfrom(1024)
        t1 = time.time()
    except (socket.timeout, OSError) as exc:
        s.close()
        return ("ERR", exc)
    s.close()

    if len(data) < 48:
        return ("SHORT", len(data))

    stratum = data[1]
    # 字节 40..47 = Transmit Timestamp（服务器发出时刻）
    secs, frac = struct.unpack("!II", data[40:48])
    srv = (secs - NTP_EPOCH_DELTA) + frac / 2.0 ** 32
    rtt_ms = (t1 - t0) * 1000.0
    # 粗偏差：本地钟 vs 服务器钟（含网络延迟，仅作数量级参考）
    offset = srv - (t0 + t1) / 2.0
    return ("OK", srv, stratum, rtt_ms, offset, addr)


def main() -> int:
    hosts = sys.argv[1:] or ["172.16.0.2", "ntp.aliyun.com", "203.107.6.88"]
    print("NTP 可达性探针  |  本机时间 {}".format(
        time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())))
    print("-" * 74)
    any_ok = False
    for h in hosts:
        try:
            ip = socket.gethostbyname(h)
        except OSError as exc:
            print("{:<20} DNS 解析失败：{}".format(h, exc))
            continue
        res = query(h)
        if res[0] == "OK":
            _, srv, stratum, rtt, offset, addr = res
            any_ok = True
            print("{:<20} ✅ 可用".format(h))
            print("   解析 IP        : {}".format(ip))
            print("   服务器时间(UTC): {}".format(
                time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(srv))))
            print("   stratum        : {} ({})".format(
                stratum, STRATUM_NAME.get(stratum, "其它")))
            print("   往返延迟       : {:.1f} ms".format(rtt))
            print("   与本地钟粗偏差 : {:+.3f} s（含网络延迟，仅供参考）".format(offset))
        elif res[0] == "ERR":
            print("{:<20} ❌ 不可用：{}".format(h, res[1]))
        else:
            print("{:<20} ❌ 响应过短（{} 字节）".format(h, res[1]))
        print()
    print("-" * 74)
    print("结论：{}".format("至少有一个源可用" if any_ok else "全部不可用 —— 不要改 ESP32 的 NTP 源"))
    return 0 if any_ok else 1


if __name__ == "__main__":
    sys.exit(main())
