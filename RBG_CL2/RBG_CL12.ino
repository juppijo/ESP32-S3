/*
 * ============================================================
 * ESP32-S3 GPIO & RGB Master (Kombinierte Version)
 * ============================================================
 * Hardware: ESP32-S3-WROOM-1 (N16R8)
 * Features: GPIO Control, PWM, Digital Read, NeoPixel RGB (Pin 48)
 * Inklusive: mDNS, CORS, GitHub-GUI Loader
 * ============================================================
*  Libraries : ESPAsyncWebServer, AsyncTCP, ESPmDNS
 *
 *  Endpunkte:
 *    GET /            → Shell-Seite (lädt GUI von GitHub)
 *    GET /set?pin=&state=   → Digital-Output (0 oder 1)
 *    GET /pwm?pin=&duty=    → PWM-Output  (0–255)
 *    GET /read?pin=         → Digital-Input lesen
 *    GET /status            → JSON aller Pin-Zustände
 *    GET /info              → JSON mit System-Infos
 *    GET /rgb?color=red | /rgb?r=&g=&b=   → LED-Farbe setzen
 *    GET /state                           → kompletter Zustand (JSON)
 *    GET /fx?name=&speed=&candle=         → Effekt starten/stoppen (none = stoppen)
 *    GET /sleep?min=                      → Einschlaf-Timer (0 = aus)
 *    GET /alarm?on=&h=&m=&dur=            → Wecker mit Sonnenaufgang
 *    GET /alloff                          → alles aus
 *
 *  Kerze an GPIO12 (PWM 0–255):
 *    GET /kerze?mode=&level=&min=&max=&speed=&style=&shape=&to=&sec=
 *        mode = off | steady | flicker | pulse | fade   (alle Parameter optional)
 *    GET /kstate                          → Zustand der Kerze (JSON)
 *    GET /ksleep?min=                     → Einschlaf-Timer (sanftes Ausblenden)
 *    GET /kalarm?on=&h=&m=&dur=           → Wecker (Kerze wird eingeblendet)
 *    GET /koff                            → Kerze, Timer und Wecker aus
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "SecretLogin.h" // Muss ssid, password, GITHUB_URL und HOSTNAME enthalten
#include <Preferences.h>
#include <time.h>

// ── NeoPixel Konfiguration (Onboard LED) ────────────────────
#define RGB_PIN 48 
Adafruit_NeoPixel pixels(1, RGB_PIN, NEO_GRB + NEO_KHZ800); 

// ── Webserver & GPIO ────────────────────────────────────────
AsyncWebServer server(80);

struct PinCfg {
  int  pin;
  bool isOutput;
  const char* label;
};

// Kombinierte Pin-Liste 
PinCfg PINS[] = {
  {  2, true,  "LED / Relais 1"  },
  {  4, true,  "Relais 2"        },
  {  5, true,  "Relais 3"        },
  { 12, true,  "Relais 4"        },
  { 13, true,  "PWM Kanal 1"     },
  { 14, true,  "PWM Kanal 2"     },
  { 36, false, "Sensor 1"        },
  { 37, false, "Sensor 2"        },
  { 38, false, "Sensor 3"        }
};
const int PIN_COUNT = sizeof(PINS) / sizeof(PINS[0]);

// ── Hilfsfunktionen ──────────────────────────────────────────

PinCfg* findPin(int pin) {
  for (int i = 0; i < PIN_COUNT; i++)
    if (PINS[i].pin == pin) return &PINS[i];
  return nullptr;
}
// CORS-Header zu jeder Antwort hinzufügen

void addCORS(AsyncWebServerResponse* r) {
  r->addHeader("Access-Control-Allow-Origin",  "*");
  r->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  r->addHeader("Access-Control-Allow-Headers", "Content-Type"); 
}

void sendOK(AsyncWebServerRequest* req, const String& body, const char* ct = "application/json") {
  AsyncWebServerResponse* r = req->beginResponse(200, ct, body);
  addCORS(r);
  req->send(r); 
}

void sendErr(AsyncWebServerRequest* req, int code, const String& msg) {
  AsyncWebServerResponse* r = req->beginResponse(code, "text/plain", msg);
  addCORS(r);
  req->send(r);
}

// ── LED-Zustand (bleibt im ESP, auch wenn die Webseite geschlossen ist) ──
Preferences prefs;

enum Fx : uint8_t { FX_NONE, FX_RAINBOW, FX_PARTY, FX_POLICE, FX_CANDLE, FX_SUNSET, FX_SUNRISE };
const char* FX_NAMES[] = {"none", "rainbow", "party", "police", "candle", "sunset", "sunrise"};

struct State {
  uint8_t r = 0, g = 0, b = 0;
  Fx      fx = FX_NONE;
  uint8_t speed = 5;      // 1–10
  uint8_t candle = 1;     // 0 = ruhig, 1 = normal, 2 = windig
} st;

struct AlarmCfg { bool on = false; uint8_t h = 7, m = 0; uint16_t durMin = 10; } alarmCfg;

unsigned long sleepEndMs  = 0;      // 0 = Timer aus
unsigned long sunStartMs  = 0, sunDurMs = 0;
unsigned long lastStep    = 0, fxStart = 0;
int           fxStep      = 0;
bool          dirty       = false;
unsigned long dirtyAt     = 0;

// Nur Hardware ansteuern + Farbe merken (für Effekte)
void showPixel(uint8_t r, uint8_t g, uint8_t b) {
  st.r = r; st.g = g; st.b = b;
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

// Speichern erst nach 2 s Ruhe → schont den Flash (Farb-Picker sendet viele Werte)
void markDirty() { dirty = true; dirtyAt = millis(); }

void setColor(uint8_t r, uint8_t g, uint8_t b) {   // vom Nutzer gesetzt → Effekt endet
  st.fx = FX_NONE;
  showPixel(r, g, b);
  markDirty();
}

void setFx(uint8_t f) {          // uint8_t statt Fx: Arduino-Prototypen stehen vor der enum
  st.fx = (Fx)f; fxStart = millis(); lastStep = 0; fxStep = 0;
  markDirty();
}

void saveState() {
  prefs.begin("led", false);
  prefs.putUChar("r", st.r);   prefs.putUChar("g", st.g);   prefs.putUChar("b", st.b);
  prefs.putUChar("fx", st.fx); prefs.putUChar("spd", st.speed); prefs.putUChar("cl", st.candle);
  prefs.end();
}

void loadState() {
  prefs.begin("led", false);
  st.r = prefs.getUChar("r", 0);  st.g = prefs.getUChar("g", 0);  st.b = prefs.getUChar("b", 0);
  st.fx = (Fx)prefs.getUChar("fx", FX_NONE);
  st.speed  = prefs.getUChar("spd", 5);
  st.candle = prefs.getUChar("cl", 1);
  prefs.end();
  if (st.fx > FX_SUNSET) st.fx = FX_NONE;   // Sonnenaufgang nicht über Neustart hinaus
  fxStart = millis();
}

bool colorByName(const String& n, uint8_t& r, uint8_t& g, uint8_t& b) {
  struct C { const char* n; uint8_t r, g, b; };
  static const C T[] = {
    {"red",255,0,0}, {"green",0,255,0}, {"blue",0,0,255}, {"yellow",255,255,0},
    {"white",255,255,255}, {"magenta",255,0,255}, {"aqua",0,255,255},
    {"orange",255,110,0}, {"off",0,0,0}
  };
  for (auto& c : T) if (n == c.n) { r = c.r; g = c.g; b = c.b; return true; }
  return false;
}

String stateJson() {
  long sl = sleepEndMs ? (long)(sleepEndMs - millis()) / 1000 : 0;
  if (sl < 0) sl = 0;
  return String("{\"r\":") + st.r + ",\"g\":" + st.g + ",\"b\":" + st.b +
         ",\"fx\":\"" + FX_NAMES[st.fx] + "\",\"speed\":" + st.speed +
         ",\"candle\":" + st.candle + ",\"sleepLeft\":" + sl +
         ",\"alarm\":{\"on\":" + (alarmCfg.on ? "true" : "false") +
         ",\"h\":" + alarmCfg.h + ",\"m\":" + alarmCfg.m + ",\"dur\":" + alarmCfg.durMin + "}}";
}

// ── Kerze an GPIO12 (PWM 0–255) ──────────────────────────────
#define KERZE_PIN    12
#define KERZE_FREQ   5000     // PWM-Frequenz in Hz
#define KERZE_GAMMA  2.0f     // 1.0 = linear; 2.0 = Helligkeit wirkt gleichmäßiger

enum KMode : uint8_t { K_OFF, K_STEADY, K_FLICKER, K_PULSE, K_FADE };
const char* K_NAMES[] = {"off", "steady", "flicker", "pulse", "fade"};

struct KState {
  KMode   mode  = K_FLICKER;
  uint8_t level = 200;   // konstant (0–255)
  uint8_t mn    = 40;    // dunkel   (0–255)
  uint8_t mx    = 255;   // hell     (0–255)
  uint8_t speed = 5;     // 1–10
  uint8_t style = 1;     // Flackern: 0 ruhig, 1 normal, 2 windig
  uint8_t shape = 0;     // Hell-Dunkel: 0 weich, 1 hart
} kst;

struct KAlarm { bool on = false; uint8_t h = 7, m = 0; uint16_t dur = 10; } kAlarm;

float         kOut = 0;                 // aktuelle Helligkeit 0–255 (vor Gamma)
int           kLastDuty = -1;
unsigned long kLast = 0, kStart = 0;
float         kFadeFrom = 0, kFadeTarget = 0;
unsigned long kFadeStart = 0, kFadeMs = 1;
unsigned long kSleepEnd = 0;            // 0 = Timer aus
bool          kDirty = false;
unsigned long kDirtyAt = 0;

void kWrite(float v) {
  v = constrain(v, 0.0f, 255.0f);
  kOut = v;
  int duty = (int)(powf(v / 255.0f, KERZE_GAMMA) * 255.0f + 0.5f);
  if (duty != kLastDuty) { kLastDuty = duty; ledcWrite(KERZE_PIN, duty); }
}

void kMark() { kDirty = true; kDirtyAt = millis(); }

void kSetMode(uint8_t m) { kst.mode = (KMode)m; kStart = millis(); kLast = 0; kMark(); }

void kFade(float to, unsigned long ms) {
  kFadeFrom = kOut; kFadeTarget = to; kFadeStart = millis();
  kFadeMs = ms < 1 ? 1 : ms;
  kst.mode = K_FADE; kMark();
}

void kSave() {
  prefs.begin("kerze", false);
  prefs.putUChar("mode", kst.mode);  prefs.putUChar("level", kst.level);
  prefs.putUChar("mn", kst.mn);      prefs.putUChar("mx", kst.mx);
  prefs.putUChar("speed", kst.speed); prefs.putUChar("style", kst.style);
  prefs.putUChar("shape", kst.shape);
  prefs.end();
}

void kLoad() {
  prefs.begin("kerze", false);
  kst.mode  = (KMode)prefs.getUChar("mode", K_FLICKER);
  kst.level = prefs.getUChar("level", 200);
  kst.mn    = prefs.getUChar("mn", 40);
  kst.mx    = prefs.getUChar("mx", 255);
  kst.speed = prefs.getUChar("speed", 5);
  kst.style = prefs.getUChar("style", 1);
  kst.shape = prefs.getUChar("shape", 0);
  prefs.end();
  if (kst.mode > K_FADE) kst.mode = K_FLICKER;
  if (kst.mode == K_FADE) kst.mode = K_STEADY;     // Überblendung nicht über Neustart hinaus
  kOut = kst.level;
  kStart = millis();
}

int qInt(AsyncWebServerRequest* req, const char* key, int lo, int hi, int def) {
  if (!req->hasParam(key)) return def;
  return constrain(req->getParam(key)->value().toInt(), lo, hi);
}

String kJson() {
  long sl = kSleepEnd ? (long)(kSleepEnd - millis()) / 1000 : 0;
  if (sl < 0) sl = 0;
  return String("{\"mode\":\"") + K_NAMES[kst.mode] + "\",\"level\":" + kst.level +
         ",\"min\":" + kst.mn + ",\"max\":" + kst.mx + ",\"speed\":" + kst.speed +
         ",\"style\":" + kst.style + ",\"shape\":" + kst.shape +
         ",\"out\":" + (int)kOut + ",\"fadeTo\":" + (int)kFadeTarget + ",\"sleepLeft\":" + sl +
         ",\"alarm\":{\"on\":" + (kAlarm.on ? "true" : "false") +
         ",\"h\":" + kAlarm.h + ",\"m\":" + kAlarm.m + ",\"dur\":" + kAlarm.dur + "}}";
}

void kerzeLoop(unsigned long now) {
  float sp = kst.speed / 5.0f;

  // ── Einschlaf-Timer: am Ende 15 s sanft ausblenden ──
  if (kSleepEnd && (long)(now - kSleepEnd) >= 0) {
    kSleepEnd = 0;
    kFade(0, 15000);
  }

  // ── Wecker: Kerze wird bis zur Weckzeit auf "hell" eingeblendet ──
  static unsigned long lastAlarm = 0;
  if (kAlarm.on && now - lastAlarm >= 1000) {
    lastAlarm = now;
    struct tm t;
    if (getLocalTime(&t, 0)) {
      long nowS   = t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec;
      long startS = kAlarm.h * 3600L + kAlarm.m * 60L - kAlarm.dur * 60L;
      long diff   = (nowS - startS + 86400L) % 86400L;       // Sekunden seit Start des Einblendens
      if (diff < kAlarm.dur * 60L) {
        kFadeFrom   = 0;
        kFadeTarget = kst.mx ? kst.mx : 255;
        kFadeMs     = kAlarm.dur * 60000UL;
        kFadeStart  = now - diff * 1000UL;
        kst.mode    = K_FADE;
        kAlarm.on   = false;                                 // einmalig
        kMark();
      }
    }
  }

  switch (kst.mode) {
    case K_OFF:
      kWrite(0);
      break;

    case K_STEADY:
      kWrite(kst.level);
      break;

    case K_FLICKER: {
      static const float    BMIN[3] = {0.55, 0.30, 0.08};
      static const float    SMTH[3] = {0.22, 0.34, 0.52};
      static const uint16_t STEP[3] = {110, 85, 65};
      static const uint8_t  GUST[3] = {2, 6, 14};            // % Chance auf Zugluft
      static float b = 0.7, tb = 0.7;
      uint8_t l = kst.style > 2 ? 1 : kst.style;
      if (now - kLast >= STEP[l] / sp) {
        kLast = now;
        if (random(100) < GUST[l]) {                         // Böe: tief einknicken
          tb = BMIN[l] + random(0, 20) / 100.0f;
        } else if (random(100) < 35) {                       // normales Züngeln
          float lo = BMIN[l] + 0.12f;
          tb = lo + (random(0, 100) / 100.0f) * (1.0f - lo);
        }
        b += (tb - b) * SMTH[l];
        kWrite(kst.mn + constrain(b, 0.0f, 1.0f) * (kst.mx - kst.mn));
      }
      break;
    }

    case K_PULSE: {                                           // Hell-Dunkel abwechselnd
      unsigned long period = (unsigned long)((kst.shape ? 3000.0f : 6000.0f) / sp);
      if (period < 200) period = 200;
      float ph  = (float)((now - kStart) % period) / (float)period;
      float lum = kst.shape ? (ph < 0.5f ? 1.0f : 0.0f)       // hart: an/aus
                            : (0.5f - 0.5f * cosf(6.2831853f * ph));  // weich: Sinus
      kWrite(kst.mn + lum * (kst.mx - kst.mn));
      break;
    }

    case K_FADE: {
      float p = (float)(now - kFadeStart) / (float)kFadeMs;
      if (p >= 1.0f) {
        kWrite(kFadeTarget);
        kst.level = (uint8_t)kFadeTarget;
        kst.mode  = kFadeTarget > 0 ? K_STEADY : K_OFF;
        kMark();
      } else {
        kWrite(kFadeFrom + (kFadeTarget - kFadeFrom) * (p < 0 ? 0 : p));
      }
      break;
    }
  }

  // Zustand verzögert in den Flash schreiben (schont den Speicher)
  if (kDirty && now - kDirtyAt > 2000) { kDirty = false; kSave(); }
}

// ── Shell-HTML (Lädt das Interface von GitHub) ──────────────
// Wichtig: innerHTML führt <script>-Tags NICHT aus.
// Lösung: DOMParser + manuelles Script-Einfügen nach dem Laden.

const char SHELL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32-S3 Control Center</title>
  <style>
    body{background:#0f172a;color:#f8fafc;font-family:sans-serif;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;margin:0}
    #loader{text-align:center;color:#38bdf8}
    .spin{width:48px;height:48px;border:4px solid #1e293b;border-top:4px solid #38bdf8;border-radius:50%;animation:s 1s linear infinite;margin:0 auto 16px}
    @keyframes s{to{transform:rotate(360deg)}}
    .fs-btn{position:fixed;top:8px;right:12px;padding:8px 12px;border:none;border-radius:8px;background:rgba(148,163,184,.25);color:#f8fafc;font-size:1.3rem;cursor:pointer;z-index:9999}
  </style>
</head>
<body>
  <button class="fs-btn" onclick="toggleFS()">⛶</button>
  <div id="root"><div id="loader"><div class="spin"></div>Lade Interface...</div></div>
  <script>
    function toggleFS(){ if(!document.fullscreenElement) document.documentElement.requestFullscreen(); else document.exitFullscreen(); }
    fetch('%GITHUB_URL%')
      .then(r => r.text())
      .then(html => {
        const doc = new DOMParser().parseFromString(html, 'text/html');
        document.getElementById('root').innerHTML = doc.body.innerHTML;
        doc.querySelectorAll('script').forEach(old => {
          const s = document.createElement('script');
          if(old.src) s.src = old.src; else s.textContent = old.textContent;
          document.body.appendChild(s);
        });
      });
  </script>
</body>
</html>
)rawliteral"; 

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  
  // NeoPixel initialisieren 
  pixels.begin();
  pixels.setBrightness(30);
  loadState();                      // letzten Zustand aus dem Flash laden
  showPixel(st.r, st.g, st.b);      // Lampe leuchtet auch nach Stromausfall sofort wieder 

  delay(500);
  Serial.println("\n╔══════════════════════════╗");
  Serial.println(  "║  ESP32-S3 GPIO2 Master   ║");
  Serial.println(  "╚══════════════════════════╝");

  Serial.printf("Flash Size: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("PSRAM Size: %d MB\n", ESP.getPsramSize() / (1024 * 1024));

   // ── Pins initialisieren ──────────────────────────────────
  for (int i = 0; i < PIN_COUNT; i++) {
    if (PINS[i].isOutput) {
      pinMode(PINS[i].pin, OUTPUT);
      digitalWrite(PINS[i].pin, LOW);
      Serial.printf("  OUT  GPIO %-3d  %s\n", PINS[i].pin, PINS[i].label);
    } else {
      pinMode(PINS[i].pin, INPUT);
      Serial.printf("  IN   GPIO %-3d  %s\n", PINS[i].pin, PINS[i].label);
    }
  }

 // ── Kerze (GPIO12): PWM erst NACH pinMode() anbinden, sonst wird es wieder gelöst
  ledcAttach(KERZE_PIN, KERZE_FREQ, 8);   // PWM 0–255 an GPIO12
  kLoad();                                // letzte Kerzen-Einstellung laden

  // ── WLAN verbinden ───────────────────────────────────────
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("\nVerbinde mit '%s'", ssid);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n✗ WLAN-Verbindung fehlgeschlagen!");
    return;
  }
  Serial.println("\n✓ Verbunden!");
  Serial.println("  IP   : http://" + WiFi.localIP().toString());
  Serial.println("  RSSI : " + String(WiFi.RSSI()) + " dBm");

  // ── mDNS ─────────────────────────────────────────────────
  if (MDNS.begin(HOSTNAME)) 
    Serial.println("  mDNS : http://" + String(HOSTNAME) + ".local");

  // ── Uhrzeit per NTP (für den Wecker), Belgien/MEZ ────────
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");

  // ── CORS Preflight (OPTIONS) ─────────────────────────────
  server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(204);
    addCORS(r);
    req->send(r);
  });

  // --- Webserver Routen ------------------------------------------------------------

  // ── GET / → Shell-Seite ──────────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    String html = SHELL_HTML;
    html.replace("%GITHUB_URL%", GITHUB_URL);
    AsyncWebServerResponse* r =
      req->beginResponse(200, "text/html; charset=utf-8", html);
    addCORS(r);
    req->send(r);
  });

  // ── GET /set?pin=2&state=1 → Digital-Output ──────────────
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("pin") || !req->hasParam("state"))
      return sendErr(req, 400, "Fehlende Parameter: pin, state");

    int pin   = req->getParam("pin")->value().toInt();
    int state = req->getParam("state")->value().toInt();

    PinCfg* p = findPin(pin);
    if (!p)        return sendErr(req, 403, "Pin " + String(pin) + " nicht in Whitelist");
    if (!p->isOutput) return sendErr(req, 403, "Pin " + String(pin) + " ist als Input konfiguriert");

    digitalWrite(pin, state ? HIGH : LOW);
    Serial.printf("SET  GPIO %-3d → %d\n", pin, state);
    sendOK(req, "{\"pin\":" + String(pin) + ",\"state\":" + String(state) + "}", "application/json");
  });

  // ── GET /pwm?pin=13&duty=128 → PWM (0–255) ───────────────
  // Für ESP32-S3: ledcAttach + ledcWrite (Arduino Core ≥ 3.x)
  server.on("/pwm", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("pin") || !req->hasParam("duty"))
      return sendErr(req, 400, "Fehlende Parameter: pin, duty");

    int pin  = req->getParam("pin")->value().toInt();
    int duty = constrain(req->getParam("duty")->value().toInt(), 0, 255);

    PinCfg* p = findPin(pin);
    if (!p || !p->isOutput)
      return sendErr(req, 403, "Pin " + String(pin) + " nicht für PWM geeignet");

    // Arduino Core 3.x: ledcAttach(pin, freq, resolution)
    ledcAttach(pin, 5000, 8);   // 5 kHz, 8-Bit (0–255)
    ledcWrite(pin, duty);

    Serial.printf("PWM  GPIO %-3d → %d/255\n", pin, duty);
    sendOK(req, "{\"pin\":" + String(pin) + ",\"duty\":" + String(duty) + "}", "application/json");
  });

  // ── GET /read?pin=36 → Digital-Input ─────────────────────
  server.on("/read", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("pin"))
      return sendErr(req, 400, "Fehlender Parameter: pin");

    int pin = req->getParam("pin")->value().toInt();
    PinCfg* p = findPin(pin);
    if (!p)
      return sendErr(req, 403, "Pin " + String(pin) + " nicht in Whitelist");

    int val = digitalRead(pin);
    Serial.printf("READ GPIO %-3d → %d\n", pin, val);
    sendOK(req, "{\"pin\":" + String(pin) + ",\"value\":" + String(val) + "}", "application/json");
  });

  // ── GET /status → JSON aller Pin-Zustände ────────────────
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"pins\":[";
    for (int i = 0; i < PIN_COUNT; i++) {
      if (i) json += ",";
      json += "{\"pin\":"    + String(PINS[i].pin)               + ","
              "\"label\":\""  + String(PINS[i].label)             + "\","
              "\"output\":"   + (PINS[i].isOutput ? "true" : "false") + ","
              "\"value\":"    + String(digitalRead(PINS[i].pin))  + "}";
    }
    json += "]}";
    sendOK(req, json, "application/json");
  });

  // ── GET /info → System-Informationen ─────────────────────
  server.on("/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{"
      "\"ip\":\""     + WiFi.localIP().toString()  + "\","
      "\"hostname\":\"" + String(HOSTNAME) + ".local\","
      "\"rssi\":"     + String(WiFi.RSSI())         + ","
      "\"uptime\":"   + String(millis() / 1000)      + ","
      "\"heap\":"     + String(ESP.getFreeHeap())    + ","
      "\"chip\":\"ESP32-S3\""
    "}";
    sendOK(req, json, "application/json");
  });

  //------------------------------------------------------------------------------------

  // RGB-LED: /rgb?color=red  oder  /rgb?r=255&g=100&b=0
  server.on("/rgb", HTTP_GET, [](AsyncWebServerRequest* req) {
    uint8_t r = 0, g = 0, b = 0;
    if (req->hasParam("r") && req->hasParam("g") && req->hasParam("b")) {
      r = constrain(req->getParam("r")->value().toInt(), 0, 255);
      g = constrain(req->getParam("g")->value().toInt(), 0, 255);
      b = constrain(req->getParam("b")->value().toInt(), 0, 255);
    } else if (!(req->hasParam("color") && colorByName(req->getParam("color")->value(), r, g, b))) {
      return sendErr(req, 400, "color oder r,g,b fehlt bzw. unbekannt");
    }
    setColor(r, g, b);
    sendOK(req, stateJson());
  });

  // Kompletter Zustand (die Webseite fragt das beim Öffnen ab)
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest* req) { sendOK(req, stateJson()); });

  // Effekt: /fx?name=candle&speed=5&candle=1   (name=none stoppt)
  server.on("/fx", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("speed"))  st.speed  = constrain(req->getParam("speed")->value().toInt(), 1, 10);
    if (req->hasParam("candle")) st.candle = constrain(req->getParam("candle")->value().toInt(), 0, 2);
    if (req->hasParam("name")) {
      String n = req->getParam("name")->value();
      int f = -1;
      for (int i = 0; i <= FX_SUNRISE; i++) if (n == FX_NAMES[i]) f = i;
      if (f < 0) return sendErr(req, 400, "Unbekannter Effekt");
      setFx((Fx)f);
    }
    markDirty();
    sendOK(req, stateJson());
  });

  // Einschlaf-Timer: /sleep?min=30  (0 = aus)
  server.on("/sleep", HTTP_GET, [](AsyncWebServerRequest* req) {
    int min = req->hasParam("min") ? req->getParam("min")->value().toInt() : 0;
    sleepEndMs = (min > 0) ? millis() + (unsigned long)min * 60000UL : 0;
    sendOK(req, stateJson());
  });

  // Wecker: /alarm?on=1&h=7&m=0&dur=10
  server.on("/alarm", HTTP_GET, [](AsyncWebServerRequest* req) {
    alarmCfg.on = req->hasParam("on") && req->getParam("on")->value() == "1";
    if (req->hasParam("h"))   alarmCfg.h      = constrain(req->getParam("h")->value().toInt(), 0, 23);
    if (req->hasParam("m"))   alarmCfg.m      = constrain(req->getParam("m")->value().toInt(), 0, 59);
    if (req->hasParam("dur")) alarmCfg.durMin = constrain(req->getParam("dur")->value().toInt(), 1, 120);
    sendOK(req, stateJson());
  });

  // Alles aus: Effekt, Timer, Wecker, LED
  server.on("/alloff", HTTP_GET, [](AsyncWebServerRequest* req) {
    sleepEndMs = 0;
    alarmCfg.on = false;
    setColor(0, 0, 0);
    sendOK(req, stateJson());
  });

  // ── Kerze (GPIO12) ───────────────────────────────────────
  server.on("/kstate", HTTP_GET, [](AsyncWebServerRequest* req) { sendOK(req, kJson()); });

  // /kerze?mode=flicker&min=60&max=255&speed=5&style=1&shape=0&level=200
  // /kerze?mode=fade&to=0&sec=30     → sanft ein-/ausblenden
  server.on("/kerze", HTTP_GET, [](AsyncWebServerRequest* req) {
    kst.level = qInt(req, "level", 0, 255, kst.level);
    kst.mn    = qInt(req, "min",   0, 255, kst.mn);
    kst.mx    = qInt(req, "max",   0, 255, kst.mx);
    kst.speed = qInt(req, "speed", 1, 10,  kst.speed);
    kst.style = qInt(req, "style", 0, 2,   kst.style);
    kst.shape = qInt(req, "shape", 0, 1,   kst.shape);
    if (kst.mn > kst.mx) kst.mn = kst.mx;
    if (req->hasParam("mode")) {
      String n = req->getParam("mode")->value();
      int m = -1;
      for (int i = 0; i <= K_FADE; i++) if (n == K_NAMES[i]) m = i;
      if (m < 0) return sendErr(req, 400, "Unbekannte Betriebsart");
      if (m == K_FADE) kFade(qInt(req, "to", 0, 255, 0), qInt(req, "sec", 1, 3600, 10) * 1000UL);
      else             kSetMode(m);
    }
    kMark();
    sendOK(req, kJson());
  });

  server.on("/ksleep", HTTP_GET, [](AsyncWebServerRequest* req) {
    int min = qInt(req, "min", 0, 1440, 0);
    kSleepEnd = (min > 0) ? millis() + (unsigned long)min * 60000UL : 0;
    sendOK(req, kJson());
  });

  server.on("/kalarm", HTTP_GET, [](AsyncWebServerRequest* req) {
    kAlarm.on  = req->hasParam("on") && req->getParam("on")->value() == "1";
    kAlarm.h   = qInt(req, "h",   0, 23,  kAlarm.h);
    kAlarm.m   = qInt(req, "m",   0, 59,  kAlarm.m);
    kAlarm.dur = qInt(req, "dur", 1, 120, kAlarm.dur);
    sendOK(req, kJson());
  });

  server.on("/koff", HTTP_GET, [](AsyncWebServerRequest* req) {
    kSleepEnd = 0;
    kAlarm.on = false;
    kSetMode(K_OFF);
    sendOK(req, kJson());
  });

  server.begin();
  Serial.println("\n✓ Webserver läuft auf Port 80");
  Serial.println("══════════════════════════════***\n");
}

void loop() {
  unsigned long now = millis();
  float sp = st.speed / 5.0;

  // ── Einschlaf-Timer ──
  if (sleepEndMs && (long)(now - sleepEndMs) >= 0) {
    sleepEndMs = 0;
    setColor(0, 0, 0);
  }

  // ── Wecker (1x pro Sekunde prüfen) ──
  static unsigned long lastAlarm = 0;
  if (alarmCfg.on && now - lastAlarm >= 1000) {
    lastAlarm = now;
    struct tm t;
    if (getLocalTime(&t, 0)) {
      long nowS   = t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec;
      long startS = alarmCfg.h * 3600L + alarmCfg.m * 60L - alarmCfg.durMin * 60L;
      long diff   = (nowS - startS + 86400L) % 86400L;       // Sekunden seit Start des Sonnenaufgangs
      if (diff < alarmCfg.durMin * 60L) {                    // wir sind im Zeitfenster
        sunDurMs = alarmCfg.durMin * 60000UL;
        setFx(FX_SUNRISE);
        sunStartMs = now - diff * 1000UL;
        alarmCfg.on = false;                                 // einmalig
      }
    }
  }

  // ── Effekte ──
  static const uint8_t COL[8][3] = {
    {255,0,0}, {255,110,0}, {255,255,0}, {0,255,0}, {0,255,255}, {0,0,255}, {255,0,255}, {255,255,255}
  };

  switch (st.fx) {
    case FX_RAINBOW:
      if (now - lastStep >= 700 / sp) {
        lastStep = now;
        fxStep = (fxStep + 1) % 7;
        showPixel(COL[fxStep][0], COL[fxStep][1], COL[fxStep][2]);
      }
      break;

    case FX_PARTY:
      if (now - lastStep >= random(250, 550) / sp) {
        lastStep = now;
        int i;
        do { i = random(8); } while (i == fxStep);
        fxStep = i;
        showPixel(COL[i][0], COL[i][1], COL[i][2]);
      }
      break;

    case FX_POLICE: {                       // rot-aus-rot-aus-blau-aus-blau-aus
      static const uint8_t P[8][3] = {
        {255,0,0}, {0,0,0}, {255,0,0}, {0,0,0}, {0,0,255}, {0,0,0}, {0,0,255}, {0,0,0}
      };
      unsigned long dur = ((fxStep % 2) ? 90 : 200) / sp;
      if (now - lastStep >= dur) {
        lastStep = now;
        showPixel(P[fxStep][0], P[fxStep][1], P[fxStep][2]);
        fxStep = (fxStep + 1) % 8;
      }
      break;
    }

    case FX_CANDLE: {
      static const float    BMIN[3] = {0.58, 0.36, 0.14};
      static const float    SMTH[3] = {0.22, 0.34, 0.52};
      static const uint16_t STEP[3] = {110, 85, 65};
      static const uint8_t  GUST[3] = {2, 6, 14};          // % Chance auf Zugluft
      static float bright = 0.7, warm = 0.5, tb = 0.7, tw = 0.5;
      uint8_t l = st.candle;
      if (now - lastStep >= STEP[l] / sp) {
        lastStep = now;
        if (random(100) < GUST[l]) {                       // Böe: tief einknicken
          tb = BMIN[l] + random(0, 22) / 100.0;
          tw = random(5, 30) / 100.0;
        } else if (random(100) < 35) {                     // normales Züngeln
          float lo = BMIN[l] + 0.12;
          tb = lo + (random(0, 100) / 100.0) * (1.0 - lo);
          tw = constrain(0.48 + (tb - 0.65) * 0.85 + random(-22, 22) / 100.0, 0.0, 1.0);
        }
        bright += (tb - bright) * SMTH[l];
        warm   += (tw - warm)   * SMTH[l];
        float lum = pow(constrain(bright, 0.08f, 1.0f), 1.15);
        showPixel(255 * lum, (24 + pow(warm, 1.15) * 181) * lum, pow(warm, 2.4) * 26 * lum);
      }
      break;
    }

    case FX_SUNSET: {                       // gelb → orange → rot → aus, je 30 s / Tempo
      static const uint8_t SS[3][3] = {{255,255,0}, {255,110,0}, {255,0,0}};
      int stage = (now - fxStart) / (unsigned long)(30000 / sp);
      if (stage < 3) {
        showPixel(SS[stage][0], SS[stage][1], SS[stage][2]);
      } else {
        setColor(0, 0, 0);
      }
      break;
    }

    case FX_SUNRISE: {
      float p = (float)(now - sunStartMs) / (float)sunDurMs;
      if (p < 1.0) {
        if      (p < 0.34) showPixel(255, 0, 0);
        else if (p < 0.67) showPixel(255, 110, 0);
        else if (p < 0.92) showPixel(255, 255, 0);
        else               showPixel(255, 255, 255);
      } else if (now - sunStartMs > sunDurMs + 15UL * 60000UL) {   // 15 Min. leuchten, dann aus
        setColor(0, 0, 0);
      }
      break;
    }

    default: break;
  }

  kerzeLoop(now);                       // Kerze an GPIO12

  // ── Zustand verzögert in den Flash schreiben ──
  if (dirty && now - dirtyAt > 2000) {
    dirty = false;
    saveState();
  }

  delay(5);
}
