#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Kredensial Jaringan Wi-Fi
const char* ssid = "TECNO POVA 5";
const char* password = "Fandik11";

// Konfigurasi Broker MQTT Publik
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782097_ClosedLoop";

// Pembagian Topik MQTT Mandiri
const char* topicPublish   = "kampus/labiot/25782097/telemetri";
const char* topicSubscribe = "kampus/labiot/25782097/kendali";
const char* topicAlert     = "kampus/labiot/25782097/alert";
const char* topicStatus    = "kampus/labiot/25782097/status";

// Alokasi Pin Perangkat Keras Sesuai Instruksi Perangkaian
const byte dhtPin = 2; // Pin D4 (GPIO 2) untuk Sensor DHT22/DHT11
const byte ledPin = 5; // Pin D1 (GPIO 5) untuk Indikator LED + Resistor 220Ω ke GND
#define DHTTYPE DHT22  

DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 4000; // Siklus kirim telemetri periodik setiap 4 detik

// Variabel status dan state latch untuk proteksi alert overheat
bool isOverheat = false;
bool lastOverheatState = false;
int currentLedState = 0;

// Callback saat pesan perintah kendali manual diterima dari broker
void callback(char* topic, byte* payload, unsigned int length) {
  char incomingBuffer[length + 1];
  memcpy(incomingBuffer, payload, length);
  incomingBuffer[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n", topic, incomingBuffer);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, incomingBuffer);
  if (err) {
    Serial.println("Gagal membedah payload JSON!");
    return;
  }

  // Kendali Manual Override hanya diizinkan jika kondisi suhu TIDAK BAHAYA (NORMAL)
  if (doc.containsKey("led") && !isOverheat) {
    currentLedState = doc["led"];
    digitalWrite(ledPin, currentLedState == 1 ? HIGH : LOW);
    Serial.printf("--> [MANUAL OVERRIDE] Status LED diubah: %s\n", currentLedState == 1 ? "MENYALA" : "PADAM");
  } else if (isOverheat) {
    Serial.println("--> [INFO] Kendali manual diabaikan. Sistem terkunci pada mode BAHAYA (Overheat)!");
  }
  Serial.println("----------------------------------------");
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Menghubungkan kembali ke Broker...");
    
    // Konfigurasi Last Will and Testament (LWT) pada client.connect()
    // Topik LWT: kampus/labiot/nim_anda/status, QoS: 1, Retain: true
    if (client.connect(clientId, NULL, NULL, topicStatus, 1, true, "{\"status\":\"OFFLINE\",\"keterangan\":\"Perangkat Terputus Tak Terduga\"}")) {
      Serial.println(" Sukses Terhubung!");
      
      // Subskripsi kembali ke topik kendali
      client.subscribe(topicSubscribe);
      Serial.printf("Subskripsi Topik: %s\n", topicSubscribe);

      // Sesaat setelah terhubung, publikasikan pesan status ONLINE ke topik status
      const char* onlineMsg = "{\"status\":\"ONLINE\",\"keterangan\":\"Perangkat Beroperasi Normal\"}";
      client.publish(topicStatus, onlineMsg, true);
      Serial.println("[STATUS] Perangkat ONLINE");
      
    } else {
      Serial.printf("Gagal (rc=%d). Coba lagi dalam 4 detik...\n", client.state());
      delay(4000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Terkoneksi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // Memelihara sesi TCP dan memproses callback masuk secara asinkron

  unsigned long now = millis();
  if (now - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      // Mesin Aturan & Logika Keselamatan (Edge Safety Engine)
      if (t > 31.0) {
        isOverheat = true;
        currentLedState = 1;
        digitalWrite(ledPin, HIGH); // Otomatis menyalakan indikator LED darurat tanpa menunggu operator
      } else {
        isOverheat = false;
        currentLedState = 0;
        digitalWrite(ledPin, LOW);  // Otomatis mematikan LED saat kondisi kembali aman/normal
      }

      // Kanal Peringatan Bahaya Mandiri (Dedicated Alert Topic dengan State Latch)
      if (isOverheat && !lastOverheatState) {
        StaticJsonDocument<200> alertDoc;
        alertDoc["status"] = "BAHAYA";
        alertDoc["parameter"] = "OVERHEAT";
        alertDoc["nilai_suhu"] = serialized(String(t, 2));

        char alertBuffer[256];
        serializeJson(alertDoc, alertBuffer);
        client.publish(topicAlert, alertBuffer);
        Serial.printf("[ALERT TERKIRIM] %s\n", alertBuffer);
      }
      lastOverheatState = isOverheat;

      // Penyusunan Dokumen JSON Telemetri Rutin dengan Klasifikasi Kondisi Suhu
      StaticJsonDocument<256> docOut;
      docOut["node_id"] = clientId;
      docOut["suhu"] = serialized(String(t, 2));
      docOut["kelembapan"] = serialized(String(h, 2));
      docOut["status_led"] = digitalRead(ledPin);
      
      // Klasifikasi status lingkungan rutin
      if (t > 31.0) {
        docOut["kondisi_suhu"] = "PANAS";
      } else {
        docOut["kondisi_suhu"] = "NORMAL";
      }

      docOut["uptime_detik"] = millis() / 1000;
      docOut["ip_lokal"] = WiFi.localIP().toString();

      char buffer[256];
      serializeJson(docOut, buffer);
      
      bool success = client.publish(topicPublish, buffer);
      if (success) {
        Serial.printf("[TELEMETRI TERKIRIM] %s\n", buffer);
      } else {
        Serial.println("[ERROR] Gagal memublikasikan telemetri!");
      }
    } else {
      Serial.println("Gagal membaca dari sensor DHT22!");
    }
  }
}
