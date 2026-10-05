/*
 * esp32c3_gateway.ino
 * ESP32-C3 RS-485 采集网关（替换树莓派网关项目）
 *
 * 当前里程碑：M4 —— 真实 485 总线收发 + Modbus RTU 解析
 *
 * 已实现：WiFi 连接与最小自恢复、SNTP 对时（含 UNSYNCED 降级）、每秒 1 包 UDP 心跳上行、
 *        每秒一轮 Modbus RTU 请求/响应：帧长驱动接收 + 三级超时、地址/功能码匹配、
 *        CRC16/Modbus 校验、数据域提取（M4 响应帧固定 9 字节 = 读 2 个保持寄存器）
 * 未实现：QI 真机协议（功能码 0x04 读输入寄存器、39 寄存器分段读取）—— 留待 M5；
 *        M4 只用 0x03 报文在「电脑端 Modbus 从站」上验证物理链路与解析正确性
 *
 * 硬约束：
 *   - 全程 millis() 非阻塞调度，禁止 delay()（唯一例外：setup() 内有界等待 USB CDC 枚举，最多 2s）
 *   - WiFi 凭据只来自 secrets.h（不入库），本文件不出现任何凭据明文
 *   - 不引入第三方库（仅用 ESP32 core 自带的 WiFi / WiFiUdp / time / HardwareSerial）
 *   - ⚠️ USB CDC On Boot 必须 = Enabled：否则 Serial 会落到 UART0（GPIO20/21），
 *     与 UART1 争用同一对引脚
 *   - ⚠️ 485 模块的 RXO 为 5V 时必须先 1kΩ/2kΩ 分压再进 GPIO20
 *   - ⚠️ EN 接法随实物主芯片型号而定（接线前先读 8 脚芯片丝印）：
 *     MAX13487 / MAX13488（5V 双向，芯片无 DE 脚）-> EN 接固定高电平（模块 VCC/5V），
 *       不得悬空 / 接 GND / 动态切换（悬空若对应 SHDN# 会关断器件，症状即 RX=0）；
 *     MAX485 / MAX3485（单向，EN 即方向控制脚）-> EN 必须接 GPIO 由固件切换，严禁固定接高。
 *       固件已内建方向控制：把 config.h 的 RS485_DE_PIN 设为实际 GPIO 号即启用（默认 -1 = 不启用）。
 *     见 00_context.md 三.5。
 *   - ⚠️【待复核·高优先】实物 485 模块主芯片型号：用户口述为 MAX13487，但商品页为三合一款
 *     （MAX3485 / MAX485 / MAX13487）、渲染图丝印为 MAX485 ESA。若复核为单向芯片，则
 *     "AutoDirection 无需方向控制"这一前提不成立，必须增加方向控制 GPIO 与收/发切换时序。
 *
 * 事实来源：04_ai_prompts/00_context.md
 * 编译设置：Arduino IDE -> Tools -> USB CDC On Boot = Enabled
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include <lwip/apps/sntp.h>   // sntp_getserver() / sntp_getreachability()：读取实际生效的 NTP 源（ESP32 core 自带，非第三方库）

#include "config.h"
#include "secrets.h"

// ---- secrets.h 编译期守卫：缺文件或未配置时直接编译失败，避免"能烧但连不上" ----
#ifndef WIFI_SSID
  #error "secrets.h 缺失或未配置"
#endif
#ifndef WIFI_PASSWORD
  #error "secrets.h 缺失或未配置（WIFI_PASSWORD 未定义）"
#endif

// ---------------------------------------------------------------
// 最小非阻塞状态机（M2 起在位；M6 只做加固，不重构）
//   IDLE -> TX -> WAIT_RX -> PARSE -> SEND -> IDLE
//   M2 起：IDLE/SEND（UDP 心跳）；M3 起：TX/WAIT_RX/PARSE（UART1 收发）
//   M4：TX 发 Modbus 请求帧；WAIT_RX 帧长驱动 + 三级超时；PARSE 做真实 Modbus RTU 解析
// ---------------------------------------------------------------
enum GatewayState : uint8_t {
  ST_IDLE = 0,   // 等待 1s 定时到点
  ST_TX,         // 向 UART1 发送 Modbus 请求帧（写前清 RX -> write -> flush）
  ST_WAIT_RX,    // 等响应：首字节 80ms / 字符间隔 4ms / 帧长驱动 / 整帧兜底 300ms
  ST_PARSE,      // 帧长 + 地址 + 功能码 + CRC16/Modbus 校验，提取数据域
  ST_SEND        // UDP 上行（M2 心跳逻辑保留）
};

static GatewayState g_state            = ST_IDLE;
static uint32_t     g_nextPollMs       = 0;   // 下一次 1s 到点的绝对时刻
static uint32_t     g_lastWifiCheckMs  = 0;
static uint32_t     g_lastReconnectMs  = 0;
static uint32_t     g_reconnectCount   = 0;
static uint32_t     g_lastNtpCheckMs   = 0;
static uint32_t     g_lastNtpTriggerMs = 0;   // 上次触发 configTzTime 的时刻（未同步时按 NTP_RETRY_INTERVAL_MS 重试）
static bool         g_linkUp           = false;  // 当前 WiFi 是否已连上
static bool         g_everConnected    = false;  // 是否曾经连上过（决定首次 begin 失败后的重试方式）
static bool         g_timeSynced       = false;
static bool         g_ntpResyncPending = false;  // 重连成功后置位，下一轮重新触发 SNTP
static bool         g_ntpTriggeredThisSession = false;  // 本轮会话是否已发起过 SNTP 对时（「已同步」判据之一）
static uint32_t     g_ntpStatusReprintMs      = 0;      // 同步确认后补打一次源状态的计划时刻（0 = 无需补打）

static WiFiUDP g_udp;
static char    g_payload[384];   // M5 最坏情况约 255 字节（时间戳 19 + " | " 3 + 39 寄存器 HEX 233），留足余量

// ---------------------------------------------------------------
// RS-485 收发缓冲（M4）
//   TX：Modbus 请求帧，固定 MODBUS_TEST_FRAME_LEN 字节
//   RX：响应帧。两个尺寸不同（发 8 收 9），必须是两个独立的缓冲，不能共用。
// ---------------------------------------------------------------
static const uint8_t MODBUS_TEST_FRAME_BYTES[] = MODBUS_TEST_FRAME;
static const uint8_t MODBUS_TX_LEN             = MODBUS_TEST_FRAME_LEN;

// 帧长驱动的目标长度：总线模式 = 响应帧长；回环模式 = 请求帧长（发 8 收 8）
#if RS485_MODE_LOOPBACK_SELFTEST
static const uint8_t g_expectRxLen = MODBUS_TX_LEN;
#else
static const uint8_t g_expectRxLen = MODBUS_EXPECT_RESP_LEN;
#endif

static_assert(sizeof(MODBUS_TEST_FRAME_BYTES) == (size_t)MODBUS_TEST_FRAME_LEN,
              "MODBUS_TEST_FRAME_LEN 与 MODBUS_TEST_FRAME 字节数不一致");

static uint8_t  g_txFrame[MODBUS_TX_LEN];
static uint8_t  g_rxFrame[MODBUS_RX_BUF_MAX];
static uint8_t  g_rxLen         = 0;
static bool     g_rxOverflow    = false;  // true = 收到超过缓冲上限的字节（只标记，不写缓冲）
static uint32_t g_tRxStartMs    = 0;      // 三级超时的共同基准（已含 TX_GUARD 保护间隔）
static uint32_t g_tRxLastByteMs = 0;      // 最后一个字节到达时刻（字符间隔判据）
static bool     g_rxIncomplete  = false;  // true = 本次未收满预期帧长（超时 / 半帧）
static uint32_t g_statOk = 0, g_statFail = 0, g_statTimeout = 0;

// ---------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------
static IPAddress ipFromString(const char *s) {
  IPAddress ip;
  if (!ip.fromString(s)) {                 // 配置写错时给出显式提示，而不是静默 0.0.0.0
    Serial.printf("[CFG][ERR] IP 字符串无法解析: %s\n", s);
  }
  return ip;
}

// 生成时间戳：已同步 -> "YYYY-MM-DD HH:MM:SS"；未同步 -> "BOOT+<秒>.<毫秒>"
static bool buildTimestamp(char *out, size_t outSize) {
  struct tm now;
  // ⚠️ 只有本轮会话经 NTP 回包确认过（g_timeSynced）才认为时间可信：
  //    复位后 RTC 可能带上一轮的旧时间让 getLocalTime() 为真，但那 ≠ 本次已对时。
  //    否则按 BOOT+ 计数输出，并由调用方标记 UNSYNCED（与 ntpTick 的降级路径一致）。
  if (g_timeSynced && getLocalTime(&now, 0)) {     // 0 = 不阻塞等待
    strftime(out, outSize, "%Y-%m-%d %H:%M:%S", &now);
    return true;
  }
  uint32_t ms = millis();
  snprintf(out, outSize, "BOOT+%lu.%03lu",
           (unsigned long)(ms / 1000UL), (unsigned long)(ms % 1000UL));
  return false;
}

// 把字节数组格式化成 "01 03 00 00 00 02 C4 0B"（日志与差异定位用）
static void bytesToHex(const uint8_t *buf, uint8_t len, char *out, size_t outSize) {
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < len; i++) {
    if (pos + 4 >= outSize) break;                       // 预留 " XX" + '\0'
    int n = snprintf(out + pos, outSize - pos, (i == 0) ? "%02X" : " %02X", buf[i]);
    if (n <= 0) break;
    pos += (size_t)n;
  }
}

#if !RS485_MODE_LOOPBACK_SELFTEST
// ---------------------------------------------------------------
// CRC16/Modbus —— 初值 0xFFFF、多项式 0xA001、逐位计算
// 本函数与 06_QI协议说明.md 第 1 节 CRC 定义一致：
//   帧内 CRC 以【低字节在前】的字节序附加/校验（与 STB3-125 协议文档一致）。
// 已独立复算：01 03 00 00 00 02 -> 0x0BC4 -> 帧尾 C4 0B。
// ---------------------------------------------------------------
static uint16_t modbusCRC16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x0001U) crc = (uint16_t)((crc >> 1) ^ 0xA001U);
      else               crc = (uint16_t)(crc >> 1);
    }
  }
  return crc;
}
#endif  // !RS485_MODE_LOOPBACK_SELFTEST

// ---------------------------------------------------------------
// WiFi：非阻塞检查 + 节流重连（绝不用 delay()）
// ---------------------------------------------------------------
static void wifiTick() {
  if ((uint32_t)(millis() - g_lastWifiCheckMs) < WIFI_CHECK_INTERVAL_MS) return;
  g_lastWifiCheckMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    g_lastReconnectMs = 0;
    if (!g_linkUp) {
      g_linkUp        = true;
      g_everConnected = true;
      g_reconnectCount = 0;
      Serial.printf("[WIFI] 已连接 SSID=%s RSSI=%d dBm IP=%s GW=%s\n",
                    WiFi.SSID().c_str(), (int)WiFi.RSSI(),
                    WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
      g_ntpResyncPending = true;           // 首次连上 / 重连成功 -> 连上之后才触发 SNTP 对时
    }
    return;
  }

  if (g_linkUp) {
    g_linkUp = false;
    Serial.printf("[WIFI] 连接断开（status=%d），开始节流重连\n", (int)WiFi.status());
  }
  if ((uint32_t)(millis() - g_lastReconnectMs) < WIFI_RECONNECT_INTERVAL_MS) return;

  g_lastReconnectMs = millis();
  g_reconnectCount++;
  if (g_everConnected) {
    WiFi.reconnect();                      // 已连过：保留凭据做快速重连
    Serial.printf("[WIFI] 第 %lu 次重连（WiFi.reconnect）\n", (unsigned long)g_reconnectCount);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // 首次从未连上：重新发起连接
    Serial.printf("[WIFI] 第 %lu 次重连（WiFi.begin）\n", (unsigned long)g_reconnectCount);
  }
}

// ---------------------------------------------------------------
// NTP：源状态全量打印（本次改造的核心可观察项）
//
// 观察盲区：旧实现只打印配置名，无法判断 NTP 实际走内网还是落到公网；
//   且旧 logNtpEffectiveSource() 找到「第一个回包的 slot」就 return，一个都没命中
//   则只剩一句兜底 —— 时序稍不对（如判定过早）就完全没有信息量。
//   现改为：三个 slot 全量打印解析后地址 + RFC5905 reachability 移位寄存器，
//   无论时序如何，都能一眼看出「哪个源回了包、哪个没回」。
//
// 所用 API（已在本 core arduino-esp32 3.3.10 中核实存在，非凭记忆）：
//   - sntp_getserver(idx) / sntp_getreachability(idx) 声明于
//     .../esp32c3-libs/3.3.10-cn/include/lwip/lwip/src/include/lwip/apps/sntp.h，
//     符号在 liblwip.a 中有定义；sdkconfig：CONFIG_LWIP_SNTP_MAX_SERVERS=3、
//     SNTP_SERVER_DNS=1、SNTP_MONITOR_SERVER_REACHABILITY=1。
//   - slot 0/1/2 依次对应 NTP_SERVER_1/2/3（configTzTime 内部按序 sntp_setservername）；
//     reachability != 0 ⇒ 该源「本轮会话确实回过包」（RFC5905 移位寄存器，复位清零）。
//   - 备选 sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED：枚举名已核实
//     （esp_sntp.h），但它与 lwip 版 sntp_getserver 声明冲突、且 COMPLETED 读后即复位、
//     不是持久标志 ⇒ 改用 reachability + 自记「本轮已发起对时」标志（更稳）。
// 不含任何第三方库，全部为 ESP32 core 自带 lwIP API。
// ---------------------------------------------------------------
static const char *const kNtpCfg[3] = { NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3 };

// 「已同步」的唯一判据：本轮会话确已收到 NTP 回包（任一 slot reachability != 0）。
// ⚠️ 绝不能用 getLocalTime()：芯片不断电时 RTC 域保留上一轮的时间，软复位后
//    getLocalTime() 立即为真，却与「本次是否刚跟 NTP 对过时」无关 ⇒ 会提前宣告同步
//    （TZ 未生效 → %z 显示 +0000；SNTP 未回包 → 读不到实际生效源）。
static bool ntpHasReply() {
  for (uint8_t i = 0; i < 3; ++i) {
    if (sntp_getreachability(i) != 0) return true;
  }
  return false;
}

// 三个 slot 全量打印：slotI=<解析后IP 或 配置名> reach=0xNN
static void logNtpSlots() {
  char line[224];
  size_t pos = 0;
  for (uint8_t i = 0; i < 3; ++i) {                 // 0=主 / 1=备1 / 2=备2
    char slot[80];
    const ip_addr_t *addr = sntp_getserver(i);
    if (addr != NULL && !ip_addr_isany(addr)) {
      const char *ip = ipaddr_ntoa(addr);           // 静态缓冲，下面立即用掉
      if (strcmp(ip, kNtpCfg[i]) == 0) snprintf(slot, sizeof(slot), "%s", ip);
      else                             snprintf(slot, sizeof(slot), "%s->%s", kNtpCfg[i], ip);
    } else {
      snprintf(slot, sizeof(slot), "%s(未解析)", kNtpCfg[i]);
    }
    int n = snprintf(line + pos, sizeof(line) - pos, "%sslot%u=%s reach=0x%02X",
                     (i == 0) ? "" : " | ", (unsigned)i, slot,
                     (unsigned)sntp_getreachability(i));
    if (n <= 0 || (size_t)n >= (sizeof(line) - pos)) break;
    pos += (size_t)n;
  }
  Serial.printf("[NTP] 源状态：%s\n", line);
}

// 全量打印 + 指明实际生效源（在「判定为真正同步」之后调用一次）
static void logNtpEffectiveSource() {
  logNtpSlots();
  for (uint8_t i = 0; i < 3; ++i) {
    if (sntp_getreachability(i) == 0) continue;     // 该源本轮未回包
    const ip_addr_t *addr = sntp_getserver(i);
    if (addr == NULL || ip_addr_isany(addr)) continue;
    Serial.printf("[NTP] 实际生效源：%s（slot%u，配置 %s）\n",
                  ipaddr_ntoa(addr), (unsigned)i, kNtpCfg[i]);
    return;
  }
  // 兜底：理论上不会走到（只在同步确认后调用），保留旧措辞以便验收脚本识别
  Serial.println("[NTP] 实际生效源：暂未能从 SNTP 读取（本次会话尚未收到 NTP 回包）");
}

// ---------------------------------------------------------------
// NTP：同步状态检测 + 状态切换日志 + 重连后重新触发
// ---------------------------------------------------------------
// 同步确认后隔 N 秒补打一次源状态，便于人工确认实际生效源保持稳定（可选增强）
static const uint32_t NTP_STATUS_REPRINT_DELAY_MS = 30000;

static void ntpTick() {
  if (g_ntpResyncPending) {
    g_ntpResyncPending = false;
    // ⚠️ 必须在 WiFi 连上之后调用：网络未就绪时 SNTP 初始化会失败，表现为永不对时
    g_lastNtpTriggerMs        = millis();
    g_ntpTriggeredThisSession = true;
    configTzTime(NTP_TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    Serial.printf("[NTP] WiFi 已连上 -> 触发 SNTP 对时 TZ=%s 主=%s 备1=%s 备2=%s\n",
                  NTP_TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  }

  if ((uint32_t)(millis() - g_lastNtpCheckMs) < 200) return;   // 节流，避免每轮都取时钟
  g_lastNtpCheckMs = millis();

  // 「已同步」= 本轮已发起对时 且 确已收到 NTP 回包（reachability != 0）。
  // 不再用 getLocalTime()：复位后 RTC 旧时间会把它骗成 true（见上方根因注释）。
  bool ntpConfirmed = g_ntpTriggeredThisSession && ntpHasReply();

  // 未同步：按 NTP_RETRY_INTERVAL_MS 周期重新触发 SNTP（任务书 §6.1.2 要求 60s 重试）
  // 判据同为 NTP 回包 ⇒ 复位后即使 RTC 有旧时间，也会持续重试，而不是假装已同步。
  if (g_linkUp && !ntpConfirmed &&
      (uint32_t)(millis() - g_lastNtpTriggerMs) >= NTP_RETRY_INTERVAL_MS) {
    g_lastNtpTriggerMs = millis();
    configTzTime(NTP_TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    Serial.printf("[NTP] 仍未同步（本轮未收到 NTP 回包），重试 SNTP（周期 %lu ms）\n",
                  (unsigned long)NTP_RETRY_INTERVAL_MS);
  }

  // 同步后补打一次源状态（可选增强，便于人工复查实际生效源）
  if (g_ntpStatusReprintMs != 0 && (int32_t)(millis() - g_ntpStatusReprintMs) >= 0) {
    g_ntpStatusReprintMs = 0;
    Serial.println("[NTP] 源状态（同步后复查）");
    logNtpSlots();
  }

  if (ntpConfirmed == g_timeSynced) return;

  if (ntpConfirmed) {
    struct tm now;
    if (!getLocalTime(&now, 0)) return;   // 系统时钟尚未就绪：下轮再宣告，避免打印误报
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %z", &now);  // %z 自检：应为 +0800
    g_timeSynced = true;
    Serial.printf("[NTP] 时间已同步：本地 %s（TZ=%s）\n", ts, NTP_TZ_INFO);
    logNtpEffectiveSource();
    g_ntpStatusReprintMs = millis() + NTP_STATUS_REPRINT_DELAY_MS;
  } else {
    g_timeSynced         = false;
    g_ntpStatusReprintMs = 0;
    Serial.println("[NTP] 时间同步丢失：降级为 BOOT+ 计数（数据行标记 UNSYNCED）");
  }
}

// ---------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------
static void sendUdpPayload() {
  // WiFi 未连接：beginPacket 必然失败，若照发会每秒刷一条 [UDP][ERR] 掩盖真正的问题。
  // 此处直接跳过并做 5s 节流提示，保证日志只在"已联网但仍发不出去"时报错。
  if (!g_linkUp) {
    static uint32_t lastNoLinkLogMs = 0;
    if ((uint32_t)(millis() - lastNoLinkLogMs) >= 5000) {
      lastNoLinkLogMs = millis();
      Serial.println("[UDP] WiFi 未连接，本秒跳过上行（等待 wifiTick 重连）");
    }
    return;
  }

  char ts[32];
  bool synced = buildTimestamp(ts, sizeof(ts));
  int n = snprintf(g_payload, sizeof(g_payload), "%s | %s", ts, synced ? "HEARTBEAT" : "UNSYNCED");
  if (n < 0 || (size_t)n >= sizeof(g_payload)) {      // 缓冲不足会被静默截断，必须显式告警
    Serial.printf("[UDP][ERR] payload 缓冲不足：需要 %d 字节 / 缓冲 %u，本包已截断\n",
                  n, (unsigned)sizeof(g_payload));
  }

  bool udpOk = (g_udp.beginPacket(UDP_TARGET_IP, UDP_TARGET_PORT) == 1);   // 不判返回值会让日志说谎：发失败仍打印“已发”
  if (udpOk) {
    g_udp.write((const uint8_t *)g_payload, strlen(g_payload));
    udpOk = (g_udp.endPacket() == 1);
  }
  if (!udpOk) {
    Serial.printf("[UDP][ERR] 发送失败（beginPacket/endPacket 未返回 1），目标 %s:%u\n",
                  UDP_TARGET_IP, (unsigned)UDP_TARGET_PORT);
  }

  Serial.printf("[UDP -> %s:%u] %s\n", UDP_TARGET_IP, (unsigned)UDP_TARGET_PORT, g_payload);
}

#if !RS485_MODE_LOOPBACK_SELFTEST
// 把当前 RX 帧按十六进制打到下一行（异常帧定位用；最长打 24 字节，避免刷屏）
static void logRxHex() {
  uint8_t n = (g_rxLen > 24) ? 24 : g_rxLen;
  char buf[3 * 24 + 8];
  bytesToHex(g_rxFrame, n, buf, sizeof(buf));
  Serial.printf("      RX=%u | %s%s\n", (unsigned)g_rxLen, buf, (g_rxLen > n) ? " ..." : "");
}

// ---------------- M4：真实 485 总线 —— Modbus RTU 响应解析 ----------------
// 顺序：超时/半帧 -> 帧长 -> 地址 -> 功能码 -> 完整帧 CRC -> 数据域
// 注：M4 固定 9 字节（0x03 读 2 寄存器）；M5 起按 byte count 动态算帧长。
static void parseResponse() {
  // (1) 超时 / 半帧：三级超时或字符间隔先触发
  if (g_rxIncomplete) {
    g_statTimeout++;
    Serial.printf("[M4] 超时（首字节 %ums / 字符间隔 %ums / 兜底 %ums）RX=%u 期望 %u\n",
                  (unsigned)RS485_FIRST_BYTE_TIMEOUT_MS, (unsigned)RS485_CHAR_TIMEOUT_MS,
                  (unsigned)RS485_FRAME_MAX_MS, (unsigned)g_rxLen, (unsigned)g_expectRxLen);
    if (g_rxLen) logRxHex();
    return;
  }

  // (2) 从站异常响应（addr + func|0x80 + 异常码 + CRC2 = 5 字节）
  //     必须放在“帧长 == 期望帧长”判定之前：否则 5 字节异常帧会先被判成“帧长异常”（原实现还会先被判成“超时”）。
  //     M5 接入 QI 真机时，“真机拒绝 00 27”的表现正是 01 84 xx yy zz，此分支是主要诊断依据（E-02）。
  if (g_rxLen >= 5 && (g_rxFrame[1] & 0x80)) {
    const uint16_t crcR = (uint16_t)g_rxFrame[g_rxLen - 2] |
                          ((uint16_t)g_rxFrame[g_rxLen - 1] << 8);
    const uint16_t crcC = modbusCRC16(g_rxFrame, (size_t)(g_rxLen - 2));
    if (crcC != crcR) {
      g_statFail++;
      Serial.printf("[M4] 异常响应 CRC 失败 期望 %04X 计算 %04X\n",
                    (unsigned)crcR, (unsigned)crcC);
      logRxHex();
      return;
    }
    if (g_rxFrame[0] != RS485_SLAVE_ADDR) {
      g_statFail++;
      Serial.printf("[M4] 异常响应地址不匹配 期望 %u 收到 %u\n",
                    (unsigned)RS485_SLAVE_ADDR, (unsigned)g_rxFrame[0]);
      logRxHex();
      return;
    }
    g_statFail++;   // 异常响应属于合法帧但代表从站报错，计入失败便于统计
    Serial.printf("[M4] 从站异常响应 FUNC=0x%02X 异常码=0x%02X（参见 06_QI协议说明.md §7 E-02）\n",
                  (unsigned)g_rxFrame[1], (unsigned)g_rxFrame[2]);
    logRxHex();
    return;
  }

  // (3) 帧长异常：M4 期望固定 9 字节
  if (g_rxLen != g_expectRxLen) {
    g_statFail++;
    Serial.printf("[M4] 帧长异常 期望 %u 收到 %u\n",
                  (unsigned)g_expectRxLen, (unsigned)g_rxLen);
    logRxHex();
    // 常见误因：MAX13487 的 RE#/EN 被常开使能 -> 自己的请求被回读（RX 帧 == TX 帧，8 字节）
    if (g_rxLen == MODBUS_TX_LEN && memcmp(g_rxFrame, g_txFrame, MODBUS_TX_LEN) == 0) {
      Serial.println("      [诊断] RX 帧 == TX 帧：疑似自发自收回显（若为 MAX485/MAX3485 说明 EN 被固定接高 -> 应改接 GPIO 由固件切换；若为 MAX13487 则按 00_context.md 三.5 核对）");
    }
    return;
  }

  const uint8_t addr = g_rxFrame[0];
  const uint8_t func = g_rxFrame[1];
  // 帧尾 2 字节为 CRC，低字节在前：recv = rx[n-2] | (rx[n-1] << 8)
  const uint16_t crcRecv = (uint16_t)g_rxFrame[g_rxLen - 2] |
                           ((uint16_t)g_rxFrame[g_rxLen - 1] << 8);
  const uint16_t crcCalc = modbusCRC16(g_rxFrame, (size_t)(g_rxLen - 2));

  // (3) 地址匹配（收到 >= 2 字节即可判）
  if (addr != RS485_SLAVE_ADDR) {
    g_statFail++;
    Serial.printf("[M4] 地址不匹配 期望 %u 收到 %u\n",
                  (unsigned)RS485_SLAVE_ADDR, (unsigned)addr);
    logRxHex();
    return;
  }

  // (4) 功能码匹配：异常响应（最高位置 1）已在 (2) 处提前处理，此处只判正常响应
  if (func != MODBUS_FUNC_READ_HOLDING && func != MODBUS_FUNC_READ_INPUT) {
    g_statFail++;
    Serial.printf("[M4] 功能码不匹配 期望 0x%02X/0x%02X 收到 0x%02X\n",
                  (unsigned)MODBUS_FUNC_READ_HOLDING, (unsigned)MODBUS_FUNC_READ_INPUT,
                  (unsigned)func);
    logRxHex();
    return;
  }

  // (5) 完整帧 CRC16/Modbus 校验
  if (crcCalc != crcRecv) {
    g_statFail++;
    Serial.printf("[M4] CRC 失败 期望 %04X 计算 %04X\n",
                  (unsigned)crcRecv, (unsigned)crcCalc);
    logRxHex();
    return;
  }

  // (6) 数据域：byte count 必须与帧长自洽（1 + 1 + 1 + N + 2）
  const uint8_t byteCount = g_rxFrame[2];
  if ((uint16_t)(byteCount + 5) != (uint16_t)g_rxLen) {
    g_statFail++;
    Serial.printf("[M4] 帧长异常 期望 %u 收到 %u（byte count=%u）\n",
                  (unsigned)(byteCount + 5), (unsigned)g_rxLen, (unsigned)byteCount);
    logRxHex();
    return;
  }

  g_statOk++;
  char dataHex[3 * MODBUS_RX_BUF_MAX + 1];
  bytesToHex(g_rxFrame + 3, byteCount, dataHex, sizeof(dataHex));
  Serial.printf("[M4] 解析 OK ADDR=%u FUNC=0x%02X DATA=%s\n",
                (unsigned)addr, (unsigned)func, dataHex);
  if ((byteCount % 2) == 0 && byteCount > 0) {           // 顺带打印 16 位寄存器值，便于与从站设置对照
    Serial.print("      REG:");
    for (uint8_t i = 0; i + 1 < byteCount; i += 2) {
      uint16_t v = ((uint16_t)g_rxFrame[3 + i] << 8) | (uint16_t)g_rxFrame[4 + i];
      Serial.printf(" [%u]=%u", (unsigned)(i / 2), (unsigned)v);
    }
    Serial.printf("  CRC=%04X\n", (unsigned)crcRecv);
  }
}
#else
// ---------------- M3 式物理回环自测（RS485_MODE_LOOPBACK_SELFTEST = 1） ----------------
// 发什么就应该一模一样收回来（GPIO21 短接 GPIO20，且不接 MAX13487）
static void parseResponse() {
  char txHex[3 * MODBUS_TX_LEN + 1];
  bytesToHex(g_txFrame, MODBUS_TX_LEN, txHex, sizeof(txHex));

  if (g_rxIncomplete) {
    g_statTimeout++;
    Serial.printf("[485] 回环 TIMEOUT（首字节 %ums / 字符间隔 %ums / 兜底 %ums）RX=%u\n",
                  (unsigned)RS485_FIRST_BYTE_TIMEOUT_MS, (unsigned)RS485_CHAR_TIMEOUT_MS,
                  (unsigned)RS485_FRAME_MAX_MS, (unsigned)g_rxLen);
  } else if (g_rxLen == MODBUS_TX_LEN && memcmp(g_txFrame, g_rxFrame, MODBUS_TX_LEN) == 0) {
    g_statOk++;
    Serial.printf("[485] 回环 OK TX=%u RX=%u | %s\n",
                  (unsigned)MODBUS_TX_LEN, (unsigned)g_rxLen, txHex);
  } else {
    char rxHex[3 * MODBUS_RX_BUF_MAX + 1];
    bytesToHex(g_rxFrame, g_rxLen, rxHex, sizeof(rxHex));
    g_statFail++;
    Serial.printf("[485] 回环 FAIL TX=%u RX=%u | 期望 %s 收到 %s\n",
                  (unsigned)MODBUS_TX_LEN, (unsigned)g_rxLen, txHex, rxHex);
    for (uint8_t i = 0; i < g_rxLen && i < MODBUS_TX_LEN; i++) {   // 差异字节逐一定位
      if (g_rxFrame[i] != g_txFrame[i]) {
        Serial.printf("      [差异] byte[%u] 期望 %02X 收到 %02X\n",
                      (unsigned)i, (unsigned)g_txFrame[i], (unsigned)g_rxFrame[i]);
      }
    }
  }
}
#endif

static void stateTick() {
  switch (g_state) {
    case ST_IDLE:
      if ((int32_t)(millis() - g_nextPollMs) >= 0) g_state = ST_TX;
      break;

    case ST_TX: {
      // ⚠️ 清 RX 缓冲必须在 write() 【之前】：
      // 物理回环下"发"与"收"是同时进行的，字节在 flush 返回前就已进 RX 缓冲；
      // 若照搬到 flush() 之后再清，会把刚收回来的字节一并丢掉 → 必然误报 TIMEOUT。
      // 真实总线上清的是上一周期残留（半帧 / 噪声 / 上一次的迟到响应）。
      while (Serial1.available()) (void)Serial1.read();

      memcpy(g_txFrame, MODBUS_TEST_FRAME_BYTES, MODBUS_TX_LEN);
#if RS485_DE_PIN >= 0
      // MAX485 / MAX3485 半双工：先把方向切到发送，否则帧根本上不了总线
      digitalWrite(RS485_DE_PIN, RS485_DE_ACTIVE_HIGH ? HIGH : LOW);
#endif
      Serial1.write(g_txFrame, MODBUS_TX_LEN);
      Serial1.flush();                                     // 等 8 字节全部移出（9600 下约 8.3ms，有界阻塞）
#if RS485_DE_PIN >= 0
      // 说明：本 core（3.3.10）的 flush() = uartFlushTxOnly(uart, true)，会一直等到 TX 完全空闲
      //   （txfifo_cnt==0 且 TX 状态机回到 IDLE），即移位寄存器已把最后一位送完，
      //   所以下面的等待不是为了补 flush 的不足，而是给 DE/AutoDirection 换向留防御性余量
      //   （MAX13487 手册 tDDD + tRED ≤ 2.6µs；9600 下一个字符 ≈ 1.04ms）。
      //    故先有界等待 >= RS485_TX_GUARD_MS（默认 2ms），再切回接收。
      for (uint32_t t0 = millis(); (uint32_t)(millis() - t0) < RS485_TX_GUARD_MS; ) { }
      digitalWrite(RS485_DE_PIN, RS485_DE_ACTIVE_HIGH ? LOW : HIGH);
#endif

      g_rxLen         = 0;
      g_rxOverflow    = false;
      g_rxIncomplete  = false;
      g_tRxStartMs    = millis() + RS485_TX_GUARD_MS;      // 保护间隔：等收发器方向切回接收后再收
      g_tRxLastByteMs = g_tRxStartMs;
      g_state         = ST_WAIT_RX;
      break;
    }

    case ST_WAIT_RX: {
      uint32_t now = millis();
      if ((int32_t)(now - g_tRxStartMs) < 0) break;        // 仍在 TX_GUARD 保护间隔内

      while (Serial1.available()) {                        // 收字节（read() 已设 timeout=0，不阻塞）
        uint8_t b = (uint8_t)Serial1.read();
        if (g_rxLen < MODBUS_RX_BUF_MAX) g_rxFrame[g_rxLen++] = b;
        else                             g_rxOverflow = true;   // 超长帧：只标记，不再写缓冲
        g_tRxLastByteMs = millis();
      }

      // 帧长驱动：按已收到的帧头动态推算本帧应到长度，收满即止
      //   - 异常响应：addr + (func|0x80) + 异常码 + CRC2 = 固定 5 字节
      //   - 正常响应：addr + func + byteCount + N + CRC2 = byteCount + 5
      //   - 帧头不足 2/3 字节时回退到常量 g_expectRxLen
      //   - ⚠️ 回环自测模式（RS485_MODE_LOOPBACK_SELFTEST=1）【不可】用此推算：
      //     该模式下"响应"就是请求帧本身，byte[2] 是寄存器地址（0x00）而不是字节数，
      //     推算会把 8 字节的期望帧误缩成 5 字节 -> 每次必报 FAIL。该模式固定用 g_expectRxLen。
#if RS485_MODE_LOOPBACK_SELFTEST
      if (g_rxLen >= g_expectRxLen) { g_state = ST_PARSE; break; }
#else
      {
        uint8_t need = g_expectRxLen;
        if (g_rxLen >= 2 && (g_rxFrame[1] & 0x80)) {
          need = 5;                                        // 从站异常响应：固定 5 字节
        } else if (g_rxLen >= 3 &&
                   (g_rxFrame[1] == MODBUS_FUNC_READ_HOLDING ||
                    g_rxFrame[1] == MODBUS_FUNC_READ_INPUT)) {
          uint16_t bc = g_rxFrame[2];                      // 仅当功能码确认为读寄存器时才把 byte[2] 当字节数
          if ((uint32_t)bc + 5U <= (uint32_t)MODBUS_RX_BUF_MAX) need = (uint8_t)(bc + 5U);
        }
        if (g_rxLen >= need) { g_state = ST_PARSE; break; }
      }
#endif
      if (g_rxOverflow) {                                  // 已超过缓冲上限，不必再等
        g_state = ST_PARSE;
        break;
      }

      now = millis();
      uint32_t frameEl = now - g_tRxStartMs;

      if (frameEl >= RS485_FRAME_MAX_MS) {                 // 三级：整帧兜底
        g_rxIncomplete = true; g_state = ST_PARSE; break;
      }
      if (g_rxLen == 0) {                                  // 一级：首字节超时
        if (frameEl >= RS485_FIRST_BYTE_TIMEOUT_MS) {
          g_rxIncomplete = true; g_state = ST_PARSE; break;
        }
      } else {                                             // 二级：字节间静默 → 判本帧结束（半帧）
        if ((now - g_tRxLastByteMs) >= RS485_CHAR_TIMEOUT_MS) {
          g_rxIncomplete = true; g_state = ST_PARSE; break;
        }
      }
      break;
    }

    case ST_PARSE:
      parseResponse();                                     // 回环模式=逐字节比对；总线模式=Modbus RTU 解析
      {
        uint32_t total = g_statOk + g_statFail + g_statTimeout;
        if (total > 0 && (total % RS485_STATS_EVERY_N) == 0) {   // 每 100 次打印一次累计
          Serial.printf("[485] 统计：成功 %lu / 失败 %lu / 超时 %lu\n",
                        (unsigned long)g_statOk, (unsigned long)g_statFail,
                        (unsigned long)g_statTimeout);
        }
      }
      g_state = ST_SEND;
      break;

    case ST_SEND:
      sendUdpPayload();
      g_nextPollMs += POLL_INTERVAL_MS;
      // 已落后一个周期以上（例如刚重连）就重新对齐，避免补发风暴
      if ((int32_t)(millis() - g_nextPollMs) >= 0) g_nextPollMs = millis() + POLL_INTERVAL_MS;
      g_state = ST_IDLE;
      break;
  }
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);                    // USB CDC（Tools -> USB CDC On Boot = Enabled）
  // USB CDC 需要主机枚举完成后才有输出；此前打的日志会丢。
  // 仅在 setup 内有界等待（最多 2s），不影响 loop() 的非阻塞约束。
  for (uint32_t t0 = millis(); !Serial && (uint32_t)(millis() - t0) < 2000; ) delay(10);

  // ---- WiFi：静态 IP + 最小自恢复 ----
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);                   // 凭据持久化到 NVS，掉电后可直接 reconnect
  WiFi.setAutoReconnect(true);             // 协议栈自带重连作为第一道防线
  WiFi.setSleep(false);                    // 关闭 modem sleep，避免延迟抖动放大（任务书 §9.1 M6 前置）

  WiFi.config(ipFromString(LOCAL_IP), ipFromString(GATEWAY),
              ipFromString(SUBNET),   ipFromString(DNS1));
  Serial.printf("[WIFI] 静态配置 IP=%s GW=%s MASK=%s DNS=%s\n",
                LOCAL_IP, GATEWAY, SUBNET, DNS1);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);    // 非阻塞：连接进度由 wifiTick() 观察
  Serial.println("[WIFI] 已发起连接（非阻塞），等待 wifiTick() 报告结果");

  // ---- NTP：此处【不】调用 configTzTime ----
  // 根因修复：SNTP 依赖已就绪的网络栈。若在 WiFi 连上之前初始化，SNTP 拿不到网络；
  // 现改为由 wifiTick() 在"首次连上 / 重连成功"时置位 g_ntpResyncPending，
  // 再由 ntpTick() 调用 configTzTime（三源：主 校内 chrony -> 备1 公网域名 -> 备2 公网 IP 直连）。
  Serial.printf("[NTP] 三源配置 TZ=%s 主=%s 备1=%s 备2=%s（等 WiFi 连上后由 ntpTick() 触发，setup 不调用 configTzTime）\n",
                NTP_TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  // ---- UDP：本地端口打开，目标由 config.h 指定 ----
  g_udp.begin(UDP_LOCAL_PORT);
  Serial.printf("[UDP] 本地端口 %u，目标 %s:%u\n",
                (unsigned)UDP_LOCAL_PORT, UDP_TARGET_IP, (unsigned)UDP_TARGET_PORT);

  // ---- UART1（RS-485 侧）：9600 8N1，begin 形参顺序为 (baud, config, RX, TX) ----
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial1.setTimeout(0);                   // read() 绝不阻塞；超时判定全部交给 millis()
#if RS485_DE_PIN >= 0
  // 方向控制引脚：上电先置于接收态，避免刚上电就占住总线
  pinMode(RS485_DE_PIN, OUTPUT);
  digitalWrite(RS485_DE_PIN, RS485_DE_ACTIVE_HIGH ? LOW : HIGH);
  Serial.printf("[485] 方向控制已启用 EN=GPIO%d（%s 为发送态）\n",
                (int)RS485_DE_PIN, RS485_DE_ACTIVE_HIGH ? "高电平" : "低电平");
#endif
  Serial.printf("[485] UART1 就绪 baud=%lu 8N1 RX=GPIO%d TX=GPIO%d\n",
                (unsigned long)RS485_BAUD, (unsigned)RS485_RX_PIN, (unsigned)RS485_TX_PIN);

#if RS485_MODE_LOOPBACK_SELFTEST
  Serial.println("[485] 模式=M3 物理回环自测：请短接 GPIO21 <-> GPIO20，且不接 MAX13487");
#else
  Serial.println("[485] 模式=真实 485 总线：ESP32(TX) -> MAX13487 -> 电脑 USB 转 485（Modbus 从站）");
  Serial.println("[485] ⚠️ RXO 先万用表实测空闲电平：5V 必须 1k/2k 分压后接 GPIO20，3.3V 则直连不分压（未实测前按 5V 处理）；EN 接法随芯片型号（先读丝印，见 00_context.md 三.5）；三方必须共地");
  Serial.printf("[485] 方向控制：%s\n", (RS485_DE_PIN >= 0)
                ? "EN 由 GPIO 动态切换（MAX485 / MAX3485 接法，RS485_DE_PIN 已配置）"
                : "无（EN 固定电平，MAX13487 / MAX13488 接法；RS485_DE_PIN = -1）");
#endif
  {
    char txHex[3 * MODBUS_TX_LEN + 1];
    bytesToHex(MODBUS_TEST_FRAME_BYTES, MODBUS_TX_LEN, txHex, sizeof(txHex));
    Serial.printf("[485] 请求 %u 字节 | %s  ->  期望响应 %u 字节（从站地址 %u，功能码 0x%02X 读 2 个保持寄存器）\n",
                  (unsigned)MODBUS_TX_LEN, txHex, (unsigned)g_expectRxLen,
                  (unsigned)RS485_SLAVE_ADDR, (unsigned)MODBUS_FUNC_READ_HOLDING);
  }

  g_nextPollMs = millis() + POLL_INTERVAL_MS;
  Serial.println("[BOOT] M4 就绪：WiFi + NTP + UDP 心跳 + 485 总线收发 + Modbus RTU 解析");
}

void loop() {
  wifiTick();     // WiFi 状态检查与节流重连
  ntpTick();      // 对时状态检测 / 重连后重新对时
  stateTick();    // IDLE -> TX -> WAIT_RX -> PARSE -> SEND
}
