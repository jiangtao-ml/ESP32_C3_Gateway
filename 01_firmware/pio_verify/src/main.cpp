/*
 * pio_verify —— PlatformIO 并行验证入口（**只编译、不烧录**）
 *
 * 设计要点：**不复制、不改写**任何现有源码。
 *   本文件只 include 现有工程的那份 .ino —— 这样 PlatformIO 编的就是
 *   **与 Arduino IDE 完全同一份源码**，产物体积的差异只可能来自
 *   "工具链 / core 版本 / 编译选项"，而不会来自代码本身。
 *
 * 若将来确认迁移，再把 .ino 正式拆成 src/*.cpp（届时需要自己补函数原型，
 * 因为 .ino 由 Arduino 预处理器自动生成原型，.cpp 不会）。
 *
 * 事实来源：02_docs/16_数据落地与商业化设计.md §9.5
 *
 * ⚠️ 2026-10-07（砚青）：下面的 `#include <WiFi.h>` **不是冗余**——
 *    LDF 的 deep 模式不会跟进 src/ 之外的 .ino 文件扫描库依赖
 *    （依赖图为空 ⇒ 库源码不编译、头文件路径不下发）；
 *    而 deep+ 能跟进、却触发上游 bug（espressif/arduino-esp32#9782，
 *    WiFi 报 Network.h 找不到）。⇒ 在 main.cpp 显式列出 .ino 用到的库头，
 *    让 LDF 从本文件即可发现 WiFi ⇒ 递归发现 Network。与 .ino 重复包含无害
 *    （头文件均有 include guard）。
 */
#include <WiFi.h>   // LDF 锚点：让 PlatformIO 发现 WiFi 库（进而递归发现 Network）
#include "../../esp32c3_gateway/esp32c3_gateway.ino"
