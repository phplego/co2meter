#include <Arduino.h>
#include <ArduinoOTA.h>
#include "MHZ19.h"                                        
#include <SoftwareSerial.h>                                // Remove if using HardwareSerial
#include <MQTT.h>
#include <ArduinoJson.h>

#include <ESPAsync_WiFiManager.h> 

#if defined(ESP8266)
//#include <WiFiManager.h>
//#include <ESP8266WebServer.h>
#elif defined(ESP32)
//#include <WebServer.h>
//#include <Esp32WifiManager.h>
#endif

#define RX_PIN D2                               // Rx pin which the MHZ19 Tx pin is attached to
#define TX_PIN D3                               // Tx pin which the MHZ19 Rx pin is attached to
#define BAUDRATE 9600                           // Device to MH-Z19 Serial baudrate (should not be changed)

#define CHECK_CO2_INTERVAL    5000              // How often read data from the sensor
#define MQTT_PUBLISH_INTERVAL 20000             // How often publish co2 data to the mqtt topic


#define MQTT_HOST       "xxx.xxx.xxx.xxx"       // MQTT host (eg m21.cloudmqtt.com)
#define MQTT_PORT       1883                    // MQTT port (18076)
#define MQTT_USER       "KJsdfyUYADSFhjscxv678" // Ingored if brocker allows guest connection
#define MQTT_PASS       "d6823645823746dcfgwed" // Ingored if brocker allows guest connection

#if __has_include("local-constants.h")
#include "local-constants.h"                    // Override some constants if local file exists
#endif

#ifdef ESP8266
    uint32_t chipID = ESP.getChipId();
#endif
#ifdef ESP32
    uint64_t macAddress = ESP.getEfuseMac();
    uint64_t macAddressTrunc = macAddress << 40;
    uint32_t chipID = macAddressTrunc >> 40;
#endif


String gDeviceName  = String() + "co2meter-" + chipID;
String gTopic       = "wifi2mqtt/co2meter";

//WiFiManager         wifiManager;
WiFiClient          wifiClient;
MQTTClient          mqttClient(2048);
//ESP8266WebServer    webServer(80);
//Logger              logger;

AsyncWebServer webServer(80);
DNSServer dnsServer;

MHZ19 myMHZ19;                                             // Constructor for library
SoftwareSerial mySerial(RX_PIN, TX_PIN);                   // (Uno example) create device to MH-Z19 serial


void mqtt_connect()
{
    static unsigned long stLastConnectTryTime = 0;
    // retring to connect not too often (every minute)
    if(stLastConnectTryTime && millis() - stLastConnectTryTime < 60000)
        return;
    stLastConnectTryTime = millis();
    Serial.print("Connecting to MQTT...");
    if(mqttClient.connect(gDeviceName.c_str(), MQTT_USER, MQTT_PASS))
    {
        Serial.println(" connected!");
        //mqttClient.subscribe(gTopic + "/set");
    }
    else
        Serial.println(" failed. Retry in 60 sec..");
}


void setup()
{
    Serial.begin(74880);                                    // Device to serial monitor feedback
    
    // =========== CO2 sensor setup ===========
    mySerial.begin(BAUDRATE);                               // (Uno example) device to MH-Z19 serial start   
    myMHZ19.begin(mySerial);                                // *Serial(Stream) refence must be passed to library begin(). 
    myMHZ19.autoCalibration();                              // Turn auto calibration ON (OFF autoCalibration(false))

    // =========== WiFi setup ===========
    //wifi_set_sleep_type(NONE_SLEEP_T); // prevent wifi sleep (stronger connection)
    // On Access Point started (not called if wifi is configured)
    // wifiManager.setAPCallback([](WiFiManager *mgr){
    //     Serial.println(String("Please connect to Wi-Fi"));
    //     Serial.println(String("Network: ") + mgr->getConfigPortalSSID());
    //     Serial.println(String("Password: 12341234"));
    //     Serial.println(String("Then go to ip: 10.0.1.1"));
    // });

    // wifiManager.setAPStaticIPConfig(IPAddress(10, 0, 1, 1), IPAddress(10, 0, 1, 1), IPAddress(255, 255, 255, 0));
    // wifiManager.setConfigPortalTimeout(60);
    // wifiManager.autoConnect(gDeviceName.c_str(), "12341234"); // IMPORTANT! Blocks execution. Waits until connected

    ESPAsync_WiFiManager ESPAsync_wifiManager(&webServer, &dnsServer, "AutoConnectAP");
    //ESPAsync_wifiManager.resetSettings();   //reset saved settings
    //ESPAsync_wifiManager.setAPStaticIPConfig(IPAddress(192,168,186,1), IPAddress(192,168,186,1), IPAddress(255,255,255,0));
    ESPAsync_wifiManager.autoConnect("AutoConnectAP");
    if (WiFi.status() == WL_CONNECTED) { Serial.print(F("Connected. Local IP: ")); Serial.println(WiFi.localIP()); }

    // Restart if not connected
    if (WiFi.status() != WL_CONNECTED)
    {
        ESP.restart();
    }

    String hostname = String() + "esp-" + gDeviceName;
    hostname.replace('.', '_'); 
    WiFi.hostname(hostname);

    // =========== MQTT setup ===========

    mqttClient.begin(MQTT_HOST, MQTT_PORT, wifiClient);
    //mqttClient.onMessage(messageReceived);
    mqtt_connect();
    bool ok = mqttClient.publish(gTopic, "started");
    Serial.println(ok ? "Status successfully published to MQTT" : "Cannot publish status to MQTT");

    // =========== Initialize OTA (firmware updates via WiFi) ===========
    ArduinoOTA.begin();
}

void co2Loop()
{
    static unsigned long getDataTimer = 0;
    static unsigned long mqttTimer = 0;
    
    if (millis() - getDataTimer >= CHECK_CO2_INTERVAL)
    {
        /* note: getCO2() default is command "CO2 Unlimited". This returns the correct CO2 reading even 
        if below background CO2 levels or above range (useful to validate sensor). You can use the 
        usual documented command with getCO2(false) */

        int CO2 = myMHZ19.getCO2();                             // Request CO2 (as ppm)
        
        Serial.print("CO2 (ppm): ");                      
        Serial.print(CO2);                                

        Serial.print(" BackCO2: ");
        Serial.print(myMHZ19.getBackgroundCO2());

        int8_t temp = myMHZ19.getTemperature();                     // Request Temperature (as Celsius)
        Serial.print(" Temp (C): ");                  
        Serial.print(temp);                               
        Serial.println();

        if (millis() - mqttTimer >= MQTT_PUBLISH_INTERVAL){
            DynamicJsonDocument doc(512);
            if(myMHZ19.errorCode == RESULT_OK){
                if(CO2)
                    doc["co2"] = CO2;
                doc["temp"] = temp;
            }
            else{
                doc["error"] = myMHZ19.errorCode;
            }
            String json;
            serializeJson(doc, json);
            mqttClient.publish(gTopic, json);        
            Serial.println(String() + "Published: " + json);  
            mqttTimer = millis();
        }

        getDataTimer = millis();
    }
}

void loop()
{
    ArduinoOTA.handle();
    co2Loop();
    mqttClient.loop();

    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected())
    {
        mqtt_connect();
    }
}