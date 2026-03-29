#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <cstring>

const char* ssid = "himanshrasal";
const char* passwd = "mangakeyou";

const char* serverUrl = "http://192.168.0.103:5000/data";

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, passwd);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  Serial.println("Connected!");
}

void loop() {
  // put your main code here, to run repeatedly:

}

struct Sensor{
    char name[20] = "default_node";
    char type[10] = "ping";
    bool isAnalog = 0;
    short pin = 16;
    float value = 0f;

    unsigned long interval = 3000; // delay between sends
    unsigned long lastSent = 0; // last time sent

    Sensor(const char* name, const char* type, bool isAnalog, int interval){
      strncpy(this->name, name, sizeof(this->name)-1);
      this->name[sizeof(this->name)-1] = '\0';

      strncpy(this->type, type, sizeof(this->type)-1);
      this->type[sizeof(this->type)-1] = '\0';

      this->isAnalog = isAnalog;

      this->interval = interval;
      lastSent = 0;

      pinMode(pin, INPUT);
    }

    void readData(){
      if(isAnalog){

      }
    }

    bool ready() {
      if (millis() - lastSent >= interval){
        lastSent = millis(); //update last sent timer
        return true;
      }
      return false;
    }
};

void sendData(Sensor* sensor) {
  WiFiClient client;
  HTTPClient http;

  http.begin(client, serverUrl);
  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"name\":\"" + String(sensor->name) + "\",";
  json += "\"type\":\"" + String(sensor->type) + "\",";
  json += "\"isAnalog\":\"" + String(sensor->isAnalog) + "\",";
  json += "\"value\":" + String(sensor->value);
  json += "}";

  int code = http.POST(json);

  Serial.println(code);

  http.end();
}