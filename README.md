# 🎮 RC Steering Wheel WebSocket System

Sistem kontrol RC mobil menggunakan steering wheel Xbox 360 via Python WebSocket.

## 📁 File dalam paket ini

| File | Fungsi |
|------|--------|
| `steering_wheel_server.py` | Python WebSocket server - baca controller + broadcast data |
| `steering_dashboard.html` | Dashboard realtime di browser |
| `esp32_rc_controller.ino` | Kode Arduino ESP32 untuk kontrol servo/relay RC |
| `wheel_config.yaml` | Auto-generated saat pertama kali kalibrasi |

---

## 🖥️ Setup PC (Python Server)

### Install dependencies:
```bash
pip install pygame websockets pyyaml
```

### Jalankan server:
```bash
python steering_wheel_server.py
```

Saat pertama kali, server akan tanya mau kalibrasi atau langsung mulai.
- Tekan **[C]** untuk kalibrasi (disarankan!)
- Tekan **[ENTER]** untuk langsung mulai

---

## 🌐 Dashboard Browser

1. Buka file `steering_dashboard.html` langsung di browser
2. Masukkan `ws://localhost:8765` di kolom URL (sudah default)
3. Klik **CONNECT**

Dashboard akan menampilkan:
- 🎯 **Steering wheel visual** yang berputar realtime
- ⚡ **Bar gas & rem** dengan animasi
- 🔘 **Status semua tombol**
- 📊 **Raw axis values**
- 🔧 **Panel kalibrasi** (bisa adjust dari browser!)

---

## 🔌 Setup ESP32

### Library Arduino yang dibutuhkan:
1. `ArduinoWebSockets` by Gil Maimon
2. `ArduinoJson` by Benoit Blanchon
3. `ESP32Servo` (biasanya sudah ada)

### Konfigurasi sebelum upload:
Edit bagian ini di `esp32_rc_controller.ino`:
```cpp
const char* WIFI_SSID     = "NamaWiFiKamu";
const char* WIFI_PASSWORD = "PasswordWiFiKamu";
const char* WS_HOST       = "192.168.1.100";  // IP PC kamu
```

Cari IP PC:
- **Windows:** buka CMD → `ipconfig`
- **Linux/Mac:** `ifconfig` atau `ip addr`

### Wiring ESP32 → RC Car:
```
ESP32 GPIO18 → Servo Steer (signal wire)
ESP32 GPIO19 → ESC Throttle (signal wire)
ESP32 GPIO21 → Relay Module IN (untuk rem)
ESP32 GND    → Common GND
```

---

## 📡 Format Data WebSocket

Data yang dikirim server (JSON, 30x/detik):
```json
{
  "type": "controller_data",
  "data": {
    "steering_angle": -45.3,
    "steering_direction": "LEFT",
    "steering_raw": -0.5034,
    "throttle": 0.72,
    "brake": 0.0,
    "buttons": {"btn_0": false, "btn_1": true},
    "connected": true,
    "timestamp": 1714180800.123
  }
}
```

---

## 🔧 Kalibrasi

### Via terminal (saat start server):
```
[C] untuk masuk mode kalibrasi
```
Ikuti instruksi: center steer → kiri → kanan → gas → rem

### Via Dashboard Browser:
1. Buka panel **Kalibrasi** di sebelah kanan dashboard
2. Edit nilai steering center, range, deadzone, dll
3. Klik **SIMPAN KALIBRASI** → langsung dikirim ke server

### Parameter kalibrasi:
| Parameter | Fungsi |
|-----------|--------|
| `steering_center` | Nilai axis saat steer di tengah |
| `steering_range` | Jarak max dari center (±) |
| `steering_max_angle` | Max derajat (default 90°) |
| `deadzone_steering` | Dead zone steer (abaikan getaran kecil) |
| `throttle_min` | Nilai axis saat gas full |
| `deadzone_pedal` | Dead zone pedal |

---

## 🚨 Troubleshooting

**Controller tidak terdeteksi:**
- Pastikan driver Xbox 360 terinstall
- Coba unplug/replug controller
- Cek di Device Manager bahwa controller terbaca

**Steering terbalik:**
- Ganti `steering_range` menjadi negatif, atau
- Swap nilai `throttle_min` dan `throttle_max`

**ESP32 tidak bisa connect:**
- Pastikan PC dan ESP32 di WiFi yang sama
- Cek firewall PC: allow port 8765
- Cek IP PC sudah benar di kode Arduino

**Servo bergetar/jitter:**
- Naikkan nilai `deadzone_steering`
- Turunkan `SMOOTH_ALPHA` di ESP32 (lebih smooth)
