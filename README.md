# InfernoGuard - Arsitektur Dual-MCU (Tanpa DHT22)

## Struktur Folder
- `esp32_main/esp32_main.ino` : Firmware ESP32 utama tanpa DHT22 (Wi-Fi `Cy--`, MQTT HiveMQ `broker.hivemq.com:1883`, Flame sensor, LED RGB, serta Buzzer 10 detik saat menerima perintah `ON_10S` di topik `infernoguard/buzzer`).
- `esp32_cam/esp32_cam.ino` : Firmware ESP32-CAM AI-Thinker (Wi-Fi `Cy--`, HTTP WebServer port `80` endpoint `GET /capture`). Di-flash menggunakan ESP Downloader / USB-Serial.
- `server/app.py` : Backend Flask + YOLOv8 (`rabahdev/fire-smoke-yolov8n` / `best.pt`) tanpa sensor suhu DHT, mengambil snapshot HTTP dari ESP32-CAM saat api terdeteksi, mempublikasikan perintah buzzer via MQTT, dan menyimpan log ke database `api_robot`.
- `server/templates/dashboard.html` : Dashboard web pemantauan status dan riwayat deteksi api.
- `database/api_robot.sql` : Skrip pembuatan database `api_robot` dan tabel `fire_history`.

# Fire-Fighter-Robot-with-Yolov8n-
