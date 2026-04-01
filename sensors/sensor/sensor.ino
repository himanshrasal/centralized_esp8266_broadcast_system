#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <cstring>
#include <ArduinoJson.h>

const char* ssid       = "himanshrasal";
const char* passwd     = "mangakeyou";
const char* deviceName = "esp_motion";

const char* serverUrl   = "http://192.168.0.103:5000/data";
const char* stateUrl    = "http://192.168.0.103:5000/states";
const char* registerUrl = "http://192.168.0.103:5000/register_outputs";

unsigned long lastStateFetch      = 0;
const unsigned long stateInterval = 2000;
unsigned long lastFeedback        = 0;
const unsigned long feedbackInterval = 3000;
unsigned long lastRegisterAttempt = 0;
bool registered = false;

// =================================================================
// SENSOR CONFIG
// Add/remove sensors here. One entry = one sensor.
// Fields: { id, sensorType, isAnalog, pin, intervalMs, usePullUp, invertDigital }
// =================================================================

struct SensorConfig {
    const char* id;
    const char* sensorType;
    bool        isAnalog;
    uint8_t     pin;
    unsigned long intervalMs;
    bool        usePullUp;
    bool        invertDigital;
};

static const SensorConfig SENSOR_CONFIGS[] = {
    // id          type      analog  pin  interval  pullup  invert
    // { "motion",  "light",  true,   D8,  2000,     false,  false },
    { "pir",  "motion", false,  D8,  1000,       false,  false },
};

static const int SENSOR_COUNT = sizeof(SENSOR_CONFIGS) / sizeof(SENSOR_CONFIGS[0]);

// =================================================================
// OUTPUT CONFIG
// Add/remove outputs here. One entry = one output.
//
// isAlert = true  -> driven by LOCAL RULES only (web UI cannot override)
// isAlert = false -> driven by WEB UI only (rules cannot override)
//
// Fields: { id, pin, isAlert, invertPin }
// =================================================================

struct OutputConfig {
    const char* id;
    uint8_t     pin;
    bool        isAlert;
    bool        invertPin;
};

static const OutputConfig OUTPUT_CONFIGS[] = {
    // id          pin  alert  invert
    { "buzzer",  D5,  true,  false  },   // alert  - rule-driven, active-LOW
    // { "led1",      D2,  false, true  },   // normal - web-driven,  active-LOW
    // { "alarm2",D8,  true,  false },
};

static const int OUTPUT_COUNT = sizeof(OUTPUT_CONFIGS) / sizeof(OUTPUT_CONFIGS[0]);

// =================================================================
// RULES TABLE
// Maps sensor readings to output states automatically.
// Only alert outputs should appear here (normal outputs are web-only).
//
// Fields: { sensorId, outputId, triggerValue, outputValue }
// =================================================================

struct Rule {
    const char* sensorId;
    const char* outputId;
    float       triggerValue;
    int         outputValue;
};

static const Rule RULES[] = {
    // sensorId    outputId   trigger  output
    { "pir",  "buzzer",   1.0f,   1 },
    { "pir",  "buzzer",   0.0f,   0 },
};

static const int RULE_COUNT = sizeof(RULES) / sizeof(RULES[0]);

// =================================================================
// SENSOR CLASS
// =================================================================

struct Sensor {
    char          id[20];
    char          type[16];
    bool          isAnalog;
    uint8_t       pin;
    unsigned long interval;
    bool          usePullUp;
    bool          digitalInvert;
    float         value;
    unsigned long lastSent;

    void init(const SensorConfig& cfg) {
        strncpy(id,   cfg.id,         sizeof(id)   - 1); id[sizeof(id)-1]     = '\0';
        strncpy(type, cfg.sensorType, sizeof(type) - 1); type[sizeof(type)-1] = '\0';
        isAnalog      = cfg.isAnalog;
        pin           = cfg.pin;
        interval      = cfg.intervalMs;
        usePullUp     = cfg.usePullUp;
        digitalInvert = cfg.invertDigital;
        value         = 0;
        lastSent      = 0;

        if (!isAnalog) {
            if (usePullUp) pinMode(pin, INPUT_PULLUP);
            else           pinMode(pin, INPUT);
        }
    }

    void readData() {
        if (isAnalog) {
            value = (pin == A0) ? analogRead(pin) : 0;
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
        http.setTimeout(1500);

        char json[256];
        snprintf(json, sizeof(json),
            "{\"device\":\"%s\",\"id\":\"%s\",\"type\":\"sensor\","
            "\"sensorType\":\"%s\",\"isAnalog\":%s,\"value\":%.2f}",
            deviceName, id, type,
            isAnalog ? "true" : "false",
            value);

        int code = http.POST(json);
        http.end();
        if (code == 200) Serial.printf("Sensor [%s] sent: %.2f\n", id, value);
        else if (code > 0) Serial.printf("Sensor [%s] HTTP: %d\n", id, code);
    }

    void activate() {
        if (!ready()) return;
        readData();
        if (WiFi.status() == WL_CONNECTED) sendData();
        lastSent = millis();
    }
};

// =================================================================
// OUTPUT CLASS
// =================================================================

struct Output {
    char          outputId[20];
    uint8_t       pin;
    bool          isAlert;
    bool          invert;
    bool          state;
    unsigned long lastChange;

    void init(const OutputConfig& cfg) {
        strncpy(outputId, cfg.id, sizeof(outputId) - 1); outputId[sizeof(outputId)-1] = '\0';
        pin         = cfg.pin;
        isAlert     = cfg.isAlert;
        invert      = cfg.invertPin;
        state       = false;
        lastChange  = 0;
        pinMode(pin, OUTPUT);
        applyPin(false);
    }

    void applyPin(bool newState) {
        digitalWrite(pin, (newState ^ invert) ? HIGH : LOW);
    }

    void handleCommand(const char* incomingId, int value, bool fromWeb) {
        if (strcmp(incomingId, outputId) != 0) return;
        if (millis() - lastChange < 100) return;

        if (isAlert  &&  fromWeb) return;
        if (!isAlert && !fromWeb) return;

        bool newState = (value != 0);
        if (state == newState) return;

        state = newState;
        applyPin(state);
        lastChange = millis();
        Serial.printf("Output [%s]: %s\n", outputId, state ? "ON" : "OFF");
    }
};

// =================================================================
// RUNTIME INSTANCES
// =================================================================

static Sensor sensors[SENSOR_COUNT];
static Output outputs[OUTPUT_COUNT];

// =================================================================
// RULE ENGINE
// =================================================================

void evaluateRules() {
    for (int r = 0; r < RULE_COUNT; r++) {
        const Rule& rule = RULES[r];
        for (int s = 0; s < SENSOR_COUNT; s++) {
            if (strcmp(sensors[s].id, rule.sensorId) != 0) continue;
            if (sensors[s].value == rule.triggerValue) {
                for (int o = 0; o < OUTPUT_COUNT; o++) {
                    outputs[o].handleCommand(rule.outputId, rule.outputValue, false);
                }
            }
        }
    }
}

// =================================================================
// OUTPUT POLLING  (web -> ESP)
// =================================================================

void checkOutputs() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClient client;
    HTTPClient http;

    char url[80];
    snprintf(url, sizeof(url), "%s?device=%s", stateUrl, deviceName);
    http.begin(client, url);
    http.setTimeout(1500);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();

        StaticJsonDocument<1024> doc;
        DeserializationError err = deserializeJson(doc, payload);

        if (!err) {
            JsonArray arr = doc.as<JsonArray>();
            for (JsonObject o : arr) {
                const char* id = o["id"].as<const char*>();
                int         val = o["value"].as<int>();
                for (int i = 0; i < OUTPUT_COUNT; i++) {
                    outputs[i].handleCommand(id, val, true);
                }
            }
        } else {
            Serial.print("JSON err: "); Serial.println(err.c_str());
        }
    }
    http.end();
}

// =================================================================
// FEEDBACK  (ESP -> server)
// =================================================================

void sendOutputStates() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClient client;
    HTTPClient http;
    http.begin(client, serverUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(1500);

    char json[512];
    int pos = snprintf(json, sizeof(json),
        "{\"device\":\"%s\",\"type\":\"output_state\",\"outputs\":[", deviceName);

    for (int i = 0; i < OUTPUT_COUNT; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"id\":\"%s\",\"value\":%d,\"isAlert\":%s}%s",
            outputs[i].outputId,
            outputs[i].state ? 1 : 0,
            outputs[i].isAlert ? "true" : "false",
            i < OUTPUT_COUNT - 1 ? "," : "");
    }
    snprintf(json + pos, sizeof(json) - pos, "]}");

    int code = http.POST(json);
    http.end();
    if (code != 200 && code > 0) Serial.printf("Feedback HTTP: %d\n", code);
}

// =================================================================
// REGISTER OUTPUTS
// =================================================================

void registerOutputs() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClient client;
    HTTPClient http;
    http.begin(client, registerUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(2000);

    char json[256];
    int pos = snprintf(json, sizeof(json),
        "{\"device\":\"%s\",\"outputs\":[", deviceName);

    for (int i = 0; i < OUTPUT_COUNT; i++) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"id\":\"%s\",\"isAlert\":%s}%s",
            outputs[i].outputId,
            outputs[i].isAlert ? "true" : "false",
            i < OUTPUT_COUNT - 1 ? "," : "");
    }
    snprintf(json + pos, sizeof(json) - pos, "]}");

    int code = http.POST(json);
    http.end();

    if (code == 200) { registered = true; Serial.println("Registered outputs OK"); }
    else              { Serial.printf("Register failed: %d\n", code); }
}

// =================================================================
// SETUP
// =================================================================

void setup() {
    Serial.begin(115200);
    delay(10);
    Serial.println("\nESP Starting...");

    for (int i = 0; i < SENSOR_COUNT; i++) sensors[i].init(SENSOR_CONFIGS[i]);
    for (int i = 0; i < OUTPUT_COUNT; i++) outputs[i].init(OUTPUT_CONFIGS[i]);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, passwd);

    Serial.print("Connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500); Serial.print("."); attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi OK! IP: " + WiFi.localIP().toString());
        delay(500);
        registerOutputs();
    } else {
        Serial.println("\nWiFi failed!");
    }

    Serial.printf("Loaded %d sensor(s), %d output(s), %d rule(s)\n",
                  SENSOR_COUNT, OUTPUT_COUNT, RULE_COUNT);
    Serial.println("Setup done!");
}

// =================================================================
// LOOP
// =================================================================

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastRecon = 0;
        if (millis() - lastRecon > 10000) {
            Serial.println("Reconnecting WiFi...");
            WiFi.begin(ssid, passwd);
            lastRecon = millis();
        }
        delay(100);
        return;
    }

    if (!registered && millis() - lastRegisterAttempt > 10000) {
        registerOutputs();
        lastRegisterAttempt = millis();
    }

    for (int i = 0; i < SENSOR_COUNT; i++) sensors[i].activate();

    evaluateRules();

    if (millis() - lastStateFetch > stateInterval) {
        checkOutputs();
        lastStateFetch = millis();
    }

    if (millis() - lastFeedback > feedbackInterval) {
        sendOutputStates();
        lastFeedback = millis();
    }

    delay(10);
}