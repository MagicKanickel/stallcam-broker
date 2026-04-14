/*
 * StallCam – ESP32-S3 Überwachungskamera für Pferdestall
 *
 * Benötigte Bibliotheken (Arduino Library Manager):
 *   - "WebSockets" von Markus Sattler (arduinoWebSockets)
 *
 * Architektur:
 *   1. ESP32 pollt BROKER_URL alle 60s → erfährt bore-URL des C# Servers
 *   2. ESP32 verbindet WebSocket zu bore.pub:PORT (kein Port-Forwarding nötig)
 *   3. C# Server empfängt Frames, zeigt sie im WPF-Fenster + im Browser
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "FS.h"
#include "SD.h"
#include "esp_http_server.h"
#include <WebSocketsClient.h>
#include "mbedtls/base64.h"

// ─── WLAN ────────────────────────────────────────────────────────────
#define STALL_SSID  "iPhone von Benjamin"   // ← anpassen
#define STALL_PASS  "111-111-"              // ← anpassen
#define AP_SSID     "StallCam"
#define AP_PASS     "pferd1234"

// ─── Web-Interface Auth (lokaler Zugang) ─────────────────────────────
#define CAM_USER    "Ludwig"
#define CAM_PASS    "!blacky14/02$"           // ← anpassen

// ─── URL-Broker (Render.com) ─────────────────────────────────────────
// Nach Deployment die URL hier eintragen, z.B.:
//   "https://stallcam-broker.onrender.com/relay-url"

#define BROKER_URL  "https://stallcam-broker.onrender.com/relay-url"
#define BROKER_POLL_SEC  60                 // Wie oft Broker abfragen

// ─── Relay-Secret (muss mit C#-Server übereinstimmen) ────────────────
#define RELAY_SECRET   "Ji8e83HUhr24hgU$§Pou"
#define RELAY_PATH     "/esp32"

// ─── Kamera-Pins (DFR1154 / ESP32-S3 AI Camera V1.1) ─────────────────
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    5
#define Y9_GPIO_NUM      4
#define Y8_GPIO_NUM      6
#define Y7_GPIO_NUM      7
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     17
#define Y4_GPIO_NUM     21
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM     16
#define VSYNC_GPIO_NUM   1
#define HREF_GPIO_NUM    2
#define PCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    8
#define SIOC_GPIO_NUM    9

// ─── Sonstige Pins ───────────────────────────────────────────────────
#define SD_CS_PIN   10
#define IR_LED_PIN  47
#define STATUS_LED   3

// ─── Einstellungen ───────────────────────────────────────────────────
Preferences prefs;
int  intervalMin  = 10;
int  durationSec  = 5;
int  camRes       = 5;
bool nightMode    = false;
bool irLedAuto    = true;
int  videoCount   = 0;
bool sdOk         = false;
bool apMode       = false;

// ─── Relay-Zustand ───────────────────────────────────────────────────
WebSocketsClient   relayWs;
volatile bool      relayStreaming  = false;
volatile bool      relayConnected  = false;
String             relayHost       = "";
int                relayPort       = 0;

void startCameraServer();
void sendRelayStatus();
void sendRelayVideoList();

// ═══════════════════════════════════════════════════════════════════════
// EINSTELLUNGEN
// ═══════════════════════════════════════════════════════════════════════
void loadSettings() {
  prefs.begin("stallcam", true);
  intervalMin = prefs.getInt("interval", 10);
  durationSec = prefs.getInt("duration", 5);
  camRes      = prefs.getInt("res",      5);
  nightMode   = prefs.getBool("night",   false);
  irLedAuto   = prefs.getBool("irAuto",  true);
  videoCount  = prefs.getInt("vidCount", 0);
  relayHost   = prefs.getString("rHost", "");
  relayPort   = prefs.getInt("rPort",    0);
  prefs.end();
}

void saveSettings() {
  prefs.begin("stallcam", false);
  prefs.putInt("interval",  intervalMin);
  prefs.putInt("duration",  durationSec);
  prefs.putInt("res",       camRes);
  prefs.putBool("night",    nightMode);
  prefs.putBool("irAuto",   irLedAuto);
  prefs.putInt("vidCount",  videoCount);
  prefs.putString("rHost",  relayHost);
  prefs.putInt("rPort",     relayPort);
  prefs.end();
}

// ═══════════════════════════════════════════════════════════════════════
// KAMERA
// ═══════════════════════════════════════════════════════════════════════
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_UXGA;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10;
  config.fb_count     = 2;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Kamera-Fehler: 0x%x\n", err); return;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1); s->set_brightness(s, 1); s->set_saturation(s, -2);
  }
  Serial.println("Kamera OK");
}

void applyNightMode() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_gainceiling(s, nightMode ? GAINCEILING_128X : GAINCEILING_2X);
  s->set_brightness(s, nightMode ? 2 : 1);
  s->set_contrast(s, nightMode ? 2 : 0);
  s->set_saturation(s, -2);
  s->set_special_effect(s, nightMode ? 2 : 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, nightMode ? 1 : 0);
}

// ═══════════════════════════════════════════════════════════════════════
// SD-KARTE
// ═══════════════════════════════════════════════════════════════════════
bool initSD() {
  if (!SD.begin(SD_CS_PIN)) { Serial.println("SD nicht gefunden!"); return false; }
  if (SD.cardType() == CARD_NONE) { Serial.println("Keine SD!"); return false; }
  if (!SD.exists("/videos")) SD.mkdir("/videos");
  Serial.printf("SD OK – %.1f GB frei\n", (SD.totalBytes()-SD.usedBytes())/1e9);
  return true;
}

// ═══════════════════════════════════════════════════════════════════════
// AVI-MJPEG HILFSFUNKTIONEN
// ═══════════════════════════════════════════════════════════════════════
static void aviWriteU32(File &f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  f.write(b, 4);
}
static void aviWriteU16(File &f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v,(uint8_t)(v>>8)};
  f.write(b, 2);
}
static void aviWriteTag(File &f, const char* t) { f.write((const uint8_t*)t, 4); }
static void aviFixU32(File &f, uint32_t pos, uint32_t v) {
  uint32_t cur = f.position();
  f.seek(pos);
  aviWriteU32(f, v);
  f.seek(cur);
}

// ═══════════════════════════════════════════════════════════════════════
// VIDEO AUFNEHMEN
// ═══════════════════════════════════════════════════════════════════════
void recordVideo() {
  if (!sdOk) { Serial.println("Keine SD!"); return; }
  if (irLedAuto) digitalWrite(IR_LED_PIN, HIGH);
  digitalWrite(STATUS_LED, HIGH);

  char filename[48];
  snprintf(filename, sizeof(filename), "/videos/vid_%04d.avi", videoCount);
  Serial.printf("Aufnahme: %s\n", filename);

  File f = SD.open(filename, FILE_WRITE);
  if (!f) { Serial.println("Datei-Fehler!"); return; }

  // Auflösung aus camRes
  uint16_t w, h;
  switch(camRes) {
    case 4: w=640;  h=480;  break;
    case 6: w=1024; h=768;  break;
    case 7: w=1280; h=1024; break;
    case 8: w=1600; h=1200; break;
    default: w=800; h=600;  break;  // SVGA
  }
  const uint32_t FPS   = 12;
  const uint32_t usPF  = 1000000 / FPS;

  // Kamera aufwärmen
  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }

  // ── AVI-Header schreiben (mit Platzhaltern) ─────────────────────
  // RIFF AVI
  aviWriteTag(f, "RIFF");  aviWriteU32(f, 0);        // [4]  RIFF-Größe – Platzhalter
  aviWriteTag(f, "AVI ");

  // hdrl LIST (Größe fest = 192)
  aviWriteTag(f, "LIST");  aviWriteU32(f, 192);
  aviWriteTag(f, "hdrl");

  // avih – Haupt-AVI-Header
  aviWriteTag(f, "avih");  aviWriteU32(f, 56);
  aviWriteU32(f, usPF);              // µs pro Frame
  aviWriteU32(f, w*h*3*FPS);        // max Bytes/s
  aviWriteU32(f, 0);                 // padding
  aviWriteU32(f, 0x10);              // flags: AVIF_HASINDEX
  aviWriteU32(f, 0);                 // [48] totalFrames – Platzhalter
  aviWriteU32(f, 0);                 // initialFrames
  aviWriteU32(f, 1);                 // streams
  aviWriteU32(f, w*h*3);            // suggestedBufferSize
  aviWriteU32(f, w);
  aviWriteU32(f, h);
  aviWriteU32(f,0); aviWriteU32(f,0); aviWriteU32(f,0); aviWriteU32(f,0); // reserved

  // strl LIST (Größe fest = 116)
  aviWriteTag(f, "LIST");  aviWriteU32(f, 116);
  aviWriteTag(f, "strl");

  // strh – Stream-Header
  aviWriteTag(f, "strh");  aviWriteU32(f, 56);
  aviWriteTag(f, "vids");  aviWriteTag(f, "MJPG");
  aviWriteU32(f, 0);       // flags
  aviWriteU16(f, 0);       // priority
  aviWriteU16(f, 0);       // language
  aviWriteU32(f, 0);       // initialFrames
  aviWriteU32(f, 1);       // scale
  aviWriteU32(f, FPS);     // rate
  aviWriteU32(f, 0);       // start
  aviWriteU32(f, 0);       // [140] length – Platzhalter
  aviWriteU32(f, w*h*3);   // suggestedBufferSize
  aviWriteU32(f, 0xFFFFFFFF); // quality
  aviWriteU32(f, 0);       // sampleSize
  aviWriteU16(f, 0); aviWriteU16(f, 0); aviWriteU16(f, w); aviWriteU16(f, h);

  // strf – BITMAPINFOHEADER
  aviWriteTag(f, "strf");  aviWriteU32(f, 40);
  aviWriteU32(f, 40);      // biSize
  aviWriteU32(f, w);
  aviWriteU32(f, h);
  aviWriteU16(f, 1);       // planes
  aviWriteU16(f, 24);      // bitCount
  aviWriteTag(f, "MJPG");  // compression
  aviWriteU32(f, w*h*3);   // sizeImage
  aviWriteU32(f, 0); aviWriteU32(f, 0); aviWriteU32(f, 0); aviWriteU32(f, 0);
  // Header fertig bei Offset 212

  // movi LIST
  aviWriteTag(f, "LIST");  aviWriteU32(f, 0);  // [216] movi-Größe – Platzhalter
  aviWriteTag(f, "movi");
  // Frame-Daten ab Offset 224

  // ── Frames aufnehmen ────────────────────────────────────────────
  uint32_t maxFrames = (uint32_t)durationSec * FPS + 20;
  uint32_t *fOff = (uint32_t*)malloc(maxFrames * 4);
  uint32_t *fSz  = (uint32_t*)malloc(maxFrames * 4);
  if (!fOff || !fSz) {
    Serial.println("Kein RAM!"); free(fOff); free(fSz);
    f.close(); SD.remove(filename); return;
  }

  uint32_t frameCnt  = 0;
  unsigned long endMs = millis() + (unsigned long)durationSec * 1000;

  while (millis() < endMs && frameCnt < maxFrames) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { delay(10); continue; }

    fOff[frameCnt] = f.position() - 224;  // Offset ab movi-Daten
    fSz[frameCnt]  = fb->len;
    frameCnt++;

    aviWriteTag(f, "00dc");
    aviWriteU32(f, fb->len);
    f.write(fb->buf, fb->len);
    if (fb->len & 1) f.write((uint8_t)0);  // auf gerade Anzahl auffüllen

    esp_camera_fb_return(fb);
  }

  // ── idx1 schreiben ───────────────────────────────────────────────
  uint32_t moviEnd = f.position();
  aviWriteTag(f, "idx1");
  aviWriteU32(f, frameCnt * 16);
  for (uint32_t i = 0; i < frameCnt; i++) {
    aviWriteTag(f, "00dc");
    aviWriteU32(f, 0x10);       // AVIIF_KEYFRAME
    aviWriteU32(f, fOff[i]);
    aviWriteU32(f, fSz[i]);
  }
  uint32_t fileSize = f.position();

  // ── Platzhalter ausfüllen ────────────────────────────────────────
  aviFixU32(f,   4, fileSize - 8);          // RIFF-Größe
  aviFixU32(f,  48, frameCnt);              // avih totalFrames
  aviFixU32(f, 140, frameCnt);              // strh length
  aviFixU32(f, 216, moviEnd - 220);         // movi LIST-Größe

  f.close();
  free(fOff); free(fSz);

  videoCount++;
  saveSettings();

  // Ältestes löschen wenn SD fast voll
  if (sdOk && SD.usedBytes() > SD.totalBytes() * 0.92) {
    char old[48];
    snprintf(old, sizeof(old), "/videos/vid_%04d.avi", videoCount - 400);
    if (SD.exists(old)) SD.remove(old);
  }

  if (irLedAuto) digitalWrite(IR_LED_PIN, LOW);
  digitalWrite(STATUS_LED, LOW);
  Serial.printf("Fertig: %u Frames → %s\n", frameCnt, filename);
  if (relayConnected) sendRelayStatus();
}

// ═══════════════════════════════════════════════════════════════════════
// WIFI
// ═══════════════════════════════════════════════════════════════════════
void initWiFi() {
  WiFi.begin(STALL_SSID, STALL_PASS);
  Serial.print("Verbinde");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.println("\nIP: " + WiFi.localIP().toString());
  } else {
    WiFi.softAP(AP_SSID, AP_PASS);
    apMode = true;
    Serial.println("\nHotspot: " + WiFi.softAPIP().toString());
  }
}

// ═══════════════════════════════════════════════════════════════════════
// BROKER-POLLING – fragt ab wo der C#-Server gerade läuft
// ═══════════════════════════════════════════════════════════════════════
bool fetchRelayUrl(String &outHost, int &outPort) {
  if (strlen(BROKER_URL) < 8) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure sslClient;
  sslClient.setInsecure();        // Render.com Let's Encrypt – bei Bedarf Cert eintragen
  HTTPClient http;
  http.begin(sslClient, BROKER_URL);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  String body = http.getString();
  http.end();

  // Leerzeichen entfernen damit Parser mit "host":"..." und "host": "..." klar kommt
  body.replace(" ", "");

  // JSON parsen: {"host":"bore.pub","port":51234,"url":"bore.pub:51234"}
  int hostIdx = body.indexOf("\"host\":\"");
  int portIdx = body.indexOf("\"port\":");
  if (hostIdx < 0 || portIdx < 0) return false;

  hostIdx += 8;
  int hostEnd = body.indexOf('"', hostIdx);
  outHost = body.substring(hostIdx, hostEnd);

  portIdx += 7;
  int portEnd = body.indexOf(',', portIdx);
  if (portEnd < 0) portEnd = body.indexOf('}', portIdx);
  outPort = body.substring(portIdx, portEnd).toInt();
  if (outPort <= 0) outPort = 443;  // Cloudflare: kein Port → HTTPS/WSS

  return outHost.length() > 0;
}

// ═══════════════════════════════════════════════════════════════════════
// HTTP BASIC AUTH (lokaler Webserver)
// ═══════════════════════════════════════════════════════════════════════
static bool checkAuth(httpd_req_t *req) {
  char auth[256] = {0};
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"StallCam\"");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return false;
  }
  char creds[128];
  snprintf(creds, sizeof(creds), "%s:%s", CAM_USER, CAM_PASS);
  unsigned char encoded[128]; size_t olen;
  mbedtls_base64_encode(encoded, sizeof(encoded), &olen,
    (const unsigned char*)creds, strlen(creds));
  encoded[olen] = 0;
  char expected[160];
  snprintf(expected, sizeof(expected), "Basic %s", (char*)encoded);
  if (strcmp(auth, expected) == 0) return true;
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"StallCam\"");
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
  return false;
}

// ═══════════════════════════════════════════════════════════════════════
// RELAY – Nachrichten an den C#-Server
// ═══════════════════════════════════════════════════════════════════════
void sendRelayStatus() {
  if (!relayConnected) return;
  char json[512];
  snprintf(json, sizeof(json),
    "{\"type\":\"status\",\"videoCount\":%d,\"sdUsed\":%llu,\"sdTotal\":%llu,"
    "\"interval\":%d,\"duration\":%d,\"camRes\":%d,"
    "\"nightMode\":%s,\"irAuto\":%s,\"apMode\":%s}",
    videoCount,
    sdOk ? SD.usedBytes()  : (uint64_t)0,
    sdOk ? SD.totalBytes() : (uint64_t)0,
    intervalMin, durationSec, camRes,
    nightMode ? "true":"false",
    irLedAuto ? "true":"false",
    apMode    ? "true":"false"
  );
  relayWs.sendTXT(json);
}

void sendRelayVideoList() {
  if (!relayConnected) return;
  if (!sdOk) { relayWs.sendTXT("{\"type\":\"videos\",\"files\":[]}"); return; }
  String json = "{\"type\":\"videos\",\"files\":[";
  File root = SD.open("/videos");
  File f = root.openNextFile();
  bool first = true;
  while (f) {
    if (!first) json += ",";
    json += "{\"name\":\""; json += f.name();
    json += "\",\"size\":";  json += f.size(); json += "}";
    first = false; f = root.openNextFile();
  }
  json += "]}";
  relayWs.sendTXT(json.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// RELAY – Befehle vom C#-Server empfangen
// ═══════════════════════════════════════════════════════════════════════
static void handleRelayCommand(const String &msg) {
  auto getInt = [&](const char* key) -> int {
    String k = String("\"") + key + "\":";
    int idx = msg.indexOf(k);
    return (idx < 0) ? -999 : msg.substring(idx + k.length()).toInt();
  };
  auto getBool = [&](const char* key) -> int {
    String k = String("\"") + key + "\":";
    int idx = msg.indexOf(k);
    if (idx < 0) return -1;
    return msg.substring(idx + k.length()).startsWith("true") ? 1 : 0;
  };

  if      (msg.indexOf("stream_start") >= 0) { relayStreaming = true; }
  else if (msg.indexOf("stream_stop")  >= 0) { relayStreaming = false; }
  else if (msg.indexOf("get_status")   >= 0) { sendRelayStatus(); }
  else if (msg.indexOf("get_videos")   >= 0) { sendRelayVideoList(); }
  else if (msg.indexOf("record")       >= 0) {
    xTaskCreate([](void*){ recordVideo(); vTaskDelete(NULL); }, "rec_r", 8192, NULL, 5, NULL);
  }
  else if (msg.indexOf("\"download\"") >= 0) {
    String k = "\"file\":\"";
    int idx = msg.indexOf(k);
    if (idx >= 0 && sdOk) {
      idx += k.length();
      int end = msg.indexOf('"', idx);
      String fname = msg.substring(idx, end);
      String fpath = "/videos/" + fname;
      File f = SD.open(fpath);
      if (f) {
        String startMsg = "{\"type\":\"file_start\",\"file\":\"" + fname + "\",\"size\":" + f.size() + "}";
        relayWs.sendTXT(startMsg.c_str());
        uint8_t buf[4096];
        while (f.available()) {
          int len = f.read(buf, sizeof(buf));
          relayWs.sendBIN(buf, len);
          delay(5);
        }
        f.close();
        relayWs.sendTXT("{\"type\":\"file_end\"}");
      }
    }
  }
  else if (msg.indexOf("delete_all")   >= 0) {
    if (sdOk) {
      File root = SD.open("/videos");
      File f = root.openNextFile();
      while (f) {
        char path[80]; snprintf(path, sizeof(path), "/videos/%s", f.name());
        f.close(); SD.remove(path); f = root.openNextFile();
      }
      videoCount = 0; saveSettings();
    }
    sendRelayStatus();
  }
  else if (msg.indexOf("save_settings") >= 0) {
    int v;
    if ((v = getInt("interval")) != -999) intervalMin = v;
    if ((v = getInt("duration")) != -999) durationSec = v;
    if ((v = getInt("camRes"))   != -999) camRes      = v;
    int nm = getBool("nightMode"); if (nm >= 0) { nightMode = nm; applyNightMode(); }
    int ir = getBool("irAuto");    if (ir >= 0) irLedAuto = ir;
    saveSettings(); sendRelayStatus();
  }
  else if (msg.indexOf("night_on")  >= 0) { nightMode = true;  applyNightMode(); saveSettings(); sendRelayStatus(); }
  else if (msg.indexOf("night_off") >= 0) { nightMode = false; applyNightMode(); saveSettings(); sendRelayStatus(); }
  else if (msg.indexOf("led_on")    >= 0) { digitalWrite(IR_LED_PIN, HIGH); }
  else if (msg.indexOf("led_off")   >= 0) { digitalWrite(IR_LED_PIN, LOW);  }
}

// ═══════════════════════════════════════════════════════════════════════
// RELAY – WebSocket-Event-Handler
// ═══════════════════════════════════════════════════════════════════════
void wsRelayEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      relayConnected = true;
      Serial.println("Relay: Verbunden!");
      {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"type\":\"auth\",\"secret\":\"%s\"}", RELAY_SECRET);
        relayWs.sendTXT(buf);
      }
      break;
    case WStype_DISCONNECTED:
      relayConnected = false;
      relayStreaming  = false;
      Serial.println("Relay: Getrennt");
      break;
    case WStype_TEXT:
      handleRelayCommand(String((char*)payload, length));
      break;
    default: break;
  }
}

// ═══════════════════════════════════════════════════════════════════════
// RELAY – FreeRTOS-Task (Broker-Polling + WebSocket + Streaming)
// ═══════════════════════════════════════════════════════════════════════
void relayTask(void *param) {
  unsigned long lastPoll      = 0;
  unsigned long lastKeepalive = 0;

  while(true) {
    unsigned long now = millis();

    // ── Broker alle 60s abfragen ───────────────────────────────────
    if (now - lastPoll >= (unsigned long)BROKER_POLL_SEC * 1000UL || lastPoll == 0) {
      lastPoll = now;
      String newHost;
      int    newPort = 0;

      if (fetchRelayUrl(newHost, newPort)) {
        if (newHost != relayHost || newPort != relayPort) {
          Serial.printf("Relay URL: %s:%d\n", newHost.c_str(), newPort);
          relayHost = newHost;
          relayPort = newPort;
          relayWs.disconnect();
          delay(200);
          // Port 443 = Cloudflare/HTTPS → WSS (SSL), sonst plain WS
          if (newPort == 443)
            relayWs.beginSSL(newHost.c_str(), newPort, RELAY_PATH);
          else
            relayWs.begin(newHost.c_str(), newPort, RELAY_PATH);
          relayWs.onEvent(wsRelayEvent);
          relayWs.setReconnectInterval(5000);
          relayWs.enableHeartbeat(8000, 5000, 3);
        }
      } else {
        Serial.println("Broker: keine URL verfügbar");
      }
    }

    // ── WebSocket keep-alive ───────────────────────────────────────
    relayWs.loop();

    // ── Cloudflare Keepalive (alle 5s) ────────────────────────────
    if (relayConnected && !relayStreaming && now - lastKeepalive >= 5000) {
      lastKeepalive = now;
      relayWs.sendTXT("{\"type\":\"ping\"}");
    }

    // ── Frames senden wenn jemand zuschaut ────────────────────────
    if (relayStreaming && relayConnected) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        relayWs.sendBIN(fb->buf, fb->len);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(80));  // ~12 fps
      } else {
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

void initRelay() {
  if (strlen(BROKER_URL) < 8) {
    Serial.println("Relay: kein Broker konfiguriert, übersprungen");
    return;
  }
  relayWs.onEvent(wsRelayEvent);
  xTaskCreate(relayTask, "relay", 20480, NULL, 3, NULL);
  Serial.println("Relay-Task gestartet");
}

// ═══════════════════════════════════════════════════════════════════════
// LOKALER WEBSERVER (WLAN-Zugang)
// ═══════════════════════════════════════════════════════════════════════
static httpd_handle_t web_httpd = NULL;

static const char HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StallCam – Lokal</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;700&family=Space+Mono&display=swap');
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0d1117;--card:#161b22;--border:#30363d;--green:#3fb950;--amber:#d29922;--red:#f85149;--blue:#58a6ff;--text:#e6edf3;--muted:#8b949e}
body{background:var(--bg);color:var(--text);font-family:'Outfit',sans-serif;min-height:100vh;padding:20px}
.hdr{display:flex;align-items:center;gap:12px;margin-bottom:24px;flex-wrap:wrap}
.logo{font-size:1.6rem;font-weight:700;letter-spacing:-0.04em}.logo span{color:var(--green)}
.badge{font-family:'Space Mono',monospace;font-size:.7rem;padding:3px 8px;border-radius:4px;background:var(--card);border:1px solid var(--border);color:var(--muted)}
.badge.on{border-color:var(--green);color:var(--green)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;max-width:960px}
@media(max-width:640px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;overflow:hidden}
.ch{padding:14px 16px;border-bottom:1px solid var(--border);font-weight:600;font-size:.9rem;display:flex;align-items:center;gap:8px}
.cb{padding:16px}
.sw{width:100%;aspect-ratio:4/3;background:#000;position:relative}
.sw img{width:100%;height:100%;object-fit:cover}
.ld{position:absolute;top:10px;left:10px;background:var(--red);color:#fff;font-size:.65rem;padding:3px 8px;border-radius:999px;display:flex;align-items:center;gap:5px;font-family:'Space Mono',monospace}
.ld::before{content:'';width:5px;height:5px;border-radius:50%;background:#fff;animation:blink 1s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
.sr{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--border);font-size:.84rem}
.sr:last-child{border:none}
.sl{color:var(--muted)}.sv{font-family:'Space Mono',monospace;color:var(--blue)}
.row{margin-bottom:13px}
.row label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:4px}
.row input[type=range]{width:100%;accent-color:var(--green)}
.row select{width:100%;background:var(--bg);border:1px solid var(--border);color:var(--text);padding:8px;border-radius:6px;font-family:'Outfit',sans-serif;font-size:.85rem}
.tr{display:flex;justify-content:space-between;align-items:center;padding:7px 0}
.tg{position:relative;width:44px;height:24px}
.tg input{opacity:0;width:0;height:0}
.sl2{position:absolute;inset:0;background:var(--border);border-radius:12px;cursor:pointer;transition:.2s}
.sl2::before{content:'';position:absolute;width:18px;height:18px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
input:checked+.sl2{background:var(--green)}
input:checked+.sl2::before{transform:translateX(20px)}
.btn{font-family:'Outfit',sans-serif;font-size:.84rem;font-weight:600;padding:9px 16px;border-radius:8px;border:none;cursor:pointer;transition:all .15s;display:inline-flex;align-items:center;gap:6px}
.bp{background:var(--green);color:#000}.bp:hover{filter:brightness(1.1)}
.bd{background:var(--red);color:#fff}
.bs{background:transparent;border:1px solid var(--border);color:var(--text)}.bs:hover{border-color:var(--blue);color:var(--blue)}
.vlist{max-height:280px;overflow-y:auto}
.vi{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--border);font-size:.82rem}
.vi:last-child{border:none}
.vn{font-family:'Space Mono',monospace;color:var(--muted);font-size:.72rem}
.sdb{height:5px;background:var(--border);border-radius:3px;margin-top:8px;overflow:hidden}
.sdf{height:100%;background:var(--green);border-radius:3px;transition:width .5s}
.sdf.w{background:var(--amber)}.sdf.c{background:var(--red)}
.full{grid-column:1/-1}
.gap{display:flex;flex-wrap:wrap;gap:8px}
</style>
</head>
<body>
<div class="hdr">
  <div class="logo">Stall<span>Cam</span> 🐴</div>
  <div class="badge on" id="wmode">● Lokal-WLAN</div>
  <div class="badge" id="sdbadge">SD: –</div>
</div>
<div class="grid">
  <div class="card full">
    <div class="ch">📹 Livestream (lokal)</div>
    <div class="sw"><img id="stream" src="/stream"><div class="ld">LIVE</div></div>
  </div>
  <div class="card">
    <div class="ch">📊 Status</div>
    <div class="cb">
      <div class="sr"><span class="sl">Videos gespeichert</span><span class="sv" id="svc">–</span></div>
      <div class="sr"><span class="sl">SD belegt</span><span class="sv" id="ssu">–</span></div>
      <div class="sr"><span class="sl">SD gesamt</span><span class="sv" id="sst">–</span></div>
      <div class="sr"><span class="sl">Intervall</span><span class="sv" id="sint">–</span></div>
      <div class="sr"><span class="sl">Aufnahmedauer</span><span class="sv" id="sdur">–</span></div>
      <div class="sdb"><div class="sdf" id="sdfill" style="width:0%"></div></div>
    </div>
  </div>
  <div class="card">
    <div class="ch">⚙️ Einstellungen</div>
    <div class="cb">
      <div class="row"><label>Intervall: <b id="iv">10 min</b></label><input type="range" id="isl" min="1" max="120" value="10" oninput="lbl('iv',this.value,' min')"></div>
      <div class="row"><label>Aufnahmedauer: <b id="dv">5 sek</b></label><input type="range" id="dsl" min="2" max="120" value="5" oninput="lbl('dv',this.value,' sek')"></div>
      <div class="row"><label>Auflösung</label>
        <select id="rsel">
          <option value="4">VGA (640×480)</option>
          <option value="5" selected>SVGA (800×600)</option>
          <option value="6">XGA (1024×768)</option>
          <option value="7">SXGA (1280×1024)</option>
          <option value="8">UXGA (1600×1200)</option>
        </select>
      </div>
      <div class="tr"><span style="font-size:.85rem">🌙 Nachtsicht</span><label class="tg"><input type="checkbox" id="nt" onchange="ctrl('night',this.checked?'on':'off')"><span class="sl2"></span></label></div>
      <div class="tr"><span style="font-size:.85rem">💡 IR-LED auto</span><label class="tg"><input type="checkbox" id="ir" checked onchange="ctrl('led',this.checked?'on':'off')"><span class="sl2"></span></label></div>
      <br>
      <div class="gap">
        <button class="btn bp" onclick="save()">💾 Speichern</button>
        <button class="btn bs" onclick="recNow()">🎬 Jetzt aufnehmen</button>
      </div>
    </div>
  </div>
  <div class="card full">
    <div class="ch">🎞️ Videos <button class="btn bd" onclick="delAll()" style="margin-left:auto;padding:5px 10px;font-size:.75rem">🗑️ Alle löschen</button></div>
    <div class="cb"><div class="vlist" id="vlist"><div style="color:var(--muted);font-size:.85rem">Lade...</div></div></div>
  </div>
</div>
<script>
function lbl(id,v,u){document.getElementById(id).textContent=v+u}
function fgb(b){if(b>1e9)return(b/1e9).toFixed(1)+' GB';if(b>1e6)return(b/1e6).toFixed(0)+' MB';return b+' B'}
function fmb(b){if(b>1e6)return(b/1e6).toFixed(1)+' MB';if(b>1e3)return(b/1e3).toFixed(0)+' KB';return b+' B'}
async function ctrl(key,val){try{await fetch('/control?'+key+'='+val);}catch(e){}}
async function loadStatus(){
  try{
    const d=await(await fetch('/status')).json();
    document.getElementById('svc').textContent=d.videoCount;
    document.getElementById('ssu').textContent=fgb(d.sdUsed);
    document.getElementById('sst').textContent=fgb(d.sdTotal);
    document.getElementById('sint').textContent=d.interval+' min';
    document.getElementById('sdur').textContent=d.duration+' sek';
    document.getElementById('sdbadge').textContent='SD: '+fgb(d.sdUsed)+'/'+fgb(d.sdTotal);
    document.getElementById('isl').value=d.interval;lbl('iv',d.interval,' min');
    document.getElementById('dsl').value=d.duration;lbl('dv',d.duration,' sek');
    document.getElementById('rsel').value=d.camRes;
    document.getElementById('nt').checked=d.nightMode;
    document.getElementById('ir').checked=d.irAuto;
    document.getElementById('wmode').textContent=d.apMode?'📡 Hotspot':'● Lokal-WLAN';
    const p=d.sdTotal>0?d.sdUsed/d.sdTotal*100:0;
    const f=document.getElementById('sdfill');
    f.style.width=p+'%';f.className='sdf'+(p>90?' c':p>70?' w':'');
  }catch(e){}
}
async function loadVideos(){
  try{
    const d=await(await fetch('/videos')).json();
    const el=document.getElementById('vlist');
    if(!d.files||!d.files.length){el.innerHTML='<div style="color:var(--muted);font-size:.85rem">Keine Videos</div>';return;}
    el.innerHTML=d.files.map(f=>`<div class="vi"><span class="vn">${f.name}</span><span style="color:var(--muted);font-size:.75rem">${fmb(f.size)}</span><a href="/download?f=${f.name}" download="${f.name}"><button class="btn bs" style="padding:5px 10px;font-size:.75rem">⬇️</button></a></div>`).join('');
  }catch(e){}
}
async function save(){
  const img=document.getElementById('stream');img.src='';
  await new Promise(r=>setTimeout(r,500));
  const b={interval:+document.getElementById('isl').value,duration:+document.getElementById('dsl').value,camRes:+document.getElementById('rsel').value,nightMode:document.getElementById('nt').checked,irAuto:document.getElementById('ir').checked};
  try{await fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});loadStatus();alert('Gespeichert!');}catch(e){alert('Fehler: '+e.message);}
  await new Promise(r=>setTimeout(r,300));img.src='/stream';
}
async function recNow(){
  if(!confirm('Jetzt aufnehmen?'))return;
  const img=document.getElementById('stream');img.src='';
  await fetch('/record');
  setTimeout(()=>{loadVideos();loadStatus();img.src='/stream';},8000);
}
async function delAll(){
  if(!confirm('Alle Videos löschen?'))return;
  const img=document.getElementById('stream');img.src='';
  await fetch('/deleteall',{method:'POST'});
  loadVideos();loadStatus();setTimeout(()=>{img.src='/stream';},300);
}
loadStatus();loadVideos();setInterval(loadStatus,15000);
</script>
</body>
</html>)HTML";

// ── HTTP Handler ──────────────────────────────────────────────────────
static esp_err_t root_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, HTML, strlen(HTML));
  return ESP_OK;
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *SCT  = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *SBND = "\r\n--" PART_BOUNDARY "\r\n";
static const char *SPRT = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  camera_fb_t *fb = NULL; esp_err_t res = ESP_OK; char part_buf[64];
  res = httpd_resp_set_type(req, SCT); if (res != ESP_OK) return res;
  while (true) {
    fb = esp_camera_fb_get(); if (!fb) { res = ESP_FAIL; break; }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, SBND, strlen(SBND));
    if (res == ESP_OK) { size_t hlen = snprintf(part_buf, sizeof(part_buf), SPRT, fb->len); res = httpd_resp_send_chunk(req, part_buf, hlen); }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb); fb = NULL; if (res != ESP_OK) break;
  }
  return res;
}

static esp_err_t status_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  char json[512];
  snprintf(json, sizeof(json),
    "{\"videoCount\":%d,\"sdUsed\":%llu,\"sdTotal\":%llu,"
    "\"interval\":%d,\"duration\":%d,\"camRes\":%d,"
    "\"nightMode\":%s,\"irAuto\":%s,\"apMode\":%s}",
    videoCount, sdOk?SD.usedBytes():(uint64_t)0, sdOk?SD.totalBytes():(uint64_t)0,
    intervalMin, durationSec, camRes,
    nightMode?"true":"false", irLedAuto?"true":"false", apMode?"true":"false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

static esp_err_t videos_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  httpd_resp_set_type(req, "application/json");
  if (!sdOk) { httpd_resp_sendstr(req, "{\"files\":[]}"); return ESP_OK; }
  String json = "{\"files\":[";
  File root = SD.open("/videos"); File f = root.openNextFile(); bool first = true;
  while (f) {
    if (!first) json += ",";
    json += "{\"name\":\""; json += f.name(); json += "\",\"size\":"; json += f.size(); json += "}";
    first = false; f = root.openNextFile();
  }
  json += "]}"; httpd_resp_sendstr(req, json.c_str()); return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  char query[128]={0}, fname[64]={0};
  httpd_req_get_url_query_str(req, query, sizeof(query));
  httpd_query_key_value(query, "f", fname, sizeof(fname));
  char path[80]; snprintf(path, sizeof(path), "/videos/%s", fname);
  File f = SD.open(path);
  if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "application/octet-stream");
  char disp[100]; snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);
  httpd_resp_set_hdr(req, "Content-Disposition", disp);
  uint8_t buf[4096]; size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0)
    if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) break;
  f.close(); httpd_resp_send_chunk(req, NULL, 0); return ESP_OK;
}

static esp_err_t settings_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  char buf[256]={0}; httpd_req_recv(req, buf, sizeof(buf)-1);
  char *p;
  if ((p=strstr(buf,"\"interval\":")))  intervalMin=atoi(p+11);
  if ((p=strstr(buf,"\"duration\":")))  durationSec=atoi(p+11);
  if ((p=strstr(buf,"\"camRes\":")))    camRes=atoi(p+9);
  nightMode = strstr(buf,"\"nightMode\":true") != NULL;
  irLedAuto = strstr(buf,"\"irAuto\":true")    != NULL;
  saveSettings(); applyNightMode();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK;
}

static esp_err_t record_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  xTaskCreate([](void*){ recordVideo(); vTaskDelete(NULL); }, "rec", 8192, NULL, 5, NULL);
  return ESP_OK;
}

static esp_err_t control_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  char query[128]={0}, val[16]={0};
  httpd_req_get_url_query_str(req, query, sizeof(query));
  if (httpd_query_key_value(query, "led",   val, sizeof(val)) == ESP_OK)
    digitalWrite(IR_LED_PIN, strcmp(val,"on")==0 ? HIGH : LOW);
  if (httpd_query_key_value(query, "night", val, sizeof(val)) == ESP_OK)
    { nightMode = strcmp(val,"on")==0; applyNightMode(); }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK;
}

static esp_err_t deleteall_handler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK;
  if (sdOk) {
    File root = SD.open("/videos"); File f = root.openNextFile();
    while (f) {
      char path[80]; snprintf(path, sizeof(path), "/videos/%s", f.name());
      f.close(); SD.remove(path); f = root.openNextFile();
    }
    videoCount = 0; saveSettings();
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}"); return ESP_OK;
}

void startCameraServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80; cfg.max_uri_handlers = 12; cfg.lru_purge_enable = true;
  if (httpd_start(&web_httpd, &cfg) != ESP_OK) { Serial.println("Webserver-Fehler!"); return; }
  httpd_uri_t routes[] = {
    {"/",          HTTP_GET,  root_handler,      NULL},
    {"/stream",    HTTP_GET,  stream_handler,    NULL},
    {"/status",    HTTP_GET,  status_handler,    NULL},
    {"/videos",    HTTP_GET,  videos_handler,    NULL},
    {"/download",  HTTP_GET,  download_handler,  NULL},
    {"/record",    HTTP_GET,  record_handler,    NULL},
    {"/control",   HTTP_GET,  control_handler,   NULL},
    {"/settings",  HTTP_POST, settings_handler,  NULL},
    {"/deleteall", HTTP_POST, deleteall_handler, NULL},
  };
  for (auto &r : routes) httpd_register_uri_handler(web_httpd, &r);
  Serial.println("Webserver OK (Port 80)");
}

// ═══════════════════════════════════════════════════════════════════════
// SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(5000);
  Serial.println("\n\n=== StallCam ===");

  pinMode(STATUS_LED, OUTPUT);
  pinMode(IR_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);
  digitalWrite(IR_LED_PIN, LOW);

  loadSettings();
  initCamera();
  sdOk = initSD();
  initWiFi();
  startCameraServer();
  initRelay();

  digitalWrite(STATUS_LED, LOW);

  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  Serial.println("Lokal: http://" + ip);
  Serial.printf("Auth:  %s / %s\n", CAM_USER, CAM_PASS);
  Serial.printf("Broker: %s\n", strlen(BROKER_URL)>8 ? BROKER_URL : "nicht konfiguriert");
}

void loop() {
  unsigned long wait = (unsigned long)intervalMin * 60000UL;
  unsigned long t0 = millis();
  while (millis() - t0 < wait) delay(1000);
  if (sdOk) recordVideo();
}
