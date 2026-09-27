/*
 * esp32c3_gateway.ino
 * ESP32-C3 RS-485 采集网关（替换树莓派网关项目）—— M1 环境骨架
 *
 * 本文件当前为最小空壳，仅用于验证工具链可用：
 *   - setup() 只初始化 USB CDC 串口
 *   - loop() 空转
 *
 * 尚未引入（按阶段计划逐个接入）：
 *   WiFi / NTP / RS-485(UART1) / Modbus / UDP 上行
 *
 * 事实来源：04_ai_prompts/00_context.md
 * 编译设置：Arduino IDE -> Tools -> USB CDC On Boot = Enabled
 */

void setup() {
  // USB CDC 串口：ESP32-C3 SuperMini 通过 USB 与电脑通信
  Serial.begin(115200);

  // 占位：M1 阶段在此接入 Serial1(UART1) 回环自检
  // Serial1.begin(9600, SERIAL_8N1, 20, 21);  // RX=GPIO20, TX=GPIO21
}

void loop() {
  // 空壳：M2 起接入最小非阻塞状态机 IDLE -> TX -> WAIT_RX -> PARSE -> SEND
}
