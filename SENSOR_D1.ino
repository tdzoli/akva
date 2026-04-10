#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Adafruit_TCS34725.h"
#include <espnow.h>

// A Gerinc (Node C) MAC címe
uint8_t masterAddress[] = {0xA0, 0x20, 0xA6, 0x19, 0x1A, 0xD8};

Adafruit_ADS1115 ads;
OneWire oneWire(D3);
DallasTemperature ds18b20(&oneWire);
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);

float pH_cal_value = 21.34;
unsigned long lastMsg = 0;

// Adatstruktúra (meg kell egyeznie a Gerinc struktúrájával!)
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

// ÚJ: ESP-NOW küldés visszajelző callback
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (sendStatus == 0) {
    Serial.println("[ESP-NOW] Adatcsomag SIKERESEN kézbesítve a Gerincnek.");
  } else {
    Serial.println("[ESP-NOW] HIBA: Adatcsomag kézbesítés SIKERTELEN! (Kód: " + String(sendStatus) + ")");
    Serial.println("[ESP-NOW] Lehetséges ok: Node C offline, csatorna eltérés, vagy hatótávolságon kívül.");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[NODE A] Szenzor modul indul...");

  Wire.begin(4, 5);
  ads.begin();
  tcs.begin();
  ds18b20.begin();

  // WiFiManager: WiFi csatlakozás (ez határozza meg a csatornát)
  WiFiManager wm;
  wm.autoConnect("AquaSzenzor_AP");

  Serial.print("[WiFi] Csatlakozva! Csatorna: ");
  Serial.println(WiFi.channel());
  Serial.print("[WiFi] Szabad heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" byte");

  // ESP-NOW inicializálása
  if (esp_now_init() != 0) {
    Serial.println("[ESP-NOW] KRITIKUS HIBA: Inicializáció sikertelen!");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);

  // ÚJ: Küldés-visszajelzés callback regisztrálása
  esp_now_register_send_cb(OnDataSent);

  // ÚJ: Peer hozzáadása a WiFi AKTUÁLIS csatornájával
  // A 0 csatorna "auto"-t jelent, ami problémás lehet, ha a két ESP
  // különböző routeren van, vagy a router csatornát váltott.
  // A WiFi.channel() biztosítja a szinkronizációt.
  uint8_t currentChannel = WiFi.channel();
  esp_now_add_peer(masterAddress, ESP_NOW_ROLE_COMBO, currentChannel, NULL, 0);
  Serial.println("[ESP-NOW] Gerinc (Node C) peer hozzáadva, csatorna: " + String(currentChannel));
}

void loop() {
  // Nincs MQTT loop — az összes kommunikáció ESP-NOW-on keresztül zajlik.
  // Ez ~15-20KB RAM megtakarítást jelent a TLS stack eltávolításával.

  if (millis() - lastMsg > 15000) {
    lastMsg = millis();

    // --- DS18B20 Hőmérséklet ---
    ds18b20.requestTemperatures();
    float tempC = ds18b20.getTempCByIndex(0);
    if (tempC < -50 || tempC > 80) tempC = 25.0;

    // --- pH mérés (ADS1115, A0 csatorna) ---
    int16_t adc0 = ads.readADC_SingleEnded(0);
    float voltsPH = ads.computeVolts(adc0);
    float ph_act = -5.70 * voltsPH + pH_cal_value;

    // --- TDS mérés (ADS1115, A1 csatorna) ---
    int16_t adc1 = ads.readADC_SingleEnded(1);
    float voltsTDS = ads.computeVolts(adc1);
    float V_offset = 1.23;
    float correctedVolts = (voltsTDS > V_offset) ? (voltsTDS - V_offset) : 0.0;
    float compCoef = 1.0 + 0.02 * (tempC - 25.0);
    float compVolts = correctedVolts / compCoef;
    float tdsValue = (133.42 * pow(compVolts, 3) - 255.86 * pow(compVolts, 2) + 857.39 * compVolts) * 0.5;
    if (tdsValue < 0) tdsValue = 0;

    // --- TCS34725 Fényszenzor ---
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

    // --- Struktúra feltöltése ---
    myData.temp = tempC;
    myData.ph = ph_act;
    myData.tds = tdsValue;
    myData.lux = lux;
    myData.kelvin = colorTemp;
    myData.r_pct = r_pct;
    myData.g_pct = g_pct;
    myData.b_pct = b_pct;

    // --- Küldés ESP-NOW-val ---
    int result = esp_now_send(masterAddress, (uint8_t *) &myData, sizeof(myData));
    if (result != 0) {
      Serial.println("[ESP-NOW] HIBA: esp_now_send() visszatérési kód: " + String(result));
    }

    // --- Debug kiírás soros monitorra ---
    Serial.println("--- Mért értékek ---");
    Serial.println("  Hőmérséklet: " + String(tempC, 2) + " °C");
    Serial.println("  pH: " + String(ph_act, 2));
    Serial.println("  TDS: " + String(tdsValue, 0) + " ppm");
    Serial.println("  Lux: " + String(lux) + "  Kelvin: " + String(colorTemp));
    Serial.println("  RGB%: R=" + String(r_pct, 1) + " G=" + String(g_pct, 1) + " B=" + String(b_pct, 1));
    Serial.println("  Szabad heap: " + String(ESP.getFreeHeap()) + " byte");
  }
}
