import cv2
from flask import Flask, Response, request
import threading
import time

app = Flask(__name__)

# Global variable to store current RTSP URL and the capture object
current_rtsp_url = ""
cap = None
lock = threading.Lock()

def get_frames():
    global cap
    while True:
        with lock:
            if cap is None or not cap.isOpened():
                time.sleep(0.1)
                continue
            
            success, frame = cap.read()
            if not success:
                # Reconnect logic if stream drops
                print(f"[VIDEO] Stream dropped, reconnecting to {current_rtsp_url}...")
                cap.release()
                cap = cv2.VideoCapture(current_rtsp_url)
                time.sleep(1)
                continue
            
            # Encode frame as JPEG
            ret, buffer = cv2.imencode('.jpg', frame)
            frame_bytes = buffer.tobytes()
            
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')

@app.route('/video_feed')
def video_feed():
    return Response(get_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

@app.route('/set_rtsp', methods=['POST'])
def set_rtsp():
    global current_rtsp_url, cap
    data = request.json
    url = data.get('url', '')
    
    with lock:
        if url != current_rtsp_url:
            print(f"[VIDEO] Switching to RTSP: {url}")
            current_rtsp_url = url
            if cap:
                cap.release()
            cap = cv2.VideoCapture(current_rtsp_url)
            
    return {"status": "ok", "url": current_rtsp_url}

if __name__ == '__main__':
    print("\n[INFO] Video Bridge Server (RTSP to HTTP)")
    print("       API: http://localhost:5000/set_rtsp")
    print("       Stream: http://localhost:5000/video_feed")
    app.run(host='0.0.0.0', port=5000, threaded=True)
