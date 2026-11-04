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
 *   - ✅ 实物已定稿：MAX13487 的 RXO 为 3.3V 逻辑，直连 GPIO20，不分压
 *   - ⚠️ EN 接法随实物主芯片型号而定（接线前先读 8 脚芯片丝印）：
 *     MAX13487 / MAX13488（5V 双向，芯片无 DE 脚）-> EN 接固定高电平（实物定稿为 3.3V，不是 5V），
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

// ---- 商用级加固 2026-10-27 砚青（对应 Codex 复核报告的固件四条）----
//   编号沿用 Codex 原报告：P0-1 时钟回退钳位 / P0-2 心跳诚实化 / P0-3 看门狗+复位原因 / P0-4 版本自报
//   ⚠️ 与 02_docs/29 的 P0 序号不同（29 号文里看门狗写作 P0-3、钳位写作 P0-1，此处已对齐 Codex 原文）
#include <esp_task_wdt.h>     // P0-3：esp_task_wdt_init/add/reset（**IDF 5.5 只有新签名**，见 config.h FW_WDT_*）
#include <esp_system.h>       // P0-3：esp_reset_reason() / esp_reset_reason_t
#include <esp_attr.h>         // P0-3：RTC_NOINIT_ATTR（boot 计数放 RTC 保留区，软复位不清零）
#include <sys/time.h>         // P0-1：settimeofday()（时钟回拉）

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

// ---------------------------------------------------------------
// 【商用级加固 2026-10-27 · P0-3】复位原因与 boot 计数
//   目的：让心跳能区分"人为拔电 / 看门狗 / panic / 软复位 / 欠压"。
//   现状（批次 3 及之前）**没有任何手段** —— 2026-10-26 那次 21:41:38 复位
//   只能靠人工回忆"换过 USB 供电线"才定性，否则就是一条无从追查的玄学复位。
//   ⚠️ RTC_NOINIT_ATTR 区**上电时是随机值**（不掉电则保留），必须先校验 magic，
//      否则首次上电会把随机数当成上次计数（可能读到 40 亿，报告直接失真）。
// ---------------------------------------------------------------
//   ⚠️ volatile：bootInfoInit() 里 count 与 magic 的**写入顺序**是正确性的一部分（见该函数），
//      若不加 volatile，编译器可以按任意顺序重排这两个独立存储，「magic 最后写」的保证就失效。
RTC_NOINIT_ATTR static volatile uint32_t g_bootMagic;   // = FW_BOOT_CNT_MAGIC 才认为后面的计数有效
RTC_NOINIT_ATTR static volatile uint32_t g_bootCount;   // 本次上电周期内的第几次启动（1 起）
static const char *g_resetReasonStr = "UNKNOWN";   // esp_reset_reason() 的可读映射
static uint32_t    g_bootCountNow   = 0;           // 本次启动序号（上心跳 boot 字段）
static uint32_t    g_txFails        = 0;           // P0-2：udpSendLine() 真失败次数（上心跳 tx_fail）

// ---------------------------------------------------------------
// 【商用级加固 2026-10-27 · P0-1】时钟护栏状态
// ---------------------------------------------------------------
static time_t   g_clkLastGood     = 0;      // 上次被接受的墙上时间（判断"回退"的基准）
static uint32_t g_clkHoldCount    = 0;      // 被拦下的回退次数（上心跳 clk_hold）
static int32_t  g_clkLastBackS    = 0;      // 最近一次被拦的回退秒数（上心跳 clk_back）
static bool     g_clkGuardPending = false;  // 已发起 SNTP，等回包后做一次钳位判定
static time_t   g_tsLastEmitted   = 0;      // 本会话已发出的最大 ts（epoch 秒）；ts 严格递增护栏用

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
static char     g_ntpPrimary[64]  = NTP_SERVER_1;     // NVS ntp1（NTP 主源，isPlausibleHost 上限 63 字符 + NUL）；备源 NTP_SERVER_2/3 不变
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
      // 【2026-10-27 第二审查者复核修复】原此处 g_reconnectCount = 0; 使心跳 reconn **恒为 0**：
      //   上行只在 g_linkUp==true 时发送，而清零恰好发生在 linkUp false->true 的同一轮，
      //   所以任何一条心跳读到的都是 0（02_docs/21 FR-11 要求计数器单调递增）。
      //   改为**本次启动内累计**，不再清零；串口「第 N 次重连」随之变为累计序号。
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
// 所用 API（已在本机 core 3.3.10 与 pio_verify core 3.3.11 中核实存在，非凭记忆）：
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

// ---------------------------------------------------------------
// 【商用级加固 2026-10-27 · P0-3】复位原因映射
//   枚举名取自本机 framework-arduinoespressif32-libs/esp32c3 的 esp_system.h（已逐个核对）。
//   ★ 对我方现场最有区分价值的两条：
//     BROWNOUT / PWR_GLITCH ⇒ 供电不稳（充电宝电压跌落），不是固件问题；
//     USB                   ⇒ USB 外设复位，插拔/更换 USB 线会落这里。
// ---------------------------------------------------------------
static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:    return "POWERON";     // 真实上电/断电重启（人为拔电最可能落这里）
    case ESP_RST_EXT:        return "EXT";         // 外部复位脚
    case ESP_RST_SW:         return "SW";          // esp_restart()：本固件 /reset 与保存后重启走这条
    case ESP_RST_PANIC:      return "PANIC";       // 异常 / Guru Meditation
    case ESP_RST_INT_WDT:    return "INT_WDT";     // 中断看门狗
    case ESP_RST_TASK_WDT:   return "TASK_WDT";    // 任务看门狗（P0-3 启用后主要自愈路径）
    case ESP_RST_WDT:        return "WDT";         // 其他看门狗
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";   // 深睡唤醒（本固件不用）
    case ESP_RST_BROWNOUT:   return "BROWNOUT";    // 欠压 ★充电宝电压跌落
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";         // ★USB 外设复位（插拔/换 USB 线）
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";  // ★电源毛刺
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "UNKNOWN";
  }
}

// 在 setup() 里 Serial 就绪后调用一次
static void bootInfoInit() {
  g_resetReasonStr = resetReasonStr(esp_reset_reason());

  if (g_bootMagic != FW_BOOT_CNT_MAGIC) {
    // RTC 保留区未被本固件写过 ⇒ 真·冷启动（断电 / 首次上电），计数从 1 重新开始。
    // 【2026-10-27 第二审查者复核修复】写入顺序必须是 **先 count、后 magic**（magic 作提交标记）：
    //   原顺序先写 magic 再写 count，若恰好在这两条 store 之间掉电，下次上电 magic 仍有效、
    //   count 却是随机值，随后 count++ 会把随机数（可能 40 亿）当成启动序号上报。
    //   改为 count 先写、magic 最后写：中途掉电 ⇒ magic 不匹配 ⇒ 下次仍按冷启动重建为 1。
    //   （两个变量均为 volatile，保证这两条 store 不被编译器重排。）
    g_bootCount = 1;
    g_bootMagic = FW_BOOT_CNT_MAGIC;
  } else if (g_bootCount < 0xFFFFFFFFUL) {
    // 用赋值而非 ++：C++20 起对 volatile 类型做 ++ 会触发 -Wvolatile 警告（本轮实测出现过）。
    g_bootCount = g_bootCount + 1;    // 软复位 / 看门狗：RTC 区仍在，计数累加
  }
  g_bootCountNow = g_bootCount;

  Serial.printf("[BOOT] reset_reason=%s boot_count=%lu（%s）\n",
                g_resetReasonStr, (unsigned long)g_bootCountNow,
                (g_bootCountNow <= 1)
                  ? "RTC 保留区已重建 => 真断电/首次上电"
                  : "RTC 保留区仍在 => 软复位或看门狗，未断电");
}

// ---------------------------------------------------------------
// 【商用级加固 2026-10-27 · P0-3】任务看门狗（TWDT）
//   ⚠️ IDF 5.5 只有 esp_task_wdt_init(const esp_task_wdt_config_t *)；
//      旧写法 esp_task_wdt_init(uint32_t, bool) 在本 framework 的头文件里**没有声明**（已 grep 核实）。
//   ⚠️ idle_core_mask = 0：**不**订阅 idle 任务。WiFi/SNTP 内部偶发长阻塞不该触发整机重启，
//      要盯的是 loop() 自己卡死。
// ---------------------------------------------------------------
static bool g_wdtOn = false;

static void wdtInit() {
#if FW_WDT_ENABLE
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = FW_WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;            // 超时 => panic => 重启，下次启动留下 TASK_WDT

  esp_err_t e = esp_task_wdt_init(&cfg);
  if (e == ESP_ERR_INVALID_STATE) {
    // 已被 core / SDK 初始化过 ⇒ 改用 reconfigure 覆盖超时
    e = esp_task_wdt_reconfigure(&cfg);
  }
  if (e != ESP_OK) {
    Serial.printf("[WDT][WARN] 初始化失败 err=%d，看门狗未启用（不影响采集）\n", (int)e);
    return;
  }

  e = esp_task_wdt_add(NULL);           // NULL = 当前任务（loopTask）
  if (e == ESP_ERR_INVALID_ARG) e = ESP_OK;   // 已订阅过（重复 add）⇒ 视为成功
  if (e != ESP_OK) {
    Serial.printf("[WDT][WARN] 订阅 loopTask 失败 err=%d，看门狗未生效\n", (int)e);
    return;
  }

  g_wdtOn = true;
  Serial.printf("[WDT] 任务看门狗已启用：%lu ms，超时=>panic 重启（reset_reason 将记 TASK_WDT）\n",
                (unsigned long)FW_WDT_TIMEOUT_MS);
#endif
}

// 每轮 loop() 开头喂一次。正常轮次耗时 <1s，30s 窗口有两个数量级余量。
static inline void wdtFeed() {
#if FW_WDT_ENABLE
  if (g_wdtOn) esp_task_wdt_reset();
#endif
}

// ---------------------------------------------------------------
// 【商用级加固 2026-10-27 · P0-1】时钟回退钳位（拆 CLOCK_STEP 死结）
//   完整背景、以及"为什么源头+兜底双保险缺一不可"见 config.h 的 FW_CLK_* 注释段。
// ---------------------------------------------------------------
static void ntpApplyWithGuard(const char *why) {
  configTzTime(NTP_TZ_INFO, g_ntpPrimary, NTP_SERVER_2, NTP_SERVER_3);
#if FW_CLK_GUARD_ENABLE
  g_clkGuardPending = true;   // SNTP 异步：真正的判定放到下一次 ntpClampTick()
#endif
  (void)why;
}

// 每轮 loop() 都调用 —— **刻意不放在 ntpTick() 的 200ms 节流之后**，
// 目的是把"SNTP 已改时钟 → 我们拉回"之间的漏网窗口压到单次 loop 迭代（微秒级），
// 否则回包后的头 200ms 内 stateTick() 可能已经用倒退的 ts 发出一行。
static void ntpClampTick() {
#if FW_CLK_GUARD_ENABLE
  if (!g_clkGuardPending) return;
  if (!ntpHasReply()) return;             // 还没有 NTP 回包：此时时钟不可信，不能当基准

  const time_t now = time(nullptr);
  if (now < (time_t)1600000000) return;   // 时钟未就绪（早于 2020-09），不参与判定

  g_clkGuardPending = false;

  if (g_clkLastGood == 0) {               // 首次拿到可信时间：直接接受并作为基线
    g_clkLastGood = now;
    return;
  }
  if (now < g_clkLastGood) {
    const int32_t back = (int32_t)(g_clkLastGood - now);
    if (back <= FW_CLK_BACK_HOLD_S) {
      // 小幅回退 ⇒ 拉回旧值：既保住已发出的 ts 不倒退，也不让偏差单向累积
      // 【2026-10-27 第二审查者复核修复】计数与最近值只统计**真的被拦下**的回退：
      //   原实现在阈值判断之前就自增，导致「大幅回退被接受」也计入 clk_hold，
      //   而 02_docs/24 Y-4 的交叉证据判据是「clk_hold>0 且 CLOCK_STEP=0 ⇒ 钳位生效」，
      //   混入被放行的事件会让该判据**假阳性**（看起来拦住了、其实放行了）。
      g_clkHoldCount++;
      g_clkLastBackS = back;
      struct timeval tv;
      tv.tv_sec  = g_clkLastGood;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      Serial.printf("[CLK] 拦下 NTP 回退 %lds（保持 %ld），累计 %lu 次\n",
                    (long)back, (long)g_clkLastGood, (unsigned long)g_clkHoldCount);
      return;                             // ⚠️ g_clkLastGood 保持不变
    }
    // 大幅回退 ⇒ 视为时钟真错（例如 RTC 被彻底打乱），接受并重建基线，避免永久卡在旧时间。
    //   ⚠️ 本分支**不计入 clk_hold**（语义见上）：当前版本对「接受了大回退」没有心跳证据，
    //      只有串口日志（真机拔 USB 时看不到）。要让它可观测需新增心跳字段（B 类，等拍板）。
    Serial.printf("[CLK][WARN] 回退 %lds 超过阈值 %ds，接受（疑似时钟真错）\n",
                  (long)back, (int)FW_CLK_BACK_HOLD_S);
  }
  g_clkLastGood = now;
#endif
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
#if FW_TS_MONOTONIC_ENABLE
    // 【P0-1 ②】ts 严格递增护栏：同一会话内 ts 绝不倒退。
    //   ⚠️ 只在"会倒退"时用 last+1 跳过；**同秒保持原样、不额外 +1** ——
    //      同秒是"1 Hz + 秒级精度"的分辨率伪影（接收端记 TS_SAME_SECOND/INFO，8h 实测 6 次），
    //      若同秒也 +1，一旦轮询周期略快于 1 s 就会每轮 +1 并**持续累积超前**。
    //      ⇒ 只在 cur < last（真倒退）时补救，其余路径与原实现逐位一致。
    const time_t cur = time(nullptr);
    if (cur >= (time_t)1600000000) {              // 时钟有效（晚于 2020-09）才参与护栏
      time_t emit = cur;
      if (g_tsLastEmitted != 0 && cur < g_tsLastEmitted) {
        emit = g_tsLastEmitted + 1;               // 跳过已被占用的秒
        Serial.printf("[CLK] ts 护栏：时钟回退，%ld -> %ld（跳过 1s，防 CLOCK_STEP）\n",
                      (long)cur, (long)emit);
      }
      struct tm fixed;
      time_t t = emit;
      localtime_r(&t, &fixed);
      strftime(out, outSize, "%Y-%m-%dT%H:%M:%S", &fixed);
      g_tsLastEmitted = emit;
      return true;
    }
#endif
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
    ntpApplyWithGuard("首次/重连后对时");   // P0-1：统一走护栏入口（内部仍是 configTzTime）
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
    ntpApplyWithGuard("未同步重试");        // P0-1：统一走护栏入口（内部仍是 configTzTime）
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
//   raw/10、raw/1000、raw-40、raw/100、raw*0.01、1:1、高字、(H*65536+L)/1000
//   ⚠️ 电量公式已于 2026-10-27 定案（D-10，见 02_docs/31）：旧口径 `(H*65536)/1000+L` 作废。
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
    // 【2026-10-27 砚青 · D-10 定案，见 02_docs/31_电量公式勘误】旧式 `(H*65536)/1000 + L` **错误**：
    //   ① 把低字 L 当"度"直接相加 ⇒ 整体放大 1000 倍（L 每 +1 错式 +1.000 度，实际只 +0.001 度）；
    //   ② 量纲不一致：同一 32 位累加器的高字贡献 65.536 度/步、低字贡献 1 度/步，差 1000 倍；
    //   ③ L 由 65535 -> 0（H+1，**正常进位**）时错式暴跌 65469.464 度 ⇒ 打穿"电量单调性"判据。
    //   正确式 = 把高低字当成**一个 32 位累加器**整体除 1000：raw 单位 = Wh ⇒ 结果单位 = 度(kWh)。
    pEnergy[i] = ((double)rH * 65536.0 + (double)rL) / 1000.0;   // 度（raw 单位 Wh）

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
  const double   energy = ((double)rH * 65536.0 + (double)rL) / 1000.0;   // 度（D-10 定案，见 02_docs/31）

  char rawHex[QI_RAW_HEX_MAX];
  bytesToHex(g_merged, QI_MERGED_LEN, rawHex, sizeof(rawHex));

  // 【2026-10-21 加固】原来这里写 `WiFi.localIP().toString().c_str()`：
  //   每轮每条数据行都会构造/析构一个 String（实测堆锯齿 356/712 B）。
  //   168h 长跑最怕的就是这种"每秒一次"的堆抖动把碎片攒起来。改成栈上定长缓冲，
  //   JSON 里的 ip 字符串逐字节不变（格式仍是 a.b.c.d）。
  char ipBuf[16];
  {
    const IPAddress ip = WiFi.localIP();
    snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u",
             (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
  }

  size_t pos   = 0;
  bool   trunc = false;
  ++g_seq;                                    // 每发一行自增 1（uint32_t 回绕正常）

  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "{\"seq\":%lu,\"dev\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\",\"ts\":\"%s\",\"ts_ok\":%s,",
          (unsigned long)g_seq, g_devStr, g_macStr,
          ipBuf, ts, tsOk ? "true" : "false");

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
//   原字段：seq / dev / ts / ts_ok / type / rssi / free_heap / mode / poll_ok / poll_err / udp_tx
//   与 02_docs/17 3.2 老师原串的差异：去掉 cfg 字段、新增 dev（本批命名要求）。
//
// 【商用级加固 2026-10-27 · P0-2 心跳诚实化 + P0-4 版本自报】
//   ⚠️ queue / dropped 由 `0` 改为 `null`：本固件**没有发送队列**（UDP 走同步 sendto，
//      不存在排队缓冲），此前恒发 0 属于"看起来有能力、实际不存在"的假数据。
//      改为 null 后语义 = "本设备无此能力"，与"队列当前为空（=0）"严格区分。
//      接收端不受影响：udp_collect.py 的 HB_REQUIRED 不含这两个键，且只对
//      poll_ok / poll_err / udp_tx 做 isinstance(int) 判定（已核对 line 120、line 1060）。
//   新增真实计数：tx_fail（udpSendLine 真失败）/ reconn（WiFi 重连次数）/ up（开机秒数）
//   新增诊断字段：fw（P0-4 版本自报）/ reset + boot（P0-3 复位原因与启动序号）
//                 clk_hold + clk_back（P0-1 被拦下的时钟回退次数与最近秒数）
// ---------------------------------------------------------------
static bool buildHeartbeatJson(const char *ts, bool tsOk) {
  size_t pos   = 0;
  bool   trunc = false;
  ++g_seq;                                    // 每发一行自增 1（uint32_t 回绕正常）
  appendf(g_payload, sizeof(g_payload), &pos, &trunc,
          "{\"seq\":%lu,\"dev\":\"%s\",\"fw\":\"%s\",\"ts\":\"%s\",\"ts_ok\":%s,\"type\":\"hb\","
          "\"rssi\":%d,\"free_heap\":%u,\"mode\":\"%s\",\"poll_ok\":%lu,\"poll_err\":%lu,\"udp_tx\":%lu,"
          "\"queue\":null,\"dropped\":null,"
          "\"tx_fail\":%lu,\"reconn\":%lu,\"up\":%lu,"
          "\"reset\":\"%s\",\"boot\":%lu,\"clk_hold\":%lu,\"clk_back\":%ld}",
          (unsigned long)g_seq, g_devStr, FW_VERSION, ts, tsOk ? "true" : "false",
          (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
          (g_readMode == 0) ? "full" : "3seg",
          (unsigned long)g_pollOk, (unsigned long)g_pollErr, (unsigned long)g_udpTx,
          (unsigned long)g_txFails,
          (unsigned long)g_reconnectCount,
          (unsigned long)(millis() / 1000UL),
          g_resetReasonStr,
          (unsigned long)g_bootCountNow,
          (unsigned long)g_clkHoldCount,
          (long)g_clkLastBackS);
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

// 把 g_payload 中已组装好的一行发出去；beginPacket / endPacket 均成功则累加 udp_tx
static bool udpSendLine() {
  const size_t want = strlen(g_payload);
  bool ok = (g_udp.beginPacket(g_udpTargetIp, g_udpTargetPort) == 1);   // 不判返回值会让日志说谎：发失败仍打印"已发"
  if (ok) {
    // 【2026-10-21 Codex 复核修正】NetworkUDP::write() 的返回值是"已写入本地 tx_buffer
    //   的字节数"，不是空口送达；Arduino-ESP32 3.x（本机 3.3.10 / pio_verify 3.3.11，
    //   两版实现相同）的 write(buf,size) 逐字节调用且忽略单字节
    //   返回值，最后 `return i`（= 传入 size），因此 wrote == want 恒成立，不能作为成功判据。
    //   成功判据只保留 endPacket()==1（= sendto() 已被协议栈受理，仍不代表空口送达）。
    //   write() 与 endPacket() 必须无条件调用，否则 pbuf 不释放。
    //   ⚠️ 这里**绝不重传**：重传会产生重复 seq，直接污染重包率(M8)。
    const size_t wrote = g_udp.write((const uint8_t *)g_payload, want);
    ok = (g_udp.endPacket() == 1);
    if (wrote != want) {
      Serial.printf("[UDP][WARN] 本地缓冲短写：want=%u wrote=%u（仅诊断，不代表空口送达）\n",
                    (unsigned)want, (unsigned)wrote);
    }
  }
  if (ok) {
    g_udpTx++;
  } else {
    g_txFails++;     // 【P0-2】真实发送失败计数 → 心跳 tx_fail（与 udp_tx 一起构成"发了多少/失了多少"）
    Serial.printf("[UDP][ERR] 发送失败（beginPacket/endPacket 未返回 1），目标 %s:%u\n",
                  g_udpTargetIp, (unsigned)g_udpTargetPort);
  }
#if SERIAL_VERBOSE_ROUND
  Serial.printf("[UDP -> %s:%u] %s\n", g_udpTargetIp, (unsigned)g_udpTargetPort, g_payload);
#endif
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
      // 【2026-10-21 Codex】g_payload 此时是不完整前缀，不能发；按"本行不发送"处理。
      Serial.printf("[UDP][ERR] payload buffer too small: need %d / cap %u，本行不发送\n",
                    n, (unsigned)sizeof(g_payload));
    } else {
      udpSendLine();
    }
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
    // 【2026-10-21 Codex】组装失败时 g_payload 是不完整 JSON，发出去只会被接收端记 bad_json，
    //   还会被误算成网络丢失。本行不发送（seq 已在 build 内占用、udp_tx 不增，便于对账）。
    Serial.printf("[UDP][ERR] payload 缓冲不足：心跳行装不下（容量 %u），本行不发送\n",
                  (unsigned)sizeof(g_payload));
  } else {
    udpSendLine();
  }

#if !RS485_MODE_LOOPBACK_SELFTEST
#if QI_SEND_ERR_ROW
  // 2) 失败行：失败轮在心跳行之外再发一行（§8.2 ③；拔 USB 后唯一的失败可见通道）
  if (g_roundFailed) {
    if (!buildErrJson(ts, tsOk)) {
      Serial.printf("[UDP][ERR] payload 缓冲不足：err 行装不下（容量 %u），本行不发送\n",
                    (unsigned)sizeof(g_payload));
    } else {
      udpSendLine();
    }
  }
#endif

  // 3) 数据行：三段全成功时再发一行（含 raw 78 字节 HEX）
  if (!g_roundFailed) {
    if (!buildDataJson(ts, tsOk)) {
      Serial.printf("[UDP][ERR] payload 缓冲不足：数据行装不下（容量 %u），本行不发送\n",
                    (unsigned)sizeof(g_payload));
    } else {
      udpSendLine();
    }
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
// 顺序：超时/半帧 -> 异常响应 -> 地址 -> 功能码 -> byte count 帧长(自洽 + 与请求一致) -> CRC -> 拼接
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
  // 【2026-10-21 Codex】正常响应必须与**本段请求**的功能码一致（全帧/3 段/回环均为 reqFunc）；
  //   旧写法同时接受 0x03/0x04，从站若回错功能码仍会被当成功。判定顺序不变。
  if (func != reqFunc) {
    g_statFail++; recordSegFail("FUNC");
    Serial.printf("[M4] 功能码不匹配 期望 0x%02X 收到 0x%02X SEG=%s REQ=0x%02X\n",
                  (unsigned)reqFunc, (unsigned)func, tag, (unsigned)reqFunc);
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

  // (5b) 【P0-1 · 2026-10-08】帧长还必须与"本段请求的寄存器数"一致。
  //   为什么 (5) 不够：(5) 比的是"从站自报的 byte count 与实收长度"——这对**任何**
  //   自洽的帧都成立（且 ST_WAIT_RX 的帧长驱动正是按同一个 byte count 提前收尾的），
  //   所以它证明不了"从站真的回满了本段请求的寄存器数"。
  //   缺这一条时的失效路径（静默掺假）：从站回短帧（例：一帧读全要 39 个、只回 30 个）
  //   -> CRC 过、地址过、功能码过、(5) 也过 -> 被判为成功 -> (7) 只 memcpy 了前 60 字节
  //   -> g_merged[60..77] 保留**上一轮**的值（g_merged 只在 (7) 按段偏移覆写、从不整轮清零）
  //   -> 数据行的 reg30..reg38 是本轮值、reg39 之后是上一轮值，且 quality 可能仍是 OK。
  //   口径（§4「错误的数据不要」）：短帧 = 本段失败 -> 该轮改发 err 行，不发数据行。
  //   期望值取 qiActiveRespLens()[g_segIndex] - 5（地址1 + 功能1 + byte count1 + CRC2），
  //   对 83/37/37/19 自动成立，不新写死常量。
  //   ⚠️ 本判据只覆盖"正常响应"；从站异常响应（5 字节）已在 (2) 提前 return，不走这里。
  if ((uint16_t)(byteCount + 5U) != (uint16_t)qiActiveRespLens()[g_segIndex]) {
    g_statFail++; recordSegFail("LEN");
    Serial.printf("[M4] 帧长与请求不符 期望 %u 收到 %u（byte count=%u）SEG=%s REQ=0x%02X\n",
                  (unsigned)qiActiveRespLens()[g_segIndex], (unsigned)g_rxLen,
                  (unsigned)byteCount, tag, (unsigned)reqFunc);
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
#if SERIAL_VERBOSE_ROUND
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
#endif  // SERIAL_VERBOSE_ROUND
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
      // 【2026-10-21 Codex】有界排空：正常一帧 83 B 远小于预算；持续噪声/驱动异常时
      //   也不会在热路径里无限读（预算 4×96+8 = 392 ≥ UART 环形缓冲 256 B + FIFO 128 B，
      //   一次 tick 足以排空一整轮缓冲）。
      {
        uint16_t budget = 4U * MODBUS_RX_BUF_MAX + 8U;
        while (budget > 0 && Serial1.available()) { (void)Serial1.read(); --budget; }
      }

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
      // 说明：本 core（3.x；本机 3.3.10 / pio_verify 3.3.11）的 flush() = uartFlushTxOnly(uart, true)，会一直等到 TX 完全空闲
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

      // 【2026-10-21 Codex】有界收字节：每次 stateTick 最多取固定预算，剩余字节留给
      //   下一次 tick。正常 83 B 帧一次取完；持续噪声时不会长时间占住主循环。
      {
        uint16_t budget = 4U * MODBUS_RX_BUF_MAX + 8U;
        while (budget > 0 && Serial1.available()) {          // read() 已设 timeout=0，不阻塞
          uint8_t b = (uint8_t)Serial1.read();
          if (g_rxLen < MODBUS_RX_BUF_MAX) g_rxFrame[g_rxLen++] = b;
          else                             g_rxOverflow = true;   // 超长帧：只标记，不再写缓冲
          g_tRxLastByteMs = millis();
          --budget;
        }
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
      // 【P0-3 · 2026-10-08】本行必须留在守卫内：g_roundFailed 声明在
      //   #if !RS485_MODE_LOOPBACK_SELFTEST 之内（见上方批次 2 的读取模式状态块），
      //   若落在守卫外，RS485_MODE_LOOPBACK_SELFTEST=1 的串口层回归分支会编译失败
      //   （'g_roundFailed' was not declared in this scope，2026-10-08 用户复核确认，
      //    该分支自批次 3.5 起一直编不过）。回环模式无"段/轮"语义 => 恒视为成功轮、恒短亮。
#if !RS485_MODE_LOOPBACK_SELFTEST
      ledPulse(g_roundFailed ? LED_POLL_FAIL_MS : LED_POLL_OK_MS);
#else
      ledPulse(LED_POLL_OK_MS);            // 回环自测：恒短亮（顺带消掉"ledPulse 定义未使用"警告）
#endif
      g_nextPollMs += g_pollIntervalMs;    // P9：整轮（三段全结束）后才推进节拍（批次 3：周期为运行时变量，NVS poll_s 可覆盖）
      // 已落后一个周期以上（例如刚重连）就重新对齐，避免补发风暴
      // 【2026-10-21 加固】这条分支原来**完全静默**：168h 对账时会出现
      //   "墙钟应有 N 轮、poll 只涨了 N-k 轮"的缺口，却查不出是设备跳轮还是网络丢包。
      //   只加日志，不加 JSON 字段（改接口属于要复核的决策）。
      if ((int32_t)(millis() - g_nextPollMs) >= 0) {
        Serial.printf("[POLL][SKIP] 落后 %ld ms，重对齐节拍（本轮之后有一轮被跳过）\n",
                      (long)(millis() - g_nextPollMs));
        g_nextPollMs = millis() + g_pollIntervalMs;
      }
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
  // 【批次1 bugfix · 2026-10-07 砚青；2026-10-21 Codex 更新计数】无主机读取（真机跑=拔 USB）时，
  //   HWCDC::write 默认 tx_timeout_ms=100ms 阻塞。热路径 write 次数随 SERIAL_VERBOSE_ROUND 变化：
  //   =1 时约 44 次/轮（一帧读全，含 39 行 REG + UDP 回显）或约 50 次/轮（3 段）；
  //   =0 时成功轮约 0 次（只剩失败/告警日志）。⇒ 去掉本行会拖慢到 ~19s/轮；
  //   设为 0 = 无主机时立即返回（日志静默丢弃），轮询恢复 1s 节拍。
  Serial.setTxTimeoutMs(0);
  // USB CDC 需要主机枚举完成后才有输出；此前打的日志会丢。
  // 仅在 setup 内有界等待（最多 2s），不影响 loop() 的非阻塞约束。
  for (uint32_t t0 = millis(); !Serial && (uint32_t)(millis() - t0) < 2000; ) delay(10);

  // 【商用级加固 2026-10-27 · P0-3】尽早记录复位原因与 boot 序号。
  //   必须在 Serial 就绪之后（否则这条日志会丢），且在任何可能触发重启的操作之前。
  bootInfoInit();

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
  Serial.println("[485] ⚠️ 实物已定稿：MAX13487 RXO（3.3V 逻辑）直连 GPIO20，不分压；EN 接固定高电平、不得动态切换；三方必须共地");
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

  // 【商用级加固 2026-10-27 · P0-3】看门狗放在 setup **最末尾**才启用：
  //   setup 期间（SoftAP / WebServer 初始化、WiFi.begin 首次拉起）耗时不确定，
  //   提前开会可能被误杀；一旦进入 loop() 每轮喂狗，30s 窗口足以覆盖任何正常轮次。
  wdtInit();

  Serial.println("[BOOT] M5+批次1+批次2+批次3 就绪：WiFi + NTP + UDP（JSON 上行：每轮 1 行心跳含 mode；全成功再加 1 行数据，含 raw 78 字节 HEX）+ 485 运行时自适应（默认一帧读全，连续失败 3 轮自动降级 3 段）+ 常驻 AP/Web 配置页//status/NVS（保存立即生效）");
}

void loop() {
  wdtFeed();      // 【P0-3】先喂狗：本轮耗时可忽略，保证 30s 窗口内必有一次喂狗
  ntpClampTick(); // 【P0-1】时钟回退钳位：**不受 ntpTick 的 200ms 节流限制**，尽量缩小漏网窗口
  wifiTick();     // WiFi 状态检查与节流重连
  ntpTick();      // 对时状态检测 / 重连后重新对时
  stateTick();    // IDLE -> TX -> WAIT_RX -> PARSE -> SEND
  webTick();      // 批次 3：Web 配置页（非阻塞 handleClient + /reset 延迟重启），不影响 1s 轮询节拍
  ledTick();      // 批次 3.5：LED 状态指示（非阻塞毫秒翻转），不影响 1s 轮询节拍
}
