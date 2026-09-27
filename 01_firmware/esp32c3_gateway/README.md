# 01_firmware/esp32c3_gateway —— ESP32-C3 RS-485 采集网关固件主工程

本目录是「ESP32-C3 替换树莓派网关项目」的固件主工程（Arduino IDE sketch）。
当前处于 **阶段二 · M2（WiFi + NTP + UDP）**：已实现联网、SNTP 对时、每秒 1 包 UDP 心跳与最小非阻塞状态机骨架；**尚未写任何 RS-485 / Modbus 代码**（M3 起）。

## 目录结构

```
01_firmware/esp32c3_gateway/
├── esp32c3_gateway.ino     # sketch 主文件（M2：WiFi + NTP + UDP + 最小非阻塞状态机）
├── config.h               # 非敏感常量（电脑/自身 IP、端口、NTP、周期；已落实为实值）
├── secrets.h.example      # 凭据模板（占位符，不含真实值）
├── secrets.h              # ← 本地自建，不入库（.gitignore 已忽略）
└── README.md              # 本文件
```

sketch 目录名必须与 .ino 文件名一致（`esp32c3_gateway`），否则 Arduino IDE 无法识别。

## 首次搭建

1. 复制凭据模板并填写真实值（本文件不入库）：

   ```powershell
   Copy-Item .\secrets.h.example .\secrets.h
   ```

   然后编辑 `secrets.h`，把 `<占位符>` 换成实际 SSID / 密码。

2. 用 Arduino IDE 打开 `esp32c3_gateway.ino`。

## 构建方式（Arduino IDE）

| 项 | 设置 |
| --- | --- |
| 开发板 | ESP32C3 Dev Module（ESP32-C3 SuperMini） |
| **USB CDC On Boot** | **Enabled** ← 必需，否则 `Serial` 无输出 |
| Flash Size | 4MB (32Mb) |
| Upload Speed | 921600（不稳定时降到 115200） |
| Port | COM 号待 M1 查询（见根目录 README 第 8 节） |
| CPU Frequency | 160MHz |

关键设置：**USB CDC On Boot = Enabled**（此项是 `Serial` 有输出的前提）。

UART1 参数（**M3 才接入**，当前代码中未调用）：

```cpp
Serial1.begin(9600, SERIAL_8N1, 20, 21);   // RX=GPIO20, TX=GPIO21，9600 8N1
```

- 引脚：ESP32-C3 的 `HardwareSerial(1)` 可映射到 GPIO20/21（SuperMini 上无 USB/Flash 冲突）。
- `HardwareSerial(1)` 与 USB CDC（`Serial`）相互独立，可同时使用。
- RX 侧电平：MAX13487 的 RXO 若为空闲 5V，必须先 1kΩ/2kΩ 分压到约 3.33V 再接 GPIO20（M1 实测后再接线，见根目录 README 第 6 节）。

## M1 最小 UART1 回环自检（不接 MAX13487）

目的：先证明「串口能自发自收」，再去碰 485 收发器。

1. **断电**，用一根杜邦线把 **GPIO21（TX1）与 GPIO20（RX1）短接**。
2. 确认 **USB 电源与电脑共地**（此时 485 模块可不接）。
3. 烧录固件，打开串口监视器（波特率 **115200**）。
4. 临时在 sketch 中加入自检片段（仅本地验证，验证完删除或改为 `#if 0`）：

   ```cpp
   Serial1.begin(9600, SERIAL_8N1, 20, 21);
   Serial1.write(0x55);
   delay(5);
   if (Serial1.available()) {
     Serial.printf("LOOPBACK OK: 0x%02X\n", Serial1.read());
   } else {
     Serial.println("LOOPBACK FAIL: no byte received");
   }
   ```

5. **预期结果**：串口打印 `LOOPBACK OK: 0x55`。
   - 打印 `FAIL` → 检查短接线是否插在 GPIO20/21、USB CDC On Boot 是否为 Enabled。
6. 自检通过后**拔掉短接线**，再进入 485 收发器接线环节。

> ⚠️ 回环自检必须**先断开 MAX13487**，避免总线偏置/终端电阻干扰判断。

## M2：WiFi + NTP + UDP 联通验证

前提：本目录已有 `secrets.h`（含真实 SSID / 密码）；`config.h` 的 `UDP_TARGET_IP` / `LOCAL_IP` 与实测一致。

1. **电脑侧**：打开 **NetAssist（网络调试助手）**，协议选 **UDP**，本地端口填 **8888**（= `UDP_TARGET_PORT`），点「打开」。
2. 确认电脑无线网卡 IPv4 仍为 `192.168.1.117`（`ipconfig /all`）；若已变化，先改 `config.h` 再烧录。
3. 烧录固件并打开串口监视器（115200）。预期串口输出：

```text
[WIFI] 静态配置 IP=192.168.1.200 GW=192.168.1.1 MASK=255.255.255.0 DNS=192.168.1.1
[WIFI] 已发起连接（非阻塞），等待 wifiTick() 报告结果
[UDP] 本地端口 12345，目标 192.168.1.117:8888
[BOOT] M2 骨架就绪：WiFi + NTP + UDP + 非阻塞状态机
[WIFI] 已连接 SSID=xxx RSSI=-55 dBm IP=192.168.1.200 GW=192.168.1.1
[NTP] WiFi 已连上 -> 触发 SNTP 对时 TZ=CST-8 主=ntp.aliyun.com 备=203.107.6.88
[NTP] 时间已同步：本地 2026-09-27 20:15:03 +0800（TZ=CST-8，源 ntp.aliyun.com / 备选 203.107.6.88）
[UDP -> 192.168.1.117:8888] BOOT+5.012 | UNSYNCED
[UDP -> 192.168.1.117:8888] 2026-09-27 20:15:01 | M2_HEARTBEAT
```

4. **判定标准**（对应任务书 §9.1 M2）：
   - NetAssist 接收区**每秒出现 1 行**，格式 `YYYY-MM-DD HH:MM:SS | M2_HEARTBEAT`；
   - 对时完成前为 `BOOT+<秒>.<毫秒> | UNSYNCED`，对时完成后自动切换为墙钟时间；
   - **时区自检**：串口 `[NTP] 时间已同步` 行末尾应为 `+0800`（东八区）。若显示 `+0000`，说明 `NTP_TZ_INFO` 未生效（见 config.h 中关于不可写 `"<CST-8>"` 的注释）；
   - 连续 ≥60s 不断包（**从第 2 个包开始计时**，规避 ARP 首包丢失）。
5. **断网自恢复验证**：断开路由器 2.4GHz 约 30s 再恢复 → 串口应出现 `[WIFI] 连接断开` → `[WIFI] 第 N 次重连` → `[WIFI] 已连接` → `[NTP] WiFi 已连上 -> 触发 SNTP 对时`，且 **≤60s 内恢复发包**。

> 排查提示：NetAssist 收不到包 → 先看串口有无 `[WIFI] 已连接`（未连上则看 SSID 是否 2.4GHz）；再看 NetAssist 端口是否 8888 且已「打开」；最后确认电脑 IPv4 未变。

## 约束

- RS-485 / Modbus 业务代码尚未编写（M3 起）；不引入任何第三方库（只用 ESP32 core 自带 WiFi / WiFiUdp / time）。
- 全程 `millis()` 非阻塞调度，**禁止 `delay()`**。
- `secrets.h` 永不入库；`config.h` 只放非敏感常量。
- 事实来源：`04_ai_prompts/00_context.md`（动手前必读）。
