#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <ESP8266HTTPClient.h>
#include <Ticker.h>
#include <time.h>
#include <ArduinoOTA.h>


#define PIN_LED 16 //3
#define PIN_RELAY 12
#define PIN_BUTTON 0
#define SLACK_SSL_FINGERPRINT "AB F0 5B A9 1A E0 AE 5F CE 32 2E 7C 66 67 49 EC DD 6D 6A 38" // If Slack changes their SSL fingerprint, you would need to update this
#define SLACK_BOT_TOKEN "your-slack-token-here" // Get token by creating new bot integration at https://my.slack.com/services/new/bot 
#define OTA_PASS "iotmaker"
#define OTA_PORT (8266)

#define LED_ON() digitalWrite(PIN_LED, LOW)
#define LED_OFF() digitalWrite(PIN_LED, HIGH)
#define LED_TOGGLE() digitalWrite(PIN_LED, digitalRead(PIN_LED) ^ 0x01)
#define RELAY_ON() digitalWrite(PIN_RELAY, LOW)
#define RELAY_OFF() digitalWrite(PIN_RELAY, LOW)

enum PLUG_STATE {
  ESP_INIT,
  ESP_CONNECTED_WIFI,
  ESP_CONNECTED_SLACK,
  ESP_SMARTCONFIG,
  ESP_OTA
} plug_state;

Ticker btnTimer;
WebSocketsClient webSocket;
bool ws_connected = false;
unsigned long lastPing = 0, lastSmart = 0, lastOta = 0;
long nextCmdId = 1;

bool longPress()
{
  static int lastPress = 0;
  if (millis() - lastPress > 3000 && digitalRead(PIN_BUTTON) == 0) {
    return true;
  } else if(digitalRead(PIN_BUTTON) == 1){
    lastPress = millis();
  }
  return false;
}

void processSlackMessage(char *payload) {
  if (strstr(payload, "\"on\"") != NULL) {
    Serial.printf("ON NEW OTA\r\n");
    LED_ON();
  } else if (strstr(payload, "\"off\"") != NULL) {
    Serial.printf("OFF NEW OTA\r\n");
    LED_OFF();
  }
}
/**
  Called on each web socket event. Handles disconnection, and also
  incoming messages from slack.
*/
void webSocketEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WebSocket] Disconnected :-( \n");
      ws_connected = false;
      delay(5000);
      break;

    case WStype_CONNECTED:
      Serial.printf("[WebSocket] Connected to: %s\r\n", payload);
      //sendPing();
      break;

    case WStype_TEXT:
      //      Serial.printf("[WebSocket] Message: %s\r\n", payload);
      processSlackMessage((char*)payload);
      break;
  }
}
/**
  Establishes a bot connection to Slack:
  1. Performs a REST call to get the WebSocket URL
  2. Conencts the WebSocket
  Returns true if the connection was established successfully.
*/
bool connectToSlack() {
  // Step 1: Find WebSocket address via RTM API (https://api.slack.com/methods/rtm.start)
  HTTPClient http;
  http.begin("https://slack.com/api/rtm.start?token="SLACK_BOT_TOKEN, SLACK_SSL_FINGERPRINT);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed with code %d\r\n", httpCode);
    return false;
  }

  WiFiClient *client = http.getStreamPtr();
  client->find("wss:\\/\\/");
  String host = client->readStringUntil('\\');
  String path = client->readStringUntil('"');
  path.replace("\\/", "/");

  // Step 2: Open WebSocket connection and register event handler
  Serial.println("WebSocket Host=" + host + " Path=" + path);
  webSocket.beginSSL(host, 443, path, "", "");
  webSocket.onEvent(webSocketEvent);
  return true;
}

/**
  Sends a ping message to Slack. Call this function immediately after establishing
  the WebSocket connection, and then every 5 seconds to keep the connection alive.
*/
void sendPing() {
  char pingstr[128];
  String pstr = String("{\"type\":\"ping\",\"id\":") + String(nextCmdId++) + String("}");
  webSocket.sendTXT(pstr);
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  // Cấu hình GPIO
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  LED_OFF();
  RELAY_OFF();
  plug_state = ESP_INIT;
  Serial.printf("Starting...\r\n");
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  ArduinoOTA.onStart([]() {
    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating \r\n");
    lastOta = millis();
    plug_state = ESP_OTA;
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();

}

void loop() {
  if (longPress() && plug_state != ESP_SMARTCONFIG) {
    LED_ON();
    WiFi.beginSmartConfig();
    lastSmart = millis();
    plug_state = ESP_SMARTCONFIG;
    Serial.printf("Startsmart\r\n");
  }
  switch (plug_state) {
    case ESP_INIT:
      Serial.printf("ESP_INIT: %d...\r\n", WiFi.status());
      if (WiFi.status() != WL_CONNECTED) {
        delay(500);
        LED_TOGGLE();
        break;
      }
      LED_ON();
      plug_state = ESP_CONNECTED_WIFI;
      configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
      while (1) {
        time_t now = time(nullptr);
        if (now > (2016 - 1970) * 365 * 24 * 3600) {
          break;
        }
        delay(100);
        Serial.printf("Wait ntp: %d...\r\n", now);
      }

      break;
    case ESP_CONNECTED_WIFI:
      Serial.printf("ESP_CONNECTED_WIFI: %d...\r\n", WiFi.status());
      if (WiFi.status() != WL_CONNECTED) {
        plug_state = ESP_INIT;
        break;
      }
      ws_connected = connectToSlack();
      if (ws_connected) {
        plug_state = ESP_CONNECTED_SLACK;
        LED_ON();
        break;
      }
      LED_TOGGLE();
      delay(5000);

      break;
    case ESP_CONNECTED_SLACK:
      if (WiFi.status() != WL_CONNECTED) {
        plug_state = ESP_INIT;
        break;
      }
      webSocket.loop();
      ArduinoOTA.handle();
      if (!ws_connected) {
        plug_state = ESP_CONNECTED_WIFI;
        break;
      }
      if (millis() - lastPing > 5000) {
        sendPing();
        lastPing = millis();
      }
      break;
    case ESP_SMARTCONFIG:
      if (WiFi.smartConfigDone()) {
        plug_state = ESP_INIT;
        break;
      }
      if (millis() - lastSmart > 60000) { //timeout 60sec
        WiFi.stopSmartConfig();
        plug_state = ESP_INIT;
        break;
      }
      delay(100);
      LED_TOGGLE();
      break;
    case ESP_OTA:
      ArduinoOTA.handle();
      if (millis() - lastOta > 60000) { //timeout 60sec
        plug_state = ESP_INIT;
        break;
      }

  }
}

