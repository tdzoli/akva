#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Adafruit_TCS34725.h"

const char* mqtt_server = "4769a5772b894e1ea634b4eff4b1c6cd.s1.eu.hivemq.cloud";
const char* mqtt_user = "akva_admin"; 
const char* mqtt_pass = "Akva123456";

WiFiClientSecure espClient;
PubSubClient client(espClient);

Adafruit_ADS1115 ads;  
OneWire oneWire(D3);
DallasTemperature ds18b20(&oneWire);
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);

float pH_cal_value = 21.34; 
unsigned long lastMsg = 0;
unsigned long lastHeartbeat = 0;

void reconnect() {
  while (!client.connected()) {
    String clientId = "NodeA_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Node A MQTT Csatlakoztatva!");
    } else { delay(5000); }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(4, 5);
  ads.begin();
  tcs.begin();
  ds18b20.begin();

  WiFiManager wm;
  wm.autoConnect("AquaSzenzor_AP");
  espClient.setInsecure(); 
  client.setServer(mqtt_server, 8883);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  // 1. HEARTBEAT
  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    client.publish("aquarium/health/nodeA", "ONLINE");
  }

  // 2. SZENZOR ADATOK KÜLDÉSE
  if (millis() - lastMsg > 15000) { 
    lastMsg = millis();
    
    ds18b20.requestTemperatures(); 
    float tempC = ds18b20.getTempCByIndex(0);
    if (tempC < -50 || tempC > 80) tempC = 25.0; 
    
    int16_t adc0 = ads.readADC_SingleEnded(0);
    float voltsPH = ads.computeVolts(adc0);
    float ph_act = -5.70 * voltsPH + pH_cal_value; 

    int16_t adc1 = ads.readADC_SingleEnded(1);
    float voltsTDS = ads.computeVolts(adc1);
    float V_offset = 1.23; 
    float correctedVolts = (voltsTDS > V_offset) ? (voltsTDS - V_offset) : 0.0;
    float compCoef = 1.0 + 0.02 * (tempC - 25.0);
    float compVolts = correctedVolts / compCoef;
    float tdsValue = (133.42 * pow(compVolts, 3) - 255.86 * pow(compVolts, 2) + 857.39 * compVolts) * 0.5;
    if(tdsValue < 0) tdsValue = 0; 

    // TCS34725 Adatfeldolgozás RGB százalékokkal
    uint16_t r, g, b, c, colorTemp, lux;
    tcs.getRawData(&r, &g, &b, &c);
    lux = tcs.calculateLux(r, g, b); 
    if (lux <= 2) lux = 0;
    colorTemp = (c < 10 || lux == 0) ? 0 : tcs.calculateColorTemperature(r, g, b); 

    // ÚJ: RGB százalékok kiszámítása
    float r_pct = 0, g_pct = 0, b_pct = 0;
    float sum = r + g + b;
    if (sum > 0) {
      r_pct = ((float)r / sum) * 100.0;
      g_pct = ((float)g / sum) * 100.0;
      b_pct = ((float)b / sum) * 100.0;
    }

    // ÚJ: JSON kibővítése az RGB százalékokkal
    String payload = "{\"temp\":" + String(tempC, 2) + 
                     ",\"ph\":" + String(ph_act, 2) + 
                     ",\"tds\":" + String(tdsValue, 0) + 
                     ",\"lux\":" + String(lux) + 
                     ",\"kelvin\":" + String(colorTemp) + 
                     ",\"r_pct\":" + String(r_pct, 1) + 
                     ",\"g_pct\":" + String(g_pct, 1) + 
                     ",\"b_pct\":" + String(b_pct, 1) + "}";
                     
    client.publish("aquarium/sensor/data", payload.c_str());
  }
}