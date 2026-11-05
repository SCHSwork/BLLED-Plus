#include <Arduino.h>
bool shouldRestart = false;
unsigned long restartRequestTime = 0;

// Why the controller last restarted, for the status page. The note survives a soft restart (it
// lives in memory that isn't cleared on reset) but not a power cut, which is fine: then the
// chip itself reports "power on".
#ifdef ESP_PLATFORM
#include "esp_system.h"
RTC_NOINIT_ATTR static char restartNote[40];
RTC_NOINIT_ATTR static uint32_t restartNoteMagic;
#else
static char restartNote[40];
static uint32_t restartNoteMagic;
#endif
String lastRestartWhy = "";
void noteRestart(const char *why)
{
    strncpy(restartNote, why, sizeof(restartNote) - 1);
    restartNote[sizeof(restartNote) - 1] = 0;
    restartNoteMagic = 0xB11ED001;
}
static void readRestartReason()
{
    String note = (restartNoteMagic == 0xB11ED001) ? String(restartNote) : String("");
    restartNoteMagic = 0;
#ifdef ESP_PLATFORM
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON: lastRestartWhy = "Power on"; note = ""; break;
    case ESP_RST_SW: lastRestartWhy = note.length() ? note : String("Restarted by the firmware"); break;
    case ESP_RST_PANIC: lastRestartWhy = "Crashed"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: lastRestartWhy = "Froze (watchdog)"; break;
    case ESP_RST_BROWNOUT: lastRestartWhy = "Power dipped (brownout)"; break;
    default: lastRestartWhy = "Other (" + String((int)esp_reset_reason()) + ")"; break;
    }
#else
    lastRestartWhy = note.length() ? note : String("Power on");
#endif
}
#include "./blled/logSerial.h"
#include "./blled/leds.h"
#include "./blled/filesystem.h"
#include "./blled/types.h"
#include "./blled/bblPrinterDiscovery.h"
#include "./blled/web-server.h"
#include "./blled/mqttmanager.h"
#include "./blled/serialmanager.h"
#include "./blled/wifi-manager.h"
#include "./blled/ssdp.h"


int wifi_reconnect_count = 0;

// Boot watchdog: after an update the controller once got stuck on blue (joined Wi-Fi, but start-up
// never finished) until it was unplugged. If start-up hasn't finished 90 s after Wi-Fi connects,
// restart and try again - the same thing unplugging it does.
#ifdef ESP_PLATFORM
#include "esp_timer.h"
static void bootWatchdogFired(void *)
{
    if (!globalVariables.started)
    {
        noteRestart("Start-up got stuck");
        esp_restart();
    }
}
#endif
void armBootWatchdog()
{
#ifdef ESP_PLATFORM
    static esp_timer_handle_t t = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = &bootWatchdogFired;
    args.name = "bootWatchdog";
    if (!t && esp_timer_create(&args, &t) == ESP_OK)
        esp_timer_start_once(t, 90ULL * 1000000ULL);
#endif
}

void defaultcolors()
{
    LogSerial.println(F("Setting default customisable colors"));
    printerConfig.runningColor = hex2rgb("#000000", 255, 255); // WHITE Running
    printerConfig.testColor = hex2rgb("#3F3CFB");              // Violet Test
    printerConfig.finishColor = hex2rgb("#00FF00");            // Green Finish

    printerConfig.stage14Color = hex2rgb("#000000"); // OFF Cleaning Nozzle
    printerConfig.stage1Color = hex2rgb("#000055");  // OFF Bed Leveling
    printerConfig.stage8Color = hex2rgb("#000000");  // OFF Calibrating Extrusion
    printerConfig.stage9Color = hex2rgb("#000000");  // OFF Scanning Bed Surface
    printerConfig.stage10Color = hex2rgb("#000000"); // OFF First Layer Inspection

    printerConfig.wifiRGB = hex2rgb("#FFA500"); // Orange Wifi Scan

    printerConfig.pauseRGB = hex2rgb("#0000FF");          // Blue Pause
    printerConfig.firstlayerRGB = hex2rgb("#FF0090");     // Pink: first layer / spaghetti
    printerConfig.nozzleclogRGB = hex2rgb("#FFD000");     // Yellow: nozzle clog
    printerConfig.needsYouRGB = hex2rgb("#FF7A00");       // Amber: waiting for you
    printerConfig.amsErrorRGB = hex2rgb("#9000FF");       // Purple: AMS problem
    printerConfig.nozzleSwapRGB = hex2rgb("#00B4FF");     // Sky blue: swapping nozzles (H2)
    printerConfig.hmsSeriousRGB = hex2rgb("#FF0000");     // Red
    printerConfig.hmsFatalRGB = hex2rgb("#FF0000");       // Red
    printerConfig.filamentRunoutRGB = hex2rgb("#FF0000"); // Red
    printerConfig.frontCoverRGB = hex2rgb("#FF0000");     // Red
    printerConfig.nozzleTempRGB = hex2rgb("#FF0000");     // Red
    printerConfig.bedTempRGB = hex2rgb("#FF0000");        // Red
}

void setup()
{
    readRestartReason();
    Serial.begin(115200);
    delay(100);
    Serial.println(F("Initializing"));
    Serial.println(ESP.getFreeHeap());
    Serial.println("");
    Serial.print(F("** Using firmware version: "));
    Serial.print(globalVariables.FWVersion);
    Serial.println(F(" **"));
    Serial.println("");
    defaultcolors();
    setupLeds();
    tweenToColor(100, 100, 100, 100, 100); // ALL LEDS ON
    Serial.println(F(""));

    tweenToColor(255, 0, 0, 0, 0); // RED
    setupFileSystem();
    loadFileSystem();
    Serial.println(F(""));

    tweenToColor(printerConfig.wifiRGB); // Customisable - Default is ORANGE
    setupSerial();

    if (strlen(globalVariables.SSID) == 0 || strlen(globalVariables.APPW) == 0)
    {
        Serial.println(F("SSID or password is missing. Please configure both by going to: https://dutchdevelop.com/blled-configuration-setup/"));
        tweenToColor(100, 0, 100, 0, 0); // PINK
        startAPMode();
        setupWebserver();
        return;
    }
    else

        if (!connectToWifi())
    {
        Serial.println(F("[WiFiManager] Not connected → AP Mode"));
        startAPMode();
        setupWebserver();
        return;
    }
    else
    {
        Serial.println(F("[WiFiManager] connected. Starting webUI."));
        armBootWatchdog();
        // Modem sleep adds 100+ ms latency spikes, which is a common reason the printer
        // drops the MQTT client. The controller is mains powered, so keep the radio awake.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        // Network time, for quiet hours. Syncs in the background; quiet hours wait until it has.
        configTzTime(printerConfig.tz, "pool.ntp.org", "time.google.com");
        tweenToColor(0, 0, 255, 0, 0); // BLUE
        setupWebserver();
        startWebHealthCheck();
    }

    start_ssdp();

    tweenToColor(34, 224, 238, 0, 0); // CYAN
    setupMqtt();
    // >>> Fix: prevent false offline after 30s
    printerVariables.disconnectMQTTms = millis();
    Serial.println();
    Serial.print(F("** BLLED Controller started "));
    Serial.print(F("using firmware version: "));
    Serial.print(globalVariables.FWVersion);
    Serial.println(F(" **"));
    Serial.println();
    globalVariables.started = true;
    Serial.println(F("Updating LEDs from Setup"));
    updateleds();
}

// Wi-Fi recovery: retry gently every 10 s, and after about a minute without success
// rescan for the strongest access point and do a full reconnect.
void wifiWatchdog()
{
    static unsigned long lostAt = 0;
    static unsigned long lastAttempt = 0;

    if (WiFi.getMode() & WIFI_AP)
        return;

    if (WiFi.status() == WL_CONNECTED)
    {
        if (lostAt != 0)
        {
            LogSerial.printf("[WiFi] Reconnected after %lu s (RSSI %d dBm)\n", (millis() - lostAt) / 1000, WiFi.RSSI());
            lostAt = 0;
            wifi_reconnect_count = 0;
        }
        return;
    }

    if (lostAt == 0)
    {
        lostAt = millis();
        lastAttempt = millis(); // auto-reconnect gets the first try
        printerVariables.wifiDrops++;
        LogSerial.printf("[WiFi] Connection dropped (%s)\n", wl_status_to_string(WiFi.status()));
        return;
    }

    if (millis() - lastAttempt < 10000)
        return;
    lastAttempt = millis();
    wifi_reconnect_count++;

    if (wifi_reconnect_count <= 5)
    {
        LogSerial.printf("[WiFi] Reconnect attempt %d\n", wifi_reconnect_count);
        WiFi.reconnect();
    }
    else
    {
        LogSerial.println(F("[WiFi] Still offline - rescanning for the strongest access point"));
        printerConfig.rescanWiFiNetwork = true; // let scanNetwork() pick a new BSSID if the old AP is gone
        scanNetwork();
        printerConfig.rescanWiFiNetwork = false;
        connectToWifi();
        WiFi.setSleep(false);
        wifi_reconnect_count = 0;
        lastAttempt = millis();
    }
}

void loop()
{
    serialLoop();

    // Captive portal for first-time setup (the BLLED_AP hotspot). This runs before
    // "started" is set, so it has to live outside that block.
    if (WiFi.getMode() & WIFI_AP)
    {
        dnsServer.processNextRequest();
    }

    if (globalVariables.started)
    {
        websocketLoop();
        ledsloop();
        wifiWatchdog();

        if (WiFi.status() == WL_CONNECTED && !(WiFi.getMode() & WIFI_AP))
        {
            bblSearchPrinters();
        }
    }
    if (printerConfig.rescanWiFiNetwork)
    {
        LogSerial.println(F("Web submitted refresh of Wifi Scan (assigning Strongest AP)"));
        tweenToColor(printerConfig.wifiRGB); // Customisable - Default is ORANGE
        scanNetwork();                       // Sets the MAC address for following connection attempt
        printerConfig.rescanWiFiNetwork = false;
        updateleds();
    }
    if (shouldRestart && millis() - restartRequestTime > 1500)
    {
        LogSerial.println(F("[WiFiSetup] Restarting now..."));
        if (restartNoteMagic != 0xB11ED001)
            noteRestart("Settings saved or update");
        ESP.restart();
    }
}