# 01_firmware/esp32c3_gateway —— ESP32-C3 RS-485 采集网关固件主工程

本目录是「ESP32-C3 替换树莓派网关项目」的固件主工程（Arduino IDE sketch）。
当前处于 **阶段二 · M1 环境搭建**：只放最小空壳，尚未写任何 WiFi / NTP / RS-485 / Modbus 业务代码。

## 目录结构

```
01_firmware/esp32c3_gateway/
├── esp32c3_gateway.ino     # sketch 主文件（空壳：setup 仅初始化 USB CDC，loop 空转）
├── config.h               # 非敏感常量（全部为占位符，M1 实测后填写）
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

UART1 参数（代码中按此调用，本阶段尚未接入）：

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

## 约束

- 本阶段不写 WiFi / NTP / RS-485 / Modbus 业务代码，不引入任何第三方库。
- `secrets.h` 永不入库；`config.h` 只放非敏感常量。
- 事实来源：`04_ai_prompts/00_context.md`（动手前必读）。
