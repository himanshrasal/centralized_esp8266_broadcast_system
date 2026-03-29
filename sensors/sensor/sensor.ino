#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <cstring>

const char* ssid = "himanshrasal";
const char* passwd = "mangakeyou";

const char* serverUrl = "http://192.168.0.103:5000/data";

struct Sensor{
    char name[20];
    char type[10];
    bool isAnalog;
    uint8_t pin;
    float value;
    bool digitalInvert = false;

    unsigned long interval = 3000; // delay between sends
    unsigned long lastSent = 0; // last time sent

    Sensor(const char* name, const char* type, bool isAnalog, uint8_t pin, unsigned long interval){
      strncpy(this->name, name, sizeof(this->name)-1);
      this->name[sizeof(this->name)-1] = '\0';

      strncpy(this->type, type, sizeof(this->type)-1);
      this->type[sizeof(this->type)-1] = '\0';

      this->isAnalog = isAnalog;
      this->pin = pin;
      this->interval = interval;

      value = 0;
      lastSent = 0;
    }

    void init() {
      if (isAnalog) return;
      pinMode(pin, INPUT);
    }

    void setPullUp() {
      if (isAnalog) return;

      pinMode(pin, INPUT_PULLUP);
    }

    void setDigitalInversion(bool inversion){
      digitalInvert = inversion;
    }

    void readData(){
      if(isAnalog){
        if(pin != A0){
          value = 0;
          Serial.println("Wrong analog pin.");
          return;
        }
        value = analogRead(pin);
      }
      else{
        bool raw = digitalRead(pin);
        value = digitalInvert ? !raw : raw;
      }
    }

    bool ready() {
      return millis() - lastSent >= interval;
    }

    void sendData() {
      WiFiClient client;
      HTTPClient http;

      http.begin(client, serverUrl);
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(2000);

      String json = "{";
      json += "\"name\":\"" + String(this->name) + "\",";
      json += "\"type\":\"" + String(this->type) + "\",";
      json += "\"isAnalog\":" + String(this->isAnalog ? "true" : "false") + ",";
      json += "\"value\":" + String(this->value);
      json += "}";

      int code = http.POST(json);

      if(code > 0){
        Serial.println(code);
      }else {
        Serial.println("HTTP failed");
      }

      http.end();
    }

    void activate(){
      if(!ready()){
        return;
      }

      readData();

      if(WiFi.status() == WL_CONNECTED){
        sendData();
      }else {
        Serial.println("WiFi not connected.");
      }

      lastSent = millis();
    }
};

Sensor btn("button", "digital", false, D5, 1000);

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, passwd);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  Serial.println("Connected!");

  btn.init();
  btn.setPullUp();
  btn.setDigitalInversion(true);

}

void loop() {
  btn.activate();
}

