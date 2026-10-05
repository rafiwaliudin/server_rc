/*
 * ESP32-S3 Mini RC Controller
 * ====================================
 * Menerima data proporsional dari Python WebSocket Server
 * Mengontrol Motor dengan PWM (TC118S) dan Steering Servo.
 * Menambahkan kontrol untuk lampu (Headlight, Taillight, Turn L/R).
 *
 * Library yang dibutuhkan:
 *   - ArduinoWebSockets by Gil Maimon
 *   - ArduinoJson by Benoit Blanchon
 *   - ESP32Servo
 */

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

// ─── KONFIGURASI WiFi
// ─────────────────────────────────────────────────────────
const char *WIFI_SSID = "ERWAYTECH";
const char *WIFI_PASSWORD = "rafiochy";

// ─── KONFIGURASI WebSocket
// ────────────────────────────────────────────────────
const char *WS_HOST = "192.168.137.1";
const uint16_t WS_PORT = 8888;
const char *WS_PATH = "/";

// ─── PIN KONFIGURASI DIGITAL OUTPUT
// ───────────────────────────────────────────
#define PIN_MOTOR_FWD 0    // TC118S Driver 1 IN1 — Forward PWM
#define PIN_MOTOR_REV 1    // TC118S Driver 1 IN2 — Reverse/Brake PWM
#define PIN_MOTOR_LEFT 3   // TC118S Driver 2 IN1 — Steering Left PWM
#define PIN_MOTOR_RIGHT 10 // TC118S Driver 2 IN2 — Steering Right PWM
#define PIN_LED_HEAD 4     // Headlight White — LEDC dimming
#define PIN_LED_TAIL 5     // Taillight Red   — LEDC dimming
#define PIN_LED_TURN_L 6   // Turn Left Amber — GPIO on/off (LEDC habis)
#define PIN_LED_TURN_R 7   // Turn Right Amber — GPIO on/off
#define PIN_BUZZER 9       // Horn Buzzer — Active HIGH
#define PIN_LED_STATUS 8   // Built-in LED on ESP32-C3 Mini — Status Indicator

// ─── KONFIGURASI LEDC & SERVO
// ─────────────────────────────────────────────────
#define MOTOR_FREQ 1000
#define LED_FREQ 1000
#define LEDC_RES 8

// ─── ID MOBIL INI ────────────────────────────────────────────────────────────
// Ganti angka ini untuk masing-masing mobil (1, 2, 3, 4, 5, 6)
#define MY_CAR_ID 1

// ─── OBJEK GLOBAL ────────────────────────────────────────────────────────────
WebSocketsClient wsClient;

struct ControlData {
  float forward = 0.0;
  float reverse = 0.0;
  float left = 0.0;
  float right = 0.0;
  bool horn = false;
  bool headlight = false;
  bool r1 = false;
  bool l1 = false;
  bool connected = false;
  unsigned long lastUpdate = 0;
};

ControlData ctrl;
bool wsConnected = false;

// Konfigurasi Speed Motor DC
int currentSpeedLevel =
    5; // Default 100% (level 1=20%, 2=40%, 3=60%, 4=80%, 5=100%)
bool lastR1 = false;
bool lastL1 = false;

// ─── SETUP ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32-S3 Mini RC Controller ===");

  // PENTING: Paksa pin motor menjadi solid LOW (0V) di awal untuk mencegah
  // motor jalan sendiri/glitch saat power on
  pinMode(PIN_MOTOR_FWD, OUTPUT);
  digitalWrite(PIN_MOTOR_FWD, LOW);
  pinMode(PIN_MOTOR_REV, OUTPUT);
  digitalWrite(PIN_MOTOR_REV, LOW);
  pinMode(PIN_MOTOR_LEFT, OUTPUT);
  digitalWrite(PIN_MOTOR_LEFT, LOW);
  pinMode(PIN_MOTOR_RIGHT, OUTPUT);
  digitalWrite(PIN_MOTOR_RIGHT, LOW);

  // Inisialisasi Turn Signals (GPIO Output Biasa)
  pinMode(PIN_LED_TURN_L, OUTPUT);
  pinMode(PIN_LED_TURN_R, OUTPUT);
  digitalWrite(PIN_LED_TURN_L, LOW);
  digitalWrite(PIN_LED_TURN_R, LOW);

  // Inisialisasi Horn Buzzer (Active HIGH)
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // Inisialisasi LED Status (Built-in LED)
  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, LOW);

  // Inisialisasi Headlight & Taillight (LEDC Core 3.x API)
  ledcAttach(PIN_LED_HEAD, LED_FREQ, LEDC_RES);
  ledcWrite(PIN_LED_HEAD, 0);

  ledcAttach(PIN_LED_TAIL, LED_FREQ, LEDC_RES);
  ledcWrite(PIN_LED_TAIL, 0);

  // Pasang LEDC PWM ke pin Motor Penggerak
  ledcAttach(PIN_MOTOR_FWD, MOTOR_FREQ, LEDC_RES);
  ledcWrite(PIN_MOTOR_FWD, 0);

  ledcAttach(PIN_MOTOR_REV, MOTOR_FREQ, LEDC_RES);
  ledcWrite(PIN_MOTOR_REV, 0);

  // Pasang LEDC PWM ke pin Motor Steering
  ledcAttach(PIN_MOTOR_LEFT, MOTOR_FREQ, LEDC_RES);
  ledcWrite(PIN_MOTOR_LEFT, 0);

  ledcAttach(PIN_MOTOR_RIGHT, MOTOR_FREQ, LEDC_RES);
  ledcWrite(PIN_MOTOR_RIGHT, 0);

  // Bersihkan data control awal
  stopAll();

  // WiFi
  connectWiFi();
  WiFi.setSleep(false); // Disable WiFi power saving for stability

  // WebSocket
  wsClient.begin(WS_HOST, WS_PORT, WS_PATH);
  wsClient.onEvent(wsEventHandler);
  wsClient.setReconnectInterval(2000);
  wsClient.enableHeartbeat(5000, 3000, 2);

  Serial.println("[WS] Menghubungkan ke ws://" + String(WS_HOST) + ":" +
                 String(WS_PORT));
}

// ─── LOOP
// ─────────────────────────────────────────────────────────────────────

void loop() {
  // Pastikan WiFi tetap terhubung
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 5000) {
      Serial.println("[WiFi] Lost connection, reconnecting...");
      WiFi.disconnect();
      WiFi.reconnect();
      lastWiFiCheck = millis();
    }
  }

  wsClient.loop();

  // Timeout: Jika tidak ada data selama 500ms, hentikan motor
  if (ctrl.connected && (millis() - ctrl.lastUpdate > 500)) {
    stopAll();
    ctrl.connected = false;
  }

  // High-frequency debug print disabled to minimize serial transmission latency
  /*
  static unsigned long debugTimer = 0;
  if (millis() - debugTimer > 200) {
    if (wsConnected) {
      Serial.printf("[CTRL] FWD:%.2f | REV:%.2f | L:%.2f | R:%.2f | HORN:%d | "
                    "L1:%d | R1:%d | SPD:%d\n",
                    ctrl.forward,
                    ctrl.reverse, ctrl.left, ctrl.right,
                    ctrl.horn, ctrl.l1, ctrl.r1, currentSpeedLevel);
    }
    debugTimer = millis();
  }
  */

  // LED Status Indicator Logic: kedip cepat jika belum konek WS, standby ON
  // jika konek
  static unsigned long lastLEDUpdate = 0;
  static bool ledState = false;
  if (wsConnected) {
    digitalWrite(PIN_LED_STATUS, HIGH); // Standby ON
  } else {
    if (millis() - lastLEDUpdate > 150) { // Kedip cepat 150ms
      ledState = !ledState;
      digitalWrite(PIN_LED_STATUS, ledState ? HIGH : LOW);
      lastLEDUpdate = millis();
    }
  }

  applyControls();
}

// ─── APPLY CONTROLS ──────────────────────────────────────────────────────────

void applyControls() {
  // L1 & R1 speed level logic removed here because the Python server
  // is now the single source of truth and manages/broadcasts speed_limit.

  // 1. Motor Control (PWM Proportional — already capped by server!)
  int fwdPwm = (int)(ctrl.forward * 255.0);
  int revPwm = (int)(ctrl.reverse * 255.0);

  if (fwdPwm > 255)
    fwdPwm = 255;
  if (revPwm > 255)
    revPwm = 255;

  ledcWrite(PIN_MOTOR_FWD, fwdPwm);
  ledcWrite(PIN_MOTOR_REV, revPwm);

  // 2. Steering Control (DC Motor)
  // Note: nilai ctrl.left dan ctrl.right sudah dilimit dari server/parsing data
  int leftPwm = (int)(ctrl.left * 255.0);
  int rightPwm = (int)(ctrl.right * 255.0);

  if (leftPwm > 255)
    leftPwm = 255;
  if (rightPwm > 255)
    rightPwm = 255;

  ledcWrite(PIN_MOTOR_LEFT, leftPwm);
  ledcWrite(PIN_MOTOR_RIGHT, rightPwm);

  // 3. Headlight (Nyala redup sebagai DRL saat konek, terang saat
  // horn/headlight ditekan)
  int headLightPwm = 0;
  if (wsConnected)
    headLightPwm = 64; // DRL 25% brightness
  if (ctrl.horn || ctrl.headlight)
    headLightPwm = 255; // High beam 100%
  ledcWrite(PIN_LED_HEAD, headLightPwm);

  // 4. Taillight (Nyala redup saat jalan biasa, nyala terang saat mengerem /
  // reverse)
  int tailLightPwm = 0;
  if (wsConnected)
    tailLightPwm = 32; // Normal tail light
  if (ctrl.reverse > 0.05)
    tailLightPwm = 255; // Brake light
  ledcWrite(PIN_LED_TAIL, tailLightPwm);

  // 5. Turn Signals (Berkedip otomatis saat setir dibelokkan)
  static unsigned long lastBlink = 0;
  static bool blinkState = false;
  if (millis() - lastBlink > 300) { // Blink interval 300ms
    blinkState = !blinkState;
    lastBlink = millis();
  }

  // Nyalakan sen kiri jika belok kiri
  if (ctrl.left > 0.1) {
    digitalWrite(PIN_LED_TURN_L, blinkState ? HIGH : LOW);
  } else {
    digitalWrite(PIN_LED_TURN_L, LOW);
  }

  // Nyalakan sen kanan jika belok kanan
  if (ctrl.right > 0.1) {
    digitalWrite(PIN_LED_TURN_R, blinkState ? HIGH : LOW);
  } else {
    digitalWrite(PIN_LED_TURN_R, LOW);
  }

  // 6. Buzzer/Horn Control (Active HIGH on Pin 8)
  digitalWrite(PIN_BUZZER, ctrl.horn ? HIGH : LOW);
}

void stopAll() {
  ctrl.forward = 0.0;
  ctrl.reverse = 0.0;
  ctrl.left = 0.0;
  ctrl.right = 0.0;
  ctrl.horn = false;
  ctrl.headlight = false;
  ctrl.l1 = false;
  ctrl.r1 = false;
  // Apply controls akan otomatis mematikan semua motor DC
}

// ─── WEBSOCKET EVENT HANDLER
// ──────────────────────────────────────────────────

void wsEventHandler(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.println("[WS] Terputus");
    wsConnected = false;
    stopAll();
    applyControls(); // Terapkan stop segera
    break;

  case WStype_CONNECTED:
    Serial.println("[WS] Terhubung ke server!");
    wsConnected = true;
    wsClient.sendTXT("{\"type\":\"ping\",\"device\":\"esp32s3\"}");
    break;

  case WStype_TEXT:
    parseMessage(payload, length);
    break;

  default:
    break;
  }
}

// ─── PARSE JSON MESSAGE
// ───────────────────────────────────────────────────────

void parseMessage(uint8_t *payload, size_t length) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    // Hindari spam serial saat error
    return;
  }

  const char *msgType = doc["type"];
  if (msgType && strcmp(msgType, "controller_data") == 0) {
    JsonObject data = doc["data"];

    // 0. Cek Target Mobil (Multi RC Car Channel)
    int target_car = data["active_car_id"] | 1; // Default ke 1 jika server lama
    if (target_car != MY_CAR_ID) {
      stopAll();
      applyControls();
      return; // Jangan lanjutkan membaca gas dan setir
    }

    // 1. Baca data Motor (Forward / Reverse)
    // Coba baca dari "throttle" & "brake" (format gauge bar)
    if (data.containsKey("throttle")) {
      ctrl.forward = data["throttle"] | 0.0f;
    } else {
      ctrl.forward = data["forward"] | 0.0f;
    }

    if (data.containsKey("brake")) {
      ctrl.reverse = data["brake"] | 0.0f;
    } else {
      ctrl.reverse = data["reverse"] | 0.0f;
    }

    // 2. Baca data Steering (Belok Kiri / Kanan)
    // Jika format gauge bar (steering_raw dari -1.0 sampai 1.0)
    if (data.containsKey("steering_raw")) {
      float steering = data["steering_raw"] | 0.0f;

      // Limit steering maksimal di 0.60 (60%) agar tidak belok patah
      if (steering > 0.60f)
        steering = 0.60f;
      if (steering < -0.60f)
        steering = -0.60f;

      if (steering < 0) {
        ctrl.left = -steering; // Jadikan positif untuk left
        ctrl.right = 0.0f;
      } else {
        ctrl.left = 0.0f;
        ctrl.right = steering;
      }
    } else {
      ctrl.left = data["left"] | 0.0f;
      ctrl.right = data["right"] | 0.0f;

      // Limit untuk input kiri/kanan normal
      if (ctrl.left > 0.60f)
        ctrl.left = 0.60f;
      if (ctrl.right > 0.60f)
        ctrl.right = 0.60f;
    }

    // 3. Baca boolean
    ctrl.horn = data["horn"] | false;
    ctrl.headlight = data["headlight"] | false;

    // Baca L1 dan R1 (Bisa dari root atau di dalam objek "buttons")
    // Pygame biasanya memetakan LB/L1 ke btn_4 dan RB/R1 ke btn_5
    // Nilainya adalah integer (0 atau 1) dari joystick.get_button()
    ctrl.l1 = data["L1"] | false;
    ctrl.r1 = data["R1"] | false;
    if (data.containsKey("buttons")) {
      JsonObject btns = data["buttons"];
      if (btns.containsKey("L1"))
        ctrl.l1 = btns["L1"].as<int>() > 0;
      if (btns.containsKey("R1"))
        ctrl.r1 = btns["R1"].as<int>() > 0;
      if (btns.containsKey("btn_4"))
        ctrl.l1 = btns["btn_4"].as<int>() > 0;
      if (btns.containsKey("btn_5"))
        ctrl.r1 = btns["btn_5"].as<int>() > 0;
    }

    ctrl.connected = data["connected"] | false;

    if (data.containsKey("speed_limit")) {
      currentSpeedLevel = data["speed_limit"].as<int>();
    }

    ctrl.lastUpdate = millis();
  }
}

// ─── WIFI CONNECT
// ─────────────────────────────────────────────────────────────

void connectWiFi() {
  Serial.print("[WiFi] Menghubungkan ke " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Terhubung! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Gagal! Restart...");
    delay(3000);
    ESP.restart();
  }
}

/*
 * ==============================================================================
 * PANDUAN WIRING KONEKSI MOTOR DC (MENGGUNAKAN 2x IC TC118S)
 * ==============================================================================
 *
 * Karena ESP32 tidak bisa memberikan arus yang cukup untuk memutar motor DC
 * secara langsung, Anda WAJIB menggunakan Motor Driver (seperti TC118S) untuk
 * motor steering, sama seperti yang digunakan pada motor penggerak utama. Anda
 * akan membutuhkan 2 buah IC Motor Driver (1 untuk jalan, 1 untuk belok).
 *
 * --- MOTOR DRIVER 1 (PENGGERAK UTAMA / DRIVE) ---
 * - VDD/VCC   -> Positif Baterai (contoh: 3.7V Li-ion)
 * - GND       -> Negatif Baterai & GND ESP32
 * - IN1       -> ESP32 PIN 0 (Forward)
 * - IN2       -> ESP32 PIN 1 (Reverse)
 * - OUT1      -> Kabel Motor Penggerak Utama 1
 * - OUT2      -> Kabel Motor Penggerak Utama 2
 *
 * --- MOTOR DRIVER 2 (STEERING / BELOK) ---
 * - VDD/VCC   -> Positif Baterai (contoh: 3.7V Li-ion)
 * - GND       -> Negatif Baterai & GND ESP32
 * - IN1       -> ESP32 PIN 3 (Left)
 * - IN2       -> ESP32 PIN 10 (Right)
 * - OUT1      -> Kabel Motor Steering 1
 * - OUT2      -> Kabel Motor Steering 2
 *
 * Catatan:
 * Jika arah putaran motor terbalik (misal ditekan kiri malah ke kanan atau maju
 * jadi mundur), cukup tukar posisi kabel pada OUT1 dan OUT2 di motor driver
 * tersebut.
 * ==============================================================================
 */
