#include <Stepper_28BYJ_48.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include "FS.h"

//#define mqtt_server "192.168.2.195" //MQTT server IP
//#define mqtt_port 1883              //MQTT server port

//WIFI and MQTT
WiFiClient espClient;
char mqtt_server[40];
char mqtt_port[6] = "1883";
bool shouldSaveConfig = false;
PubSubClient client(espClient);
int ledPin = 2;                     //PIN used for the onboard led

String action;                      //Action manual/auto
int path = 0;                       //Direction of blind (1 = down, 0 = stop, -1 = up)

//MQTT topics
const char* mqttclientid;         //Generated MQTT client id

//Stored data
long currentPosition = 0;           //Current position of the blind
long maxPosition = 2000000;         //Max position of the blind
boolean loadDataSuccess = false;
boolean saveItNow = false;          //If true will store positions to SPIFFS


Stepper_28BYJ_48 small_stepper(D1, D3, D2, D4);

/**
 * Loading configuration that has been saved on SPIFFS.
 * Returns false if not successful
 */
bool loadConfig() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }
  json.printTo(Serial);
  Serial.println();

  //Store variables locally
  currentPosition = long(json["currentPosition"]);
  maxPosition = long(json["maxPosition"]);
  strcpy(mqtt_server, json["mqtt_server"]);
  strcpy(mqtt_port, json["mqtt_port"]);

  return true;
}

bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition"] = currentPosition;
  json["maxPosition"] = maxPosition;
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  json.printTo(configFile);

  Serial.println("Saved JSON to SPIFFS");
  json.printTo(Serial);
  Serial.println();
  return true;
}

/*
 * Setup WIFI connection and connect the MQTT client to the
 * MQTT server
 */
void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(mqttclientid)) {
      Serial.println("connected");

      //Send register MQTT message with JSON of chipid and ip-address
      sendmsg("/raw/esp8266/register","{ \"id\": \""+String(ESP.getChipId())+"\", \"ip\":"+WiFi.localIP().toString()+"\"}");

      //Setup subscription
      client.subscribe(("/raw/esp8266/"+String(ESP.getChipId())+"/in").c_str());

    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      ESP.wdtFeed();
      delay(5000);
    }
  }
}

/*
 * Blink the onboard led
 */
void qblink(){
    digitalWrite(ledPin, LOW); //turn on led
    delay(50);
    digitalWrite(ledPin, HIGH);
    delay(50);
}

/*
 * Common function to turn on WIFI radio, connect and get an IP address,
 * connect to MQTT server and publish a message on the bus.
 * Finally, close down the connection and radio
 */
void sendmsg(String topic, String payload){
    //Blink
    qblink();

    //Send status to MQTT bus if connected
    if (client.connected()){
      client.publish(topic.c_str(), payload.c_str());
      Serial.println("Published MQTT message");
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  /*
   * Possible input
   * - start. Will set existing position as 0
   * - max Will set the max position
   * - -1 / 0 / 1 . Will steer the blinds up/stop/down
   */
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String res ="";
  for (int i=0;i<length;i++) {
    res+= String((char)payload[i]);
  }

  /*
   * Check if calibration is running and if stop is received. Store the location
   */
  if (action == "set" && res == "0"){
    maxPosition = currentPosition;
    saveItNow = true;
  }

  /*
   * Below are actions based on inbound MQTT payload
   */
  if (res == "start"){
    /*
     * Store the current position as the start position
     */
    currentPosition = 0;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "max"){
    /*
     * Store the max position of a closed blind
     */
    maxPosition = currentPosition;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "0"){
    /*
     * Stop
     */
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "1"){
    /*
     * Move down without limit to max position
     */
    path = 1;
    action = "manual";
  } else if (res == "-1"){
    /*
     * Move up without limit to top position
     */
    path = -1;
    action = "manual";
  } else if (res == "close"){
    /*
     * Move down the blind and stop automatically when
     * max position is reached
     */
    path = 1;
    action = "auto";
  } else if (res == "open"){
    /*
     * Move up the blind and stop automatically when
     * top position is reached
     */
    path = -1;
    action = "auto";
  } else {
    /*
     * Any other message will stop the blind
     */
    path = 0;
  }

  Serial.println(res);
  Serial.println();
}

/*
 * Callback from WIFI Manager for saving configuration
 */
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.print("Starting now\n");

  action = "";

  //Setup MQTT Client ID
  mqttclientid = ("ESPClient-"+String(ESP.getChipId())).c_str();
  Serial.print("MQTT Client ID: ");
  Serial.println(mqttclientid);

  //Define customer parameters for WIFI Manager
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);

  //Setup WIFI Manager
  WiFiManager wifiManager;

  //reset settings - for testing
  //clean FS, for testing
  //SPIFFS.format();
  //wifiManager.resetSettings();

  //Make sure params gets saved
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);

  wifiManager.autoConnect("BlindsConfigAP", "nidayand");

  //Load config upon start
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }


  /* Save the config back from WIFI Manager.
   *  This is only called after configuration
   */
  if (shouldSaveConfig) {
    //read updated parameters
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());

    //Save the data
    saveConfig();
  }

  /* Setup connection for MQTT and for subscribed
   *  messages
   */
  client.setServer(mqtt_server, int(mqtt_port));
  client.setCallback(mqttCallback);

  /*
   * Try to load FS data configuration every time when
   * booting up. If loading does not work, set the default
   * positions
   */
  loadDataSuccess = loadConfig();
  if (!loadDataSuccess){
    currentPosition = 0;
    maxPosition = 2000000;
  }

  //Setup OTA
  {

    // Authentication to avoid unauthorized updates
    //ArduinoOTA.setPassword((const char *)"nidayand");

    ArduinoOTA.onStart([]() {
      Serial.println("Start");
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
    ArduinoOTA.begin();
  }
}

void loop(){
  //OTA client code
  ArduinoOTA.handle();

  if (!client.connected()) {
    reconnect();
  } else {
    client.loop();

    if (saveItNow){
      saveConfig();
      saveItNow = false;
    }

    if (action == "auto"){
      /*
       * Automatically open or close blind
       */
      switch (path) {
        case -1:
            if (currentPosition > 0){
              small_stepper.step(path);
              currentPosition = currentPosition + path;
            } else {
              path = 0;
              Serial.println("Stopped. Reached top position");
              saveItNow = true;
            }
            break;
        case 1:
          if (currentPosition < maxPosition){
              small_stepper.step(path);
              currentPosition = currentPosition + path;
          } else {
            path = 0;
            Serial.println("Stopped. Reached max position");
            saveItNow = true;
          }
      }
    } else if (action == "manual" && path != 0) {
      /*
       * Manually running the blind
       */
      small_stepper.step(path);
      currentPosition = currentPosition + path;
    }
  }
}