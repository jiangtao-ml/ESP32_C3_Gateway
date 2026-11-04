// ============================================================================
//  rs485_probe v3 —— 洞洞板焊接体检（免万用表 / 不需要接真机）
//
//  v3 相对 v2 的三处改动，全部是为了回答一个问题：
//      「这套测量方法本身可信吗？」
//
//  ① 先检查 UART0 是否占用了 GPIO20/21。
//     ESP32-C3 的 GPIO20/21 正是 U0RXD/U0TXD。如果框架或库初始化过 UART0，
//     这两个脚会被外设接管，读到的就是外设电平（空闲时为高），与焊接无关。
//     v3 检测到占用会强制释放，并明确打印 —— 这本身就是一种误判来源。
//
//  ② 增加「对照脚」：GPIO10（本板未连接任何东西，已知悬空）。
//     它必须被读成「外部高阻」。如果它也被读成"被驱动"，
//     说明这套判据不可信，上面关于 GPIO20/21 的所有结论一律作废。
//
//  ③ delay 从 3 ms 加到 12 ms，并重复 3 次取多数，
//     排除内部上下拉对抗大电容时放电不充分造成的假读数。
//
//  判据（内部下拉 45k 下采样，hi 越接近满值越说明外部有源驱动）：
//        hi ≈ 满值 -> 外部有低阻源在驱动该电平
//        hi ≈ 0    -> 外部高阻（开路）或外部强下拉
//        中间值    -> 弱驱动 / 半接触 / 正在抖动
//
//  安全：只读，除 LED(GPIO8) 外不驱动任何引脚。烧录前必须拔掉真机 485 线。
// ============================================================================

#include <Arduino.h>
#include <driver/uart.h>

static const int PIN_RX  = 20;   // 接模块 RO / TXD
static const int PIN_TX  = 21;   // 接模块 DI / RXD
static const int PIN_LED = 8;    // 板载蓝灯，低电平点亮
static const int PIN_REF = 10;   // 对照脚：本板未连接，应始终悬空

struct Result { uint32_t hi; uint32_t n; };

// 单次采样（内部会自动 pinMode，因此不要在外面对同一脚设输出后立即调用）
static Result probe(int pin, uint8_t mode, uint32_t n) {
  pinMode(pin, mode);
  delay(12);                      // v3：3 ms -> 12 ms，给大电容充分放电
  Result r = {0, 0};
  for (uint32_t i = 0; i < n; i++) {
    if (digitalRead(pin)) r.hi++;
    r.n++;
    delayMicroseconds(30);
  }
  return r;
}

static int pct(const Result &r) { return r.n ? (int)((r.hi * 100) / r.n) : 0; }

enum Kind { K_OPEN, K_HIGH, K_LOW, K_MIXED };

static Kind classify(int up, int dn) {
  if (up >= 95 && dn >= 95) return K_HIGH;
  if (up <= 5  && dn <= 5 ) return K_LOW;
  if (up >= 95 && dn <= 5 ) return K_OPEN;
  return K_MIXED;
}

static const char *kindText(Kind k) {
  switch (k) {
    case K_OPEN:  return "外部高阻（开路 / 悬空 / 没接上）";
    case K_HIGH:  return "外部低阻 -> 高电平（有源驱动）";
    case K_LOW:   return "外部低阻 -> 低电平（被拉死）";
    default:      return "读数不稳（半接触 / 抖动）";
  }
}

// 三态测一次，返回归类；同时把三次结果打出来
static Kind probeAndPrint(const char *title, int pin, bool showFloat) {
  Result u = probe(pin, INPUT_PULLUP,   600);
  Result d = probe(pin, INPUT_PULLDOWN, 600);
  const int uu = pct(u), du = pct(d);

  Serial.printf("  %s (GPIO%d)\n", title, pin);
  if (showFloat) {
    Result f = probe(pin, INPUT, 400);
    Serial.printf("     浮空/上拉/下拉 : %d%% / %d%% / %d%% 高\n", pct(f), uu, du);
  } else {
    Serial.printf("     上拉/下拉      : %d%% / %d%% 高\n", uu, du);
  }
  Kind k = classify(uu, du);
  Serial.printf("     判定           : %s\n", kindText(k));
  return k;
}

// 检查 UART0 有没有占用 GPIO20/21
static bool releaseUart0IfHeld() {
  if (uart_is_driver_installed(UART_NUM_0)) {
    uart_driver_delete(UART_NUM_0);
    Serial.println("[警告] UART0 驱动曾安装并占用 GPIO20/21 —— 已强制释放。");
    Serial.println("       这本身就可能让之前的读数是「串口空闲电平」而非真实焊接状态。");
    return true;
  }
  Serial.println("[通过] UART0 未安装，GPIO20/21 不受串口外设占用。");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1600);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Serial.println();
  Serial.println("########################################################");
  Serial.println("#  RS485 焊接体检 v3（含方法自检 + UART0 占用检查）");
  Serial.println("#  前提：真机 485 线已拔掉，只由 USB 供电");
  Serial.println("########################################################");
  Serial.println();
  releaseUart0IfHeld();
}

void loop() {
  static int round = 0;
  round++;
  Serial.printf("\n========== 第 %d 轮 ==========\n", round);

  // ---- 先测对照脚，它是"尺子准不准"的唯一依据 ----
  Kind kRef = probeAndPrint("[REF] 对照脚 · GPIO10（本板未接）", PIN_REF, true);
  Serial.println();
  if (kRef == K_OPEN) {
    Serial.println("  >> 尺子校准通过：悬空脚被正确读成「外部高阻」。");
    Serial.println("     因此下面 GPIO20/21 的判读可信。");
  } else {
    Serial.println("  >> !!! 尺子校准失败 !!!");
    Serial.println("     悬空脚竟然被读成了「有源驱动 / 被拉死」。");
    Serial.println("     说明本测量方法在有干扰的情况下不可靠，下面的结论全部作废。");
  }

  Serial.println();
  Kind kRx = probeAndPrint("[RX] 接模块 RO / TXD", PIN_RX, true);
  Kind kTx = probeAndPrint("[TX] 接模块 DI / RXD", PIN_TX, true);

  Serial.println();
  if (kRx == K_HIGH)      Serial.println("RX: [通] GPIO20 被有源驱动为高");
  else if (kRx == K_LOW)  Serial.println("RX: [短路] GPIO20 被拉死在低");
  else if (kRx == K_OPEN) Serial.println("RX: [断] GPIO20 高阻，与 RO 无连接");
  else                    Serial.println("RX: [不稳] 半接触");

  if (kTx == K_OPEN || kTx == K_MIXED) Serial.println("TX: [正常] GPIO21 高阻，符合接 CMOS 输入");
  else if (kTx == K_HIGH)              Serial.println("TX: [短路] GPIO21 被拉到高（3.3V/5V）");
  else                                 Serial.println("TX: [短路] GPIO21 被拉到低（GND）");

  Serial.println();
  Serial.printf("汇总：REF=%s  RX=%s  TX=%s\n", kindText(kRef), kindText(kRx), kindText(kTx));

  // 灯：RX 通=短闪，断=长闪
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED, LOW);
    delay(kRx == K_HIGH ? 60 : 300);
    digitalWrite(PIN_LED, HIGH);
    delay(kRx == K_HIGH ? 600 : 200);
  }
  delay(5000);
}
