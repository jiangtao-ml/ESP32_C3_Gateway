/*
 * esp32c3_gateway.ino
 * ESP32-C3 RS-485 采集网关（替换树莓派网关项目）
 *
 * 当前里程碑：M5 + 批次1/2/3 —— QI 真机协议：功能码 0x04 读输入寄存器 + 39 寄存器（一帧读全 / 3 段运行时自适应）
 *             批次 3：常驻 AP + Web 配置页 + /status + NVS 持久化（只包一层配置层，485 侧逻辑不变）
 *
 * 已实现：WiFi 连接与最小自恢复、SNTP 对时（含 UNSYNCED 降级）、每秒 1 轮 UDP 上行、
 *        每秒 1 轮 Modbus RTU 请求/响应（一轮 = S1->S2->S3 三个子回合，同一秒内依次跑完）：
 *        帧长驱动接收 + 三级超时、地址/功能码/CRC16 逐段校验、3 段数据域拼接（78 字节原始寄存器值）
 *        【批次 1】上行改为手写 snprintf 组装的 JSON：每轮 1 行心跳 +（三段全成功时）1 行数据；
 *        数据行含 raw（78 字节 HEX，硬约束 12）与 quality / invalid；设备命名 dev = "ESP485-" + MAC 后 6 位；
 *        累计计数器 poll_ok / poll_err / udp_tx（+ seq / rssi / free_heap）
 *        【批次 2】运行时自适应读取：启动先发一帧读全（01 04 00 00 00 27 B0 10，期望 83 字节），
 *        从未成功且连续 QI_FULLFRAME_FALLBACK_AFTER 轮失败则本会话降级为 3 段（S1/S2/S3，37/37/19）；
 *        曾经成功过则永不降级；心跳行新增 mode 字段（"full"/"3seg"）供远程观察当前模式。
 *        【批次 3】常驻 SoftAP + Web 配置页（4 卡片；POST /save 与 GET /reset 走 Basic Auth）
 *        + /status（老师版 22 项逐字 + 我方 dev/mode/free_heap）+ NVS 持久化（命名空间 cfg485，
 *        保存后立即生效、无需重启）；配置层实现于 web_config.h，485 状态机与 UDP JSON 上行一行不改。
 * 未实现：M6 起的状态机加固与断线补传 —— 留待后续；本文件只做读操作，绝不含写/合闸/分闸帧
 *        （M4 的 0x03 回环帧仅保留给 RS485_MODE_LOOPBACK_SELFTEST=1 的串口层自检）
 *
 * 硬约束：
 *   - 全程 millis() 非阻塞调度，禁止 delay()（唯一例外：setup() 内有界等待 USB CDC 枚举，最多 2s）
 *   - 凭据一律不入库 / 不打日志：WiFi 凭据默认值来自 secrets.h；AP 口令与管理口令来自 NVS 或
 *     MAC 派生算法（仅算法入代码）。本文件与 web_config.h 均不出现任何真实密码明文
 *     （串口只打印"已设置 / 未设置 / 开放网络"）
 *   - 不引入第三方库（仅用 ESP32 core 自带的 WiFi / WiFiUdp / time / HardwareSerial
 *     / WebServer / Preferences）；配置页手写 HTML 存 PROGMEM，JSON 手写 snprintf 组装
 *   - ⚠️ USB CDC On Boot 必须 = Enabled：否则 Serial 会落到 UART0（GPIO20/21），
 *     与 UART1 争用同一对引脚
 *   - ⚠️ 485 模块的 RXO 为 5V 时必须先 1kΩ/2kΩ 分压再进 GPIO20
 *   - ⚠️ EN 接法随实物主芯片型号而定（接线前先读 8 脚芯片丝印）：
 *     MAX13487 / MAX13488（5V 双向，芯片无 DE 脚）-> EN 接固定高电平（模块 VCC/5V），
 *       不得悬空 / 接 GND / 动态切换（悬空若对应 SHDN# 会关断器件，症状即 RX=0）；
 *     MAX485 / MAX3485（单向，EN 即方向控制脚）-> EN 必须接 GPIO 由固件切换，严禁固定接高。
 *       固件已内建方向控制：把 config.h 的 RS485_DE_PIN 设为实际 GPIO 号即启用（默认 -1 = 不启用）。
 *     见 00_context.md 三.5。
 *   - ✅【已确认】实物 485 模块主芯片型号 = MAX13487（2026-10-05 丝印确认，见 00_context.md 五 待确认盲区闭环）：
 *     按 AutoDirection（无需方向控制脚）实现，RS485_DE_PIN 保持 -1（EN 固定电平，不得动态切换）。
 *     （历史：曾为三合一款 MAX3485 / MAX485 / MAX13487 致型号未定；若日后换 MAX485 / MAX3485 单向芯片，
 *      把 RS485_DE_PIN 改成实际 GPIO 号即可启用固件方向控制。）
 *
 * 事实来源：04_ai_prompts/00_context.md
 * 编译设置：Arduino IDE -> Tools -> USB CDC On Boot = Enabled
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include <stdarg.h>   // 仅标准库：vsnprintf 追加式组包（非第三方库）
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
  ST_WAIT_RX,    // 等响应：首字节 80ms / 字符间隔 8ms / 帧长驱动 / 整帧兜底 300ms
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
static char    g_payload[QI_UDP_PAYLOAD_MAX];   // 批次 1：384 -> 1024 -> 1200（§8.2 ②：含 raw 78B HEX 的最坏行 1106 B，小缓冲会静默截断）

// ---- 批次 1：设备标识（dev / mac）与累计计数器 ---------------------------------
static char     g_macStr[QI_MAC_STR_LEN] = "00:00:00:00:00:00";  // 完整 MAC（冒号分隔大写）
static char     g_devStr[QI_DEV_STR_LEN] = "ESP485-000000";      // dev = ESP485-<MAC 后 6 位 HEX 大写>
static bool     g_devIdReady = false;       // dev / mac 是否已成功读出
static uint32_t g_seq       = 0;            // 每发一行（心跳行 / 数据行 / err 行）自增 1，uint32_t 回绕正常
static uint32_t g_pollOk    = 0;            // poll_ok：三段全成功的轮数
static uint32_t g_pollErr   = 0;            // poll_err：至少有 1 段失败的轮数
static uint32_t g_udpTx     = 0;            // udp_tx：beginPacket / endPacket 都返回 1 的发送次数

// ---------------------------------------------------------------
// RS-485 收发缓冲（M5）
//   TX：Modbus 请求帧。回环模式 = MODBUS_TEST_FRAME_LEN；总线模式 = 当前段请求帧（均 8 字节）
//   RX：响应帧。与 TX 尺寸不同（发 8 收 37/37/19 或 83），必须是两个独立缓冲，不能共用。
// ---------------------------------------------------------------
static const uint8_t MODBUS_TEST_FRAME_BYTES[] = MODBUS_TEST_FRAME;
static const uint8_t MODBUS_TX_LEN             = MODBUS_TEST_FRAME_LEN;

// 本子回合实际发送长度（P2：运行期变量，不写死）
static uint8_t g_txLen = MODBUS_TX_LEN;

// 帧长驱动的兜底初值（真正判长仍按 byte count 动态算，见 ST_WAIT_RX / parseResponse，P4）：
//   回环模式 = 请求帧长（发 8 收 8）；总线模式 = 当前段期望长（每段在 ST_TX 按 g_readMode 设定）。
//   批次 2 起启动默认一帧读全 => 初值 = QI_FULL_RESP_LEN(83)。
#if RS485_MODE_LOOPBACK_SELFTEST
static uint8_t g_expectRxLen = MODBUS_TX_LEN;
#else
static uint8_t g_expectRxLen = QI_FULL_RESP_LEN;
#endif

// ---- 批次 2：运行时读取模式（一帧读全 <-> 3 段）+ 失败自动降级状态 ---------------
//   g_readMode：0 = 一帧读全（启动默认）/ 1 = 3 段读取（降级后本会话不再回切）。
//   g_fullConsecFail：一帧模式下连续失败轮数（每轮结束时判定一次）。
//   g_fullEverOk：一帧模式本会话是否曾经成功（成功过则永不降级）。
//   放在回环守卫之外：回环模式也要编译（心跳行引用），但恒为默认 0。
static uint8_t g_readMode       = 0;
static uint8_t g_fullConsecFail = 0;
static bool    g_fullEverOk     = false;

static_assert(sizeof(MODBUS_TEST_FRAME_BYTES) == (size_t)MODBUS_TEST_FRAME_LEN,
              "MODBUS_TEST_FRAME_LEN 与 MODBUS_TEST_FRAME 字节数不一致");

#if !RS485_MODE_LOOPBACK_SELFTEST
// ---- M5：0x04 请求帧表（P2）--------------------------------------------------
static const uint8_t QI_SEG1_FRAME_BYTES[] = QI_SEG1_FRAME;
static const uint8_t QI_SEG2_FRAME_BYTES[] = QI_SEG2_FRAME;
static const uint8_t QI_SEG3_FRAME_BYTES[] = QI_SEG3_FRAME;
static const uint8_t QI_FULL_FRAME_BYTES[] = QI_FULL_FRAME;

// 静态检查：各请求帧长度必须与 QI_SEG_FRAME_LEN 一致（编译验证要求）
static_assert(sizeof(QI_SEG1_FRAME_BYTES) == (size_t)QI_SEG_FRAME_LEN,
              "QI_SEG1_FRAME 字节数 != QI_SEG_FRAME_LEN");
static_assert(sizeof(QI_SEG2_FRAME_BYTES) == (size_t)QI_SEG_FRAME_LEN,
              "QI_SEG2_FRAME 字节数 != QI_SEG_FRAME_LEN");
static_assert(sizeof(QI_SEG3_FRAME_BYTES) == (size_t)QI_SEG_FRAME_LEN,
              "QI_SEG3_FRAME 字节数 != QI_SEG_FRAME_LEN");
static_assert(sizeof(QI_FULL_FRAME_BYTES) == (size_t)QI_SEG_FRAME_LEN,
              "QI_FULL_FRAME 字节数 != QI_SEG_FRAME_LEN");

// 批次 2：两组常量表都编译（不再有编译期二选一），运行时按 g_readMode 选择。
//   一帧读全组：1 段、off=0、byteCount=78（落进 g_merged 的结果与 3 段拼接逐字节一致）。
//   3 段组：S1/S2/S3、off=0/32/64、期望 37/37/19。
static const uint8_t  QI_FULL_SEG_COUNT           = 1;
static const uint8_t *const QI_FULL_SEG_FRAMES[1] = { QI_FULL_FRAME_BYTES };
static const uint8_t  QI_FULL_SEG_RESP_LENS[1]    = { QI_FULL_RESP_LEN };
static const uint16_t QI_FULL_SEG_MERGE_OFF[1]    = { 0 };

static const uint8_t  QI_3SEG_COUNT           = 3;
static const uint8_t *const QI_3SEG_FRAMES[3] = { QI_SEG1_FRAME_BYTES, QI_SEG2_FRAME_BYTES, QI_SEG3_FRAME_BYTES };
static const uint8_t  QI_3SEG_RESP_LENS[3]    = { QI_SEG1_RESP_LEN, QI_SEG2_RESP_LEN, QI_SEG3_RESP_LEN };
static const uint16_t QI_3SEG_MERGE_OFF[3]    = { 0, 32, 64 };   // C7：S1->0 / S2->32 / S3->64

// 按 g_readMode 取当前生效表（返回 const 指针，静态分配，非阻塞）
static inline uint8_t qiActiveSegCount() {
  return (g_readMode == 0) ? QI_FULL_SEG_COUNT : QI_3SEG_COUNT;
}
static inline const uint8_t *const *qiActiveFrames() {
  return (g_readMode == 0) ? QI_FULL_SEG_FRAMES : QI_3SEG_FRAMES;
}
static inline const uint8_t *qiActiveRespLens() {
  return (g_readMode == 0) ? QI_FULL_SEG_RESP_LENS : QI_3SEG_RESP_LENS;
}
static inline const uint16_t *qiActiveMergeOff() {
  return (g_readMode == 0) ? QI_FULL_SEG_MERGE_OFF : QI_3SEG_MERGE_OFF;
}

// ---- M5：段索引 / 拼接缓冲 / 轮次结果（P1/P3/P5/P6/P7/P8/P13）----------------
static uint8_t  g_segIndex = 0;                    // 0=S1 / 1=S2 / 2=S3（=0 模式恒为 0=FULL）
static uint8_t  g_merged[QI_MERGED_LEN];           // 39 寄存器 × 2 字节原始值
static bool     g_roundFailed    = false;          // 本轮是否已有失败（任一段失败即整轮失败）
static uint8_t  g_failSeg        = 0xFF;           // 本轮首个失败段号（0/1/2；0xFF=无）
static const char *g_failReason  = NULL;           // 本轮失败原因（六种之一）
static char     g_failReasonBuf[12];               // EXC84-xx 用
static uint8_t  g_segFailStreak[3] = { 0, 0, 0 };  // P7：各段连续失败计数（成功清零）
static uint32_t g_nextSegMs      = 0;              // 段间静默到点时刻（同一轮内推进下一段）

// 段间静默：Modbus RTU 要求相邻帧之间 >= 3.5 个字符时间，9600 8N1 下约 4.0ms。
// 三段在同一秒内连发，若无此间隔，从站可能把两段请求并成一帧。
static const uint32_t RS485_SEG_GAP_MS = 4;
#endif  // !RS485_MODE_LOOPBACK_SELFTEST

static uint8_t  g_txFrame[MODBUS_TX_LEN];
static uint8_t  g_rxFrame[MODBUS_RX_BUF_MAX];
static uint8_t  g_rxLen         = 0;
static bool     g_rxOverflow    = false;  // true = 收到超过缓冲上限的字节（只标记，不写缓冲）
static uint32_t g_tRxStartMs    = 0;      // 三级超时的共同基准（已含 TX_GUARD 保护间隔）
static uint32_t g_tRxLastByteMs = 0;      // 最后一个字节到达时刻（字符间隔判据）
static bool     g_rxIncomplete  = false;  // true = 本次未收满预期帧长（超时 / 半帧）
static uint32_t g_statOk = 0, g_statFail = 0, g_statTimeout = 0;
// ---------------------------------------------------------------
// 批次 3：运行时可配置层（NVS <-> 变量；编译期宏降级为"默认值"）
//   485 采集状态机与 UDP JSON 上行【一行不改】：本层只提供"配置从哪来"。
//   变量在此定义（供 wifiTick / ntpTick / udpSendLine / stateTick 使用）；
//   配置装载 + Web 路由实现在同目录 web_config.h（在本文件 stateTick() 之后 include）。
//   初始值 = 编译期默认（secrets.h 的 WiFi、config.h 的 NTP/UDP/周期），
//   loadConfig() 之后若 NVS 有值则被覆盖。任何真实密码一律不打日志。
// ---------------------------------------------------------------
static char     g_wifiSsid[33]    = WIFI_SSID;        // NVS wifi_ssid；无值回落 secrets.h
static char     g_wifiPass[65]    = WIFI_PASSWORD;    // NVS wifi_pass；无值回落 secrets.h（绝不回显 / 不打日志）
static bool     g_wifiOpen        = false;            // NVS wifi_open：开放网络（无密码）
static char     g_udpTargetIp[16] = UDP_TARGET_IP;    // NVS udp_ip；无值回落 config.h
static uint16_t g_udpTargetPort   = UDP_TARGET_PORT;  // NVS udp_port；无值回落 config.h
static char     g_ntpPrimary[48]  = NTP_SERVER_1;     // NVS ntp1（NTP 主源）；备源 NTP_SERVER_2/3 不变
static uint32_t g_pollIntervalMs  = POLL_INTERVAL_MS; // NVS poll_s（1~5s）；无值回落 POLL_INTERVAL_MS
static char     g_adminUser[33]   = "admin";          // NVS admin_user（Web 写操作 Basic Auth 账号）
static char     g_adminPass[65]   = { 0 };            // NVS admin_pass；无值 = 与 AP 口令同源的 MAC 派生算法
static char     g_apPass[65]      = { 0 };            // NVS ap_pass（常驻 SoftAP 口令；<8 字符会被 softAP 拒绝）
static char     g_apSsid[33]      = { 0 };            // ESP485-<MAC 后 6 位>（= g_devStr），loadConfig() 填充
static bool     g_configured      = false;            // NVS 是否存过任意配置键（/status 的 configured）
static bool     g_wifiFromBuiltin = true;             // WiFi 凭据是否来自编译期内置默认（secrets.h）

// ---------------------------------------------------------------
// 【批次 3.5】LED 状态指示（板载蓝灯 GPIO8，低电平点亮；全 millis() 非阻塞，绝不 delay）
//   语义见 config.h LED_* 宏注释：WiFi 未连接 = 慢闪；采集成功 = 短亮脉冲；采集失败 = 长亮脉冲。
//   脉冲与慢闪互斥：WiFi 断开时 sendUdpPayload 不计数不脉冲，慢闪独占；连上后回到脉冲模式。
// ---------------------------------------------------------------
static bool     g_ledOn           = false;            // 当前电平（true=亮）；慢闪翻转状态
static uint32_t g_ledOffAtMs      = 0;                // >0 = 脉冲亮着，到点熄灭
static uint32_t g_ledNextFlipMs   = 0;                // 慢闪翻转时刻

static void ledPulse(uint32_t ms) {                   // 触发一次"亮 ms 后灭"的脉冲（供 ST_SEND 每轮调用）
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
  g_ledOn      = true;
  g_ledOffAtMs = millis() + ms;
}

static void ledTick() {
  const uint32_t now = millis();
  if (g_ledOffAtMs != 0 && (int32_t)(now - g_ledOffAtMs) >= 0) {   // 脉冲到点熄灭
    digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
    g_ledOn      = false;
    g_ledOffAtMs = 0;
  }
  if (!g_linkUp) {                                      // WiFi 未连接：慢闪（500ms 翻转）
    if ((int32_t)(now - g_ledNextFlipMs) >= 0) {
      g_ledNextFlipMs = now + LED_WIFI_SLOW_MS;
      g_ledOn = !g_ledOn;
      digitalWrite(LED_PIN, (g_ledOn != (bool)LED_ACTIVE_LOW) ? LOW : HIGH);
    }
  } else {
    if (g_ledNextFlipMs != 0) {                         // 刚从慢闪切回脉冲模式：先灭灯（避免恢复瞬间常亮）
      digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
      g_ledOn          = false;
      g_ledOffAtMs     = 0;
      g_ledNextFlipMs  = 0;
    }
  }
}

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

#if RS485_MODE_LOOPBACK_SELFTEST
// 仅回环自检（RS485_MODE_LOOPBACK_SELFTEST=1）的文本心跳使用；JSON 上行走 buildTimestampIso()。
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
#endif

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


// 追加式组包：把 fmt 追加到 buf[*pos] 之后并推进 *pos。
// 任一子串写不下（vsnprintf 返回值 >= 剩余容量）即置 *truncated = true 并停止后续追加；
// 与旧实现一致：缓冲不足必须显式告警，绝不静默截断。仅用 <stdarg.h>，无第三方库。
static bool appendf(char *buf, size_t cap, size_t *pos, bool *truncated, const char *fmt, ...) {
  if (*truncated) return false;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= (cap - *pos)) {
    *truncated = true;
    return false;
  }
  *pos += (size_t)n;
  return true;
}

// 批次 1：设备命名 —— 读 MAC 并拼出 mac / dev（dev = "ESP485-" + MAC 后 6 位 HEX 大写）。
// 返回 false 表示本次读到全 0（WiFi 栈尚未就绪），调用方可稍后重试。
static bool refreshDevId() {
  uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
  WiFi.macAddress(mac);
  const bool allZero = (uint8_t)(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0;
  if (allZero && g_devIdReady) return true;          // 已有有效值就别用全 0 覆盖
  snprintf(g_macStr, sizeof(g_macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
           (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
  snprintf(g_devStr, sizeof(g_devStr), "%s%02X%02X%02X",
           QI_DEV_PREFIX, (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
  g_devIdReady = !allZero;
  return g_devIdReady;
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
    WiFi.begin(g_wifiSsid, g_wifiOpen ? (const char *)nullptr : g_wifiPass);   // 批次 3：用运行时凭据（NVS 或 secrets.h 默认）重连
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
static const char *kNtpCfg[3] = { NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3 };   // 批次 3：slot0 指向运行时主源（loadConfig() 里改为 g_ntpPrimary）

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

// JSON 时间戳（ISO8601，批次 1 3.7）：已对时 -> "YYYY-MM-DDTHH:MM:SS"；未对时 -> "BOOT+<秒>"。
// 返回 true = 本轮会话确已收到 NTP 回包（即 JSON 的 ts_ok = true）。
// 判据仍是 ntpHasReply()（本轮会话确实收到过 NTP 回包），不得改回"用 getLocalTime() 判同步"：
//   芯片不断电时 RTC 保留上一轮的时间，软复位后 getLocalTime() 立即为真却与本次是否对过时无关。
// 这里额外要求 getLocalTime(&now, 0) 成功，只是确认系统时钟已就绪（与 ntpTick 的宣告条件一致），
// 避免出现"ts_ok=true 却只能填 BOOT+"的自相矛盾。
static bool buildTimestampIso(char *out, size_t outSize) {
  struct tm now;
  if (ntpHasReply() && getLocalTime(&now, 0)) {   // 0 = 不阻塞等待
    strftime(out, outSize, "%Y-%m-%dT%H:%M:%S", &now);
    return true;
  }
  snprintf(out, outSize, "BOOT+%lu", (unsigned long)(millis() / 1000UL));
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
    configTzTime(NTP_TZ_INFO, g_ntpPrimary, NTP_SERVER_2, NTP_SERVER_3);   // 批次 3：主源为运行时变量
    Serial.printf("[NTP] WiFi 已连上 -> 触发 SNTP 对时 TZ=%s 主=%s 备1=%s 备2=%s\n",
                  NTP_TZ_INFO, g_ntpPrimary, NTP_SERVER_2, NTP_SERVER_3);
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
    configTzTime(NTP_TZ_INFO, g_ntpPrimary, NTP_SERVER_2, NTP_SERVER_3);   // 批次 3：主源为运行时变量
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

#if !RS485_MODE_LOOPBACK_SELFTEST
// 段号标签（P10：解析日志要能看出当前段号与功能码 0x04）
static const char *qiSegTagFor(uint8_t idx) {
  if (g_readMode == 0) {              // 一帧读全：失败段 tag = "F"
    (void)idx;
    return QI_SEG_TAG_FULL;
  }
  static const char *const kTag[3] = { "S1", "S2", "S3" };
  return (idx < 3) ? kTag[idx] : "S?";
}
static const char *qiSegTag() { return qiSegTagFor(g_segIndex); }

// 记录本段失败并维护 P7 连续失败计数；reason 限六种之一
static void recordSegFail(const char *reason) {
  if (!g_roundFailed) {                 // 只记本轮第一处失败（供 P13）
    g_roundFailed = true;
    g_failSeg     = g_segIndex;
    g_failReason  = reason;
  }
  if (g_readMode == 0 && g_fullConsecFail < 255) g_fullConsecFail++;   // 批次 2：一帧模式失败轮计数
  if (g_segFailStreak[g_segIndex] < 255) g_segFailStreak[g_segIndex]++;
  if (g_segFailStreak[g_segIndex] >= 3) {   // P7：连续失败 N=3 起整轮丢弃并显式告警
    Serial.printf("[M5] PARTIAL/FAIL SEG=%s 连续失败 %u 次 原因=%s（本轮整体丢弃，不上传残缺数据）\n",
                  qiSegTag(), (unsigned)g_segFailStreak[g_segIndex], reason);
  }
}

static void recordSegOk() {
  g_segFailStreak[g_segIndex] = 0;      // P7：成功即清零
  if (g_readMode == 0) {                // 批次 2：一帧成功 => 曾成功 + 清连续失败（此后永不降级）
    g_fullEverOk     = true;
    g_fullConsecFail = 0;
  }
}
#endif  // !RS485_MODE_LOOPBACK_SELFTEST

#if !RS485_MODE_LOOPBACK_SELFTEST

// ---------------------------------------------------------------
// 批次 1 - 数据行（仅三段全成功时发 1 行）
//   字段顺序照抄 02_docs/17 3.1 的原文格式串（便于日后与老师的数据并表），
//   末尾追加我方相对老师版本的两处改进（02_docs/16 9.4）：
//     - "raw"：78 字节原始寄存器值 HEX => 公式可演进 + 历史可重放（老师版没有；硬约束 12）
//     - "quality" / "invalid"：无效字段（量程越界 / 无传感器伪装）显式标出
//   另有 "seg"：三段读取 = "3"，一帧读全 = "1"（批次 2 起运行时判定）。
// 本段只做"读出来再换算"，不改 485 侧任何逻辑（帧 / 段数 / 超时 / 解析 / CRC 全不动）。
// ---------------------------------------------------------------

// 取第 i 个寄存器（大端：高字节在前）
static inline uint16_t qiReg(uint8_t i) {
  return (uint16_t)(((uint16_t)g_merged[(size_t)i * 2] << 8) | (uint16_t)g_merged[(size_t)i * 2 + 1]);
}

// invalid 列表：逗号分隔，按字段出现顺序追加
static void addInvalid(char *out, size_t cap, const char *name) {
  size_t used = strlen(out);
  if (used != 0) {
    if (used + 1 >= cap) return;
    out[used++] = ',';
  }
  out[used] = '\0';
  size_t room = cap - used - 1;
  if (room > 0) strncat(out, name, room);
}

// 数据行 seg（成功轮读取模式）：一帧读全 -> "1"；三段读取 -> "3"（批次 2 起运行时判定）
static inline const char *qiDataSegTag() {
  return (g_readMode == 0) ? QI_SEG_TAG_MODE_FULL : QI_SEG_TAG_MODE_3SEG;
}

// 换算口径 = 05_hardware/STB3-125_字段字典.csv 的 conv 列：
//   raw/10、raw/1000、raw-40、raw/100、raw*0.01、1:1、高字、(H*65536)/1000+L
// invalid 口径 = 06_tests/db_proto.py 的 L2 语义：
//   A/B/C_temp == -40 = "已知不上报"（QI_PHASE_TEMP_ABSENT=1 时**不进** invalid、不降 quality）；
//   其余越界（含 A/B/C_temp 真越界 < -40 或 > 160）才算本次异常（N_temp 不参与 "== -40" 判定）。
static bool buildDataJson(const char *ts, bool tsOk) {
  const uint16_t rOpen = qiReg(0), rRec1 = qiReg(1), rRec2 = qiReg(2), rRec3 = qiReg(3);
  const uint16_t rFreq = qiReg(4), rLeak = qiReg(5), rNTemp = qiReg(6);

  const double freq  = (double)rFreq / 10.0;     // Hz
  const double leak  = (double)rLeak / 1000.0;   // A
  const int    nTemp = (int)rNTemp - 40;         // C

  char invalid[QI_INVALID_MAX];
  invalid[0] = '\0';
  if (freq < 0.0 || freq > 100.0)  addInvalid(invalid, sizeof(invalid), "frequent");
  if (leak < 0.0 || leak > 1.0)    addInvalid(invalid, sizeof(invalid), "leakage_current");
  if (nTemp < -40 || nTemp > 160)  addInvalid(invalid, sizeof(invalid), "N_temp");

  static const char kPhase[3] = { 'A', 'B', 'C' };
  const uint8_t kBase[3]      = { 7, 16, 25 };   // A:7..15 / B:16..24 / C:25..33
  int      pTemp[3];
  uint16_t pVolt[3], pPower[3], pQ[3], pS[3], pH[3];
  double   pCur[3], pPf[3], pEnergy[3];

  for (uint8_t i = 0; i < 3; i++) {
    const uint8_t  b  = kBase[i];
    const uint16_t rT = qiReg(b + 0), rV = qiReg(b + 1), rI = qiReg(b + 2), rP = qiReg(b + 3);
    const uint16_t rW = qiReg(b + 4), rR = qiReg(b + 5), rA = qiReg(b + 6);
    const uint16_t rH = qiReg(b + 7), rL = qiReg(b + 8);

    pTemp[i]  = (int)rT - 40;                 // C   （raw-40）
    pVolt[i]  = rV;                           // V   （1:1）
    pCur[i]   = (double)rI / 100.0;           // A   （raw/100）
    pPf[i]    = (double)rP * 0.01;            //     （raw*0.01）
    pPower[i] = rW;                           // W   （1:1）
    pQ[i]     = rR;                           // VAR （1:1）
    pS[i]     = rA;                           // VA  （1:1）
    pH[i]     = rH;                           // 高字
    // 电量公式存疑（与 06_tests/db_proto.py 文件头一致）：
    //   文档 / 字段字典写 (H*65536)/1000 + L；若该字段真实语义是"32 位累加器、单位 0.001 度"，
    //   正确式应为 (H*65536 + L)/1000 —— 两者相差 1000 倍。此处按文档实现，不擅自改口径。
    pEnergy[i] = (double)rH * 65536.0 / 1000.0 + (double)rL;   // 度

    char nm[12];
    if (pTemp[i] == -40) {
#if QI_PHASE_TEMP_ABSENT
      // §8.2 ①：该型号恒不上报接线柱温度（raw 恒 0 => 恰为量程下限 -40）= "已知不上报"：
      //   不进 invalid、不降 quality（口径同 06_tests/db_proto.py --known-missing）。
#else
      snprintf(nm, sizeof(nm), "%c_temp", kPhase[i]);   // 退回旧行为：== -40 也视为无效
      addInvalid(invalid, sizeof(invalid), nm);
#endif
    } else if (pTemp[i] < -40 || pTemp[i] > 160) {
      snprintf(nm, sizeof(nm), "%c_temp", kPhase[i]);   // 真越界 = 本次异常：进 invalid、降 PARTIAL
      addInvalid(invalid, sizeof(invalid), nm);
    }
    if (pVolt[i] > 600) {                     // 量程 0..600 V（1:1）
      snprintf(nm, sizeof(nm), "%c_voltage", kPhase[i]);
      addInvalid(invalid, sizeof(invalid), nm);
    }
    if (pPf[i] > 1.0) {                       // 量程 0..1（raw*0.01）
      snprintf(nm, sizeof(nm), "%c_Pf", kPhase[i]);
      addInvalid(invalid, sizeof(invalid), nm);
    }
    // 电流 raw/100 => 上限 655.35 A，与字段字典量程 [0,655.35] 恒成立，无需判越界
  }

  const uint16_t rPower = qiReg(34), rQ = qiReg(35), rS = qiReg(36);
  const uint16_t rH     = qiReg(37), rL = qiReg(38);
  const double   energy = (double)rH * 65536.0 / 1000.0 + (double)rL;   // 度（公式同上，存疑）

  char rawHex[QI_RAW_HEX_MAX];
  bytesToHex(g_merged, QI_MERGED_LEN, rawHex, sizeof(rawHex));

  size_t pos   = 0;
  bool   trunc = false;
  ++g_seq;                                    // 每发一行自增 1（uint32_t 回绕正常）

  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "{\"seq\":%lu,\"dev\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\",\"ts\":\"%s\",\"ts_ok\":%s,",
          (unsigned long)g_seq, g_devStr, g_macStr,
          WiFi.localIP().toString().c_str(), ts, tsOk ? "true" : "false");

  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "\"openState\":\"%04X\",\"record1\":\"%04X\",\"record2\":\"%04X\",\"record3\":\"%04X\","
          "\"frequent\":%.1f,\"leakage_current\":%.3f,\"N_temp\":%d,",
          (unsigned)rOpen, (unsigned)rRec1, (unsigned)rRec2, (unsigned)rRec3,
          freq, leak, nTemp);

  for (uint8_t i = 0; i < 3; i++) {
    appendf(g_payload, sizeof(g_payload), &pos, &trunc,
            "\"%c_temp\":%d,\"%c_voltage\":%u,\"%c_current\":%.2f,\"%c_Pf\":%.2f,"
            "\"%c_power\":%u,\"%c_Q\":%u,\"%c_S\":%u,\"%c_H\":%u,\"%c_L\":%.3f,",
            kPhase[i], pTemp[i], kPhase[i], (unsigned)pVolt[i], kPhase[i], pCur[i],
            kPhase[i], pPf[i], kPhase[i], (unsigned)pPower[i], kPhase[i], (unsigned)pQ[i],
            kPhase[i], (unsigned)pS[i], kPhase[i], (unsigned)pH[i], kPhase[i], pEnergy[i]);
  }

  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "\"power\":%u,\"Q\":%u,\"S\":%u,\"H\":%u,\"L\":%.3f,",
          (unsigned)rPower, (unsigned)rQ, (unsigned)rS, (unsigned)rH, energy);

  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "\"quality\":\"%s\",\"invalid\":\"%s\",\"seg\":\"%s\",\"raw\":\"%s\"}",
          (invalid[0] != '\0') ? "PARTIAL" : "OK", invalid, qiDataSegTag(), rawHex);

  return !trunc;
}
#endif  // !RS485_MODE_LOOPBACK_SELFTEST
// ---------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------
// ---------------------------------------------------------------
// 批次 1 - 心跳行（每轮都发，独立一类）
//   seq / dev / ts / ts_ok / type / rssi / free_heap / 计数器（queue、dropped 本批固定 0）
//   与 02_docs/17 3.2 老师原串的差异：去掉 cfg 字段、新增 dev（本批命名要求）。
// ---------------------------------------------------------------
static bool buildHeartbeatJson(const char *ts, bool tsOk) {
  size_t pos   = 0;
  bool   trunc = false;
  ++g_seq;                                    // 每发一行自增 1（uint32_t 回绕正常）
  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "{\"seq\":%lu,\"dev\":\"%s\",\"ts\":\"%s\",\"ts_ok\":%s,\"type\":\"hb\","
          "\"rssi\":%d,\"free_heap\":%u,\"mode\":\"%s\",\"poll_ok\":%lu,\"poll_err\":%lu,\"udp_tx\":%lu,"
          "\"queue\":%u,\"dropped\":%u}",
          (unsigned long)g_seq, g_devStr, ts, tsOk ? "true" : "false",
          (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
          (g_readMode == 0) ? "full" : "3seg",
          (unsigned long)g_pollOk, (unsigned long)g_pollErr, (unsigned long)g_udpTx,
          (unsigned)0, (unsigned)0);
  return !trunc;
}

#if QI_SEND_ERR_ROW && !RS485_MODE_LOOPBACK_SELFTEST
// ---------------------------------------------------------------
// 批次 1 §8.2 ③ - 失败行（type:"err"）
//   仅在失败轮、于心跳行之外再发一行。第一次真机运行时 USB 必须拔掉（安全红线）
//   => 没有串口 => 这行是唯一能看见"失败段 + 原因"的通道（串口 FAIL:Sx:原因 始终保留）。
//   格式串与指令 §8.2 ③ 逐字一致（seq / dev / ts / ts_ok / type / seg / reason / poll_err）。
// ---------------------------------------------------------------
static bool buildErrJson(const char *ts, bool tsOk) {
  size_t pos   = 0;
  bool   trunc = false;
  ++g_seq;                                    // 每发一行自增 1（uint32_t 回绕正常）
  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "{\"seq\":%lu,\"dev\":\"%s\",\"ts\":\"%s\",\"ts_ok\":%s,\"type\":\"err\","
          "\"seg\":\"%s\",\"reason\":\"%s\",\"poll_err\":%lu}",
          (unsigned long)g_seq, g_devStr, ts, tsOk ? "true" : "false",
          qiSegTagFor(g_failSeg),
          (g_failReason != NULL) ? g_failReason : "LEN",
          (unsigned long)g_pollErr);
  return !trunc;
}
#endif

// 把 g_payload 中已组装好的一行发出去；成功（beginPacket / endPacket 都返回 1）则累加 udp_tx
static bool udpSendLine() {
  bool ok = (g_udp.beginPacket(g_udpTargetIp, g_udpTargetPort) == 1);   // 不判返回值会让日志说谎：发失败仍打印"已发"
  if (ok) {
    g_udp.write((const uint8_t *)g_payload, strlen(g_payload));
    ok = (g_udp.endPacket() == 1);
  }
  if (ok) {
    g_udpTx++;
  } else {
    Serial.printf("[UDP][ERR] 发送失败（beginPacket/endPacket 未返回 1），目标 %s:%u\n",
                  g_udpTargetIp, (unsigned)g_udpTargetPort);
  }
  Serial.printf("[UDP -> %s:%u] %s\n", g_udpTargetIp, (unsigned)g_udpTargetPort, g_payload);
  return ok;
}

static void sendUdpPayload() {
#if !RS485_MODE_LOOPBACK_SELFTEST
  // 轮次计数与 UDP 是否发得出去无关：poll_ok / poll_err 反映本轮 485 采集结果
  if (g_roundFailed) g_pollErr++;
  else               g_pollOk++;
#endif

  // WiFi 未连接：beginPacket 必然失败，若照发会每秒刷一条 [UDP][ERR] 掩盖真正的问题。
  // 此处直接跳过并做 5s 节流提示，保证日志只在"已联网但仍发不出去"时报错。
  // 代价：掉线期间无法满足"每轮必发一行"（现场看不到心跳）；见批次 1 回报第 5 条。
  if (!g_linkUp) {
    static uint32_t lastNoLinkLogMs = 0;
    if ((uint32_t)(millis() - lastNoLinkLogMs) >= 5000) {
      lastNoLinkLogMs = millis();
      Serial.println("[UDP] WiFi 未连接，本秒跳过上行（等待 wifiTick 重连）");
    }
    return;
  }

#if RS485_MODE_LOOPBACK_SELFTEST
  // Loopback selftest: keep legacy text heartbeat unchanged (batch-1 hard constraint 6/13).
  {
    char ts[32];
    bool synced = buildTimestamp(ts, sizeof(ts));
    int  n = snprintf(g_payload, sizeof(g_payload), "%s | %s", ts, synced ? "HEARTBEAT" : "UNSYNCED");
    if (n < 0 || (size_t)n >= sizeof(g_payload)) {
      Serial.printf("[UDP][ERR] payload buffer too small: need %d / cap %u\n", n, (unsigned)sizeof(g_payload));
    }
    udpSendLine();
  }
#else
  if (!g_devIdReady) (void)refreshDevId();    // 首次读到全 0 时在这里补算

  char ts[QI_TS_ISO_LEN];
  const bool tsOk = buildTimestampIso(ts, sizeof(ts));

#if !RS485_MODE_LOOPBACK_SELFTEST
  if (g_roundFailed) {
    // 失败原因必须留在串口日志里（FAIL:Sx:原因 无论 QI_SEND_ERR_ROW 取值都保留）。
    // §8.2 ③：拔 USB 后串口拿不到 => QI_SEND_ERR_ROW=1 时另发一行 type:"err"（见下）。
    Serial.printf("[M5] 本轮失败：SEG=%s 原因=%s（已计入 poll_err）\n",
                  qiSegTagFor(g_failSeg),
                  (g_failReason != NULL) ? g_failReason : "LEN");
  }
#endif

  // 1) 心跳行：每轮都发（含失败轮）
  if (!buildHeartbeatJson(ts, tsOk)) {
    Serial.printf("[UDP][ERR] payload 缓冲不足：心跳行装不下（容量 %u），本包已截断\n",
                  (unsigned)sizeof(g_payload));
  }
  udpSendLine();

#if !RS485_MODE_LOOPBACK_SELFTEST
#if QI_SEND_ERR_ROW
  // 2) 失败行：失败轮在心跳行之外再发一行（§8.2 ③；拔 USB 后唯一的失败可见通道）
  if (g_roundFailed) {
    if (!buildErrJson(ts, tsOk)) {
      Serial.printf("[UDP][ERR] payload 缓冲不足：err 行装不下（容量 %u），本包已截断\n",
                    (unsigned)sizeof(g_payload));
    }
    udpSendLine();
  }
#endif

  // 3) 数据行：三段全成功时再发一行（含 raw 78 字节 HEX）
  if (!g_roundFailed) {
    if (!buildDataJson(ts, tsOk)) {
      Serial.printf("[UDP][ERR] payload 缓冲不足：数据行装不下（容量 %u），本包已截断\n",
                    (unsigned)sizeof(g_payload));
    }
    udpSendLine();
  }
#endif
#endif
}

#if !RS485_MODE_LOOPBACK_SELFTEST
// 把当前 RX 帧按十六进制打到下一行（异常帧定位用；最长打 24 字节，避免刷屏）
static void logRxHex() {
  uint8_t n = (g_rxLen > 24) ? 24 : g_rxLen;
  char buf[3 * 24 + 8];
  bytesToHex(g_rxFrame, n, buf, sizeof(buf));
  Serial.printf("      RX=%u | %s%s\n", (unsigned)g_rxLen, buf, (g_rxLen > n) ? " ..." : "");
}

// ---------------- M5：真实 485 总线 —— Modbus RTU 响应解析 + 3 段记账 ----------------
// 顺序：超时/半帧 -> 异常响应 -> 地址 -> 功能码 -> byte count 帧长 -> CRC -> 拼接
// 注：帧长仍按 byte count 动态算（need = rxFrame[2] + 5，对 37/37/19/83 自动成立，P4），
//     不按段写死 37/19。每段独立校验（P6）；失败原因记入 g_failReason 供 P13 上行。
static void parseResponse() {
  const char   *tag     = qiSegTag();
  const uint8_t reqFunc = qiActiveFrames()[g_segIndex][1];   // M5 恒为 0x04（只读）

  // (1) 超时 / 半帧：三级超时或字符间隔先触发
  if (g_rxIncomplete) {
    g_statTimeout++;
    recordSegFail("TIMEOUT");
    Serial.printf("[M4] 超时（首字节 %ums / 字符间隔 %ums / 兜底 %ums）RX=%u 期望 %u SEG=%s REQ=0x%02X\n",
                  (unsigned)RS485_FIRST_BYTE_TIMEOUT_MS, (unsigned)RS485_CHAR_TIMEOUT_MS,
                  (unsigned)RS485_FRAME_MAX_MS, (unsigned)g_rxLen, (unsigned)g_expectRxLen,
                  tag, (unsigned)reqFunc);
    if (g_rxLen) logRxHex();
    return;
  }

  // (2) 从站异常响应（addr + (func|0x80) + 异常码 + CRC2 = 5 字节）
  //     必须放在其它判定之前：否则 5 字节异常帧会先被判成帧长异常（E-02 的主要诊断依据）。
  if (g_rxLen >= 5 && (g_rxFrame[1] & 0x80)) {
    const uint16_t crcR = (uint16_t)g_rxFrame[3] | ((uint16_t)g_rxFrame[4] << 8);
    const uint16_t crcC = modbusCRC16(g_rxFrame, 3);
    if (crcC != crcR) {
      g_statFail++; recordSegFail("CRC");
      Serial.printf("[M4] 异常响应 CRC 失败 期望 %04X 计算 %04X SEG=%s REQ=0x%02X\n",
                    (unsigned)crcR, (unsigned)crcC, tag, (unsigned)reqFunc);
      logRxHex();
      return;
    }
    if (g_rxFrame[0] != RS485_SLAVE_ADDR) {
      g_statFail++; recordSegFail("ADDR");
      Serial.printf("[M4] 异常响应地址不匹配 期望 %u 收到 %u SEG=%s REQ=0x%02X\n",
                    (unsigned)RS485_SLAVE_ADDR, (unsigned)g_rxFrame[0], tag, (unsigned)reqFunc);
      logRxHex();
      return;
    }
    g_statFail++;   // 合法异常帧但代表从站报错，计入失败
    if (g_rxFrame[1] == (uint8_t)(MODBUS_FUNC_READ_INPUT | 0x80)) {   // 0x84：本段功能码被拒
      snprintf(g_failReasonBuf, sizeof(g_failReasonBuf), "EXC84-%02X", (unsigned)g_rxFrame[2]);
      recordSegFail(g_failReasonBuf);
    } else {
      recordSegFail("FUNC");
    }
    Serial.printf("[M4] 从站异常响应 FUNC=0x%02X 异常码=0x%02X SEG=%s REQ=0x%02X（参见 06_QI协议说明.md §7 E-02）\n",
                  (unsigned)g_rxFrame[1], (unsigned)g_rxFrame[2], tag, (unsigned)reqFunc);
    logRxHex();
    return;
  }

  const uint8_t addr = g_rxFrame[0];
  const uint8_t func = g_rxFrame[1];

  // (3) 地址匹配
  if (addr != RS485_SLAVE_ADDR) {
    g_statFail++; recordSegFail("ADDR");
    Serial.printf("[M4] 地址不匹配 期望 %u 收到 %u SEG=%s REQ=0x%02X\n",
                  (unsigned)RS485_SLAVE_ADDR, (unsigned)addr, tag, (unsigned)reqFunc);
    logRxHex();
    return;
  }

  // (4) 功能码匹配（异常响应已在 (2) 提前处理，此处只判正常响应）
  if (func != MODBUS_FUNC_READ_HOLDING && func != MODBUS_FUNC_READ_INPUT) {
    g_statFail++; recordSegFail("FUNC");
    Serial.printf("[M4] 功能码不匹配 期望 0x%02X/0x%02X 收到 0x%02X SEG=%s REQ=0x%02X\n",
                  (unsigned)MODBUS_FUNC_READ_HOLDING, (unsigned)MODBUS_FUNC_READ_INPUT,
                  (unsigned)func, tag, (unsigned)reqFunc);
    logRxHex();
    return;
  }

  // (5) 帧长：按 byte count 动态算（P4，对 37/37/19/83 自动成立，不按段写死）
  const uint8_t byteCount = g_rxFrame[2];
  if ((uint16_t)(byteCount + 5) != (uint16_t)g_rxLen) {
    g_statFail++; recordSegFail("LEN");
    Serial.printf("[M4] 帧长异常 期望 %u 收到 %u（byte count=%u）SEG=%s REQ=0x%02X\n",
                  (unsigned)(byteCount + 5), (unsigned)g_rxLen, (unsigned)byteCount,
                  tag, (unsigned)reqFunc);
    logRxHex();
    return;
  }

  // (6) 完整帧 CRC16/Modbus
  const uint16_t crcRecv = (uint16_t)g_rxFrame[g_rxLen - 2] |
                           ((uint16_t)g_rxFrame[g_rxLen - 1] << 8);
  const uint16_t crcCalc = modbusCRC16(g_rxFrame, (size_t)(g_rxLen - 2));
  if (crcCalc != crcRecv) {
    g_statFail++; recordSegFail("CRC");
    Serial.printf("[M4] CRC 失败 期望 %04X 计算 %04X SEG=%s REQ=0x%02X\n",
                  (unsigned)crcRecv, (unsigned)crcCalc, tag, (unsigned)reqFunc);
    logRxHex();
    return;
  }

  // (7) P5 拼接：把数据域（去 addr/func/byteCount/CRC）拷到合并缓冲的段偏移
  const uint16_t off = qiActiveMergeOff()[g_segIndex];
  if ((uint32_t)off + (uint32_t)byteCount > (uint32_t)QI_MERGED_LEN) {
    g_statFail++; recordSegFail("LEN");
    Serial.printf("[M4] 帧长异常 段=%s 数据 %u 字节越界（off=%u / 合并缓冲 %u）SEG=%s REQ=0x%02X\n",
                  tag, (unsigned)byteCount, (unsigned)off, (unsigned)QI_MERGED_LEN,
                  tag, (unsigned)reqFunc);
    logRxHex();
    return;
  }
  memcpy(g_merged + off, g_rxFrame + 3, byteCount);
  recordSegOk();

  g_statOk++;
  char dataHex[3 * MODBUS_RX_BUF_MAX + 1];
  bytesToHex(g_rxFrame + 3, byteCount, dataHex, sizeof(dataHex));
  Serial.printf("[M4] 解析 OK ADDR=%u FUNC=0x%02X DATA=%s SEG=%s REQ=0x%02X\n",
                (unsigned)addr, (unsigned)func, dataHex, tag, (unsigned)reqFunc);
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
    case ST_IDLE: {
#if !RS485_MODE_LOOPBACK_SELFTEST
      // 同一轮内推进下一段：只等段间静默（>= t3.5），不等 1s 轮询节拍（P1/P9）
      if (g_segIndex != 0) {
        if ((int32_t)(millis() - g_nextSegMs) >= 0) g_state = ST_TX;
        break;
      }
#endif
      if ((int32_t)(millis() - g_nextPollMs) >= 0) g_state = ST_TX;
      break;
    }

    case ST_TX: {
      // ⚠️ 清 RX 缓冲必须在 write() 【之前】：
      // 物理回环下"发"与"收"是同时进行的，字节在 flush 返回前就已进 RX 缓冲；
      // 若照搬到 flush() 之后再清，会把刚收回来的字节一并丢掉 → 必然误报 TIMEOUT。
      // 真实总线上清的是上一周期残留（半帧 / 噪声 / 上一次的迟到响应）。
      while (Serial1.available()) (void)Serial1.read();

#if RS485_MODE_LOOPBACK_SELFTEST
      // 回环自测：固定发 M3/M4 的 0x03 回环帧（发 8 收 8），本分支保持不动
      memcpy(g_txFrame, MODBUS_TEST_FRAME_BYTES, MODBUS_TX_LEN);
      g_txLen       = MODBUS_TX_LEN;
      g_expectRxLen = MODBUS_TX_LEN;
#else
      // P2：按 g_readMode + g_segIndex 选当前请求帧（只读 0x04，无写 / 合闸 / 分闸 / 广播）
      memcpy(g_txFrame, qiActiveFrames()[g_segIndex], QI_SEG_FRAME_LEN);
      g_txLen       = QI_SEG_FRAME_LEN;
      g_expectRxLen = qiActiveRespLens()[g_segIndex];   // 仅兜底初值；判长仍按 byte count 动态算（P4）
#endif
#if RS485_DE_PIN >= 0
      // MAX485 / MAX3485 半双工：先把方向切到发送，否则帧根本上不了总线
      digitalWrite(RS485_DE_PIN, RS485_DE_ACTIVE_HIGH ? HIGH : LOW);
#endif
      Serial1.write(g_txFrame, g_txLen);
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
      //   - 正常响应：addr + func + byteCount + N + CRC2 = byteCount + 5（对 37/37/19/83 自动成立）
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

    case ST_PARSE: {
      parseResponse();                                     // 回环=逐字节比对；总线=Modbus RTU 解析 + 段记账
      {
        uint32_t total = g_statOk + g_statFail + g_statTimeout;
        if (total > 0 && (total % RS485_STATS_EVERY_N) == 0) {   // 每 100 次打印一次累计
          Serial.printf("[485] 统计：成功 %lu / 失败 %lu / 超时 %lu\n",
                        (unsigned long)g_statOk, (unsigned long)g_statFail,
                        (unsigned long)g_statTimeout);
        }
      }
#if !RS485_MODE_LOOPBACK_SELFTEST
      // P3：一段（无论成败）结束 -> 段号前进；走完最后一段回 0 并转 ST_SEND（本轮结束）
      g_segIndex++;
      if (g_segIndex >= qiActiveSegCount()) {
        g_segIndex = 0;
        g_state    = ST_SEND;              // P13：本轮结束 -> 恰好发一行 UDP
      } else {
        g_nextSegMs = millis() + RS485_SEG_GAP_MS;   // 段间 >= t3.5 静默后立即发下一段
        g_state     = ST_IDLE;
      }
#else
      g_state = ST_SEND;
#endif
      break;
    }

    case ST_SEND:
      sendUdpPayload();                    // P13：总线模式发「心跳 +（失败）err 行 +（全成功）数据行」；回环模式保留心跳
#if !RS485_MODE_LOOPBACK_SELFTEST
      // 批次 2：一帧读全失败自动降级判定（只在轮末判一次，一轮一次；本会话不回切）。
      //   从未成功过 && 连续失败达阈值 => 降级为 3 段（err 行已在上行里按旧模式标好 "F"）；
      //   曾经成功过则永不降级（偶发失败是总线问题，3 段同样会失败）。
      if (g_readMode == 0 && !g_fullEverOk && g_fullConsecFail >= QI_FULLFRAME_FALLBACK_AFTER) {
        g_readMode       = 1;
        g_fullConsecFail = 0;
        Serial.printf("[M5] 一帧读全连续 %u 轮失败（%s），本会话降级为 3 段读取（E-02 B 分支）\n",
                      (unsigned)QI_FULLFRAME_FALLBACK_AFTER,
                      (g_failReason != NULL) ? g_failReason : "LEN");
      }
#endif
      // 【批次 3.5】LED 状态脉冲：成功轮短亮 / 失败轮长亮（现场免工具判活；WiFi 断开时由 ledTick 慢闪接管）
      ledPulse(g_roundFailed ? LED_POLL_FAIL_MS : LED_POLL_OK_MS);
      g_nextPollMs += g_pollIntervalMs;    // P9：整轮（三段全结束）后才推进节拍（批次 3：周期为运行时变量，NVS poll_s 可覆盖）
      // 已落后一个周期以上（例如刚重连）就重新对齐，避免补发风暴
      if ((int32_t)(millis() - g_nextPollMs) >= 0) g_nextPollMs = millis() + g_pollIntervalMs;
#if !RS485_MODE_LOOPBACK_SELFTEST
      g_roundFailed = false;               // 本轮结果已上报，复位
      g_failSeg     = 0xFF;
      g_failReason  = NULL;
#endif
      g_state = ST_IDLE;
      break;
  }
}

// ---------------------------------------------------------------
// 批次 3：配置层（NVS 装载 + 常驻 SoftAP + Web 路由 + /status）
//   放在此处 include：上方已定义全部运行时变量（g_devStr / g_readMode / g_timeSynced /
//   g_udpTargetIp / g_ntpPrimary / g_pollIntervalMs ...）与 kNtpCfg / ntpHasReply 等工具函数，
//   web_config.h 直接使用它们；编译期默认仍来自 config.h 与 secrets.h（NVS 无值时回落）。
//   485 采集状态机与 UDP JSON 上行一行不改：本层只包在外面。
// ---------------------------------------------------------------
#include "web_config.h"

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);                    // USB CDC（Tools -> USB CDC On Boot = Enabled）
  // 【批次1 bugfix · 2026-10-07 砚青】无主机读取（真机跑=拔 USB）时，HWCDC::write 默认
  //   tx_timeout_ms=100ms 阻塞；每轮热路径 ~190 次 write（3 段超时/失败日志 + UDP 两行）
  //   ⇒ 轮询被拖慢到 ~19s/轮（开串口有主机读时不阻塞，1s/轮）。设为 0 = 无主机时立即返回
  //   （日志静默丢弃），轮询恢复 1s 节拍。⚠️ 待 Codex/用户独立复核。
  Serial.setTxTimeoutMs(0);
  // USB CDC 需要主机枚举完成后才有输出；此前打的日志会丢。
  // 仅在 setup 内有界等待（最多 2s），不影响 loop() 的非阻塞约束。
  for (uint32_t t0 = millis(); !Serial && (uint32_t)(millis() - t0) < 2000; ) delay(10);

  // ---- WiFi：静态 IP + 最小自恢复 ----
  WiFi.mode(WIFI_AP_STA);                   // 批次 3：STA + 常驻 SoftAP（AP 常开，见 web_config.h 已记录的口子①）

  // 批次 1：设备命名 —— dev = "ESP485-" + MAC 后 6 位十六进制（大写）
  //   例 MAC 48:F6:EE:15:9B:88 -> dev ESP485-159B88；mac 字段保留完整写法
  if (refreshDevId()) {
    Serial.printf("[DEV] dev=%s mac=%s（批次 1：JSON 上行标识）\n", g_devStr, g_macStr);
  } else {
    Serial.println("[DEV][WARN] MAC 读取为全 0（WiFi 栈未就绪），将在首次上行前重试");
  }

  // 批次 3：从 NVS 装载运行时配置（无值回落编译期默认）。此刻 WiFi 栈已就绪，macAddress 才有效。
  loadConfig();
  kNtpCfg[0] = g_ntpPrimary;                // NTP 主源改为运行时变量（备源 NTP_SERVER_2/3 不变）

  WiFi.persistent(true);                   // 凭据持久化到 NVS，掉电后可直接 reconnect
  WiFi.setAutoReconnect(true);             // 协议栈自带重连作为第一道防线
  WiFi.setSleep(false);                    // 关闭 modem sleep，避免延迟抖动放大（任务书 §9.1 M6 前置）

  WiFi.config(ipFromString(LOCAL_IP), ipFromString(GATEWAY),
              ipFromString(SUBNET),   ipFromString(DNS1));
  Serial.printf("[WIFI] 静态配置 IP=%s GW=%s MASK=%s DNS=%s\n",
                LOCAL_IP, GATEWAY, SUBNET, DNS1);

  WiFi.begin(g_wifiSsid, g_wifiOpen ? (const char *)nullptr : g_wifiPass);
                                           // 批次 3：凭据来自 NVS（无值回落 secrets.h）；非阻塞，进度由 wifiTick() 观察
  Serial.println("[WIFI] 已发起连接（非阻塞），等待 wifiTick() 报告结果");

  // 批次 3：常驻 SoftAP + Web 配置页 + /status（必须在 WiFi STA 配置之后调用）
  webSetup();

  // ---- NTP：此处【不】调用 configTzTime ----
  // 根因修复：SNTP 依赖已就绪的网络栈。若在 WiFi 连上之前初始化，SNTP 拿不到网络；
  // 现改为由 wifiTick() 在"首次连上 / 重连成功"时置位 g_ntpResyncPending，
  // 再由 ntpTick() 调用 configTzTime（三源：主 校内 chrony -> 备1 公网域名 -> 备2 公网 IP 直连）。
  Serial.printf("[NTP] 三源配置 TZ=%s 主=%s 备1=%s 备2=%s（等 WiFi 连上后由 ntpTick() 触发，setup 不调用 configTzTime）\n",
                NTP_TZ_INFO, g_ntpPrimary, NTP_SERVER_2, NTP_SERVER_3);

  // ---- UDP：本地端口打开，目标由运行时配置（NVS，无值回落 config.h 的 UDP_TARGET_IP/PORT）指定 ----
  g_udp.begin(UDP_LOCAL_PORT);
  Serial.printf("[UDP] 本地端口 %u，目标 %s:%u\n",
                (unsigned)UDP_LOCAL_PORT, g_udpTargetIp, (unsigned)g_udpTargetPort);

  // ---- UART1（RS-485 侧）：9600 8N1，begin 形参顺序为 (baud, config, RX, TX) ----
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial1.setTimeout(0);                   // read() 绝不阻塞；超时判定全部交给 millis()

  // 【批次 3.5】板载蓝灯：上电初始灭（脉冲/慢闪均由 ledTick 接管）
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW);
  Serial.printf("[LED] 状态灯就绪 GPIO%d（%s 电平点亮）：WiFi 断=慢闪 / 采集成功=短闪 / 失败=长闪\n",
                (int)LED_PIN, LED_ACTIVE_LOW ? "低" : "高");
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
  Serial.println("[485] 模式=真实 485 总线：ESP32(TX) -> MAX13487 -> QI 真机 STB3-125（Modbus RTU 从站地址 1）");
  Serial.println("[485] ⚠️ RXO 先万用表实测空闲电平：5V 必须 1k/2k 分压后接 GPIO20，3.3V 则直连不分压（未实测前按 5V 处理）；MAX13487 的 EN 接固定高电平、不得动态切换；三方必须共地");
  Serial.printf("[485] 方向控制：%s\n", (RS485_DE_PIN >= 0)
                ? "EN 由 GPIO 动态切换（MAX485 / MAX3485 接法，RS485_DE_PIN 已配置）"
                : "无（EN 固定电平，MAX13487 / MAX13488 接法；RS485_DE_PIN = -1）");
  {
    char b1[3 * QI_SEG_FRAME_LEN + 1], b2[3 * QI_SEG_FRAME_LEN + 1], b3[3 * QI_SEG_FRAME_LEN + 1];
    char bf[3 * QI_SEG_FRAME_LEN + 1];
    bytesToHex(QI_SEG1_FRAME_BYTES, QI_SEG_FRAME_LEN, b1, sizeof(b1));
    bytesToHex(QI_SEG2_FRAME_BYTES, QI_SEG_FRAME_LEN, b2, sizeof(b2));
    bytesToHex(QI_SEG3_FRAME_BYTES, QI_SEG_FRAME_LEN, b3, sizeof(b3));
    bytesToHex(QI_FULL_FRAME_BYTES, QI_SEG_FRAME_LEN, bf, sizeof(bf));
    Serial.printf("[485] M5 模式=运行时自适应（批次 2）从站地址 %u 功能码 0x%02X 读输入寄存器：\n",
                  (unsigned)RS485_SLAVE_ADDR, (unsigned)MODBUS_FUNC_READ_INPUT);
    Serial.printf("[485]   启动默认一帧读全：请求 %s  期望响应 %u 字节（39 个寄存器，E-02 A 分支）\n",
                  bf, (unsigned)QI_FULL_RESP_LEN);
    Serial.printf("[485]   从未成功且连续 %u 轮失败自动降级为 3 段（E-02 B 分支，本会话不回切）：\n",
                  (unsigned)QI_FULLFRAME_FALLBACK_AFTER);
    Serial.printf("[485]     S1 寄存器 0->15  请求 %s  期望响应 %u 字节\n", b1, (unsigned)QI_SEG1_RESP_LEN);
    Serial.printf("[485]     S2 寄存器 16->31 请求 %s  期望响应 %u 字节\n", b2, (unsigned)QI_SEG2_RESP_LEN);
    Serial.printf("[485]     S3 寄存器 32->38 请求 %s  期望响应 %u 字节\n", b3, (unsigned)QI_SEG3_RESP_LEN);
    Serial.printf("[485]   3 段一轮段间 >= %lu ms 静默，整轮 %lu ms；合并 %u 字节\n",
                  (unsigned long)RS485_SEG_GAP_MS, (unsigned long)g_pollIntervalMs, (unsigned)QI_MERGED_LEN);
  }
#endif

  g_nextPollMs = millis() + g_pollIntervalMs;
  Serial.println("[BOOT] M5+批次1+批次2+批次3 就绪：WiFi + NTP + UDP（JSON 上行：每轮 1 行心跳含 mode；全成功再加 1 行数据，含 raw 78 字节 HEX）+ 485 运行时自适应（默认一帧读全，连续失败 3 轮自动降级 3 段）+ 常驻 AP/Web 配置页//status/NVS（保存立即生效）");
}

void loop() {
  wifiTick();     // WiFi 状态检查与节流重连
  ntpTick();      // 对时状态检测 / 重连后重新对时
  stateTick();    // IDLE -> TX -> WAIT_RX -> PARSE -> SEND
  webTick();      // 批次 3：Web 配置页（非阻塞 handleClient + /reset 延迟重启），不影响 1s 轮询节拍
  ledTick();      // 批次 3.5：LED 状态指示（非阻塞毫秒翻转），不影响 1s 轮询节拍
}
