#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <cstring>

const char *ssid = "himanshrasal";
const char *passwd = "mangakeyou";

const char *deviceName = "esp1";

const char *serverUrl = "http://192.168.0.103:5000/data";
const char *stateUrl = "http://192.168.0.103:5000/states";

// Timing variables
unsigned long lastStateFetch = 0;
unsigned long stateInterval = 2000; // 2 seconds
unsigned long lastFeedback = 0;
unsigned long feedbackInterval = 3000; // 3 seconds
unsigned long lastRegisterAttempt = 0;
bool registered = false;

// ================= SENSOR =================

struct Sensor {
  char id[20];
  char type[10];
  bool isAnalog;
  uint8_t pin;
  float value;
  bool digitalInvert = false;

  unsigned long interval = 3000;
  unsigned long lastSent = 0;

  Sensor(const char *id, const char *type, bool isAnalog, uint8_t pin,
         unsigned long interval) {
    strncpy(this->id, id, sizeof(this->id) - 1);
    this->id[sizeof(this->id) - 1] = '\0';

    strncpy(this->type, type, sizeof(this->type) - 1);
    this->type[sizeof(this->type) - 1] = '\0';

    this->isAnalog = isAnalog;
    this->pin = pin;
    this->interval = interval;

    value = 0;
    lastSent = 0;
  }

  void init() {
    if (isAnalog)
      return;
    pinMode(pin, INPUT);
  }

  void setPullUp() {
    if (isAnalog)
      return;
    pinMode(pin, INPUT_PULLUP);
  }

  void setDigitalInversion(bool inversion) { digitalInvert = inversion; }

  void readData() {
    if (isAnalog) {
      if (pin != A0) {
        value = 0;
        Serial.println("Wrong analog pin.");
        return;
      }
      value = analogRead(pin);
    } else {
      bool raw = digitalRead(pin);
      value = digitalInvert ? !raw : raw;
    }
  }

  bool ready() { return millis() - lastSent >= interval; }

  void sendData() {
    WiFiClient client;
    HTTPClient http;

    http.begin(client, serverUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(1000);

    String json = "{";
    json += "\"device\":\"" + String(deviceName) + "\",";
    json += "\"id\":\"" + String(this->id) + "\",";
    json += "\"type\":\"sensor\",";
    json += "\"sensorType\":\"" + String(this->type) + "\",";
    json += "\"isAnalog\":" + String(this->isAnalog ? "true" : "false") + ",";
    json += "\"value\":" + String(this->value);
    json += "}";

    int code = http.POST(json);
    http.end();

    if (code == 200) {
      Serial.println("Sensor sent");
    } else if (code > 0) {
      Serial.printf("Sensor HTTP: %d\n", code);
    }
  }

  void activate() {
    if (!ready())
      return;

    readData();

    if (WiFi.status() == WL_CONNECTED) {
      sendData();
    }

    lastSent = millis();
  }
};

// ================= OUTPUT =================
struct Output {
  char outputId[20];
  uint8_t pin;
  bool state;
  bool invert = false;
  bool isAlert = false;
  bool manualOverride = false; // NEW: Track if manually controlled
  unsigned long lastChange = 0;

  Output(const char *id, uint8_t pin, bool isAlert = false) {
    strncpy(this->outputId, id, sizeof(this->outputId) - 1);
    this->outputId[sizeof(this->outputId) - 1] = '\0';

    this->pin = pin;
    this->state = false;
    this->isAlert = isAlert;
    this->manualOverride = false;
    this->lastChange = 0;
  }

  void init() {
    pinMode(pin, OUTPUT);
    setState(false);
  }

  void setInversion(bool inv) { invert = inv; }

  void setState(bool newState, bool fromManual = false) {
    if (state == newState)
      return;

    state = newState;
    manualOverride = fromManual;

    bool outputLevel;
    if (invert) {
      outputLevel = !state;
    } else {
      outputLevel = state;
    }

    digitalWrite(pin, outputLevel ? HIGH : LOW);
    lastChange = millis();

    Serial.printf("%s: %s (manual=%d)\n", outputId, state ? "ON" : "OFF",
                  manualOverride);
  }

  void handleCommand(const String &incomingId, int value, bool fromWeb = true) {
    if (incomingId != String(this->outputId))
      return;

    // Don't allow rapid toggling
    if (millis() - lastChange < 100)
      return;

    // If this is from web and output is alert type, track manual override
    if (fromWeb && isAlert) {
      setState(value, true);
    }
    // If this is from rules (button) and no manual override, apply
    else if (!fromWeb && !manualOverride) {
      setState(value, false);
    }
    // If from rules but has manual override, ignore
    else if (!fromWeb && manualOverride) {
      Serial.printf("Ignoring rule for %s due to manual override\n", outputId);
    }
    // If from web and not alert type
    else if (fromWeb && !isAlert) {
      setState(value, true);
    }
  }

  void clearManualOverride() {
    manualOverride = false;
    Serial.printf("Cleared manual override for %s\n", outputId);
  }
};

// ================= OBJECTS =================

Sensor btn("button1", "button", false, D5, 1000);

Output outputs[] = {
    Output("buzzer1", D3, true), // Alert type
    Output("led", D4, false)     // Normal type
};

const int OUTPUT_COUNT = sizeof(outputs) / sizeof(outputs[0]);

// ================= RULE EVALUATION =================
void evaluateRules() {
  // Check button state and control buzzer
  if (btn.value == 1) {
    // Button pressed - trigger buzzer if no manual override
    outputs[0].handleCommand("buzzer1", 1, false); // from rule
  } else {
    // Button released - turn off buzzer if no manual override
    outputs[0].handleCommand("buzzer1", 0, false); // from rule
  }
}

// ================= OUTPUT POLLING =================

void checkOutputs() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClient client;
  HTTPClient http;

  String url = String(stateUrl) + "?device=" + deviceName;

  http.begin(client, url);
  http.setTimeout(1000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();

    // Use small buffer to save memory
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      JsonArray arr = doc.as<JsonArray>();

      for (JsonObject o : arr) {
        String id = o["id"];
        int value = o["value"];

        for (int i = 0; i < OUTPUT_COUNT; i++) {
          outputs[i].handleCommand(id, value, true); // from web
        }
      }
    } else {
      Serial.print("JSON error: ");
      Serial.println(err.c_str());
    }
  }

  http.end();
}

// ================= FEEDBACK =================

void sendOutputStates() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClient client;
  HTTPClient http;

  http.begin(client, serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1000);

  // Use smaller JSON
  String json = "{";
  json += "\"device\":\"" + String(deviceName) + "\",";
  json += "\"type\":\"output_state\",";
  json += "\"outputs\":[";

  for (int i = 0; i < OUTPUT_COUNT; i++) {
    json += "{";
    json += "\"id\":\"" + String(outputs[i].outputId) + "\",";
    json += "\"value\":" + String(outputs[i].state) + ",";
    json += "\"isAlert\":" + String(outputs[i].isAlert ? "true" : "false");
    json += "}";

    if (i != OUTPUT_COUNT - 1)
      json += ",";
  }

  json += "]}";

  int code = http.POST(json);
  http.end();

  if (code == 200) {
    // Success, no debug
  }
}

void registerOutputs() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClient client;
  HTTPClient http;

  http.begin(client, "http://192.168.0.103:5000/register_outputs");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(1000);

  String json = "{";
  json += "\"device\":\"" + String(deviceName) + "\",";
  json += "\"outputs\":[";

  for (int i = 0; i < OUTPUT_COUNT; i++) {
    json += "{";
    json += "\"id\":\"" + String(outputs[i].outputId) + "\",";
    json += "\"isAlert\":" + String(outputs[i].isAlert ? "true" : "false");
    json += "}";

    if (i != OUTPUT_COUNT - 1)
      json += ",";
  }

  json += "]}";

  int code = http.POST(json);
  http.end();

  if (code == 200) {
    registered = true;
    Serial.println("Registered outputs successfully");
  } else {
    Serial.printf("Register failed: %d\n", code);
  }
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  delay(10);

  Serial.println("\n\nESP Starting...");

  // Setup WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, passwd);

  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    delay(1000); // Wait for network stability
    registerOutputs();
  } else {
    Serial.println("\nWiFi connection failed!");
  }

  // Initialize sensors
  btn.init();
  btn.setPullUp();
  btn.setDigitalInversion(true);

  // Initialize outputs
  for (int i = 0; i < OUTPUT_COUNT; i++) {
    outputs[i].init();
  }

  outputs[0].setInversion(true); // buzzer (active LOW)
  outputs[1].setInversion(true); // led (active LOW)

  Serial.println("Setup complete!");
}

// ================= LOOP =================

void loop() {
  // Handle WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000) { // Try reconnect every 10 sec
      Serial.println("WiFi lost, reconnecting...");
      WiFi.begin(ssid, passwd);
      lastReconnect = millis();
    }
    delay(100);
    return;
  }

  // Register if not registered yet
  if (!registered && millis() - lastRegisterAttempt > 10000) {
    registerOutputs();
    lastRegisterAttempt = millis();
  }

  // Read button and evaluate rules
  btn.activate();
  evaluateRules(); // This will trigger buzzer based on button

  // Poll for web commands
  if (millis() - lastStateFetch > stateInterval) {
    checkOutputs();
    lastStateFetch = millis();
  }

  // Send feedback to server
  if (millis() - lastFeedback > feedbackInterval) {
    sendOutputStates();
    lastFeedback = millis();
  }

  // Small delay to prevent watchdog
  delay(10);
}