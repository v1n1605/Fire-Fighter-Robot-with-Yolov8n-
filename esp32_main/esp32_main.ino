#include <WiFi.h>
#include <PubSubClient.h>

// --- KONFIGURASI WI-FI & MQTT HIVEMQ ---
const char* WIFI_SSID     = "Cy--";
const char* WIFI_PASSWORD = "kzoinucd";

const char* MQTT_BROKER       = "broker.hivemq.com";
const int   MQTT_PORT         = 1883;
const char* MQTT_TOPIC_FLAME  = "infernoguard/flame";
const char* MQTT_TOPIC_STATUS = "infernoguard/alert_status";
const char* MQTT_TOPIC_BUZZER = "infernoguard/buzzer";

// --- PIN DRIVER MOTOR L298N & ESP32 ---
const int ENA = 23;   // PWM Motor Kiri
const int IN1 = 19;
const int IN2 = 18;
const int IN3 = 5;
const int IN4 = 17;
const int ENB = 16;   // PWM Motor Kanan

// --- PIN SENSOR API (S1 TIDAK DIGUNAKAN; HANYA S2 - S5) ---
const int NUM_FLAME_SENSORS = 5;
const int flamePins[NUM_FLAME_SENSORS] = {25, 33, 32, 35, 34};
// flamePins[0] = 25 (S1 - Tidak digunakan)
// flamePins[1] = 33 (S2 - Kiri)
// flamePins[2] = 32 (S3 - Center saja)
// flamePins[3] = 35 (S4 - Kanan)
// flamePins[4] = 34 (S5 - Kanan)

// --- PIN ULTRASONIK HC-SR04 ---
const int TRIG_PIN = 21;
const int ECHO_PIN = 22;

// --- PIN BUZZER ---
const int BUZZER_PIN = 4;

// --- KONSTANTA & THRESHOLD ---
const int MOTOR_SPEED = 180;
const int RETREAT_SPEED = 150;            // Kecepatan mundur agar lebih terkontrol
const int FIRE_THRESHOLD = 1000;          // Minimal nilai sensor untuk dianggap ada api (makin dekat makin naik)
const int STOP_DISTANCE_CM = 15;          // Batas jarak mulai mundur / jaga jarak (<= 20 cm)
const int SAFE_DISTANCE_CM = 25;          // Jarak aman setelah mundur (histeresis agar tidak bolak-balik cepat)
const unsigned long SENSOR_INTERVAL_MS = 1000UL;
const unsigned long CAMERA_BUZZER_DURATION_MS = 10000UL;

enum FireDirection {
  NO_FIRE     = 0,
  FIRE_LEFT   = 1,
  FIRE_CENTER = 2,
  FIRE_RIGHT  = 3,
};

WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long lastMqttPublishMs = 0;
unsigned long buzzerUntilMs     = 0;
bool isRetreating               = false;

// --- PROTOTIPE FUNGSI ---
long bacaSekaliCm();
long bacaUltrasonikMedian();
int predictFireDirectionMax(const int values[NUM_FLAME_SENSORS]);
void applyDecision(int direction, long jarakCm);
void updateBuzzerFromDbAlert();
void jalanMaju(int spd);
void jalanMundur(int spd);
void belokKanan(int spd);
void belokKiri(int spd);
void hentiTotal();
void printDebug(const int values[NUM_FLAME_SENSORS], int direction, long jarak);
const char* directionName(int direction);

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  // Buzzer hanya aktif jika server mengirim ON_10S setelah api terdeteksi & disimpan ke DB
  if (String(topic) == MQTT_TOPIC_BUZZER) {
    if (message == "ON_10S" || message == "ON" || message == "1") {
      buzzerUntilMs = millis() + CAMERA_BUZZER_DURATION_MS;
      Serial.println("[BUZZER DB] Api terkonfirmasi & tersimpan di DB -> Buzzer aktif 10 detik!");
    }
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Menghubungkan ke ");
  Serial.print(WIFI_SSID);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < 15000UL) {
    delay(500);
    Serial.print(".");
    updateBuzzerFromDbAlert();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Terhubung! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Belum terhubung, coba lagi di loop berikutnya.");
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;

  String clientId = "InfernoGuard_Robot_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("[MQTT] Terhubung ke broker.");
    mqttClient.subscribe(MQTT_TOPIC_BUZZER);
  } else {
    Serial.print("[MQTT] Gagal connect, rc=");
    Serial.println(mqttClient.state());
  }
}

void updateBuzzerFromDbAlert() {
  unsigned long now = millis();
  if (buzzerUntilMs > 0 && (long)(now - buzzerUntilMs) < 0) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    buzzerUntilMs = 0;
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(ENA, OUTPUT);
  pinMode(ENB, OUTPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  // Konfigurasi hanya pin S2 - S5 (indeks 1..4); S1 (indeks 0) tidak digunakan
  for (int i = 1; i < NUM_FLAME_SENSORS; i++) {
    pinMode(flamePins[i], INPUT);
  }

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  hentiTotal();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  connectWiFi();
  connectMQTT();

  Serial.println("Robot Pemadam Api (Mundur Jaga Jarak + Buzzer Hanya Saat Tersimpan DB) Siap!");
}

void loop() {
  connectWiFi();
  if (!mqttClient.connected()) {
    static unsigned long lastMqttRetry = 0;
    if (millis() - lastMqttRetry >= 3000UL) {
      lastMqttRetry = millis();
      connectMQTT();
    }
  } else {
    mqttClient.loop();
  }

  // Buzzer hanya mengikuti status alarm dari server/DB
  updateBuzzerFromDbAlert();

  // 1. Baca jarak ultrasonik terfilter median
  long jarakCm = bacaUltrasonikMedian();

  // 2. Baca sensor aktif S2 - S5 (S1 diabaikan / diset 0)
  int flameValues[NUM_FLAME_SENSORS];
  flameValues[0] = 0; // S1 tidak digunakan
  for (int i = 1; i < NUM_FLAME_SENSORS; i++) {
    flameValues[i] = analogRead(flamePins[i]);
  }

  // 3. Tentukan arah menggunakan logika Max Sensor pada S2 - S5
  int direction = predictFireDirectionMax(flameValues);
  bool flameDetected = (direction != NO_FIRE);

  // Publikasi status api ke MQTT secara berkala tanpa menghambat loop kontrol
  unsigned long nowMs = millis();
  if (mqttClient.connected() && (nowMs - lastMqttPublishMs >= SENSOR_INTERVAL_MS)) {
    lastMqttPublishMs = nowMs;
    mqttClient.publish(MQTT_TOPIC_FLAME, flameDetected ? "1" : "0");
    mqttClient.publish(MQTT_TOPIC_STATUS, flameDetected ? "BAHAYA" : "AMAN");
  }

  // Debug ke Serial Monitor
  printDebug(flameValues, direction, jarakCm);

  // 4. Eksekusi keputusan gerakan (Mundur untuk jaga jarak & cegah menabrak)
  applyDecision(direction, jarakCm);

  delay(40);
}

// --- BACA PULSA ULTRASONIK TUNGGAL ---
long bacaSekaliCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  if (duration == 0) return 999;

  long cm = (duration * 34) / 2000;
  if (cm <= 0) return 999;
  return cm;
}

// --- FILTER MEDIAN 5 SAMPEL ---
long bacaUltrasonikMedian() {
  const int N = 5;
  long samples[N];

  for (int i = 0; i < N; i++) {
    samples[i] = bacaSekaliCm();
    if (i < N - 1) delay(6);
  }

  for (int i = 1; i < N; i++) {
    long key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }

  return samples[N / 2];
}

// --- LOGIKA PENCARIAN SENSOR TERTINGGI (HANYA S2 - S5; S3 SAJA = CENTER) ---
int predictFireDirectionMax(const int values[NUM_FLAME_SENSORS]) {
  int maxVal = values[1]; // Mulai dari S2 (indeks 1)
  int maxIndex = 1;

  for (int i = 2; i < NUM_FLAME_SENSORS; i++) {
    if (values[i] > maxVal) {
      maxVal = values[i];
      maxIndex = i;
    }
  }

  if (maxVal < FIRE_THRESHOLD) {
    return NO_FIRE;
  }

  if (maxIndex == 1) return FIRE_LEFT;               // S2
  if (maxIndex == 2) return FIRE_CENTER;             // S3 saja sebagai center
  if (maxIndex == 3 || maxIndex == 4) return FIRE_RIGHT; // S4 & S5

  return NO_FIRE;
}

// --- KEPUTUSAN GERAKAN: JAGA JARAK DENGAN MUNDUR OTOMATIS ---
void applyDecision(int direction, long jarakCm) {
  // Aktifkan mode mundur jika jarak terlalu dekat (<= STOP_DISTANCE_CM)
  if (jarakCm > 0 && jarakCm <= STOP_DISTANCE_CM) {
    isRetreating = true;
  } else if (jarakCm >= SAFE_DISTANCE_CM || jarakCm <= 0) {
    // Keluar dari mode mundur hanya setelah jarak mencapai batas aman (>= 25 cm)
    isRetreating = false;
  }

  // Prioritas 1: Mundur untuk menjaga jarak agar tidak menabrak
  if (isRetreating) {
    jalanMundur(RETREAT_SPEED);
    return;
  }

  // Prioritas 2: Jarak aman dan tidak ada api -> berhenti
  if (direction == NO_FIRE) {
    hentiTotal();
    return;
  }

  // Prioritas 3: Jarak aman dan ada api -> ikuti arah api (maju hanya jika jarak cukup aman)
  if (direction == FIRE_LEFT) {
    belokKanan(MOTOR_SPEED);
  } else if (direction == FIRE_RIGHT) {
    belokKiri(MOTOR_SPEED);
  } else if (direction == FIRE_CENTER) {
    if (jarakCm > SAFE_DISTANCE_CM) {
      jalanMaju(MOTOR_SPEED);
    } else {
      hentiTotal();
    }
  }
}

// --- FUNGSI MOTOR L298N ---
void jalanMaju(int spd) {
  analogWrite(ENA, spd);
  analogWrite(ENB, spd);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void jalanMundur(int spd) {
  analogWrite(ENA, spd);
  analogWrite(ENB, spd);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void belokKanan(int spd) {
  analogWrite(ENA, spd);
  analogWrite(ENB, spd);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void belokKiri(int spd) {
  analogWrite(ENA, spd);
  analogWrite(ENB, spd);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void hentiTotal() {
  analogWrite(ENA, 0);
  analogWrite(ENB, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

// --- DEBUG SERIAL ---
void printDebug(const int values[NUM_FLAME_SENSORS], int direction, long jarak) {
  Serial.print("Jarak: "); Serial.print(jarak); Serial.print("cm | ");
  Serial.print("Sensor [S2-S5]: ");
  for (int i = 1; i < NUM_FLAME_SENSORS; i++) {
    Serial.print(values[i]);
    if (i < NUM_FLAME_SENSORS - 1) Serial.print(",");
  }
  Serial.print(" => Arah: ");
  Serial.print(directionName(direction));

  if (isRetreating) {
    Serial.print(" [MUNDUR JAGA JARAK]");
  } else if (direction == FIRE_CENTER && jarak <= SAFE_DISTANCE_CM) {
    Serial.print(" [STOP DEKAT API]");
  }
  Serial.println();
}

const char* directionName(int direction) {
  if (direction == FIRE_LEFT)   return "FIRE_LEFT";
  if (direction == FIRE_CENTER) return "FIRE_CENTER";
  if (direction == FIRE_RIGHT)  return "FIRE_RIGHT";
  return "NO_FIRE";
}