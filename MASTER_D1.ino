#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <SD.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <time.h> 

const char* mqtt_server = "4769a5772b894e1ea634b4eff4b1c6cd.s1.eu.hivemq.cloud";
const char* mqtt_user = "akva_admin"; 
const char* mqtt_pass = "Akva123456";

const int SD_CS_PIN = D3; 
const int SD_CD_PIN = D4;

bool sdCardPresent = false;
volatile bool cdStateChanged = false;
unsigned long lastCdDebounce = 0;

WiFiClientSecure espClient;
PubSubClient client(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000); 

const char* logFileName = "/akva_log.csv";
const char* setupFileName = "/setup.csv";

float activeSetpoint = 0.0;
unsigned long lastHeartbeat = 0; 

ICACHE_RAM_ATTR void handleCDInterrupt() {
  cdStateChanged = true;
}

// 🔧 ÚJ: float → vesszős string
String formatFloat(float value, int decimals = 2) {
  String s = String(value, decimals);
  s.replace('.', ',');
  return s;
}

String getFormattedDateTime() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime); 
  char buffer[25];
  sprintf(buffer, "%04d.%02d.%02d %s",
          ptm->tm_year + 1900,
          ptm->tm_mon + 1,
          ptm->tm_mday,
          timeClient.getFormattedTime().c_str());
  return String(buffer);
}

void loadSetupFromSD() {
  if (!sdCardPresent) return;
  if (SD.exists(setupFileName)) {
    File setupFile = SD.open(setupFileName, FILE_READ);
    if (setupFile) {
      String content = setupFile.readStringUntil('\n');
      content.trim(); 
      if (content.length() > 0) activeSetpoint = content.toFloat();
      setupFile.close();
    }
  } else {
    File setupFile = SD.open(setupFileName, FILE_WRITE);
    if (setupFile) {
      setupFile.println("25.0");
      activeSetpoint = 25.0;
      setupFile.close();
    }
  }
}

bool saveSetupToSD(float newSetpoint) {
  if (!sdCardPresent) return false;
  if (SD.exists(setupFileName)) SD.remove(setupFileName); 
  
  File setupFile = SD.open(setupFileName, FILE_WRITE);
  if (setupFile) { 
    setupFile.println(newSetpoint); 
    setupFile.close(); 
    return true; 
  }
  return false; 
}

void setupSDCard() {
  if (digitalRead(SD_CD_PIN) == HIGH) {
    Serial.println("[SD] Nincs kartya a foglalatban!");
    sdCardPresent = false;
    client.publish("aquarium/health/sdcard", "ERROR", true);
    return;
  }

  if (!SD.begin(SD_CS_PIN)) { 
    Serial.println("[SD] Inicializalasi hiba!");
    sdCardPresent = false; 
    client.publish("aquarium/health/sdcard", "ERROR", true);
    return; 
  }
  
  sdCardPresent = true;
  client.publish("aquarium/health/sdcard", "OK", true);
  
  if (!SD.exists(logFileName)) {
    File logFile = SD.open(logFileName, FILE_WRITE);
    if (logFile) { 
      logFile.println("Datum_Ido;Homerseklet_C;pH;TDS_ppm;Fenyero_Lux;Szinhomerseklet_K;R_szazalek;G_szazalek;B_szazalek"); 
      logFile.close(); 
    }
  }
  loadSetupFromSD();
}

// 🔧 MÓDOSÍTVA: minden float vesszős
void logDataToSD(String timestamp, float temp, float ph, float tds, int lux, int kelvin, float r_pct, float g_pct, float b_pct) {
  if (!sdCardPresent) return;
  File logFile = SD.open(logFileName, FILE_WRITE);
  if (logFile) {
    logFile.print(timestamp); logFile.print(";");
    
    logFile.print(formatFloat(temp)); logFile.print(";");
    logFile.print(formatFloat(ph)); logFile.print(";");
    logFile.print(formatFloat(tds)); logFile.print(";");
    
    logFile.print(lux); logFile.print(";");
    logFile.print(kelvin); logFile.print(";");
    
    logFile.print(formatFloat(r_pct)); logFile.print(";");
    logFile.print(formatFloat(g_pct)); logFile.print(";");
    logFile.println(formatFloat(b_pct));
    
    logFile.close();
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  if (String(topic) == "aquarium/setpoint") {
    float requestedSetpoint = msg.toFloat();
    if (saveSetupToSD(requestedSetpoint)) {
      activeSetpoint = requestedSetpoint;
      client.publish("aquarium/status/setpoint", String(activeSetpoint).c_str(), true); 
    }
  }
  else if (String(topic) == "aquarium/request" && msg == "CHECK") {
    if(activeSetpoint > 0)
      client.publish("aquarium/status/setpoint", String(activeSetpoint).c_str(), true);
    client.publish("aquarium/health/sdcard", sdCardPresent ? "OK" : "ERROR", true);
  }
  else if (String(topic) == "aquarium/sensor/data") {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, msg);
    if (!error) {
      float r = doc["r_pct"] | 0.0;
      float g = doc["g_pct"] | 0.0;
      float b = doc["b_pct"] | 0.0;

      logDataToSD(
        getFormattedDateTime(),
        doc["temp"],
        doc["ph"],
        doc["tds"],
        doc["lux"],
        doc["kelvin"],
        r, g, b
      );
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "NodeC_Gerinc_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe("aquarium/setpoint");
      client.subscribe("aquarium/request"); 
      client.subscribe("aquarium/sensor/data"); 
      
      client.publish("aquarium/health/sdcard", sdCardPresent ? "OK" : "ERROR", true);
      if(activeSetpoint > 0)
        client.publish("aquarium/status/setpoint", String(activeSetpoint).c_str(), true);
    } else {
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(SD_CD_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SD_CD_PIN), handleCDInterrupt, CHANGE);
  
  WiFiManager wm;
  wm.autoConnect("AquaMaster_Gerinc");
  timeClient.begin();
  
  espClient.setInsecure();
  client.setServer(mqtt_server, 8883);
  client.setCallback(callback);
  
  setupSDCard();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  if (cdStateChanged) {
    if (millis() - lastCdDebounce > 200) { 
      lastCdDebounce = millis();
      cdStateChanged = false;
      
      bool isInserted = (digitalRead(SD_CD_PIN) == LOW);
      
      if (isInserted && !sdCardPresent) {
        setupSDCard(); 
      } 
      else if (!isInserted && sdCardPresent) {
        sdCardPresent = false;
        client.publish("aquarium/health/sdcard", "ERROR", true);
        SD.end(); 
      }
    }
  }

  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    client.publish("aquarium/health/nodeC", "ONLINE");
  }
}
