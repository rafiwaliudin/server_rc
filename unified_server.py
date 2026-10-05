import asyncio
import json
import time
import threading
import os
import sys
import socket
import signal
from dataclasses import dataclass, asdict

# Setup encoding for Windows
if sys.platform == 'win32':
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

# --- Install dependencies ---
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

try:
    import cv2
    from flask import Flask, Response, request
    import logging
except ImportError:
    os.system(f"{sys.executable} -m pip install opencv-python flask")
    import cv2
    from flask import Flask, Response, request
    import logging

# --- CONFIGURATION ---
CONFIG_FILE = "wheel_config.yaml"
DEFAULT_CONFIG = {
    "websocket": {"host": "0.0.0.0", "port": 8888, "broadcast_hz": 100},
    "controller": {
        "joystick_index": 0,
        "axis_steering": 0,
        "axis_throttle": 5,
        "axis_brake": 4,
        "combined_pedals": False,
        "axis_combined": 2,
        "speed_limits": [5, 5, 5],
        "active_car_ids": [1, 2, 3],
        "btn_horn": 0,
        "btn_headlight": 3,
        "btn_speed_down": 4,
        "btn_speed_up": 5,
        "btn_selection": 7,
    },
    "calibration": {
        "steering_center": 0.0,
        "steering_range": 1.0,
        "steering_max_angle": 90.0,
        "throttle_min": 1.0,
        "throttle_max": -1.0,
        "brake_min": 1.0,
        "brake_max": -1.0,
        "deadzone_steering": 0.02,
        "deadzone_pedal": 0.02,
    },
}


def load_config():
    if os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, "r") as f:
            loaded = yaml.safe_load(f)
        # Merge with defaults
        cfg = {}
        for section in DEFAULT_CONFIG:
            cfg[section] = dict(DEFAULT_CONFIG[section])
            if loaded and section in loaded:
                cfg[section].update(loaded[section])
        return cfg
    return {k: dict(v) for k, v in DEFAULT_CONFIG.items()}


def save_config(cfg):
    with open(CONFIG_FILE, "w") as f:
        yaml.dump(cfg, f, default_flow_style=False)
    print(f"[CFG] Saved to {CONFIG_FILE}")


config = load_config()


# --- GLOBAL STATE ---
@dataclass
class ControllerState:
    steering_angle: float = 0.0
    steering_direction: str = "CENTER"
    steering_raw: float = 0.0
    throttle: float = 0.0
    brake: float = 0.0
    buttons: dict = None
    connected: bool = False
    timestamp: float = 0.0
    active_car_id: int = 1
    speed_limit: int = 5  # Speed level (1 to 5, mapping to max throttle: 20%, 40%, 60%, 80%, 100%)
    headlight: bool = False
    trigger_selection: bool = False
    game_active: bool = False
    web_controlled: bool = False

    def __post_init__(self):
        if self.buttons is None:
            self.buttons = {}

ctrl_cfg = config.get("controller", {})
speed_limits = ctrl_cfg.get("speed_limits", [5, 5, 5])
active_car_ids = ctrl_cfg.get("active_car_ids", [1, 2, 3])

states = [
    ControllerState(
        active_car_id=active_car_ids[i] if i < len(active_car_ids) else i+1,
        speed_limit=speed_limits[i] if i < len(speed_limits) else 5
    )
    for i in range(3)
]
clients = set()

# --- VIDEO BRIDGE ---
video_app = Flask(__name__)
werkzeug_log = logging.getLogger("werkzeug")
werkzeug_log.setLevel(logging.ERROR)

@video_app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response

if "video" in config and "sources" in config["video"]:
    current_rtsp_urls = config["video"]["sources"] + [""] * (3 - len(config["video"]["sources"]))
    current_rtsp_urls = current_rtsp_urls[:3]
else:
    current_rtsp_urls = ["", "", ""]
caps = [None, None, None]
video_locks = [threading.Lock(), threading.Lock(), threading.Lock()]
latest_frames = [None, None, None]
video_thread_running = False


# --- HELPER FUNCTIONS ---
def axis_to_pedal(raw, axis_min, axis_max, deadzone):
    if abs(axis_min - axis_max) < 0.001:
        return 0.0
    norm = (raw - axis_max) / (axis_min - axis_max)
    norm = max(0.0, min(1.0, norm))
    if norm < deadzone:
        return 0.0
    return (norm - deadzone) / (1.0 - deadzone)


def axis_to_steering(raw, cal):
    sr = cal.get("steering_range", 1.0)
    if abs(sr) < 0.001:
        sr = 1.0
    centered = raw - cal.get("steering_center", 0.0)
    norm = max(-1.0, min(1.0, centered / sr))
    dz = cal.get("deadzone_steering", 0.02)
    if abs(norm) < dz:
        norm = 0.0
    angle = norm * cal.get("steering_max_angle", 90.0)
    if abs(angle) < 1.0:
        direction = "CENTER"
    elif angle > 0:
        direction = "RIGHT"
    else:
        direction = "LEFT"
    return angle, direction

# --- DASHBOARD SERVER ---
import http.server
import socketserver

def start_dashboard_server():
    PORT = 8889
    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, format, *args):
            pass # Mute HTTP logs
        
        def do_GET(self):
            if self.path == '/':
                self.path = '/steering_dashboard.html'
            elif self.path == '/control':
                self.path = '/control.html'
            return super().do_GET()
    try:
        # Avoid 'Address already in use'
        socketserver.TCPServer.allow_reuse_address = True
        with socketserver.TCPServer(("", PORT), Handler) as httpd:
            httpd.serve_forever()
    except Exception as e:
        print(f"[DASHBOARD] Could not start server on port {PORT}: {e}")

# --- CONTROLLER THREAD ---
def controller_thread():
    """Runs in background thread. Reads joystick input via pygame with hotplug support and low system jitter."""
    global states

    print("[CTRL] Thread started, initializing pygame...")
    pygame.init()
    pygame.joystick.init()
    print(f"[CTRL] Pygame initialized.")

    joysticks = {}
    btn_7_press_times = {}
    btn_3_prev = {}
    btn_4_prev = {}
    btn_5_prev = {}
    
    last_count = -1
    last_count_check_time = 0.0

    while True:
        try:
            pygame.event.pump()
            
            # Query OS for physical joystick count at 1Hz instead of 100Hz to eliminate system call latency jitter
            current_time = time.time()
            if current_time - last_count_check_time > 1.0:
                last_count_check_time = current_time
                count = pygame.joystick.get_count()
            else:
                count = last_count
        except Exception as e:
            count = -1

        # Re-initialize joysticks ONLY when the physical count changes or on startup/error
        if count != last_count or count == -1:
            try:
                pygame.joystick.quit()
                pygame.joystick.init()
                
                real_count = pygame.joystick.get_count()
                new_joysticks = {}
                for idx in range(real_count):
                    try:
                        joy = pygame.joystick.Joystick(idx)
                        joy.init()
                        new_joysticks[idx] = joy
                        if idx not in joysticks:
                            print(f"[CTRL] Joystick {idx} connected: {joy.get_name()}")
                            if idx < len(states):
                                states[idx].connected = True
                                btn_7_press_times[idx] = 0.0
                    except Exception:
                        pass
                
                # Detect disconnected ones
                for idx in list(joysticks.keys()):
                    if idx not in new_joysticks:
                        print(f"[CTRL] Joystick {idx} disconnected.")
                        if idx < len(states):
                            states[idx].connected = False
                            
                joysticks = new_joysticks
                last_count = len(joysticks)
            except Exception as e:
                print(f"[CTRL] Reconnect error: {e}")
                last_count = -1

        count = len(joysticks)

        # Update states
        for i in range(min(count, 3)):  # Max 3 steerings supported
            if states[i].web_controlled: continue
            
            joy = joysticks.get(i)
            if not joy: continue
            
            st = states[i]
            st.connected = True

            ctrl = config["controller"]
            cal = config["calibration"]

            try:
                # Steering
                ax_steer = ctrl.get("axis_steering", 0)
                if joy.get_numaxes() > ax_steer:
                    raw = joy.get_axis(ax_steer)
                    st.steering_raw = raw
                    st.steering_angle, st.steering_direction = axis_to_steering(raw, cal)

                # Throttle
                ax_thr = ctrl.get("axis_throttle", 5)
                if joy.get_numaxes() > ax_thr:
                    raw_t = joy.get_axis(ax_thr)
                    st.throttle = axis_to_pedal(
                        raw_t, cal["throttle_min"], cal["throttle_max"], cal["deadzone_pedal"]
                    )

                # Brake
                ax_brk = ctrl.get("axis_brake", 4)
                if joy.get_numaxes() > ax_brk:
                    raw_b = joy.get_axis(ax_brk)
                    st.brake = axis_to_pedal(
                        raw_b, cal["brake_min"], cal["brake_max"], cal["deadzone_pedal"]
                    )

                # Buttons
                buttons = {}
                for b in range(joy.get_numbuttons()):
                    val = joy.get_button(b)
                    buttons[f"btn_{b}"] = val
                st.buttons = buttons
                st.timestamp = time.time()

                # Get mapped buttons from config
                btn_horn_id = ctrl.get("btn_horn", 0)
                btn_hl_id = ctrl.get("btn_headlight", 3)
                btn_sd_id = ctrl.get("btn_speed_down", 4)
                btn_su_id = ctrl.get("btn_speed_up", 5)
                btn_sel_id = ctrl.get("btn_selection", 7)

                # Speed Limit Change Logic
                btn_4_val = buttons.get(f"btn_{btn_sd_id}", 0)
                btn_5_val = buttons.get(f"btn_{btn_su_id}", 0)
                
                if btn_4_val == 1 and btn_4_prev.get(i, 0) == 0:
                    st.speed_limit = max(1, st.speed_limit - 1)
                    print(f"[SPEED] Steer {i+1} speed limit decreased to {st.speed_limit}")
                    if "speed_limits" not in config["controller"]:
                        config["controller"]["speed_limits"] = [5, 5, 5]
                    config["controller"]["speed_limits"][i] = st.speed_limit
                    save_config(config)
                if btn_5_val == 1 and btn_5_prev.get(i, 0) == 0:
                    st.speed_limit = min(5, st.speed_limit + 1)
                    print(f"[SPEED] Steer {i+1} speed limit increased to {st.speed_limit}")
                    if "speed_limits" not in config["controller"]:
                        config["controller"]["speed_limits"] = [5, 5, 5]
                    config["controller"]["speed_limits"][i] = st.speed_limit
                    save_config(config)
                
                
                # Headlight toggle logic
                btn_3_val = buttons.get(f"btn_{btn_hl_id}", 0)
                if btn_3_val == 1 and btn_3_prev.get(i, 0) == 0:
                    st.headlight = not st.headlight
                    print(f"[LIGHT] Steer {i+1} headlight toggled to {st.headlight}")
                btn_3_prev[i] = btn_3_val

                btn_4_prev[i] = btn_4_val
                btn_5_prev[i] = btn_5_val

                # Multi RC Car Channel Switch Logic (Hold button for 3s to trigger selection overlay)
                if buttons.get(f"btn_{btn_sel_id}", 0) == 1:
                    if btn_7_press_times.get(i, 0.0) == 0.0:
                        btn_7_press_times[i] = time.time()
                    elif btn_7_press_times[i] < time.time() and time.time() - btn_7_press_times[i] >= 3.0:
                        print(f"[SELECTION] Steer {i+1} triggered character selection screen")
                        st.trigger_selection = True
                        btn_7_press_times[i] = time.time() + 99999.0
                else:
                    btn_7_press_times[i] = 0.0

            except Exception as e:
                print(f"[CTRL] Error reading joystick {i}: {e}. Forcing re-init.")
                last_count = -1
                pass

        # Disconnect handling for unplugged ones
        for i in range(count, len(states)):
            if not states[i].web_controlled:
                states[i].connected = False

        # Web control safety timeout (revert if no command received for 1 second)
        current_time_for_timeout = time.time()
        for i in range(len(states)):
            if states[i].web_controlled and (current_time_for_timeout - states[i].timestamp) > 1.0:
                states[i].web_controlled = False
                states[i].connected = False
                states[i].throttle = 0.0
                states[i].brake = 0.0
                states[i].steering_angle = 0.0
                states[i].steering_direction = "CENTER"

        time.sleep(0.01)


# --- WEBSOCKET HANDLER ---
async def ws_handler(ws):
    clients.add(ws)
    addr = ws.remote_address
    print(f"[WS] Client connected: {addr} (total: {len(clients)})")
    
    # Disable Nagle's algorithm (TCP_NODELAY) and optimize buffer size for zero lag
    try:
        sock = ws.transport.get_extra_info('socket')
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            print(f"[WS] Optimized TCP socket (TCP_NODELAY) for {addr}")
    except Exception as e:
        print(f"[WS] Failed to set socket options: {e}")

    try:
        async for message in ws:
            try:
                cmd = json.loads(message)
                msg_type = cmd.get("type", "")
                if msg_type == "ping":
                    await ws.send(json.dumps({"type": "pong"}))
                elif msg_type == "telemetry":
                    # Broadcast telemetry to all connected clients except the sender
                    payload = json.dumps(cmd)
                    for ws_client in list(clients):
                        if ws_client != ws:
                            try:
                                await ws_client.send(payload)
                            except Exception:
                                pass
                elif msg_type == "get_config":
                    await ws.send(json.dumps({"type": "config", "data": config}))
                elif msg_type == "update_config":
                    section = cmd.get("section")
                    key = cmd.get("key")
                    value = cmd.get("value")
                    if section in config and key in config[section]:
                        config[section][key] = value
                        save_config(config)
                        await ws.send(json.dumps({"type": "config_updated", "ok": True}))
                elif msg_type == "set_speed_limit":
                    steer_index = cmd.get("steer_index", 0)
                    value = cmd.get("value", 5)
                    if 0 <= steer_index < len(states):
                        states[steer_index].speed_limit = max(1, min(5, int(value)))
                        print(f"[WS] Speed limit for Steer {steer_index+1} set to {states[steer_index].speed_limit}")
                        if "speed_limits" not in config["controller"]:
                            config["controller"]["speed_limits"] = [5, 5, 5]
                        config["controller"]["speed_limits"][steer_index] = states[steer_index].speed_limit
                        save_config(config)
                        # Broadcast immediately to all connected clients to sync dashboard instantly
                        payload = json.dumps({
                            "type": "controller_data",
                            "steer_index": steer_index,
                            "data": {
                                "steering_angle": round(states[steer_index].steering_angle, 2),
                                "steering_direction": states[steer_index].steering_direction,
                                "steering_raw": round(states[steer_index].steering_raw, 4),
                                "throttle": round(states[steer_index].throttle, 3),
                                "brake": round(states[steer_index].brake, 3),
                                "forward": states[steer_index].throttle > 0.1,
                                "reverse": states[steer_index].brake > 0.1,
                                "left": states[steer_index].steering_direction == "LEFT",
                                "right": states[steer_index].steering_direction == "RIGHT",
                                "horn": states[steer_index].buttons.get(f"btn_{config['controller'].get('btn_horn', 0)}", 0) == 1 if states[steer_index].buttons else False,
                                "headlight": states[steer_index].headlight,
                                "buttons": states[steer_index].buttons,
                                "connected": states[steer_index].connected,
                                "timestamp": states[steer_index].timestamp,
                                "active_car_id": states[steer_index].active_car_id,
                                "speed_limit": states[steer_index].speed_limit,
                                "occupied_cars": [s.active_car_id for idx, s in enumerate(states) if idx != steer_index and s.connected],
                                "trigger_selection": False,
                            },
                        })
                        for ws_client in list(clients):
                            try:
                                await ws_client.send(payload)
                            except Exception:
                                pass
                elif msg_type == "set_game_state":
                    steer_index = cmd.get("steer_index", 0)
                    active = cmd.get("active", False)
                    if 0 <= steer_index < len(states):
                        states[steer_index].game_active = active
                        print(f"[GAME] Steer {steer_index+1} game_active set to {active}")
                elif msg_type == "web_control":
                    steer_index = cmd.get("steer_index", 0)
                    if 0 <= steer_index < len(states):
                        st = states[steer_index]
                        st.web_controlled = True
                        st.connected = True
                        st.steering_angle = float(cmd.get("steering_angle", 0.0))
                        st.steering_direction = cmd.get("steering_direction", "CENTER")
                        st.steering_raw = float(cmd.get("steering_raw", 0.0))
                        
                        raw_throttle = float(cmd.get("throttle", 0.0))
                        st.throttle = min(raw_throttle, st.speed_limit * 0.2)
                        st.brake = float(cmd.get("brake", 0.0))
                        st.timestamp = time.time()
                elif msg_type == "select_car":
                    steer_index = cmd.get("steer_index", 0)
                    car_id = cmd.get("car_id", 1)
                    if 0 <= steer_index < len(states):
                        states[steer_index].active_car_id = car_id
                        print(f"[SELECTION] Steer {steer_index+1} selected RC Car {car_id}")
                        if "active_car_ids" not in config["controller"]:
                            config["controller"]["active_car_ids"] = [1, 2, 3]
                        config["controller"]["active_car_ids"][steer_index] = car_id
                        save_config(config)
            except Exception:
                pass
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        clients.discard(ws)
        print(f"[WS] Client disconnected: {addr} (remaining: {len(clients)})")


async def broadcast_loop():
    hz = config["websocket"].get("broadcast_hz", 60)
    interval = 1.0 / hz
    while True:
        if clients:
            for i, st in enumerate(states):
                if not st.connected: continue
                # Simple logic for digital commands
                is_forward = st.throttle > 0.1
                is_reverse = st.brake > 0.1
                is_left = st.steering_direction == "LEFT"
                is_right = st.steering_direction == "RIGHT"
                is_horn = st.buttons.get(f"btn_{config['controller'].get('btn_horn', 0)}", 0) == 1
                
                trigger_sel = st.trigger_selection
                if trigger_sel:
                    st.trigger_selection = False

                payload = json.dumps({
                    "type": "controller_data",
                    "steer_index": i,
                    "data": {
                        "steering_angle": round(st.steering_angle, 2),
                        "steering_direction": st.steering_direction,
                        "steering_raw": round(st.steering_raw, 4),
                        "throttle": round(st.throttle, 3),
                        "brake": round(st.brake, 3),
                        "forward": is_forward,
                        "reverse": is_reverse,
                        "left": is_left,
                        "right": is_right,
                        "horn": is_horn,
                        "headlight": st.headlight,
                        "buttons": st.buttons,
                        "connected": st.connected,
                        "timestamp": st.timestamp,
                        "active_car_id": st.active_car_id,
                        "speed_limit": st.speed_limit,
                        "occupied_cars": [s.active_car_id for idx, s in enumerate(states) if idx != i and s.connected],
                        "trigger_selection": trigger_sel,
                        "game_active": st.game_active,
                    },
                })
                dead = set()
                for ws in list(clients):
                    try:
                        await ws.send(payload)
                    except Exception:
                        dead.add(ws)
                for ws in dead:
                    clients.discard(ws)
        await asyncio.sleep(interval)


def video_grabber_thread(steer_index):
    """Runs in background to continuously grab frames from cv2.VideoCapture,
    preventing queue buildup and latency accumulation."""
    global caps, latest_frames, video_thread_running, current_rtsp_urls
    print(f"[VIDEO] Grabber thread started for Steer {steer_index + 1}.")
    while video_thread_running:
        cap = caps[steer_index]
        if cap is not None:
            with video_locks[steer_index]:
                if cap.isOpened():
                    try:
                        ok, frame = cap.read()
                        if ok:
                            latest_frames[steer_index] = frame
                        else:
                            print(f"[VIDEO] Steer {steer_index + 1} connection lost, reconnecting...")
                            cap.release()
                            url = current_rtsp_urls[steer_index]
                            source = int(url) if url.isdigit() else url
                            if not isinstance(source, int) and str(source).startswith("rtsp"):
                                os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp"
                            caps[steer_index] = cv2.VideoCapture(source)
                            caps[steer_index].set(cv2.CAP_PROP_BUFFERSIZE, 1)
                    except Exception as e:
                        print(f"[VIDEO] Steer {steer_index + 1} grabber error: {e}")
                        time.sleep(1.0)
        time.sleep(0.005)


def gen_frames(steer_index):
    global latest_frames
    while True:
        frame = None
        with video_locks[steer_index]:
            if latest_frames[steer_index] is not None:
                frame = latest_frames[steer_index].copy()
        
        if frame is not None:
            _, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 80])
            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n" + buf.tobytes() + b"\r\n"
            )
        time.sleep(0.015) # cap output feed to smooth ~60fps


@video_app.route("/video_feed/<int:steer_index>")
def video_feed(steer_index):
    if steer_index < 0 or steer_index >= 3:
        return "Invalid steer index", 400
    return Response(gen_frames(steer_index), mimetype="multipart/x-mixed-replace; boundary=frame")


@video_app.route("/set_rtsp", methods=["POST", "OPTIONS"])
def set_rtsp():
    if request.method == "OPTIONS":
        return "", 204
    global current_rtsp_urls, caps
    req = request.json
    steer_index = int(req.get("steer_index", 0))
    url = req.get("url", "")
    
    if steer_index < 0 or steer_index >= 3:
        return {"error": "Invalid steer index"}, 400
        
    with video_locks[steer_index]:
        if url != current_rtsp_urls[steer_index]:
            current_rtsp_urls[steer_index] = url
            if "video" not in config:
                config["video"] = {"sources": ["", "", ""]}
            if "sources" not in config["video"]:
                config["video"]["sources"] = ["", "", ""]
            while len(config["video"]["sources"]) < 3:
                config["video"]["sources"].append("")
            config["video"]["sources"][steer_index] = url
            save_config(config)
            if caps[steer_index]:
                caps[steer_index].release()
            
            if url:
                source = int(url) if url.isdigit() else url
                if not url.isdigit() and url.startswith("rtsp"):
                    os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp"
                caps[steer_index] = cv2.VideoCapture(source)
                if caps[steer_index].isOpened():
                    caps[steer_index].set(cv2.CAP_PROP_BUFFERSIZE, 1)
                print(f"[VIDEO] Steer {steer_index + 1} source set to: {source}")
            else:
                caps[steer_index] = None
                print(f"[VIDEO] Steer {steer_index + 1} source cleared.")
    return {"status": "ok", "steer_index": steer_index, "url": current_rtsp_urls[steer_index]}


# --- PORT KILLER ---
def kill_port(port):
    """Kill any process using the given port on Windows."""
    try:
        import subprocess
        result = subprocess.run(
            ["netstat", "-ano"],
            capture_output=True,
            text=True,
        )
        for line in result.stdout.splitlines():
            if f":{port}" in line and "LISTENING" in line:
                parts = line.split()
                pid = int(parts[-1])
                if pid > 0:
                    print(f"[PORT] Killing PID {pid} on port {port}...")
                    subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                                   capture_output=True)
                    time.sleep(1)
    except Exception as e:
        print(f"[PORT] Could not auto-kill port {port}: {e}")


# --- MAIN ---
async def main():
    global video_thread_running
    ws_port = config["websocket"]["port"]

    # Windows system timer precision enhancement (makes asyncio.sleep extremely precise, preventing lag/stuttering)
    if sys.platform == 'win32':
        try:
            import ctypes
            ctypes.windll.winmm.timeBeginPeriod(1)
            print("[OS] High-precision Windows system timer activated (1ms)")
        except Exception as e:
            print(f"[OS] Could not set Windows timer resolution: {e}")

    # Kill anything on our ports first
    kill_port(ws_port)
    kill_port(5000)
    kill_port(8889)

    # Start controller thread
    t = threading.Thread(target=controller_thread, daemon=True)
    t.start()

    # Start zero-latency video grabber threads for all 3 steers
    video_thread_running = True
    for i in range(3):
        if current_rtsp_urls[i]:
            url = current_rtsp_urls[i]
            source = int(url) if url.isdigit() else url
            if not isinstance(source, int) and str(source).startswith("rtsp"):
                os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;tcp"
            caps[i] = cv2.VideoCapture(source)
            if caps[i].isOpened():
                caps[i].set(cv2.CAP_PROP_BUFFERSIZE, 1)
                
    for i in range(3):
        threading.Thread(target=video_grabber_thread, args=(i,), daemon=True).start()

    # Start Flask video bridge
    threading.Thread(
        target=lambda: video_app.run(
            host="0.0.0.0", port=5000, threaded=True, use_reloader=False
        ),
        daemon=True,
    ).start()

    # Start Dashboard Server
    threading.Thread(target=start_dashboard_server, daemon=True).start()

    print(f"\n{'='*50}")
    print(f"  UNIFIED SERVER")
    print(f"{'='*50}")
    print(f"  WebSocket : ws://localhost:{ws_port}")
    print(f"  Video     : http://localhost:5000")
    print(f"  Dashboard : http://localhost:8889")
    print(f"{'='*50}\n")

    # Start WebSocket server with retry
    for attempt in range(5):
        try:
            async with websockets.serve(
                ws_handler, 
                "0.0.0.0", 
                ws_port,
                process_request=None,
                ping_interval=5,
                ping_timeout=2
            ) as server:
                print(f"[WS] Server listening on port {ws_port} (100Hz)")
                await broadcast_loop()
        except OSError as e:
            if e.errno == 10048:
                print(f"[WS] Port {ws_port} busy, retrying in 3s... (attempt {attempt+1}/5)")
                kill_port(ws_port)
                await asyncio.sleep(3)
            else:
                raise
    print("[ERR] Could not bind WebSocket after 5 attempts.")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[EXIT] Server stopped.")
    except Exception as e:
        print(f"\n[ERR] {e}")
    finally:
        video_thread_running = False
        if sys.platform == 'win32':
            try:
                import ctypes
                ctypes.windll.winmm.timeEndPeriod(1)
                print("[OS] Windows system timer released")
            except Exception:
                pass
