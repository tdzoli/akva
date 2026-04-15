#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <SD.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h> 
#include <espnow.h> 

const char* mqtt_server = "";
const char* mqtt_user = ""; 
const char* mqtt_pass = "";

// Telegram Bot Token
const char* botToken = "";

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
const char* fishCatalogFileName = "/fish_catalog.csv";

// Globális határértékek
float activeSetpoint = 25.0;
float tempMin = 20.0, tempMax = 28.0;
float phMin = 6.0, phMax = 8.0;
float tdsMin = 0, tdsMax = 1000;
int luxMin = 0, luxMax = 10000;
String chatId = "0";

unsigned long lastHeartbeat = 0;
unsigned long lastTelegramCheck = 0; 

// ÚJ: Boot üzenet egyszeri elküldéséhez
bool isFirstBoot = true;

// ÚJ: Telegram üzenetsor (nem blokkoló küldéshez)
// -------------------------------------------------
// A régi megoldásban a sendTelegramMessage() azonnal bontotta az MQTT-t,
// létrehozott egy TLS kapcsolatot a Telegram API felé, és megvárta a választ.
// Ez 3-10 másodpercig blokkolta a loop()-ot, ami alatt:
//   - Az ESP-NOW callback-ből érkező szenzor-adatok elveszhettek
//   - A heartbeat kimaradt → a dashboard OFFLINE-nak látta Node C-t
//   - Az MQTT keep-alive lejárhatott → a broker bontotta a kapcsolatot
//
// Új megoldás: A Telegram üzeneteket egy FIFO sorba (queue) tesszük,
// és a loop() minden körében csak EGYET küldünk el, ha van várakozó.
// Így a blokkolás max. 1 üzenetre korlátozódik, és a többi funkció
// (heartbeat, ESP-NOW, MQTT) a következő loop iterációban azonnal fut.
const int TELEGRAM_QUEUE_SIZE = 5;
String telegramQueue[TELEGRAM_QUEUE_SIZE];
int telegramQueueHead = 0;  // Következő olvasási pozíció
int telegramQueueTail = 0;  // Következő írási pozíció
int telegramQueueCount = 0; // Aktuális elemszám

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

sensor_data_struct receivedData;
volatile bool newDataAvailable = false;
bool hasReceivedFirstData = false;
bool fishCatalogRequestPending = false;

String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// ÚJ: Üzenet hozzáadása a Telegram sorhoz
// Ha a sor tele van, a legrégebbi üzenetet eldobjuk (felülírjuk).
void queueTelegramMessage(String text) {
  if (chatId == "" || chatId == "0") return;
  
  if (telegramQueueCount >= TELEGRAM_QUEUE_SIZE) {
    // Sor tele → legrégebbi eldobása
    Serial.println("[Telegram] FIGYELEM: Sor tele, legregebbi uzenet eldobva!");
    telegramQueueHead = (telegramQueueHead + 1) % TELEGRAM_QUEUE_SIZE;
    telegramQueueCount--;
  }
  
  telegramQueue[telegramQueueTail] = text;
  telegramQueueTail = (telegramQueueTail + 1) % TELEGRAM_QUEUE_SIZE;
  telegramQueueCount++;
  Serial.println("[Telegram] Uzenet sorba helyezve. Varakozo: " + String(telegramQueueCount));
}

// ÚJ: Egyetlen üzenet tényleges elküldése (a loop()-ból hívva)
// Ez továbbra is blokkoló, de egyszerre csak 1 üzenetet küld.
void processTelegramQueue() {
  if (telegramQueueCount == 0) return;
  
  // Üzenet kivétele a sor elejéről
  String text = telegramQueue[telegramQueueHead];
  telegramQueueHead = (telegramQueueHead + 1) % TELEGRAM_QUEUE_SIZE;
  telegramQueueCount--;
  
  Serial.println("[Telegram] Memoria felszabaditasa: MQTT kapcsolat atmeneti bontasa...");
  client.disconnect(); 
  delay(100); 
  
  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();
  clientSecure.setBufferSizes(1024, 512); 
  
  Serial.println("[Telegram] Csatlakozas az api.telegram.org-hoz...");
  if (clientSecure.connect("api.telegram.org", 443)) {
    text.replace("\n", "\\n"); 
    
    String payload = "{\"chat_id\":\"" + chatId + "\", \"text\":\"" + text + "\"}";
    String request = String("POST /bot") + botToken + "/sendMessage HTTP/1.1\r\n" +
                     "Host: api.telegram.org\r\n" +
                     "Content-Type: application/json\r\n" +
                     "Content-Length: " + String(payload.length()) + "\r\n" +
                     "Connection: close\r\n\r\n" +
                     payload;
                     
    clientSecure.print(request);
    Serial.println("[Telegram] Keres elkuldve, valaszra varunk...");
    
    unsigned long timeout = millis();
    while (clientSecure.available() == 0) {
      if (millis() - timeout > 5000) {
         Serial.println("[Telegram] HIBA: Idotullepes (Timeout)!");
         clientSecure.stop();
         // Az MQTT reconnect a loop()-ban automatikusan megtörténik
         return;
      }
    }
    
    Serial.println("[Telegram] SIKERES KULDES!");
    clientSecure.stop();
  } else {
    Serial.println("[Telegram] KRITIKUS HIBA: TLS kapcsolat sikertelen!");
  }
  
  // Nem hívunk itt reconnect()-et — a loop() elején lévő
  // if (!client.connected()) reconnect(); automatikusan kezeli.
}

void OnDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  newDataAvailable = true;
  hasReceivedFirstData = true;
  client.publish("aquarium/health/nodeA", "ONLINE");
}

ICACHE_RAM_ATTR void handleCDInterrupt() {
  cdStateChanged = true;
}

String getFormattedDateTime() {
  timeClient.update();
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime); 
  char buffer[25];
  sprintf(buffer, "%04d.%02d.%02d %s", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, timeClient.getFormattedTime().c_str());
  return String(buffer);
}


void createDefaultFishCatalogOnSD() {
  if (!sdCardPresent) return;
  if (SD.exists(fishCatalogFileName)) return;

  File fishFile = SD.open(fishCatalogFileName, FILE_WRITE);
  if (!fishFile) {
    Serial.println("[FISH] Nem sikerult letrehozni a fish_catalog.csv fajlt!");
    return;
  }

  fishFile.println("hal_nev;hom_min_c;hom_max_c;ph_min;ph_max");
  fishFile.println("Neon tetra;20;26;6.0;7.0");
  fishFile.println("Cardinal tetra;23;29;5.0;7.0");
  fishFile.println("Guppy;22;28;7.0;8.5");
  fishFile.println("Platy;20;26;7.0;8.2");
  fishFile.println("Swordtail;22;28;7.0;8.0");
  fishFile.println("Molly;24;28;7.0;8.5");
  fishFile.println("Zebra danio;18;26;6.5;7.5");
  fishFile.println("Betta splendens;24;30;6.0;7.5");
  fishFile.println("Angelfish;24;30;6.0;7.4");
  fishFile.println("Discus;28;30;6.0;7.0");
  fishFile.close();

  Serial.println("[FISH] Alapertelmezett fish_catalog.csv letrehozva.");
}

void publishFishCatalog() {
  if (!client.connected()) return;

  if (!sdCardPresent) {
    client.publish("aquarium/fish/catalog", "ERROR_SD");
    return;
  }

  if (!SD.exists(fishCatalogFileName)) {
    createDefaultFishCatalogOnSD();
  }

  File fishFile = SD.open(fishCatalogFileName, FILE_READ);
  if (!fishFile) {
    client.publish("aquarium/fish/catalog", "ERROR_OPEN");
    return;
  }

  client.publish("aquarium/fish/catalog", "BEGIN");
  while (fishFile.available()) {
    String line = fishFile.readStringUntil('\n');
    line.trim();

    if (line.length() == 0 || line.startsWith("#")) continue;

    String firstCol = getValue(line, ';', 0);
    firstCol.trim();
    String firstColLower = firstCol;
    firstColLower.toLowerCase();

    if (firstColLower == "hal_nev" || firstColLower == "name") continue;

    String payload = "LINE;" + line;
    if (!client.publish("aquarium/fish/catalog", payload.c_str())) {
      Serial.println("[FISH] MQTT publish hiba a katalogus kuldese kozben!");
      fishFile.close();
      client.publish("aquarium/fish/catalog", "ERROR_MQTT");
      return;
    }

    delay(5);
    yield();
  }

  fishFile.close();
  client.publish("aquarium/fish/catalog", "END");
  Serial.println("[FISH] Hal-katalogus elkuldve MQTT-n.");
}


void loadSetupFromSD() {
  if (!sdCardPresent) return;
  if (SD.exists(setupFileName)) {
    File setupFile = SD.open(setupFileName, FILE_READ);
    if (setupFile) {
      String content = setupFile.readStringUntil('\n');
      content.trim(); 
      if (content.length() > 0) {
        activeSetpoint = getValue(content, ';', 0).toFloat();
        tempMin = getValue(content, ';', 1).toFloat();
        tempMax = getValue(content, ';', 2).toFloat();
        phMin = getValue(content, ';', 3).toFloat();
        phMax = getValue(content, ';', 4).toFloat();
        tdsMin = getValue(content, ';', 5).toFloat();
        tdsMax = getValue(content, ';', 6).toFloat();
        luxMin = getValue(content, ';', 7).toInt();
        luxMax = getValue(content, ';', 8).toInt();
        chatId = getValue(content, ';', 9);
      }
      setupFile.close();
    }
  } else {
    String defaultSettings = "25.0;20.0;28.0;6.0;8.0;0;1000;0;10000;0";
    File setupFile = SD.open(setupFileName, FILE_WRITE);
    if (setupFile) { 
      setupFile.println(defaultSettings);
      setupFile.close(); 
    }
    loadSetupFromSD(); 
  }
}

bool saveSetupToSD(String csvLine) {
  if (!sdCardPresent) return false;
  if (SD.exists(setupFileName)) SD.remove(setupFileName);
  File setupFile = SD.open(setupFileName, FILE_WRITE);
  if (setupFile) { 
    setupFile.println(csvLine); 
    setupFile.close(); 
    return true; 
  }
  return false;
}

String getSettingsString() {
  return String(activeSetpoint) + ";" + String(tempMin) + ";" + String(tempMax) + ";" +
         String(phMin) + ";" + String(phMax) + ";" + String(tdsMin) + ";" + String(tdsMax) + ";" +
         String(luxMin) + ";" + String(luxMax) + ";" + (chatId == "" ? "0" : chatId);
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
  createDefaultFishCatalogOnSD();
}

void logDataToSD(String timestamp, float temp, float ph, float tds, int lux, int kelvin, float r_pct, float g_pct, float b_pct) {
  if (!sdCardPresent) return;
  File logFile = SD.open(logFileName, FILE_WRITE);
  if (logFile) {
    logFile.print(timestamp); logFile.print(";");
    logFile.print(temp); logFile.print(";");
    logFile.print(ph); logFile.print(";");
    logFile.print(tds); logFile.print(";");
    logFile.print(lux); logFile.print(";");
    logFile.print(kelvin); logFile.print(";");
    logFile.print(r_pct); logFile.print(";");
    logFile.print(g_pct); logFile.print(";");
    logFile.println(b_pct);
    logFile.close();
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  if (String(topic) == "aquarium/settings/set") {
    if (saveSetupToSD(msg)) {
      loadSetupFromSD(); 
      client.publish("aquarium/settings/state", msg.c_str(), true); 
    } else {
      Serial.println("[HIBA] SD kartya nem irhato, mentes sikertelen!");
    }
  }
  else if (String(topic) == "aquarium/request" && msg == "CHECK") {
    client.publish("aquarium/settings/state", getSettingsString().c_str(), true);
    client.publish("aquarium/health/sdcard", sdCardPresent ? "OK" : "ERROR", true);
  }
  else if (String(topic) == "aquarium/request" && msg == "FISH_CATALOG") {
    fishCatalogRequestPending = true;
  }
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "NodeC_Gerinc_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Node C (Gerinc) MQTT SIKER!");
      client.subscribe("aquarium/settings/set"); 
      client.subscribe("aquarium/request"); 
      
      client.publish("aquarium/health/sdcard", sdCardPresent ? "OK" : "ERROR", true);
      client.publish("aquarium/settings/state", getSettingsString().c_str(), true);
    } else { delay(5000); }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(SD_CD_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SD_CD_PIN), handleCDInterrupt, CHANGE);
  
  WiFiManager wm;
  wm.autoConnect("AquaMaster_Gerinc");
  
  if (esp_now_init() != 0) {
    Serial.println("Hiba az ESP-NOW inicializalasakor!");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv); 
  
  timeClient.begin();
  
  espClient.setInsecure();
  client.setServer(mqtt_server, 8883);
  client.setCallback(callback);
  
  setupSDCard();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  // Indulási értesítés (sorba helyezve, nem blokkolóan)
  if (client.connected() && isFirstBoot) {
    isFirstBoot = false;
    queueTelegramMessage("🟢 AquaControl Rendszer elindult!\nA Gerinc (Node C) csatlakozott az MQTT szerverhez.");
  }

  // ÚJ: Telegram sor feldolgozása — iterációnként max. 1 üzenet
  processTelegramQueue();

  if (fishCatalogRequestPending) {
    fishCatalogRequestPending = false;
    publishFishCatalog();
  }

  // ESP-NOW adat fogadása
  if (newDataAvailable) {
    newDataAvailable = false; 
    logDataToSD(getFormattedDateTime(), receivedData.temp, receivedData.ph, receivedData.tds, receivedData.lux, receivedData.kelvin, receivedData.r_pct, receivedData.g_pct, receivedData.b_pct);
    
    if (client.connected()) {
      String payload = "{\"temp\":" + String(receivedData.temp, 2) + 
                       ",\"ph\":" + String(receivedData.ph, 2) + 
                       ",\"tds\":" + String(receivedData.tds, 0) + 
                       ",\"lux\":" + String(receivedData.lux) + 
                       ",\"kelvin\":" + String(receivedData.kelvin) + 
                       ",\"r_pct\":" + String(receivedData.r_pct, 1) + 
                       ",\"g_pct\":" + String(receivedData.g_pct, 1) + 
                       ",\"b_pct\":" + String(receivedData.b_pct, 1) + "}";
      client.publish("aquarium/sensor/data", payload.c_str());
    }
  }

  // 10 perces Telegram határérték ellenőrzés (sorba helyezve)
  if (hasReceivedFirstData && (millis() - lastTelegramCheck > 600000)) { 
    lastTelegramCheck = millis();
    
    String alertMsg = "";
    if (receivedData.temp < tempMin || receivedData.temp > tempMax) 
      alertMsg += "🌡 Homerseklet: " + String(receivedData.temp, 1) + " C (Hatar: " + String(tempMin, 1) + "-" + String(tempMax, 1) + ")\n";
    if (receivedData.ph < phMin || receivedData.ph > phMax) 
      alertMsg += "🧪 pH ertek: " + String(receivedData.ph, 1) + " (Hatar: " + String(phMin, 1) + "-" + String(phMax, 1) + ")\n";
    if (receivedData.tds < tdsMin || receivedData.tds > tdsMax) 
      alertMsg += "💧 TDS (Oldott anyag): " + String(receivedData.tds, 0) + " ppm (Hatar: " + String(tdsMin, 0) + "-" + String(tdsMax, 0) + ")\n";
    if (receivedData.lux < luxMin || receivedData.lux > luxMax) 
      alertMsg += "💡 Fenyero: " + String(receivedData.lux) + " lux (Hatar: " + String(luxMin) + "-" + String(luxMax) + ")\n";

    if (alertMsg != "") {
      String finalMsg = "⚠️ AKVARIUM RIASZTAS! ⚠️\n\nA kovetkezo ertekek atleptek a hatarerteket:\n\n" + alertMsg;
      queueTelegramMessage(finalMsg);
    }
  }

  // SD kártya be/ki figyelése (Telegram riasztás sorba helyezve)
  if (cdStateChanged) {
    if (millis() - lastCdDebounce > 200) { 
      lastCdDebounce = millis();
      cdStateChanged = false;
      bool isInserted = (digitalRead(SD_CD_PIN) == LOW);
      
      if (isInserted && !sdCardPresent) {
        setupSDCard(); 
        if (sdCardPresent) {
           queueTelegramMessage("💾 VISSZAÁLLÍTVA: SD kártya behelyezve és inicializálva. Az adatmentés folytatódik.");
        }
      } 
      else if (!isInserted && sdCardPresent) {
        sdCardPresent = false;
        client.publish("aquarium/health/sdcard", "ERROR", true);
        SD.end(); 
        queueTelegramMessage("⚠️ FIGYELEM: Az SD kártyát eltávolították! A lokális adatmentés jelenleg szünetel.");
      }
    }
  }

  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    client.publish("aquarium/health/nodeC", "ONLINE");
  }
}
