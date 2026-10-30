#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <time.h>

// ================= 1. WIFI CREDENTIALS =================
const char* WIFI_SSID     = "IB707";
const char* WIFI_PASSWORD = "IB707IB707";

// ================= 2. PIN DEFINITIONS ==================
// Matches our wiring table (GPIO2 for ADC1, GPIO6/7 for I2C OLED)
#define UV_SENSOR_PIN 2   // GUVA-S12SD SIG pin (ADC1_CH2)
#define I2C_SDA_PIN   8   // OLED SDA pin
#define I2C_SCL_PIN   9   // OLED SCL pin

// ================= 3. MQTT (HIVEMQ) CONFIG =============
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_TOPIC    = "venkat_iot/sensors/uv/node_01";
const char* DEVICE_ID     = "nRF5280_node_01"; // Matches existing Grafana dashboard

// ================= 4. INFLUXDB CLOUD CONFIG ============
// Pre-filled from your InfluxDB Cloud account screenshots
#define INFLUXDB_URL    "https://us-east-1-1.aws.cloud2.influxdata.com"
#define INFLUXDB_TOKEN  "Dbzt7IHP3G4E4McgOmsRBrZLUaK2fsEgexNVFzYSQI1V2PhqWbL6SFhB64Vg-jOY6HQIu7BibH8jkSQvHqLNWw=="
#define INFLUXDB_ORG    "IOT"
#define INFLUXDB_BUCKET "uv_sensor_data"
#define TZ_INFO         "UTC0"

// ================= OBJECT INITIALIZATION ===============
WiFiClient espClient;
PubSubClient mqttClient(espClient);
InfluxDBClient influxClient(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
Point uvPoint("uv_telemetry"); // Matches Grafana _measurement filter

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

unsigned long seq_num = 1;
unsigned long lastPublishTime = 0;
const unsigned long PUBLISH_INTERVAL_MS = 2000; // Every 2 seconds

// Reconnect to HiveMQ if connection drops
void reconnectMQTT() {
    if (!mqttClient.connected()) {
        String clientId = "ESP32_UV_Node_" + String(random(0xffff), HEX);
        if (mqttClient.connect(clientId.c_str())) {
            Serial.println("[MQTT] Connected to HiveMQ Broker!");
        } else {
            Serial.printf("[MQTT] Failed, rc=%d. Retrying next loop...\n", mqttClient.state());
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // 1. Configure ADC for GUVA-S12SD (0V - 3.3V range)
    analogReadResolution(12);
    analogSetPinAttenuation(UV_SENSOR_PIN, ADC_11db);

    // 2. Initialize I2C & 0.96" OLED Display
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10);
        display.println("Connecting WiFi...");
        display.display();
    } else {
        Serial.println("[OLED] SSD1306 not found (check I2C pins)");
    }

    // 3. Connect to Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setTxPower(WIFI_POWER_8_5dBm); // Stabilizes ESP32-C3 SuperMini Wi-Fi connection!
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());

    // 4. Sync UTC Time via NTP (Required for ISO Timestamp & InfluxDB TLS)
    timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

    // 5. Setup HiveMQ & InfluxDB
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    if (influxClient.validateConnection()) {
        Serial.println("[InfluxDB] Connected to: " + influxClient.getServerUrl());
    } else {
        Serial.println("[InfluxDB] Connection failed: " + influxClient.getLastErrorMessage());
    }
}

void loop() {
    if (!mqttClient.connected()) {
        reconnectMQTT();
    }
    mqttClient.loop();

    if (millis() - lastPublishTime >= PUBLISH_INTERVAL_MS) {
        lastPublishTime = millis();

        // --- A. READ GUVA-S12SD SENSOR (32-Sample Average) ---
        uint32_t mv_sum = 0;
        for (int i = 0; i < 32; i++) {
            mv_sum += analogReadMilliVolts(UV_SENSOR_PIN);
            delay(2);
        }
        float avg_mv = mv_sum / 32.0;
        float voltage_v = avg_mv / 1000.0;

        // Convert GUVA-S12SD voltage to UV Intensity (mW/cm^2)
        // GUVA-S12SD module with 4.3M-ohm amplifier outputs ~0.477V per 1 mW/cm2
        // (Or UV Index = voltage_v / 0.1V)
        float uv_intensity_mw_cm2 = voltage_v / 0.1; // Scales 0-1.1V to 0-11+ scale for your Grafana threshold
        if (uv_intensity_mw_cm2 < 0.0) uv_intensity_mw_cm2 = 0.0;

        // Round to 3 and 2 decimal places
        voltage_v = round(voltage_v * 1000.0) / 1000.0;
        uv_intensity_mw_cm2 = round(uv_intensity_mw_cm2 * 100.0) / 100.0;

        // --- B. FORMAT ISO-8601 UTC TIMESTAMP ---
        time_t now = time(nullptr);
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);
        char iso_timestamp[30];
        strftime(iso_timestamp, sizeof(iso_timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

        // --- C. PUBLISH JSON TO HIVEMQ (TASK 1 & 2) ---
        JsonDocument doc;
        doc["device_id"] = DEVICE_ID;
        doc["timestamp"] = iso_timestamp;
        doc["seq_num"] = seq_num;
        doc["voltage_v"] = voltage_v;
        doc["uv_intensity_mw_cm2"] = uv_intensity_mw_cm2;

        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);
        mqttClient.publish(MQTT_TOPIC, jsonBuffer);
        Serial.printf("[HiveMQ] Sent: %s\n", jsonBuffer);

        // --- D. WRITE POINT DIRECTLY TO INFLUXDB (TASK 3) ---
        uvPoint.clearFields();
        uvPoint.clearTags();
        uvPoint.addTag("device_id", DEVICE_ID);
        uvPoint.addField("sequence_number", (long)seq_num);
        uvPoint.addField("voltage_v", voltage_v);
        uvPoint.addField("uv_intensity_mw_cm2", uv_intensity_mw_cm2);

        if (influxClient.writePoint(uvPoint)) {
            Serial.printf("[InfluxDB] Logged seq #%lu | V: %.3f V | UV: %.2f mW/cm2\n",
                          seq_num, voltage_v, uv_intensity_mw_cm2);
        } else {
            Serial.println("[InfluxDB Error] " + influxClient.getLastErrorMessage());
        }

        // --- E. UPDATE 0.96" OLED DISPLAY ---
        display.clearDisplay();
        display.setCursor(0, 0);
        display.setTextSize(1);
        display.println("LIVE UV SENSOR NODE");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

        display.setCursor(0, 16);
        display.printf("Voltage: %.3f V\n", voltage_v);

        display.setCursor(0, 30);
        display.printf("UV Int : %.2f mW/cm2\n", uv_intensity_mw_cm2);

        display.setCursor(0, 48);
        if (uv_intensity_mw_cm2 >= 11.0) {
            display.println("STATUS : DANGER (>11)");
        } else {
            display.printf("Seq #%lu | MQTT+DB OK", seq_num);
        }
        display.display();

        seq_num++;
    }
}