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
// 上行 UDP —— 目标为【电脑侧】接收端（当前唯一一台电脑，Windows）
// ⚠️ 192.168.1.113 是电脑侧 IP（会随网络漂移），192.168.1.200 是 ESP32 自身 IP，勿混用
// ---------------------------------------------------------------
#define UDP_TARGET_IP        "192.168.1.113"   // 电脑侧（2026-10-25 21:44 实测；402FBS 网段）
                                               // ⚠️ 漂移史：09-27 .117 → 10-04 .118 → 10-05 .114 → 10-05 晚 .113
                                               //    → 10-08 .121 → 10-22 .150（原定旧测试电脑，**已弃用**）
                                               //    → 10-23 .120 → 10-25 192.168.1.113
                                               //    → 10-26 曾临时改 10.111.73.223（校园网），**已回滚**
                                               // ⚠️ **本行只是编译期默认值**：启动时若 NVS 的 `udp_ip` 有值，
                                               //    就用 NVS 的值覆盖它（批次 3 运行时配置）。
                                               //    ⇒ 改本行**必须重新编译烧录才可能生效**，而那也只在 NVS 无值时才有用。
                                               // ⚠️ **不要用 `/reset`**：它会把 WiFi 凭据一起清掉，还得重新配网。
                                               //    正确做法＝Web 配置页把「UDP 目标」改成实际 IP 后保存（立即生效，不用重烧）。
#define UDP_TARGET_PORT      8888              // 电脑侧接收端监听端口
#define UDP_LOCAL_PORT       12345             // ESP32 本地 UDP 端口（M2 只发不收；便于抓包定位）

// ---------------------------------------------------------------
// 网络 —— ESP32 自身静态 IP（Q-02 主方案：学校 WiFi + WiFi.config 固定）
// ⚠️ 2026-10-26 曾临时改到校园网 10.111.0.0/16，**已回滚**：校园网开客户端隔离，
//    同网段设备 ping 不通（实测网关可通、ESP32 100% 丢包），UDP 必然发不出去。
//    ⇒ 本项目只能跑在**自己可控的路由器/热点**下（402FBS 这类，无 AP 隔离）。
// ---------------------------------------------------------------
#define LOCAL_IP             "192.168.1.200"   // ESP32 自身（M1 前置：ping 确认空闲）
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
#define RS485_RX_PIN                20     // UART1 RX = GPIO20（3.3V 逻辑直连，不分压；实物已定稿）
                                           // ⚠️ R-13：M4 接线曾 5V 直入本脚（ESP32-C3 IO 绝对最大 3.6V），输入通路可能已受损。
                                           //    排障顺序：先用 RS485_MODE_LOOPBACK_SELFTEST=1 + 短接 GPIO21↔GPIO20 验证本脚；
                                           //    若回环异常，改用 GPIO3 / GPIO4 / GPIO5 / GPIO10 作 UART1 RX，并同步改本行与文档。
#define RS485_TX_PIN                21     // UART1 TX = GPIO21
#define RS485_TX_GUARD_MS           2      // 发送→接收的方向切换保护间隔（AutoDirection 换向余量）
                                           // 说明：Serial1.flush() 在本 core（3.x；本机 3.3.10 / pio_verify 3.3.11）会一直等到 TX 完全空闲
                                           //   （HardwareSerial::flush → uartFlush → uartFlushTxOnly(uart, true) → uart_ll_is_tx_idle：
                                           //    txfifo_cnt==0 且 TX 状态机回到 IDLE），所以 2ms 不用于补 flush 的不足，
                                           //   仅为换向留防御性余量（MAX13487 手册 tDDD + tRED ≤ 2.6µs；MAX485 变体切换 DE 同理）。
                                           // ⚠️ M4 真实总线上若稳定丢首字节，先在 config.h 把 RS485_TX_GUARD_MS 调到 3~5ms 复测（不是改 RS485_CHAR_TIMEOUT_MS）
#define RS485_FIRST_BYTE_TIMEOUT_MS 80     // 一级：等首个字节的时限
#define RS485_CHAR_TIMEOUT_MS       8       // M5 正式档位（4~8，取 8）。二级：字节间静默阈值（≈7.7 个字符时间，8 ms @9600 8N1）；
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

// 串口逐轮详细回显开关（2026-10-21 新增）。
//   1 = 保留每轮的 [M4] 解析行 + 39 个寄存器 REG 行 + 每条 UDP 行的整包回显
//       （约 44 次 write / 1961 B 每轮）。台面调试、验收脚本 grep 用。
//   0 = 只保留失败与告警（[UDP][ERR]、[M4] 超时、[POLL][SKIP]）。
//       **168h 长跑必须为 0**：拔 USB 时这些字节全部格式化后被丢弃，
//       168h 累计约 1.09 GB 的纯浪费，且每轮多占约 1–2 ms 主循环。
//   ⚠️ 设为 0 后 `m4_accept.py` / `m5_parse.py` 依赖的 "[M4] 解析 OK" 与 "REG:" 锚点会消失，
//      那些脚本只在台面用，不受影响。
#define SERIAL_VERBOSE_ROUND         0

// 数据行 "seg" 取值（批次 2 起）：一帧读全 -> "1"，三段读取 -> "3"（指令 3.1 的 "<0|1|3>"）
// ---------------------------------------------------------------
// 批次 3（常驻 AP + Web 配置页 + /status + NVS 持久化）：版本号 / AP 默认 / NVS 键名 / 校验区间
// 事实来源：04_ai_prompts/07_Codex提示词_批次3_AP与Web配置与NVS.md 三 config.h
// 本段只新增常量与键名，不改任何 485 侧逻辑与 UDP JSON 上行格式。
// ---------------------------------------------------------------
// 【2026-10-27 砚青】0.6.0-batch3 -> 0.7.0-batch4。
//   本版含：P0-1 时钟回退钳位 / P0-2 心跳诚实化 / P0-3 看门狗+复位原因 / P0-4 版本自报
//          + D-10 电量公式修正（(H*65536)/1000+L -> (H*65536+L)/1000）。
//   ⚠️ **电量刻度与 0.6.x 不兼容**（差 1000 倍）⇒ 接收端/报表用 fw 字段区分口径，不要混算。
//   不升版号等于 P0-4「版本自报」白做——现场无法区分跑的是哪一版。
#define FW_VERSION                   "0.7.0-batch4"   // /status 的 fw 字段 + 心跳 fw 字段（同一常量）
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
//   snprintf 溢出会显式告警并回 500（不发截断页），见 web_config.h 的 webSendPage()。
#define QI_WEB_PAGE_MAX              8192
// /status JSON 缓冲：老师 22 项 + 我方 3 项；最坏约 761 字节（超长 SSID 32B、NTP 主机名 63B、5 个计数各 10 位）。
//   768 仅余 ~23 B 偏紧，取 1024 留余量（snprintf 溢出会显式告警并回 500，绝不静默截断）。
#define QI_STATUS_JSON_MAX           1024

// -------------------------------------------------------------------
// 【批次 3.5 · LED 状态指示】板载蓝灯（HW-466AB：GPIO8，低电平点亮）
//   ⚠️ [待确认] 不同批次 SuperMini 板的 LED 引脚/极性可能不同（另有接 GPIO8 高电平的版本）；
//      首次上电观察：若闪烁语义相反（常亮=正常），把 LED_ACTIVE_LOW 改 0 即可。
//   语义（全 millis() 非阻塞翻转，绝不 delay）：
//     WiFi 未连接        -> 慢闪（半周期 LED_WIFI_SLOW_MS 亮/灭交替）
//     本轮采集成功       -> 短亮 LED_POLL_OK_MS（每轮脉冲，1s 周期下=每秒轻闪）
//     本轮采集失败       -> 长亮 LED_POLL_FAIL_MS（现场区分"活着但总线有问题"）
#define LED_PIN                      8
#define LED_ACTIVE_LOW               1
#define LED_POLL_OK_MS               30
#define LED_POLL_FAIL_MS             200
#define LED_WIFI_SLOW_MS             500

// -------------------------------------------------------------------
// 【商用级加固 · 2026-10-27 砚青】对应 Codex 复核报告的固件四条 A 类修复
//
//   编号一律沿用 Codex 原报告（**不是** 02_docs/29 的 P0 序号，两套编号不同，勿混）：
//     P0-1 时钟回退钳位（拆掉 CLOCK_STEP 死结）  -> FW_CLK_* / FW_TS_MONOTONIC_ENABLE
//     P0-2 心跳诚实化（queue/dropped 由假 0 改 null） -> 见 .ino buildHeartbeatJson()
//     P0-3 看门狗 + 复位原因上报 + boot 计数      -> FW_WDT_* / FW_BOOT_CNT_MAGIC
//     P0-4 版本自报（心跳带 fw 字段）             -> 复用下方已有的 FW_VERSION
//
//   ⚠️ 全部是**新增开关**，置 0 即回到批次 3 行为，便于 A/B 对照与一键回退。
//   ⚠️ 本轮改动**未经真机验证**（截至 2026-10-27 只做了双模式编译），
//      烧录后必须重跑一轮并复核 reset/boot/clk_hold 三类新字段。
// -------------------------------------------------------------------

// ---- P0-3 任务看门狗（TWDT）----
// 为什么必须开：批次 3 及之前，固件若在 ST_WAIT_RX 等状态里卡死（或被 ISR 长时间抢占），
//   现场表现为"板子还在、灯还亮，但不再发包"，**无法区分"死机"与"掉线"**，
//   且只能靠人去断电重启。开了 WDT 至少能自愈，并由 esp_reset_reason() 留下 TASK_WDT 证据。
// 取值依据（2026-10-27 砚青按 B 类 F-4 修订 30s -> 60s）：
//   ① 正常路径极快：整轮最坏耗时是 3 段读取（3×约 300ms 兜底 + 段间静默）≈ 1s 量级，
//      60s 有近两个数量级余量，**不会误杀**。
//   ② 真正的风险在 **Web 配置页**：WebServer 库的 HTTP_MAX_{DATA,POST,SEND,CLOSE}_WAIT 各 5000ms
//      （`WebServer.h:66-69`，且**四个宏都没有 `#ifndef` 保护** ⇒ 无法用 `-D` 覆盖、也不该改库），
//      NetworkClient 的 `WIFI_CLIENT_MAX_WRITE_RETRY=10` × `SELECT_TIMEOUT=1s` ⇒ **单次 write 最坏 10s**
//      （`NetworkClient.cpp:31-32`）。8 KB 配置页按 1 KB 分块写、TCP 发送缓冲不足时要多次 write
//      ⇒ 极端慢客户端下最坏 5 + 2×10 + 5 ≈ **30s**，正好压 30s 的线 ⇒ 会误杀。
//      抬到 60s 后留 2 倍余量。
//   ③ 代价：真卡死的检出延迟从 30s 变 60s。对 8h/168h soak **无实质影响**（重启即自愈，
//      且 `esp_reset_reason()` 会留 `TASK_WDT` 证据 + `boot` 计数 +1，可观测）。
//   ⚠️ 残留风险：若连续两个极端慢客户端各阻塞满额（>60s），仍可能重启。这是**已接受的失败模式**
//      （配置页是人工操作场景，非法自动流量），判据见 02_docs/30 §5 F-4。
// ⚠️ 本机 IDF 5.5 的 esp_task_wdt.h **只有新签名** esp_task_wdt_init(const esp_task_wdt_config_t*)，
//    旧的两参数写法 esp_task_wdt_init(30, true) 在该头文件里无声明 ⇒ 会编译失败（已实测 grep）。
#define FW_WDT_ENABLE                1
#define FW_WDT_TIMEOUT_MS            60000
#define FW_BOOT_CNT_MAGIC            0x48504254UL   // boot 计数 magic：RTC_NOINIT 区上电是随机值，必须先校验

// ---- P0-1 时钟回退钳位 ----
// 背景（02_docs/24 §5.6 的 Y-4）：CLOCK_STEP 事件数必须 = 0，否则 168h 总体判 F。
//   触发链：WiFi 掉线 → 重连 → 重新 configTzTime → 本机 RTC 日漂 0.9~4.3s
//   ⇒ SNTP 把时钟往回拨 ⇒ 1 Hz + 秒级 ts 必然撞上已用过的秒 ⇒ 接收端记 CLOCK_STEP。
// 双保险，缺一不可：
//   ① 源头（FW_CLK_GUARD_ENABLE）：ntpClampTick() 发现系统时钟被拨回时 settimeofday() 拉回旧值；
//   ② 兜底（FW_TS_MONOTONIC_ENABLE）：buildTimestampIso() 保证 ts 会话内**严格递增**（倒退时用 last+1 跳过）。
// ⚠️ 只做 ② 不行：ts 会单向超前且**每次回拨都累积**，168h 内可能偏出十几秒，M10 时间准确性会难看。
// ⚠️ 只做 ① 不行：configTzTime 是异步的，回包到 loop() 拦截之间存在窗口，stateTick() 可能已发出倒退的 ts。
// ⚠️ 绝不采用"ts 冻结、等真实时间追上"：接收端 udp_collect.py:640 对同秒撞车**直接丢弃该行**
//    （StoreResult.SAME_SECOND 不入库），冻结会连续丢行，直接伤 M2 完整率。
#define FW_CLK_GUARD_ENABLE          1
#define FW_CLK_BACK_HOLD_S           3600   // 回退 <= 此值 ⇒ 拦下并拉回；> 此值视为时钟真错，接受并重建基线
#define FW_TS_MONOTONIC_ENABLE       1      // ts 严格递增护栏（只在"会倒退"时 last+1，同秒行为保持原样）

