#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Servo.h>

const char* mqtt_server = "4769a5772b894e1ea634b4eff4b1c6cd.s1.eu.hivemq.cloud";
const char* mqtt_user = "akva_admin"; 
const char* mqtt_pass = "Akva123456";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// Hardveres lábak (WeMos D1)
const int RELAY_1 = D1; // Lámpa 1
const int RELAY_2 = D2; // Lámpa 2
const int SERVO_PIN = D5; // Etető szervó

Servo feederServo;
unsigned long lastHeartbeat = 0;

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == "aquarium/control") {
    // Relék (Active LOW - LOW szinten kapcsolnak be általában az SBX modulok)
    if (msg == "1") { digitalWrite(RELAY_1, HIGH); client.publish("aquarium/status/led1", "1", true); }
    if (msg == "0") { digitalWrite(RELAY_1, LOW); client.publish("aquarium/status/led1", "0", true); }
    if (msg == "2") { digitalWrite(RELAY_2, HIGH); client.publish("aquarium/status/led2", "1", true); }
    if (msg == "3") { digitalWrite(RELAY_2, LOW); client.publish("aquarium/status/led2", "0", true); }
    
    // Etető szervó ciklusa (msg == 4)
    if (msg == "4") {
      Serial.println("[NODE B] Etetes inditasa...");
      feederServo.attach(SERVO_PIN);
      feederServo.write(180); // Kinyit
      delay(1000);
      feederServo.write(0);   // Bezár
      delay(1000);
      feederServo.detach();   // Leválaszt, hogy ne zúgjon folyamatosan
    }
  }

  // Szinkronizáció kérésre
  if (String(topic) == "aquarium/request" && msg == "CHECK") {
    client.publish("aquarium/status/led1", digitalRead(RELAY_1) == LOW ? "1" : "0", true);
    client.publish("aquarium/status/led2", digitalRead(RELAY_2) == LOW ? "1" : "0", true);
  }
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "NodeB_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Node B MQTT Csatlakoztatva!");
      client.subscribe("aquarium/control");
      client.subscribe("aquarium/request");
    } else { delay(5000); }
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  digitalWrite(RELAY_1, HIGH); // Alapból kikapcsolva
  digitalWrite(RELAY_2, HIGH);

  WiFiManager wm;
  wm.autoConnect("AquaControl_AP");
  espClient.setInsecure();
  client.setServer(mqtt_server, 8883);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  // HEARTBEAT
  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    client.publish("aquarium/health/nodeB", "ONLINE");
  }
}
