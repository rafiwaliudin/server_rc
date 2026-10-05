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
#include <ESP32Servo.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

// ─── KONFIGURASI WiFi
// ─────────────────────────────────────────────────────────
//const char *WIFI_SSID = "SERVER_RC";
//const char *WIFI_PASSWORD = "Autopia2026";

const char *WIFI_SSID = "Alfabeta_Room";
const char *WIFI_PASSWORD = "Alfabeta2025";

// ─── KONFIGURASI WebSocket
// ────────────────────────────────────────────────────
// const char *WS_HOST = "172.20.10.2"; //Iphone Hotspot
// const char *WS_HOST = "10.42.0.1"; //ServerRC Hotspot
const char *WS_HOST = "192.168.100.196"; //Alfabeta_Room Hotspot
const uint16_t WS_PORT = 8888;
const char *WS_PATH = "/";

// ─── PIN KONFIGURASI DIGITAL OUTPUT
// ───────────────────────────────────────────
#define PIN_MOTOR_FWD 0  // TC118S Driver 1 IN1 — Forward PWM
#define PIN_MOTOR_REV 1  // TC118S Driver 1 IN2 — Reverse/Brake PWM
#define PIN_SERVO 3      // Steering Servo proporsional
#define PIN_LED_HEAD 4   // Headlight White — LEDC dimming
#define PIN_LED_TAIL 5   // Taillight Red   — LEDC dimming
#define PIN_LED_TURN_L 7 // Turn Left Amber — GPIO on/off (LEDC habis)
#define PIN_LED_TURN_R 6 // Turn Right Amber — GPIO on/off 
#define PIN_BUZZER 9     // Horn Buzzer — Active HIGH
#define PIN_LED_STATUS 8 // Built-in LED on ESP32-C3 Mini — Status Indicator

// ─── KONFIGURASI BATERAI TELEMETRI (LiPo 1S 3.7V)
// ──────────────────────────────────────────
#define PIN_BATTERY 2 // Gunakan GPIO2 (ADC1_CH2) untuk membaca voltase baterai

// Konfigurasi Kalibrasi & Hardware Telemetri
const float referenceVoltage = 3.3; 
const int adcResolution = 4095; // 12-bit ADC
const float calibrationFactor = 0.73; // Kalibrasi (Tegangan Asli / Tegangan Terbaca)
const float R1 = 10000.0; 
const float R2 = 10000.0;
const float dividerRatio = (R1 + R2) / R2;

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
Servo steeringServo;

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
unsigned long lastTelemetryUpdate = 0; // Timer untuk telemetri

// Konfigurasi Speed Motor DC
int currentSpeedLevel =
    5; // Default 50% (level 1=10%, 2=20%, 3=30%, 4=40%, 5=50%)
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

  // Inisialisasi Servo
  steeringServo.attach(PIN_SERVO);
  steeringServo.write(90); // Mulai dari posisi tengah

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

  // Inisialisasi Pin Baterai
  pinMode(PIN_BATTERY, INPUT);

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

  // Kirim data telemetri baterai setiap 2 detik jika terhubung ke server
  if (wsConnected && (millis() - lastTelemetryUpdate > 2000)) {
    sendTelemetry();
    lastTelemetryUpdate = millis();
  }
}

// ─── APPLY CONTROLS ──────────────────────────────────────────────────────────

void applyControls() {
  // L1 & R1 speed level logic removed here because the Python server
  // is now the single source of truth and manages/broadcasts speed_limit.

  // 1. Motor Control (Maju & Mundur)
  // Menyesuaikan batas maksimum PWM per speed level agar tarikan gas tetap
  // proporsional dari 0. Speed 1: max ~150, Speed 5: max 255
  int maxFwdPwm = 125 + (currentSpeedLevel * 26);
  if (maxFwdPwm > 255)
    maxFwdPwm = 255;

  // Mundur dibuat sama persis dengan kecepatan maju
  int maxRevPwm = maxFwdPwm;

  // Nilai PWM proporsional dengan menghilangkan deadzone motor DC
  // Motor DC umumnya butuh minimal PWM 40-50 agar bisa mulai berputar.
  const int MIN_PWM = 50;

  int fwdPwm = 0;
  if (ctrl.forward > 0.01) {
    fwdPwm = MIN_PWM + (int)(ctrl.forward * (maxFwdPwm - MIN_PWM));
  }

  int revPwm = 0;
  if (ctrl.reverse > 0.01) {
    revPwm = MIN_PWM + (int)(ctrl.reverse * (maxRevPwm - MIN_PWM));
  }

  if (fwdPwm > 255)
    fwdPwm = 255;
  if (revPwm > 255)
    revPwm = 255;

  static int lastFwdPwm = -1;
  static int lastRevPwm = -1;

  if (fwdPwm != lastFwdPwm) {
    ledcWrite(PIN_MOTOR_FWD, fwdPwm);
    lastFwdPwm = fwdPwm;
  }
  if (revPwm != lastRevPwm) {
    ledcWrite(PIN_MOTOR_REV, revPwm);
    lastRevPwm = revPwm;
  }

  // 2. Steering Control (Proporsional dengan Servo)
  // Hitung steering berdasarkan perbedaan kanan dan kiri (range -1.0 to 1.0)
  float steeringVal = ctrl.right - ctrl.left;

  // Konversi steering -1.0 s/d 1.0 menjadi sudut servo proporsional
  // 90 adalah tengah (center)
  // 70 adalah full kiri
  // 110 adalah full kanan
  int targetAngle = 90 + (int)(steeringVal * 25.0);
  if (targetAngle < 65)
    targetAngle = 65;
  if (targetAngle > 115)
    targetAngle = 115;

  // Terapkan langsung ke Servo (hilangkan software smoothing agar real-time dan
  // tidak terasa delay)
  static int lastAngle = -1;
  if (targetAngle != lastAngle) {
    steeringServo.write(targetAngle);
    lastAngle = targetAngle;
  }

  // 3. Headlight (Nyala redup sebagai DRL saat konek, terang saat
  // horn/headlight ditekan)
  int headLightPwm = 0;
  if (wsConnected)
    headLightPwm = 64; // DRL 25% brightness
  if (ctrl.horn || ctrl.headlight)
    headLightPwm = 255; // High beam 100%

  static int lastHeadLightPwm = -1;
  if (headLightPwm != lastHeadLightPwm) {
    ledcWrite(PIN_LED_HEAD, headLightPwm);
    lastHeadLightPwm = headLightPwm;
  }

  // 4. Taillight (Nyala redup saat jalan biasa, nyala terang saat mengerem /
  // reverse)
  int tailLightPwm = 0;
  if (wsConnected)
    tailLightPwm = 32; // Normal tail light
  if (ctrl.reverse > 0.05)
    tailLightPwm = 255; // Brake light

  static int lastTailLightPwm = -1;
  if (tailLightPwm != lastTailLightPwm) {
    ledcWrite(PIN_LED_TAIL, tailLightPwm);
    lastTailLightPwm = tailLightPwm;
  }

  // 5. Turn Signals (Berkedip otomatis saat setir dibelokkan)
  static unsigned long lastBlink = 0;
  static bool blinkState = false;
  if (millis() - lastBlink > 300) { // Blink interval 300ms
    blinkState = !blinkState;
    lastBlink = millis();
  }

  // Nyalakan sen kiri jika belok kiri
  bool turnL = (ctrl.left > 0.1) && blinkState;
  static bool lastTurnL = false;
  if (turnL != lastTurnL) {
    digitalWrite(PIN_LED_TURN_L, turnL ? HIGH : LOW);
    lastTurnL = turnL;
  }

  // Nyalakan sen kanan jika belok kanan
  bool turnR = (ctrl.right > 0.1) && blinkState;
  static bool lastTurnR = false;
  if (turnR != lastTurnR) {
    digitalWrite(PIN_LED_TURN_R, turnR ? HIGH : LOW);
    lastTurnR = turnR;
  }

  // 6. Buzzer/Horn Control (Active HIGH on Pin 8)
  static bool lastHorn = false;
  if (ctrl.horn != lastHorn) {
    digitalWrite(PIN_BUZZER, ctrl.horn ? HIGH : LOW);
    lastHorn = ctrl.horn;
  }
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

// ─── SEND TELEMETRY ──────────────────────────────────────────────────────────

// 1. Function to read accurate voltage using multiple samples (Tanpa delay agar RC tidak lag)
float readBatteryVoltage() {
  int numReadings = 20; 
  long totalAnalog = 0;
  for (int i = 0; i < numReadings; i++) {
    totalAnalog += analogRead(PIN_BATTERY);
  }
  float avgAnalog = (float)totalAnalog / numReadings;
  float pinVoltage = (avgAnalog / (float)adcResolution) * referenceVoltage;
  return pinVoltage * dividerRatio * calibrationFactor;
}

// 2. Function to calculate battery percentage accurately (Non-linear untuk LiPo)
int calculateBatteryPercentage(float voltage) {
  if (voltage >= 4.20) return 100;
  if (voltage >= 4.10) return 95;
  if (voltage >= 4.00) return 85;
  if (voltage >= 3.90) return 75;
  if (voltage >= 3.80) return 60;
  if (voltage >= 3.75) return 45;
  if (voltage >= 3.70) return 25;
  if (voltage >= 3.60) return 10;
  if (voltage >= 3.50) return 5;
  return 0;
}

// 3. Function to determine charging status
bool isCharging(float voltage) {
  return (voltage >= 4.20);
}

void sendTelemetry() {
  float batteryVoltage = readBatteryVoltage();
  int percentage = calculateBatteryPercentage(batteryVoltage);
  bool charging = isCharging(batteryVoltage);

  // Buat JSON manual dengan snprintf (Jauh lebih ringan & cepat dari ArduinoJson)
  char output[128];
  snprintf(output, sizeof(output), "{\"type\":\"telemetry\",\"car_id\":%d,\"data\":{\"battery_voltage\":%.2f,\"battery_percent\":%d,\"is_charging\":%s}}", MY_CAR_ID, batteryVoltage, percentage, charging ? "true" : "false");
  
  wsClient.sendTXT(output);
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

  if (msgType && strcmp(msgType, "sleep_esp") == 0) {
    int target_car = doc["car_id"] | 1;
    if (target_car == MY_CAR_ID) {
      Serial.println("[SLEEP] Memasuki Deep Sleep untuk menghemat baterai saat "
                     "charging...");
      stopAll();
      applyControls(); // matikan motor dan lampu
      delay(500); // beri waktu sedikit agar perintah mati benar-benar terkirim
                  // ke hardware

      // Matikan WiFi agar lebih hemat sebelum sleep
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);

      // Masuk ke mode deep sleep
      esp_deep_sleep_start();
    }
    return;
  }

  if (msgType && strcmp(msgType, "controller_data") == 0) {
    JsonObject data = doc["data"];

    // 0. Cek Target Mobil (Multi RC Car Channel)
    int target_car = data["active_car_id"] | 1; // Default ke 1 jika server lama
    if (target_car != MY_CAR_ID) {
      return; // Abaikan pesan untuk mobil lain, jangan panggil stopAll() karena
              // akan bentrok!
    }

    // 0.5. Cek Status Game (Aktif / Idle)
    bool game_active =
        data["game_active"] | true; // Default true (backward compatibility)
    if (!game_active) {
      stopAll();
      applyControls();
      return; // Abaikan input jika game sedang idle
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

      // Limit steering maksimal di 1.0 (100%) agar bisa belok full 70-80
      // derajat
      if (steering > 1.0f)
        steering = 1.0f;
      if (steering < -1.0f)
        steering = -1.0f;

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
      if (ctrl.left > 1.0f)
        ctrl.left = 1.0f;
      if (ctrl.right > 1.0f)
        ctrl.right = 1.0f;
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
    steeringServo.write(135); // Kanan 45 derajat (90 + 45)
    delay(1000);
    if (WiFi.status() == WL_CONNECTED) break;

    steeringServo.write(45);  // Kiri 45 derajat (90 - 45)
    delay(1000);
    if (WiFi.status() == WL_CONNECTED) break;

    steeringServo.write(90);  // Ke center (tengah)
    delay(3000);

    Serial.print(".");
    tries++;
  }
  
  // Pastikan servo kembali ke tengah setelah selesai inisiasi
  steeringServo.write(90);

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
 * PANDUAN WIRING KONEKSI MOTOR DC & SERVO
 * ==============================================================================
 *
 * --- MOTOR DRIVER 1 (PENGGERAK UTAMA / DRIVE) TC118S ---
 * - VDD/VCC   -> Positif Baterai (contoh: 3.7V Li-ion)
 * - GND       -> Negatif Baterai & GND ESP32
 * - IN1       -> ESP32 PIN 0 (Forward)
 * - IN2       -> ESP32 PIN 1 (Reverse)
 * - OUT1      -> Kabel Motor Penggerak Utama 1
 * - OUT2      -> Kabel Motor Penggerak Utama 2
 *
 * --- MOTOR SERVO (STEERING / BELOK) ---
 * - VCC (Merah)    -> Positif Baterai atau 5V
 * - GND (Hitam)    -> Negatif Baterai & GND ESP32
 * - Signal (Kuning)-> ESP32 PIN 3 (Steering Servo proporsional)
 *
 * ==============================================================================
 */
