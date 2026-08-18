/*
  ESP32-C2 / ESP32-C3 / ESP8285 Smart Light Switch
  =================================================
  - Wi-Fi AP mode for direct connection (no router needed)
  - Web UI: on/off, brightness slider (ESP32 & ESP8285 PWM), (ESP32) ambient light display, current display
  - Daily schedule: auto turn on/off at configured times
  - Persist settings to flash

  Supported chips (single sketch, selected by the board you pick):
  - ESP32-C2 / ESP32-C3 : PWM dimming + ambient + current; multi ADC channels
  - ESP8285 (ESP8266)    : on/off + PWM brightness (GPIO5 analogWrite), current only; single ADC (A0)

  Hardware (ESP32-C3 pin mapping):
  - GPIO4 : lamp PWM output (brightness control)
  - GPIO3 : ambient light sensor (ADC, photodiode + 10k pulldown)
  - GPIO0 : current sense amplifier output (ADC, with R11/R84 2/3 divider)
  - GPIO9 : on-board BOOT button (active LOW; separate from the lamp pin)

  ESP8285 default pin mapping (see the config block below):
  - LIGHT_PWM_PIN     : lamp digital output (relay / SSR)
  - CURRENT_ADC_PIN   : current sense (the only ADC, A0, 0~1.0V)
  - BUTTON_PIN        : on-board BOOT/flash button (usually GPIO0 on ESP8285)

  NOTE: No separate status LED on these boards — the lamp pin drives the lamp directly.
        The BOOT button is reused as the local button: short press toggles the lamp,
        long press (3s) restores factory settings.
*/

// 固件版本号：每次发布/重大改动时递增；会显示在网页标题下方
#define FIRMWARE_VERSION "1.2.0"

#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  #include <EEPROM.h>
  #include <Updater.h>        // 网页 OTA 固件升级（esp8266 的 Update 类）
  #include <TZ.h>             // NTP 时区字符串（如 TZ_Asia_Shanghai）

  // ---- ESP8266 专用：必须放在“最后一个 #include”之前 ----
  // Arduino IDE 会自动生成函数前向原型并插在最后一个 #include 之后，
  // 若 SettingsStore 定义在原型之后就会报 “was not declared in this scope”。
  #define EEPROM_SIZE 256
  typedef struct {
    uint8_t lightOn;
    uint8_t brightness;
    uint8_t schedEn;
    char onTime[6];
    char offTime[6];
    char wifiSsid[33];
    char wifiPassword[65];
  } SettingsStore;

  // ESP8266 的 Updater 不提供 UPDATE_SIZE_UNKNOWN，用最大可写固件空间代替
  #define UPDATE_SIZE_UNKNOWN ESP.getFreeSketchSpace()
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Preferences.h>
  #include <ESPmDNS.h>
  #include <Update.h>         // 网页 OTA 固件升级
  #include <time.h>           // NTP 时间：configTime / getLocalTime / time()
#endif

// ==================== 硬件引脚配置（按芯片分两套） ====================
// ESP32-C2/C3：多 ADC 通道，支持亮度 PWM + 环境光 + 电流
// ESP8285    ：单 ADC（A0，0~1.0V），不做环境光、仅用 A0 做电流检测；
//              灯用 GPIO5 的 analogWrite() 软件 PWM 做开关 + 亮度调光
#ifdef ESP8266
  // ---- ESP8285 / ESP8266 引脚（按你的 ESP8285 板实际接线修改） ----
  #define LIGHT_PWM_PIN           5    // 灯控数字输出（继电器/SSR）。避免 strapping 脚 GPIO0/2/15
  #define RELAY_ACTIVE_LOW        0     // 1=低电平吸合（多数继电器模块），0=高电平吸合。若灯“相反/不受控”改 1
  #define STATUS_LED_PIN          (-1)  // 无独立状态 LED
  #define AMBIENT_LIGHT_ADC_PIN   (-1)  // ESP8285 仅 1 路 ADC，已留给电流检测，故不读环境光
  #define CURRENT_ADC_PIN         A0    // 电流检测（唯一 ADC 通道，量程 0~1.0V）
  #define BUTTON_PIN              0     // 板载 BOOT/下载键通常是 GPIO0（低电平有效）
  // 功能开关
  #define BRIGHTNESS_SUPPORTED    1     // ESP8285 用 analogWrite() 支持亮度 PWM 调光
  #define AMBIENT_SUPPORTED       0     // 不读环境光
  #define CURRENT_SUPPORTED       1     // 保留实时电流显示
  #define ADC_MAX                 1023  // ESP8285/ESP8266 ADC 为 10 位
  #define ADC_VREF_MV             1000  // ESP8285/ESP8266 ADC 量程 0~1.0V（与 ESP32 的 3.3V 不同）
  #define CURRENT_CAL_SCALE       1.6f  // 实测校准：该板同负载实测电流约为显示值的 2.2 倍，用于修正分压/增益总误差
#else
  // ---- ESP32-C2/C3 引脚 ----
  #define LIGHT_PWM_PIN           4     // 灯 PWM 输出（直接驱动灯，板载无独立状态 LED）
  #define STATUS_LED_PIN          (-1)  // 独立状态 LED；本板无此功能，设为 -1 禁用
  #define AMBIENT_LIGHT_ADC_PIN   3     // 光敏检测：VDD → 光敏二极管 U2 → GPIO3 → R8(10k) → GND，亮度越高电压越高
  #define CURRENT_ADC_PIN         0     // 电流检测
  #define BUTTON_PIN              9     // 本地按键 = 板载 BOOT 键(GPIO9，低电平有效)
  #define BRIGHTNESS_SUPPORTED    1
  #define AMBIENT_SUPPORTED       1
  #define CURRENT_SUPPORTED       1
  #define ADC_MAX                 4095  // ESP32 ADC 12 位
  #define ADC_VREF_MV             3300  // 使用 ADC_11db 衰减后约 0~3.3V
  #define CURRENT_CAL_SCALE       1.0f  // ESP32 参考板暂不校准
#endif

// 本地 BOOT 键复用为两个功能（单一按键，低电平有效，内部上拉）：
//   - 短按(松开)        -> 切换灯的开关
//   - 长按 RESET_HOLD_MS -> 恢复出厂设置（清空灯光/定时/Wi-Fi）并重启回热点
// 注意：BOOT 键也是 strapping 脚（ESP32 为 GPIO9，ESP8285 通常为 GPIO0）；
//       上电/复位时按住会进入下载模式，本逻辑仅在启动完成后运行时生效。
#define RESET_HOLD_MS           3000  // 长按触发恢复出厂的时长(ms)

// ==================== PWM 配置（仅 ESP32 用） ====================
#define PWM_FREQ        5000
#define PWM_RESOLUTION  10            // 占空比范围 0-1023
#define PWM_MAX_DUTY    1023

// ==================== 电流检测校准 ====================
// 电流检测部分（以用户 ESPHome 工作配置为准）：
// - INA180A1IDBVR  电流检测放大器，固定增益 20 V/V
// - R_shunt = 20 mΩ（高侧分流电阻，位于 VCC 与 USB 输出 VOUT 之间）
// - 分压 = 1/3（INA180 输出分压到 ESP8285 A0，A0 只能 0~1.0V）
// 公式：电流(A) = ADC 电压(V) / (分流电阻(Ω) * 放大器增益 * 分压比)
//             = V_adc / (0.02 * 20 * 1/3) = V_adc * 7.5   （与 ESPHome 的 multiply:7500 一致）
// 最大可测电流 ≈ 1.0V / (0.02*20*1/3) ≈ 7.5A。
#ifdef ESP8266
  #define SHUNT_RESISTANCE_MILLIOHM    20.0f      // 分流电阻，单位 mΩ（按 ESPHome：20 mΩ）
  #define CURRENT_SENSE_GAIN           20.0f      // INA180A1 固定增益 20 V/V
  #define CURRENT_SENSE_DIVIDER_RATIO  (1.0f/3.0f) // 分压比 1/3
#else
  // ESP32-C3 板：GPIO0 前端有 R11=100kΩ / R84=200kΩ 分压，Vadc = V_INA180 × 2/3
  #define SHUNT_RESISTANCE_MILLIOHM    150.0f
  #define CURRENT_SENSE_GAIN           20.0f
  #define CURRENT_SENSE_DIVIDER_RATIO  (2.0f/3.0f)
#endif

// 清零阈值：低于该电流(A)视为 INA180 偏置/噪声，归零不显示。
// 原按 ESPHome 取 0.045A(≈6mV)；本负载仅 ~15mA 且空载干净归零，下调到 0.010A 以便显示小负载。
#define CURRENT_ZERO_THRESHOLD_A  0.010f

// ==================== Wi-Fi AP 配置 ====================
#define AP_SSID_PREFIX  "ESPSwitch-"   // 热点名前缀，后接 MAC 后两字节作为唯一编号（如 ESPSwitch-1A2B）
#define AP_PASSWORD     "12345678"    // 至少 8 位
#define AP_CHANNEL      1
#define AP_MAX_CLIENTS  4
#define MDNS_HOST_PREFIX   "espswitch-"   // mDNS 主机名前缀（小写，符合 mDNS 规范），后接 MAC 后两字节唯一编号（如 espswitch-1A2B.local）
#define WIFI_CONNECT_TIMEOUT_MS  20000  // STA 连接超时，超时后回退到热点

// ==================== 全局对象 ====================
#ifdef ESP8266
  ESP8266WebServer server(80);
#else
  WebServer server(80);
  Preferences prefs;
#endif

// 网络
String wifiSsid = "";
String wifiPassword = "";
bool staMode = false;          // true=已连上局域网(STA)，false=热点(AP)模式
String currentIP = "";

// 运行状态
bool lightOn = false;
uint8_t brightnessPercent = 100;     // 0-100，默认 100%（避免 PWM 占空比为 0 导致灯不亮）
bool scheduleEnabled = false;
String onTimeStr = "07:00";
String offTimeStr = "23:00";

// 本地按键（= 板载 BOOT 键 GPIO9）
bool lastButtonState = HIGH;       // 上一次原始读值（用于检测变化、重启去抖计时）
bool buttonStable = HIGH;           // 去抖后的稳定态，用于检测按下/松开跳变
unsigned long lastDebounce = 0;
const unsigned long DEBOUNCE_MS = 50;
unsigned long pressStart = 0;     // 本次按下的起始时间
bool longPressFired = false;      // 长按重置是否已触发

// 定时器
uint16_t onTimeMinutes = 7 * 60;
uint16_t offTimeMinutes = 23 * 60;

// 电流采样平滑
float currentSmoothed = 0.0f;
int32_t currentOffsetMV = 0;   // 开机零点校准：INA180 偏置电压(mV)，从每次读数扣除
const float CURRENT_ALPHA = 0.5f;   // 平滑系数：越大越灵敏（0.5 + 100ms 高频采样 ≈ 0.5s 收敛）
uint32_t lastCurrentSampleMs = 0;   // 最近一次电流采样时刻(ms)，供 loop 高频采样

// 电流诊断（供网页显示，实时刷新）
int      diagRawAdc     = 0;       // 原始 ADC 读数（0 ~ ADC_MAX）
uint32_t diagMv         = 0;        // A0 电压 (mV)
float    diagCurrentMA  = 0.0f;     // 计算电流 (mA)

// LEDC API 兼容性（仅 ESP32 系列使用；ESP8285 无 LEDC，亮度由数字输出实现）
#ifndef ESP8266
  #ifndef ESP_ARDUINO_VERSION_MAJOR
    #define ESP_ARDUINO_VERSION_MAJOR 2
  #endif
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    #define LEDC_NEW_API
  #endif
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
  <div class="subtitle" id="subtitle">ESP Light Switch</div>

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

    <div class="card" id="card-brightness">
      <div class="row">
        <span class="label">亮度</span>
        <span class="value" id="brightnessVal">90%</span>
      </div>
      <input type="range" id="brightness" min="1" max="100" value="100" oninput="updateBrightnessLabel()" onchange="setBrightness()">
    </div>

    <div class="card">
      <div class="row">
        <div id="ambientWrap">
          <div class="metric-label">环境亮度</div>
          <div class="metric" id="ambient">--</div>
        </div>
        <div>
          <div class="metric-label">实时电流</div>
          <div class="metric" id="current" style="cursor:pointer;">--</div>
        </div>
      </div>
    </div>

    <div class="card" id="card-diag" style="display:none;">
      <div class="row" style="margin:0;">
        <span class="label">电流诊断</span>
      </div>
      <div class="note" id="diag" style="text-align:left;margin-top:8px;font-family:Consolas,Menlo,monospace;">--</div>
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
      <div class="note" style="margin-top:12px;line-height:1.6;color:#555;">
        连接成功后，在与开关<b>同一局域网</b>的手机/电脑上，浏览器打开
        <b style="color:#007aff;" id="mdnsHint">http://espswitch-XXXX.local</b><br>
        若该地址打不开，请用上方“当前地址”里的 IP 访问（如 http://192.168.x.x）。
      </div>
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
// 电流自动切换单位：<1A 显示 mA，≥1A 显示 A
function fmtCurrent(a) {
  if (!(typeof a === 'number') || !isFinite(a) || a < 0) a = 0;
  if (a < 1.0) return (a * 1000).toFixed(1) + ' mA';
  return a.toFixed(2) + ' A';
}
// 电流诊断卡片：点击实时电流，整张卡片显示/隐藏（无箭头）
function toggleDiag() {
  var cd = $('card-diag');
  if (!cd) return;
  cd.style.display = (cd.style.display === 'none') ? 'block' : 'none';
}
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
    // 电流显示：小电流用 mA，大电流(≥1A)用 A，自动切换单位（与诊断同源，均带 1 位小数）
    var cur = parseFloat(d.current);
    if (!(typeof cur === 'number') || !isFinite(cur) || cur < 0) cur = 0;
    $('current').innerText = fmtCurrent(cur);
    $('scheduleEn').checked = d.scheduleEnabled;
    $('onTime').value = d.onTime;
    $('offTime').value = d.offTime;
    $('netMode').innerText = (d.mode === 'sta') ? '局域网' : '热点';
    $('netIp').innerText = d.ip;
    // 按设备能力隐藏不支持的功能（ESP8285 不做亮度、不读环境光）
    if (d.brightnessSup != 1) { const bc = $('card-brightness'); if (bc) bc.style.display = 'none'; }
    if (d.ambientSup != 1) { const aw = $('ambientWrap'); if (aw) aw.style.display = 'none'; }
    // 电流诊断卡片：仅启用电流检测时显示
    if (d.currentSup != 1) { const dc = $('card-diag'); if (dc) dc.style.display = 'none'; }
    else {
      var mA = (typeof d.diagMA === 'number' && isFinite(d.diagMA)) ? d.diagMA : 0;
      var raw = (typeof d.diagRaw === 'number') ? d.diagRaw : 0;
      var mv = (typeof d.diagMv === 'number') ? d.diagMv : 0;
      var amax = (typeof d.adcMax === 'number' && d.adcMax > 0) ? d.adcMax : 1023;
      $('diag').innerText =
        'raw=' + raw + '/' + amax +
        '  A0=' + mv + 'mV' +
        '  计算=' + fmtCurrent(mA / 1000.0) +
        '  (阈值' + (d.curThrMA || 45) + 'mA以下→实时电流显示0，校准×' + (d.curCal || 1) + ')';
    }
    if (!$('wifiSsid').value) $('wifiSsid').value = d.wifiSsid;
    const upH = Math.floor(d.uptime / 3600).toString().padStart(2,'0');
    const upM = Math.floor((d.uptime % 3600) / 60).toString().padStart(2,'0');
    const upS = (d.uptime % 60).toString().padStart(2,'0');
    $('status').innerText = '已连接 (' + d.ip + ') · 运行 ' + upH + ':' + upM + ':' + upS;
    var fv = (d.ver && d.ver.length) ? d.ver : '?';
    $('subtitle').innerText = 'ESP Light Switch · v' + fv;
    if (d.mdns && $('mdnsHint')) $('mdnsHint').innerText = 'http://' + d.mdns;
  } catch(e) {
    $('status').innerText = '连接失败，请检查网络后刷新';
  }
}

$('current').addEventListener('click', toggleDiag);
setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>
)rawliteral";

// ==================== 辅助函数 ====================
#ifdef ESP8266
// ESP8285/ESP8266：用 analogWrite() 软件 PWM 驱动 GPIO5（支持亮度调光）
// 说明：ESP8266 无硬件 PWM，analogWrite 由定时器中断实现，频率默认 1kHz，
//       分辨率 0~1023（10 位）。继电器模块建议亮度保持 100%，中间值会令其抖动/蜂鸣。
void setupPWM() {
  pinMode(LIGHT_PWM_PIN, OUTPUT);
  analogWriteFreq(1000);          // 1kHz，适合 LED 调光；继电器无需调光可保持 100%
  analogWriteRange(1023);         // 0~1023（10 位分辨率）
  analogWrite(LIGHT_PWM_PIN, 0);  // 先关
}

void setLightOutput() {
  // 期望电平：0=灭；亮灯时按 brightnessPercent 映射到 0~1023 作为占空比
  uint16_t level = lightOn ? (uint16_t)((uint32_t)brightnessPercent * 1023 / 100) : 0;
  if (RELAY_ACTIVE_LOW) level = 1023 - level;   // 低电平吸合：反转电平
  analogWrite(LIGHT_PWM_PIN, level);
  if (STATUS_LED_PIN >= 0 && STATUS_LED_PIN != LIGHT_PWM_PIN) {
    digitalWrite(STATUS_LED_PIN, lightOn ? HIGH : LOW);
  }
}
#else
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
#endif

// 读取 ADC 并转成 mV，多次采样取平均（适配 ESP32 的 12 位/3.3V 与 ESP8285 的 10 位/1.0V）
uint32_t readVoltageMV(uint8_t pin) {
  const int samples = 16;
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  uint32_t adcVal = sum / samples;
  return (uint32_t)(adcVal * ADC_VREF_MV / ADC_MAX);
}

uint8_t readAmbientPercent() {
  if (AMBIENT_LIGHT_ADC_PIN < 0) return 0;   // ESP8285 不读环境光
  uint32_t mv = readVoltageMV(AMBIENT_LIGHT_ADC_PIN);
  // 本板光敏电路：VDD → 光敏二极管 U2 → GPIO3 → R8(10k) → GND。
  // 亮度越高，光电流越大，GPIO3 电压越高。因此直接用 mv / Vref 得到亮度百分比。
  uint32_t pct = mv * 100UL / ADC_VREF_MV;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

float readCurrentA() {
  if (!CURRENT_SUPPORTED) { diagRawAdc = 0; diagMv = 0; diagCurrentMA = 0; return 0.0f; }
  int rawAdc = (int)analogRead(CURRENT_ADC_PIN);   // 单次原始读数，用于网页诊断展示
  uint32_t mvRaw = readVoltageMV(CURRENT_ADC_PIN);
  // 扣除开机校准得到的 INA180 零点偏置，避免空载显示假电流（如 ESP32-C3 的 ~180mA）
  uint32_t mv = (mvRaw > (uint32_t)currentOffsetMV) ? (mvRaw - (uint32_t)currentOffsetMV) : 0;
  float shuntOhm = SHUNT_RESISTANCE_MILLIOHM / 1000.0f;
  if (shuntOhm <= 0.0f || CURRENT_SENSE_GAIN <= 0.0f) {
    diagRawAdc = rawAdc; diagMv = mv; diagCurrentMA = 0;
    return 0.0f;
  }
  float currentRaw = (mv / 1000.0f) / (shuntOhm * CURRENT_SENSE_GAIN * CURRENT_SENSE_DIVIDER_RATIO);
  diagRawAdc = rawAdc;
  diagMv = mv;
  // 低于清零阈值视为 INA180 偏置底噪：先用原始（未校准）电流判断并归零，避免空载被校准后误显示
  float current = currentRaw;
  if (current < CURRENT_ZERO_THRESHOLD_A) current = 0.0f;
  current *= CURRENT_CAL_SCALE;   // 校准放在归零之后：修正该板分压/增益总误差
  diagCurrentMA = current * 1000.0f;
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

// 取“当天分钟数”。联网时返回真实本地时间（NTP 同步后的墙钟），
// 未同步时回落到“开机后分钟数”，保证无外网也能粗略工作。
uint16_t getDeviceMinutesSinceMidnight() {
  time_t now = time(nullptr);
  if (now > 100000) {                 // 已同步真实时间（2026 年 epoch 远大于此）
    struct tm t;
#ifdef ESP8266
    localtime_r(&now, &t);
#else
    getLocalTime(&t);
#endif
    return (uint16_t)(t.tm_hour * 60 + t.tm_min);
  }
  return (uint16_t)((millis() / 60000UL) % 1440UL);
}

// 连上局域网后同步真实时间（中国 UTC+8）。AP 模式无外网则不同步，定时回落到开机计时。
void syncTime() {
  if (!staMode) return;
#ifdef ESP8266
  configTime(TZ_Asia_Shanghai, 0, "pool.ntp.org", "time.nist.gov");
#else
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.cloudflare.com");
#endif
}

// 启动后阻塞等待 NTP 时间就位（最多 ~3 秒），让开机时灯状态能与定时对齐
void waitForTimeSync() {
  if (!staMode) return;
  for (int i = 0; i < 30; i++) {
    if (time(nullptr) > 100000) return;
    delay(100);
  }
}

// 给定分钟数，判断定时状态下灯“应该”开还是关
bool scheduleShouldBeOn(uint16_t nowMin) {
  if (onTimeMinutes == offTimeMinutes) return lightOn;   // 起止相同则不强制改变
  if (onTimeMinutes < offTimeMinutes)
    return (nowMin >= onTimeMinutes && nowMin < offTimeMinutes);
  return (nowMin >= onTimeMinutes || nowMin < offTimeMinutes);
}

void applySchedule() {
  if (!scheduleEnabled) return;
  uint16_t nowMin = getDeviceMinutesSinceMidnight();
  // 仅在到达“开点”或“关点”这一分钟内动作一次，平时不干扰手动开关
  static uint16_t lastBoundaryMin = 0xFFFF;
  bool atOn  = (nowMin == onTimeMinutes);
  bool atOff = (nowMin == offTimeMinutes);
  if ((atOn || atOff) && lastBoundaryMin != nowMin) {
    lastBoundaryMin = nowMin;
    bool shouldBeOn = atOn;   // 到开点 -> 开；到关点 -> 关
    if (shouldBeOn != lightOn) {
      lightOn = shouldBeOn;
      setLightOutput();
    }
  }
}

// 板载 BOOT 键(GPIO9) 复用为单一按键：
//   - 短按(松开)        -> 切换灯的开关
//   - 长按 RESET_HOLD_MS -> 恢复出厂设置（清空灯光/定时/Wi-Fi）并重启回热点
void handleButton() {
#if BUTTON_PIN != LIGHT_PWM_PIN
  if (BUTTON_PIN < 0) return;
  bool reading = digitalRead(BUTTON_PIN);  // BOOT 键低电平有效
  if (reading != lastButtonState) {
    lastDebounce = millis();              // 状态变化，重启去抖计时
  }
  if ((millis() - lastDebounce) > DEBOUNCE_MS) {
    // 去抖通过后，用稳定态 buttonStable 检测“按下/松开”的真实跳变
    if (reading != buttonStable) {
      buttonStable = reading;
      if (reading == LOW) {
        // 刚按下：记录起始时间（pressStart 现在一定会被正确赋值）
        pressStart = millis();
        longPressFired = false;
      } else {
        // 刚松开，且未触发长按 -> 短按切换灯开关
        if (!longPressFired) {
          lightOn = !lightOn;
          setLightOutput();
          Serial.println(lightOn ? "短按：开灯" : "短按：关灯");
        }
      }
    }
    // 持续按住达到时长：恢复出厂（仅在稳定为 LOW 期间检测）
    if (reading == LOW && !longPressFired && (millis() - pressStart >= RESET_HOLD_MS)) {
      longPressFired = true;
      Serial.println("长按重置：清空所有设置并重启到热点模式...");
#ifdef ESP8266
      eepromClearAll();
#else
      prefs.begin("lightSwitch", false);
      prefs.clear();
      prefs.end();
#endif
      delay(300);
      ESP.restart();
    }
  }
  lastButtonState = reading;
#endif
}

// ==================== ESP8285/ESP8266 EEPROM 持久化层 ====================
// ESP8285/ESP8266 没有 Preferences 库，用 EEPROM 模拟（固定布局）。
// 注意：SettingsStore 与 EEPROM_SIZE 已在文件顶部（最后一个 #include 之前）
// 提前声明，否则 Arduino 自动生成的函数前向原型会因看不到该结构体而报错。
#ifdef ESP8266
static void eepromRead(SettingsStore& s) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, s);
  EEPROM.end();
  if (s.lightOn == 0xFF) {        // 全新未写过（Flash 全 0xFF）当作空，但写入合理默认
    memset(&s, 0, sizeof(s));
    s.brightness = 100;           // 默认 100%，避免 PWM 占空比为 0 导致灯不亮、开关失灵
  } else {
    // 亮度 0 视为无效（旧固件残留 / 未初始化），纠正为默认 100%，
    // 否则会出现「开关关着正常、但开灯后灯不亮」；用户主动设的 1~100 保留。
    if (s.brightness == 0 || s.brightness > 100) s.brightness = 100;
  }
}
static void eepromWrite(const SettingsStore& s) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, s);
  EEPROM.commit();
  EEPROM.end();
}
static void eepromClearAll() {
  SettingsStore s;
  memset(&s, 0, sizeof(s));       // 基线：全部清零（灯关、无 WiFi、无定时）
  s.brightness = 100;             // 恢复出厂亮度默认 100%，避免重启后占空比为 0 灯不亮
  eepromWrite(s);
}
#endif


// ==================== 持久化（EEPROM / Preferences 双实现） ====================
void loadSettings() {
#ifdef ESP8266
  SettingsStore s; eepromRead(s);
  lightOn = s.lightOn ? true : false;
  brightnessPercent = s.brightness;          // ESP8285 用 analogWrite() PWM 调光
  scheduleEnabled = s.schedEn ? true : false;
  onTimeStr = String(s.onTime);
  offTimeStr = String(s.offTime);
#else
  prefs.begin("lightSwitch", false);
  lightOn = prefs.getBool("lightOn", false);
  brightnessPercent = prefs.getUChar("brightness", 100);
  scheduleEnabled = prefs.getBool("schedEn", false);
  onTimeStr = prefs.getString("onTime", "07:00");
  offTimeStr = prefs.getString("offTime", "23:00");
  prefs.end();
#endif
  onTimeMinutes = timeStrToMinutes(onTimeStr);
  offTimeMinutes = timeStrToMinutes(offTimeStr);
}

void saveSettings() {
#ifdef ESP8266
  SettingsStore s; eepromRead(s);
  s.lightOn = lightOn ? 1 : 0;
  s.brightness = brightnessPercent;
  s.schedEn = scheduleEnabled ? 1 : 0;
  strncpy(s.onTime, onTimeStr.c_str(), sizeof(s.onTime) - 1); s.onTime[5] = 0;
  strncpy(s.offTime, offTimeStr.c_str(), sizeof(s.offTime) - 1); s.offTime[5] = 0;
  eepromWrite(s);
#else
  prefs.begin("lightSwitch", false);
  prefs.putBool("lightOn", lightOn);
  prefs.putUChar("brightness", brightnessPercent);
  prefs.putBool("schedEn", scheduleEnabled);
  prefs.putString("onTime", onTimeStr);
  prefs.putString("offTime", offTimeStr);
  prefs.end();
#endif
}

// ==================== 网络（热点 + 局域网回退） ====================
void loadWiFi() {
#ifdef ESP8266
  SettingsStore s; eepromRead(s);
  wifiSsid = String(s.wifiSsid);
  wifiPassword = String(s.wifiPassword);
#else
  prefs.begin("lightSwitch", false);
  wifiSsid = prefs.getString("wifiSsid", "");
  wifiPassword = prefs.getString("wifiPassword", "");
  prefs.end();
#endif
}

void saveWiFi(const String& ssid, const String& password) {
#ifdef ESP8266
  SettingsStore s; eepromRead(s);
  strncpy(s.wifiSsid, ssid.c_str(), sizeof(s.wifiSsid) - 1); s.wifiSsid[32] = 0;
  strncpy(s.wifiPassword, password.c_str(), sizeof(s.wifiPassword) - 1); s.wifiPassword[64] = 0;
  eepromWrite(s);
#else
  prefs.begin("lightSwitch", false);
  prefs.putString("wifiSsid", ssid);
  prefs.putString("wifiPassword", password);
  prefs.end();
#endif
}

void clearWiFi() {
#ifdef ESP8266
  SettingsStore s; eepromRead(s);
  memset(s.wifiSsid, 0, sizeof(s.wifiSsid));
  memset(s.wifiPassword, 0, sizeof(s.wifiPassword));
  eepromWrite(s);
#else
  prefs.begin("lightSwitch", false);
  prefs.remove("wifiSsid");
  prefs.remove("wifiPassword");
  prefs.end();
#endif
}

// 取设备唯一编号（如 "1A2B"），热点名与 mDNS 域名共用，保证多台设备可区分。
// 关键：必须用出厂固定的硬件标识，不能用 WiFi.macAddress()——ESP32 在 AP/STA 模式下
// 返回的 MAC 不同（STA=base，AP=base+1），会导致热点名后缀与连上 WiFi 后的域名后缀不一致。
const char* getMacSuffix() {
  static char suffix[5] = {0};
  if (suffix[0] == 0) {
#ifdef ESP8266
    uint32_t id = ESP.getChipId();
    snprintf(suffix, sizeof(suffix), "%02X%02X", (uint8_t)(id >> 8), (uint8_t)id);
#else
    uint64_t efuse = ESP.getEfuseMac();   // 出厂烧录，与 WiFi 模式无关，固定不变
    snprintf(suffix, sizeof(suffix), "%02X%02X", (uint8_t)(efuse >> 8), (uint8_t)efuse);
#endif
  }
  return suffix;
}

void startMDNS() {
  char host[32];
  snprintf(host, sizeof(host), MDNS_HOST_PREFIX "%s", getMacSuffix());
  if (MDNS.begin(host)) {
    Serial.printf("mDNS 已启动: %s.local\n", host);
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
    char host[32];
    snprintf(host, sizeof(host), MDNS_HOST_PREFIX "%s", getMacSuffix());
    WiFi.setHostname(host);   // 让路由器/局域网显示唯一主机名（如 espswitch-1A2B）
#ifdef ESP8266
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);   // ESP8266：SDK 层自动回连已配网络
#endif
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

  // 热点模式：用 MAC 后两字节作为唯一编号，避免多台设备热点同名冲突（如 ESPSwitch-1A2B）
  staMode = false;
  char apSsid[32];
  snprintf(apSsid, sizeof(apSsid), AP_SSID_PREFIX "%s", getMacSuffix());
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  currentIP = WiFi.softAPIP().toString();
  Serial.print("热点已启动。SSID: ");
  Serial.println(apSsid);
  Serial.print("密码: ");
  Serial.println(AP_PASSWORD);
  Serial.print("AP IP: ");
  Serial.println(currentIP);
}

// 运行时保活：已成功连上局域网(staMode==true)后若中途掉线，自动用已保存的
// 凭证重新连接。热点模式(staMode==false)下不干预，避免 AP/STA 反复横跳。
void ensureWiFi() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 5000) return;   // 每 5 秒检查一次，省 CPU
  lastCheck = millis();
  if (!staMode) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("STA 掉线，尝试用已保存的 Wi-Fi 重连...");
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  }
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
  json += "\"brightnessSup\":" + String(BRIGHTNESS_SUPPORTED) + ",";
  json += "\"ambientSup\":" + String(AMBIENT_SUPPORTED) + ",";
  json += "\"currentSup\":" + String(CURRENT_SUPPORTED) + ",";
  // 电流诊断（供网页显示）：原始 ADC、A0 电压(mV)、计算电流(mA)、ADC 满量程
  json += "\"diagRaw\":" + String(diagRawAdc) + ",";
  json += "\"diagMv\":" + String(diagMv) + ",";
  json += "\"diagMA\":" + String(diagCurrentMA, 1) + ",";
  json += "\"adcMax\":" + String(ADC_MAX) + ",";
  json += "\"curThrMA\":" + String(CURRENT_ZERO_THRESHOLD_A * 1000.0f, 0) + ",";
  json += "\"curCal\":" + String(CURRENT_CAL_SCALE, 2) + ",";
  json += "\"ver\":\"" + String(FIRMWARE_VERSION) + "\",";
  char mdnsName[40];
  snprintf(mdnsName, sizeof(mdnsName), "%s%s.local", MDNS_HOST_PREFIX, getMacSuffix());
  json += "\"mdns\":\"" + String(mdnsName) + "\",";
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
  brightnessPercent = (uint8_t)constrain(val, 1, 100);   // 亮度最低 1%，0 视为关灯由开关控制，避免开灯不亮
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
  // 启用定时时立即按当前时间对齐灯状态；之后仅在定时边界点动作（不抢手动控制）
  if (scheduleEnabled) {
    uint16_t nowMin = getDeviceMinutesSinceMidnight();
    bool shouldBeOn = scheduleShouldBeOn(nowMin);
    if (shouldBeOn != lightOn) { lightOn = shouldBeOn; setLightOutput(); }
  }
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
    Serial.println("本地 BOOT 键已启用(GPIO" + String(BUTTON_PIN) + ")：短按开关灯，长按 " + String(RESET_HOLD_MS) + "ms 恢复出厂。");
  }
#else
  Serial.println("警告：BUTTON_PIN 与 LIGHT_PWM_PIN 冲突，本地按键已禁用。");
#endif

  // ADC 配置（ESP8285/ESP8266 的 ADC 固定 10 位、量程 0~1.0V，无需配置）
#ifndef ESP8266
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // 支持约 0-3.3V 输入
#endif

  setupPWM();

  // 电流零点校准：开机、确保继电器关闭（无负载）时采样 INA180 零点偏置，后续读数扣除，空载显示归零。
  // （ESP32-C3 实测 GPIO0 有约 72mV 偏置，直接算成 180mA；校准后可消除空载假电流。）
  if (CURRENT_SUPPORTED) {
    lightOn = false;            // 强制关灯，确保采样时无负载
    setLightOutput();
    delay(300);
    int32_t acc = 0;
    for (int i = 0; i < 16; i++) { acc += (int32_t)readVoltageMV(CURRENT_ADC_PIN); delay(10); }
    currentOffsetMV = acc / 16;
    Serial.println("[电流校准] 零点偏移=" + String(currentOffsetMV) + " mV (已扣除，空载将显示 0)");
  }

  loadSettings();
  setupNetwork();
  syncTime();
  waitForTimeSync();
  // 开机按当前（真实）时间让灯状态与定时一致：当前在开启窗口内则点亮，否则熄灭
  if (scheduleEnabled) {
    uint16_t nowMin = getDeviceMinutesSinceMidnight();
    bool shouldBeOn = scheduleShouldBeOn(nowMin);
    if (shouldBeOn != lightOn) lightOn = shouldBeOn;
  }
  setLightOutput();

  // 电流采样诊断：开灯后看这一行，与网页「电流诊断」卡片内容一致。
  //   - raw=0 / mV=0  → 硬件上 A0 没信号（INA180 输出没接到 A0，或 R_shunt/分压未焊好），与软件无关。
  //   - mV 有值但电流算错 → 多半是 SHUNT_RESISTANCE_MILLIOHM / 分压比 / currentOffsetMV 还需微调。
  if (CURRENT_SUPPORTED) {
    readCurrentA();   // 填充 diagRawAdc / diagMv / diagCurrentMA
    Serial.println("[电流诊断] rawADC=" + String(diagRawAdc) +
                  "  补偿后电压=" + String(diagMv) + " mV" +
                  "  计算电流=" + String(diagCurrentMA, 1) + " mA");
  }

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
  ensureWiFi();   // 运行中掉线自动回连上次 Wi-Fi

  // 高频采样电流：loop 每 100ms 调一次 readCurrentA()，让平滑值快速收敛，
  // 避免网页实时电流滞后（之前只在网页刷新时才采样，约 2s 一步，开灯要等好几秒才响应）
  if (millis() - lastCurrentSampleMs >= 100) {
    lastCurrentSampleMs = millis();
    readCurrentA();
  }

  delay(5);
}
