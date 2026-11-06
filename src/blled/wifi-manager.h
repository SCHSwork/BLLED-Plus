#ifndef _BLLEDWIFI_MANAGER
#define _BLLEDWIFI_MANAGER

#include <Arduino.h>
#include <ArduinoJson.h> 
#include <WiFi.h>

#include "filesystem.h"
#include "types.h"

#include <DNSServer.h>
DNSServer dnsServer;
IPAddress apIP(192, 168, 4, 1);

bool shouldSaveConfig = true;
int connectionAttempts = 1;
int wifimode = 0;
uint8_t bssid[6] = {0};

void configModeCallback() {
  Serial.println(F("Entered config mode"));
  Serial.print(F("AP IP address: "));
  Serial.println(WiFi.softAPIP());
}
const char* wl_status_to_string(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
  }
  return "UNKNOWN";
}
int str2mac(const char* mac, uint8_t* values){
    if( 6 == sscanf( mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",&values[0], &values[1], &values[2],&values[3], &values[4], &values[5] ) ){
        return 1;
    }else{
        return 0;
    }
}
bool connectToWifi(){
    Serial.println(F("-------------------------------------"));
    WiFi.mode(WIFI_STA);   //ESP32 connects to an access point
    // Name shown in the router's device list (otherwise it shows as "esp32-xxxxxx")
    WiFi.setHostname(globalVariables.Host.c_str());
    delay(10);
    WiFi.disconnect();
    if(strlen(printerConfig.BSSID) == 0){
        WiFi.begin(globalVariables.SSID, globalVariables.APPW);
        wifimode = 1;
    }
    else{
        // Reference https://stackoverflow.com/questions/70415075/esp32-select-one-from-multiple-wifi-ap-with-same-name-ssid

        //Function to convert String MAC address to 6 byte array
        if(str2mac(printerConfig.BSSID,bssid)){
            WiFi.begin(globalVariables.SSID, globalVariables.APPW, 0, bssid);
            wifimode = 0;
        }
        else{
            if (printerConfig.debuging || printerConfig.debugingchange){
                Serial.print(F("Parsing MAC Address Failed, reverting to "));
                Serial.println(globalVariables.SSID);
            } 
            WiFi.begin(globalVariables.SSID, globalVariables.APPW);
            wifimode = 1;
        }
    }
    
    // Wait up to ~40 s. Routers often refuse or drop the first attempt right after a restart
    // (the old connection hasn't timed out on their side yet), so transient states like
    // "disconnected" or "connection lost" are not a reason to give up. Half way through, try
    // again without the remembered access point (BSSID), in case a mesh node changed.
    const int maxChecks = 20;
    wl_status_t status = WiFi.status();
    wl_status_t lastShown = (wl_status_t)255;
    for (int check = 1; status != WL_CONNECTED; check++) {
        if (check > maxChecks) {
            Serial.print(F("Wi-Fi didn't connect within 40 s, last status: "));
            Serial.println(wl_status_to_string(status));
            return false;
        }
        if (check == maxChecks / 2 + 1) {
            WiFi.disconnect();
            delay(100);
            WiFi.begin(globalVariables.SSID, globalVariables.APPW);
            wifimode = 1;
            Serial.println(F("Retrying without a specific access point (BSSID)"));
        }
        Serial.print(F("Connecting to WIFI.. Status check #"));
        Serial.print(check);
        Serial.print(F(" / "));
        Serial.print(maxChecks);
        Serial.print(F("      SSID: "));
        Serial.print(globalVariables.SSID);
        if (strlen(printerConfig.BSSID) > 0 && wifimode == 0) {
            Serial.print(F("   BSSID: "));
            Serial.print(printerConfig.BSSID);
        }
        Serial.println();
        delay(2000); // Giving time to connect
        status = WiFi.status();
        if (status != lastShown) {
            lastShown = status;
            Serial.print(F("Wifi Status: "));
            Serial.println(wl_status_to_string(status));
        }
    }


    #ifdef ARDUINO_ARCH_ESP32
        WiFi.setTxPower(WIFI_POWER_19_5dBm); // https://github.com/G6EJD/ESP32-8266-Adjust-WiFi-RF-Power-Output/blob/main/README.md
    #endif
    
    #ifdef ESP32
        WiFi.setTxPower(WIFI_POWER_19_5dBm); // https://github.com/G6EJD/ESP32-8266-Adjust-WiFi-RF-Power-Output/blob/main/README.md
    #endif
    Serial.print(F("IP_ADDRESS:"));    // !!! Line required in this format for WifiSetup.html page to show IP Address correct.
    Serial.print(WiFi.localIP());      // !!! Line required in this format for WifiSetup.html page to show IP Address correct.
    Serial.println(F("\n         "));  // !!! Line required in this format for WifiSetup.html page to show IP Address correct.

    Serial.print(F("Connected To Wifi Access Point:  "));
    Serial.println(globalVariables.SSID);
    Serial.print(F("Specific BSSID:                  "));  //MAC address of connected AP
    Serial.println(WiFi.BSSIDstr());
    Serial.print(F("RSSI (Signal Strength):          "));
    Serial.println(WiFi.RSSI());
    Serial.print(F("BLLED Controller IP Address:     "));
    Serial.println(WiFi.localIP());
    Serial.println();
    Serial.print(F("Use web browser to access 'http://"));  // Instruction for user to go to Config page
    Serial.print(WiFi.localIP());                           // Instruction for user to go to Config page
    Serial.println(F("/' to view the setup page"));         // Instruction for user to go to Config page
    Serial.println();
    return true;
};

void startAPMode(bool keepTrying = false) {
    WiFi.disconnect(true, true);
    // With saved Wi-Fi details, keep trying them in the background while the setup hotspot is up
    WiFi.mode(keepTrying ? WIFI_AP_STA : WIFI_AP);
    delay(500);
    WiFi.softAP("BLLED_AP");
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    dnsServer.start(53, "*", apIP);

    Serial.print(F("[WiFiManager] AP started on IP: "));
    Serial.println(WiFi.softAPIP());
    if (keepTrying) {
        WiFi.setAutoReconnect(true);
        WiFi.begin(globalVariables.SSID, globalVariables.APPW);
        Serial.println(F("[WiFiManager] Still trying the saved Wi-Fi in the background"));
    }
}

void scanNetwork()
{
    //Reference / Credit - https://www.esp32.com/viewtopic.php?t=18979#p70299
    int bestRSSI = -200;
    String bestBSSID = "";

    Serial.println(F("Wifi network scan start"));
    int n = WiFi.scanNetworks();
    Serial.println(F("Network scan complete"));

    if (n == 0) {
        Serial.println(F("No wifi networks found :("));
    } else {
        Serial.print(n); // Count
        Serial.println(F(" Wifi networks found"));
        Serial.println(F("-------------------------------------------------------------------------"));
        for (int i = 0; i < n; ++i) {                // Print SSID and RSSI for each network found
            Serial.print(i + 1);
            Serial.print(F(": "));
            Serial.print(WiFi.SSID(i));
            Serial.print(F(" ("));
            Serial.print(WiFi.RSSI(i));
            Serial.print(F("dBm, "));
            if (WiFi.RSSI(i) < -50) Serial.print(F(" ")); //Spacing... because I just can't look at the results when misaligned.
            Serial.print(constrain(2 * (WiFi.RSSI(i) + 100), 0, 100));
            Serial.print(F("%)      "));
            Serial.print(F("BSSID: "));
            Serial.print(WiFi.BSSIDstr(i));
            Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " open" : " encrypted");

            if(WiFi.SSID(i) == globalVariables.SSID && WiFi.RSSI(i) > bestRSSI)
            {
                bestBSSID = WiFi.BSSIDstr(i);
                bestRSSI = WiFi.RSSI(i); 
            }
            delay(10);
        }
        Serial.println();
        if(printerConfig.BSSID == bestBSSID.c_str()){
                Serial.print(F("BSSID already set to: "));
                Serial.print(printerConfig.BSSID);
        }
        else{
            if(strlen(printerConfig.BSSID) == 0 || printerConfig.rescanWiFiNetwork){
                strcpy(printerConfig.BSSID,bestBSSID.c_str());
                Serial.print(F("Saving strongest AP for: "));
                Serial.println(globalVariables.SSID);
                Serial.print(F("Strongest BSSID (MAC Address): "));
                Serial.print(printerConfig.BSSID);
            }
            else{
                Serial.print(F("Ignoring strongest AP for: "));
                Serial.println(globalVariables.SSID);
                Serial.print(F("Using alrady saved BSSID (MAC Address): "));
                Serial.print(printerConfig.BSSID);
            }
            Serial.print(F("     RSSI (Signal Strength): "));
            Serial.print(bestRSSI);
            Serial.println(F("dBm"));
        }
    }
    Serial.println();
}


#endif
