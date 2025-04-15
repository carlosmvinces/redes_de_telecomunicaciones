#include <WiFi.h>
#include <HTTPClient.h>
#include <HX711.h>
#include <HardwareSerial.h>
#include <ESP32Servo.h>

// ========== CONFIGURACIÓN WiFi ==========
const char* ssid = "ssid";
const char* password = "pswd";
const char* server_host = "http://IP:1880";
const char* THINGSPEAK_API_KEY = "writing_api_token";
const char* THINGSPEAK_URL = "http://api.thingspeak.com/update";

// ========== PINES ==========
#define RFID_RX 16
#define RFID_TX 17
#define HX711_DT 4
#define HX711_SCK 5
#define SERVO_PIN 13
#define AGUA_SENSOR_PIN 18
#define RELAY_BOMBA 19

HX711 balanza;
HardwareSerial RFID(2);
Servo servoMotor;

String ultimoTag = "";
unsigned long ultimoTiempoLectura = 0;
unsigned long intervaloLectura = 5000;

unsigned long lastPost = 0;
unsigned long postInterval = 10000;

unsigned long lastConsulta = 0;
unsigned long consultaInterval = 3000;

bool pulsoRFID = false;

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n✅ WiFi conectado");

  RFID.begin(9600, SERIAL_8N1, RFID_RX, RFID_TX);
  balanza.begin(HX711_DT, HX711_SCK);
  balanza.set_scale(-207.78);
  balanza.tare();

  servoMotor.setPeriodHertz(50);
  servoMotor.attach(SERVO_PIN, 500, 2400);
  servoMotor.write(0);

  pinMode(AGUA_SENSOR_PIN, INPUT_PULLUP);
  pinMode(RELAY_BOMBA, OUTPUT);
  digitalWrite(RELAY_BOMBA, HIGH); // bomba apagada
}

void loop() {
  unsigned long ahora = millis();

  // ===== LECTURA RFID =====
  if (RFID.available()) {
    String tag = RFID.readStringUntil('#');
    tag.trim();
    tag += '#';

    if (tag != ultimoTag || (ahora - ultimoTiempoLectura > intervaloLectura)) {
      ultimoTag = tag;
      ultimoTiempoLectura = ahora;
      pulsoRFID = true;
      Serial.println("📶 RFID leído: " + tag);

      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(String(server_host) + "/rfid");
        int resp = http.POST("");
        Serial.println("POST /rfid -> " + String(resp));
        http.end();
      }
    }

    // ⚠️ Limpiar cualquier dato restante en el búfer para evitar doble lectura
    while (RFID.available()) RFID.read();
  }

  // ===== ENVÍO DE DATOS A NODERED Y THINGSPEAK =====
  if (ahora - lastPost > postInterval) {
    lastPost = ahora;

    float peso = balanza.get_units(5);
    bool hayAgua = (digitalRead(AGUA_SENSOR_PIN) == LOW);
    bool aguaInvertida = hayAgua ? 1 : 0;
    int pulso = pulsoRFID ? 1 : 0;

    Serial.printf("📤 Peso: %.2f g | Agua baja: %s | Pulso RFID: %d\n", peso, hayAgua ? "SÍ" : "NO", pulso);

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;

      http.begin(String(server_host) + "/peso");
      http.addHeader("Content-Type", "application/json");
      http.POST("{\"peso\":" + String(peso, 2) + "}");
      http.end();

      http.begin(String(server_host) + "/agua");
      http.addHeader("Content-Type", "application/json");
      http.POST("{\"agua_baja\":" + String(!hayAgua ? "true" : "false") + "}");
      http.end();
    }

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      String url = String(THINGSPEAK_URL) +
                   "?api_key=" + THINGSPEAK_API_KEY +
                   "&field1=" + String(peso, 2) +
                   "&field2=" + String(aguaInvertida) +
                   "&field4=" + String(pulso);
      http.begin(url);
      int responseCode = http.GET();
      http.end();

      if (responseCode > 0) {
        Serial.println("✅ Datos enviados a ThingSpeak");
      } else {
        Serial.println("❌ Error enviando a ThingSpeak");
      }
    }

    pulsoRFID = false; // ✅ Reiniciar justo después del envío, no antes
  }

  // ===== CONSULTAR Node-RED (botones) =====
  if (ahora - lastConsulta > consultaInterval) {
    lastConsulta = ahora;

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(String(server_host) + "/consultar");
      int respCode = http.GET();

      if (respCode == 200) {
        String resp = http.getString();

        if (resp.indexOf("\"activar_servo\":true") != -1) {
          Serial.println("✅ Activando servo desde Node-RED");
          servoMotor.write(90);
          delay(3000);
          servoMotor.write(0);
        }

        if (resp.indexOf("\"activar_bomba\":true") != -1) {
          if (digitalRead(AGUA_SENSOR_PIN) == LOW) {
            Serial.println("⚠️ Agua ya en nivel alto. Bomba no activada.");
            return;
          }

          Serial.println("✅ Activando bomba desde Node-RED");
          digitalWrite(RELAY_BOMBA, LOW);
          unsigned long inicio = millis();
          unsigned long timeout = 30000;

          while (digitalRead(AGUA_SENSOR_PIN) == HIGH && millis() - inicio < timeout) {
            delay(100);
          }

          digitalWrite(RELAY_BOMBA, HIGH);
          Serial.println("🚿 Bomba apagada.");
        }
      }

      http.end();
    }
  }
}
