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
 */
#include "../../esp32c3_gateway/esp32c3_gateway.ino"
