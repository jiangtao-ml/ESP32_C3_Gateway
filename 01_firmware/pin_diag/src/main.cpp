// ============================================================================
//  pin_diag —— 单独测 GPIO21 / W4 到底坏没坏（不需要万用表）
//
//  为什么需要它：
//    业务固件跑起来只能看到「总线静默」这一个现象，而它有三种成因
//    （W4 断 / GPIO21 输出级坏 / 模块发送侧坏），现象完全一致。
//    本固件把这三件事拆成三个可分辨的测量。
//
//  三个测试：
//    T1 自驱 readback —— GPIO21 设为 OUTPUT 输出高/低，再读回自己的 PAD 电平。
//         ESP32 的 digitalRead() 读的是输入寄存器，对输出脚读的是**实际引脚电平**
//         ⇒ 能验「输出级是否真的把电平拉出去了」。
//         输出高读回低 = 被外部强下拉(短 GND) 或 输出级拉不高
//         输出低读回高 = 被外部强上拉(短 3V3/5V) 或 输出级拉不低
//
//    T2 弱偏置三态 —— 内部 45k 上拉/下拉各读一次，查**短路**（比推挽更灵敏，能查几十 kΩ 漏电）
//         下拉读到高 = 外部强上拉(短 3V3/5V)；上拉读到低 = 外部强下拉(短 GND)
//         两者都正常 = 外部是纯高阻(模块 DI 正常形态) —— 但**无法区分断路**
//
//    T3 低频方波看总线回声 —— ★ 唯一能验 W4 导通的零焊接手段 ★
//         GPIO21 输出 1 Hz 方波（500 ms 高 / 500 ms 低）。
//         若 W4 通：模块 DI 收到边沿 -> AutoDirection 驱动 A/B -> 总线被拉到对应电平
//                   -> 模块 RO 输出 -> GPIO20 读到**跟随**电平
//         若 W4 断：DI 不动 -> 总线无变化 -> GPIO20 恒高（空闲）
//         每个半周期末尾用「浮空 / 上拉 / 下拉」三种读法各读一次，
//         以区分「被推挽驱动(有效)」与「RO 被禁用而浮空(无效)」。
//
//  安全：只驱动 GPIO21（→ 模块 DI）与 LED(GPIO8)。不动 EN，不发任何 Modbus 帧。
//        前提：真机 485 线已拔掉，仅 USB 供电；桌面台面测试。
// ============================================================================

#include <Arduino.h>
#include <driver/uart.h>

static const int PIN_RX  = 20;   // 接模块 RO / TXD
static const int PIN_TX  = 21;   // 接模块 DI / RXD（= W4 的另一端）
static const int PIN_REF = 10;   // 对照脚：板上未连接，应恒为「外部高阻」
static const int PIN_LED = 8;    // 板载蓝灯，低电平点亮

// ---------- 三态读（区分「被推挽驱动」与「浮空」） ----------
struct Tri {
  int up;    // 内部上拉时读到的电平
  int dn;    // 内部下拉时读到的电平
};

static Tri readTri(int pin, int settle_ms) {
  pinMode(pin, INPUT_PULLUP);
  delay(settle_ms);
  Tri t;
  t.up = digitalRead(pin) ? 1 : 0;
  pinMode(pin, INPUT_PULLDOWN);
  delay(settle_ms);
  t.dn = digitalRead(pin) ? 1 : 0;
  return t;
}

static const char *triText(const Tri &t) {
  if (t.up == 1 && t.dn == 0) return "浮空/高阻（无人驱动）";
  if (t.up == 1 && t.dn == 1) return "被强驱动 -> 高（外部低阻上拉）";
  if (t.up == 0 && t.dn == 0) return "被强驱动 -> 低（外部低阻下拉）";
  return "读数不稳（半接触 / 抖动）";
}

// ---------- T1：自驱 readback ----------
static void test1_selfDrive() {
  Serial.println("\n---- T1  GPIO21 输出能力（自驱 + 读回）----");
  pinMode(PIN_TX, OUTPUT);
  digitalWrite(PIN_TX, HIGH);
  delay(3);
  int a = digitalRead(PIN_TX) ? 1 : 0;
  Serial.printf("  输出 HIGH -> 读回 %d\n", a);

  digitalWrite(PIN_TX, LOW);
  delay(3);
  int b = digitalRead(PIN_TX) ? 1 : 0;
  Serial.printf("  输出 LOW  -> 读回 %d\n", b);

  pinMode(PIN_TX, INPUT);   // 用完立刻放回高阻，避免干扰后面的测量

  if (a == 1 && b == 0) {
    Serial.println("  判定：✅ 输出级正常，且未被短路到电源/地");
    Serial.println("        ⇒ 排除「GPIO21 被 5V/3.3V/GND 短路」与「输出级烧死」");
    Serial.println("        ⇒ 但**仍不能证明 W4 通**，看 T3");
  } else if (a == 0) {
    Serial.println("  判定：❌ 输出 HIGH 却被读成 LOW —— 被外部强下拉，或输出级拉不高");
    Serial.println("        ⇒ 高度怀疑 GPIO21 短到 GND（查 W4 与 GND 锡条是否碰锡）");
  } else {
    Serial.println("  判定：❌ 输出 LOW 却被读成 HIGH —— 被外部强上拉，或输出级拉不低");
    Serial.println("        ⇒ 高度怀疑 GPIO21 短到 3.3V/5V（查 F-014 / G-014 交叉点）");
  }
}

// ---------- T2：短路检测 ----------
static void test2_shortCheck() {
  Serial.println("\n---- T2  GPIO21 弱偏置三态（查短路）----");
  Tri t = readTri(PIN_TX, 12);
  Serial.printf("  上拉读 %d / 下拉读 %d  => %s\n", t.up, t.dn, triText(t));
  if (t.up == 1 && t.dn == 0) {
    Serial.println("  判定：✅ 外部纯高阻（模块 DI 的正常形态）—— 无短路");
  } else {
    Serial.println("  判定：❌ 有低阻源在驱动 —— 短路，按上面方向查对应电源轨/地");
  }
}

// ---------- T3：低频方波看总线回声（验 W4） ----------
static void test3_busEcho() {
  Serial.println("\n---- T3  GPIO21 1Hz 方波 -> 看 GPIO20 是否跟随（验 W4）----");
  Serial.println("  （共 6 个半周期，每个 500ms；末尾三种读法各采一次）");

  pinMode(PIN_TX, OUTPUT);
  int hi_driven = 0, hi_total = 0, lo_driven = 0, lo_total = 0;

  for (int i = 0; i < 6; i++) {
    int level = (i % 2 == 0) ? HIGH : LOW;
    digitalWrite(PIN_TX, level);
    delay(500);

    Tri t = readTri(PIN_RX, 12);
    bool driven = !(t.up == 1 && t.dn == 0);
    int val = driven ? t.up : -1;   // -1 = 浮空（RO 被禁用）

    Serial.printf("  #%d TX=%s | RX: 上拉 %d / 下拉 %d -> %s\n",
                  i + 1,
                  level == HIGH ? "HIGH" : "LOW ",
                  t.up, t.dn,
                  driven ? (val ? "驱动为高" : "驱动为低") : "浮空");

    if (level == HIGH) { hi_total++; if (driven && val == 1) hi_driven++; }
    else               { lo_total++; if (driven && val == 0) lo_driven++; }
  }

  digitalWrite(PIN_TX, HIGH);
  pinMode(PIN_TX, INPUT);

  Serial.printf("\n  汇总：TX=HIGH 时 RX 跟随为高 %d/%d ；TX=LOW 时 RX 跟随为低 %d/%d\n",
                hi_driven, hi_total, lo_driven, lo_total);

  if (hi_driven >= 2 && lo_driven >= 2) {
    Serial.println("  判定：✅✅ GPIO20 电平**跟随** GPIO21 —— W4 导通、模块在驱动总线");
    Serial.println("        ⇒ 若此时业务固件仍发不出去，问题不在硬件层，查固件时序/波特率");
  } else if (hi_driven + lo_driven == 0) {
    Serial.println("  判定：❌ 完全无跟随 —— W4 断路（或模块发送侧坏 / DI 未接到）");
    Serial.println("        ⇒ 与业务固件「总线静默」一致。W4 或模块二者之一，需飞线二分");
  } else {
    Serial.println("  判定：⚠️ 部分跟随 —— 半接触/虚焊特征（漆包线假焊的典型形态）");
    Serial.println("        ⇒ 大概率 W4 假焊：焊点看着饱满但漆皮隔断，间歇导通");
  }
}

// ---------- 对照脚 ----------
static void test0_reference() {
  Serial.println("\n---- T0  对照脚 GPIO10（板上未接，尺子准不准）----");
  Tri t = readTri(PIN_REF, 12);
  Serial.printf("  上拉读 %d / 下拉读 %d  => %s\n", t.up, t.dn, triText(t));
  if (t.up == 1 && t.dn == 0) {
    Serial.println("  判定：✅ 尺子准 —— 悬空脚被正确读成高阻，上面各判读可信");
  } else {
    Serial.println("  判定：❌ 尺子不准 —— 悬空脚竟被读成有源驱动，本轮结论作废");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1600);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  Serial.println();
  Serial.println("############################################################");
  Serial.println("#  pin_diag —— GPIO21 / W4 单点诊断（无需万用表）");
  Serial.println("#  前提：真机 485 线已拔掉；仅 USB 供电；桌面台面");
  Serial.println("############################################################");

  if (uart_is_driver_installed(UART_NUM_0)) {
    uart_driver_delete(UART_NUM_0);
    Serial.println("[警告] UART0 驱动曾安装并占用 GPIO20/21 —— 已强制释放");
  } else {
    Serial.println("[通过] UART0 未占用 GPIO20/21");
  }
}

void loop() {
  static int round = 0;
  round++;
  Serial.printf("\n========== 第 %d 轮 ==========\n", round);

  test0_reference();
  test1_selfDrive();
  test2_shortCheck();
  test3_busEcho();

  Serial.println("\n>>> 本轮结束，5 s 后重测（换接线后可看变化）\n");
  delay(5000);
}
