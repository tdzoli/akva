#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Adafruit_TCS34725.h"
#include <espnow.h>

const char* mqtt_server = "";
const char* mqtt_user = ""; 
const char* mqtt_pass = "";

// ============================================================
// KALIBRÁCIÓ BEÁLLÍTÁSA
// ============================================================
// true  = Kalibráló mód (csak Serial-ra ír nyers feszültséget)
// false = Normál üzem (ESP-NOW + MQTT, eredeti működés)
const bool CALIB_MODE = false;

// Kalibrált értékek (2025. mérés, pH 4.01/6.86/9.18 pufferekből):
// pH = m × V + b
float pH_slope     = -5.748;
float pH_intercept = 21.616;
// ============================================================

WiFiClientSecure espClient;
PubSubClient client(espClient);

Adafruit_ADS1115 ads;
OneWire oneWire(D3);
DallasTemperature ds18b20(&oneWire);
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);

unsigned long lastMsg = 0;
unsigned long lastHeartbeat = 0;

uint8_t masterAddress[] = {0xA0, 0x20, 0xA6, 0x19, 0x1A, 0xD8};

typedef struct sensor_data_struct {
  float temp;
  float ph;
  float tds;
  int lux;
  int kelvin;
  float r_pct;
  float g_pct;
  float b_pct;
} sensor_data_struct;

sensor_data_struct myData;

void reconnect() {
  while (!client.connected()) {
    String clientId = "NodeA_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Node A MQTT Csatlakoztatva!");
    } else {
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(4, 5);
  ads.begin();
  ds18b20.begin();

  if (CALIB_MODE) {
    Serial.println();
    Serial.println("=============================================");
    Serial.println("   pH SZENZOR KALIBRACIOS MOD AKTIV");
    Serial.println("=============================================");
    Serial.println();
    Serial.println("Lepes 1: Helyezd a szondat pH 6.86 pufferbe.");
    Serial.println("         A potmeterrel allitsd a feszultseget ~2.50 V-ra.");
    Serial.println("Lepes 2: Oblites utan pH 4.01 pufferbe. Jegyezd fel a V erteket.");
    Serial.println("Lepes 3: Oblites utan pH 9.18 pufferbe. Jegyezd fel a V erteket.");
    Serial.println();
    Serial.println("A meres 2 masodpercenkent frissul.");
    Serial.println("=============================================");
    Serial.println();
    return;
  }

  tcs.begin();

  WiFiManager wm;
  wm.autoConnect("AquaSzenzor_AP");

  if (esp_now_init() != 0) {
    Serial.println("Hiba az ESP-NOW inicializalasakor!");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_add_peer(masterAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

  espClient.setInsecure();
  client.setServer(mqtt_server, 8883);
}

void loop() {

  // ==========================================================
  // KALIBRÁLÓ MÓD
  // ==========================================================
  if (CALIB_MODE) {
    if (millis() - lastMsg > 2000) {
      lastMsg = millis();

      ds18b20.requestTemperatures();
      float tempC = ds18b20.getTempCByIndex(0);
      if (tempC < -50 || tempC > 80) tempC = -999.0;

      int16_t adc0 = ads.readADC_SingleEnded(0);
      float voltsPH = ads.computeVolts(adc0);
      float ph_calc = pH_slope * voltsPH + pH_intercept;

      Serial.print("Homerseklet: ");
      Serial.print(tempC, 2);
      Serial.print(" C  |  pH feszultseg: ");
      Serial.print(voltsPH, 4);
      Serial.print(" V  |  ADC nyers: ");
      Serial.print(adc0);
      Serial.print("  |  Szamitott pH (jelenlegi keplet): ");
      Serial.println(ph_calc, 2);
    }
    return;
  }

  // ==========================================================
  // NORMÁL ÜZEM
  // ==========================================================
  if (!client.connected()) reconnect();
  client.loop();

  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    client.publish("aquarium/health/nodeA", "ONLINE");
  }

  if (millis() - lastMsg > 15000) {
    lastMsg = millis();

    ds18b20.requestTemperatures();
    float tempC = ds18b20.getTempCByIndex(0);
    if (tempC < -50 || tempC > 80) tempC = 25.0;

    int16_t adc0 = ads.readADC_SingleEnded(0);
    float voltsPH = ads.computeVolts(adc0);
    float ph_act = pH_slope * voltsPH + pH_intercept;

    int16_t adc1 = ads.readADC_SingleEnded(1);
    float voltsTDS = ads.computeVolts(adc1);
    float V_offset = 1.23;
    float correctedVolts = (voltsTDS > V_offset) ? (voltsTDS - V_offset) : 0.0;
    float compCoef = 1.0 + 0.02 * (tempC - 25.0);
    float compVolts = correctedVolts / compCoef;
    float tdsValue = (133.42 * pow(compVolts, 3) - 255.86 * pow(compVolts, 2) + 857.39 * compVolts) * 0.5;
    if (tdsValue < 0) tdsValue = 0;

    uint16_t r, g, b, c, colorTemp, lux;
    tcs.getRawData(&r, &g, &b, &c);
    lux = tcs.calculateLux(r, g, b);
    if (lux <= 2) lux = 0;
    colorTemp = (c < 10 || lux == 0) ? 0 : tcs.calculateColorTemperature(r, g, b);

    float r_pct = 0, g_pct = 0, b_pct = 0;
    float sum = r + g + b;
    if (sum > 0) {
      r_pct = ((float)r / sum) * 100.0;
      g_pct = ((float)g / sum) * 100.0;
      b_pct = ((float)b / sum) * 100.0;
    }

    myData.temp = tempC;
    myData.ph = ph_act;
    myData.tds = tdsValue;
    myData.lux = lux;
    myData.kelvin = colorTemp;
    myData.r_pct = r_pct;
    myData.g_pct = g_pct;
    myData.b_pct = b_pct;

    esp_now_send(masterAddress, (uint8_t *)&myData, sizeof(myData));
    Serial.println("Adatcsomag elkuldve ESP-NOW-n a Gerincnek.");
  }
}
