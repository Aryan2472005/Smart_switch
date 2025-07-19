#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <Ticker.h>

// ----- CONFIG -----
ESP8266WebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wifiManager;
Ticker touchTicker;

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

const int ledPins[4] = { D5, D6, D3, D4 };
const int touchPins[4] = { D1, D2, D7, D8 };
bool ledStates[4] = { false, false, false, false };
bool lastTouchStates[4] = { true, true, true, true };
bool topicProcessed[4] = { false, false, false, false };

#define EEPROM_SIZE 16

unsigned long mqttConnectTime = 0;
unsigned long lastMQTTAttempt = 0;
unsigned long lastIpPublish = 0;
bool skipInitialMQTT = true;
bool wifiConnected = false;
unsigned long wifiStartAttemptTime = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 10 * 60 * 1000; // 10 minutes

// ----- FUNCTIONS -----
void saveLEDStates() {
  for (int i = 0; i < 4; i++) EEPROM.write(i, ledStates[i] ? 1 : 0);
  EEPROM.write(15, 0xAB);
  EEPROM.commit();
}

void loadLEDStates() {
  if (EEPROM.read(15) != 0xAB) {
    for (int i = 0; i < 4; i++) {
      EEPROM.write(i, 0);
      ledStates[i] = false;
      digitalWrite(ledPins[i], HIGH);
    }
    EEPROM.write(15, 0xAB);
    EEPROM.commit();
  } else {
    for (int i = 0; i < 4; i++) {
      uint8_t val = EEPROM.read(i);
      ledStates[i] = (val == 1);
      digitalWrite(ledPins[i], ledStates[i] ? LOW : HIGH);
    }
  }
}

void handleTouchInputs() {
  for (int i = 0; i < 4; i++) {
    bool touchNow = digitalRead(touchPins[i]);
    if (lastTouchStates[i] == HIGH && touchNow == LOW) {
      ledStates[i] = !ledStates[i];
      digitalWrite(ledPins[i], ledStates[i] ? LOW : HIGH);
      if (client.connected()) {
        String topic = "syphon/led/control/" + String(i + 1);
        String message = ledStates[i] ? "ON" : "OFF";
        client.publish(topic.c_str(), message.c_str(), true);
      }
      saveLEDStates();
    }
    lastTouchStates[i] = touchNow;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  for (int i = 0; i < 4; i++) {
    String expectedTopic = "syphon/led/control/" + String(i + 1);
    if (topicStr == expectedTopic) {
      if (skipInitialMQTT && millis() - mqttConnectTime < 2000) {
        if (!topicProcessed[i]) topicProcessed[i] = true;
        return;
      }
      bool newState = (message == "ON");
      if (ledStates[i] != newState) {
        ledStates[i] = newState;
        digitalWrite(ledPins[i], newState ? LOW : HIGH);
        saveLEDStates();
      }
    }
  }
}

void reconnectMQTT() {
  if (!client.connected() && millis() - lastMQTTAttempt > 5000) {
    lastMQTTAttempt = millis();
    String clientId = "NodeMCU-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      mqttConnectTime = millis();
      skipInitialMQTT = true;
      memset(topicProcessed, 0, sizeof(topicProcessed));

      for (int i = 0; i < 4; i++) {
        String topic = "syphon/led/control/" + String(i + 1);
        client.subscribe(topic.c_str());
        client.publish(topic.c_str(), ledStates[i] ? "ON" : "OFF", true);
      }
      client.publish("syphon/esp/ip", WiFi.localIP().toString().c_str(), true);
    }
  }
}

void tryConnectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  wifiStartAttemptTime = millis();
}

void startConfigPortal() {
  WiFi.disconnect(true);
  delay(500);
  wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  wifiManager.setBreakAfterConfig(true);
  wifiManager.setConfigPortalTimeout(0);
  wifiManager.startConfigPortal("Smart Home");
}

void setup() {
  Serial.begin(9600);
  EEPROM.begin(EEPROM_SIZE);

  for (int i = 0; i < 4; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], HIGH);
    pinMode(touchPins[i], INPUT_PULLUP);
  }

  loadLEDStates();
  touchTicker.attach_ms(50, handleTouchInputs);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  if (WiFi.SSID().length() == 0) {
    startConfigPortal();
  } else {
    tryConnectWiFi();
  }


  server.on("/reset_wifi", []() {
    server.send(200, "text/plain", "Resetting WiFi...");
    delay(500);
    wifiManager.resetSettings();
    ESP.restart();
  });

  server.on("/get_ip", []() {
    server.send(200, "text/plain", WiFi.localIP().toString());
  });

  server.on("/status", []() {
    String json = "{";
    for (int i = 0; i < 4; i++) {
      json += "\"led" + String(i + 1) + "\":" + (ledStates[i] ? "true" : "false");
      if (i < 3) json += ",";
    }
    json += "}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  server.begin();
}

void loop() {
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      reconnectMQTT();
    }
    if (!client.connected()) reconnectMQTT();
    client.loop();
    if (skipInitialMQTT && millis() - mqttConnectTime > 2000) skipInitialMQTT = false;
    if (millis() - lastIpPublish > 10000) {
      lastIpPublish = millis();
      client.publish("syphon/esp/ip", WiFi.localIP().toString().c_str(), true);
    }
  } else {
    if (millis() - wifiStartAttemptTime > WIFI_CONNECT_TIMEOUT) {
      startConfigPortal();
      wifiStartAttemptTime = millis(); // reset after portal
    }
    wifiManager.process();
  }
}
