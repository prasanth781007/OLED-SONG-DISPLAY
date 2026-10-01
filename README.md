# ESP32 OLED Animation Player (Firebase, free plan)

Files: `index.html` (website), `esp32_firebase_oled.ino` (ESP32), `firestore.rules`

## Why Firestore, not Storage
As far as I know, Firebase Storage now needs the paid Blaze plan, so the animation is stored
as base64 text in Firestore (free Spark plan): 16 frames per document, 14 docs for 222 frames.

## 1. Firebase console (project euhjasg)
1. Build > Authentication > Sign-in method > enable **Email/Password**.
2. Authentication > Users > Add user x2:  your website login + a device login (e.g. esp32@example.com).
3. Build > Firestore Database > Create database (production mode).
4. Firestore > Rules > paste `firestore.rules`, put your website email in it, Publish.

## 2. Website
Don't double-click the file (Firebase Auth fails on file://). Either
- `python -m http.server 8000` in this folder, open http://localhost:8000, or
- Firebase Hosting (free): `firebase init hosting` then `firebase deploy`.
If you deploy, add the domain in Authentication > Settings > Authorized domains (localhost is already allowed).

Login > choose your `.h` file > preview plays > Upload > press **Play on ESP32**.

## 3. ESP32
Install libraries: Adafruit SSD1306, Adafruit GFX, ArduinoJson 7.x.
Edit WiFi + device email/password at the top of the sketch, upload. OLED SDA=21, SCL=22.
The ESP32 saves the animation in flash (LittleFS), so it keeps playing offline and after reboot,
and checks for a new selection every 20 s.

## Supported .h format
The GSNCreations format from your files (`*_WIDTH`, `*_HEIGHT`, `*_FRAME_COUNT`, `*_delays[]`, `*_frames[][]`, 1-bit).
