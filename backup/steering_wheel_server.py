import asyncio
import json
import math
import time
import threading
import os
import sys
from dataclasses import dataclass, asdict
from typing import Optional

# Setup encoding for Windows Console to handle emojis/special chars
if sys.platform == 'win32':
    sys.stdout.reconfigure(encoding='utf-8')

# Install dependencies if missing
try:
    import pygame
except ImportError:
    os.system(f"{sys.executable} -m pip install pygame")
    import pygame

try:
    import websockets
except ImportError:
    os.system(f"{sys.executable} -m pip install websockets")
    import websockets

try:
    import yaml
except ImportError:
    os.system(f"{sys.executable} -m pip install pyyaml")
    import yaml

# --- Default Configuration ---

DEFAULT_CONFIG = {
    "websocket": {
        "host": "0.0.0.0",
        "port": 8765,
        "broadcast_hz": 30,
    },
    "controller": {
        "joystick_index": 0,
        "axis_steering": 0,
        "axis_throttle": 4,
        "axis_brake": 5,
        "combined_pedals": False,
        "axis_combined": 2,
    },
    "calibration": {
        "steering_center": 0.0,
        "steering_range": 1.0,
        "steering_max_angle": 90.0,
        "throttle_min": -1.0,
        "throttle_max": 1.0,
        "brake_min": -1.0,
        "brake_max": 1.0,
        "deadzone_steering": 0.02,
        "deadzone_pedal": 0.02,
    }
}

CONFIG_FILE = "wheel_config.yaml"

def load_config():
    if os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, "r") as f:
            loaded = yaml.safe_load(f)
        config = DEFAULT_CONFIG.copy()
        for section, values in loaded.items():
            if section in config:
                config[section].update(values)
        return config
    return DEFAULT_CONFIG.copy()

def save_config(config):
    with open(CONFIG_FILE, "w") as f:
        yaml.dump(config, f, default_flow_style=False)
    print(f"[OK] Konfigurasi disimpan ke {CONFIG_FILE}")

# --- Global State ---

@dataclass
class ControllerState:
    steering_raw: float = 0.0
    steering_angle: float = 0.0
    steering_direction: str = "CENTER"
    throttle: float = 0.0
    brake: float = 0.0
    buttons: dict = None
    connected: bool = False
    timestamp: float = 0.0

    def __post_init__(self):
        if self.buttons is None:
            self.buttons = {}

state = ControllerState()
clients = set()
config = load_config()
calibrating = False

# --- Calculation Functions ---

def apply_deadzone(value: float, deadzone: float) -> float:
    if abs(value) < deadzone:
        return 0.0
    sign = 1 if value > 0 else -1
    return sign * (abs(value) - deadzone) / (1.0 - deadzone)

def axis_to_pedal(raw: float, axis_min: float, axis_max: float, deadzone: float) -> float:
    normalized = (raw - axis_max) / (axis_min - axis_max)
    normalized = max(0.0, min(1.0, normalized))
    if normalized < deadzone:
        return 0.0
    return (normalized - deadzone) / (1.0 - deadzone)

def axis_to_steering(raw: float, cal: dict) -> tuple:
    centered = raw - cal["steering_center"]
    normalized = centered / cal["steering_range"]
    normalized = max(-1.0, min(1.0, normalized))
    normalized = apply_deadzone(normalized, cal["deadzone_steering"])
    angle = normalized * cal["steering_max_angle"]
    if abs(angle) < 1.0:
        direction = "CENTER"
    elif angle > 0:
        direction = "RIGHT"
    else:
        direction = "LEFT"
    return angle, direction

# --- Calibration Mode ---

def run_calibration(joystick):
    global config, calibrating
    calibrating = True
    cal = config["calibration"]
    ctrl = config["controller"]

    print("\n" + "="*60)
    print("  MODE KALIBRASI STEERING WHEEL")
    print("="*60)

    print("\n[1/4] Pusatkan steer (jangan diputar), lalu tekan ENTER...")
    input()
    center_val = joystick.get_axis(ctrl["axis_steering"])
    cal["steering_center"] = round(center_val, 4)
    print(f"      Center: {center_val:.4f}")

    print("\n[2/4] Putar steer ke KIRI penuh, tahan, lalu tekan ENTER...")
    input()
    left_val = joystick.get_axis(ctrl["axis_steering"])
    print(f"      Nilai kiri: {left_val:.4f}")

    print("\n[3/4] Putar steer ke KANAN penuh, tahan, lalu tekan ENTER...")
    input()
    right_val = joystick.get_axis(ctrl["axis_steering"])
    print(f"      Nilai kanan: {right_val:.4f}")

    max_dev = max(abs(left_val - cal["steering_center"]),
                  abs(right_val - cal["steering_center"]))
    cal["steering_range"] = round(max_dev, 4)
    print(f"      Range: {max_dev:.4f}")

    print("\n[4/4] Injak GAS penuh, lalu tekan ENTER...")
    input()
    throttle_full = joystick.get_axis(ctrl["axis_throttle"])
    print(f"      Gas penuh: {throttle_full:.4f}")

    print("      Lepas GAS sepenuhnya, lalu tekan ENTER...")
    input()
    throttle_release = joystick.get_axis(ctrl["axis_throttle"])
    cal["throttle_min"] = round(throttle_full, 4)
    cal["throttle_max"] = round(throttle_release, 4)
    print(f"      Gas: min={throttle_full:.4f}, max={throttle_release:.4f}")

    if not ctrl["combined_pedals"]:
        print("\n      Injak REM penuh, lalu tekan ENTER...")
        input()
        brake_full = joystick.get_axis(ctrl["axis_brake"])
        print(f"      Rem penuh: {brake_full:.4f}")
        print("      Lepas REM sepenuhnya, lalu tekan ENTER...")
        input()
        brake_release = joystick.get_axis(ctrl["axis_brake"])
        cal["brake_min"] = round(brake_full, 4)
        cal["brake_max"] = round(brake_release, 4)
        print(f"      Rem: min={brake_full:.4f}, max={brake_release:.4f}")

    config["calibration"] = cal
    save_config(config)
    print("\n[OK] Kalibrasi selesai!")
    print("="*60 + "\n")
    calibrating = False

# --- WebSocket Server ---

async def handler(websocket):
    clients.add(websocket)
    print(f"[CONN] Client terhubung: {websocket.remote_address} (total: {len(clients)})")
    try:
        async for message in websocket:
            try:
                cmd = json.loads(message)
                if cmd.get("type") == "ping":
                    await websocket.send(json.dumps({"type": "pong"}))
                elif cmd.get("type") == "get_config":
                    await websocket.send(json.dumps({
                        "type": "config",
                        "data": config
                    }))
                elif cmd.get("type") == "update_config":
                    section = cmd.get("section")
                    key = cmd.get("key")
                    value = cmd.get("value")
                    if section in config and key in config[section]:
                        config[section][key] = value
                        save_config(config)
                        await websocket.send(json.dumps({"type": "config_updated", "ok": True}))
            except Exception:
                pass
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        clients.discard(websocket)
        print(f"[DISC] Client terputus: {websocket.remote_address} (sisa: {len(clients)})")

async def broadcast_loop():
    ws_cfg = config["websocket"]
    interval = 1.0 / ws_cfg["broadcast_hz"]
    while True:
        if clients and state.connected:
            payload = {
                "type": "controller_data",
                "data": {
                    "steering_angle": round(state.steering_angle, 2),
                    "steering_direction": state.steering_direction,
                    "steering_raw": round(state.steering_raw, 4),
                    "throttle": round(state.throttle, 3),
                    "brake": round(state.brake, 3),
                    "buttons": state.buttons,
                    "connected": state.connected,
                    "timestamp": state.timestamp,
                }
            }
            msg = json.dumps(payload)
            disconnected = set()
            for ws in list(clients):
                try:
                    await ws.send(msg)
                except Exception:
                    disconnected.add(ws)
            for ws in disconnected:
                clients.discard(ws)
        await asyncio.sleep(interval)

# --- Controller Reader Thread ---

def controller_thread():
    global state, config
    pygame.init()
    pygame.joystick.init()
    print("\n[INIT] Mendeteksi controller...")
    joystick = None

    while True:
        pygame.event.pump()
        count = pygame.joystick.get_count()

        if count == 0:
            if state.connected:
                print("[WARN] Controller terputus!")
                state.connected = False
                joystick = None
            time.sleep(1)
            continue

        if joystick is None:
            try:
                ctrl_idx = config["controller"]["joystick_index"]
                if ctrl_idx >= count:
                    ctrl_idx = 0
                joystick = pygame.joystick.Joystick(ctrl_idx)
                joystick.init()
                print(f"[OK] Controller ditemukan: {joystick.get_name()}")
                state.connected = True
                if joystick.get_numaxes() <= 3:
                    config["controller"]["combined_pedals"] = True
                    print("      Mode: Combined Pedals")
            except Exception as e:
                print(f"[ERR] Gagal inisialisasi: {e}")
                joystick = None
                state.connected = False
                time.sleep(1)
                continue

        try:
            for event in pygame.event.get():
                if event.type == pygame.JOYDEVICEREMOVED:
                    print("[WARN] Controller dicabut!")
                    joystick = None
                    state.connected = False
                    break

            if not state.connected or joystick is None:
                continue

            ctrl = config["controller"]
            cal = config["calibration"]

            if joystick.get_numaxes() > ctrl["axis_steering"]:
                raw_steer = joystick.get_axis(ctrl["axis_steering"])
                state.steering_raw = raw_steer
                state.steering_angle, state.steering_direction = axis_to_steering(raw_steer, cal)

            if ctrl["combined_pedals"]:
                if joystick.get_numaxes() > ctrl["axis_combined"]:
                    raw_combined = joystick.get_axis(ctrl["axis_combined"])
                    if raw_combined < 0:
                        state.throttle = axis_to_pedal(-raw_combined, -1.0, 0.0, cal["deadzone_pedal"])
                        state.brake = 0.0
                    else:
                        state.brake = axis_to_pedal(raw_combined, 1.0, 0.0, cal["deadzone_pedal"])
                        state.throttle = 0.0
            else:
                if joystick.get_numaxes() > ctrl["axis_throttle"]:
                    raw_throttle = joystick.get_axis(ctrl["axis_throttle"])
                    state.throttle = axis_to_pedal(raw_throttle, cal["throttle_min"], cal["throttle_max"], cal["deadzone_pedal"])
                if joystick.get_numaxes() > ctrl["axis_brake"]:
                    raw_brake = joystick.get_axis(ctrl["axis_brake"])
                    state.brake = axis_to_pedal(raw_brake, cal["brake_min"], cal["brake_max"], cal["deadzone_pedal"])

            buttons = {}
            for i in range(joystick.get_numbuttons()):
                buttons[f"btn_{i}"] = joystick.get_button(i)
            state.buttons = buttons
            state.timestamp = time.time()
        except Exception:
            joystick = None
            state.connected = False
            time.sleep(0.5)
        time.sleep(0.01)

# --- Main ---

async def main():
    ws_cfg = config["websocket"]
    host = ws_cfg["host"]
    port = ws_cfg["port"]

    t = threading.Thread(target=controller_thread, daemon=True)
    t.start()

    print(f"\n[INFO] Server: ws://{host}:{port}")
    print("       Dashboard: buka steering_dashboard.html")
    print("       Tekan [C] untuk kalibrasi (5s timeout)...")

    def get_input_with_timeout(timeout):
        if sys.platform == 'win32':
            import msvcrt
            start_time = time.time()
            while time.time() - start_time < timeout:
                if msvcrt.kbhit():
                    return msvcrt.getch().decode('utf-8').lower()
                time.sleep(0.1)
            return None
        else:
            import select
            rlist, _, _ = select.select([sys.stdin], [], [], timeout)
            if rlist:
                return sys.stdin.readline().strip().lower()
            return None

    choice = get_input_with_timeout(5)
    if choice == 'c':
        pygame.init()
        pygame.joystick.init()
        if pygame.joystick.get_count() > 0:
            joy = pygame.joystick.Joystick(0)
            joy.init()
            run_calibration(joy)
        else:
            print("[ERR] Controller belum terhubung!")

    print(f"\n[START] Server aktif di ws://{host}:{port}")
    try:
        async with websockets.serve(handler, host, port):
            await broadcast_loop()
    except asyncio.CancelledError:
        pass

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n\n[EXIT] Server dihentikan.")
    except Exception as e:
        print(f"\n[ERR] Server Error: {e}")

