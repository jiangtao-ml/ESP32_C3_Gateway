# 07 · Codex 提示词 —— 批次 3：常驻 AP + Web 配置页 + `/status` + NVS

> **使用方式**：把 `====` 之间整段复制给 Codex。
> **前置状态**：批次 2 已结案（commit `de51f59`，Flash `1005739` / RAM `39292`）；对齐蓝图 = `02_docs/17_参考实现逆向笔记_2026-10-05.md`（老师固件 1.5.0 的配置页 / `/status` 22 项 / NVS）。
> **目标**：功能对齐老师版的"配置能力"——照他的路由名与字段名做（生态兼容，将来他的 `esp485_config.py` 可直接管我们的设备），叠加我们已有的 `dev`/`mode` 体系。

====

## 0. 任务一句话

给固件加上：**常驻 SoftAP + Web 配置页（4 卡片，Basic Auth 保护写操作）+ `/status` JSON（老师 22 项 + 我方追加）+ NVS 配置持久化（保存立即生效，无需重启）**。485 采集状态机、UDP JSON 上行**一行不改**，只在其外面包一层"配置层"。

## 1. 对齐蓝图（来自 `02_docs/17`，老师版逆向）

- 配置页 4 卡片（中文 `lang="zh-CN"`、移动端 viewport）：
  ① 接入的局域网热点（SSID / 密码 / 「该网络无密码」勾选）
  ② NTP 与数据接收（Chrony 地址 / UDP 接收地址 / UDP 端口）
  ③ 采集周期（秒，**1–5**）
  ④ 管理账号（`admin_user` + `admin_pass`，**留空 = 保持不变**）
- 路由：`POST /save`（`x-www-form-urlencoded`）、`GET /reset`（页面带 `confirm()`）、`GET /status`
- 校验提示风格：`invalid port (1-65535)`、`poll period (1-5s)`、`不一致或非法, 未改动配置`
- 自述行为：「保存后立即生效、无需重启」「配置写入闪存，掉电不丢」「设备热点 …（密码 …）始终在线」
- `/status` 22 项（老师字段，逐字对齐）：
  `mac / fw / configured / wifi_ssid_cfg / wifi_from_builtin / chrony / server / port / poll_period_s / wifi_connected / ssid / ip / rssi / ap / ap_ip / time_synced / uptime_ms / poll_ok / poll_err / udp_tx / queue / dropped`
- AP **常开**（老师自述"始终在线"）；AP 密码是**运行时变量（NVS）**，不是硬编码。

## 2. 实现裁定（本项目口径，与老师版的差异都写在这里）

| # | 项 | 裁定 |
| --- | --- | --- |
| 1 | **AP SSID / 密码** | SSID = `ESP485-<MAC后6位>`（与 `dev` 一致）；密码 = NVS `ap_pass`，**无值时用 MAC 派生默认**（算法：`"485" + MAC后4位十六进制`，仅算法入代码）。⚠️ **任何真实密码不得出现在文档/提交里** |
| 2 | **鉴权** | `POST /save` 与 `GET /reset` 走 **HTTP Basic Auth**（账号 = NVS `admin_user/admin_pass`，默认 `admin` / MAC 派生同上）；`GET /`（页面）与 `GET /status` **免鉴权**（照老师现状，作为已知口子记录，M7 再收紧）。`/status` **绝不含任何密码字段** |
| 3 | **NVS** | `Preferences`，命名空间 `cfg485`。key：`wifi_ssid / wifi_pass / wifi_open / ntp1 / udp_ip / udp_port / poll_s / admin_user / admin_pass / ap_pass`。**读法：NVS 有值用 NVS，无值回落编译期默认**（`secrets.h` 的 WiFi、`config.h` 的 NTP/UDP/周期）。`configured` = NVS 是否存过任意配置 |
| 4 | **运行时化改造**（编译期宏 → 变量） | ① `UDP_TARGET_IP/PORT` → `g_udpTargetIp[16] / g_udpTargetPort`（`udpSendLine()` 改用）；② `POLL_INTERVAL_MS` → `g_pollIntervalMs`（宏保留为**默认值**；ST_SEND 的节拍累加改用变量）；③ NTP 主源 → `g_ntpPrimary[48]`（备源 NTP_SERVER_2/3 不变；改主源后重新 `configTzTime` + 置 `g_ntpResyncPending`）。④ WiFi 凭据：`WiFi.begin()` 改用"NVS 或默认"的运行时值 |
| 5 | **保存立即生效（无需重启）** | ① WiFi 凭据变 → `WiFi.disconnect()` + 新凭据 `WiFi.begin()`（断连期间上行自然跳过，现有 `g_linkUp` 逻辑覆盖）；② UDP 目标变 → 新值即刻用于 `beginPacket`；③ 周期变 → 下轮节拍生效；④ chrony 变 → 重新触发 SNTP。**注意静态 IP 不在 Web 配置范围**（LOCAL_IP/GATEWAY 仍编译期，STA 动态网段适配留后续） |
| 6 | **`/status` 追加字段** | 老师 22 项**逐字保留**，末尾追加我方 3 项：`"dev"`（ESP485-XXXXXX）、`"mode"`（"full"/"3seg"，与心跳行一致）、`"free_heap"`。`fw` 值 = 新宏 `FW_VERSION "0.6.0-batch3"`（config.h 定义） |
| 7 | **表单字段名** | 仅 `admin_user` / `admin_pass` 经逆向确认；其余卡片字段名自定（建议 `ssid/pass/open/ntp1/server/port/poll`），**集中定义并注释**"老师表单 name 待其工具 `esp485_config.py` 到手后再对齐" |
| 8 | **页面资源** | HTML 存 **PROGMEM**（`server.send_P`），单页 ~4–5 KB；**不引第三方库**，用 ESP32 core 自带 `WebServer.h` + `Preferences.h` |
| 9 | **`GET /reset`** | Basic Auth 通过后：`prefs.clear()`（保留出厂态）→ 串口打印 → `ESP.restart()`。页面按钮带 `confirm()` |
| 10 | **明确不做**（后续批次/商业版） | OTA、captive portal、`esp485_config.py`、HTTPS、AP 超时关闭/按键开启（照老师常开，口子已记录于 `02_docs/17` §7） |

## 3. 改动清单

### config.h
1. 新增 `FW_VERSION "0.6.0-batch3"`、`AP_DEFAULT_PREFIX "ESP485-"`、NVS 命名空间常量、`POLL_PERIOD_MIN_S 1 / MAX_S 5`。
2. `POLL_INTERVAL_MS` 注释改为"默认轮询周期（NVS `poll_s` 可覆盖，1–5s）"。
3. 其余宏不动。

### esp32c3_gateway.ino（或新增 `web_config.h`/`web_config.cpp` 放在 sketch 目录——sketch 单目录，建议拆成 `web_config.h` 被 .ino include）
1. **配置层**：启动时 `loadConfig()`（NVS → 运行时变量，无值回落默认）；
2. **AP 启动**：`WiFi.mode(WIFI_AP_STA)` + `softAP(apSsid, apPass)`（在 WiFi STA 配置之后）；AP 信息打印到串口（SSID 可打，**密码不得打印**）；
3. **Web 路由**：`GET /`（页面）、`POST /save`（auth + 校验 + NVS 写 + 运行时生效 + 回"已保存"页或 302 回 `/`）、`GET /reset`（auth + confirm 由页面负责）、`GET /status`（22+3 项 JSON）；
4. **loop 挂载**：`server.handleClient()` 加进 `loop()`（非阻塞；不得影响 1s 轮询节拍）；
5. **心跳行**：保持批次 2 的 `mode` 字段（**不加**老师的心跳 `cfg` 字段——其含义逆向未确认，问清老师再对齐）。

## 4. 验收判据

### 4.1 编译
- 0 error / 0 warning；报 Flash/RAM 与批次 2 基线（`1005739` / `39292`）差值（WebServer+Preferences 预期 +80~200 KB，超出需说明）。
- 回环模式（`RS485_MODE_LOOPBACK_SELFTEST=1`）仍编译通过。

### 4.2 台面自测（无 485 也做）
1. 上电后能扫到热点 `ESP485-XXXXXX`；连接后访问 `http://192.168.4.1/` 见配置页；
2. 同一局域网内访问 `http://192.168.1.200/` 也能见（AP/STA 双接口）；
3. `GET /status` 返回 22+3 项 JSON（无密码字段）；
4. `POST /save` 无鉴权 → 401；带 Basic Auth → 保存成功且串口打印 `[CFG]` 行；
5. 改 UDP 目标为另一台电脑 IP → **立即**在新目标收到 UDP（无需重启）；
6. 改采集周期 2 → 实测 2s/轮；改回 1 → 1s/轮；
7. 断电重启 → 配置保持（NVS）；`GET /reset` → 恢复默认并重启；
8. `admin_pass` 留空保存 → 密码不变；
9. **轮询回归**：整个测试期间 485 轮询照常（悬空态 err 行 / 心跳 `poll_err` 连续），`setTxTimeoutMs(0)` 在位。

### 4.3 交付物
diff（config.h + .ino + 新增 web_config.h）· 编译输出 · §4.2 自测记录（含 /status 完整 JSON 样例）· **不自审**，交独立复核。

## 5. 硬约束（红线）

1. 485 状态机 / UDP JSON 字段与顺序 / seq 与计数器 / 三级超时 / 帧长驱动 **一行不改**；
2. **任何真实凭据/密码不写入代码提交、文档、串口日志**（默认密码只允许 MAC 派生算法）；
3. 非阻塞：`server.handleClient()` 之外不得引入阻塞调用；loop 内不得 `delay()`;
4. 只读不写（485 侧不变）；`setTxTimeoutMs(0)` 保留；批次 2 的运行时自适应逻辑不动；
5. 提交信息注明"配置页路由与 /status 字段名对齐老师版（依据 `02_docs/17`）"。

====

## 附：WorkBuddy 侧配套（不由 Codex 做）

- 真机/台面验证操作卡（AP 连接 + /save 生效 + NVS 掉电保持的步骤与判据）。
- 复核重点：Basic Auth 覆盖面、NVS 读写边界、运行时化改造不破坏轮询节拍、`/status` 无敏感字段。
- "问老师"清单追加：心跳 `cfg` 字段含义、`esp485_config.py` 表单字段名（拿到后对齐）。
