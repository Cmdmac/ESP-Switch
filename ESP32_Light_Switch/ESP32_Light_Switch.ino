/*
  ESP32-C2 / ESP32-C3 Smart Light Switch
  ======================================
  - Wi-Fi AP mode for direct connection (no router needed)
  - Web UI: on/off, brightness slider, ambient light display, current display
  - Daily schedule: auto turn on/off at configured times
  - Persist settings to flash

  Hardware (default pin mapping):
  - GPIO2 : lamp PWM output (brightness control)
  - GPIO3 : ambient light sensor (ADC)
  - GPIO4 : current sense amplifier output (ADC)
  - GPIO9 : on-board BOOT button (active LOW; separate from the lamp pin)

  NOTE: No separate status LED on this board — the PWM pin drives the lamp directly.
        The BOOT button is on GPIO9 (NOT GPIO2). If you wire a local control button,
        use a free GPIO; never put RESET_BUTTON_PIN on GPIO2 (it drives the lamp).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Update.h>          // 网页 OTA 固件升级

// ==================== 硬件引脚配置 ====================
// 根据实际 PCB 修改以下宏。C2/C3 的 GPIO3/GPIO4 都支持 ADC。
#define LIGHT_PWM_PIN           2     // 灯 PWM 输出（直接驱动灯，板载无独立状态 LED）
#define STATUS_LED_PIN          (-1)  // 独立状态 LED；本板无此功能，设为 -1 禁用
#define AMBIENT_LIGHT_ADC_PIN   3     // 光敏检测
#define CURRENT_ADC_PIN         4     // 电流检测
#define BUTTON_PIN              (-1)  // 本地按键引脚；若与 LIGHT_PWM_PIN 冲突请设为 -1

// 长按重置（恢复出厂）按键：接一个对地按键（内部上拉，按下为 LOW）
// 持续按住 RESET_HOLD_MS 毫秒后，清空所有设置并重启回热点模式
// 注意：板载 BOOT 键在 GPIO9，与灯 PWM(GPIO2) 不冲突。
//       请勿将 RESET_BUTTON_PIN 设为 GPIO2（那是灯 PWM 输出）。
//       可接空闲 GPIO（如 GPIO5/6/7/8/10），或复用板载 GPIO9 BOOT 键
//       （注意 GPIO9 也是 strapping 脚，长按可能触发下载模式）。
#define RESET_BUTTON_PIN        (-1)  // 默认禁用；启用时改为对应 GPIO
#define RESET_HOLD_MS           3000  // 长按触发时长(ms)

// ==================== PWM 配置 ====================
#define PWM_FREQ        5000
#define PWM_RESOLUTION  10            // 占空比范围 0-1023
#define PWM_MAX_DUTY    1023

// ==================== 电流检测校准 ====================
// 原理图电流检测部分：
// - U4 : INA180A1IDBVR  电流检测放大器，固定增益 20 V/V
// - R83: 150 mΩ         高侧分流电阻（位于 VCC 与 USB 输出 VOUT 之间）
// 公式：电流(A) = ADC 电压(V) / (分流电阻(Ω) * 放大器增益)
// 例如：Rshunt=150mΩ=0.15Ω, Gain=20 => 1A 时输出 3.0V，满量程约 1.1A
#define SHUNT_RESISTANCE_MILLIOHM  150.0f
#define CURRENT_SENSE_GAIN         20.0f

// ==================== Wi-Fi AP 配置 ====================
#define AP_SSID         "ESP-Light-Switch"
#define AP_PASSWORD     "12345678"    // 至少 8 位
#define AP_CHANNEL      1
#define AP_MAX_CLIENTS  4
#define DEVICE_HOSTNAME     "esp-light-switch"
#define WIFI_CONNECT_TIMEOUT_MS  20000  // STA 连接超时，超时后回退到热点

// ==================== 全局对象 ====================
WebServer server(80);
Preferences prefs;

// 网络
String wifiSsid = "";
String wifiPassword = "";
bool staMode = false;          // true=已连上局域网(STA)，false=热点(AP)模式
String currentIP = "";

// 运行状态
bool lightOn = false;
uint8_t brightnessPercent = 50;     // 0-100
bool scheduleEnabled = false;
String onTimeStr = "07:00";
String offTimeStr = "23:00";

// 本地按键
bool lastButtonState = HIGH;
unsigned long lastDebounce = 0;
const unsigned long DEBOUNCE_MS = 50;

// 长按重置按键
#if RESET_BUTTON_PIN > 0
bool resetBtnHeld = false;
unsigned long resetHoldStart = 0;
#endif

// 定时器
uint16_t onTimeMinutes = 7 * 60;
uint16_t offTimeMinutes = 23 * 60;

// 电流采样平滑
float currentSmoothed = 0.0f;
const float CURRENT_ALPHA = 0.2f;

// LEDC API 兼容性
#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define LEDC_NEW_API
#endif

// ==================== Web 页面 ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP 智能灯控</title>
<style>
  :root { --primary: #2196F3; --bg: #f5f5f5; --card: #fff; --text: #333; }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg); color: var(--text); }
  .container { max-width: 480px; margin: 0 auto; padding: 20px; }
  h1 { text-align: center; font-size: 1.4rem; margin-bottom: 6px; }
  .subtitle { text-align: center; color: #888; font-size: 0.85rem; margin-bottom: 20px; }
  .card { background: var(--card); border-radius: 16px; padding: 18px; margin-bottom: 16px; box-shadow: 0 2px 8px rgba(0,0,0,0.06); }
  .row { display: flex; justify-content: space-between; align-items: center; margin: 14px 0; }
  .row:first-child { margin-top: 0; }
  .row:last-child { margin-bottom: 0; }
  .label { font-size: 0.9rem; color: #555; }
  .value { font-size: 1.05rem; color: #111; font-weight: 500; }
  .switch { position: relative; display: inline-block; width: 56px; height: 30px; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: #ccc; border-radius: 30px; transition: .3s; }
  .slider:before { position: absolute; content: ""; height: 22px; width: 22px; left: 4px; bottom: 4px; background: white; border-radius: 50%; transition: .3s; }
  input:checked + .slider { background: var(--primary); }
  input:checked + .slider:before { transform: translateX(26px); }
  input[type=range] { width: 100%; margin: 10px 0; }
  .metric { font-size: 1.7rem; font-weight: 600; color: var(--primary); }
  .metric-label { font-size: 0.8rem; color: #888; }
  input[type=time] { border: 1px solid #ddd; border-radius: 8px; padding: 6px; font-size: 1rem; }
  button { width: 100%; padding: 14px; border: none; border-radius: 12px; background: var(--primary); color: white; font-size: 1rem; cursor: pointer; transition: opacity .2s; }
  button:active { opacity: 0.85; }
  .note { font-size: 0.75rem; color: #999; margin-top: 10px; text-align: center; }
  .tabs { display: flex; border-bottom: 2px solid #e6e6e6; margin-bottom: 18px; }
  .tab { flex: 1; text-align: center; padding: 12px 4px 11px; color: #999; font-size: 0.95rem; cursor: pointer; border-bottom: 3px solid transparent; margin-bottom: -2px; transition: color .2s, border-color .2s; user-select: none; }
  .tab.active { color: var(--primary); font-weight: 600; border-bottom-color: var(--primary); }
  .tab-content { display: none; }
  .tab-content.active { display: block; }
</style>
</head>
<body>
<div class="container">
  <h1>智能灯控</h1>
  <div class="subtitle">ESP Light Switch</div>

  <div class="tabs">
    <div class="tab active" id="tabBtn-control" onclick="switchTab('control')">控制</div>
    <div class="tab" id="tabBtn-network" onclick="switchTab('network')">网络</div>
    <div class="tab" id="tabBtn-ota" onclick="switchTab('ota')">升级</div>
  </div>

  <div class="tab-content active" id="tab-control">
    <div class="card">
      <div class="row">
        <span class="label">电源开关</span>
        <label class="switch">
          <input type="checkbox" id="power" onchange="setPower()">
          <span class="slider"></span>
        </label>
      </div>
    </div>

    <div class="card">
      <div class="row">
        <span class="label">亮度</span>
        <span class="value" id="brightnessVal">50%</span>
      </div>
      <input type="range" id="brightness" min="0" max="100" value="50" oninput="updateBrightnessLabel()" onchange="setBrightness()">
    </div>

    <div class="card">
      <div class="row">
        <div>
          <div class="metric-label">环境亮度</div>
          <div class="metric" id="ambient">--</div>
        </div>
        <div style="text-align:right;">
          <div class="metric-label">实时电流</div>
          <div class="metric" id="current">--</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="row">
        <span class="label">定时开关</span>
        <label class="switch">
          <input type="checkbox" id="scheduleEn" onchange="saveSchedule()">
          <span class="slider"></span>
        </label>
      </div>
      <div class="row">
        <span class="label">开启时间</span>
        <input type="time" id="onTime" onchange="saveSchedule()">
      </div>
      <div class="row">
        <span class="label">关闭时间</span>
        <input type="time" id="offTime" onchange="saveSchedule()">
      </div>
    </div>

    <button onclick="saveSettings()">保存灯光设置到 Flash</button>
  </div>

  <div class="tab-content" id="tab-network">
    <div class="card">
      <div class="row">
        <span class="label">网络模式</span>
        <span class="value" id="netMode">--</span>
      </div>
      <div class="row">
        <span class="label">当前地址</span>
        <span class="value" id="netIp">--</span>
      </div>
      <div class="row">
        <span class="label">Wi-Fi 名称</span>
        <input type="text" id="wifiSsid" placeholder="SSID" style="border:1px solid #ddd;border-radius:8px;padding:6px;font-size:1rem;width:55%;">
      </div>
      <div class="row">
        <span class="label">密码</span>
        <input type="password" id="wifiPassword" placeholder="留空不修改" style="border:1px solid #ddd;border-radius:8px;padding:6px;font-size:1rem;width:55%;">
      </div>
      <button type="button" onclick="saveWiFi()">保存并连接 Wi-Fi</button>
      <div class="note"><a href="#" onclick="forgetWiFi();return false;" style="color:#888;">忘记 Wi-Fi，恢复热点模式</a></div>
    </div>
  </div>

  <div class="tab-content" id="tab-ota">
    <div class="card">
      <div class="label" style="margin-bottom:10px;">固件升级（OTA）</div>
      <input type="file" id="firmware" accept=".bin" style="width:100%;font-size:0.9rem;margin-bottom:10px;">
      <button type="button" onclick="uploadFirmware()">选择 .bin 并升级</button>
      <div class="note" id="updateStatus"></div>
    </div>
  </div>

  <div class="note" id="status">正在连接设备...</div>
</div>

<script>
const $ = id => document.getElementById(id);
function updateBrightnessLabel() { $('brightnessVal').innerText = $('brightness').value + '%'; }
function switchTab(name) {
  ['control','network','ota'].forEach(t => {
    $('tab-' + t).classList.toggle('active', t === name);
    $('tabBtn-' + t).classList.toggle('active', t === name);
  });
  window.scrollTo(0, 0);
}

async function setPower() {
  await fetch('/api/power?state=' + ($('power').checked ? 'on' : 'off'), {method:'POST'});
  refresh();
}

async function setBrightness() {
  await fetch('/api/brightness?value=' + $('brightness').value, {method:'POST'});
  refresh();
}

async function saveSchedule() {
  const en = $('scheduleEn').checked ? 1 : 0;
  const url = '/api/schedule?enabled=' + en + '&on=' + $('onTime').value + '&off=' + $('offTime').value;
  await fetch(url, {method:'POST'});
  refresh();
}

async function saveSettings() {
  await fetch('/api/save', {method:'POST'});
  $('status').innerText = '设置已保存到 Flash';
  setTimeout(() => $('status').innerText = '', 2000);
}

async function saveWiFi() {
  const ssid = $('wifiSsid').value;
  const pwd = $('wifiPassword').value;
  if (!ssid) { alert('请填写 Wi-Fi 名称'); return; }
  await fetch('/api/wifi?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pwd), {method:'POST'});
  $('status').innerText = '已保存，正在重启连接 Wi-Fi...';
}

async function forgetWiFi() {
  if (!confirm('确定恢复热点模式？设备将断开局域网。')) return;
  await fetch('/api/forgetwifi', {method:'POST'});
  $('status').innerText = '正在重启进入热点模式...';
}

async function uploadFirmware() {
  const f = $('firmware').files[0];
  if (!f) { alert('请先选择固件 .bin 文件'); return; }
  const fd = new FormData();
  fd.append('firmware', f);
  $('updateStatus').innerText = '正在上传固件，请勿断电...';
  try {
    const r = await fetch('/update', { method: 'POST', body: fd });
    const t = await r.text();
    $('updateStatus').innerText = t + '（设备将自动重启）';
  } catch(e) {
    $('updateStatus').innerText = '升级失败：' + e.message;
  }
}

async function refresh() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    $('power').checked = d.on;
    $('brightness').value = d.brightness;
    $('brightnessVal').innerText = d.brightness + '%';
    $('ambient').innerText = d.ambient + '%';
    $('current').innerText = d.current + 'A';
    $('scheduleEn').checked = d.scheduleEnabled;
    $('onTime').value = d.onTime;
    $('offTime').value = d.offTime;
    $('netMode').innerText = (d.mode === 'sta') ? '局域网' : '热点';
    $('netIp').innerText = d.ip;
    if (!$('wifiSsid').value) $('wifiSsid').value = d.wifiSsid;
    const upH = Math.floor(d.uptime / 3600).toString().padStart(2,'0');
    const upM = Math.floor((d.uptime % 3600) / 60).toString().padStart(2,'0');
    const upS = (d.uptime % 60).toString().padStart(2,'0');
    $('status').innerText = '已连接 (' + d.ip + ') · 运行 ' + upH + ':' + upM + ':' + upS;
  } catch(e) {
    $('status').innerText = '连接失败，请检查网络后刷新';
  }
}

setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>
)rawliteral";

// ==================== 辅助函数 ====================
void setupPWM() {
  pinMode(LIGHT_PWM_PIN, OUTPUT);
  digitalWrite(LIGHT_PWM_PIN, LOW);
#ifdef LEDC_NEW_API
  ledcAttach(LIGHT_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
#else
  ledcSetup(0, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(LIGHT_PWM_PIN, 0);
#endif
}

void setLightOutput() {
  uint32_t duty = lightOn ? ((uint32_t)brightnessPercent * PWM_MAX_DUTY / 100) : 0;
#ifdef LEDC_NEW_API
  ledcWrite(LIGHT_PWM_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
  if (STATUS_LED_PIN >= 0 && STATUS_LED_PIN != LIGHT_PWM_PIN) {
    digitalWrite(STATUS_LED_PIN, lightOn ? HIGH : LOW);
  }
}

// 读取 ADC 并转成 mV，多次采样取平均
uint32_t readVoltageMV(uint8_t pin) {
  const int samples = 16;
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  uint32_t adcVal = sum / samples;
  return (uint32_t)(adcVal * 3300UL / 4095UL);
}

uint8_t readAmbientPercent() {
  uint32_t mv = readVoltageMV(AMBIENT_LIGHT_ADC_PIN);
  // 环境越亮，光敏电阻阻值越低，分压越小。0mV=100%（最亮），3300mV=0%（最暗）
  if (mv >= 3300) return 0;
  return (uint8_t)((3300UL - mv) * 100UL / 3300UL);
}

float readCurrentA() {
  uint32_t mv = readVoltageMV(CURRENT_ADC_PIN);
  float shuntOhm = SHUNT_RESISTANCE_MILLIOHM / 1000.0f;
  if (shuntOhm <= 0.0f || CURRENT_SENSE_GAIN <= 0.0f) return 0.0f;
  float current = (mv / 1000.0f) / (shuntOhm * CURRENT_SENSE_GAIN);
  if (current < 0.01f) current = 0.0f;
  currentSmoothed = CURRENT_ALPHA * current + (1.0f - CURRENT_ALPHA) * currentSmoothed;
  return currentSmoothed;
}

// 时间工具
uint16_t timeStrToMinutes(const String& t) {
  if (t.length() < 5) return 0;
  int h = t.substring(0, 2).toInt();
  int m = t.substring(3, 5).toInt();
  return (uint16_t)constrain(h * 60 + m, 0, 1439);
}

String minutesToTimeStr(uint16_t mins) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", mins / 60, mins % 60);
  return String(buf);
}

uint16_t getDeviceMinutesSinceMidnight() {
  return (uint16_t)((millis() / 60000UL) % 1440UL);
}

void applySchedule() {
  if (!scheduleEnabled) return;
  uint16_t nowMin = getDeviceMinutesSinceMidnight();
  bool shouldBeOn;
  if (onTimeMinutes == offTimeMinutes) {
    shouldBeOn = lightOn;
  } else if (onTimeMinutes < offTimeMinutes) {
    shouldBeOn = (nowMin >= onTimeMinutes && nowMin < offTimeMinutes);
  } else {
    shouldBeOn = (nowMin >= onTimeMinutes || nowMin < offTimeMinutes);
  }
  if (shouldBeOn != lightOn) {
    lightOn = shouldBeOn;
    setLightOutput();
  }
}

void handleButton() {
#if BUTTON_PIN != LIGHT_PWM_PIN
  if (BUTTON_PIN < 0) return;
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounce = millis();
  }
  if ((millis() - lastDebounce) > DEBOUNCE_MS) {
    if (reading == LOW && lastButtonState == HIGH) {
      lightOn = !lightOn;
      setLightOutput();
    }
  }
  lastButtonState = reading;
#endif
}

// 长按重置（恢复出厂）：持续按住 RESET_BUTTON_PIN 达 RESET_HOLD_MS 后，
// 清空 lightSwitch 命名空间下所有设置（灯光、定时、Wi-Fi 凭证），重启回到热点模式。
void handleResetButton() {
#if RESET_BUTTON_PIN > 0
  bool pressed = (digitalRead(RESET_BUTTON_PIN) == LOW);  // 对地按键，按下为 LOW
  if (pressed) {
    if (!resetBtnHeld) {
      resetBtnHeld = true;
      resetHoldStart = millis();
      Serial.println("检测到重置按键，保持按住以恢复出厂设置...");
    } else if (millis() - resetHoldStart >= RESET_HOLD_MS) {
      Serial.println("长按重置：清空所有设置并重启到热点模式...");
      prefs.begin("lightSwitch", false);
      prefs.clear();
      prefs.end();
      delay(300);
      ESP.restart();
    }
  } else {
    resetBtnHeld = false;
  }
#endif
}

// ==================== 持久化 ====================
void loadSettings() {
  prefs.begin("lightSwitch", false);
  lightOn = prefs.getBool("lightOn", false);
  brightnessPercent = prefs.getUChar("brightness", 50);
  scheduleEnabled = prefs.getBool("schedEn", false);
  onTimeStr = prefs.getString("onTime", "07:00");
  offTimeStr = prefs.getString("offTime", "23:00");
  prefs.end();
  onTimeMinutes = timeStrToMinutes(onTimeStr);
  offTimeMinutes = timeStrToMinutes(offTimeStr);
}

void saveSettings() {
  prefs.begin("lightSwitch", false);
  prefs.putBool("lightOn", lightOn);
  prefs.putUChar("brightness", brightnessPercent);
  prefs.putBool("schedEn", scheduleEnabled);
  prefs.putString("onTime", onTimeStr);
  prefs.putString("offTime", offTimeStr);
  prefs.end();
}

// ==================== 网络（热点 + 局域网回退） ====================
void loadWiFi() {
  prefs.begin("lightSwitch", false);
  wifiSsid = prefs.getString("wifiSsid", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  prefs.end();
}

void saveWiFi(const String& ssid, const String& password) {
  prefs.begin("lightSwitch", false);
  prefs.putString("wifiSsid", ssid);
  prefs.putString("wifiPassword", password);
  prefs.end();
}

void clearWiFi() {
  prefs.begin("lightSwitch", false);
  prefs.remove("wifiSsid");
  prefs.remove("wifiPassword");
  prefs.end();
}

void startMDNS() {
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    Serial.printf("mDNS 已启动: %s.local\n", DEVICE_HOSTNAME);
  }
}

// 启动网络：
// 1. 若已保存 Wi-Fi 凭证，先尝试连接局域网(STA)
// 2. 连接成功 -> 使用局域网（之后都走 LAN）
// 3. 无凭证或连接失败 -> 回退到热点(AP)
void setupNetwork() {
  loadWiFi();

  if (wifiSsid.length() > 0) {
    Serial.print("尝试连接 Wi-Fi: ");
    Serial.println(wifiSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      staMode = true;
      currentIP = WiFi.localIP().toString();
      Serial.print("已连接局域网。IP: ");
      Serial.println(currentIP);
      startMDNS();
      return;
    }
    Serial.println("局域网连接失败，回退到热点模式。");
  }

  // 热点模式
  staMode = false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  currentIP = WiFi.softAPIP().toString();
  Serial.print("热点已启动。SSID: ");
  Serial.println(AP_SSID);
  Serial.print("密码: ");
  Serial.println(AP_PASSWORD);
  Serial.print("AP IP: ");
  Serial.println(currentIP);
}

// ==================== Web 路由 ====================
void handleRoot() {
  server.send(200, "text/html", String(FPSTR(INDEX_HTML)));
}

void handleStatus() {
  String json = "{";
  json += "\"on\":" + String(lightOn ? "true" : "false") + ",";
  json += "\"brightness\":" + String(brightnessPercent) + ",";
  json += "\"ambient\":" + String(readAmbientPercent()) + ",";
  json += "\"current\":" + String(readCurrentA(), 3) + ",";
  json += "\"scheduleEnabled\":" + String(scheduleEnabled ? "true" : "false") + ",";
  json += "\"onTime\":\"" + onTimeStr + "\",";
  json += "\"offTime\":\"" + offTimeStr + "\",";
  json += "\"mode\":\"" + String(staMode ? "sta" : "ap") + "\",";
  json += "\"ip\":\"" + currentIP + "\",";
  json += "\"wifiSsid\":\"" + wifiSsid + "\",";
  json += "\"uptime\":" + String(millis() / 1000UL);
  json += "}";
  server.send(200, "application/json", json);
}

void handlePower() {
  String state = server.arg("state");
  if (state == "on") lightOn = true;
  else if (state == "off") lightOn = false;
  setLightOutput();
  server.send(200, "text/plain", "OK");
}

void handleBrightness() {
  int val = server.arg("value").toInt();
  brightnessPercent = (uint8_t)constrain(val, 0, 100);
  setLightOutput();
  server.send(200, "text/plain", "OK");
}

void handleSchedule() {
  scheduleEnabled = (server.arg("enabled") == "1");
  String newOn = server.arg("on");
  String newOff = server.arg("off");
  if (newOn.length() == 5) onTimeStr = newOn;
  if (newOff.length() == 5) offTimeStr = newOff;
  onTimeMinutes = timeStrToMinutes(onTimeStr);
  offTimeMinutes = timeStrToMinutes(offTimeStr);
  server.send(200, "text/plain", "OK");
}

void handleSave() {
  saveSettings();
  server.send(200, "text/plain", "Saved");
}

// 配置 Wi-Fi：保存凭证后重启，由启动逻辑自动连接局域网
void handleWiFiConfig() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID 不能为空");
    return;
  }
  saveWiFi(ssid, password);
  server.send(200, "text/plain", "已保存，设备即将重启连接 Wi-Fi");
  delay(500);
  ESP.restart();
}

// 忘记 Wi-Fi：清除凭证并重启回到热点模式
void handleForgetWiFi() {
  clearWiFi();
  server.send(200, "text/plain", "已清除 Wi-Fi，设备即将重启进入热点模式");
  delay(500);
  ESP.restart();
}

// ==================== Setup & Loop ====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP Light Switch starting...");

  if (STATUS_LED_PIN >= 0 && STATUS_LED_PIN != LIGHT_PWM_PIN) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }

#if BUTTON_PIN != LIGHT_PWM_PIN
  if (BUTTON_PIN >= 0) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
  }
#else
  Serial.println("警告：BUTTON_PIN 与 LIGHT_PWM_PIN 冲突，本地按键已禁用。");
#endif

#if RESET_BUTTON_PIN > 0
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  Serial.println("长按重置按键已启用 (GPIO" + String(RESET_BUTTON_PIN) + ")。");
#endif

  // ADC 配置
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // 支持约 0-3.3V 输入

  setupPWM();
  loadSettings();
  setLightOutput();
  setupNetwork();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/power", HTTP_POST, handlePower);
  server.on("/api/brightness", HTTP_POST, handleBrightness);
  server.on("/api/schedule", HTTP_POST, handleSchedule);
  server.on("/api/save", HTTP_POST, handleSave);
  server.on("/api/wifi", HTTP_POST, handleWiFiConfig);
  server.on("/api/forgetwifi", HTTP_POST, handleForgetWiFi);

  // 网页 OTA 升级：接收上传的固件 .bin，写入另一个 OTA 分区，完成后重启
  server.on("/update", HTTP_POST,
    []() {
      // 整个请求体接收完成后的回调
      if (Update.hasError()) {
        server.send(500, "text/plain", "升级失败：" + String(Update.getError()));
      } else {
        server.send(200, "text/plain", "升级成功，设备即将重启...");
        delay(500);
        ESP.restart();
      }
    },
    []() {
      // 接收上传数据的回调（分块流式写入）
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA 开始: %s\n", upload.filename.c_str());
        // UPDATE_SIZE_UNKNOWN：使用当前可用的最大空间
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {  // true = 设置下次启动使用该分区
          Serial.printf("OTA 成功: %u 字节\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  server.begin();

  Serial.println("HTTP server started.");
}

void loop() {
  server.handleClient();
  applySchedule();
  handleButton();
  handleResetButton();
  delay(5);
}
