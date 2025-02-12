#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <EEPROM.h>
#include <WiFiManager.h>    // https://github.com/tzapu/WiFiManager 
#include <StateMachine.h>   // https://github.com/jrullan/StateMachine
#include <neotimer.h>       // https://github.com/jrullan/neotimer
#include <TelnetStream.h>
#include <Print.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

#include "SmartBmsData.h"
#include "SmartBmsError.h"
#include "SmartBmsReader.h"

/* ----- Defines ----- */
#define PIN_RED_LED 0
#define PIN_BLUE_LED 2
#define OTA_PORT 3232
#define TELNET_PORT 23

#define PC_SERIAL_BAUD 9600

#define BMS_SERIAL_BAUD_RATE 9600
#define BMS_SERIAL_RX_PIN 12
#define BMS_SERIAL_INVERT true

/* ----- EEPROM map ----- */
struct eeprom_storage {
  char wifi_ssid[32];
  char wifi_password[32];

  char mqtt_username[32];
  char mqtt_password[32];
  char mqtt_server[32];
  uint16_t mqtt_server_port;
} eeprom;

/* ----- Declare static variables ----- */

class MySerial: public Print {
    using Print::Print;   // Inheriting constructors
    public: 
    size_t write(uint8_t val) override;   // Overriding base functionality
    size_t write(const uint8_t *buffer, size_t size) override;   // Overriding base functionality
};
MySerial logger;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
EspSoftwareSerial::UART bmsPort;
SmartBmsReader smartBmsReader(&bmsPort);

WiFiManager wifiManager;
WiFiManagerParameter custom_mqtt_server("mqtt_server", "MQTT server");
WiFiManagerParameter custom_mqtt_username("mqtt_username", "MQTT username");
WiFiManagerParameter custom_mqtt_password("mqtt_password", "MQTT password");
WiFiManagerParameter custom_mqtt_server_port("mqtt_server_port", "MQTT server port");

Neotimer timeoutTimer = Neotimer();  // General timeout timer used in states

/* Declare state machine */
StateMachine machine = StateMachine();
State* State_SetupWifi = machine.addState(&setupWifi);
State* State_startConfiguration = machine.addState(&startConfigurationManager);
State* State_SetupStream = machine.addState(&setupStream);
State* State_SetupOta = machine.addState(&setupOta);
State* State_SetupMqtt = machine.addState(&setupMqtt);
State* State_SetupBMS = machine.addState(&setupBms);
State* State_Idle = machine.addState(&idle);
State* State_EepromSave = machine.addState(&saveEeprom);
State* State_Error = machine.addState(&error);

/* Static variables */
uint16_t tick;  // Increased every loop, every ~1 mS
uint16_t tick_250;  // Increased every ~250 mS

SmartBmsData smartBmsData;

/* ----- Setup ----- */
void setup() {

  /* Setup transitions for states */
  State_SetupWifi->addTransition(&checkSetupWifi, State_SetupStream);
  State_SetupWifi->addTransition(&waitTimeout, State_Error);

  State_SetupStream->addTransition(&checkTrue, State_SetupOta);
  State_SetupOta->addTransition(&checkTrue, State_SetupMqtt);

  State_SetupMqtt->addTransition(&checkSetupMqtt, State_SetupBMS);
  State_SetupMqtt->addTransition(&waitTimeout, State_Error);

  State_SetupBMS->addTransition(&checkTrue, State_Idle);

  State_Idle->addTransition(&mqttNotConnected, State_SetupMqtt);
  State_Idle->addTransition(&wifiNotConnected, State_SetupWifi);

  /* Initialize pins */
  pinMode(PIN_RED_LED, OUTPUT);
  pinMode(PIN_BLUE_LED, OUTPUT);
  ledRed(false);
  ledBlue(true);

  /* Read storage from EEPROM */
  EEPROM.begin(sizeof(eeprom));
  EEPROM.get(0, eeprom);

  /* Setup serial port and flush */
  Serial.begin(PC_SERIAL_BAUD);
  Serial.println("SETUP: Flush");
  Serial.flush();
  Serial.println("SETUP: Starting");
}

/* ----- Main loop ----- */
void loop() {
  ArduinoOTA.handle();
  machine.run();
  processCommands();
  mqttClient.loop();

  tick++;
  if (tick % 250 == 0){
    tick_250++;
  }
  delay(1);
}

/* --- States ---- */

void error(void) {
  /*  Error state, restart */
  logger.println("Error state, restarting");
  ESP.restart();
}
/* Setup wifi */
void setupWifi(void) {
  if (machine.executeOnce) {
    logger.println("Wifi: SSID '" + String(eeprom.wifi_ssid) + "'");
    logger.println("Wifi: Pwd '" + String(eeprom.wifi_password) + "'");
    logger.println("Wifi: Connecting");

    wifiManager.setTitle("SmartBMS/123 to Mqtt");

    custom_mqtt_server.setValue(eeprom.mqtt_server, sizeof(eeprom.mqtt_server));
    wifiManager.addParameter(&custom_mqtt_server);
    custom_mqtt_username.setValue(eeprom.mqtt_username, sizeof(eeprom.mqtt_username));
    wifiManager.addParameter(&custom_mqtt_username);
    custom_mqtt_password.setValue(eeprom.mqtt_password, sizeof(eeprom.mqtt_password));
    wifiManager.addParameter(&custom_mqtt_password);
    custom_mqtt_server_port.setValue(String(eeprom.mqtt_server_port).c_str(), 5);
    wifiManager.addParameter(&custom_mqtt_server_port);

    wifiManager.setDebugOutput(true, WM_DEBUG_NOTIFY );
  
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    String ap_name = "configure-";
    ap_name += String(WiFi.macAddress());
    ap_name.replace(":", "");
    wifiManager.autoConnect(ap_name.c_str());  // connect wifi. If fail, start ap.

    timeoutTimer.set(30000);
    timeoutTimer.start();

    randomSeed(tick);  // use time it takes for wifi to establish as seed
  }
  ledBlink();
}

void setupStream(void) {
  if (machine.executeOnce) {
    TelnetStream.begin(TELNET_PORT);
    logger.println("Stream: Setup");
  }
}

void setupOta(void) {
  logger.println("OTA: Setup");
  ArduinoOTA.setPort(OTA_PORT);
  //ArduinoOTA.setHostname("myesp32");
  //ArduinoOTA.setPassword("admin");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    logger.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    logger.println("");
    logger.println("OTA: End");
    TelnetStream.stop();
    TelnetStream.end();
    delay(10);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (progress % 1000 == 0) {
      logger.println("OTA: Progress: " + String(progress) + "/" + String(total));
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    logger.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      logger.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      logger.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      logger.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      logger.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      logger.println("End Failed");
    }
  });
  ArduinoOTA.begin();
}

void setupBms(void) {

  bmsPort.begin(BMS_SERIAL_BAUD_RATE, SWSERIAL_8N1, BMS_SERIAL_RX_PIN, -1, BMS_SERIAL_INVERT);
  if (!bmsPort) {  // If the object did not initialize, then its configuration is invalid
    logger.println("BMS: Invalid configuration"); 
    machine.transitionTo(State_Error);
  }
  //bmsPort.enableIntTx(false);
  //bmsPort.enableTx(false);
  logger.println("BMS: Port ready"); 
} 

void startConfigurationManager(void) {
  if (machine.executeOnce) {
    ledRed(true);

    wifiManager.setTitle("SmartBMS/123 to Mqtt");
    wifiManager.setDebugOutput(true, WM_DEBUG_NOTIFY );
    logger.println("CONFIG: Starting portal");
  
    WiFi.begin();
    String ap_name = "configure-";
    ap_name += String(WiFi.macAddress());
    ap_name.replace(":", "");
  
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    if (!wifiManager.startConfigPortal(ap_name.c_str())) {
      logger.println("SETUP: failed");
      machine.transitionTo(State_Error);
    }
    ledRed(false);
  }
}

/* Setup mqtt */
void setupMqtt(void) {
  if (machine.executeOnce) {
    logger.println("MQTT: '" + String(eeprom.mqtt_server) + "'");
    mqttClient.setServer(eeprom.mqtt_server, eeprom.mqtt_server_port);
    //mqttClient.setCallback(mqttCallback);

    timeoutTimer.set(30000);
    timeoutTimer.start();
  }
  ledBlink();
}

bool checkSetupMqtt(void) {
  if (mqttClient.connected()) {
    logger.println("MQTT: Connected");
    // mqttClient.subscribe(getTopic("command").c_str());
    String topic = getTopic("online");
    mqttClient.publish(topic.c_str(), "1");
    ledRed(false);
    return true;
  }

  if (tick % 500 == 0) {  // reconnect every 5:th second
    String client_id = "smartbms123-";
    client_id += String(WiFi.macAddress());
    client_id.replace(":", "");

    if (!mqttClient.connect(client_id.c_str(), eeprom.mqtt_username, eeprom.mqtt_password)) {
      logger.println("MQTT: " + String(mqttClient.state()));
    }
  }
  return false;
}

bool mqttNotConnected(void) {
  if (!mqttClient.connected()) {
    logger.println("MQTT: Not connected");
    return true;
  }
  return false;
}

/* ----- Transitions ----- */

bool checkSetupWifi(void) {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wifi: Connected, IP: ");
    logger.println(WiFi.localIP());
    ledRed(false);
    return true;
  }
  return false;
}

bool wifiNotConnected(void) {
  if (WiFi.status() == WL_CONNECTED) {
    return false;
  } 
  logger.print("Wifi: Not conencted");
  return true;
}

bool checkTrue(void) {
  return true;
}

bool waitTimeout(void) {
  return timeoutTimer.done();
}


/* ----- Callbacks ----- */
void saveConfigCallback () {
  logger.println("CONFIG: Save config");

  logger.println("CONFIG: ssid " + wifiManager.getWiFiSSID());
  memset(eeprom.wifi_ssid, 0, sizeof(eeprom.wifi_ssid));
  strncpy(eeprom.wifi_ssid, wifiManager.getWiFiSSID().c_str(), sizeof(eeprom.wifi_ssid));

  logger.println("CONFIG: pwd " + wifiManager.getWiFiPass());
  memset(eeprom.wifi_password, 0, sizeof(eeprom.wifi_password));
  strncpy(eeprom.wifi_password, wifiManager.getWiFiPass().c_str(), sizeof(eeprom.wifi_password));

  logger.println("CONFIG: MQTT server " + String(custom_mqtt_server.getValue()));
  memset(eeprom.mqtt_server, 0, sizeof(eeprom.mqtt_server));
  strncpy(eeprom.mqtt_server, custom_mqtt_server.getValue(), sizeof(eeprom.mqtt_server));

  logger.println("CONFIG: MQTT username " + String(custom_mqtt_username.getValue()));
  memset(eeprom.mqtt_username, 0, sizeof(eeprom.mqtt_username));
  strncpy(eeprom.mqtt_username, custom_mqtt_username.getValue(), sizeof(eeprom.mqtt_username));

  logger.println("CONFIG: MQTT password " + String(custom_mqtt_password.getValue()));
  memset(eeprom.mqtt_password, 0, sizeof(eeprom.mqtt_password));
  strncpy(eeprom.mqtt_password, custom_mqtt_password.getValue(), sizeof(eeprom.mqtt_password));

  logger.println("CONFIG: MQTT port " + String(custom_mqtt_server_port.getValue()));
  eeprom.mqtt_server_port = String(custom_mqtt_server_port.getValue()).toInt();

  machine.transitionTo(State_EepromSave);
}

void saveEeprom(void) {
  EEPROM.put(0, eeprom);
  delay(100);
  EEPROM.commit();
  logger.println("EEPROM: Saved");

  machine.transitionTo(State_Idle);
}

void idle(void) {
  static Neotimer sendMqttTimer_1 = Neotimer();
  static Neotimer sendMqttTimer_3 = Neotimer();
  static Neotimer sendMqttTimer_5 = Neotimer();
  static Neotimer sendMqttTimer_120 = Neotimer();

  if (machine.executeOnce) {
    logger.println("IDLE");

    sendMqttTimer_1.set(1000);
    sendMqttTimer_1.start();

    sendMqttTimer_3.set(3000);
    sendMqttTimer_3.start();

    sendMqttTimer_5.set(5000);
    sendMqttTimer_5.start();

    sendMqttTimer_120.set(120000);
    sendMqttTimer_120.start();
    ledBlue(false);
  }

  // On every cycle, send current cell voltage and temperture:
  if (smartBmsReader.bmsDataReady() == SmartBmsError::SBMS_OK) {
    const SmartBmsError err = smartBmsReader.decodeBmsData(&smartBmsData);
    if (err == SmartBmsError::SBMS_OK) {
      ledBlue(true);

      logger.println("BMS: Cell " + String(smartBmsData.getCurrentCell()) + " " + String(smartBmsData.getCurrentCellVoltage()) + "v " + String(smartBmsData.getCurrentCellTemperature()) + "°C");

      String topic = getTopic("voltage/" + String(smartBmsData.getCurrentCell()));
      mqttClient.publish(topic.c_str(), String(smartBmsData.getCurrentCellVoltage()).c_str());

      topic = getTopic("temperature/" + String(smartBmsData.getCurrentCell()));
      mqttClient.publish(topic.c_str(), String(smartBmsData.getCurrentCellTemperature()).c_str());

      ledBlue(false);
    } else if (err == SmartBmsError::SBMS_ERR_READ_STREAM) {
            logger.println("BMS: Could not read");
    } else if (err == SmartBmsError::SBMS_ERR_INVALID_CHECKSUM) {
            logger.println("BMS: Invalid checksum.");
    }
  }

  if (sendMqttTimer_1.repeat()) {
    switch (random(4)) {
      case 0:
        mqttClient.publish(getTopic("pack-voltage").c_str(), String(smartBmsData.getPackVoltage()).c_str());
        break;
      case 1:
        mqttClient.publish(getTopic("pack-current").c_str(), String(smartBmsData.getPackCurrent()).c_str());
        break;
      case 2:
        mqttClient.publish(getTopic("allowed-charge").c_str(), String(smartBmsData.isAllowedToCharge()).c_str());
        break;
      case 3:
        mqttClient.publish(getTopic("allowed-discharge").c_str(), String(smartBmsData.isAllowedToDischarge()).c_str());
        break;
    }
  }

  if (sendMqttTimer_3.repeat()) {
    switch (random(4)) {
      case 0:
        mqttClient.publish(getTopic("lowest-cell-voltage").c_str(), String(smartBmsData.getLowestCellVoltage()).c_str());
        mqttClient.publish(getTopic("lowest-cell-voltage-number").c_str(), String(smartBmsData.getLowestCellVoltageNumber()).c_str());
        break;
      case 1:
        mqttClient.publish(getTopic("highest-cell-voltage").c_str(), String(smartBmsData.getHighestCellVoltage()).c_str());
        mqttClient.publish(getTopic("highest-cell-voltage-number").c_str(), String(smartBmsData.getHighestCellVoltageNumber()).c_str());
        break;
      case 2:
        mqttClient.publish(getTopic("lowest-cell-temperature").c_str(), String(smartBmsData.getLowestCellTemperature()).c_str());
        mqttClient.publish(getTopic("lowest-cell-temperature-number").c_str(), String(smartBmsData.getLowestCellTemperatureNumber()).c_str());
        break;
      case 3:
        mqttClient.publish(getTopic("highest-cell-temperature").c_str(), String(smartBmsData.getHighestCellTemperature()).c_str());
        mqttClient.publish(getTopic("highest-cell-temperature-number").c_str(), String(smartBmsData.getHighestCellTemperatureNumber()).c_str());
        break;
    }
  }

  if (sendMqttTimer_5.repeat()) {
    switch (random(6)) {
      case 0:
        mqttClient.publish(getTopic("alarm-communication-error").c_str(), String(smartBmsData.hasCommunicationError()).c_str());
        break;
      case 1:
        mqttClient.publish(getTopic("alarm-min-voltage").c_str(), String(smartBmsData.isMinVoltageAlarmActive()).c_str());
        break;
      case 2:
        mqttClient.publish(getTopic("alarm-max-voltage").c_str(), String(smartBmsData.isMaxVoltageAlarmActive()).c_str());
        break;
      case 3:
        mqttClient.publish(getTopic("alarm-min-temperature").c_str(), String(smartBmsData.isMinTemperatureAlarmActive()).c_str());
        break;
      case 4:
        mqttClient.publish(getTopic("alarm-max-temperature").c_str(), String(smartBmsData.isMaxTemperatureAlarmActive()).c_str());
        break;
      case 5:
        mqttClient.publish(getTopic("soc").c_str(), String(smartBmsData.getPackSoc()).c_str());
        break;
    }
  }

  if (sendMqttTimer_120.repeat()) {
    switch (random(8)) {
      case 0:
        mqttClient.publish(getTopic("cell-count").c_str(), String(smartBmsData.getCellCount()).c_str());
        break;
      case 1:
        mqttClient.publish(getTopic("balance-voltage").c_str(), String(smartBmsData.getCellVoltageBalance()).c_str());
        break;
      case 2:
        mqttClient.publish(getTopic("pack-capacity").c_str(), String(smartBmsData.getPackCapacity()).c_str());
        break;
      case 3:
        mqttClient.publish(getTopic("pack-energy").c_str(), String(smartBmsData.getPackRemainingEnergy()).c_str());
        break;
      case 4:
        mqttClient.publish(getTopic("min-cell-voltage").c_str(), String(smartBmsData.getCellVoltageMin()).c_str());
        break;
      case 5:
        mqttClient.publish(getTopic("max-cell-voltage").c_str(), String(smartBmsData.getCellVoltageMax()).c_str());
        break;
      case 6:
        mqttClient.publish(getTopic("pack-charge-current").c_str(), String(smartBmsData.getPackChargeCurrent()).c_str());
        break;
      case 7:
        mqttClient.publish(getTopic("pack-discharge-current").c_str(), String(smartBmsData.getPackDischargeCurrent()).c_str());
        break;
    }
  }

  ledBlinkSlow();
}

void processCommands(void) {
  switch (TelnetStream.read()) {
    case 'r':
      logger.println("CMD: Restarting");
      delay(100);
      TelnetStream.stop();
      delay(100);
      ESP.restart();
      break;
    case 'c':
      logger.println("CMD: Starting config manager");
      machine.transitionTo(State_startConfiguration);
      break;
    case 's':
      logger.println("--- Status ---");
      logger.print("IP: ");
      logger.println(WiFi.localIP());
      logger.println();

      logger.print("Mqtt: ");
      logger.print(mqttClient.connected());
      logger.println();
      logger.println();

      logger.println((String) "Cell-Count: " + smartBmsData.getCellCount());
      logger.println((String) "Balance-Voltage: " + smartBmsData.getCellVoltageBalance() + "V");
      logger.println((String) "Pack-Capacity: " + smartBmsData.getPackCapacity() + "kWh");
      logger.println((String) "Pack-Energy: " + smartBmsData.getPackRemainingEnergy() + "kWh");
      logger.println((String) "Min-Cell-Voltage: " + smartBmsData.getCellVoltageMin() + "V");
      logger.println((String) "Max-Cell-Voltage: " + smartBmsData.getCellVoltageMax() + "V");
      logger.println((String) "Pack-Charge-Current: " + smartBmsData.getPackChargeCurrent() + "A");
      logger.println((String) "Pack-Discharge-Current: " + smartBmsData.getPackDischargeCurrent() + "A");
      logger.println((String) "Alarm-Communication-Error: " + (smartBmsData.hasCommunicationError() ? "Active" : "Inactive"));
      logger.println((String) "Alarm-Min-Voltage: " + (smartBmsData.isMinVoltageAlarmActive() ? "Active" : "Inactive"));
      logger.println((String) "Alarm-Max-Voltage: " + (smartBmsData.isMaxVoltageAlarmActive() ? "Active" : "Inactive"));
      logger.println((String) "Alarm-Min-Temp: " + (smartBmsData.isMinTemperatureAlarmActive() ? "Active" : "Inactive"));
      logger.println((String) "Alarm-Max-Temp: " + (smartBmsData.isMaxTemperatureAlarmActive() ? "Active" : "Inactive"));
      logger.println((String) "Pack-SOC: " + smartBmsData.getPackSoc() + "%");
      logger.println((String) "Lowest-Cell-Voltage: " + smartBmsData.getLowestCellVoltage() + "V");
      logger.println((String) "Lowest-Cell-Voltage-Numer: " + smartBmsData.getLowestCellVoltageNumber());
      logger.println((String) "Highest-Cell-Voltage: " + smartBmsData.getHighestCellVoltage() + "V");
      logger.println((String) "Highest-Cell-Voltage-Number: " + smartBmsData.getHighestCellVoltageNumber());
      logger.println((String) "Lowest-Cell-Temp: " + smartBmsData.getLowestCellTemperature() + "°C");
      logger.println((String) "Lowest-Cell-Temp-Number: " + smartBmsData.getLowestCellTemperatureNumber());
      logger.println((String) "Highest-Cell-Temp: " + smartBmsData.getHighestCellTemperature() + "°C");
      logger.println((String) "Highest-Cell-Temp-Number: " + smartBmsData.getHighestCellTemperatureNumber());
      logger.println((String) "Pack-Voltage: " + smartBmsData.getPackVoltage() + "V");
      logger.println((String) "Pack-Current: " + smartBmsData.getPackCurrent() + "A");
      logger.println((String) "Allowed-Charge: " + (smartBmsData.isAllowedToCharge() ? "Yes" : "No"));
      logger.println((String) "Allowed-Discharge: " + (smartBmsData.isAllowedToDischarge() ? "Yes" : "No"));
      logger.println();
      break;
    case 'q':
      logger.println("CMD: Bye");
      TelnetStream.stop();
      break;
    }
}

void ledRed(bool state) {
  if (state) {
    digitalWrite(PIN_RED_LED, LOW);
  } else {
    digitalWrite(PIN_RED_LED, HIGH);
  }
}

void ledBlue(bool state) {
  if (state) {
    digitalWrite(PIN_BLUE_LED, LOW);
  } else {
    digitalWrite(PIN_BLUE_LED, HIGH);
  }
}

void ledBlink(void) {
  ledRed(tick_250 %2 == 0);  // Blink LED every 250 mS
}

void ledBlinkSlow(void) {
  if (tick % 5000 >= 0 && tick % 5000 <= 20) {
    ledRed(true);
  } else {
    ledRed(false);
  }
}

/* Generate topic for MQTT */
String getTopic(String t) {
  String topic = "smartbms123/";
  topic += String(WiFi.macAddress());
  topic.replace(":", "");
  topic = topic + "/" + t;
  return topic;
}

size_t MySerial::write(uint8_t val) {
    TelnetStream.write(val);
    return Serial.write(val);
}
size_t MySerial::write(const uint8_t *buffer, size_t size) {
    TelnetStream.write(buffer, size);
    return Serial.write(buffer, size);
}
