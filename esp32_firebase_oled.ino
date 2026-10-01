// ============================================================
//  ESP32 + SSD1306 OLED  -  Firebase animation player
//  - ESP32 logs in to Firebase directly (Email/Password, REST)
//  - Reads the animation from Firestore (free Spark plan)
//  - Saves it to flash (LittleFS) so it plays offline / after reboot
//  - Checks every POLL_MS for a new animation chosen on the website
//
//  Libraries (Library Manager):
//    Adafruit SSD1306, Adafruit GFX, ArduinoJson (v7.x)
//  Board: ESP32 (any). OLED: SDA=21, SCL=22, address 0x3C
// ============================================================
#include "mbedtls/base64.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>

// ---------- YOUR SETTINGS ----------
const char *WIFI_SSID = "prasan";
const char *WIFI_PASS = "123456789";
const char *DEVICE_EMAIL =
    "prasanthanith5@gmail.com"; // create this user in Firebase > Authentication
const char *DEVICE_PASS = "prasanthanith5@gmail.com";

const char *API_KEY = "AIzaSyArvnz-mlIzSivbAsKetuPI5Y0NByh5-EA";
const char *PROJECT_ID = "euhjasg";
const unsigned long POLL_MS =
    20000; // 1 Firestore read per poll (free: 50,000 reads/day)
// -----------------------------------

#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
WiFiClientSecure secureClient;

String idToken;
unsigned long tokenExpiresAt = 0;
unsigned long lastPoll = 0;

// current animation (loaded from flash)
String localId, localVersion;
uint16_t aFrames = 0, aW = 0, aH = 0, aFrameBytes = 0;
uint16_t *aDelays = nullptr;

// ---------- OLED helpers ----------
void showMsg(const String &l1, const String &l2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(l1);
  display.setCursor(0, 36);
  display.println(l2);
  display.display();
}

// ---------- WiFi ----------
bool wifiConnect(uint32_t timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED)
    return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs)
    delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// ---------- Firebase Auth (email/password via REST) ----------
bool firebaseSignIn() {
  HTTPClient http;
  String url = String("https://identitytoolkit.googleapis.com/v1/"
                      "accounts:signInWithPassword?key=") +
               API_KEY;
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["email"] = DEVICE_EMAIL;
  body["password"] = DEVICE_PASS;
  body["returnSecureToken"] = true;
  String payload;
  serializeJson(body, payload);
  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("Sign-in failed: %d\n", code);
    http.end();
    secureClient.stop();
    return false;
  }
  JsonDocument doc;
  deserializeJson(doc, http.getString());
  http.end();
  secureClient.stop();
  idToken = doc["idToken"].as<String>();
  long expires = atol(doc["expiresIn"] | "3600");
  tokenExpiresAt =
      millis() + (unsigned long)(expires - 120) * 1000UL; // refresh 2 min early
  Serial.println("Firebase login OK");
  return idToken.length() > 0;
}

bool tokenValid() {
  return idToken.length() > 0 && (long)(millis() - tokenExpiresAt) < 0;
}

// ---------- Firestore REST GET ----------
int firestoreGet(const String &path, String &out, HTTPClient &http) {
  if (!tokenValid() && !firebaseSignIn())
    return -1;
  String url = String("https://firestore.googleapis.com/v1/projects/") +
               PROJECT_ID + "/databases/(default)/documents/" + path;
  http.begin(secureClient, url);
  http.addHeader("Authorization", "Bearer " + idToken);
  int code = http.GET();
  if (code == 200)
    out = http.getString();
  return code;
}

int firestoreGetOnce(const String &path, String &out) {
  HTTPClient http;
  int code = firestoreGet(path, out, http);
  http.end();
  secureClient.stop();
  return code;
}

long intField(JsonVariant f) {
  const char *s = f["integerValue"];
  return s ? atol(s) : 0;
}

// ---------- local storage (LittleFS) ----------
bool loadMeta() {
  File f = LittleFS.open("/meta.json", "r");
  if (!f)
    return false;
  JsonDocument m;
  if (deserializeJson(m, f)) {
    f.close();
    return false;
  }
  f.close();
  localId = m["id"].as<String>();
  localVersion = m["version"].as<String>();
  aFrames = m["frames"];
  aW = m["w"];
  aH = m["h"];
  aFrameBytes = ((aW + 7) / 8) * aH;
  free(aDelays);
  aDelays = (uint16_t *)malloc(aFrames * sizeof(uint16_t));
  for (uint16_t i = 0; i < aFrames; i++)
    aDelays[i] = m["delays"][i] | 100;
  return LittleFS.exists("/anim.bin") && aFrames > 0 && aFrameBytes <= 1024;
}

void saveMeta(const String &id, const String &version, uint16_t frames,
              uint16_t w, uint16_t h, const uint16_t *d) {
  JsonDocument m;
  m["id"] = id;
  m["version"] = version;
  m["frames"] = frames;
  m["w"] = w;
  m["h"] = h;
  JsonArray arr = m["delays"].to<JsonArray>();
  for (uint16_t i = 0; i < frames; i++)
    arr.add(d[i]);
  File f = LittleFS.open("/meta.json", "w");
  serializeJson(m, f);
  f.close();
}

// ---------- download animation from Firestore -> flash ----------
bool downloadAnimation(const String &id, const String &activeVersion) {
  String body;
  showMsg("New animation...", id);
  if (firestoreGetOnce("animations/" + id, body) != 200) {
    Serial.println("Animation doc not found");
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body))
    return false;
  body = String();
  JsonObject f = doc["fields"];
  uint16_t frames = intField(f["frameCount"]), w = intField(f["width"]),
           h = intField(f["height"]);
  uint16_t fpc = intField(f["framesPerChunk"]),
           chunks = intField(f["chunkCount"]);
  uint16_t frameBytes = ((w + 7) / 8) * h;
  if (!frames || !fpc || !chunks || frameBytes > 1024) {
    showMsg("Bad animation", "size not supported");
    delay(2000);
    return false;
  }

  uint16_t *delays = (uint16_t *)malloc(frames * sizeof(uint16_t));
  const char *csv = f["delays"]["stringValue"] | "";
  for (uint16_t i = 0; i < frames; i++) {
    delays[i] = 100;
  }
  {
    const char *p = csv;
    uint16_t i = 0;
    while (*p && i < frames) {
      delays[i++] = atoi(p);
      while (*p && *p != ',')
        p++;
      if (*p == ',')
        p++;
    }
  }
  doc.clear();

  size_t need = (size_t)frames * frameBytes;
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < need)
    LittleFS.remove("/anim.bin"); // make room
  File out = LittleFS.open("/anim.tmp", "w");
  if (!out) {
    free(delays);
    return false;
  }

  HTTPClient http;
  http.setReuse(true);
  bool ok = true;
  for (uint16_t c = 0; c < chunks && ok; c++) {
    showMsg("Downloading", String(c + 1) + " / " + String(chunks));
    int code;
    String cb;
    code = firestoreGet("animations/" + id + "/chunks/c" + c, cb, http);
    if (code != 200) {
      Serial.printf("chunk %d failed: %d\n", c, code);
      ok = false;
      break;
    }

    JsonDocument filter;
    filter["fields"]["data"]["stringValue"] = true;
    JsonDocument cd;
    DeserializationError e =
        deserializeJson(cd, cb, DeserializationOption::Filter(filter));
    cb = String();
    const char *b64 = cd["fields"]["data"]["stringValue"];
    if (e || !b64) {
      ok = false;
      break;
    }

    size_t inLen = strlen(b64), cap = inLen * 3 / 4 + 4, outLen = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
      ok = false;
      break;
    }
    if (mbedtls_base64_decode(buf, cap, &outLen, (const uint8_t *)b64, inLen) !=
        0)
      ok = false;
    else if (out.write(buf, outLen) != outLen)
      ok = false;
    free(buf);
  }
  http.end();
  secureClient.stop();
  out.close();

  if (ok) {
    File chk = LittleFS.open("/anim.tmp", "r");
    ok = chk && chk.size() == need;
    if (chk)
      chk.close();
  }
  if (!ok) {
    LittleFS.remove("/anim.tmp");
    free(delays);
    showMsg("Download failed", "will retry");
    delay(1500);
    return false;
  }

  LittleFS.remove("/anim.bin");
  LittleFS.rename("/anim.tmp", "/anim.bin");
  saveMeta(id, activeVersion, frames, w, h, delays);
  free(delays);
  loadMeta();
  Serial.println("Animation saved to flash");
  return true;
}

// ---------- ask Firebase which animation should play ----------
void checkForUpdate() {
  lastPoll = millis();
  if (!wifiConnect(8000))
    return;
  String body;
  int code = firestoreGetOnce("config/active", body);
  if (code != 200)
    return; // 404 = nothing selected yet
  JsonDocument doc;
  if (deserializeJson(doc, body))
    return;
  String id = doc["fields"]["animationId"]["stringValue"].as<String>();
  String ver = doc["fields"]["version"]["stringValue"].as<String>();
  if (id.length() && (id != localId || ver != localVersion)) {
    if (!downloadAnimation(id, ver))
      lastPoll = millis() - POLL_MS + 5000; // retry in 5 s
  }
}

// ---------- play from flash ----------
void playOnce() {
  File f = LittleFS.open("/anim.bin", "r");
  if (!f)
    return;
  uint8_t frame[1024];
  for (uint16_t i = 0; i < aFrames; i++) {
    uint32_t t0 = millis();
    if (f.read(frame, aFrameBytes) != aFrameBytes)
      break;
    display.clearDisplay();
    display.drawBitmap(
        0, 0, frame, aW, aH,
        SSD1306_WHITE); // same row-major 1-bit format as your .h files
    display.display();
    while (millis() - t0 < aDelays[i])
      delay(1);
  }
  f.close();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
    while (true)
      delay(1000);
  }
  showMsg("Starting...");
  if (!LittleFS.begin(true)) {
    showMsg("LittleFS error");
    while (true)
      delay(1000);
  }
  secureClient.setInsecure(); // simple; for production use
                              // secureClient.setCACert(root_ca)

  bool have = loadMeta();
  showMsg("WiFi...", WIFI_SSID);
  if (wifiConnect()) {
    showMsg("Firebase login...");
    checkForUpdate();
  } else
    showMsg("No WiFi", have ? "playing saved anim" : "");
  if (!have && !loadMeta())
    showMsg("No animation yet", "upload on website");
}

void loop() {
  if (aFrames > 0 && LittleFS.exists("/anim.bin"))
    playOnce();
  else
    delay(500);
  if (millis() - lastPoll > POLL_MS)
    checkForUpdate();
}
