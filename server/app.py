from datetime import datetime
import os
import socket
import threading
import time
import uuid

import cv2
from flask import Flask, jsonify, render_template
from flask_cors import CORS
from huggingface_hub import hf_hub_download
import mysql.connector
import numpy as np
import paho.mqtt.client as mqtt
from paho.mqtt.client import CallbackAPIVersion
import requests
from ultralytics import YOLO

app = Flask(__name__)
CORS(app)

# Otomatis unduh (tersimpan di cache lokal) dan muat best.pt dari Hugging Face
ckpt = hf_hub_download(repo_id="rabahdev/fire-smoke-yolov8n", filename="best.pt")
model = YOLO(ckpt)
print("Daftar kelas model:", model.names)

# =========================================================================
# KONFIGURASI HTTP ESP32-CAM & MQTT HIVEMQ (TANPA DHT & TANPA LED RGB)
# =========================================================================
ESP_CAM_URL = "http://10.178.52.68/capture"
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 1883
MQTT_TOPIC_FLAME = "infernoguard/flame"
MQTT_TOPIC_STATUS = "infernoguard/alert_status"
MQTT_TOPIC_BUZZER = "infernoguard/buzzer"

# Resolusi inferensi dinaikkan agar api kecil lebih terlihat
INFER_IMG_SIZE = 800
CONF_THRESHOLD = 0.01

CAPTURE_DURATION = 5.0
COOLDOWN_DURATION = 10.0
STATUS_LOG_INTERVAL = 10.0
BUZZER_DURATION = 10.0

CAPTURE_DIR = os.path.join("static", "captures")
os.makedirs(CAPTURE_DIR, exist_ok=True)
LATEST_PREVIEW_PATH = os.path.join("static", "latest_capture.jpg")

DB_CONFIG = {
    "host": "localhost",
    "port": 3307,
    "user": "root",
    "password": "kzoinucd",
    "database": "api_robot",
}

system_state = {
    "flame_detected": False,
    "alert_level": "AMAN",  # AMAN | BAHAYA
    "last_flame_update": "-",
    "status": "STANDBY",  # STANDBY | CAPTURING | COOLDOWN
    "cooldown_remaining": 0,
    "capture_remaining": 0,
    "latest_image": (
        "/static/latest_capture.jpg"
        if os.path.exists(LATEST_PREVIEW_PATH)
        else None
    ),
    "latest_capture_time": "-",
    "latest_result": "Belum ada tangkapan",
    "latest_fire_pct": 0.0,
    "status_logs_10s": [],
}
state_lock = threading.Lock()
mqtt_lock = threading.Lock()

client_id = f"infernoguard-backend-{uuid.uuid4().hex[:8]}"
mqtt_client = mqtt.Client(
    callback_api_version=CallbackAPIVersion.VERSION1,
    client_id=client_id,
    clean_session=True,
)
mqtt_connected = False


def force_ipv4():
  orig_getaddrinfo = socket.getaddrinfo

  def ipv4_only(*args, **kwargs):
    res = orig_getaddrinfo(*args, **kwargs)
    v4 = [r for r in res if r[0] == socket.AF_INET]
    return v4 if v4 else res

  socket.getaddrinfo = ipv4_only


force_ipv4()


def compute_alert_level(flame_detected):
  return "BAHAYA" if flame_detected else "AMAN"


def is_fire_class(cls_id):
  cls_name = str(model.names.get(cls_id, "")).lower()
  if "smoke" in cls_name or "asap" in cls_name:
    return False
  return "fire" in cls_name or "api" in cls_name


def draw_fire_only_boxes(frame, fire_boxes):
  annotated = frame.copy()
  for x1, y1, x2, y2, label, conf_val in fire_boxes:
    p1 = (int(round(x1)), int(round(y1)))
    p2 = (int(round(x2)), int(round(y2)))
    cv2.rectangle(annotated, p1, p2, (0, 0, 255), 2)
    text = f"{label} {conf_val:.2f}"
    text_y = max(18, p1[1] - 8)
    cv2.putText(
        annotated,
        text,
        (p1[0], text_y),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.6,
        (0, 0, 255),
        2,
    )
  return annotated


def get_db_connection():
  return mysql.connector.connect(**DB_CONFIG)


def save_detection_log(image_frame, fire_percentage):
  try:
    now = datetime.now()
    tanggal = now.strftime("%Y-%m-%d")
    waktu = now.strftime("%H:%M:%S")
    filename = f"fire_{now.strftime('%Y%m%d_%H%M%S_%f')[:-3]}.jpg"
    filepath = os.path.join(CAPTURE_DIR, filename)
    web_image_path = f"/static/captures/{filename}"

    cv2.imwrite(filepath, image_frame)

    conn = get_db_connection()
    cursor = conn.cursor()
    query = (
        "INSERT INTO fire_history (tanggal, waktu, gambar_path, besaran_api) "
        "VALUES (%s, %s, %s, %s)"
    )
    cursor.execute(
        query, (tanggal, waktu, web_image_path, round(fire_percentage, 2))
    )
    conn.commit()
    cursor.close()
    conn.close()
    print(f"[LOG DB] BAHAYA! Tersimpan: {filename}")
  except Exception as e:
    print(f"[ERROR DB] Gagal simpan ke database: {e}")


def trigger_buzzer_10s():
  try:
    with mqtt_lock:
      if not mqtt_connected:
        print("[BUZZER WARN] MQTT belum terhubung saat mengirim perintah buzzer.")
      info = mqtt_client.publish(
          MQTT_TOPIC_BUZZER, "ON_10S", qos=0, retain=False
      )
      info.wait_for_publish(timeout=2.0)
    print(
        f"[BUZZER] Kamera mendeteksi api! Perintah buzzer {BUZZER_DURATION}s"
        " dikirim."
    )
  except Exception as e:
    print(f"[BUZZER ERROR] Gagal mengirim perintah buzzer: {e}")


def trigger_capture_and_verify():
  try:
    with state_lock:
      system_state["status"] = "CAPTURING"
      system_state["capture_remaining"] = int(CAPTURE_DURATION)

    print(
        f"[TRIGGER] BAHAYA (Api Terdeteksi)! Capture {CAPTURE_DURATION}s"
        f" (imgsz={INFER_IMG_SIZE}, fire-only)..."
    )
    start_time = time.time()
    fire_detected_in_cycle = False
    best_fire_frame = None
    best_fire_pct = 0.0
    last_valid_frame = None

    while (time.time() - start_time) < CAPTURE_DURATION:
      remaining = max(0, int(CAPTURE_DURATION - (time.time() - start_time)))
      with state_lock:
        system_state["capture_remaining"] = remaining

      try:
        response = requests.get(ESP_CAM_URL, timeout=(2.5, 4.0))
        if response.status_code == 200:
          img_array = np.array(bytearray(response.content), dtype=np.uint8)
          frame = cv2.imdecode(img_array, cv2.IMREAD_COLOR)

          if frame is not None:
            results = model(
                frame, imgsz=INFER_IMG_SIZE, conf=CONF_THRESHOLD, verbose=False
            )
            frame_h, frame_w, _ = frame.shape
            total_area = frame_h * frame_w

            fire_boxes_in_frame = []
            frame_fire_pct = 0.0

            for r in results:
              for box in r.boxes:
                cls_id = int(box.cls[0])
                if not is_fire_class(cls_id):
                  continue

                conf_val = float(box.conf[0])
                cls_name = str(model.names.get(cls_id, "fire"))
                x1, y1, x2, y2 = box.xyxy[0].tolist()
                box_area = max(0.0, (x2 - x1)) * max(0.0, (y2 - y1))
                fire_pct = (box_area / total_area) * 100.0

                fire_boxes_in_frame.append((x1, y1, x2, y2, cls_name, conf_val))
                if fire_pct > frame_fire_pct:
                  frame_fire_pct = fire_pct

            clean_or_fire_frame = draw_fire_only_boxes(frame, fire_boxes_in_frame)
            last_valid_frame = clean_or_fire_frame.copy()

            if fire_boxes_in_frame:
              fire_detected_in_cycle = True
              if frame_fire_pct >= best_fire_pct:
                best_fire_pct = frame_fire_pct
                best_fire_frame = clean_or_fire_frame.copy()
      except Exception as req_err:
        print(f"[WARN HTTP] {req_err}")

      time.sleep(0.25)

    now_time_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    cache_buster = int(time.time())

    if fire_detected_in_cycle and best_fire_frame is not None:
      cv2.imwrite(LATEST_PREVIEW_PATH, best_fire_frame)
      with state_lock:
        system_state["latest_image"] = (
            f"/static/latest_capture.jpg?t={cache_buster}"
        )
        system_state["latest_capture_time"] = now_time_str
        system_state["latest_result"] = "API TERDETEKSI"
        system_state["latest_fire_pct"] = round(best_fire_pct, 2)
      threading.Thread(target=trigger_buzzer_10s, daemon=True).start()
      save_detection_log(best_fire_frame, best_fire_pct)
    elif last_valid_frame is not None:
      cv2.imwrite(LATEST_PREVIEW_PATH, last_valid_frame)
      with state_lock:
        system_state["latest_image"] = (
            f"/static/latest_capture.jpg?t={cache_buster}"
        )
        system_state["latest_capture_time"] = now_time_str
        system_state["latest_result"] = "AMAN (BUKAN API)"
        system_state["latest_fire_pct"] = 0.0

  except Exception as e:
    print(f"[ERROR SIKLUS] {e}")

  finally:
    cooldown_start = time.time()
    while (time.time() - cooldown_start) < COOLDOWN_DURATION:
      rem = max(0, int(COOLDOWN_DURATION - (time.time() - cooldown_start)))
      with state_lock:
        system_state["status"] = "COOLDOWN"
        system_state["cooldown_remaining"] = rem
      time.sleep(0.5)

    with state_lock:
      system_state["status"] = "STANDBY"
      system_state["cooldown_remaining"] = 0
      system_state["capture_remaining"] = 0


def status_10s_logger():
  # Mencatat snapshot status sistem setiap 10 detik untuk dashboard
  while True:
    time.sleep(STATUS_LOG_INTERVAL)
    now_str = datetime.now().strftime("%H:%M:%S")
    with state_lock:
      flame = system_state["flame_detected"]
      level = compute_alert_level(flame)
      system_state["alert_level"] = level

      entry = {
          "waktu": now_str,
          "api": "TERDETEKSI" if flame else "TIDAK",
          "level": level,
          "mode": system_state["status"],
      }
      system_state["status_logs_10s"].insert(0, entry)
      system_state["status_logs_10s"] = system_state["status_logs_10s"][:20]


def on_connect(client, userdata, flags, rc):
  global mqtt_connected
  if rc == 0:
    mqtt_connected = True
    print(
        f"[MQTT] Terhubung ke {MQTT_BROKER}:{MQTT_PORT} (id={client_id})"
    )
    client.subscribe(MQTT_TOPIC_FLAME, qos=0)
    client.subscribe(MQTT_TOPIC_STATUS, qos=0)
  else:
    mqtt_connected = False
    print(f"[MQTT] Gagal terhubung, kode rc={rc}")


def on_disconnect(client, userdata, rc):
  global mqtt_connected
  mqtt_connected = False
  print(f"[MQTT] Terputus dari broker (rc={rc})")


def on_message(client, userdata, msg):
  try:
    payload = msg.payload.decode("utf-8", errors="ignore").strip()
    now_str = datetime.now().strftime("%H:%M:%S")
    trigger_needed = False

    with state_lock:
      if msg.topic == MQTT_TOPIC_FLAME:
        system_state["flame_detected"] = payload in ("1", "true", "TRUE", "ON")
        system_state["last_flame_update"] = now_str
      elif msg.topic == MQTT_TOPIC_STATUS:
        upper = payload.upper()
        system_state["flame_detected"] = upper == "BAHAYA"
        system_state["last_flame_update"] = now_str

      flame_now = system_state["flame_detected"]
      level = compute_alert_level(flame_now)
      system_state["alert_level"] = level

      if flame_now and system_state["status"] == "STANDBY":
        system_state["status"] = "CAPTURING"
        trigger_needed = True

    if trigger_needed:
      threading.Thread(target=trigger_capture_and_verify, daemon=True).start()

  except Exception as e:
    print(f"[MQTT ERROR] {e}")


def start_mqtt_thread():
  mqtt_client.on_connect = on_connect
  mqtt_client.on_disconnect = on_disconnect
  mqtt_client.on_message = on_message
  while True:
    try:
      print(f"[MQTT] Menghubungkan ke {MQTT_BROKER}:{MQTT_PORT}...")
      mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=30)
      mqtt_client.loop_forever(retry_first_connection=True)
    except Exception as e:
      print(f"[MQTT RECONNECT] {e}")
      time.sleep(3)


@app.route("/")
def index():
  return render_template("dashboard.html")


@app.route("/api/status")
def get_status():
  with state_lock:
    return jsonify(system_state)


@app.route("/api/history")
def get_history():
  try:
    conn = get_db_connection()
    cursor = conn.cursor(dictionary=True)
    cursor.execute(
        "SELECT id, tanggal, waktu, gambar_path, besaran_api FROM fire_history"
        " ORDER BY id DESC"
    )
    rows = cursor.fetchall()

    cursor.execute(
        "SELECT COUNT(*) as total, AVG(besaran_api) as rata, MAX(besaran_api)"
        " as maks FROM fire_history"
    )
    stats = cursor.fetchone()
    cursor.close()
    conn.close()

    summary = {
        "total_kejadian": stats["total"] if stats["total"] else 0,
        "rata_besaran": round(stats["rata"], 2) if stats["rata"] else 0,
        "max_besaran": round(stats["maks"], 2) if stats["maks"] else 0,
    }
    return jsonify({"history": rows, "summary": summary})
  except Exception as e:
    return jsonify({
        "history": [],
        "summary": {"total_kejadian": 0, "rata_besaran": 0, "max_besaran": 0},
        "error": str(e),
    })


if __name__ == "__main__":
  threading.Thread(target=start_mqtt_thread, daemon=True).start()
  threading.Thread(target=status_10s_logger, daemon=True).start()
  app.run(host="0.0.0.0", port=5000, debug=False)