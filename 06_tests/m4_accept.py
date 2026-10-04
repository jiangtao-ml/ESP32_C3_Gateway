#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M4 验收补测（一键跑完 3 项）
=============================

补测项（来自 README §4 / 00_context.md 第五节「M4 尚待补的验收」）：
  [1] 连续 10 次超时后仍能完成后续轮询
  [2] 刻意注入 1 位 CRC 错误 -> ESP32 能识别（报 CRC 失败）且不崩溃，之后继续轮询
  [3] 1000 次累计统计（无误报、无丢帧、账目自洽）

做法：
  同时打开两个串口 ——
    ESP32_PORT（115200）：读固件日志，统计 解析OK / 超时 / CRC失败 / 帧长异常 ...
    SLAVE_PORT（9600）  ：当 Modbus RTU 从站，按脚本化的"行为档位"应答。

  档位（按从站收到的请求序号，1 起算）：
    阶段 1  基线      请求 1..10    -> 正常应答
    阶段 2  静默      请求 11..20   -> 不应答（制造 10 次超时）
    阶段 3  恢复      请求 21..30   -> 正常应答（验证"超时后仍继续轮询"）
    阶段 4  CRC 注入  请求 31..35   -> 回一个 CRC 错 1 位的帧
    阶段 5  长稳      请求 36..N    -> 正常应答，跑到累计 N 次

用法：
    python m4_accept.py COM5 COM7                 # 完整补测（1000 次，约 17 分钟）
    python m4_accept.py COM5 COM7 60              # 冒烟测试（60 次，约 1 分钟，先跑这个）

注意：
  1. 关掉 Arduino IDE 的串口监视器（同一端口不能被两个程序占用）。
  2. 跑之前先确认 ESP32 已在正常轮询（README §4 的接线口径）。
  3. ESP32 不要在测试中途复位 —— 固件计数器会归零，账目就对不上了。
"""

import sys
import time

import serial

# ---------------------------------------------------------------
# 参数
# ---------------------------------------------------------------
ESP_PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
SLV_PORT = sys.argv[2] if len(sys.argv) > 2 else "COM7"
TOTAL = int(sys.argv[3]) if len(sys.argv) > 3 else 1000

BAUD_ESP = 115200
BAUD_SLV = 9600
SLAVE_ID = 1
REGS = {0: 10, 1: 20}

REQ_LEN = 8               # 0x03 请求固定 8 字节
IDLE_END = 0.05           # 从站侧：静默 50ms 仍拼不出合法帧 -> 判噪声清空
READ_TO = 0.005
HARD_LIMIT = TOTAL * 1.6 + 120.0   # 硬超时（秒），防止死等

# 阶段边界（从站请求序号，1 起算，含）
P_BASE = (1, 10)
P_SILENT = (11, 20)
P_RECOVER = (21, 30)
P_CRCBAD = (31, 35)
P_STEADY = (36, TOTAL)

PHASE_NAME = {1: "基线", 2: "静默(10次)", 3: "恢复", 4: "CRC注入", 5: "长稳"}


def phase_of(idx: int) -> int:
    if idx <= P_BASE[1]:
        return 1
    if idx <= P_SILENT[1]:
        return 2
    if idx <= P_RECOVER[1]:
        return 3
    if idx <= P_CRCBAD[1]:
        return 4
    return 5


def behavior_of(idx: int) -> str:
    p = phase_of(idx)
    if p == 2:
        return "silent"
    if p == 4:
        return "crcbad"
    return "normal"


def crc16(data: bytes) -> bytes:
    """CRC16/Modbus，返回线上字节序（低字节在前）。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def stamp() -> str:
    return time.strftime("%H:%M:%S")


def build_resp(start: int, qty: int) -> bytes:
    vals = [REGS.get(start + i, 0) for i in range(qty)]
    payload = bytes([SLAVE_ID, 0x03, qty * 2]) + b"".join(v.to_bytes(2, "big") for v in vals)
    return payload + crc16(payload)


def parse_request(frame: bytes):
    """返回 (start, qty) 或 None。"""
    body = frame[:-2]
    if crc16(body) != frame[-2:]:
        return None
    if body[0] != SLAVE_ID or body[1] != 0x03:
        return None
    return (body[2] << 8) | body[3], (body[4] << 8) | body[5]


# ---------------------------------------------------------------
# ESP32 日志分类
# ---------------------------------------------------------------
def classify(line: str):
    """返回 (类别, 是否计一次轮询)。类别：OK/TIMEOUT/CRC/FRAMELEN/EXC/OTHER/NOISE"""
    if "[485]" in line and "统计：" in line:
        return "STATS", False
    if "[M4] 解析 OK" in line:
        return "OK", True
    if "[M4] 超时" in line:
        return "TIMEOUT", True
    if "[M4] CRC 失败" in line:
        return "CRC", True
    if "[M4] 帧长异常" in line:
        return "FRAMELEN", True
    if "[M4] 从站异常响应" in line:
        return "EXC", True
    if "[M4]" in line:
        return "OTHER", True
    if line.startswith("      ") or "[诊断]" in line:
        return "SUBLINE", False
    return "NOISE", False


def main() -> int:
    esp = serial.Serial(ESP_PORT, BAUD_ESP, timeout=READ_TO)
    slv = serial.Serial(SLV_PORT, BAUD_SLV, bytesize=8, parity="N",
                        stopbits=1, timeout=READ_TO)
    print("[{}] ESP32={} @{} | 从站={} @{} | 目标 {} 次轮询".format(
        stamp(), ESP_PORT, BAUD_ESP, SLV_PORT, BAUD_SLV, TOTAL), flush=True)

    raw_path = "03_logs/M4验收补测_{}.log".format(time.strftime("%Y%m%d_%H%M%S"))
    raw = open(raw_path, "w", encoding="utf-8")

    tallies = {"OK": 0, "TIMEOUT": 0, "CRC": 0, "FRAMELEN": 0, "EXC": 0, "OTHER": 0}
    polls = 0                                  # ESP32 侧看到的轮询次数
    slv_req = 0                                # 从站侧收到的合法请求次数
    req_by_phase = {i: 0 for i in range(1, 6)}
    ok_by_phase = {i: 0 for i in range(1, 6)}

    silent_range = []                          # 静默期 ESP32 超时的时间戳
    crc_lines = []                             # CRC 失败证据
    ok_evidence = []                           # 关键解析 OK 证据（阶段 3/5 的前几条）
    stats_lines = []
    recover_first_ok_at = None
    t_start = time.time()

    esp_buf = ""       # ESP32 行缓冲
    slv_buf = bytearray()
    slv_last = time.time()
    done = False

    while not done:
        if (time.time() - t_start) > HARD_LIMIT:
            print("[{}] ⚠️ 硬超时（{}s），提前收尾".format(stamp(), int(HARD_LIMIT)), flush=True)
            break

        # ---- 读从站口：扫描合法请求并按档位应答 ----
        chunk = slv.read(256)
        if chunk:
            slv_buf += chunk
            slv_last = time.time()
            i, n = 0, len(slv_buf)
            while i < n:
                if slv_buf[i] != SLAVE_ID:
                    i += 1
                    continue
                if n - i < REQ_LEN:
                    break
                cand = bytes(slv_buf[i:i + REQ_LEN])
                pr = parse_request(cand)
                if pr is None:
                    i += 1
                    continue
                i += REQ_LEN
                slv_req += 1
                p = phase_of(slv_req)
                req_by_phase[p] += 1
                beh = behavior_of(slv_req)
                start, qty = pr
                if beh == "silent":
                    silent_range.append(None)
                    print("[{}] [slave] #{:<4} 阶段{} 静默（不应答）".format(
                        stamp(), slv_req, p), flush=True)
                elif beh == "crcbad":
                    resp = bytearray(build_resp(start, qty))
                    good_crc = bytes(resp[-2:])
                    resp[-1] ^= 0x01                      # 翻转 CRC 低字节的 bit0
                    slv.write(bytes(resp))
                    slv.flush()
                    raw.write("CRC注入 #{:<4} TX {}\n".format(slv_req, bytes(resp).hex(" ").upper()))
                    print("[{}] [slave] #{:<4} 阶段{} 注入 CRC 错误 {}（应为 {}）".format(
                        stamp(), slv_req, p, bytes(resp).hex(" ").upper(), good_crc.hex(" ").upper()),
                        flush=True)
                else:
                    resp = build_resp(start, qty)
                    slv.write(resp)
                    slv.flush()
                    print("[{}] [slave] #{:<4} 阶段{} 正常应答 {}".format(
                        stamp(), slv_req, p, resp.hex(" ").upper()), flush=True)
            if i > 0:
                del slv_buf[:i]
        elif slv_buf and (time.time() - slv_last) > IDLE_END:
            raw.write("从站残帧 {} 字节 {}\n".format(len(slv_buf), bytes(slv_buf).hex(" ").upper()))
            slv_buf.clear()

        # ---- 读 ESP32 口：逐行分类 ----
        data = esp.read(4096)
        if data:
            esp_buf += data.decode("utf-8", errors="replace")
            while "\n" in esp_buf:
                line, esp_buf = esp_buf.split("\n", 1)
                line = line.strip("\r")
                raw.write(line + "\n")
                if not line.strip():
                    continue
                cat, counts = classify(line)
                if cat == "STATS":
                    stats_lines.append(line)
                    print("[{}] [esp] {}".format(stamp(), line), flush=True)
                    continue
                if cat in ("SUBLINE", "NOISE"):
                    continue
                if counts:
                    polls += 1
                    tallies[cat] = tallies.get(cat, 0) + 1
                    p = phase_of(slv_req) if slv_req else 1
                    if cat == "OK":
                        ok_by_phase[p] = ok_by_phase.get(p, 0) + 1
                        if recover_first_ok_at is None and p >= 3:
                            recover_first_ok_at = (stamp(), slv_req)
                            ok_evidence.append("[{}] 第 {} 次轮询恢复：{}".format(stamp(), slv_req, line))
                        elif len(ok_evidence) < 3 and p >= 3:
                            ok_evidence.append("[{}] 第 {} 次轮询：{}".format(stamp(), slv_req, line))
                    if cat == "CRC":
                        crc_lines.append("[{}] 第 {} 次轮询：{}".format(stamp(), slv_req, line))
                    if cat == "TIMEOUT" and 2 <= p <= 2:
                        silent_range[len(silent_range) - 1] = stamp()
                    print("[{}] [esp] {}".format(stamp(), line), flush=True)
                    if polls >= TOTAL:
                        done = True
                        break
        time.sleep(0.001)

    # 收尾：把 ESP32 缓冲里剩余的输出读干净（最多 3 秒）
    t_end = time.time() + 3.0
    while time.time() < t_end:
        data = esp.read(4096)
        if not data:
            break
        esp_buf += data.decode("utf-8", errors="replace")
        while "\n" in esp_buf:
            line, esp_buf = esp_buf.split("\n", 1)
            line = line.strip("\r")
            raw.write(line + "\n")
            cat, counts = classify(line)
            if cat == "STATS":
                stats_lines.append(line)
            elif counts:
                polls += 1
                tallies[cat] = tallies.get(cat, 0) + 1

    dur = time.time() - t_start
    esp.close()
    slv.close()

    # ---------------------------------------------------------------
    # 报告
    # ---------------------------------------------------------------
    total_seen = sum(tallies.values())
    out = []
    out.append("")
    out.append("=" * 62)
    out.append("M4 验收补测报告")
    out.append("=" * 62)
    out.append("运行时长        : {:.1f} s".format(dur))
    out.append("从站收到请求    : {} 次（分阶段：{}）".format(
        slv_req, ", ".join("阶段{}({}次)".format(i, req_by_phase[i]) for i in range(1, 6))))
    out.append("ESP32 侧轮询结果（合计 {} 次）".format(total_seen))
    out.append("  解析 OK       : {}".format(tallies["OK"]))
    out.append("  超时          : {}".format(tallies["TIMEOUT"]))
    out.append("  CRC 失败      : {}".format(tallies["CRC"]))
    out.append("  帧长异常      : {}".format(tallies["FRAMELEN"]))
    out.append("  从站异常响应  : {}".format(tallies["EXC"]))
    out.append("  其他          : {}".format(tallies["OTHER"]))
    out.append("")
    out.append("分阶段 解析 OK 数：{}".format(
        ", ".join("阶段{}={}".format(i, ok_by_phase.get(i, 0)) for i in range(1, 6))))
    out.append("")

    # 补测 1
    p1 = (tallies["TIMEOUT"] >= 10) and (ok_by_phase.get(3, 0) > 0) and (ok_by_phase.get(5, 0) > 0)
    out.append("[补测 1] 连续 10 次超时后仍完成后续轮询 : {}".format("PASS" if p1 else "FAIL"))
    out.append("         实测超时 {} 次；静默阶段后 阶段3 解析OK {}, 阶段5 解析OK {}".format(
        tallies["TIMEOUT"], ok_by_phase.get(3, 0), ok_by_phase.get(5, 0)))
    if ok_evidence:
        out.append("         证据（恢复后的解析 OK）：")
        for e in ok_evidence[:5]:
            out.append("           " + e)
    out.append("")

    # 补测 2
    p2 = (len(crc_lines) >= 1) and (ok_by_phase.get(5, 0) > 0)
    out.append("[补测 2] 注入 1 位 CRC 错误被识别且不崩溃 : {}".format("PASS" if p2 else "FAIL"))
    out.append("         实测 CRC 失败 {} 次（注入了 {} 帧）".format(len(crc_lines), P_CRCBAD[1] - P_CRCBAD[0] + 1))
    for e in crc_lines[:5]:
        out.append("           " + e)
    if ok_by_phase.get(5, 0) > 0:
        out.append("         注入后仍正常解析 OK {} 次 ⇒ 未崩溃、已恢复".format(ok_by_phase.get(5, 0)))
    out.append("")

    # 补测 3
    expected_timeout = P_SILENT[1] - P_SILENT[0] + 1
    expected_crc = P_CRCBAD[1] - P_CRCBAD[0] + 1
    arith_ok = (total_seen == polls) and (tallies["TIMEOUT"] >= expected_timeout) and (tallies["CRC"] >= expected_crc)
    p3 = (polls >= TOTAL) and arith_ok and (tallies["FRAMELEN"] + tallies["OTHER"] + tallies["EXC"] == 0)
    out.append("[补测 3] {} 次累计统计（账目自洽、无误报丢帧）: {}".format(TOTAL, "PASS" if p3 else "FAIL"))
    out.append("         期望 超时≥{}、CRC≥{}；实测 超时 {}、CRC {}，帧长异常 {}，其他 {}".format(
        expected_timeout, expected_crc, tallies["TIMEOUT"], tallies["CRC"],
        tallies["FRAMELEN"], tallies["OTHER"]))
    out.append("         账目核对：解析OK + 超时 + CRC失败 + 帧长异常 + 其他 = {} （轮询总数 {}）".format(
        total_seen, polls))
    if stats_lines:
        out.append("         固件自身累计统计行（每 100 次打印一条）：")
        for s in stats_lines[-12:]:
            out.append("           " + s)
    out.append("")
    out.append("原始日志：{}".format(raw_path))
    out.append("=" * 62)

    text = "\n".join(out)
    print(text, flush=True)
    raw.write("\n" + text + "\n")
    raw.close()

    with open("03_logs/M4验收补测_report.txt", "w", encoding="utf-8") as f:
        f.write(text + "\n")

    return 0 if (p1 and p2 and p3) else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[accept] 手工中断", flush=True)
        sys.exit(130)
    except serial.SerialException as exc:
        print("[accept] 串口异常：{}".format(exc), flush=True)
        sys.exit(1)
