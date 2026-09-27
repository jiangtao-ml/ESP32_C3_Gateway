/*
 * esp32c3_gateway.ino
 * ESP32-C3 RS-485 采集网关（替换树莓派网关项目）
 *
 * 当前里程碑：M2 —— WiFi + NTP + UDP + 最小非阻塞状态机骨架
 *
 * 已实现：WiFi 连接与最小自恢复、SNTP 对时（含 UNSYNCED 降级）、每秒 1 包 UDP 心跳上行
 * 未实现：RS-485 收发、Modbus 组帧/解析（M3 起；本文件不含任何 485 / Modbus 代码）
 *
 * 硬约束：
 *   - 全程 millis() 非阻塞调度，禁止 delay()
 *   - WiFi 凭据只来自 secrets.h（不入库），本文件不出现任何凭据明文
 *   - 不引入第三方库（仅用 ESP32 core 自带的 WiFi / WiFiUdp / time）
 *
 * 事实来源：04_ai_prompts/00_context.md
 * 编译设置：Arduino IDE -> Tools -> USB CDC On Boot = Enabled
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>

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
//   M2：TX / WAIT_RX / PARSE 为直通占位，SEND 发心跳；M3 起填充 485 收发与解析
// ---------------------------------------------------------------
enum GatewayState : uint8_t {
  ST_IDLE = 0,   // 等待 1s 定时到点
  ST_TX,         // 发送查询帧（M3：Serial1）
  ST_WAIT_RX,    // 等设备回复（M3：按帧长收帧 + 300ms 兜底）
  ST_PARSE,      // 解析回复（M3：Modbus RTU）
  ST_SEND        // UDP 上行（M2：心跳包）
};

static GatewayState g_state            = ST_IDLE;
static uint32_t     g_nextPollMs       = 0;   // 下一次 1s 到点的绝对时刻
static uint32_t     g_lastWifiCheckMs  = 0;
static uint32_t     g_lastReconnectMs  = 0;
static uint32_t     g_reconnectCount   = 0;
static uint32_t     g_lastNtpCheckMs   = 0;
static bool         g_linkUp           = false;  // 当前 WiFi 是否已连上
static bool         g_everConnected    = false;  // 是否曾经连上过（决定首次 begin 失败后的重试方式）
static bool         g_timeSynced       = false;
static bool         g_ntpResyncPending = false;  // 重连成功后置位，下一轮重新触发 SNTP

static WiFiUDP g_udp;
static char    g_payload[160];

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
  if (getLocalTime(&now, 0)) {             // 0 = 不阻塞等待
    strftime(out, outSize, "%Y-%m-%d %H:%M:%S", &now);
    return true;
  }
  uint32_t ms = millis();
  snprintf(out, outSize, "BOOT+%lu.%03lu",
           (unsigned long)(ms / 1000UL), (unsigned long)(ms % 1000UL));
  return false;
}

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
      g_ntpResyncPending = true;           // 重连后立即重新触发 SNTP（时间可能已漂移）
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
// NTP：同步状态检测 + 状态切换日志 + 重连后重新触发
// ---------------------------------------------------------------
static void ntpTick() {
  if (g_ntpResyncPending) {
    g_ntpResyncPending = false;
    configTzTime(NTP_TZ_INFO, NTP_SERVER, NTP_SERVER_IP);
    Serial.println("[NTP] 重新触发 SNTP 对时");
  }

  if ((uint32_t)(millis() - g_lastNtpCheckMs) < 200) return;   // 节流，避免每轮都取时钟
  g_lastNtpCheckMs = millis();

  struct tm now;
  bool synced = getLocalTime(&now, 0);
  if (synced == g_timeSynced) return;

  g_timeSynced = synced;
  if (synced) {
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &now);
    Serial.printf("[NTP] 时间已同步：本地 %s（TZ=%s，源 %s / 备选 %s）\n",
                  ts, NTP_TZ_INFO, NTP_SERVER, NTP_SERVER_IP);
  } else {
    Serial.println("[NTP] 时间同步丢失：降级为 BOOT+ 计数（数据行标记 UNSYNCED）");
  }
}

// ---------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------
static void sendUdpPayload() {
  char ts[32];
  bool synced = buildTimestamp(ts, sizeof(ts));
  snprintf(g_payload, sizeof(g_payload), "%s | %s", ts, synced ? "M2_HEARTBEAT" : "UNSYNCED");

  g_udp.beginPacket(UDP_TARGET_IP, UDP_TARGET_PORT);
  g_udp.write((const uint8_t *)g_payload, strlen(g_payload));
  g_udp.endPacket();

  Serial.printf("[UDP -> %s:%u] %s\n", UDP_TARGET_IP, (unsigned)UDP_TARGET_PORT, g_payload);
}

static void stateTick() {
  switch (g_state) {
    case ST_IDLE:
      if ((int32_t)(millis() - g_nextPollMs) >= 0) g_state = ST_TX;
      break;

    case ST_TX:
      // M3：在此向 485 总线发送 Modbus 查询帧（Serial1.write + flush）
      g_state = ST_WAIT_RX;
      break;

    case ST_WAIT_RX:
      // M3：在此收帧（首字节后按帧长动态判超时，300ms 兜底）
      g_state = ST_PARSE;
      break;

    case ST_PARSE:
      // M3：在此做 Modbus RTU 解析 + CRC 校验
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

  // ---- NTP：主域名 + 备选 IP 直连 ----
  configTzTime(NTP_TZ_INFO, NTP_SERVER, NTP_SERVER_IP);
  Serial.printf("[NTP] SNTP 已启动 TZ=%s 主=%s 备=%s\n", NTP_TZ_INFO, NTP_SERVER, NTP_SERVER_IP);

  // ---- UDP：本地端口打开，目标由 config.h 指定 ----
  g_udp.begin(UDP_LOCAL_PORT);
  Serial.printf("[UDP] 本地端口 %u，目标 %s:%u\n",
                (unsigned)UDP_LOCAL_PORT, UDP_TARGET_IP, (unsigned)UDP_TARGET_PORT);

  g_nextPollMs = millis() + POLL_INTERVAL_MS;
  Serial.println("[BOOT] M2 骨架就绪：WiFi + NTP + UDP + 非阻塞状态机");
}

void loop() {
  wifiTick();     // WiFi 状态检查与节流重连
  ntpTick();      // 对时状态检测 / 重连后重新对时
  stateTick();    // IDLE -> TX -> WAIT_RX -> PARSE -> SEND
}
