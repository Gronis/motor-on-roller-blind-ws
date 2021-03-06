#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "FS.h"
#include <WiFiClient.h>
#include "stepper.h"

#include <WebSocketsServer.h>

// Version number for checking if there are new code releases and notifying the user
String version = "1.2.0";

//Configure Default Settings for AP logon
String APid = "BlindsConnectAP";
String APpw = "nidayand";

//Setup WIFI Manager
WiFiManager wifiManager;
//Fixed settings for WIFI
WiFiClient espClient;
PubSubClient psclient(espClient);   //MQTT client
String mqttclientid;                //Generated MQTT client id
char mqtt_server[128];               //WIFI config: MQTT server config (optional)
char mqtt_port[6] = "1883";         //WIFI config: MQTT port config (optional)
String outputTopic;                 //MQTT topic for sending messages
String inputTopic;                  //MQTT topic for listening
boolean mqttActive = true;

char config_name[128] = "blinds";   //WIFI config: Bonjour name of device

String action;                      //Action manual/auto
int path = 0;                       //Direction of blind (1 = down, 0 = stop, -1 = up)
int setPos = 0;                     //The set position 0-100% by the client
long currentPosition = 0;           //Current position of the blind
long maxPosition = 2000000;         //Max position of the blind. Initial value
boolean loadDataSuccess = false;
boolean saveItNow = false;          //If true will store positions to SPIFFS
bool shouldSaveConfig = false;      //Used for WIFI Manager callback to save parameters
boolean initLoop = true;            //To enable actions first time the loop is run

Stepper_28BYJ_48 small_stepper(D8, D7, D6, D5); //Initiate stepper driver

WiFiServer server(80);                          // TCP server at port 80 will respond to HTTP requests
WebSocketsServer webSocket = WebSocketsServer(81);  // WebSockets will respond on port 81

void (*softwareReset)(void) = 0; //declare reset function at address 0

/****************************************************************************************
   Loading configuration that has been saved on SPIFFS.
   Returns false if not successful
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
  strcpy(config_name, json["config_name"]);
  strcpy(mqtt_server, json["mqtt_server"]);
  strcpy(mqtt_port, json["mqtt_port"]);
  small_stepper.setMotorSpeed(json["motor_speed"]);

  return true;
}

/**
   Save configuration data to a JSON file
   on SPIFFS
*/
bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition"] = currentPosition;
  json["maxPosition"] = maxPosition;
  json["config_name"] = config_name;
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["motor_speed"] = small_stepper.getMotorSpeed();

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
/****************************************************************************************
*/

/*
   Connect to MQTT server and publish a message on the bus.
   Finally, close down the connection and radio
*/
void sendmsg(String topic, String payload) {
  if (!mqttActive)
    return;

  Serial.println("Trying to send msg..."+topic+":"+payload);
  //Send status to MQTT bus if connected
  if (psclient.connected()) {
    psclient.publish(topic.c_str(), payload.c_str());
  } else {
    Serial.println("PubSub client is not connected...");
  }
}
/*
   Connect the MQTT client to the
   MQTT server
*/
void reconnect() {
  if (!mqttActive)
    return;

  // Loop until we're reconnected
  while (!psclient.connected()) {
    Serial.print("Attempting MQTT connection to ");
    Serial.print(mqtt_server);
    // Attempt to connect
    if (psclient.connect(mqttclientid.c_str())) {
      Serial.println("connected");

      //Send register MQTT message with JSON of chipid and ip-address
      sendmsg("/raw/esp8266/register", getInfoPayload());

      //Setup subscription
      psclient.subscribe(inputTopic.c_str());

    } else {
      Serial.print("failed, rc=");
      Serial.print(psclient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      ESP.wdtFeed();
      delay(5000);
    }
  }
}

String getInfoPayload(){
  return "{ \"id\": \"" + String(ESP.getChipId()) +
        "\", \"ip\":\"" + WiFi.localIP().toString() +
        "\", \"type\":\"roller blind" +
        "\", \"name\":\"" + config_name + "\"";
        "\", \"motor_speed\":" + String(small_stepper.getMotorSpeed()) + "}";
}

String getStatusPayload(){
  int set = (setPos * 100) / maxPosition;
  int pos = (currentPosition * 100) / maxPosition;
  return "{ \"set\":" + String(set) + ", \"position\":" + String(pos) + " }";
}

/*
   Common function to get a topic based on the chipid. Useful if flashing
   more than one device
*/
String getMqttTopic(String type)
{
  return String("/raw/esp8266/") + config_name + "/" + type;
}

/****************************************************************************************
*/
void processMsg(String res, uint8_t clientnum){
  /*
     Check if calibration is running and if stop is received. Store the location
  */
  if (action == "set" && res == "(0)") {
    maxPosition = currentPosition;
    saveItNow = true;
  }

  /*
     Below are actions based on inbound MQTT payload
  */
  if (res == "(start)") {
    /*
       Store the current position as the start position
    */
    currentPosition = 0;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(max)") {
    /*
       Store the max position of a closed blind
    */
    maxPosition = currentPosition;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(0)") {
    /*
       Stop
    */
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "(1)") {
    /*
       Move down without limit to max position
    */
    path = 1;
    action = "manual";
  } else if (res == "(-1)") {
    /*
       Move up without limit to top position
    */
    path = -1;
    action = "manual";
  } else if (res == "(update)") {
    //Send status (position) details to client
    String content = getStatusPayload();
    sendmsg(outputTopic, content);
    webSocket.sendTXT(clientnum, content);
  } else if (res == "(ping)") {
    //Do nothing
  } else if (res == "(info)") {
    // Getter info
    String content = getInfoPayload();
    sendmsg(outputTopic, content);
    webSocket.broadcastTXT(content);
  } else if (res.startsWith("(motor_speed")) {
    // Setter motor_speed
    char *motor_speed_str = &res[res.indexOf(' ')];
    res[res.indexOf(')')] = '\n';
    int motor_speed = String(motor_speed_str).toInt();
    res[res.indexOf('\n')] = ')';
    small_stepper.setMotorSpeed(motor_speed);
    saveItNow = true;
  }
  else if (res == "(reset)") {
    factoryReset();
  } else {
    /*
       Any other message will take the blind to a position
       Incoming value = 0-100
       path is now the position
    */
    path = maxPosition * res.toInt() / 100;
    setPos = path; //Copy path for responding to updates
    action = "auto";
    //Send the instruction to all connected devices
    String content = getStatusPayload();
    sendmsg(outputTopic, content);
    webSocket.broadcastTXT(content);
  }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_TEXT:
            Serial.printf("[%u] get Text: %s\n", num, payload);

            String res = (char*)payload;

            //Send to common MQTT and websocket function
            processMsg(res, num);
            break;
    }
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String res = "";
  for (int i = 0; i < length; i++) {
    res += String((char) payload[i]);
  }
  processMsg(res, NULL);
}

/**
  Turn of power to coils whenever the blind
  is not moving
*/
void stopPowerToCoils() {
  digitalWrite(D8, LOW);
  digitalWrite(D7, LOW);
  digitalWrite(D6, LOW);
  digitalWrite(D5, LOW);
}

/*
   Callback from WIFI Manager for saving configuration
*/
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void factoryReset(){
  SPIFFS.format();
  wifiManager.resetSettings();
  delay(2000);
  softwareReset();
}

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.print("Starting now\n");

  //Reset the action
  action = "";

  //Setup MQTT Client ID
  mqttclientid = ("blinds_" + String(ESP.getChipId()));
  strcpy(config_name, mqttclientid.c_str());
  Serial.println("MQTT Client ID: "+String(mqttclientid));

  //Set the WIFI hostname
  WiFi.hostname(config_name);

  //Define customer parameters for WIFI Manager
  WiFiManagerParameter custom_config_name("Name", "Bonjour name", config_name, 40);
  WiFiManagerParameter custom_mqtt_server("server", "MQTT server (optional)", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqtt_port, 6);

  //reset settings - for testing
  //clean FS, for testing
  //SPIFFS.format();
  //wifiManager.resetSettings();

  wifiManager.setSaveConfigCallback(saveConfigCallback);
  //add all your parameters here
  wifiManager.addParameter(&custom_config_name);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.autoConnect(APid.c_str(), APpw.c_str());

  //Load config upon start
  while (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system, retrying...");
    delay(1000);
  }

  /* Save the config back from WIFI Manager.
      This is only called after configuration
      when in AP mode
  */
  if (shouldSaveConfig) {
    //read updated parameters
    strcpy(config_name, custom_config_name.getValue());
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());

    //Save the data
    saveConfig();
  }

  /*
     Try to load FS data configuration every time when
     booting up. If loading does not work, set the default
     positions
  */
  loadDataSuccess = loadConfig();
  if (!loadDataSuccess) {
    currentPosition = 0;
    maxPosition = 2000000;
  }

  // Setup MQTT topics
  outputTopic = getMqttTopic("out");
  inputTopic = getMqttTopic("in");
  Serial.println("Sending to Mqtt-topic: " + outputTopic);
  Serial.println("Listening to Mqtt-topic: " + inputTopic);

  /*
    Setup multi DNS (Bonjour)
    */
  if (!MDNS.begin(config_name)) {
    Serial.println("Error setting up MDNS responder!");
    while(1) {
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");

  // Start TCP (HTTP) server
  server.begin();
  Serial.println("TCP server started");

  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);

  //Start websocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  /* Setup connection for MQTT and for subscribed
    messages IF a server address has been entered
  */
  if (String(mqtt_server) != ""){
    Serial.println("Registering MQTT server");
    psclient.setServer(mqtt_server, String(mqtt_port).toInt());
    psclient.setCallback(mqttCallback);
  } else {
    mqttActive = false;
    Serial.println("NOTE: No MQTT server address has been registered. Only using websockets");
  }


  //Setup OTA
  {

    // Authentication to avoid unauthorized updates
    //ArduinoOTA.setPassword((const char *)"nidayand");

    ArduinoOTA.setHostname(config_name);

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

int tripCounter = 0;
void loop(void)
{
  //OTA client code
  ArduinoOTA.handle();

  //Websocket listner
  webSocket.loop();

  //MQTT client
  if (mqttActive){
    if (!psclient.connected())
      reconnect();
    psclient.loop();
  }


  /**
    Storing positioning data and turns off the power to the coils
  */
  if (saveItNow) {
    saveConfig();
    saveItNow = false;

    /*
      If no action is required by the motor make sure to
      turn off all coils to avoid overheating and less energy
      consumption
    */
    stopPowerToCoils();

  }

  /**
    Manage actions. Steering of the blind
  */
  if (action == "auto") {
    /*
       Automatically open or close blind
    */
    if (currentPosition > path){
      small_stepper.step(-1);
      currentPosition = currentPosition - 1;
    } else if (currentPosition < path){
      small_stepper.step(1);
      currentPosition = currentPosition + 1;
    } else {
      path = 0;
      action = "";
      String content = getStatusPayload();
      webSocket.broadcastTXT(content);
      sendmsg(outputTopic, content);
      Serial.println("Stopped. Reached wanted position");
      saveItNow = true;
    }

 } else if (action == "manual" && path != 0) {
    /*
       Manually running the blind
    */
    small_stepper.step(path);
    currentPosition = currentPosition + path;
  }

  /*
     After running setup() the motor might still have
     power on some of the coils. This is making sure that
     power is off the first time loop() has been executed
     to avoid heating the stepper motor draining
     unnecessary current
  */
  if (initLoop) {
    initLoop = false;
    stopPowerToCoils();
  }

  /**
    Serving the webpage
  */
  {
    // Check if a client has connected
    WiFiClient webclient = server.available();
    if (!webclient) {
      return;
    }
    Serial.println("New client");

    // Wait for data from client to become available
    while(webclient.connected() && !webclient.available()){
      delay(1);
    }

    // Read the first line of HTTP request
    String req = webclient.readStringUntil('\r');

    // First line of HTTP request looks like "GET /path HTTP/1.1"
    // Retrieve the "/path" part by finding the spaces
    int addr_start = req.indexOf(' ');
    int addr_end = req.indexOf(' ', addr_start + 1);
    if (addr_start == -1 || addr_end == -1) {
      Serial.print("Invalid request: ");
      Serial.println(req);
      return;
    }
    req = req.substring(addr_start + 1, addr_end);
    Serial.print("Request: ");
    Serial.println(req);
    webclient.flush();

    String s;
    if (req == "/")
    {
      s = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<!DOCTYPE html>\n<html>\n<head>\n  <meta http-equiv=\"Cache-Control\" content=\"no-cache, no-store, must-revalidate\" />\n  <meta http-equiv=\"Pragma\" content=\"no-cache\" />\n  <meta http-equiv=\"Expires\" content=\"0\" />\n  <title>{NAME}</title>\n  <link rel=\"stylesheet\" href=\"https://unpkg.com/onsenui/css/onsenui.css\">\n  <link rel=\"stylesheet\" href=\"https://unpkg.com/onsenui/css/onsen-css-components.min.css\">\n  <script src=\"https://unpkg.com/onsenui/js/onsenui.min.js\"></script>\n  <script src=\"https://unpkg.com/jquery/dist/jquery.min.js\"></script>\n  <script>\n  var cversion = \"{VERSION}\";\n  var wsUri = \"ws://\"+location.host+\":81/\";\n  var repo = \"motor-on-roller-blind-ws\";\n\n  window.fn = {};\n  window.fn.open = function() {\n    var menu = document.getElementById('menu');\n    menu.open();\n  };\n\n  window.fn.load = function(page) {\n    var content = document.getElementById('content');\n    var menu = document.getElementById('menu');\n    content.load(page)\n      .then(menu.close.bind(menu)).then(setActions());\n  };\n\n  var gotoPos = function(percent){\n    doSend(percent);\n  };\n  var instr = function(action){\n    doSend(\"(\"+action+\")\");\n  };\n\n  var setActions = function(){\n    doSend(\"(update)\");\n    doSend(\"(info)\");\n    $.get(\"https://api.github.com/repos/nidayand/\"+repo+\"/releases\", function(data){\n      if (data.length>0 && data[0].tag_name !== cversion){\n        $(\"#cversion\").text(cversion);\n        $(\"#nversion\").text(data[0].tag_name);\n        $(\"#update-card\").show();\n      }\n    });\n\n    setTimeout(function(){\n      $(\"#arrow-close\").on(\"click\", function(){$(\"#setrange\").val(100);gotoPos(100);});\n      $(\"#arrow-open\").on(\"click\", function(){$(\"#setrange\").val(0);gotoPos(0);});\n      $(\"#setrange\").on(\"change\", function(){gotoPos($(\"#setrange\").val())});\n\n      $(\"#arrow-up-man\").on(\"click\", function(){instr(\"-1\")});\n      $(\"#arrow-down-man\").on(\"click\", function(){instr(\"1\")});\n      $(\"#arrow-stop-man\").on(\"click\", function(){instr(\"0\")});\n      $(\"#set-start\").on(\"click\", function(){instr(\"start\")});\n      $(\"#set-max\").on(\"click\", function(){instr(\"max\");});\n\n      $(\"#set-motor-speed\").on(\"click\", function(){instr(\"motor_speed \" + $(\"#motor-speed\").val());});\n      $(\"#factory-reset\").on(\"click\", function (){instr(\"reset\");});\n\n    }, 200);\n  };\n  $(document).ready(function(){\n    setActions();\n  });\n\n  var websocket;\n  var timeOut;\n  function retry(){\n    clearTimeout(timeOut);\n    timeOut = setTimeout(function(){\n      websocket=null; init();},5000);\n  };\n  function init(){\n    ons.notification.toast({message: 'Connecting...', timeout: 1000});\n    try{\n      websocket = new WebSocket(wsUri);\n      websocket.onclose = function () {};\n      websocket.onerror = function(evt) {\n        ons.notification.toast({message: 'Cannot connect to device', timeout: 2000});\n        retry();\n      };\n      websocket.onopen = function(evt) {\n        ons.notification.toast({message: 'Connected to device', timeout: 2000});\n        setTimeout(function(){doSend(\"(update)\");}, 1000);\n      };\n      websocket.onclose = function(evt) {\n        ons.notification.toast({message: 'Disconnected. Retrying', timeout: 2000});\n        retry();\n      };\n      websocket.onmessage = function(evt) {\n        try{\n          var msg = JSON.parse(evt.data);\n          console.log(msg)\n          if (typeof msg.position !== 'undefined'){\n            $(\"#pbar\").attr(\"value\", msg.position);\n          };\n          if (typeof msg.set !== 'undefined'){\n            $(\"#setrange\").val(msg.set);\n          };\n          if (typeof msg.motor_speed !== 'undefined'){\n            $(\"#motor-speed\").val(msg.motor_speed);\n          }\n        } catch(err){}\n      };\n    } catch (e){\n      ons.notification.toast({message: 'Cannot connect to device. Retrying...', timeout: 2000});\n      retry();\n    };\n  };\n  function doSend(msg){\n    if (websocket && websocket.readyState == 1){\n      websocket.send(msg);\n    }\n  };\n  window.addEventListener(\"load\", init, false);\n  window.onbeforeunload = function() {\n    if (websocket && websocket.readyState == 1){\n      websocket.close();\n    };\n  };\n  </script>\n</head>\n<body>\n\n<ons-splitter>\n  <ons-splitter-side id=\"menu\" side=\"left\" width=\"220px\" collapse swipeable>\n    <ons-page>\n      <ons-list>\n        <ons-list-item onclick=\"fn.load('home.html')\" tappable>\n          Home\n        </ons-list-item>\n        <ons-list-item onclick=\"fn.load('settings.html')\" tappable>\n          Settings\n        </ons-list-item>\n        <ons-list-item onclick=\"fn.load('about.html')\" tappable>\n          About\n        </ons-list-item>\n      </ons-list>\n    </ons-page>\n  </ons-splitter-side>\n  <ons-splitter-content id=\"content\" page=\"home.html\"></ons-splitter-content>\n</ons-splitter>\n\n<template id=\"home.html\">\n  <ons-page>\n    <ons-toolbar>\n      <div class=\"left\">\n        <ons-toolbar-button onclick=\"fn.open()\">\n          <ons-icon icon=\"md-menu\"></ons-icon>\n        </ons-toolbar-button>\n      </div>\n      <div class=\"center\">\n        {NAME}\n      </div>\n    </ons-toolbar>\n<ons-card>\n    <div class=\"title\">Adjust position</div>\n  <div class=\"content\"><p>Move the slider to the wanted position or use the arrows to open/close to the max positions</p></div>\n  <ons-row>\n      <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\">\n      </ons-col>\n      <ons-col>\n         <ons-progress-bar id=\"pbar\" value=\"75\"></ons-progress-bar>\n      </ons-col>\n      <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\">\n      </ons-col>\n  </ons-row>\n    <ons-row>\n      <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\">\n        <ons-icon id=\"arrow-open\" icon=\"fa-arrow-up\" size=\"2x\"></ons-icon>\n      </ons-col>\n      <ons-col>\n        <ons-range id=\"setrange\" style=\"width: 100%;\" value=\"25\"></ons-range>\n      </ons-col>\n      <ons-col width=\"40px\" style=\"text-align: center; line-height: 31px;\">\n        <ons-icon id=\"arrow-close\" icon=\"fa-arrow-down\" size=\"2x\"></ons-icon>\n      </ons-col>\n    </ons-row>\n\n    </ons-card>\n    <ons-card id=\"update-card\" style=\"display:none\">\n      <div class=\"title\">Update available</div>\n      <div class=\"content\">You are running <span id=\"cversion\"></span> and <span id=\"nversion\"></span> is the latest. Go to <a href=\"https://github.com/nidayand/motor-on-roller-blind-ws/releases\">the repo</a> to download</div>\n    </ons-card>\n  </ons-page>\n</template>\n\n<template id=\"settings.html\">\n  <ons-page>\n    <ons-toolbar>\n      <div class=\"left\">\n        <ons-toolbar-button onclick=\"fn.open()\">\n          <ons-icon icon=\"md-menu\"></ons-icon>\n        </ons-toolbar-button>\n      </div>\n      <div class=\"center\">\n        Settings\n      </div>\n    </ons-toolbar>\n  <ons-card>\n    <div class=\"title\">Instructions</div>\n    <div class=\"content\">\n    <p>\n    <ol>\n      <li>Use the arrows and stop button to navigate to the top position i.e. the blind is opened</li>\n      <li>Click the START button</li>\n      <li>Use the down arrow to navigate to the max closed position</li>\n      <li>Click the MAX button</li>\n      <li>Calibration is completed!</li>\n    </ol>\n    </p>\n  </div>\n  </ons-card>\n  <ons-card>\n    <div class=\"title\">Control</div>\n    <ons-row style=\"width:100%\">\n      <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-up-man\" icon=\"fa-arrow-up\" size=\"2x\"></ons-icon></ons-col>\n      <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-stop-man\" icon=\"fa-stop\" size=\"2x\"></ons-icon></ons-col>\n      <ons-col style=\"text-align:center\"><ons-icon id=\"arrow-down-man\" icon=\"fa-arrow-down\" size=\"2x\"></ons-icon></ons-col>\n    </ons-row>\n  </ons-card>\n  <ons-card>\n    <div class=\"title\">Store</div>\n    <ons-row style=\"width:100%\">\n      <ons-col style=\"text-align:center\"><ons-button id=\"set-start\">Set Start</ons-button></ons-col>\n      <ons-col style=\"text-align:center\">&nbsp;</ons-col>\n      <ons-col style=\"text-align:center\"><ons-button id=\"set-max\">Set Max</ons-button></ons-col>\n    </ons-row>\n  </ons-card>\n  <ons-card>\n    <div class=\"title\">Motor</div>\n    <ons-row style=\"width:100%\">\n      <ons-col style=\"text-align:right\">\n        Speed\n        <ons-input id=\"motor-speed\"></ons-input>\n        <ons-button id=\"set-motor-speed\">Set Speed</ons-button>\n      </ons-col>\n    </ons-row>\n  </ons-card>\n  <ons-card>\n    <div class=\"title\">Factory Reset</div>\n    <ons-row style=\"width:100%\">\n      <ons-col style=\"text-align:right\">\n        <ons-button id=\"factory-reset\">Reset</ons-button>\n      </ons-col>\n    </ons-row>\n  </ons-card>\n  </ons-page>\n</template>\n\n<template id=\"about.html\">\n  <ons-page>\n    <ons-toolbar>\n      <div class=\"left\">\n        <ons-toolbar-button onclick=\"fn.open()\">\n          <ons-icon icon=\"md-menu\"></ons-icon>\n        </ons-toolbar-button>\n      </div>\n      <div class=\"center\">\n        About\n      </div>\n    </ons-toolbar>\n  <ons-card>\n    <div class=\"title\">Motor on a roller blind</div>\n    <div class=\"content\">\n    <p>\n      <ul>\n        <li>3d print files and instructions: <a href=\"https://www.thingiverse.com/thing:2392856\">https://www.thingiverse.com/thing:2392856</a></li>\n        <li>MQTT based version on Github: <a href=\"https://github.com/nidayand/motor-on-roller-blind\">https://github.com/nidayand/motor-on-roller-blind</a></li>\n        <li>WebSocket based version on Github: <a href=\"https://github.com/nidayand/motor-on-roller-blind-ws\">https://github.com/nidayand/motor-on-roller-blind-ws</a></li>\n        <li>Licensed unnder <a href=\"https://creativecommons.org/licenses/by/3.0/\">Creative Commons</a></li>\n      </ul>\n    </p>\n  </div>\n  </ons-card>\n  </ons-page>\n</template>\n\n</body>\n</html>\r\n\r\n ";
      s.replace("{VERSION}", "V" + version);
      s.replace("{NAME}",String(config_name));
      Serial.println("Sending 200");
    }
    else
    {
      s = "HTTP/1.1 404 Not Found\r\n\r\n";
      Serial.println("Sending 404");
    }

    //Print page but as max package is 2048 we need to break it down
    while(s.length()>2000){
      String d = s.substring(0,2000);
      webclient.print(d);
      s.replace(d,"");
    }
    webclient.print(s);

    Serial.println("Done with client");

  }
}
