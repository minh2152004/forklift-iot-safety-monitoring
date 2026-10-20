from flask import Flask, request, jsonify, Response
import numpy as np
import cv2
from ultralytics import YOLO
import paho.mqtt.client as mqtt
import ssl
import time
import os
import logging
import base64
import queue
import threading
import json

# ---------------- LOGGING ----------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)
logger = logging.getLogger(__name__)

# ---------------- CONFIG ----------------

MQTT_BROKER  = os.environ.get("MQTT_BROKER", "")
MQTT_PORT    = int(os.environ.get("MQTT_PORT", "8883"))
MQTT_USER    = os.environ.get("MQTT_USER", "")
MQTT_PASS    = os.environ.get("MQTT_PASS", "")
MQTT_TOPIC   = os.environ.get("MQTT_TOPIC", "forklift/alert")

API_KEY      = os.environ.get("API_KEY", "")

CONF_THRESHOLD = 0.4
DIST_THRESHOLD = 5.0

FOCAL_LENGTH = 388
REAL_HEIGHT  = 1.7

MAX_IMAGE_SIZE = 5 * 1024 * 1024

# ---------------- VALIDATION ----------------

if not all([MQTT_BROKER, MQTT_USER, MQTT_PASS, API_KEY]):
    logger.error("Missing required environment variables")
    logger.error("Required: MQTT_BROKER, MQTT_USER, MQTT_PASS, API_KEY")
    exit(1)

# ---------------- MQTT ----------------

mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
mqtt_client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()

# ---------------- SSE SUBSCRIBERS ----------------

subscribers = []
subscribers_lock = threading.Lock()


def push_to_subscribers(data):
    with subscribers_lock:
        dead = []
        for q in subscribers:
            try:
                q.put_nowait(data)
            except queue.Full:
                dead.append(q)
        for q in dead:
            subscribers.remove(q)


# ---------------- APP ----------------

app = Flask(__name__)
model = YOLO("yolov8n.pt")
last_alert_time = 0


def verify_api_key():
    key = request.headers.get("X-API-Key", "")
    return key == API_KEY


@app.route('/health', methods=['GET'])
def health():
    return jsonify({"status": "ok"}), 200


@app.route('/stream', methods=['GET'])
def stream():
    q = queue.Queue(maxsize=2)
    with subscribers_lock:
        subscribers.append(q)

    def generate():
        try:
            while True:
                try:
                    data = q.get(timeout=30)
                    yield f"data: {data}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"
        except GeneratorExit:
            with subscribers_lock:
                if q in subscribers:
                    subscribers.remove(q)

    return Response(
        generate(),
        mimetype='text/event-stream',
        headers={
            'Cache-Control': 'no-cache',
            'X-Accel-Buffering': 'no',
        }
    )


@app.route('/upload', methods=['POST'])
def upload():
    global last_alert_time

    if not verify_api_key():
        logger.warning(f"Unauthorized request from {request.remote_addr}")
        return jsonify({"error": "unauthorized"}), 401

    if request.content_length is None or request.content_length > MAX_IMAGE_SIZE:
        return jsonify({"error": "payload too large"}), 413

    if not request.data:
        return jsonify({"error": "empty payload"}), 400

    file_bytes = np.frombuffer(request.data, np.uint8)
    frame = cv2.imdecode(file_bytes, cv2.IMREAD_COLOR)

    if frame is None:
        return jsonify({"error": "invalid image"}), 400

    results = model(frame, conf=CONF_THRESHOLD)
    alert_triggered = False
    annotated = frame.copy()

    for r in results:
        if r.boxes is None:
            continue
        for box in r.boxes:
            cls = int(box.cls[0])
            if cls == 0:
                x1, y1, x2, y2 = map(int, box.xyxy[0])
                h = y2 - y1
                if h > 0:
                    distance = (FOCAL_LENGTH * REAL_HEIGHT) / h
                    logger.info(f"Person detected at {distance:.2f}m")
                    in_danger = distance < DIST_THRESHOLD
                    if in_danger:
                        alert_triggered = True
                    color = (0, 0, 255) if in_danger else (0, 255, 0)
                    cv2.rectangle(annotated, (x1, y1), (x2, y2), color, 2)
                    label = f"Person {distance:.1f}m"
                    cv2.putText(annotated, label, (x1, y1 - 8),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2)

    if alert_triggered:
        cv2.rectangle(annotated, (0, 0), (annotated.shape[1], 36), (0, 0, 200), -1)
        cv2.putText(annotated, "DANGER ZONE", (10, 26),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.85, (255, 255, 255), 2)

    success, jpeg_buf = cv2.imencode('.jpg', annotated, [cv2.IMWRITE_JPEG_QUALITY, 80])
    if success:
        b64 = base64.b64encode(jpeg_buf.tobytes()).decode('utf-8')
        timestamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        payload = json.dumps({
            "img":   b64,
            "alert": alert_triggered,
            "ts":    timestamp
        })
        push_to_subscribers(payload)
        logger.info(f"Frame pushed to {len(subscribers)} subscriber(s)")

    if alert_triggered:
        if time.time() - last_alert_time > 1:
            mqtt_client.publish(MQTT_TOPIC, "DANGER")
            logger.warning("ALERT SENT - person in danger zone")
            last_alert_time = time.time()

    return jsonify({"alert": alert_triggered}), 200


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5001, threaded=True)