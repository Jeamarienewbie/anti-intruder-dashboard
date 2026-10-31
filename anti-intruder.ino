#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "SD_MMC.h"
#include <time.h>

/* ══════════════════ TIME (NTP → day/time reports, Asia/Manila) ══════════════════ */
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 8 * 3600;   // Philippines UTC+8
const int   DAYLIGHT_OFFSET_SEC = 0;
bool timeSynced = false;
void syncTimeNow() {
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "time.nist.gov");
    struct tm ti;
    for (int i = 0; i < 20; i++) {  // ~10s max wait
        if (getLocalTime(&ti)) { timeSynced = true; return; }
        delay(500);
    }
}
// "2026-10-14 14:32:05", "2026-10-14", "14:32:05", "Wednesday"
String fmtDateTime(time_t t, char* dayBuf = nullptr, size_t dayLen = 0,
                   String* dateOut = nullptr, String* timeOut = nullptr) {
    struct tm* tm = localtime(&t);
    char dt[20]; strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M:%S", tm);
    if (dayBuf) strftime(dayBuf, dayLen, "%A", tm);
    if (dateOut) { char d[11]; strftime(d, sizeof(d), "%Y-%m-%d", tm); *dateOut = String(d); }
    if (timeOut) { char s[9];  strftime(s, sizeof(s), "%H:%M:%S", tm); *timeOut = String(s); }
    return String(dt);
}

/* ══════════════════ NETWORK CONFIG ══════════════════ */
// AP mode (local dashboard access) - always available
const char* ap_ssid = "S3-GUARD-PRO";
const char* ap_pass = "YOUR_AP_PASSWORD";
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// STA mode (connects to home WiFi for INTERNET - required for SMS and Firebase)
// If unreachable, the system still works locally; SMS & cloud sync are simply skipped.
const char* wifi_ssid = "YOUR_WIFI_SSID";
const char* wifi_pass = "YOUR_WIFI_PASSWORD";

/* ══════════════════ SMS (PhilSMS API v3) ══════════════════ */
const char* SMS_TOKEN  = "YOUR_PHILSMS_API_TOKEN";
const char* SMS_TO     = "639XXXXXXXXX";      // recipient (intl format: 63XXXXXXXXXX)
const char* SMS_SENDER = "PhilSMS";           // sender id (max 11 chars)
const char* SMS_HOST   = "dashboard.philsms.com";
const String SMS_PATH  = "/api/v3/sms/send";

/* ══════════════════ FIREBASE REALTIME DB ══════════════════ */
const char* FIREBASE_HOST = "https://YOUR_PROJECT_ID-default-rtdb.asia-southeast1.firebasedatabase.app"; // Your Realtime Database URL
const char* FIREBASE_AUTH = "";               // Database secret or web API key (optional)

/* ══════════════════ PINS ══════════════════ */
#define PIR_PIN 5
#define TRIG_PIN 2
#define ECHO_PIN 1
#define LED_PIN 41
#define BUZZER_PIN 42

// Sensor selection: false = ULTRASONIC ONLY (PIR ignored in alarm logic)
// Set to true to re-enable PIR motion detection.
const bool USE_PIR = false;

// Print sensor diagnostics to Serial Monitor every second
const bool DEBUG_SENSORS = true;

WebServer server(80);
unsigned long counter = 0; float distanceValue = 0; bool motion = false; unsigned long start_time;
bool lastPir = false;
unsigned long lastDistanceTime = 0;
const unsigned long DISTANCE_INTERVAL = 60;  // ms between ultrasonic reads (fast response)

float alertThreshold = 200.0;                // 200 cm (2 meters) default detection range
bool soundEnabled = true;
// Allowed range for threshold setting (prevents accidental 0 / out-of-range)
const float THRESHOLD_MIN = 10.0;
const float THRESHOLD_MAX = 400.0;

// Ultrasonic debounce: require N consecutive in-range reads before alarming
int ultraConfirmCount = 0;
const int ULTRA_CONFIRM_READS = 3;

// SMS state (accessed by SMS task on Core 0)
volatile bool smsPending = false;
volatile int  smsResult  = 0;   // 0 idle | 1 sending | 2 sent | 3 failed
volatile int  smsCode    = 0;   // last HTTP status code
String smsQueue = "";
SemaphoreHandle_t sdMutex = NULL;

/* ══════════════════ EVENT MANAGEMENT ══════════════════ */
#define MAX_EVENTS 200
#define LOG_PATH "/log.txt"
struct Event {
    unsigned long id;
    unsigned long uptime;
    float dist;
    bool pir;
    char sev;       // 'H' High | 'M' Medium | 'L' Low
    char type;      // 'B' Both | 'U' Ultrasonic | 'P' PIR | 'I' Intrusion | 'M' Motion
    time_t epoch;   // wall-clock time (0 = unsynced, fallback shows uptime)
};
Event events[MAX_EVENTS];
int eventCount = 0;
unsigned long nextEventId = 1;

// Severity bands scaled for 2 m range: <60cm High (close), <130cm Medium, else Low
char sevOf(float d) { return d < 60 ? 'H' : (d < 130 ? 'M' : 'L'); }

/* ══════════════════ LOUD BUZZER (battery-safe, max volume) ══════════════════
   HARDWARE — required for LOUD sound on 18650x2 (2x3.7V) pack:
   - Do NOT power the buzzer from the ESP32 GPIO pin directly (GPIO ≈ 12 mA max).
   - Use a 5V ACTIVE buzzer (or 3-12V passive piezo) driven through an NPN
     transistor (2N2222 / S8050 / BC547):
       Battery+ (4.2-8.4V / VIN 5V) → Buzzer+ | Buzzer- → transistor COLLECTOR
       transistor EMITTER → GND | GPIO42 → 1 kΩ → transistor BASE
     Add 1N4148/1N4007 diode across buzzer pins (cathode to +) for active buzzers.
   - Common GND between ESP32, battery module, and buzzer is mandatory.
   - Passive piezo: firmware below drives it at resonance (~2.5–3.2 kHz) with
     50% PWM = loudest possible in software. Active buzzer: the siren warble
     still makes it louder & more noticeable than steady DC.
   - If brownout resets occur on alarm: add 470–1000 µF cap across battery
     output and keep buzzer leads short/thick.
*/
const int BUZZER_FREQ_LO = 2500;   // resonance low  — max SPL
const int BUZZER_FREQ_HI = 3200;   // resonance high — max SPL
const unsigned long BUZZER_WARBLE_MS = 250;  // siren alternation
inline void buzzerLoudTick() {
    // Non-blocking siren: alternates frequency every 250 ms (no delay())
    int f = ((millis() / BUZZER_WARBLE_MS) % 2 == 0) ? BUZZER_FREQ_LO : BUZZER_FREQ_HI;
    tone(BUZZER_PIN, f);   // 50% duty square wave = full volume
}
inline void buzzerOff() {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
}
void logToSD(Event e);
void logToFirebase(Event e);
// Forward declarations using custom type Event MUST sit after the struct above,
// otherwise Arduino's auto-generated prototypes (inserted at file top, before
// struct Event) fail with "'Event' was not declared in this scope".
String eventToJson(Event &e);

// Event queue for Core 0 async cloud worker
volatile bool cloudEventPending = false;
Event pendingCloudEvent;

void addEvent(float dist, bool pir, char type) {
    if (eventCount >= MAX_EVENTS) {
        memmove(&events[0], &events[1], sizeof(Event) * (MAX_EVENTS - 1));
        eventCount = MAX_EVENTS - 1;
    }
    Event e;
    e.id = nextEventId++;
    e.uptime = (millis() - start_time) / 1000;
    e.dist = dist;
    e.pir = pir;
    e.sev = sevOf(dist);
    e.type = type;
    e.epoch = timeSynced ? time(nullptr) : 0;
    events[eventCount++] = e;
    logToSD(e);

    // Queue for Firebase sync on Core 0
    pendingCloudEvent = e;
    cloudEventPending = true;
}

void clearEvents() {
    eventCount = 0;
    if (SD_MMC.exists(LOG_PATH)) SD_MMC.remove(LOG_PATH);
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>S3-GUARD | IDS Dashboard</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
:root{
  --bg:#f4f6f9;--surface:#ffffff;--card:#ffffff;--card2:#eef1f5;
  --primary:#2563eb;--cyan:#0891b2;--green:#059669;--red:#dc2626;--orange:#d97706;--violet:#7c3aed;
  --text:#1a2332;--muted:#64748b;--border:rgba(15,23,42,0.08);--glow:0 0 40px rgba(37,99,235,0.10)
}
html,body{height:100%}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);display:flex;overflow:hidden}
::-webkit-scrollbar{width:4px}::-webkit-scrollbar-thumb{background:var(--primary);border-radius:4px}

/* ── SIDEBAR ── */
.sidebar{
  width:72px;background:linear-gradient(180deg,#ffffff,var(--surface));
  border-right:1px solid var(--border);display:flex;flex-direction:column;align-items:center;
  padding:16px 0;gap:4px;flex-shrink:0;z-index:100
}
.sidebar .brand{font-size:1.5rem;margin-bottom:16px;padding:8px;background:linear-gradient(135deg,var(--primary),var(--violet));border-radius:14px;width:44px;height:44px;display:flex;align-items:center;justify-content:center}
.sidebar .nav-item{
  width:48px;height:48px;border-radius:14px;display:flex;align-items:center;justify-content:center;
  color:var(--muted);cursor:pointer;transition:all 0.25s;position:relative;font-size:1.2rem
}
.sidebar .nav-item:hover{color:var(--text);background:rgba(15,23,42,0.04)}
.sidebar .nav-item.active{color:var(--primary);background:rgba(37,99,235,0.10)}
.sidebar .nav-item.active::before{content:'';position:absolute;left:0;top:50%;transform:translateY(-50%);width:3px;height:20px;background:var(--primary);border-radius:0 3px 3px 0}
.sidebar .bottom{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;gap:8px}
.sidebar .bottom .nav-item{font-size:0.95rem}
.sidebar .status-dot{width:8px;height:8px;border-radius:50%;animation:blink 2s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0.3}}

/* ── HAMBURGER ── */
.hamburger{display:none;flex-direction:column;gap:4px;cursor:pointer;padding:4px;background:none;border:none;flex-shrink:0}
.hamburger span{display:block;width:20px;height:2px;background:var(--text);border-radius:2px;transition:all 0.3s}
.hamburger.open span:nth-child(1){transform:rotate(45deg) translate(4px,4px)}
.hamburger.open span:nth-child(2){opacity:0}
.hamburger.open span:nth-child(3){transform:rotate(-45deg) translate(4px,-4px)}
.sidebar-overlay{display:none;position:fixed;inset:0;background:rgba(15,23,42,0.35);z-index:99;opacity:0;transition:opacity 0.3s}
.sidebar-overlay.show{display:block;opacity:1}

/* ── MAIN ── */
.main{flex:1;display:flex;flex-direction:column;overflow-y:auto;height:100vh}
.topbar{
  position:sticky;top:0;z-index:50;display:flex;align-items:center;justify-content:space-between;
  padding:14px 28px;background:rgba(255,255,255,0.7);backdrop-filter:blur(16px);border-bottom:1px solid var(--border);flex-shrink:0
}
.topbar-left{display:flex;align-items:center;gap:10px}
.topbar-left h1{font-size:1.15rem;font-weight:700;background:linear-gradient(135deg,var(--text),var(--muted));-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.topbar-right{display:flex;align-items:center;gap:14px}
.clock{font-size:0.78rem;color:var(--muted);font-variant-numeric:tabular-nums}
.status-badge{display:flex;align-items:center;gap:6px;padding:5px 12px;border-radius:20px;background:rgba(5,150,105,0.10);border:1px solid rgba(5,150,105,0.20);font-size:0.68rem;font-weight:600;color:var(--green)}
.status-badge.offline{background:rgba(220,38,38,0.08);border-color:rgba(220,38,38,0.18);color:var(--red)}

/* ── PAGES ── */
.pages{flex:1;padding:24px 28px;max-width:1300px;width:100%;margin:0 auto}
.page{display:none;animation:fadeIn 0.3s ease}
.page.active{display:block}
@keyframes fadeIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}

/* ── KPI GRID ── */
.kpi-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin-bottom:20px}
.kpi-card{
  background:var(--card);border-radius:16px;padding:18px 20px;border:1px solid var(--border);
  position:relative;overflow:hidden;transition:border-color 0.3s;box-shadow:0 1px 3px rgba(15,23,42,0.04)
}
.kpi-card::after{
  content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,transparent,var(--accent,var(--primary)),transparent);opacity:0;transition:opacity 0.3s
}
.kpi-card:hover::after{opacity:1}
.kpi-card .label{font-size:0.65rem;color:var(--muted);text-transform:uppercase;letter-spacing:0.7px;margin-bottom:6px}
.kpi-card .value{font-size:1.8rem;font-weight:700;line-height:1.2}
.kpi-card .sub{font-size:0.7rem;color:var(--muted);margin-top:4px}
.kpi-card .icon{position:absolute;right:16px;top:50%;transform:translateY(-50%);font-size:1.8rem;opacity:0.10}

/* ── CONTENT GRID ── */
.content-grid{display:grid;grid-template-columns:1fr 360px;gap:16px;margin-bottom:16px}
.card{
  background:var(--card);border-radius:16px;padding:20px;border:1px solid var(--border);margin-bottom:16px;
  transition:border-color 0.3s;box-shadow:0 1px 3px rgba(15,23,42,0.04)
}
.card:hover{border-color:rgba(37,99,235,0.18)}
.card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px}
.card-title{font-size:0.85rem;font-weight:600;display:flex;align-items:center;gap:8px}
.card-title .accent{color:var(--muted);font-weight:400;font-size:0.73rem}

/* ── CHART ── */
canvas{width:100%!important;border-radius:10px;display:block}
.chart-box{height:150px;margin-bottom:12px}

/* ── RADAR ── */
.radar-box{height:110px;background:#eef2f7;border-radius:12px;overflow:hidden;position:relative;border:1px solid var(--border);margin-bottom:12px}
.radar-fill{position:absolute;bottom:0;left:0;right:0;transition:height 0.5s cubic-bezier(0.34,1.56,0.64,1);background:linear-gradient(to top,var(--primary),rgba(37,99,235,0.05))}
.radar-value{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:2rem;font-weight:800;color:var(--text);text-shadow:0 2px 12px rgba(255,255,255,0.8)}

/* ── ALERT BANNER ── */
.alert-banner{
  display:none;align-items:center;gap:12px;padding:14px 18px;margin-bottom:16px;
  background:linear-gradient(135deg,rgba(220,38,38,0.08),rgba(220,38,38,0.02));
  border:1px solid rgba(220,38,38,0.22);border-radius:12px;animation:slideDown 0.3s ease
}
.alert-banner.show{display:flex}
@keyframes slideDown{from{opacity:0;transform:translateY(-10px)}to{opacity:1;transform:translateY(0)}}
.alert-banner .icon{font-size:1.6rem}
.alert-banner .info h4{color:var(--red);font-size:0.9rem}
.alert-banner .info p{font-size:0.73rem;color:var(--muted);margin-top:2px}
.alert-banner .sms-status{margin-left:auto;font-size:0.68rem;font-weight:700;padding:3px 10px;border-radius:20px}

/* ── TABLE ── */
.table-wrap{overflow-x:auto}
table{width:100%;border-collapse:collapse}
th{font-size:0.62rem;text-transform:uppercase;color:var(--muted);letter-spacing:0.8px;padding:8px 12px;border-bottom:1px solid var(--border);text-align:left;font-weight:600}
td{padding:10px 12px;font-size:0.82rem;border-bottom:1px solid rgba(15,23,42,0.04)}
tr:hover td{background:rgba(15,23,42,0.02)}
.badge{padding:2px 10px;border-radius:20px;font-size:0.65rem;font-weight:700}
.badge-red{background:rgba(220,38,38,0.10);color:var(--red)}
.badge-grn{background:rgba(5,150,105,0.10);color:var(--green)}
.badge-org{background:rgba(217,119,6,0.10);color:var(--orange)}

/* ── DUAL STATUS ── */
.dual-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:2px}
.stat-block{background:var(--card2);padding:12px;border-radius:12px;text-align:center}
.stat-block .stat-label{font-size:0.62rem;color:var(--muted);text-transform:uppercase;margin-bottom:5px}
.stat-block .stat-value{font-size:0.85rem;font-weight:700}

/* ── HISTORY ── */
.history-item{
  display:flex;align-items:center;gap:14px;padding:12px 14px;background:var(--card2);
  border-radius:10px;margin-bottom:6px;transition:transform 0.2s
}
.history-item:hover{transform:translateX(4px)}
.history-item .h-icon{width:34px;height:34px;border-radius:10px;display:flex;align-items:center;justify-content:center;font-size:1rem;flex-shrink:0}
.history-item .h-body{flex:1;min-width:0}
.history-item .h-title{font-size:0.82rem;font-weight:600}
.history-item .h-meta{font-size:0.68rem;color:var(--muted);margin-top:2px}
.history-item .h-right{text-align:right;flex-shrink:0}
.filter-bar{display:flex;gap:8px;margin-bottom:16px;flex-wrap:wrap}
.filter-bar span{padding:4px 14px;border-radius:20px;font-size:0.72rem;font-weight:600;cursor:pointer;transition:all 0.2s;border:1px solid transparent;color:var(--muted)}
.filter-bar span:hover{color:var(--text);background:rgba(15,23,42,0.04)}
.filter-bar .active{background:rgba(220,38,38,0.08);color:var(--red);border-color:rgba(220,38,38,0.20)}

/* ── SYSTEM INFO ── */
.info-row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid var(--border);font-size:0.8rem}
.info-row:last-child{border:none}
.info-row .il{color:var(--muted)}
.info-row .iv{font-weight:600;font-family:monospace}
.hw-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.hw-item{padding:14px;background:var(--card2);border-radius:12px;display:flex;justify-content:space-between;align-items:center}
.hw-item .hw-name{font-size:0.8rem;font-weight:600}
.hw-item .hw-desc{font-size:0.65rem;color:var(--muted);margin-top:2px}

/* ── SETTINGS ── */
.setting-row{display:flex;justify-content:space-between;align-items:center;padding:14px 0;border-bottom:1px solid var(--border);gap:14px}
.setting-row:last-child{border:none}
.setting-label{font-size:0.85rem;font-weight:600}
.setting-desc{font-size:0.68rem;color:var(--muted);margin-top:2px}
.setting-input{background:var(--card2);border:1px solid var(--border);border-radius:8px;padding:8px 12px;color:var(--text);font-size:0.82rem;outline:none;width:70px;font-family:monospace}
.setting-input:focus{border-color:var(--primary)}
.toggle{width:40px;height:22px;background:var(--green);border-radius:11px;position:relative;cursor:pointer;border:none;transition:0.2s;flex-shrink:0}
.toggle::after{content:'';position:absolute;top:3px;right:3px;width:16px;height:16px;background:#fff;border-radius:8px;transition:0.2s}
.toggle.off{background:#cbd5e1}
.toggle.off::after{right:auto;left:3px}
.ip-input{flex:1;background:var(--card2);border:1px solid var(--border);border-radius:8px;padding:9px 12px;color:var(--text);font-size:0.82rem;outline:none;font-family:monospace}
.ip-input:focus{border-color:var(--primary)}
.btn{background:var(--primary);border:none;border-radius:8px;padding:9px 16px;color:#fff;font-weight:600;cursor:pointer;font-size:0.82rem;transition:all 0.2s}
.btn:hover{background:#1d4ed8;box-shadow:0 4px 20px rgba(37,99,235,0.25)}
.btn-danger{background:rgba(220,38,38,0.10);color:var(--red);border:1px solid rgba(220,38,38,0.25)}
.btn-danger:hover{background:var(--red);color:#fff;box-shadow:0 4px 20px rgba(220,38,38,0.25)}
.btn-sm{padding:5px 12px;font-size:0.72rem}

/* ── RESPONSIVE ── */
@media(max-width:1100px){.content-grid{grid-template-columns:1fr}.kpi-grid{grid-template-columns:repeat(2,1fr)}}
@media(max-width:700px){
  .hamburger{display:flex}
  .sidebar{position:fixed;left:-80px;top:0;bottom:0;height:100vh;transition:left 0.3s;z-index:100;box-shadow:4px 0 30px rgba(0,0,0,0.12)}
  .sidebar.open{left:0}
  .sidebar .nav-item.active::before{left:auto;right:0;border-radius:3px 0 0 3px}
  .pages{padding:14px 16px}.topbar{padding:10px 16px;gap:8px}
  .topbar-left h1{font-size:0.85rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:120px}
  .topbar-right{gap:8px}.clock{font-size:0.65rem}
  .kpi-grid{gap:10px;grid-template-columns:repeat(2,1fr)}
  .kpi-card{padding:14px 16px}.kpi-card .value{font-size:1.3rem}
  .dual-grid{grid-template-columns:1fr}.hw-grid{grid-template-columns:1fr}
  .filter-bar{gap:6px}.filter-bar span{font-size:0.65rem;padding:3px 10px}
  .history-item{padding:10px 12px}.history-item .h-title{font-size:0.75rem}
}
@media(max-width:480px){
  .kpi-grid{grid-template-columns:1fr}
  .topbar-left h1{max-width:90px;font-size:0.78rem}
  .status-badge{padding:3px 8px;font-size:0.6rem}
  .clock{display:none}
}
</style>
</head>
<body>

<!-- SIDEBAR -->
<div class="sidebar">
  <div class="brand">&#x1f6e1;</div>
  <div class="nav-item active" onclick="showPage('dashboard',this)" title="Dashboard">&#x1f4ca;</div>
  <div class="nav-item" onclick="showPage('history',this)" title="History">&#x1f4c5;</div>
  <div class="nav-item" onclick="showPage('analytics',this)" title="Analytics">&#x1f4c8;</div>
  <div class="nav-item" onclick="showPage('system',this)" title="System">&#x2699;</div>
  <div class="bottom">
    <div class="nav-item" id="side-status" title="Status" style="pointer-events:none;color:var(--green)">&#x25cf;</div>
  </div>
</div>

<div class="sidebar-overlay" id="sidebar-overlay" onclick="toggleSidebar()"></div>
<!-- MAIN -->
<div class="main">
  <div class="topbar">
    <div class="topbar-left">
      <button class="hamburger" id="hamburger" onclick="toggleSidebar()" aria-label="Toggle menu"><span></span><span></span><span></span></button>
      <h1>S3-GUARD</h1>
    </div>
    <div class="topbar-right">
      <div class="status-badge" id="top-status">
        <span style="font-size:0.7rem">&#x25cf;</span> <span id="status-text">ONLINE</span>
      </div>
      <span class="clock" id="clock">--:--:--</span>
    </div>
  </div>

  <div class="pages">

  <!-- ═══ DASHBOARD ═══ -->
  <div class="page active" id="page-dashboard">
    <div class="alert-banner" id="alert-banner">
      <div class="icon">&#x26a1;</div>
      <div class="info">
        <h4>Intrusion Detected</h4>
        <p id="banner-meta">Object detected within range</p>
      </div>
      <span class="sms-status" id="banner-sms" style="display:none"></span>
    </div>

    <div class="kpi-grid">
      <div class="kpi-card" style="--accent:var(--red)"><div class="icon">&#x1f6a8;</div><div class="label">Total Intrusions</div><div class="value" id="kpi-count">0</div><div class="sub">This session</div></div>
      <div class="kpi-card" style="--accent:var(--cyan)"><div class="icon">&#x1f4e1;</div><div class="label">Current Distance</div><div class="value" id="kpi-dist">-- <span style="font-size:0.9rem">cm</span></div><div class="sub">Ultrasonic sensor</div></div>
      <div class="kpi-card" style="--accent:var(--green)"><div class="icon">&#x23f1;</div><div class="label">Uptime</div><div class="value" id="kpi-up">0s</div><div class="sub">Since last boot</div></div>
      <div class="kpi-card" style="--accent:var(--violet)"><div class="icon">&#x1f4f6;</div><div class="label">Network</div><div class="value" id="kpi-rssi" style="font-size:1.1rem">--</div><div class="sub" id="kpi-rssi-sub">Signal</div></div>
    </div>

    <div class="content-grid">
      <div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f4e1; Live Distance <span class="accent">Last 60s</span></span></div>
          <div class="chart-box"><canvas id="telemetry-chart"></canvas></div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f4cb; Recent Events</span><span style="font-size:0.72rem;color:var(--muted)" id="landing-count">0 events</span></div>
          <div class="table-wrap" style="max-height:300px;overflow-y:auto" id="landing-events">
            <div class="empty-state" id="recent-empty" style="text-align:center;padding:30px;color:var(--muted);font-size:0.82rem">No intrusions recorded this session.</div>
            <div id="recent-list"></div>
          </div>
        </div>
      </div>
      <div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f4cd; Proximity Radar</span></div>
          <div class="radar-box">
            <div class="radar-fill" id="radar-fill" style="height:0%"></div>
            <div class="radar-value"><span id="radar-dist">--</span><span style="font-size:0.9rem">cm</span></div>
          </div>
          <div class="dual-grid">
            <div class="stat-block"><div class="stat-label">PIR Status</div><div class="stat-value" id="pir-status" style="color:var(--green)">CLEAR</div></div>
            <div class="stat-block"><div class="stat-label">Zone</div><div class="stat-value" id="zone-status" style="color:var(--green)">SAFE</div></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f4bb; Device</span></div>
          <div><div class="info-row"><span class="il">Device ID</span><span class="iv">S3-GUARD-01</span></div>
          <div class="info-row"><span class="il">Model</span><span class="iv">ESP32-S3</span></div>
          <div class="info-row"><span class="il">Firmware</span><span class="iv">v2.1.0</span></div>
          <div class="info-row"><span class="il">Alert Range</span><span class="iv" style="color:var(--orange)">&le; 200 cm (2 m)</span></div></div>
        </div>
      </div>
    </div>
  </div>

  <!-- ═══ HISTORY ═══ -->
  <div class="page" id="page-history">
    <div class="card">
      <div class="card-header"><span class="card-title">&#x1f4c5; Event Timeline</span><div style="display:flex;align-items:center;gap:10px"><span style="font-size:0.72rem;color:var(--muted)" id="history-count">0 events</span><button class="btn btn-danger btn-sm" onclick="clearEventsUI()">Clear All</button></div></div>
      <div class="filter-bar">
        <span class="active" data-filter="all" onclick="filterHistory('all',this)">All</span>
        <span data-filter="High" onclick="filterHistory('High',this)">High</span>
        <span data-filter="Medium" onclick="filterHistory('Medium',this)">Medium</span>
        <span data-filter="Low" onclick="filterHistory('Low',this)">Low</span>
      </div>
      <div id="history-list"><div class="empty-state" style="text-align:center;padding:30px;color:var(--muted);font-size:0.82rem">Waiting for events...</div></div>
    </div>
  </div>

  <!-- ═══ ANALYTICS ═══ -->
  <div class="page" id="page-analytics">
    <div class="kpi-grid" style="grid-template-columns:repeat(3,1fr)">
      <div class="kpi-card" style="--accent:var(--orange)"><div class="icon">&#x2b06;</div><div class="label">Peak Distance</div><div class="value" id="an-peak">--<span style="font-size:0.9rem">cm</span></div></div>
      <div class="kpi-card" style="--accent:var(--cyan)"><div class="icon">&#x2b07;</div><div class="label">Min Distance</div><div class="value" id="an-min">--<span style="font-size:0.9rem">cm</span></div></div>
      <div class="kpi-card" style="--accent:var(--violet)"><div class="icon">&#x25cf;</div><div class="label">Avg Distance</div><div class="value" id="an-avg">--<span style="font-size:0.9rem">cm</span></div></div>
    </div>
    <div class="card">
      <div class="card-header"><span class="card-title">&#x1f4c8; Distance Distribution</span></div>
      <div class="chart-box" style="height:180px"><canvas id="analytics-chart"></canvas></div>
    </div>
    <div class="card">
      <div class="card-header"><span class="card-title">&#x1f4ca; Session Statistics</span></div>
      <div id="stats-summary" style="font-size:0.82rem;color:var(--muted);text-align:center;padding:16px">Collecting data...</div>
    </div>
  </div>

  <!-- ═══ SYSTEM ═══ -->
  <div class="page" id="page-system">
    <div class="content-grid">
      <div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f50c; Hardware Matrix</span></div>
          <div class="hw-grid">
            <div class="hw-item"><div><div class="hw-name">PIR Sensor</div><div class="hw-desc">GPIO 5 - Motion</div></div><span class="badge badge-grn" id="hw-pir">ACTIVE</span></div>
            <div class="hw-item"><div><div class="hw-name">Ultrasonic</div><div class="hw-desc">Trig:2 Echo:1</div></div><span class="badge badge-grn" id="hw-ultra">ACTIVE</span></div>
            <div class="hw-item"><div><div class="hw-name">LED</div><div class="hw-desc">GPIO 41 - Alert</div></div><span class="badge" id="hw-led" style="background:rgba(15,23,42,0.05);color:var(--muted)">STANDBY</span></div>
            <div class="hw-item"><div><div class="hw-name">Buzzer</div><div class="hw-desc">GPIO 42 - Sound</div></div><span class="badge" id="hw-buzzer" style="background:rgba(15,23,42,0.05);color:var(--muted)">STANDBY</span></div>
          </div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x2699; Configuration</span></div>
          <div class="setting-row"><div><div class="setting-label">Alert Threshold</div><div class="setting-desc">Intrusion trigger distance</div></div><div style="display:flex;align-items:center;gap:6px"><input class="setting-input" id="threshold-val" type="number" value="200" min="10" max="400" onchange="setThresholdVal(this.value)"><span style="font-size:0.75rem;color:var(--muted)">cm</span></div></div>
          <div class="setting-row"><div><div class="setting-label">Sound Alert</div><div class="setting-desc">Buzzer on detection</div></div><button class="toggle" id="tog-sound" onclick="toggleSound(this)"></button></div>
          <div class="setting-row"><div><div class="setting-label">Auto Refresh</div><div class="setting-desc">Reload logs every 10s</div></div><button class="toggle" id="tog-refresh"></button></div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f5c2; Event Management</span></div>
          <div class="info-row"><span class="il">Stored Events</span><span class="iv" id="mgr-total">--</span></div>
          <div class="info-row"><span class="il">Last Event Uptime</span><span class="iv" id="mgr-last">--</span></div>
          <div style="display:flex;gap:8px;margin-top:12px">
            <button class="btn btn-sm" onclick="syncEvents();el('mgr-msg').innerText='Events refreshed.'">Refresh</button>
            <button class="btn btn-danger btn-sm" onclick="clearEventsUI()">Clear All Events</button>
          </div>
          <div id="mgr-msg" style="font-size:0.68rem;color:var(--muted);margin-top:8px"></div>
        </div>
      </div>
      <div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f4f6; Connection</span></div>
          <div class="info-row"><span class="il">Mode</span><span class="iv" id="dev-net">--</span></div>
          <div class="info-row"><span class="il">ESP32 IP</span><span class="iv" id="dev-ip">--</span></div>
          <div class="info-row"><span class="il">RSSI</span><span class="iv" id="dev-rssi">--</span></div>
          <div class="info-row"><span class="il">Uptime</span><span class="iv" id="dev-up">--</span></div>
          <div class="info-row"><span class="il">Intrusions</span><span class="iv" id="dev-count">--</span></div>
          <div style="margin-top:14px"><div style="font-size:0.68rem;color:var(--muted);margin-bottom:6px">Signal</div>
          <div style="height:4px;background:rgba(15,23,42,0.06);border-radius:2px;overflow:hidden"><div id="rssi-bar" style="height:100%;transition:0.5s;width:0%"></div></div></div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f4f1; SMS Alerts (PhilSMS)</span></div>
          <div class="info-row"><span class="il">Status</span><span class="iv" id="sms-status">Idle</span></div>
          <div class="info-row"><span class="il">HTTP Code</span><span class="iv" id="sms-code">--</span></div>
          <div class="info-row"><span class="il">Recipient</span><span class="iv">YOUR_PHONE_NUMBER</span></div>
          <div class="info-row"><span class="il">Sender ID</span><span class="iv">PhilSMS</span></div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x1f310; Remote Access</span></div>
          <div style="display:flex;gap:8px"><input class="ip-input" id="settings-ip" type="text" placeholder="192.168.4.1" value="192.168.4.1"><button class="btn" onclick="applyIPFromSettings()">Connect</button></div>
          <div style="margin-top:12px;display:flex;align-items:center;gap:8px">
            <button class="toggle off" id="tog-debug" onclick="toggleDebug(this)"></button>
            <span style="font-size:0.72rem;color:var(--muted)">Show IP config panel</span>
          </div>
        </div>
        <div class="card">
          <div class="card-header"><span class="card-title">&#x2139; About</span></div>
          <div style="font-size:0.75rem;color:var(--muted);line-height:1.6">
          <strong style="color:var(--text)">S3-GUARD v2.1</strong><br>
          ESP32-S3 Intrusion Detection System<br>
          PIR + Ultrasonic + SD Logging + SMS Alert<br>
          <span style="font-size:0.68rem">&#x1f4a1; GPIO 41 | &#x1f50a; GPIO 42</span>
          </div>
        </div>
      </div>
    </div>
  </div>

  </div><!-- /pages -->
</div><!-- /main -->

<!-- IP CONFIG (debug) -->
<div id="ip-config" style="display:none;position:fixed;bottom:20px;right:20px;background:var(--card);border:1px solid rgba(37,99,235,0.4);border-radius:14px;padding:14px 18px;z-index:9999;box-shadow:0 10px 50px rgba(15,23,42,0.18)">
  <p style="font-size:0.7rem;color:var(--muted);margin-bottom:8px">Enter ESP32 IP address</p>
  <div style="display:flex;gap:6px"><input id="esp-ip" type="text" placeholder="192.168.4.1" value="192.168.4.1" style="background:var(--card2);border:1px solid var(--border);border-radius:8px;padding:6px 10px;color:var(--text);font-size:0.8rem;outline:none;font-family:monospace;width:130px"><button onclick="applyIP()" style="background:var(--primary);border:none;border-radius:8px;padding:6px 12px;color:#fff;font-weight:600;cursor:pointer;font-size:0.8rem">Go</button></div>
</div>

<script>
var ESP_IP = '';
var urlParams = new URLSearchParams(window.location.search);
var qip = urlParams.get('ip') || urlParams.get('esp') || '';
if(qip) { ESP_IP = qip.indexOf('http')===0?qip:'http://'+qip; document.getElementById('ip-config').style.display='block'; document.getElementById('esp-ip').value=qip.replace(/^https?:\/\//,''); }
else if(window.location.hostname==='192.168.4.1'||window.location.hostname===''){ESP_IP='';}
else{document.getElementById('ip-config').style.display='block';}
var serverEvents=[],serverStats=null,distHistory=[],lastMotion=!1,threshold=200,soundOn=!0,currentFilter='all';
function fmtDT(a){if(a.dt&&a.dt.length)return a.dt+(a.day?' ('+a.day+')':'');return 'Up: '+fmtTime(a.u)}
function makeChart(a,b,c){var d=document.getElementById(a),e=d.getContext('2d');return{canvas:d,ctx:e,data:new Array(60).fill(null),label:b,color:c}}
var telChart=makeChart('telemetry-chart','Distance','#2563eb'),anaChart=makeChart('analytics-chart','Distance','#7c3aed');
function drawChart(a){var b=a.canvas,c=a.ctx,d=a.data,e=a.color,f=window.devicePixelRatio||1,g=b.parentElement;b.width=g.clientWidth*f;b.height=g.clientHeight*f;c.scale(f,f);var h=g.clientWidth,i=g.clientHeight;c.clearRect(0,0,h,i);var j=d.filter(function(a){return a!==null});if(j.length<2){c.fillStyle='rgba(15,23,42,0.25)';c.font='12px sans-serif';c.textAlign='center';c.fillText('Waiting for data...',h/2,i/2);return}var k=Math.max.apply(null,j.concat([100])),l=h/(d.length-1);c.strokeStyle='rgba(15,23,42,0.06)';c.lineWidth=1;for(var m=0;m<=4;m++){var n=i-(m/4)*(i-20)-4;c.beginPath();c.moveTo(0,n);c.lineTo(h,n);c.stroke()}var o=c.createLinearGradient(0,0,0,i);o.addColorStop(0,e+'30');o.addColorStop(1,'transparent');c.beginPath();var p=!1;d.forEach(function(a,b){if(a===null)return;var f=b*l,g=i-(a/k)*(i-20)-4;if(!p){c.moveTo(f,i);c.lineTo(f,g);p=!0}else c.lineTo(f,g)});c.lineTo((d.length-1)*l,i);c.closePath();c.fillStyle=o;c.fill();c.beginPath();p=!1;d.forEach(function(a,b){if(a===null)return;var f=b*l,g=i-(a/k)*(i-20)-4;if(!p){c.moveTo(f,g);p=!0}else c.lineTo(f,g)});c.strokeStyle=e;c.lineWidth=2;c.lineJoin='round';c.lineCap='round';c.stroke()}
function update(){fetch(ESP_IP+'/status').then(function(a){return a.json()}).then(function(a){setOnline(!0);var b=Math.round(a.dist),c=a.motion===!0||a.motion==='true',pir=a.pir===!0||a.pir==='true';if(a.threshold!==undefined)threshold=a.threshold;if(a.sound!==undefined){soundOn=a.sound===!0||a.sound==='true';var t=el('tog-sound');if(t){if(soundOn)t.classList.remove('off');else t.classList.add('off')}}telChart.data.push(b>=400?null:b);if(telChart.data.length>60)telChart.data.shift();distHistory.push(b);if(distHistory.length>500)distHistory.shift();el('kpi-count').innerText=a.count;el('kpi-dist').innerHTML=b+'<span style="font-size:0.9rem">cm</span>';el('kpi-up').innerText=fmtTime(a.uptime);if(a.net&&a.net==='online'){el('kpi-rssi').innerHTML='<span style="font-size:1rem">Internet</span>';el('kpi-rssi-sub').innerText=a.rssi+' dBm'}else{el('kpi-rssi').innerText='Local';el('kpi-rssi-sub').innerText='AP mode'}el('radar-dist').innerText=b;var d=Math.max(0,Math.min(100,100-b/400*100));el('radar-fill').style.height=d+'%';el('radar-fill').style.background=b<=threshold?'linear-gradient(to top,rgba(220,38,38,0.8),rgba(220,38,38,0.1))':'linear-gradient(to top,rgba(37,99,235,0.8),rgba(37,99,235,0.1))';el('pir-status').innerText=pir?'TRIGGERED':'CLEAR';el('pir-status').style.color=pir?'var(--red)':'var(--green)';el('zone-status').innerText=b<=threshold?'BREACH':'SAFE';el('zone-status').style.color=b<=threshold?'var(--red)':'var(--green)';var e=el('alert-banner');if(c){e.classList.add('show');if(pir&&b<=threshold){el('banner-meta').innerText='PIR & Ultrasonic object at '+b+'cm'}else if(pir){el('banner-meta').innerText='PIR motion detected'}else{el('banner-meta').innerText='Ultrasonic object detected at '+b+'cm'}}else{e.classList.remove('show')}if(a.sms!==void 0){updateSMS(a)}lastMotion=c;el('hw-pir').className='badge '+(pir?'badge-red':'badge-grn');el('hw-pir').innerText=pir?'DETECTED':'ACTIVE';el('hw-ultra').className='badge '+(b<=threshold?'badge-red':'badge-grn');el('hw-ultra').innerText=b<=threshold?'DETECTED':'ACTIVE';el('hw-led').className='badge '+(c?'badge-red':'badge-grn');el('hw-led').innerText=c?'ALARM (ON)':'STANDBY';el('hw-buzzer').className='badge '+(c&&soundOn?'badge-red':'');el('hw-buzzer').style.cssText=c&&soundOn?'':'background:rgba(15,23,42,0.05);color:var(--muted)';el('hw-buzzer').innerText=c&&soundOn?'ALARM (ON)':'STANDBY';el('dev-net').innerText=a.net==='online'?'Online (WiFi STA)':'Local (AP)';el('dev-ip').innerText=ESP_IP||window.location.hostname;el('dev-rssi').innerText=(a.rssi||'--')+' dBm';el('dev-up').innerText=fmtTime(a.uptime);el('dev-count').innerText=a.count;if(a.rssi){var f=Math.min(100,Math.max(0,(a.rssi+100)*2));el('rssi-bar').style.width=f+'%';el('rssi-bar').style.background=f>60?'var(--green)':f>30?'var(--orange)':'var(--red)'}drawChart(telChart);}).catch(function(){setOnline(!1)})}
function setThresholdVal(v){threshold=parseFloat(v)||200;fetch(ESP_IP+'/threshold?val='+threshold,{method:'POST'}).catch(function(){})}
function dlCSV(){var rows=[['id','datetime','day','date','time','distance_cm','pir','severity','type']];serverEvents.forEach(function(a){rows.push([a.id,a.dt||'',a.day||'',a.date||'',a.time||'',Math.round(a.d),a.p?'1':'0',sevLabel(a.s),a.t])});var csv=rows.map(function(r){return r.join(',')}).join('\n');var b=new Blob([csv],{type:'text/csv'});var u=URL.createObjectURL(b);var l=document.createElement('a');l.href=u;l.download='s3-guard-report.csv';l.click();setTimeout(function(){URL.revokeObjectURL(u)},2000)}
function toggleSound(btn){btn.classList.toggle('off');var en=!btn.classList.contains('off');fetch(ESP_IP+'/sound?enable='+en,{method:'POST'}).catch(function(){})}
function updateSMS(a){var s=el('sms-status'),c=el('sms-code'),b=el('banner-sms');if(a.sms===1){s.innerText='Sending...';s.style.color='var(--orange)';b.style.display='none'}else if(a.sms===2){s.innerText='Sent';s.style.color='var(--green)';c.innerText=a.smsCode;b.innerText='SMS sent';b.style.display='inline-block';b.style.background='rgba(5,150,105,0.1)';b.style.color='var(--green)'}else if(a.sms===3){s.innerText='Failed';s.style.color='var(--red)';c.innerText=a.smsCode;b.innerText='SMS failed';b.style.display='inline-block';b.style.background='rgba(220,38,38,0.1)';b.style.color='var(--red)'}else{s.innerText='Idle';s.style.color='var(--muted)';c.innerText='--'}}
function syncEvents(){fetch(ESP_IP+'/events').then(function(a){return a.json()}).then(function(a){serverEvents=a;renderEventLog()}).catch(function(){});fetch(ESP_IP+'/events/stats').then(function(a){return a.json()}).then(function(a){serverStats=a;renderStats()}).catch(function(){})}
function sevLabel(a){return a==='H'?'High':a==='M'?'Medium':'Low'}
function sevCls(a){return a==='H'?'badge-red':a==='M'?'badge-org':'badge-grn'}
function renderEventLog(){renderRecent(serverEvents);renderHistory(serverEvents)}
function renderRecent(a){var b=el('recent-list'),c=el('recent-empty');el('landing-count').innerText=a.length+' events';if(!a.length){c.style.display='block';return}c.style.display='none';b.innerHTML='';a.slice(0,25).forEach(function(a){var g=a.s==='H'?'#dc2626':a.s==='M'?'#d97706':'#059669',d=(a.t==='B'||a.t==='I')?'&#x26a0;':(a.t==='U'?'&#x1f4e1;':'&#x1f440;'),f=fmtDT(a)+' | ID:'+a.id,title=a.t==='B'?'Both: Intrusion at '+Math.round(a.d)+'cm':(a.t==='U'?'Ultrasonic at '+Math.round(a.d)+'cm':(a.t==='P'?'PIR Motion Detected':(a.t==='I'?'Intrusion at '+Math.round(a.d)+'cm':'Motion at '+Math.round(a.d)+'cm')));b.innerHTML+='<div class="history-item"><div class="h-icon" style="background:'+g+'15;color:'+g+'">'+d+'</div><div class="h-body"><div class="h-title">'+title+'</div><div class="h-meta">'+f+'</div></div><div class="h-right"><span class="badge '+sevCls(a.s)+'">'+sevLabel(a.s)+'</span></div></div>'})}
function renderHistory(a){var b=el('history-list');el('history-count').innerText=a.length+' events';var c=a;if(currentFilter!=='all')c=a.filter(function(a){return sevLabel(a.s)===currentFilter});if(c.length===0){b.innerHTML='<div class="empty-state" style="text-align:center;padding:30px;color:var(--muted);font-size:0.82rem">No '+(currentFilter==='all'?'':'matching ')+'events.</div>';return}b.innerHTML='<div style="margin-bottom:10px"><button class="btn btn-sm" onclick="dlCSV()">Download CSV report (day/time)</button></div>';c.slice(0,80).forEach(function(a){var g=a.s==='H'?'#dc2626':a.s==='M'?'#d97706':'#059669',d=(a.t==='B'||a.t==='I')?'&#x26a0;':(a.t==='U'?'&#x1f4e1;':'&#x1f440;'),f=fmtDT(a)+' | ID:'+a.id,title=a.t==='B'?'Both: Intrusion at '+Math.round(a.d)+'cm':(a.t==='U'?'Ultrasonic at '+Math.round(a.d)+'cm':(a.t==='P'?'PIR Motion Detected':(a.t==='I'?'Intrusion at '+Math.round(a.d)+'cm':'Motion at '+Math.round(a.d)+'cm')));b.innerHTML+='<div class="history-item"><div class="h-icon" style="background:'+g+'15;color:'+g+'">'+d+'</div><div class="h-body"><div class="h-title">'+title+'</div><div class="h-meta">'+f+'</div></div><div class="h-right"><span class="badge '+sevCls(a.s)+'">'+sevLabel(a.s)+'</span></div></div>'})}
function filterHistory(a,b){currentFilter=a;var c=document.querySelectorAll('.filter-bar span');c.forEach(function(a){a.classList.remove('active')});if(b)b.classList.add('active');else document.querySelector('.filter-bar [data-filter="'+a+'"]').classList.add('active');renderHistory(serverEvents)}
function renderStats(){if(!serverStats)return;var s=serverStats;el('an-peak').innerHTML=s.peak+'<span style="font-size:0.9rem">cm</span>';el('an-min').innerHTML=s.min+'<span style="font-size:0.9rem">cm</span>';el('an-avg').innerHTML=s.avg+'<span style="font-size:0.9rem">cm</span>';anaChart.data=serverEvents.slice(0,60).map(function(a){return a.d}).reverse();drawChart(anaChart);el('mgr-total').innerText=s.total;el('mgr-last').innerText=s.total?fmtTime(s.lastUptime):'--';el('stats-summary').innerHTML='<span style="display:grid;grid-template-columns:repeat(3,1fr);gap:14px;text-align:center"><span><span style="display:block;font-size:1.4rem;font-weight:700;color:var(--red)">'+s.high+'</span><span style="font-size:0.68rem;color:var(--muted)">High</span></span><span><span style="display:block;font-size:1.4rem;font-weight:700;color:var(--orange)">'+s.medium+'</span><span style="font-size:0.68rem;color:var(--muted)">Medium</span></span><span><span style="display:block;font-size:1.4rem;font-weight:700;color:var(--green)">'+s.low+'</span><span style="font-size:0.68rem;color:var(--muted)">Low</span></span></span><div style="margin-top:14px;padding-top:12px;border-top:1px solid var(--border);display:flex;justify-content:space-between;font-size:0.75rem"><span style="color:var(--muted)">Intrusions (alarm)</span><strong>'+s.intrusions+'</strong></div><div style="display:flex;justify-content:space-between;font-size:0.75rem;margin-top:6px"><span style="color:var(--muted)">Motions (out of range)</span><strong>'+s.motions+'</strong></div><div style="display:flex;justify-content:space-between;font-size:0.75rem;margin-top:6px"><span style="color:var(--muted)">Total Transactions</span><strong>'+s.total+'</strong></div><div style="display:flex;justify-content:space-between;font-size:0.75rem;margin-top:6px"><span style="color:var(--muted)">Min / Peak Distance</span><strong>'+s.min+' / '+s.peak+' cm</strong></div>'}
function clearEventsUI(){if(!confirm('Clear all '+serverEvents.length+' events?'))return;fetch(ESP_IP+'/events/clear',{method:'POST'}).then(function(a){return a.json()}).then(function(){serverEvents=[];serverStats=null;el('kpi-count').innerText='0';renderEventLog();renderStats();syncEvents()}).catch(function(){})}
function setOnline(a){var b=el('top-status'),c=el('side-status');if(a){b.className='status-badge';b.innerHTML='<span style="font-size:0.7rem;color:var(--green)">&#x25cf;</span> <span id="status-text">ONLINE</span>';c.style.color='var(--green)'}else{b.className='status-badge offline';b.innerHTML='<span style="font-size:0.7rem;color:var(--red)">&#x25cf;</span> <span id="status-text">OFFLINE</span>';c.style.color='var(--red)'}}
function showPage(a,b){document.querySelectorAll('.page').forEach(function(a){a.classList.remove('active')});document.getElementById('page-'+a).classList.add('active');document.querySelectorAll('.nav-item').forEach(function(a){a.classList.remove('active')});if(b)b.classList.add('active');if(window.innerWidth<=700){document.querySelector('.sidebar').classList.remove('open');document.getElementById('hamburger').classList.remove('open');document.getElementById('sidebar-overlay').classList.remove('show')}}
function el(a){return document.getElementById(a)}
function fmtTime(a){if(a<60)return a+'s';if(a<3600)return Math.floor(a/60)+'m '+(a%60)+'s';return Math.floor(a/3600)+'h '+Math.floor((a%3600)/60)+'m'}
function applyIP(){var a=el('esp-ip').value.trim();if(a.indexOf('http')!==0)a='http://'+a;ESP_IP=a;el('settings-ip').value=el('esp-ip').value.trim();update()}
function applyIPFromSettings(){var a=el('settings-ip').value.trim();if(a.indexOf('http')!==0)a='http://'+a;ESP_IP=a;el('esp-ip').value=el('settings-ip').value.trim();update()}
function toggleDebug(a){a.classList.toggle('off');el('ip-config').style.display=a.classList.contains('off')?'none':'block'}
function toggleSidebar(){var a=document.querySelector('.sidebar'),b=document.getElementById('sidebar-overlay'),c=document.getElementById('hamburger');a.classList.toggle('open');c.classList.toggle('open');b.classList.toggle('show')}
function updateClock(){el('clock').innerText=new Date().toLocaleTimeString()+' | '+new Date().toLocaleDateString()}
window.addEventListener('resize',function(){drawChart(telChart);drawChart(anaChart)});
setInterval(update,500);setInterval(updateClock,1000);setInterval(syncEvents,3000);
updateClock();update();syncEvents();
</script>
</body>
</html>
)rawliteral";

static float pingOnce() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(3);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(12);   // slightly longer trigger = more energy for 2 m echoes
    digitalWrite(TRIG_PIN, LOW);
    // 35000 us timeout ≈ 6 m max; comfortably covers 2 m (2 m ≈ 11660 us round-trip)
    long d = pulseIn(ECHO_PIN, HIGH, 35000);
    if (d == 0) return -1;   // no echo
    float dist = (d * 0.0343) / 2.0;
    if (dist < 2.0 || dist > 400.0) return -1;
    return dist;
}
// Median-of-3 filter: reliable up to 2 m, rejects stray 0 / spike readings.
// Returns 400.0 when nothing in range (same sentinel as before).
float getDistance() {
    float a = pingOnce(); delayMicroseconds(600);
    float b = pingOnce(); delayMicroseconds(600);
    float c = pingOnce();
    float v[3] = {a, b, c};
    // sort 3 values
    for (int i = 0; i < 2; i++) for (int j = i + 1; j < 3; j++)
        if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
    float median = v[1];
    if (median < 0) {
        // 2/3 missed → treat as clear; 1/3 missed but median valid → use it
        if (v[2] > 0) return v[2] > 200.0 ? 400.0 : v[2]; // far single echo = clear
        return 400.0;
    }
    return median;
}

void logLineToSD(const String& m) {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    File f = SD_MMC.open(LOG_PATH, FILE_APPEND);
    if(f) { f.println(m); f.close(); }
    if (sdMutex) xSemaphoreGive(sdMutex);
}

void logToSD(Event e) {
    String line = "id=" + String(e.id) + " uptime=" + String(e.uptime) +
                  " dist=" + String(e.dist, 1) + " pir=" + String(e.pir ? 1 : 0) +
                  " sev=" + String(e.sev) + " type=" + String(e.type) +
                  " epoch=" + String((unsigned long)e.epoch);
    if (e.epoch > 0) {
        char dayBuf[16] = {0}; String dOut, tOut;
        String dt = fmtDateTime(e.epoch, dayBuf, sizeof(dayBuf), &dOut, &tOut);
        line += " dt=\"" + dt + "\" date=" + dOut + " time=" + tOut + " day=" + String(dayBuf);
    }
    logLineToSD(line);
}

void loadEventsFromSD() {
    if (!SD_MMC.exists(LOG_PATH)) return;
    File f = SD_MMC.open(LOG_PATH, "r");
    if (!f) return;
    while (f.available() && eventCount < MAX_EVENTS) {
        String line = f.readStringUntil('\n'); line.trim();
        if (line.length() == 0) continue;
        Event e; e.id = 0; e.uptime = 0; e.dist = 0; e.pir = false; e.sev = 'L'; e.type = 'I'; e.epoch = 0;
        int pos = 0;
        while (pos < line.length()) {
            int sp = line.indexOf(' ', pos);
            String tok = line.substring(pos, sp < 0 ? line.length() : sp);
            int eq = tok.indexOf('=');
            if (eq > 0) {
                String key = tok.substring(0, eq), val = tok.substring(eq + 1);
                val.replace("\"", "");
                if (key == "id") e.id = val.toInt();
                else if (key == "uptime") e.uptime = val.toInt();
                else if (key == "dist") e.dist = val.toFloat();
                else if (key == "pir") e.pir = (val.toInt() == 1);
                else if (key == "sev") e.sev = val.charAt(0);
                else if (key == "type") e.type = val.charAt(0);
                else if (key == "epoch") e.epoch = (time_t)strtoul(val.c_str(), nullptr, 10);
            }
            pos = sp < 0 ? line.length() : sp + 1;
        }
        if (e.id > 0) {
            if (e.id >= nextEventId) nextEventId = e.id + 1;
            events[eventCount++] = e;
        }
    }
    f.close();
}

String eventToJson(Event &e) {
    String j = "{\"id\":" + String(e.id) +
             ",\"u\":" + String(e.uptime) +
             ",\"d\":" + String(e.dist, 1) +
             ",\"p\":" + String(e.pir ? "true" : "false") +
             ",\"s\":\"" + String(e.sev) + "\"" +
             ",\"t\":\"" + String(e.type) + "\"" +
             ",\"epoch\":" + String((unsigned long)e.epoch);
    if (e.epoch > 0) {
        char dayBuf[16] = {0}; String dOut, tOut;
        String dt = fmtDateTime(e.epoch, dayBuf, sizeof(dayBuf), &dOut, &tOut);
        j += ",\"dt\":\"" + dt + "\",\"date\":\"" + dOut +
             "\",\"time\":\"" + tOut + "\",\"day\":\"" + String(dayBuf) + "\"";
    } else {
        j += ",\"dt\":\"\",\"date\":\"\",\"time\":\"\",\"day\":\"\"";
    }
    j += "}";
    return j;
}

String eventsJson() {
    String j = "[";
    for (int i = eventCount - 1; i >= 0; i--) {
        j += eventToJson(events[i]);
        if (i > 0) j += ",";
    }
    j += "]";
    return j;
}

// Daily report: counts grouped by calendar day + optional ?day=YYYY-MM-DD filter
String eventsReportJson(const String& dayFilter) {
    // collect unique days
    String days[200]; int dayCounts[200]; int nDays = 0;
    String out = "[";
    bool first = true;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = eventCount - 1; i >= 0; i--) {
            Event &e = events[i];
            String dOut = "";
            if (e.epoch > 0) { String tOut; char db[16]={0}; fmtDateTime(e.epoch, db, sizeof(db), &dOut, &tOut); }
            else dOut = "unsynced-uptime";
            if (dayFilter.length() && dOut != dayFilter) continue;
            if (pass == 0) {  // build day summary
                int f = -1;
                for (int k = 0; k < nDays; k++) if (days[k] == dOut) { f = k; break; }
                if (f < 0 && nDays < 200) { days[nDays] = dOut; dayCounts[nDays] = 1; nDays++; }
                else if (f >= 0) dayCounts[f]++;
            } else {  // emit events
                if (!first) out += ",";
                out += eventToJson(e);
                first = false;
            }
        }
        if (pass == 0) {
            // prepend summary as separate endpoint? we return events; summary via stats
        }
    }
    out += "]";
    return out;
}

String eventsDailySummaryJson() {
    // {"days":[{"date":"2026-10-14","day":"Wednesday","count":5}, ...]}
    struct DC { String date, day; int count; };
    DC arr[200]; int n = 0;
    for (int i = 0; i < eventCount; i++) {
        Event &e = events[i];
        String dOut = "unsynced"; String dayN = "-";
        if (e.epoch > 0) { String tOut; char db[16]={0}; fmtDateTime(e.epoch, db, sizeof(db), &dOut, &tOut); dayN = String(db); }
        int f = -1;
        for (int k = 0; k < n; k++) if (arr[k].date == dOut) { f = k; break; }
        if (f < 0 && n < 200) { arr[n].date = dOut; arr[n].day = dayN; arr[n].count = 1; n++; }
        else if (f >= 0) arr[f].count++;
    }
    String j = "{\"days\":[";
    for (int k = 0; k < n; k++) {
        j += "{\"date\":\"" + arr[k].date + "\",\"day\":\"" + arr[k].day + "\",\"count\":" + String(arr[k].count) + "}";
        if (k < n - 1) j += ",";
    }
    j += "]}";
    return j;
}

String eventsStatsJson() {
    long total = eventCount, high = 0, med = 0, low = 0, intrusions = 0, motions = 0;
    float peak = -1, min = 1e9, sum = 0;
    for (int i = 0; i < eventCount; i++) {
        Event &e = events[i];
        if (peak < 0 || e.dist > peak) peak = e.dist;
        if (e.dist < min) min = e.dist;
        sum += e.dist;
        if (e.sev == 'H') high++; else if (e.sev == 'M') med++; else low++;
        if (e.type != 'M') intrusions++; else motions++;
    }
    float avg = total ? sum / total : 0;
    String j = "{\"total\":" + String(total) +
               ",\"high\":" + String(high) +
               ",\"medium\":" + String(med) +
               ",\"low\":" + String(low) +
               ",\"intrusions\":" + String(intrusions) +
               ",\"motions\":" + String(motions) +
               ",\"peak\":" + String(peak < 0 ? 0 : peak, 1) +
               ",\"min\":" + String(total ? min : 0, 1) +
               ",\"avg\":" + String(avg, 1) +
               ",\"lastUptime\":" + String(total ? events[eventCount-1].uptime : 0) + "}";
    return j;
}

/* ══════════════════ FIREBASE REALTIME DB LOGGING ══════════════════ */
void pushStatusToFirebase() {
    if (strlen(FIREBASE_HOST) == 0 || WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    String url = String(FIREBASE_HOST);
    if (!url.endsWith("/")) url += "/";
    url += "status.json";
    if (strlen(FIREBASE_AUTH) > 0) url += "?auth=" + String(FIREBASE_AUTH);
    bool pirNow = USE_PIR && (digitalRead(PIR_PIN) == HIGH);
    time_t nowT = timeSynced ? time(nullptr) : 0;
    char dayBuf[16] = {0}; String dOut, tOut; String dt = "";
    if (nowT > 0) dt = fmtDateTime(nowT, dayBuf, sizeof(dayBuf), &dOut, &tOut);
    String payload = "{\"live_distance\":" + String(distanceValue, 1) +
                     ",\"pir_triggered\":" + String(pirNow ? "true" : "false") +
                     ",\"total_intrusions\":" + String(counter) +
                     ",\"threshold\":" + String(alertThreshold, 1) +
                     ",\"datetime\":\"" + dt + "\",\"date\":\"" + dOut +
                     "\",\"time\":\"" + tOut + "\",\"day\":\"" + String(dayBuf) + "\"" +
                     ",\"uptime\":" + String((millis() - start_time) / 1000) + "}";
    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.PUT(payload);
        // Serial.printf("Firebase status PUT %d\n", code);
        http.end();
    }
}

void logToFirebase(Event e) {
    if (strlen(FIREBASE_HOST) == 0 || WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);

    String url = String(FIREBASE_HOST);
    if (!url.endsWith("/")) url += "/";
    url += "events.json";
    if (strlen(FIREBASE_AUTH) > 0) {
        url += "?auth=" + String(FIREBASE_AUTH);
    }

    char dayBuf[16] = {0}; String dOut, tOut; String dt = "";
    if (e.epoch > 0) dt = fmtDateTime(e.epoch, dayBuf, sizeof(dayBuf), &dOut, &tOut);
    String payload = "{\"id\":" + String(e.id) +
                     ",\"uptime\":" + String(e.uptime) +
                     ",\"dist\":" + String(e.dist, 1) +
                     ",\"distance\":" + String(e.dist, 1) +
                     ",\"pir\":" + String(e.pir ? "true" : "false") +
                     ",\"sev\":\"" + String(e.sev) + "\"" +
                     ",\"type\":\"" + String(e.type) + "\"" +
                     ",\"datetime\":\"" + dt + "\",\"date\":\"" + dOut +
                     "\",\"time\":\"" + tOut + "\",\"day\":\"" + String(dayBuf) + "\"" +
                     ",\"epoch\":" + String((unsigned long)e.epoch) + "}";

    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(payload);
        Serial.printf("Firebase POST response: %d\n", code);
        http.end();
    }
}

/* ══════════════════ SMS (PhilSMS API v3) ══════════════════ */
int sendSMS(const String& message) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("SMS: no internet (STA not connected)");
        return -1;
    }
    WiFiClientSecure client;
    client.setInsecure(); // Handles TLS certificates dynamically
    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(10000);
    String body = "{\"recipient\":\"" + String(SMS_TO) +
                  "\",\"sender_id\":\"" + String(SMS_SENDER) +
                  "\",\"type\":\"plain\",\"message\":\"" + message + "\"}";
    int code = -1;
    String response = "";
    if (http.begin(client, String("https://") + SMS_HOST + SMS_PATH)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Accept", "application/json");
        http.addHeader("Authorization", "Bearer " + String(SMS_TOKEN));
        code = http.POST(body);
        if (code > 0) {
            response = http.getString();
            Serial.printf("SMS HTTP [%d]: %s\n", code, response.c_str());
        }
        http.end();
    } else {
        Serial.println("SMS: Connection failed to " + String(SMS_HOST));
    }

    // Check if PhilSMS returned an error payload (e.g. Unauthenticated, Insufficient balance)
    if (response.indexOf("\"status\":\"error\"") >= 0 || response.indexOf("Unauthenticated") >= 0) {
        Serial.println("SMS Dispatch Rejected by PhilSMS: " + response);
        return (code > 0) ? -code : -1;
    }
    return code;
}

// Runs on Core 0 so it never blocks the main sensing/server loop (Core 1)
void smsTask(void* param) {
    for (;;) {
        if (smsPending) {
            smsPending = false;
            smsResult = 1;
            String msg = smsQueue;
            smsQueue = "";
            int code = sendSMS(msg);
            smsCode = code;
            smsResult = (code >= 200 && code < 300) ? 2 : 3;
            logLineToSD("SMS " + String(smsResult == 2 ? "SENT" : "FAILED") +
                        " code:" + String(code) + " msg:" + msg);
        }

        if (cloudEventPending) {
            cloudEventPending = false;
            logToFirebase(pendingCloudEvent);
        }

        // Push live status to Firebase every ~1.5s (throttled)
        static unsigned long lastStatusPush = 0;
        if (millis() - lastStatusPush > 1500) {
            lastStatusPush = millis();
            pushStatusToFirebase();
        }

        delay(50);
    }
}

void setup() {
    Serial.begin(115200);
    start_time = millis();
    sdMutex = xSemaphoreCreateMutex();
    pinMode(PIR_PIN, INPUT);
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);

    // Initial Standby state: LED OFF, Buzzer OFF (silent)
    digitalWrite(LED_PIN, LOW);
    pinMode(BUZZER_PIN, OUTPUT);
    buzzerOff();

    // Boot indication (3 quick blinks)
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
    }

    SD_MMC.begin();

    // STA + AP concurrently: AP for local dashboard, STA for internet (SMS)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apGateway, apSubnet);
    WiFi.softAP(ap_ssid, ap_pass);
    WiFi.begin(wifi_ssid, wifi_pass);

    Serial.println("AP started: S3-GUARD-PRO");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(250); attempts++; }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Internet (STA) connected: ");
        Serial.println(WiFi.localIP());
        digitalWrite(LED_PIN, HIGH); delay(200); digitalWrite(LED_PIN, LOW);
        // NTP sync for day/time reports (non-blocking-ish, ~10s max)
        syncTimeNow();
        if (timeSynced) {
            time_t t = time(nullptr);
            char db[16]={0}; String dO, tO;
            Serial.println("Time synced: " + fmtDateTime(t, db, sizeof(db), &dO, &tO) + " " + String(db));
        } else {
            Serial.println("NTP sync failed - events will show uptime until WiFi/NTP recovers");
        }
    } else {
        Serial.println("Internet (STA) unavailable - SMS disabled until WiFi is reachable");
        for (int i = 0; i < 2; i++) { digitalWrite(LED_PIN, HIGH); delay(200); digitalWrite(LED_PIN, LOW); delay(200); }
    }

    loadEventsFromSD();
    Serial.print("Events loaded from SD: ");
    Serial.println(eventCount);

    server.on("/", [](){
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "text/html", INDEX_HTML);
    });
    server.on("/status", [](){
        bool p = USE_PIR && (digitalRead(PIR_PIN) == HIGH);
        time_t nowT = timeSynced ? time(nullptr) : 0;
        char dayBuf[16] = {0}; String dOut, tOut; String dt = "";
        if (nowT > 0) dt = fmtDateTime(nowT, dayBuf, sizeof(dayBuf), &dOut, &tOut);
        String j = "{\"motion\":" + String(motion?"true":"false") +
                   ",\"pir\":" + String(p?"true":"false") +
                   ",\"count\":" + String(counter) +
                   ",\"dist\":" + String(distanceValue, 1) +
                   ",\"threshold\":" + String(alertThreshold, 1) +
                   ",\"sound\":" + String(soundEnabled?"true":"false") +
                   ",\"uptime\":" + String((millis()-start_time)/1000) +
                   ",\"rssi\":" + String(WiFi.RSSI()) +
                   ",\"net\":\"" + String(WiFi.status()==WL_CONNECTED?"online":"local") + "\"" +
                   ",\"timeSynced\":" + String(timeSynced?"true":"false") +
                   ",\"datetime\":\"" + dt + "\",\"date\":\"" + dOut +
                   "\",\"time\":\"" + tOut + "\",\"day\":\"" + String(dayBuf) + "\"" +
                   ",\"epoch\":" + String((unsigned long)nowT) +
                   ",\"sms\":" + String(smsResult) +
                   ",\"smsCode\":" + String(smsCode) + "}";
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", j);
    });
    server.on("/threshold", HTTP_POST, [](){
        if (server.hasArg("val")) {
            float v = server.arg("val").toFloat();
            if (v >= THRESHOLD_MIN && v <= THRESHOLD_MAX) alertThreshold = v;
        }
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", "{\"threshold\":" + String(alertThreshold, 1) + "}");
    });
    server.on("/sound", HTTP_POST, [](){
        if (server.hasArg("enable")) {
            soundEnabled = (server.arg("enable") == "1" || server.arg("enable") == "true");
        }
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", "{\"sound\":" + String(soundEnabled?"true":"false") + "}");
    });
    server.on("/events", [](){
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", eventsJson());
    });
    server.on("/events/stats", [](){
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", eventsStatsJson());
    });
    // Daily reports: /events/report            → all events (with day/time fields)
    //                /events/report?day=YYYY-MM-DD → filtered to one calendar day
    //                /events/daily            → per-day counts [{date,day,count}]
    server.on("/events/report", [](){
        String day = server.hasArg("day") ? server.arg("day") : "";
        day.trim();
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", eventsReportJson(day));
    });
    server.on("/events/daily", [](){
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", eventsDailySummaryJson());
    });
    server.on("/time/resync", HTTP_POST, [](){
        syncTimeNow();
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json",
            "{\"synced\":" + String(timeSynced?"true":"false") + "}");
    });
    server.on("/events/clear", HTTP_POST, [](){
        clearEvents();
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "application/json", "{\"ok\":true,\"total\":0}");
    });
    server.on("/logs", [](){
        server.sendHeader("Access-Control-Allow-Origin", "*");
        if(SD_MMC.exists(LOG_PATH)){ File f = SD_MMC.open(LOG_PATH, "r"); server.streamFile(f, "text/plain"); f.close(); }
        else server.send(200, "text/plain", "");
    });
    server.begin();

    // Start SMS worker on Core 0 (keeps sensing responsive on Core 1)
    xTaskCreatePinnedToCore(smsTask, "smsTask", 16384, NULL, 1, NULL, 0);
}

void loop() {
    server.handleClient();
    if (millis() - start_time < 3000) return; // 3s sensor stabilization on boot

    unsigned long now = millis();

    // Read ultrasonic at fixed interval (non-blocking cadence -> faster HTTP response)
    if (now - lastDistanceTime >= DISTANCE_INTERVAL) {
        lastDistanceTime = now;
        distanceValue = getDistance();
    }

    bool p = USE_PIR && (digitalRead(PIR_PIN) == HIGH);

    // Debounced ultrasonic: must be in range for ULTRA_CONFIRM_READS consecutive reads
    bool rawUltra = (distanceValue >= 2.0 && distanceValue <= alertThreshold);
    if (rawUltra) {
        if (ultraConfirmCount < ULTRA_CONFIRM_READS) ultraConfirmCount++;
    } else {
        ultraConfirmCount = 0;
    }
    bool ultraTriggered = (ultraConfirmCount >= ULTRA_CONFIRM_READS);
    bool intrusion = p || ultraTriggered;

    // Serial diagnostics (1 Hz) - watch this to find which sensor is stuck
    static unsigned long lastDebug = 0;
    if (DEBUG_SENSORS && now - lastDebug >= 1000) {
        lastDebug = now;
        Serial.printf("[DBG] dist=%.1fcm | rawUltra=%d confirm=%d/%d | PIR=%d (used=%d) | intrusion=%d\n",
                      distanceValue, rawUltra, ultraConfirmCount, ULTRA_CONFIRM_READS, p, USE_PIR, intrusion);
    }

    // Hourly NTP re-sync keeps day/time reports accurate (only when online)
    static unsigned long lastNtpRetry = 0;
    if (WiFi.status() == WL_CONNECTED && !timeSynced && now - lastNtpRetry > 60000) {
        lastNtpRetry = now;
        syncTimeNow();
    }

    if (intrusion) {
        // Active alarm: Red LED ON + LOUD siren (max volume even on battery pack)
        digitalWrite(LED_PIN, HIGH);
        if (soundEnabled) {
            buzzerLoudTick();   // 2.5/3.2 kHz warble @ 50% duty — loudest in software
        } else {
            buzzerOff();
        }

        if (!motion) {
            motion = true;
            counter++;

            char eventType;
            if (p && ultraTriggered) eventType = 'B';
            else if (ultraTriggered) eventType = 'U';
            else eventType = 'P';

            addEvent(distanceValue, p, eventType);
            Event &last = events[eventCount - 1];
            String when = "";
            if (last.epoch > 0) {
                char db[16] = {0}; String dO, tO;
                when = " on " + fmtDateTime(last.epoch, db, sizeof(db), &dO, &tO) + " (" + String(db) + ")";
            }

            // Queue SMS alert (sent asynchronously on Core 0) — includes day/time
            if (p && ultraTriggered) {
                smsQueue = "S3-GUARD ALERT! Intrusion by PIR & Ultrasonic at " + String((int)distanceValue) + " cm" + when + ". Check area now.";
            } else if (ultraTriggered) {
                smsQueue = "S3-GUARD ALERT! Intrusion by Ultrasonic at " + String((int)distanceValue) + " cm" + when + ". Check area now.";
            } else {
                smsQueue = "S3-GUARD ALERT! PIR motion detected" + when + ". Check area now.";
            }
            smsPending = true;
        }
    } else {
        // Standby: Turn OFF Red LED and Buzzer
        digitalWrite(LED_PIN, LOW);
        buzzerOff();

        if (motion) {
            motion = false;
        }
    }
    lastPir = p;
    delay(2);   // fast loop for responsive dashboard & sensing
}