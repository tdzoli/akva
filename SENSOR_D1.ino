#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Adafruit_TCS34725.h"
#include <espnow.h> // ÚJ: ESP-NOW könyvtár

const char* mqtt_server = "";
const char* mqtt_user = "";
const char* mqtt_pass = "";

WiFiClientSecure espClient;
PubSubClient client(espClient);

Adafruit_ADS1115 ads;  
OneWire oneWire(D3);
DallasTemperature ds18b20(&oneWire);
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);

float pH_cal_value = 21.34; 
unsigned long lastMsg = 0;
unsigned long lastHeartbeat = 0;

// ÚJ: A Gerinc (Node C) MAC címe
uint8_t masterAddress[] = {0xA0, 0x20, 0xA6, 0x19, 0x1A, 0xD8};

// ÚJ: Adatstruktúra definiálása (könnyű és gyors adatátvitelhez)
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
  tcs.begin();
  ds18b20.begin();

  // WiFiManager inicializálása
  WiFiManager wm;
  wm.autoConnect("AquaSzenzor_AP");
  
  // ÚJ: ESP-NOW inicializálása a WiFi csatlakozás után
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
  if (!client.connected()) reconnect();
  client.loop();

  // 1. HEARTBEAT (Ez továbbra is a felhőbe megy, jelezve, hogy a szenzor él)
  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    client.publish("aquarium/health/nodeA", "ONLINE");
  }

  // 2. SZENZOR ADATOK OLVASÁSA ÉS KÜLDÉSE (Közvetlenül a Gerincnek)
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

    // TCS34725 Adatfeldolgozás
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

    // ÚJ: Struktúra feltöltése a mért adatokkal
    myData.temp = tempC;
    myData.ph = ph_act;
    myData.tds = tdsValue;
    myData.lux = lux;
    myData.kelvin = colorTemp;
    myData.r_pct = r_pct;
    myData.g_pct = g_pct;
    myData.b_pct = b_pct;

    // ÚJ: Adatküldés ESP-NOW-val közvetlenül a Gerincnek (offline)
    esp_now_send(masterAddress, (uint8_t *) &myData, sizeof(myData));
    
    Serial.println("Adatcsomag elkuldve ESP-NOW-n a Gerincnek.");
  }
}