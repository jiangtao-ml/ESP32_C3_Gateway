# 06 · Codex 提示词 —— 批次 2：一帧读全 + 失败自动降级（运行时自适应）

> **使用方式**：把 `====` 之间整段复制给 Codex。
> **前置状态**：批次 1 已真机验证 100% 通过并 commit（`1334527`，基线 Flash `1004935` / RAM `39284`）。
> **本批次目标**：把「一帧读全 vs 3 段读取」从**编译期开关**改为**运行时自适应**，真机跑一次即收敛 E-02。

====

## 0. 任务一句话

在现有非阻塞状态机上，把 0x04 轮询的「一帧读全（`01 04 00 00 00 27 B0 10`，期望 83 字节）」与「3 段读取（37/37/19 字节）」改为**运行时模式变量**：启动先试一帧读全；**从未成功且连续 3 轮失败**则自动降级为 3 段，本会话不再回切。降级后的 3 段路径必须与批次 1 已验证代码完全一致。

## 1. 背景（必读）

1. 设备 STB3-125，Modbus RTU：地址 1 / 9600 / 8N1 / 无校验 / CRC16-Modbus（低字节先）。**只读不写**，功能码恒 `0x04`。
2. 协议原文自述「一次最多读 16 个寄存器」，读 39 个（`00 27`）是否被接受 = **Errata E-02，未实测**。判定树（`02_docs/06_QI协议说明.md` §7 / `06_tests/M5_真机联调操作卡.md` §5）：
   - 收到 **83 字节合法响应** → 一帧读全成立（A 分支）；
   - 收到 **`01 84 02` 异常响应**（或超时）→ 3 段为正式方案（B 分支）。
3. 现有实现（`esp32c3_gateway.ino`）：编译期宏 `QI_READ_SPLIT_3SEG`（config.h，当前 =1）在 ST_TX / parseResponse / ST_PARSE 三处二选一编译。**两条路径的解析代码已存在且共用**：
   - 一帧：`QI_FULL_FRAME`（8 字节，已在 config.h C5 定义）→ 期望 83 字节；
   - 3 段：`QI_SEG_FRAMES[3]` → 37/37/19 字节。
   - parseResponse (7) 按 `QI_SEG_MERGE_OFF[g_segIndex]` 把数据域 memcpy 进 `g_merged`（78 字节）。**一帧模式 = 1 段、off=0、byteCount=78，落进 g_merged 的结果与 3 段拼接逐字节一致** ⇒ `buildDataJson`（读 g_merged）**零改动**。
4. 帧长驱动（`need = rxFrame[2] + 5`）对 37/37/19/83 自动成立；三级超时（80ms/8ms/300ms）不变；`MODBUS_RX_BUF_MAX=96` 已够 83 字节，**都不要动**。

## 2. 改动清单

### config.h

| # | 项 | 内容 |
| --- | --- | --- |
| 1 | 废除编译期二选一 | `QI_READ_SPLIT_3SEG` 宏**删除**（或保留注释说明已被运行时模式取代）；两套常量表都编译 |
| 2 | 新增降级阈值 | `#define QI_FULLFRAME_FALLBACK_AFTER 3` |
| 3 | 常量表整理 | 一帧组：`{QI_FULL_FRAME}`、COUNT=1、RESP_LEN=83、MERGE_OFF={0}、TAG="F"；3 段组：沿用现有 `QI_SEG_FRAMES/RESP_LENS/MERGE_OFF`、TAG="S1/S2/S3"。建议两组都以 `const` 数组集中定义，按 `g_readMode` 选择 |

### esp32c3_gateway.ino

| # | 项 | 内容 |
| --- | --- | --- |
| 1 | 运行时状态 | `static uint8_t g_readMode = 0;`（0=一帧，1=三段，**启动默认 0**）；`static uint8_t g_fullConsecFail = 0;`；`static bool g_fullEverOk = false;` |
| 2 | ST_TX | 按 `g_readMode` 选请求帧与 `g_expectRxLen`（83 或 37/37/19）。`#if RS485_MODE_LOOPBACK_SELFTEST` 回环分支**原样保留** |
| 3 | ST_PARSE 段推进 | 一帧模式 `QI_SEG_COUNT=1`：一段结束即本轮结束转 ST_SEND；3 段模式不变 |
| 4 | parseResponse | ① `reqFunc`/段偏移/`tag` 按 `g_readMode` 取；② 一帧成功：`g_fullEverOk=true`、清 `g_fullConsecFail`；③ 一帧失败（TIMEOUT/EXC84-xx/CRC/LEN 等任意原因）：`g_fullConsecFail++`，**从未成功且达到 `QI_FULLFRAME_FALLBACK_AFTER`** ⇒ `g_readMode=1` + 打印降级日志（见判据 3.3）+ 清计数；**曾经成功过则永不降级**（偶发失败是总线问题，3 段同样会失败） |
| 5 | seg 字段语义（**保持批次 1 双关语义，只扩展值域**） | 数据行 `seg` = 成功轮读取模式：`"1"`（一帧）/`"3"`（三段）；err 行 `seg` = 失败段 tag：`"F"` / `"S1"/"S2"/"S3"` |
| 6 | 心跳行加 `mode` | `"mode":"full"` / `"mode":"3seg"`（放 `free_heap` 附近），远程可直接观察当前模式与降级 |
| 7 | 降级日志 | 降级时刻串口打印一条明确日志（格式见 3.3），之后每轮不再重复打印 |

## 3. 验收判据

### 3.1 编译
- `arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" 01_firmware/esp32c3_gateway`：**0 error / 0 warning**；
- 报告 Flash/RAM 与批次 1 基线（`1004935` / `39284`）的差值（预期 +几十~几百 B，量级异常需说明）。

### 3.2 逻辑自查（Codex 提交 diff 时附一页说明）
- 一帧成功路径：seg="1"、quality 正常、raw 仍 78 字节（g_merged 内容与 3 段拼接一致）；
- 降级路径：第 3 次失败打印降级日志，第 4 轮起 seg="3"、mode="3seg"，poll 计数连续；
- 回环自测模式（`RS485_MODE_LOOPBACK_SELFTEST=1`）不受影响。

### 3.3 真机 / 台面（由 WorkBuddy 出操作卡，Codex 不负责）
- **真机一次跑同时收敛 E-02**：无论最终 seg="1"（E-02 A 分支成立）还是先降级后 seg="3"（B 分支成立），**都算批次 2 成功**；结果由 JSON `mode`/`seg` 字段 + 串口降级日志判读。
- 降级日志格式：`[M5] 一帧读全连续 3 轮失败（EXC84-02），本会话降级为 3 段读取（E-02 B 分支）`（失败原因取最后一次的 `g_failReason`）。

## 4. 硬约束（红线，违反即返工）

1. **非阻塞状态机不变**（IDLE→TX→WAIT_RX→PARSE→SEND），禁止 delay()、禁止重构；
2. 手写 Modbus 帧组装，不引第三方库；**固件只搬原始值**，换算留在组包函数（现有 buildDataJson 逻辑不动）；
3. **只读不写**：不得出现任何 0x05/0x06/0x10/0x03 帧；一帧读全帧 `01 04 00 00 00 27 B0 10` 来自 `02_docs/06_QI协议说明.md` §3.1 第 1 条，不得新发明帧；
4. 三级超时 / 帧长驱动 / `MODBUS_RX_BUF_MAX=96` / `RS485_CHAR_TIMEOUT_MS=8` 不动；
5. `Serial.setTxTimeoutMs(0)`（批次 1 bugfix）必须保留；
6. 完成后**不自审**：diff 交 WorkBuddy/用户独立复核。

## 5. 交付物

1. `config.h` + `esp32c3_gateway.ino` 的改动（diff 或整文件）；
2. 编译输出（含 Flash/RAM 用量与基线差值）；
3. §3.2 逻辑自查一页说明。

====

## 附：WorkBuddy 侧配套（不由 Codex 做）

- `06_tests/db_proto.py`：`seg_ok` 适配一帧模式（seg="1" ⇒ `seg_ok="1"`）；失败行 seg="F" 的解析。
- 真机验证操作卡 + E-02 结论回写（`06_QI` §7 / Errata）：等真机跑完由 WorkBuddy 执行。
