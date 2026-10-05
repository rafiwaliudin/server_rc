#include <WiFi.h>
#include <WebServer.h>

// --- Configuration ---
const char* ssid = "Alfabeta_Room";
const char* password = "Alfabeta2025";

// --- Pins ---
const int batteryPin = 2; // Pin to read battery voltage (Analog pin 2 / GPIO 2)
// const int chargeStatusPin = 4; // Uncomment if you have a dedicated digital pin for charging status (e.g. from TP4056)

// --- Battery Specifications (Assuming standard 1S LiPo) ---
const float maxVoltage = 4.2;
const float minVoltage = 3.2;

// --- Hardware Calibration ---
// ESP32 ADC Reference Voltage is nominally 3.3V, but varies slightly.
const float referenceVoltage = 3.3; 
const int adcResolution = 4095; // 12-bit ADC for ESP32
// Kalibrasi Tegangan (Wajib disesuaikan jika menggunakan ESP32)
// ESP32 ADC sering meleset. Jika tidak ada Multimeter, dan baterai sudah agak lama dipakai, 
// tegangan aslinya mungkin sekitar 3.7V - 3.8V. 
// Rumus: kalibrasi = (Tegangan Asli) / (Tegangan Terbaca dengan factor 1.0)
const float calibrationFactor = 1.0; // <- UBAH INI NANTI SETELAH DIUKUR MULTIMETER

// Voltage Divider Resistors (Sesuai skema SMD 10k)
const float R1 = 10000.0; 
const float R2 = 10000.0;
const float dividerRatio = (R1 + R2) / R2;

WebServer server(80);

// --- Functions ---

// 1. Function to read accurate voltage using multiple samples
float readBatteryVoltage() {
  int numReadings = 20; // Take 20 samples to smooth out noise
  long totalAnalog = 0;
  for (int i = 0; i < numReadings; i++) {
    totalAnalog += analogRead(batteryPin);
    delay(5);
  }
  float avgAnalog = (float)totalAnalog / numReadings;
  
  // Calculate voltage at the analog pin
  float pinVoltage = (avgAnalog / (float)adcResolution) * referenceVoltage;
  
  // Calculate actual battery voltage considering the voltage divider & calibration
  float batVoltage = pinVoltage * dividerRatio * calibrationFactor;
  return batVoltage;
}

// 2. Function to calculate battery percentage accurately (Non-linear for LiPo)
int calculateBatteryPercentage(float voltage) {
  int percentage;
  
  // LiPo discharge curve is non-linear. This lookup table provides better accuracy.
  if (voltage >= 4.20) percentage = 100;
  else if (voltage >= 4.10) percentage = 95;
  else if (voltage >= 4.00) percentage = 85;
  else if (voltage >= 3.90) percentage = 75;
  else if (voltage >= 3.80) percentage = 60;
  else if (voltage >= 3.75) percentage = 45;
  else if (voltage >= 3.70) percentage = 25;
  else if (voltage >= 3.60) percentage = 10;
  else if (voltage >= 3.50) percentage = 5;
  else percentage = 0;

  return percentage;
}

// 3. Function to determine charging status
bool isCharging(float voltage) {
  // Method 1: Using voltage threshold
  // Charger standar (seperti TP4056) akan menahan tegangan di sekitar 4.20V saat mengisi daya.
  // Jika tegangan mencapai atau di atas 4.20V, kita asumsikan sedang dicolok charger.
  if (voltage >= 4.20) {
    return true;
  }
  
  return false;
}

// --- Web Server Handlers ---

void handleRoot() {
  float currentVoltage = readBatteryVoltage();
  int currentPercentage = calculateBatteryPercentage(currentVoltage);
  bool charging = isCharging(currentVoltage);
  
  String html = "<!DOCTYPE html><html><head><title>Battery Telemetry</title>";
  html += "<meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<style>";
  html += "body { font-family: 'Inter', 'Segoe UI', sans-serif; text-align: center; background-color: #0f172a; color: #f8fafc; padding-top: 10vh; margin: 0; }";
  html += ".card { background: #1e293b; border-radius: 20px; padding: 40px; display: inline-block; box-shadow: 0 10px 25px rgba(0,0,0,0.5); border: 1px solid #334155; min-width: 280px;}";
  html += ".title { font-size: 1.2em; text-transform: uppercase; letter-spacing: 2px; color: #94a3b8; margin-bottom: 20px; }";
  
  // Color code based on percentage
  String batteryColor = "#22c55e"; // Green
  if (currentPercentage <= 20) batteryColor = "#ef4444"; // Red
  else if (currentPercentage <= 50) batteryColor = "#eab308"; // Yellow
  
  html += ".value { font-size: 5em; font-weight: 800; margin: 10px 0; color: " + batteryColor + "; text-shadow: 0 0 20px " + batteryColor + "40; }";
  html += ".voltage { font-size: 1.5em; color: #cbd5e1; margin-bottom: 30px; font-weight: 300; }";
  html += ".status { font-size: 1.4em; font-weight: 600; padding: 10px 20px; border-radius: 30px; display: inline-block; }";
  
  if (charging) {
    html += ".status { background: rgba(59, 130, 246, 0.2); color: #60a5fa; border: 1px solid #3b82f6; }";
    html += ".charging-icon { margin-right: 8px; animation: pulse 1.5s infinite; }";
  } else {
    html += ".status { background: rgba(148, 163, 184, 0.1); color: #94a3b8; border: 1px solid #475569; }";
  }
  
  html += "@keyframes pulse { 0% { opacity: 0.5; } 50% { opacity: 1; text-shadow: 0 0 10px #60a5fa; } 100% { opacity: 0.5; } }";
  html += "</style>";
  
  // Auto-refresh the page every 3 seconds to update telemetry
  html += "<meta http-equiv=\"refresh\" content=\"3\">"; 
  html += "</head><body>";
  
  html += "<div class=\"card\">";
  html += "<div class=\"title\">Battery Status</div>";
  html += "<div class=\"value\">" + String(currentPercentage) + "%</div>";
  html += "<div class=\"voltage\">" + String(currentVoltage, 3) + " V</div>";
  
  if (charging) {
    html += "<div class=\"status\"><span class=\"charging-icon\">⚡</span> Charging</div>";
  } else {
    html += "<div class=\"status\">Discharging</div>";
  }
  
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(batteryPin, INPUT);
  // pinMode(chargeStatusPin, INPUT_PULLUP); // Uncomment if using dedicated charging pin
  
  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  // Setup Web Server
  server.on("/", handleRoot);
  server.begin();
  Serial.println("Telemetry server started on port 80");
}

void loop() {
  // Handle incoming HTTP requests
  server.handleClient();
  
  // Optional: Print to Serial monitor periodically for debugging
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 2000) {
    float v = readBatteryVoltage();
    Serial.printf("Voltage: %.3f V | Battery: %d%% | Charging: %s\n", 
                  v, calculateBatteryPercentage(v), isCharging(v) ? "Yes" : "No");
    lastPrint = millis();
  }
}
