#pragma once

/*
 * config.h —— 非敏感配置常量（ESP32-C3 RS-485 采集网关）
 *
 * 规则：
 *   - 本文件只放非敏感常量；WiFi 凭据一律放 secrets.h（见 secrets.h.example）。
 *   - 事实来源：04_ai_prompts/00_context.md
 *   - 值变化时同步 README 第 8 节与任务书对应条目。
 */

// ---------------------------------------------------------------
// 上行 UDP —— 目标为【电脑侧】网络调试助手（NetAssist）接收地址
// ⚠️ 192.168.1.113 是电脑侧 IP（会随网络漂移），192.168.1.200 是 ESP32 自身 IP，勿混用
// ---------------------------------------------------------------
#define UDP_TARGET_IP        "192.168.1.113"   // 电脑侧（每次开工前 ipconfig /all 确认，会随网络变化）
                                               // ⚠️ 漂移史：09-27 .117 → 10-04 .118 → 10-05 .114 → **10-05 晚 .113**
                                               //    本次(2026-10-05 20:26)由 WorkBuddy 依用户授权同步
                                               //    （电脑切回自购路由器 402FBS，实测 192.168.1.113）；
                                               //    待同步 README 第 8 节与任务书对应条目（属用户/总控职权）
#define UDP_TARGET_PORT      8888              // 电脑侧 NetAssist 监听端口
#define UDP_LOCAL_PORT       12345             // ESP32 本地 UDP 端口（M2 只发不收；便于抓包定位）

// ---------------------------------------------------------------
// 网络 —— ESP32 自身静态 IP（Q-02 主方案：学校 WiFi + WiFi.config 固定）
// ---------------------------------------------------------------
#define LOCAL_IP             "192.168.1.200"   // M1 前置：ping + arp -a 确认该地址空闲
#define GATEWAY              "192.168.1.1"
#define SUBNET               "255.255.255.0"
#define DNS1                 "192.168.1.1"

// ---------------------------------------------------------------
// 时间同步（NTP / SNTP）—— 三源，按序尝试（主 -> 备1 -> 备2）
//
// 事实来源：04_ai_prompts/00_context.md；实测见 06_tests/ntp_probe.py（2026-10-04）。
// 三个源报出的 UTC 时间完全一致 ⇒ 内网 chrony 时间正确且确实在开放 NTP 服务。
//
// ⚠️ 为什么必须保留公网源（不要"优化"掉）：
//   网络退路方案是手机 2.4GHz 热点（见 00_context.md 网络环境与裁定）。手机热点下
//   172.16.0.2 必然不可达 ⇒ 若把它当作唯一源，退路一启用就退回 BOOT+ / UNSYNCED，
//   时间戳全部失效。主源 + 公网兜底是硬要求。
// ---------------------------------------------------------------
#define NTP_SERVER_1         "172.16.0.2"        // 主：校内 chrony（导师 2026-10-02 提供）
                                                 //   实测 2026-10-04：stratum 2 / RTT 3.4ms
                                                 //   ⚠️ 172.16.0.0/12 私网段，仅校园网内可达
                                                 //      （手机热点下不可达 ⇒ 下面两个公网源必须保留作兜底）
#define NTP_SERVER_2         "ntp.aliyun.com"    // 备1：公网域名；实测 2026-10-04：stratum 2 / RTT 41.0ms（依赖 DNS）
#define NTP_SERVER_3         "203.107.6.88"      // 备2：公网 IP 直连；实测 2026-10-04：stratum 2 / RTT 40.8ms
                                                 //   （校园网不下发外部 DNS / DNS 不可用时仍能对时）
// ⚠️ 必须写 "CST-8"，不可写 "<CST-8>"：POSIX TZ 中尖括号用于引用分区"名称"，
//    写成 "<CST-8>" 时整个 "CST-8" 被当成名称、偏移量缺省为 0 → 等效 UTC（曾导致时间差 8 小时）。
#define NTP_TZ_INFO          "CST-8"             // 东八区 UTC+8（CST = UTC+8 分区，POSIX TZ 串）

// ---------------------------------------------------------------
// 轮询节奏与 WiFi 自恢复（全部非阻塞）
// ---------------------------------------------------------------
#define POLL_INTERVAL_MS     1000   // 默认轮询周期（NVS poll_s 可覆盖，1-5s；运行时变量 g_pollIntervalMs）；1 秒循环（millis() 非阻塞）
#define WIFI_CHECK_INTERVAL_MS  5000  // 每 5 秒检查一次 WiFi 状态
#define WIFI_RECONNECT_INTERVAL_MS 5000  // 断开后重连节流：≥5s 一次
#define NTP_RETRY_INTERVAL_MS      60000 // 未同步时重试 SNTP 的周期（任务书 §6.1.2：WiFi 已连上且仍未对时时每 60s 重试一次）

// ---------------------------------------------------------------
// RS-485 / UART1 —— M4：真实 485 总线收发 + Modbus RTU 解析已启用
//
// 超时三级设计（全部由 millis() 驱动，无 delay()）：
//   一级 首字节超时 -> 二级 字符间隔超时（判本帧结束）-> 三级 整帧兜底
// 另加【帧长驱动】：收满预期帧长立即结束等待，不等第二级/第三级超时。
// 9600 8N1 下 1 字节 ≈ 1.04ms、8 字节 ≈ 8.3ms，故下列阈值余量充足。
//
// ✅【已确认】实物 485 模块主芯片型号 = MAX13487（2026-10-05 丝印确认，见 00_context.md 五 待确认盲区闭环）：
//   曾为三合一款（MAX3485 / MAX485 / MAX13487）致型号未定；现确认按 MAX13487 的
//   AutoDirection（**无需方向控制脚**）实现，RS485_DE_PIN 保持 -1（EN 固定高电平，不得动态切换）。
//   若日后换成 MAX485 / MAX3485（单向），把下方 RS485_DE_PIN 改成实际 GPIO 号即可启用方向控制。
//   半双工下"自己发的字节会先进 RX 缓冲"，故 ST_TX 里清 RX 必须在 write() 之前（已实现）。
// ---------------------------------------------------------------
#define RS485_BAUD                  9600   // 已确认为最终值（9600，8N1，无校验，设备地址 1）
#define RS485_RX_PIN                20     // UART1 RX = GPIO20（经分压后接入）
                                           // ⚠️ R-13：M4 接线曾 5V 直入本脚（ESP32-C3 IO 绝对最大 3.6V），输入通路可能已受损。
                                           //    排障顺序：先用 RS485_MODE_LOOPBACK_SELFTEST=1 + 短接 GPIO21↔GPIO20 验证本脚；
                                           //    若回环异常，改用 GPIO3 / GPIO4 / GPIO5 / GPIO10 作 UART1 RX，并同步改本行与文档。
#define RS485_TX_PIN                21     // UART1 TX = GPIO21
#define RS485_TX_GUARD_MS           2      // 发送→接收的方向切换保护间隔（AutoDirection 换向余量）
                                           // 说明：Serial1.flush() 在本 core（3.3.10）会一直等到 TX 完全空闲
                                           //   （HardwareSerial::flush → uartFlush → uartFlushTxOnly(uart, true) → uart_ll_is_tx_idle：
                                           //    txfifo_cnt==0 且 TX 状态机回到 IDLE），所以 2ms 不用于补 flush 的不足，
                                           //   仅为换向留防御性余量（MAX13487 手册 tDDD + tRED ≤ 2.6µs；MAX485 变体切换 DE 同理）。
                                           // ⚠️ M4 真实总线上若稳定丢首字节，先把它调到 3~5ms 复测
#define RS485_FIRST_BYTE_TIMEOUT_MS 80     // 一级：等首个字节的时限
#define RS485_CHAR_TIMEOUT_MS       8      // M5 正式档位（4~8，取 8）。二级：字节间静默阈值（≈4 个字符时间）；
                                           //    t3.5 框尾静默对应 9600 下约 4ms，超过即判本帧结束（半帧）。
                                           //    历史：M4 期间为「电脑端从站经 USB 转 485 批量交付」临时放大到 25；
                                           //    M5 接 QI 真机（纯硬件 UART，无 USB 批量交付抖动）起收回正式值域 4~8。
                                           //    ⚠️ 若真机出现半帧（RX=n 期望 N）则进一步收紧（先记录、不要改代码）。
                                           //    本值只决定帧尾判定的宽松度，不影响 1s 轮询节奏（三级兜底仍为 300ms）。
#define RS485_FRAME_MAX_MS          300    // 三级：整帧兜底，防死等
// ---- 方向控制（EN / DE-RE#）：⚠️ 接法随实物主芯片型号，接线前必须先用放大镜读 8 脚芯片丝印 ----
//   ① MAX13487 / MAX13488（+5V，AutoDirection，芯片无 DE 脚）：EN 接固定高电平（模块 VCC/5V），
//      本项保持 -1 —— 固件不参与方向控制（不得动态切换，见 00_context.md 三.5 ①）。
//      模块丝印的 EN 可能引到芯片三种脚，处置在接固定高下一致，但诊断思路不同：
//        EN=RE#   -> 高 = 交给 AutoDirection 状态机（正常收发）；低 = 强制 RO 常开（会自发自收）
//        EN=SHDN# -> 高 = 正常工作；低 = 关断器件（症状 RX=0，模块可能发热）
//        EN=DE    -> 高 = 发送（MAX485 变体）；固定接高会占死总线
//      以上三种脚均为标准 CMOS 输入（VIH 2.0V / VIL 0.8V），3.3V 与 5V 逻辑均可驱动。
//   ② MAX485 / MAX3485（单向半双工，EN 即方向控制脚）：EN 必须接一个空闲 GPIO，并把该 GPIO 号
//      填到本项 —— 固件发送前拉高 EN、发送完成并等满保护间隔后拉低。严禁固定接高
//      （常使能会占死总线：自己发的字节被自己收回来，从站永远无法应答）。
//   空闲 GPIO 建议 GPIO3 / GPIO4 / GPIO5 / GPIO10：避开 GPIO2 / GPIO8 / GPIO9 三颗 strapping 脚
//   （上电电平会决定启动模式）与 GPIO18 / GPIO19（原生 USB D-/D+，占用会丢串口）。
//   ⚠️ 改本项后必须重新编译烧录；若实测方向相反（收不到响应 / RX 帧恒等于 TX 帧），
//      把 RS485_DE_ACTIVE_HIGH 改成 0 复测。
#define RS485_DE_PIN                (-1)   // -1 = 无方向控制（MAX13487 / MAX13488）；MAX485 / MAX3485 填实际 GPIO 号
#define RS485_DE_ACTIVE_HIGH        1      // 1 = EN 高电平为"发送"（MAX485 常见接法）；实测相反则改 0

#define RS485_STATS_EVERY_N         100    // 每 N 次回环打印一次累计统计

// ---------------------------------------------------------------
// M5：真实 485 总线（QI 真机 STB3-125，功能码 0x04 读输入寄存器）+ 3 段读取
// 事实来源：04_ai_prompts/00_context.md、05_hardware/STB3-125_Errata.md §4「B 方案：3 段读取」
// ---------------------------------------------------------------
#define RS485_SLAVE_ADDR            1      // 从站地址（QI 设备地址 1）
#define MODBUS_FUNC_READ_HOLDING    0x03   // 读保持寄存器（仅 RS485_MODE_LOOPBACK_SELFTEST=1 的串口层自检用）
#define MODBUS_FUNC_READ_INPUT      0x04   // 读输入寄存器（QI 真机用，M5 起真实总线唯一功能码）

// 请求帧（C2）—— MODBUS_TEST_FRAME 仅供 RS485_MODE_LOOPBACK_SELFTEST = 1 的串口层自检使用；
// 真实总线模式（=0）下不得再发 0x03（0x03 读的是保持寄存器=配置值，会产出"看着正常但语义错误"的数据）。
// CRC16/Modbus = 0x0BC4 -> 低字节在前 = C4 0B（已独立复算，与 00_context.md 第四节 4.1 一致）
#define MODBUS_TEST_FRAME  {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B}
#define MODBUS_TEST_FRAME_LEN        8      // 地址1 + 功能1 + 起始2 + 数量2 + CRC2

// 期望响应帧长（M4 遗留，回环自检与长帧兜底参考）：地址1 + 功能1 + 字节数1 + 数据4 + CRC2 = 9
#define MODBUS_EXPECT_RESP_LEN       9

// ---- M5：0x04 读输入寄存器请求帧（C3~C7）------------------------------------
// C3：读取方案开关 —— 【批次 2 起废除编译期二选一，改为运行时自适应】
//   运行时变量 g_readMode（定义在 esp32c3_gateway.ino）：
//     0 = 一帧读全（QI_FULL_FRAME，启动默认；用于验 E-02 的 A 分支）
//     1 = 3 段读取（S1/S2/S3；从未成功且连续 QI_FULLFRAME_FALLBACK_AFTER 轮失败时自动降级）
//   两组常量表均始终编译，按 g_readMode 选择；降级后本会话不回切。
//   ⚠️ 原编译期宏 QI_READ_SPLIT_3SEG 已删除（不再有编译期二选一）。

// C4：3 段请求帧（每段均 8 字节）
//   S1 覆盖 30001~30016（0x0000~0x000F，16 个寄存器）
//   S2 覆盖 30017~30032（0x0010~0x001F，16 个寄存器）
//   S3 覆盖 30033~30039（0x0020~0x0026，7 个寄存器）
//   ⚠️ CRC 已由 06_tests/m5_frame_check.py 独立复核，逐字节照抄，不得重算后替换。
#define QI_SEG1_FRAME  {0x01, 0x04, 0x00, 0x00, 0x00, 0x10, 0xF1, 0xC6}
#define QI_SEG2_FRAME  {0x01, 0x04, 0x00, 0x10, 0x00, 0x10, 0xF0, 0x03}
#define QI_SEG3_FRAME  {0x01, 0x04, 0x00, 0x20, 0x00, 0x07, 0xB0, 0x02}
#define QI_SEG_FRAME_LEN             8      // 地址1 + 功能1 + 起始2 + 数量2 + CRC2

// C5：一帧读全请求帧（运行时 g_readMode = 0 时使用）
//   覆盖 30001~30039（0x0000~0x0026，39 个寄存器）
//   ⚠️ CRC 已由 06_tests/m5_frame_check.py 独立复核，逐字节照抄。
#define QI_FULL_FRAME  {0x01, 0x04, 0x00, 0x00, 0x00, 0x27, 0xB0, 0x10}

// C6：各段期望响应帧长（⚠️ 仅作长帧兜底的初值；实际收帧仍按 byte count 动态算，不得写死判帧长）
//   正常响应 = 地址1 + 功能1 + 字节数1 + 数据(2×N) + CRC2
#define QI_SEG1_RESP_LEN             37     // 1 + 1 + 1 + 2×16 + 2
#define QI_SEG2_RESP_LEN             37     // 1 + 1 + 1 + 2×16 + 2
#define QI_SEG3_RESP_LEN             19     // 1 + 1 + 1 + 2×7  + 2
#define QI_FULL_RESP_LEN             83     // 1 + 1 + 1 + 2×39 + 2

// 批次 2：一帧读全失败自动降级阈值 —— 从未成功且连续失败达此轮数即降级为 3 段。
//   仅当"本会话从未成功过"才降级；曾经成功过则永不降级（偶发失败是总线问题，3 段同样会失败）。
#define QI_FULLFRAME_FALLBACK_AFTER  3

// 批次 2：seg 取值常量
//   数据行 seg（成功轮读取模式）：一帧读全 -> "1"；三段读取 -> "3"
//   err 行 seg（失败段 tag）：一帧读全 -> "F"；三段读取 -> "S1"/"S2"/"S3"
#define QI_SEG_TAG_FULL              "F"
#define QI_SEG_TAG_MODE_FULL         "1"
#define QI_SEG_TAG_MODE_3SEG         "3"

// C7：3 段合并缓冲（39 个寄存器 × 2 字节）；拼接偏移 S1->0、S2->32、S3->64
#define QI_MERGED_LEN                78

// C8：RX 缓冲上限保持 96（最大一帧 83 字节已够，不动）
#define MODBUS_RX_BUF_MAX            96

// 接收模式开关：
//   0 = 真实 485 总线（默认）：接 MAX13487 + QI 真机（STB3-125），发 8 字节 0x04 读帧 / 期望收 37/37/19（或 83）字节
//   1 = M3 式物理回环自测：GPIO21 短接 GPIO20 且不接 MAX13487，发 8 字节 / 期望收回同一 8 字节
//       该分支代码保留不动，用于串口层回归（改 1 烧录自测，测完务必改回 0）。
#define RS485_MODE_LOOPBACK_SELFTEST 0   // 2026-10-04 第 2 步自检已通过（GPIO20 完好，R-13 解除），已由 WorkBuddy 改回 0

// ---------------------------------------------------------------
// 批次 1（功能对齐）：JSON 上行 / 设备命名 / 累计计数器
// 事实来源：04_ai_prompts/04_Codex提示词_功能对齐_JSON上报与统计.md 三
// 本段不涉及任何 485 侧逻辑（帧 / 段数 / 超时 / 解析 / CRC 一律不动）。
// ---------------------------------------------------------------
// g_payload 容量：数据行除工程量外还要带 raw（78 字节 -> "XX XX ..." 233 字符）
// 以及 quality / invalid / seg 等字段 => 典型约 892 字节；小缓冲会被 snprintf 静默截断。
// §8.2 ②：最坏合成 1106 B（invalid 填满 12 个字段名 + 10 位计数器 + raw 233 字符）> 1024
//   => 提到 1200（余量 ~90 B）；appendf 的截断告警路径保留不动（不会静默截断）。
#define QI_UDP_PAYLOAD_MAX           1200

// dev = QI_DEV_PREFIX + MAC 后 6 位十六进制（大写）；例 48:F6:EE:15:9B:88 -> "ESP485-159B88"
#define QI_DEV_PREFIX                "ESP485-"
#define QI_MAC_STR_LEN               18   // "48:F6:EE:15:9B:88" + '\0'
#define QI_DEV_STR_LEN               16   // "ESP485-159B88" + '\0'
#define QI_TS_ISO_LEN                24   // "2026-10-06T20:10:03" + 余量 + '\0'
#define QI_RAW_HEX_MAX               (3 * QI_MERGED_LEN + 1)   // 78 字节 -> 233 字符 + '\0'
#define QI_INVALID_MAX               160  // invalid（无效字段逗号列表）缓冲；15 个字段名最长约 128 字符

// §8.2 ①：该型号恒不上报接线柱温度（raw 恒 0 => 换算后恰为量程下限 -40）。
//   1（默认）= A/B/C_temp == -40 视为"已知不上报"：不进 invalid、不降 quality（仍可 OK）；
//   0        = 退回旧行为（== -40 也进 invalid、降 PARTIAL）。
//   真正越界（< -40 或 > 160）无论宏取值都算本次异常 => 进 invalid、降 PARTIAL。
//   口径与 06_tests/db_proto.py 的 --known-missing（默认 A_temp,B_temp,C_temp）一致。
#define QI_PHASE_TEMP_ABSENT         1

// §8.2 ③：失败行（type:"err"）开关，本次烧录默认 1（M6 之后可改回 0）。
//   1 = 失败轮在心跳行之外再发一行 {"type":"err","seg":..,"reason":..}；
//       理由：第一次真机运行时 USB 必须拔掉（安全红线）=> 没有串口 => 这行是唯一能
//       看见"失败段 + 原因"的通道。串口日志 FAIL:Sx:原因 无论宏取值都保留。
//   0 = 严格两类行（心跳 + 数据，对齐老师版本）。
#define QI_SEND_ERR_ROW              1

// 数据行 "seg" 取值（批次 2 起）：一帧读全 -> "1"，三段读取 -> "3"（指令 3.1 的 "<0|1|3>"）
// ---------------------------------------------------------------
// 批次 3（常驻 AP + Web 配置页 + /status + NVS 持久化）：版本号 / AP 默认 / NVS 键名 / 校验区间
// 事实来源：04_ai_prompts/07_Codex提示词_批次3_AP与Web配置与NVS.md 三 config.h
// 本段只新增常量与键名，不改任何 485 侧逻辑与 UDP JSON 上行格式。
// ---------------------------------------------------------------
#define FW_VERSION                   "0.6.0-batch3"   // /status 的 fw 字段
#define AP_DEFAULT_SSID_PREFIX       "ESP485-"        // 常驻 SoftAP SSID 前缀（+ MAC 后 6 位 = dev）
#define AP_DEFAULT_PASS_PREFIX       "485-"           // 默认口令派生前缀（+ MAC 后 4 位 HEX，恰 8 字符）
#define QI_AP_PASS_MIN_LEN           8                // softAP 口令下限：1~7 字符会被协议栈拒绝并启动失败

// NVS（Preferences）命名空间与键名；读法：NVS 有值用 NVS，无值回落编译期默认（secrets.h / 本文件）
#define NVS_NAMESPACE                "cfg485"
#define NVS_KEY_WIFI_SSID            "wifi_ssid"
#define NVS_KEY_WIFI_PASS            "wifi_pass"
#define NVS_KEY_WIFI_OPEN            "wifi_open"
#define NVS_KEY_NTP1                 "ntp1"
#define NVS_KEY_UDP_IP               "udp_ip"
#define NVS_KEY_UDP_PORT             "udp_port"
#define NVS_KEY_POLL_S               "poll_s"
#define NVS_KEY_ADMIN_USER           "admin_user"
#define NVS_KEY_ADMIN_PASS           "admin_pass"
#define NVS_KEY_AP_PASS              "ap_pass"

// 采集周期允许区间（秒）：Web 表单校验用（老师版口径 1-5s）
#define POLL_PERIOD_MIN_S            1
#define POLL_PERIOD_MAX_S            5

// 配置页缓冲：模板 3359 字符 + 变量展开最坏约 962 字符 => 约 4.3 KB（8192 留 ~2 倍余量）
//   snprintf 溢出会显式告警并截断（绝不静默），见 web_config.h 的 webSendPage()。
#define QI_WEB_PAGE_MAX              8192
// /status JSON 缓冲：老师 22 项 + 我方 3 项；最坏约 745 字节（超长 SSID 32B、chrony 47B、5 个计数各 10 位）。
//   768 仅余 ~23 B 偏紧，取 1024 留余量（snprintf 溢出会显式告警并回 500，绝不静默截断）。
#define QI_STATUS_JSON_MAX           1024
