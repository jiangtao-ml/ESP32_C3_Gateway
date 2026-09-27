/*
 * esp32c3_gateway.ino
 * ESP32-C3 RS-485 采集网关（替换树莓派网关项目）
 *
 * 当前里程碑：M3 —— UART1 物理回环自测（GPIO21 短接 GPIO20）
 *
 * 已实现：WiFi 连接与最小自恢复、SNTP 对时（含 UNSYNCED 降级）、每秒 1 包 UDP 心跳上行、
 *        UART1 9600 8N1 每秒一次物理回环自测（发 8 字节测试帧并逐字节比对）
 * 未实现：Modbus RTU 组帧 / 解析 / CRC（M4 起；本文件不含任何 Modbus 协议代码）
 *
 * 硬约束：
 *   - 全程 millis() 非阻塞调度，禁止 delay()
 *   - WiFi 凭据只来自 secrets.h（不入库），本文件不出现任何凭据明文
 *   - 不引入第三方库（仅用 ESP32 core 自带的 WiFi / WiFiUdp / time / HardwareSerial）
 *   - ⚠️ USB CDC On Boot 必须 = Enabled：否则 Serial 会落到 UART0（GPIO20/21），
 *     与 UART1 回环测试争用同一对引脚

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
//   M3：TX / WAIT_RX / PARSE 已填充 UART1 物理回环自测；M4 把 PARSE 换成 Modbus RTU 解析
// ---------------------------------------------------------------
enum GatewayState : uint8_t {
  ST_IDLE = 0,   // 等待 1s 定时到点
  ST_TX,         // 向 UART1 发送 8 字节测试帧
  ST_WAIT_RX,    // 收回环字节：首字节 80ms / 字符间隔 4ms / 整帧 300ms 三级判超时
  ST_PARSE,      // 比对 TX/RX 并打印结果（M4 改为 Modbus RTU 解析 + CRC）
  ST_SEND        // UDP 上行（M2 心跳逻辑保留）
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
// UART1 物理回环自测（M3）：发什么就应该一模一样收回来
// ---------------------------------------------------------------
static const uint8_t RS485_TEST_FRAME[]   = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
static const uint8_t RS485_TEST_FRAME_LEN = sizeof(RS485_TEST_FRAME);   // = 8

static uint8_t  g_txFrame[RS485_TEST_FRAME_LEN];
static uint8_t  g_rxFrame[RS485_TEST_FRAME_LEN];
static uint8_t  g_rxLen         = 0;
static uint32_t g_tRxStartMs    = 0;      // 三级超时的共同基准（已含 TX_GUARD 保护间隔）
static uint32_t g_tRxLastByteMs = 0;      // 最后一个字节到达时刻（字符间隔判据）
static bool     g_rxIncomplete  = false;  // true = 本次未收满帧（超时）
static uint32_t g_loopOk = 0, g_loopFail = 0, g_loopTimeout = 0;

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

// 把字节数组格式化成 "01 03 00 00 00 02 C4 0B"（回环日志与差异定位用）
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
// NTP：同步状态检测 + 状态切换日志 + 重连后重新触发
// ---------------------------------------------------------------
static void ntpTick() {
  if (g_ntpResyncPending) {
    g_ntpResyncPending = false;
    // ⚠️ 必须在 WiFi 连上之后调用：网络未就绪时 SNTP 初始化会失败，表现为永不对时
    configTzTime(NTP_TZ_INFO, NTP_SERVER, NTP_SERVER_IP);
    Serial.printf("[NTP] WiFi 已连上 -> 触发 SNTP 对时 TZ=%s 主=%s 备=%s\n",
                  NTP_TZ_INFO, NTP_SERVER, NTP_SERVER_IP);
  }

  if ((uint32_t)(millis() - g_lastNtpCheckMs) < 200) return;   // 节流，避免每轮都取时钟
  g_lastNtpCheckMs = millis();

  struct tm now;
  bool synced = getLocalTime(&now, 0);
  if (synced == g_timeSynced) return;

  g_timeSynced = synced;
  if (synced) {
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %z", &now);  // %z 用于自检：应为 +0800，若为 +0000 即 TZ 未生效
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

    case ST_TX: {
      // ⚠️ 清 RX 缓冲必须在 write() 【之前】：
      // 物理回环下"发"与"收"是同时进行的，字节在 flush 返回前就已进 RX 缓冲；
      // 若照搬到 flush() 之后再清，会把刚收回来的回环字节一并丢掉 → 必然误报 TIMEOUT。
      // 这里清的是上一周期残留（半帧 / 噪声）。
      while (Serial1.available()) (void)Serial1.read();

      memcpy(g_txFrame, RS485_TEST_FRAME, RS485_TEST_FRAME_LEN);
      Serial1.write(g_txFrame, RS485_TEST_FRAME_LEN);
      Serial1.flush();                                     // 等 8 字节全部移出（9600 下约 8.3ms，有界阻塞）

      g_rxLen         = 0;
      g_rxIncomplete  = false;
      g_tRxStartMs    = millis() + RS485_TX_GUARD_MS;      // 保护间隔：到点前不收
      g_tRxLastByteMs = g_tRxStartMs;
      g_state         = ST_WAIT_RX;
      break;
    }

    case ST_WAIT_RX: {
      uint32_t now = millis();
      if ((int32_t)(now - g_tRxStartMs) < 0) break;        // 仍在 TX_GUARD 保护间隔内

      while (Serial1.available()) {                        // 收字节（read() 已设 timeout=0，不阻塞）
        uint8_t b = (uint8_t)Serial1.read();
        if (g_rxLen < RS485_TEST_FRAME_LEN) g_rxFrame[g_rxLen++] = b;
        // 超出预期长度的字节直接丢弃，避免缓冲把状态机卡在 WAIT_RX
        g_tRxLastByteMs = millis();
      }

      if (g_rxLen >= RS485_TEST_FRAME_LEN) {               // 收满 8 字节，提前结束等待
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
      } else {                                             // 二级：字节间静默 → 判本帧已结束（半帧）
        if ((now - g_tRxLastByteMs) >= RS485_CHAR_TIMEOUT_MS) {
          g_rxIncomplete = true; g_state = ST_PARSE; break;
        }
      }
      break;
    }

    case ST_PARSE: {
      char txHex[3 * RS485_TEST_FRAME_LEN + 1];
      bytesToHex(g_txFrame, RS485_TEST_FRAME_LEN, txHex, sizeof(txHex));

      if (g_rxIncomplete) {
        g_loopTimeout++;
        Serial.printf("[485] 回环 TIMEOUT（首字节 %ums / 字符间隔 %ums / 兜底 %ums）RX=%u\n",
                      (unsigned)RS485_FIRST_BYTE_TIMEOUT_MS, (unsigned)RS485_CHAR_TIMEOUT_MS,
                      (unsigned)RS485_FRAME_MAX_MS, (unsigned)g_rxLen);
      } else if (memcmp(g_txFrame, g_rxFrame, RS485_TEST_FRAME_LEN) == 0) {
        g_loopOk++;
        Serial.printf("[485] 回环 OK TX=%u RX=%u | %s\n",
                      (unsigned)RS485_TEST_FRAME_LEN, (unsigned)g_rxLen, txHex);
      } else {
        char rxHex[3 * RS485_TEST_FRAME_LEN + 1];
        bytesToHex(g_rxFrame, g_rxLen, rxHex, sizeof(rxHex));
        g_loopFail++;
        Serial.printf("[485] 回环 FAIL TX=%u RX=%u | 期望 %s 收到 %s\n",
                      (unsigned)RS485_TEST_FRAME_LEN, (unsigned)g_rxLen, txHex, rxHex);
        for (uint8_t i = 0; i < g_rxLen; i++) {            // 差异字节逐一定位
          if (i >= RS485_TEST_FRAME_LEN || g_rxFrame[i] != g_txFrame[i]) {
            Serial.printf("      [差异] byte[%u] 期望 %02X 收到 %02X\n",
                          (unsigned)i, (unsigned)g_txFrame[i], (unsigned)g_rxFrame[i]);
          }
        }
      }

      uint32_t total = g_loopOk + g_loopFail + g_loopTimeout;
      if (total > 0 && (total % RS485_STATS_EVERY_N) == 0) {   // 每 100 次打印一次累计
        Serial.printf("[485] 统计：成功 %lu / 失败 %lu / 超时 %lu\n",
                      (unsigned long)g_loopOk, (unsigned long)g_loopFail,
                      (unsigned long)g_loopTimeout);
      }
      g_state = ST_SEND;
      break;
    }

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

  // ---- NTP：此处【不】调用 configTzTime ----
  // 根因修复：SNTP 依赖已就绪的网络栈。若在 WiFi 连上之前初始化，SNTP 拿不到网络；
  // 现改为由 wifiTick() 在"首次连上 / 重连成功"时置位 g_ntpResyncPending，
  // 再由 ntpTick() 调用 configTzTime（主域名 + 备选 IP 直连）。

  // ---- UDP：本地端口打开，目标由 config.h 指定 ----
  g_udp.begin(UDP_LOCAL_PORT);
  Serial.printf("[UDP] 本地端口 %u，目标 %s:%u\n",
                (unsigned)UDP_LOCAL_PORT, UDP_TARGET_IP, (unsigned)UDP_TARGET_PORT);

  // ---- UART1（RS-485 侧）：9600 8N1，begin 形参顺序为 (baud, config, RX, TX) ----
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial1.setTimeout(0);                   // read() 绝不阻塞；超时判定全部交给 millis()
  Serial.printf("[485] UART1 就绪 baud=%lu 8N1 RX=GPIO%d TX=GPIO%d\n",
                (unsigned long)RS485_BAUD, (unsigned)RS485_RX_PIN, (unsigned)RS485_TX_PIN);
  Serial.println("[485] 物理回环自测：请用杜邦线短接 GPIO21 <-> GPIO20（M3 不接 MAX13487）");

  g_nextPollMs = millis() + POLL_INTERVAL_MS;
  Serial.println("[BOOT] M3 就绪：WiFi + NTP + UDP 心跳 + UART1 物理回环自测");
}

void loop() {
  wifiTick();     // WiFi 状态检查与节流重连
  ntpTick();      // 对时状态检测 / 重连后重新对时
  stateTick();    // IDLE -> TX -> WAIT_RX -> PARSE -> SEND
}
