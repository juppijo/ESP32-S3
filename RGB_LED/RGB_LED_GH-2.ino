#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>

// WLAN Daten
const char* ssid = "Proximus-Home-B558";
const char* password = "wemrecmncrhud";

#define LED_PIN 48
Adafruit_NeoPixel pixels(1, LED_PIN, NEO_GRB + NEO_KHZ800);
AsyncWebServer server(80);

// --- Das HTML Interface (Minimalistisch & Dunkel) ---

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<title>ESP32-LDC</title>

<style>
  html, body {
    margin: 0;
    padding: 0;
    height: 100%;
    background: black;
    overflow: hidden;
    font-family: Arial, sans-serif;
  }

  /* Mini Status oben (fast unsichtbar) */
  #topbar {
    position: fixed;
    top: 0;
    left: 0;
    width: 100%;
    height: 20px;
    background: rgba(0,0,0,0.3);
    color: #38bdf8;
    font-size: 10px;
    text-align: center;
    z-index: 10;
  }

  /* Hauptbereich */
  #app {
    margin: 20;
    padding: 20;
    position: fixed;
    top: 10;
    left: 0;
    width: 100%;
    height: 100%;
  }
</style>

</head>

<body>

<div id="topbar">ESP32-S3 verbunden</div>
<div id="app"></div>

<script>
const url = "https://juppijo.github.io/esp32-d1/RGB_LED/index.html";

// AUTOMATISCH laden
fetch(url)
  .then(res => res.text())
  .then(html => {
    document.getElementById("app").innerHTML = html;
  })
  .catch(err => {
    document.getElementById("app").innerHTML =
      "<div style='color:red;text-align:center;margin-top:40px;'>⚠️ CORS Fehler</div>";
  });
</script>

</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  pixels.begin();
  pixels.setBrightness(30);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  Serial.println("\nVerbunden!");
  Serial.println(WiFi.localIP());

  // Route für die Webseite
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  // Route für die Hardware-Steuerung
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("color")) {
      String color = request->getParam("color")->value();
      if (color == "red")        pixels.setPixelColor(0, pixels.Color(255, 0, 0));
      else if (color == "green")  pixels.setPixelColor(0, pixels.Color(0, 255, 0));
      else if (color == "yellow")  pixels.setPixelColor(0, pixels.Color(255, 255, 0));
      else if (color == "white")  pixels.setPixelColor(0, pixels.Color(255, 255, 255));
      else if (color == "magenta")  pixels.setPixelColor(0, pixels.Color(255, 255, 255));
      else if (color == "aqua")  pixels.setPixelColor(0, pixels.Color(0, 255, 255)); 
      else if (color == "blue")   pixels.setPixelColor(0, pixels.Color(0, 0, 255));
      else if (color == "off")    pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      pixels.show();
      request->send(200, "text/plain", "OK");
    }
  });

  server.begin();
}

void loop() {}
