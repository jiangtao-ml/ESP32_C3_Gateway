/*
 * web_config.h —— 批次 3：常驻 SoftAP + Web 配置页 + /status + NVS 持久化（配置层）
 *
 * 事实来源：04_ai_prompts/07_Codex提示词_批次3_AP与Web配置与NVS.md
 *          02_docs/17（老师版逆向：配置页 4 卡片 / 路由 / /status 22 项 / 表单 name）
 *
 * 设计要点：
 *   - 485 采集状态机与 UDP JSON 上行【一行不改】：本文件只在其外面包一层"配置层"。
 *   - AP 常开（WiFi.mode(WIFI_AP_STA) + softAP）：SSID = ESP485-<MAC 后 6 位>（= dev）；
 *     口令 = NVS ap_pass，无值时按 MAC 派生（仅算法入代码；任何真实密码不入库/不入文档/不打串口日志）。
 *   - 路由：GET /（页面，免鉴权）· POST /save（Basic Auth + 校验 + NVS 写 + 立即生效）
 *           GET /reset（Basic Auth，清 NVS + 重启）· GET /status（22+3 项 JSON，免鉴权，绝不含密码字段）
 *   - 非阻塞：WebServer::enableDelay(false)（默认 handleClient() 会 delay(1)，违反 loop() 非阻塞红线）；
 *     WiFi.disconnect(false, false, 0)（默认带 100ms 阻塞等待）。loop() 内无 delay()。
 *   - 不引第三方库：仅 ESP32 core 自带 <WebServer.h> + <Preferences.h>。
 *   - 表单 name 采用逆向确证名 admin_user / admin_pass（老师工具 esp485_config.py 逆向确证）；
 *     其余按老师版页面逆向后缀 ssid / pass / open / chrony / server / port / period
 *     （⚠️ 待老师工具 esp485_config.py 到手后再逐字对齐）。
 *
 * ⚠️ 已知口子（照老师现状实现，M7 再收紧，见指令 §2.10 / 02_docs/17 §7）：
 *   ① AP 常开不关闭  ② /status 免鉴权  ③ UDP 明文  ④ 配置页 / 免鉴权
 *   （页面按老师版口径显示 AP 口令；该口令为运行时 NVS 值或 MAC 派生值，非代码常量。
 *     WiFi 密码与管理密码一律不回显，只显示"已设置 / 未设置 / 开放网络"。）
 */

#pragma once

#include <WebServer.h>
#include <Preferences.h>

// ================================================================
// 1. 静态资源：配置页 HTML（单页；%% = CSS 中的百分号，交给 snprintf）
// ================================================================
static const char QI_CFG_PAGE_FMT[] = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-C3 485 采集器配置</title><style>
body{font-family:system-ui,-apple-system,"Microsoft YaHei",sans-serif;margin:0;padding:16px;background:#f5f6f8;color:#222}
h1{font-size:18px;margin:0 0 4px}p.sub{font-size:12px;color:#888;margin:0 0 14px}
h2{font-size:14px;margin:0 0 6px;color:#333}
label{display:block;font-size:13px;color:#555;margin:14px 0 4px}
input[type=text],input[type=password]{width:100%%;box-sizing:border-box;font-size:16px;padding:10px;border:1px solid #cfd3d8;border-radius:6px;background:#fff}
button{width:100%%;font-size:16px;padding:12px;margin-top:18px;border:0;border-radius:6px;background:#1a7f37;color:#fff}
button.gray{background:#8a8f98;margin-top:10px}
.card{background:#fff;border-radius:8px;padding:14px 16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.kv{font-size:13px;line-height:1.9}.kv b{font-weight:600}
.msg{padding:10px 12px;border-radius:6px;background:#e7f5ea;color:#125b2b;font-size:14px;margin-bottom:12px}
.err{padding:10px 12px;border-radius:6px;background:#fdecea;color:#8a1c12;font-size:14px;margin-bottom:12px}
.tip{font-size:12px;color:#888;line-height:1.8}
.chk{font-size:13px;color:#555;margin-top:12px}
</style></head><body>
<h1>ESP32-C3 + MAX13487 采集器</h1>
<p class="sub">固件 %s &nbsp;|&nbsp; MAC %s</p>
%s
<div class="card"><div class="kv">
MAC：<b>%s</b><br>
WiFi：<b>%s</b><br>
本机 IP：<b>%s</b><br>
配置来源：<b>%s</b><br>
对时状态：<b>%s</b><br>
Modbus：<b>%s</b>
</div></div>
<form action="/save" method="post">
<div class="card">
<h2>① 接入的局域网热点</h2>
<label>WiFi 名称（SSID）——留空则保持当前配置不变</label>
<input type="text" name="ssid" value="%s" autocapitalize="off" spellcheck="false" placeholder="留空=保持当前 WiFi">
<label>WiFi 密码（当前 %s）</label>
<input type="password" name="pass" value="" autocomplete="new-password" spellcheck="false" placeholder="留空=保持原密码不变">
<div class="chk"><label style="display:inline"><input type="checkbox" name="open" value="1"%s> 该网络无密码（开放网络）</label></div>
</div>
<div class="card">
<h2>② NTP 与数据接收</h2>
<label>Chrony / NTP 服务器地址（出厂默认 %s）</label>
<input type="text" name="chrony" value="%s" inputmode="decimal" autocapitalize="off" spellcheck="false">
<label>UDP 接收程序地址（出厂默认 %s）</label>
<input type="text" name="server" value="%s" inputmode="decimal" autocapitalize="off" spellcheck="false">
<label>UDP 端口（1-65535）</label>
<input type="text" name="port" value="%u" inputmode="numeric">
</div>
<div class="card">
<h2>③ 采集周期</h2>
<label>采集周期（秒，1-5）</label>
<input type="text" name="period" value="%u" inputmode="numeric">
</div>
<div class="card">
<h2>④ 管理账号</h2>
<label>登录用户名</label>
<input type="text" name="admin_user" value="%s" autocapitalize="off" spellcheck="false">
<label>登录密码（留空=保持不变）</label>
<input type="password" name="admin_pass" value="" autocomplete="new-password" placeholder="留空=保持原密码不变">
</div>
<button type="submit">保存并立即生效</button>
</form>
<form action="/reset" method="get" onsubmit="return confirm('恢复出厂默认（清除 NVS 配置并重启）？')">
<button class="gray" type="submit">恢复出厂默认</button></form>
<p class="tip">保存后立即生效、无需重启：设备用新的 WiFi 账号重连（可能断几秒），
随即按新的 Chrony 服务器对时、把采集数据发往新的接收地址；配置写入闪存，掉电不丢。<br>
若新 WiFi 填错导致连不上，设备热点 %s（密码 %s）始终在线，
连上后打开 http://%s/ 即可改回来。<br>
串口日志与 /status 可查看状态 JSON；电脑端工具 tools/esp485_config.py 也能发现并修改本机。</p>
</body></html>)HTML";

// /status —— 老师版 22 项字段逐字对齐（顺序 / 类型一致；尾部不留 '}'，由调用方追加我方 3 项）
static const char QI_STATUS_FMT[] =
    "{\"mac\":\"%s\",\"fw\":\"%s\",\"configured\":%s,\"wifi_ssid_cfg\":\"%s\",\"wifi_from_builtin\":%s,"
    "\"chrony\":\"%s\",\"server\":\"%s\",\"port\":%u,\"poll_period_s\":%u,\"wifi_connected\":%s,"
    "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ap\":\"%s\",\"ap_ip\":\"%s\",\"time_synced\":%s,"
    "\"uptime_ms\":%lu,\"poll_ok\":%u,\"poll_err\":%u,\"udp_tx\":%u,\"queue\":null,\"dropped\":null";
// 【2026-10-27 砚青】`queue` / `dropped` 由「恒发 0」改为 **JSON null**（B 类第 6 项，口径统一）：
//   补传队列属 M6，本批次**根本没有这个结构** ⇒ 说实话就该是 null，不是 0。
//   发 0 会让接收端/报表读成「队列长度为 0、从未丢包」，属**语义造假**；
//   而心跳一直是 null（契约见 02_docs/16）⇒ 两处口径此前不一致，本次统一为 null。
//   ⚠️ 接收端如按 int 解析这两个键会抛 TypeError，须按"可空"处理（与心跳同）。
// ================================================================
// 2. Web 层状态（静态分配，非阻塞）
// ================================================================
static WebServer   g_web(80);            // 绑所有接口：SoftAP(192.168.4.1) 与 STA 本机 IP(192.168.1.200) 均可访问
static Preferences g_prefs;              // NVS 句柄（loadConfig() 打开后保持读写，供 /save 写入）
static bool        g_prefsOk   = false;
static uint32_t    g_resetAtMs = 0;      // >0 = 已接受 /reset，到点重启（先把响应发完再重启）

static char g_webPage[QI_WEB_PAGE_MAX];
static char g_statusJson[QI_STATUS_JSON_MAX];

// ================================================================
// 3. 小工具（仅本层使用）
// ================================================================
// 严格十进制解析 + 区间校验（不接受空串 / 非数字 / 溢出）
static bool parseU32Range(const String &s, uint32_t lo, uint32_t hi, uint32_t *out) {
  if (s.length() == 0 || s.length() > 9) return false;
  uint32_t v = 0;
  for (unsigned int i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10U + (uint32_t)(c - '0');
    if (v > hi) return false;
  }
  if (v < lo) return false;
  *out = v;
  return true;
}

// 合法 IPv4（UDP 接收地址必须是地址，不接受域名：beginPacket 解析域名会失败）
static bool isValidIpv4(const String &s) {
  IPAddress tmp;
  return tmp.fromString(s.c_str());
}

// 合法 IP 或主机名（chrony 允许域名，例 ntp.aliyun.com）
static bool isPlausibleHost(const String &s) {
  if (s.length() == 0 || s.length() > 63) return false;
  for (unsigned int i = 0; i < s.length(); i++) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// 默认口令派生算法（仅算法入代码；任何真实密码不入库 / 不入文档 / 不打串口日志）：
//   "485-" + MAC 后 4 位十六进制大写（例 485-9B88）—— 恰 8 字符，满足 softAP 的 >=8 字符下限
//   （APClass::create() 对 1~7 字符口令会 log_e("passphrase too short!") 并启动失败）。
static void deriveDefaultPass(char *out, size_t n) {
  uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
  WiFi.macAddress(mac);
  snprintf(out, n, "%s%02X%02X", AP_DEFAULT_PASS_PREFIX, (unsigned)mac[4], (unsigned)mac[5]);
  if (strlen(out) < (size_t)QI_AP_PASS_MIN_LEN) snprintf(out, n, "%s0000", AP_DEFAULT_PASS_PREFIX);
}

// 口令是否可用于 softAP（0 字符 = 开放网络；1~7 字符会被协议栈拒绝）
static bool apPassUsable(const char *s) {
  const size_t n = strlen(s);
  return (n == 0) || (n >= (size_t)QI_AP_PASS_MIN_LEN);
}

// ================================================================
// 4. 页面渲染
// ================================================================
static void webSendPage(int code, const char *bannerText, bool isErr) {
  char banner[320];
  if (bannerText != NULL && bannerText[0] != '\0') {
    snprintf(banner, sizeof(banner), "<div class=\"%s\">%s</div>", isErr ? "err" : "msg", bannerText);
  } else {
    banner[0] = '\0';
  }

  char wifiInfo[96];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(wifiInfo, sizeof(wifiInfo), "已连接 %s（RSSI %d dBm）",
             WiFi.SSID().c_str(), (int)WiFi.RSSI());
  } else {
    snprintf(wifiInfo, sizeof(wifiInfo), "未连接（wifiTick 每 5s 节流重连）");
  }

  // 与老师版的一处差异：老师页把 WiFi 密码回显在 label 里；本层只显示状态，不回显任何真实凭据。
  const char *passHint = g_wifiOpen ? "开放网络（无密码）" : ((g_wifiPass[0] != '\0') ? "已设置" : "未设置");

  char busInfo[96];
  snprintf(busInfo, sizeof(busInfo), "%s poll_ok=%lu poll_err=%lu udp_tx=%lu",
           (g_readMode == 0) ? "一帧读全(full)" : "3 段(3seg)",
           (unsigned long)g_pollOk, (unsigned long)g_pollErr, (unsigned long)g_udpTx);

  char staIp[20];
  snprintf(staIp, sizeof(staIp), "%s", WiFi.localIP().toString().c_str());
  char apIp[20];
  snprintf(apIp, sizeof(apIp), "%s", WiFi.softAPIP().toString().c_str());

  // 参数顺序必须与 QI_CFG_PAGE_FMT 中的 %s/%u 一一对应（22 个占位符）
  int n = snprintf(g_webPage, sizeof(g_webPage), QI_CFG_PAGE_FMT,
                   FW_VERSION, g_macStr, banner,
                   g_macStr, wifiInfo, staIp,
                   g_configured ? "NVS 已保存配置" : "内置默认（secrets.h / config.h）",
                   g_timeSynced ? "已同步" : "未同步（BOOT+ 计数）",
                   busInfo,
                   g_wifiSsid, passHint, (g_wifiOpen ? " checked" : ""),
                   NTP_SERVER_1, g_ntpPrimary,
                   UDP_TARGET_IP, g_udpTargetIp,
                   (unsigned)g_udpTargetPort,
                   (unsigned)(g_pollIntervalMs / 1000UL),
                   g_adminUser,
                   g_apSsid, g_apPass, apIp);
  if (n < 0 || (size_t)n >= sizeof(g_webPage)) {
    // 【2026-10-21 Codex】缓冲区不足时 g_webPage 只是不完整的前缀，发出去会让浏览器
    //   收到结构损坏的 HTML（缺 </html>、表单截断）。改为显式 500，不发截断内容。
    Serial.printf("[WEB][ERR] 配置页缓冲不足（需要 %d / 容量 %u），本次返回 500，不发送截断页\n",
                  n, (unsigned)sizeof(g_webPage));
    g_web.send(500, "text/plain; charset=utf-8", "config page too large");
    return;
  }
  g_web.send_P(code, "text/html; charset=utf-8", g_webPage);   // send_P：不发 String 拷贝，也不触发 "String cast failed" 告警
}

static void webSendError(const char *msg) { webSendPage(400, msg, true); }// ================================================================
// 5. 路由处理
// ================================================================
static void handleRoot() { webSendPage(200, "", false); }
static void handleNotFound() { g_web.send(404, "text/plain; charset=utf-8", "not found"); }

// GET /status —— 老师版 22 项 + 我方 3 项（dev / mode / free_heap）；绝不含任何密码字段（免鉴权口的边界）
static void handleStatus() {
  const bool wifiConn = (WiFi.status() == WL_CONNECTED);
  char ssidNow[36];
  snprintf(ssidNow, sizeof(ssidNow), "%s", wifiConn ? WiFi.SSID().c_str() : "");
  char ipNow[20];
  snprintf(ipNow, sizeof(ipNow), "%s", WiFi.localIP().toString().c_str());
  char apIp[20];
  snprintf(apIp, sizeof(apIp), "%s", WiFi.softAPIP().toString().c_str());

  // 参数顺序与 QI_STATUS_FMT 的 22 个占位符一一对应
  int n = snprintf(g_statusJson, sizeof(g_statusJson), QI_STATUS_FMT,
                   g_macStr, FW_VERSION,
                   g_configured ? "true" : "false",
                   g_wifiSsid,
                   g_wifiFromBuiltin ? "true" : "false",
                   g_ntpPrimary, g_udpTargetIp,
                   (unsigned)g_udpTargetPort,
                   (unsigned)(g_pollIntervalMs / 1000UL),
                   wifiConn ? "true" : "false",
                   ssidNow, ipNow, (int)WiFi.RSSI(),
                   g_apSsid, apIp,
                   g_timeSynced ? "true" : "false",
                   (unsigned long)millis(),
                   (unsigned)g_pollOk, (unsigned)g_pollErr, (unsigned)g_udpTx);
                   // queue / dropped 已改为格式串里的字面 null，**不再传参**（少两个 %u 占位）
  if (n <= 0 || (size_t)n >= sizeof(g_statusJson)) {
    Serial.printf("[WEB][ERR] /status 缓冲不足（需要 %d / 容量 %u）\n", n, (unsigned)sizeof(g_statusJson));
    g_web.send(500, "application/json; charset=utf-8", "{\"err\":\"status buffer too small\"}");
    return;
  }
  // 【2026-10-21 Codex】第二次追加也要检查返回值，否则余量不足时会静默截断成非法 JSON。
  int m = snprintf(g_statusJson + n, sizeof(g_statusJson) - (size_t)n,
                   ",\"dev\":\"%s\",\"mode\":\"%s\",\"free_heap\":%u}",
                   g_devStr, (g_readMode == 0) ? "full" : "3seg", (unsigned)ESP.getFreeHeap());
  if (m < 0 || (size_t)n + (size_t)m >= sizeof(g_statusJson)) {
    Serial.printf("[WEB][ERR] /status 二次追加缓冲不足（已有 %d + 需要 %d / 容量 %u）\n",
                  n, m, (unsigned)sizeof(g_statusJson));
    g_web.send(500, "application/json; charset=utf-8", "{\"err\":\"status buffer too small\"}");
    return;
  }
  g_web.send_P(200, "application/json; charset=utf-8", g_statusJson);
}

// POST /save —— Basic Auth 保护；任何校验失败都不改动配置（提示口径与老师版一致）
static void handleSave() {
  if (!g_web.authenticate(g_adminUser, g_adminPass)) {
    g_web.requestAuthentication(BASIC_AUTH, NULL, String("需要管理账号登录"));
    return;
  }

  const String vSsid = g_web.arg("ssid");         // 空 = 保持当前
  const String vPass = g_web.arg("pass");         // 空 = 保持原密码
  const bool   vOpen = g_web.hasArg("open");      // 勾选 = 开放网络
  const String vChr  = g_web.arg("chrony");
  const String vSrv  = g_web.arg("server");
  const String vPort = g_web.arg("port");
  const String vPer  = g_web.arg("period");
  const String vUser = g_web.arg("admin_user");
  const String vApw  = g_web.arg("admin_pass");   // 空 = 保持不变

  // ---- 校验（先全校验、后落盘：失败即"未改动配置"）----
  uint32_t newPort = (uint32_t)g_udpTargetPort;
  uint32_t newPer  = g_pollIntervalMs / 1000UL;
  if (!parseU32Range(vPort, 1, 65535, &newPort))                             { webSendError("invalid port (1-65535)"); return; }
  if (!parseU32Range(vPer, POLL_PERIOD_MIN_S, POLL_PERIOD_MAX_S, &newPer))   { webSendError("poll period (1-5s)"); return; }
  if (vChr.length() > 0 && !isPlausibleHost(vChr))                           { webSendError("不一致或非法, 未改动配置"); return; }
  if (vSrv.length() > 0 && !isValidIpv4(vSrv))                               { webSendError("不一致或非法, 未改动配置"); return; }
  if (vSsid.length() > 32 || vUser.length() > 32 ||
      vPass.length() > 63 || vApw.length() > 63)                             { webSendError("不一致或非法, 未改动配置"); return; }

  // ---- 逐项生效（NVS + 运行时变量；保存后立即生效，无需重启）----
  bool wifiChanged = false;
  if (vSsid.length() > 0) {
    const bool   newOpen = vOpen;
    const String newPass = newOpen ? String("") : ((vPass.length() > 0) ? vPass : String(g_wifiPass));
    if (vSsid != String(g_wifiSsid) || newPass != String(g_wifiPass) || newOpen != g_wifiOpen) {
      vSsid.toCharArray(g_wifiSsid, sizeof(g_wifiSsid));
      newPass.toCharArray(g_wifiPass, sizeof(g_wifiPass));
      g_wifiOpen        = newOpen;
      wifiChanged       = true;
      g_wifiFromBuiltin = false;
      g_configured      = true;
      if (g_prefsOk) {
        g_prefs.putString(NVS_KEY_WIFI_SSID, g_wifiSsid);
        g_prefs.putBool(NVS_KEY_WIFI_OPEN, g_wifiOpen);
        if (g_wifiOpen) g_prefs.remove(NVS_KEY_WIFI_PASS);
        else            g_prefs.putString(NVS_KEY_WIFI_PASS, g_wifiPass);
      }
    }
  }

  if (vSrv.length() > 0 && vSrv != String(g_udpTargetIp)) {
    vSrv.toCharArray(g_udpTargetIp, sizeof(g_udpTargetIp));
    g_configured = true;
    if (g_prefsOk) g_prefs.putString(NVS_KEY_UDP_IP, g_udpTargetIp);
    Serial.printf("[CFG] UDP 接收地址 -> %s（下一包立即生效）\n", g_udpTargetIp);
  }
  if (newPort != (uint32_t)g_udpTargetPort) {
    g_udpTargetPort = (uint16_t)newPort;
    g_configured = true;
    if (g_prefsOk) g_prefs.putUInt(NVS_KEY_UDP_PORT, newPort);
    Serial.printf("[CFG] UDP 端口 -> %u（下一包立即生效）\n", (unsigned)g_udpTargetPort);
  }
  if (newPer * 1000UL != g_pollIntervalMs) {
    g_pollIntervalMs = newPer * 1000UL;
    g_configured = true;
    if (g_prefsOk) g_prefs.putUInt(NVS_KEY_POLL_S, newPer);
    Serial.printf("[CFG] 采集周期 -> %lu s（下一轮节拍生效）\n", (unsigned long)newPer);
  }
  if (vChr.length() > 0 && vChr != String(g_ntpPrimary)) {
    vChr.toCharArray(g_ntpPrimary, sizeof(g_ntpPrimary));
    g_configured = true;
    if (g_prefsOk) g_prefs.putString(NVS_KEY_NTP1, g_ntpPrimary);
    g_ntpResyncPending = true;                 // 下一轮 ntpTick() 用新主源重新 configTzTime（备源 NTP_SERVER_2/3 不变）
    Serial.printf("[CFG] NTP 主源 -> %s（重新触发 SNTP）\n", g_ntpPrimary);
  }
  if (vUser.length() > 0 && vUser != String(g_adminUser)) {
    vUser.toCharArray(g_adminUser, sizeof(g_adminUser));
    g_configured = true;
    if (g_prefsOk) g_prefs.putString(NVS_KEY_ADMIN_USER, g_adminUser);
    Serial.println("[CFG] 管理账号用户名已更新");
  }
  if (vApw.length() > 0) {
    vApw.toCharArray(g_adminPass, sizeof(g_adminPass));
    g_configured = true;
    if (g_prefsOk) g_prefs.putString(NVS_KEY_ADMIN_PASS, g_adminPass);
    Serial.println("[CFG] 管理账号密码已更新（不回显）");
  }

  // ---- WiFi 凭据变更：立即断开并用新凭据重连（断连期间上行由 g_linkUp 逻辑自然跳过）----
  if (wifiChanged) {
    Serial.printf("[CFG] WiFi 凭据已更新 -> 立即重连（SSID=%s，%s，凭据不回显）\n",
                  g_wifiSsid, g_wifiOpen ? "开放网络" : "密码已设置");
    WiFi.disconnect(false, false, 0);          // timeoutLength=0：不阻塞（默认 100ms 会违反非阻塞红线）
    g_everConnected    = false;                // 让 wifiTick() 首个动作走 WiFi.begin(新凭据) 而不是 reconnect()
    g_ntpResyncPending = true;                 // 重连成功后重新对时
    WiFi.begin(g_wifiSsid, g_wifiOpen ? (const char *)nullptr : g_wifiPass);
  }

  Serial.printf("[CFG] 配置已保存并立即生效（来源=%s）\n", g_configured ? "NVS" : "内置默认");
  webSendPage(200, "配置已保存并立即生效（无需重启）", false);
}

// GET /reset —— Basic Auth 保护；清 NVS 后重启（响应先发完，1.2s 后再重启，避免掐断 HTTP 响应）
static void handleReset() {
  if (!g_web.authenticate(g_adminUser, g_adminPass)) {
    g_web.requestAuthentication(BASIC_AUTH, NULL, String("需要管理账号登录"));
    return;
  }
  if (g_prefsOk) g_prefs.clear();
  Serial.println("[CFG] 已恢复出厂默认（NVS 命名空间 cfg485 已清空），1.2s 后重启");
  g_web.send(200, "text/html; charset=utf-8",
             "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>已恢复出厂默认</title></head><body style=\"font-family:system-ui;padding:16px\">"
             "<h3>已恢复出厂默认</h3><p>NVS 配置已清空，设备正在重启…</p></body></html>");
  g_resetAtMs = millis() + 1200;
}

// ================================================================
// 6. 配置装载 / 启停
// ================================================================
// NVS -> 运行时变量；无值回落编译期默认（secrets.h 的 WiFi、config.h 的 NTP/UDP/周期）
static void loadConfig() {
  uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
  WiFi.macAddress(mac);

  // AP SSID = 前缀 + MAC 后 6 位（与 dev 一致）；默认口令 = "485-" + MAC 后 4 位（>=8 字符，满足 softAP 下限）
  snprintf(g_apSsid, sizeof(g_apSsid), "%s%02X%02X%02X",
           AP_DEFAULT_SSID_PREFIX, (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
  deriveDefaultPass(g_apPass, sizeof(g_apPass));
  snprintf(g_adminPass, sizeof(g_adminPass), "%s", g_apPass);   // 管理密码默认与 AP 口令同源（仅算法）

  g_prefsOk = g_prefs.begin(NVS_NAMESPACE, false);
  if (!g_prefsOk) {
    Serial.println("[CFG][WARN] NVS 命名空间打开失败，本次全部使用编译期默认（配置无法保存）");
    return;
  }

  g_configured = g_prefs.isKey(NVS_KEY_WIFI_SSID) || g_prefs.isKey(NVS_KEY_WIFI_PASS) ||
                 g_prefs.isKey(NVS_KEY_WIFI_OPEN) || g_prefs.isKey(NVS_KEY_NTP1)      ||
                 g_prefs.isKey(NVS_KEY_UDP_IP)    || g_prefs.isKey(NVS_KEY_UDP_PORT)  ||
                 g_prefs.isKey(NVS_KEY_POLL_S)    || g_prefs.isKey(NVS_KEY_ADMIN_USER) ||
                 g_prefs.isKey(NVS_KEY_ADMIN_PASS) || g_prefs.isKey(NVS_KEY_AP_PASS);

  char tmp[96];
  if (g_prefs.getString(NVS_KEY_WIFI_SSID, tmp, sizeof(tmp)) > 0 && tmp[0] != '\0') {
    snprintf(g_wifiSsid, sizeof(g_wifiSsid), "%s", tmp);
    g_wifiFromBuiltin = false;    // 口径：NVS 未存 wifi_ssid 即视为"来自内置默认（secrets.h）"
  }
  if (g_prefs.getString(NVS_KEY_WIFI_PASS, tmp, sizeof(tmp)) > 0) {
    snprintf(g_wifiPass, sizeof(g_wifiPass), "%s", tmp);
  }
  g_wifiOpen = g_prefs.getBool(NVS_KEY_WIFI_OPEN, g_wifiOpen);
  if (g_wifiOpen) g_wifiPass[0] = '\0';

  if (g_prefs.getString(NVS_KEY_NTP1, tmp, sizeof(tmp)) > 0 && tmp[0] != '\0') {
    snprintf(g_ntpPrimary, sizeof(g_ntpPrimary), "%s", tmp);
  }
  if (g_prefs.getString(NVS_KEY_UDP_IP, tmp, sizeof(tmp)) > 0 && tmp[0] != '\0') {
    snprintf(g_udpTargetIp, sizeof(g_udpTargetIp), "%s", tmp);
  }
  uint32_t p = g_prefs.getUInt(NVS_KEY_UDP_PORT, 0U);
  if (p >= 1U && p <= 65535U) g_udpTargetPort = (uint16_t)p;
  uint32_t s = g_prefs.getUInt(NVS_KEY_POLL_S, 0U);
  if (s >= POLL_PERIOD_MIN_S && s <= POLL_PERIOD_MAX_S) g_pollIntervalMs = s * 1000UL;

  if (g_prefs.getString(NVS_KEY_ADMIN_USER, tmp, sizeof(tmp)) > 0 && tmp[0] != '\0') {
    snprintf(g_adminUser, sizeof(g_adminUser), "%s", tmp);
  }
  if (g_prefs.getString(NVS_KEY_ADMIN_PASS, tmp, sizeof(tmp)) > 0 && tmp[0] != '\0') {
    snprintf(g_adminPass, sizeof(g_adminPass), "%s", tmp);
  }
  if (g_prefs.getString(NVS_KEY_AP_PASS, tmp, sizeof(tmp)) >= (size_t)QI_AP_PASS_MIN_LEN) {
    snprintf(g_apPass, sizeof(g_apPass), "%s", tmp);      // <8 字符会让 softAP 失败，忽略之（用派生口令）
  }
  // NVS 句柄保持打开（readOnly=false），/save 直接写入，无需重开

  Serial.printf("[CFG] 配置来源=%s WiFi_SSID=%s WiFi密码=%s NTP主源=%s UDP目标=%s:%u 周期=%lu s "
                "（任何真实密码一律不回显）\n",
                g_configured ? "NVS" : "内置默认",
                g_wifiSsid, g_wifiOpen ? "无(开放网络)" : ((g_wifiPass[0] != '\0') ? "已设置" : "未设置"),
                g_ntpPrimary, g_udpTargetIp, (unsigned)g_udpTargetPort,
                (unsigned long)(g_pollIntervalMs / 1000UL));
}

// 常驻 SoftAP + Web 路由启动（必须在 WiFi STA 配置之后调用）
static void webSetup() {
  // ⚠️ 必须：WebServer::handleClient() 默认 _nullDelay=true 会 delay(1)，违反 loop() 非阻塞红线
  g_web.enableDelay(false);
  g_web.on("/",       HTTP_GET,  handleRoot);
  g_web.on("/save",   HTTP_POST, handleSave);
  g_web.on("/reset",  HTTP_GET,  handleReset);
  g_web.on("/status", HTTP_GET,  handleStatus);
  g_web.onNotFound(handleNotFound);
  g_web.begin();

  if (!apPassUsable(g_apPass)) deriveDefaultPass(g_apPass, sizeof(g_apPass));   // 防御：非法长度退回 MAC 派生
  const bool apOk = WiFi.softAP(g_apSsid, g_apPass);
  Serial.println("[WEB] 路由就绪：GET / · POST /save · GET /reset · GET /status");
  Serial.printf("[WEB] 常驻 SoftAP %s SSID=%s（口令 %u 字符，按红线不回显）AP_IP=%s\n",
                apOk ? "已启动" : "启动失败", g_apSsid, (unsigned)strlen(g_apPass),
                WiFi.softAPIP().toString().c_str());
  Serial.printf("[WEB] STA 侧 http://%s/ 与 AP 侧 http://%s/ 均可访问"
                "（GET / 与 /status 免鉴权；/save 与 /reset 需 Basic Auth）\n",
                LOCAL_IP, WiFi.softAPIP().toString().c_str());
}

// loop() 挂载点：非阻塞；顺带处理 /reset 的延迟重启（先把 HTTP 响应发完）
static void webTick() {
  g_web.handleClient();
  if (g_resetAtMs != 0 && (int32_t)(millis() - g_resetAtMs) >= 0) {
    g_resetAtMs = 0;
    ESP.restart();
  }
}
