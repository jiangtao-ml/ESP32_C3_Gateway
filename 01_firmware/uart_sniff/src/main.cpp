// ============================================================================
//  uart_sniff —— ESP32 侧「只听不发」原始字节监听
//
//  为什么要单独做：
//    之前的注入实验是在业务固件持续发送的背景下做的，ESP32 每秒发 3 帧，
//    与 CH340 的注入帧在同一对 A/B 上撞车 ⇒ 收到的位错既可能是「链路质量差」，
//    也可能只是「总线冲突」。两者处理方式完全不同，必须分开测。
//
//  本固件：UART1 只初始化接收（RX=GPIO20 / TX=GPIO21），**永不写入**。
//          GPIO21 被 UART 外设持为空闲高，模块 DI 收到恒定高 ⇒ 不驱动总线
//          ⇒ 总线上只有 CH340 一个发送源，这是干净的单源测量。
//
//  用法：烧录后接 CH340，运行注入脚本，看串口打印的原始 HEX。
//        判据：收到 8 字节 8 次完全相同 = 链路无误码（那之前的位错就是冲突）；
//              每次都不同 = 链路真的有误码（信号完整性 / 接触电阻问题）。
// ============================================================================

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1500);

  // 只收不发：UART1 RX=GPIO20，TX=GPIO21 由外设持有（空闲高），本固件从不 write
  Serial1.begin(9600, SERIAL_8N1, 20, 21);
  Serial1.setTimeout(0);

  Serial.println();
  Serial.println("############################################################");
  Serial.println("#  uart_sniff —— ESP32 只听不发（单源测量，排除总线冲突）");
  Serial.println("#  UART1 RX=GPIO20 / TX=GPIO21 @9600 8N1，永不写入");
  Serial.println("#  等 CH340 注入，下面按批打印收到的原始 HEX");
  Serial.println("############################################################");
}

void loop() {
  static uint8_t buf[64];
  static int n = 0;
  static uint32_t lastByte = 0;

  // 收到就攒，50 ms 无新字节则整批打印一次
  while (Serial1.available()) {
    int b = Serial1.read();
    if (b < 0) break;
    if (n < (int)sizeof(buf)) buf[n++] = (uint8_t)b;
    lastByte = millis();
  }

  if (n > 0 && (uint32_t)(millis() - lastByte) > 50) {
    Serial.printf("[RX %2d] ", n);
    for (int i = 0; i < n; i++) Serial.printf("%02X ", buf[i]);
    Serial.println();
    n = 0;
  }
}
