#ifndef _BLLEDFILESYSTEM
#define _BLLEDFILESYSTEM

#include <WiFi.h>
#include "FS.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "types.h"

const char *configPath = "/blledconfig.json";
// Settings are written to a new file first and only then swapped in, keeping the previous
// version, so a power cut in the middle of saving can't leave the controller without settings.
const char *configTmpPath = "/blledconfig.tmp";
const char *configBakPath = "/blledconfig.bak";

static bool configFileValid(const char *path)
{
    File f = LittleFS.open(path, "r");
    if (!f)
        return false;
    JsonDocument doc;
    bool ok = !deserializeJson(doc, f) && doc["ssid"].is<const char *>();
    f.close();
    return ok;
}

// At start-up: if the settings file is missing or broken, use the copy that was being saved or
// the previous one
void recoverConfig()
{
    if (configFileValid(configPath))
    {
        if (LittleFS.exists(configTmpPath))
            LittleFS.remove(configTmpPath); // a save that was cut short; the current file is fine
        return;
    }
    // Moved to the larger layout? The settings were kept in NVS for the move
    {
        Preferences p;
        if (p.begin("blledmig", false))
        {
            size_t n = p.getBytesLength("cfg");
            if (n > 10 && n <= 6000)
            {
                HeapBuf buf(n);
                uint8_t *cfg = buf.p;
                if (!cfg)
                {
                    p.end();
                    return;
                }
                p.getBytes("cfg", cfg, n);
                File f = LittleFS.open(configPath, "w");
                if (f)
                {
                    f.write(cfg, n);
                    f.close();
                }
                if (configFileValid(configPath))
                {
                    LogSerial.println(F("[Filesystem] Restored your settings after moving to the larger layout"));
                    const esp_partition_t *run = esp_ota_get_running_partition();
                    if (run && run->size > 0x140000)
                        p.remove("cfg"); // moved for good: no need to keep the copy
                    p.end();
                    return;
                }
            }
            p.end();
        }
    }
    const char *backups[] = {configTmpPath, configBakPath};
    for (const char *b : backups)
    {
        if (LittleFS.exists(b) && configFileValid(b))
        {
            LittleFS.remove(configPath);
            if (LittleFS.rename(b, configPath))
            {
                LogSerial.printf("[Filesystem] Settings file was damaged - recovered it from %s\n", b);
                return;
            }
        }
    }
}

char *generateRandomString(int length)
{
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    int charsetLength = strlen(charset);

    char *randomString = new char[length + 1];
    for (int i = 0; i < length; i++)
    {
        int randomIndex = random(0, charsetLength);
        randomString[i] = charset[randomIndex];
    }
    randomString[length] = '\0';

    return randomString;
}

void saveFileSystem()
{
    LogSerial.println(F("[Filesystem] Saving config"));

    JsonDocument json;
    json["ssid"] = globalVariables.SSID;
    json["appw"] = globalVariables.APPW;
    json["HTTPUser"] = securityVariables.HTTPUser;
    json["HTTPPass"] = securityVariables.HTTPPass;
    json["host"] = globalVariables.Host;
    json["printerIp"] = printerConfig.printerIP;
    json["accessCode"] = printerConfig.accessCode;
    json["serialNumber"] = printerConfig.serialNumber;
    // json["webpagePassword"] = printerConfig.webpagePassword;
    json["bssi"] = printerConfig.BSSID;
    json["brightness"] = printerConfig.brightness;
    // LED Behaviour (Choose One)
    json["maintMode"] = printerConfig.maintMode;
    json["discoMode"] = printerConfig.discoMode;
    json["replicatestate"] = printerConfig.replicatestate;
    // Running Color
    json["runningRGB"] = printerConfig.runningColor.RGBhex;
    json["runningWW"] = printerConfig.runningColor.ww;
    json["runningCW"] = printerConfig.runningColor.cw;
    // Test LED Colors
    json["showtestcolor"] = printerConfig.testcolorEnabled;
    json["testRGB"] = printerConfig.testColor.RGBhex;
    json["testWW"] = printerConfig.testColor.ww;
    json["testCW"] = printerConfig.testColor.cw;
    json["debugwifi"] = printerConfig.debugwifi;
    // Options
    json["finishindication"] = printerConfig.finishindication;
    json["finishColor"] = printerConfig.finishColor.RGBhex;
    json["finishWW"] = printerConfig.finishColor.ww;
    json["finishCW"] = printerConfig.finishColor.cw;
    json["finishExit"] = printerConfig.finishExit;
    json["finish_check"] = printerConfig.finish_check;
    json["finishTimerMins"] = printerConfig.finishTimeOut;
    json["inactivityEnabled"] = printerConfig.inactivityEnabled;
    json["inactivityTimeOut"] = printerConfig.inactivityTimeOut;
    json["controlChamberLight"] = printerConfig.controlChamberLight; //control chamber light
    // Debugging
    json["debuging"] = printerConfig.debuging;
    json["debugingchange"] = printerConfig.debugingchange;
    json["mqttdebug"] = printerConfig.mqttdebug;
    // Printer Dependant
    json["p1Printer"] = printerVariables.isP1Printer;
    json["doorSwitch"] = printerVariables.useDoorSwtich;
    // Customise LED Colors (during Lidar)
    json["stage14RGB"] = printerConfig.stage14Color.RGBhex;
    json["stage14WW"] = printerConfig.stage14Color.ww;
    json["stage14CW"] = printerConfig.stage14Color.cw;
    json["stage1RGB"] = printerConfig.stage1Color.RGBhex;
    json["stage1WW"] = printerConfig.stage1Color.ww;
    json["stage1CW"] = printerConfig.stage1Color.cw;
    json["stage8RGB"] = printerConfig.stage8Color.RGBhex;
    json["stage8WW"] = printerConfig.stage8Color.ww;
    json["stage8CW"] = printerConfig.stage8Color.cw;
    json["stage9RGB"] = printerConfig.stage9Color.RGBhex;
    json["stage9WW"] = printerConfig.stage9Color.ww;
    json["stage9CW"] = printerConfig.stage9Color.cw;
    json["stage10RGB"] = printerConfig.stage10Color.RGBhex;
    json["stage10WW"] = printerConfig.stage10Color.ww;
    json["stage10CW"] = printerConfig.stage10Color.cw;
    // Customise LED Colors
    json["errordetection"] = printerConfig.errordetection;
    json["wifiRGB"] = printerConfig.wifiRGB.RGBhex;
    json["wifiWW"] = printerConfig.wifiRGB.ww;
    json["wifiCW"] = printerConfig.wifiRGB.cw;
    json["pauseRGB"] = printerConfig.pauseRGB.RGBhex;
    json["pauseWW"] = printerConfig.pauseRGB.ww;
    json["pauseCW"] = printerConfig.pauseRGB.cw;
    json["firstlayerRGB"] = printerConfig.firstlayerRGB.RGBhex;
    json["firstlayerWW"] = printerConfig.firstlayerRGB.ww;
    json["firstlayerCW"] = printerConfig.firstlayerRGB.cw;
    json["nozzleclogRGB"] = printerConfig.nozzleclogRGB.RGBhex;
    json["nozzleclogWW"] = printerConfig.nozzleclogRGB.ww;
    json["nozzleclogCW"] = printerConfig.nozzleclogRGB.cw;
    json["hmsSeriousRGB"] = printerConfig.hmsSeriousRGB.RGBhex;
    json["hmsSeriousWW"] = printerConfig.hmsSeriousRGB.ww;
    json["hmsSeriousCW"] = printerConfig.hmsSeriousRGB.cw;
    json["hmsFatalRGB"] = printerConfig.hmsFatalRGB.RGBhex;
    json["hmsFatalWW"] = printerConfig.hmsFatalRGB.ww;
    json["hmsFatalCW"] = printerConfig.hmsFatalRGB.cw;
    json["filamentRunoutRGB"] = printerConfig.filamentRunoutRGB.RGBhex;
    json["filamentRunoutWW"] = printerConfig.filamentRunoutRGB.ww;
    json["filamentRunoutCW"] = printerConfig.filamentRunoutRGB.cw;
    json["frontCoverRGB"] = printerConfig.frontCoverRGB.RGBhex;
    json["frontCoverWW"] = printerConfig.frontCoverRGB.ww;
    json["frontCoverCW"] = printerConfig.frontCoverRGB.cw;
    json["nozzleTempRGB"] = printerConfig.nozzleTempRGB.RGBhex;
    json["nozzleTempWW"] = printerConfig.nozzleTempRGB.ww;
    json["nozzleTempCW"] = printerConfig.nozzleTempRGB.cw;
    json["bedTempRGB"] = printerConfig.bedTempRGB.RGBhex;
    json["bedTempWW"] = printerConfig.bedTempRGB.ww;
    json["bedTempCW"] = printerConfig.bedTempRGB.cw;
    //HMS Error handling
    json["hmsIgnoreList"] = printerConfig.hmsIgnoreList;
    // Effects
    json["fadeMs"] = printerConfig.fadeMs;
    json["pauseFx"] = printerConfig.pauseFx;
    json["errorFx"] = printerConfig.errorFx;
    json["finishFx"] = printerConfig.finishFx;
    json["progressFx"] = printerConfig.progressFx;
    json["filamentColorFx"] = printerConfig.filamentColorFx;
    json["timelapseSteady"] = printerConfig.timelapseSteady;
    json["heatGlowFx"] = printerConfig.heatGlowFx;
    json["celebrateFx"] = printerConfig.celebrateFx;
    json["coolDownEnabled"] = printerConfig.coolDownEnabled;
    json["coolDownTemp"] = printerConfig.coolDownTemp;
    json["doorWorkLight"] = printerConfig.doorWorkLight;
    json["almostDoneEnabled"] = printerConfig.almostDoneEnabled;
    json["almostDoneMins"] = printerConfig.almostDoneMins;
    json["firstLayerSpotlight"] = printerConfig.firstLayerSpotlight;
    json["silentPwm"] = printerConfig.silentPwm;
    json["calR"] = printerConfig.calR;
    json["amsPulseCount"] = printerConfig.amsPulseCount;
    json["nozzleSwapFx"] = printerConfig.nozzleSwapFx;
    json["nozzleSwapRGB"] = printerConfig.nozzleSwapRGB.RGBhex;
    json["nozzleSwapWW"] = printerConfig.nozzleSwapRGB.ww;
    json["nozzleSwapCW"] = printerConfig.nozzleSwapRGB.cw;
    json["calG"] = printerConfig.calG;
    json["calB"] = printerConfig.calB;
    json["colorsV2"] = true;
    json["needsYouEnabled"] = printerConfig.needsYouEnabled;
    json["needsYouRGB"] = printerConfig.needsYouRGB.RGBhex;
    json["needsYouWW"] = printerConfig.needsYouRGB.ww;
    json["needsYouCW"] = printerConfig.needsYouRGB.cw;
    json["amsErrorRGB"] = printerConfig.amsErrorRGB.RGBhex;
    json["amsErrorWW"] = printerConfig.amsErrorRGB.ww;
    json["amsErrorCW"] = printerConfig.amsErrorRGB.cw;
    json["idleDimEnabled"] = printerConfig.idleDimEnabled;
    json["idleDimLevel"] = printerConfig.idleDimLevel;
    json["idleOffMins"] = printerConfig.idleOffMins;
    json["autoUpdate"] = printerConfig.autoUpdate;
    json["autoUpdateWindow"] = printerConfig.autoUpdateWindow;
    json["autoUpdateStart"] = printerConfig.autoUpdateStart;
    json["autoUpdateEnd"] = printerConfig.autoUpdateEnd;
    // Quiet hours
    json["quietEnabled"] = printerConfig.quietEnabled;
    json["quietStart"] = printerConfig.quietStart;
    json["quietEnd"] = printerConfig.quietEnd;
    json["quietBrightness"] = printerConfig.quietBrightness;
    json["tz"] = printerConfig.tz;

    File configFile = LittleFS.open(configTmpPath, "w");
    if (!configFile)
    {
        LogSerial.println(F("[Filesystem] Failed to save config"));
        return;
    }
    serializeJson(json, configFile);
    configFile.close();
    if (!configFileValid(configTmpPath))
    {
        LogSerial.println(F("[Filesystem] Saving config failed (the new file didn't read back) - kept the old settings"));
        LittleFS.remove(configTmpPath);
        return;
    }
    // Swap the new file in, keeping the previous one as a backup
    if (LittleFS.exists(configPath))
    {
        LittleFS.remove(configBakPath);
        LittleFS.rename(configPath, configBakPath);
    }
    if (!LittleFS.rename(configTmpPath, configPath))
    {
        LogSerial.println(F("[Filesystem] Saving config failed (couldn't swap the file in)"));
        return;
    }
    LogSerial.println(F("[Filesystem] Config Saved"));
}

void loadFileSystem()
{
    LogSerial.println(F("[Filesystem] Loading config"));

    File configFile;
    int attempts = 0;
    while (attempts < 2)
    {
        configFile = LittleFS.open(configPath, "r");
        if (configFile)
        {
            break;
        }
        attempts++;
        LogSerial.println(F("[Filesystem] Failed to open config file, retrying.."));
        delay(2000);
    }
    if (!configFile)
    {
        LogSerial.print(F("[Filesystem] Failed to open config file after "));
        LogSerial.print(attempts);
        LogSerial.println(F(" retries"));

        LogSerial.println(F("[Filesystem] Clearing config"));
        saveFileSystem();
        return;
    }
    JsonDocument json;
    auto deserializeError = deserializeJson(json, configFile);

    if (!deserializeError)
    {
        // strlcpy + "| \"\"": a missing key or an over-long value must not crash or
        // overflow at boot (that would be a boot loop needing a USB reflash).
        strlcpy(globalVariables.SSID, json["ssid"] | "", sizeof(globalVariables.SSID));
        strlcpy(globalVariables.APPW, json["appw"] | "", sizeof(globalVariables.APPW));
        const char *hostValue = json["host"] | "BLLED";
        globalVariables.Host = hostValue;

        strlcpy(securityVariables.HTTPUser, json["HTTPUser"] | "", sizeof(securityVariables.HTTPUser));
        strlcpy(securityVariables.HTTPPass, json["HTTPPass"] | "", sizeof(securityVariables.HTTPPass));
        strlcpy(printerConfig.printerIP, json["printerIp"] | "", sizeof(printerConfig.printerIP));
        strlcpy(printerConfig.accessCode, json["accessCode"] | "", sizeof(printerConfig.accessCode));
        strlcpy(printerConfig.serialNumber, json["serialNumber"] | "", sizeof(printerConfig.serialNumber));
        strlcpy(printerConfig.BSSID, json["bssi"] | "", sizeof(printerConfig.BSSID));
        printerConfig.brightness = json["brightness"];
        // LED Behaviour (Choose One)
        printerConfig.maintMode = json["maintMode"];
        printerConfig.discoMode = json["discoMode"];
        printerConfig.replicatestate = json["replicatestate"];
        // Running Color
        printerConfig.runningColor = hex2rgb(json["runningRGB"], json["runningWW"], json["runningCW"]);
        // Test LED Colors
        printerConfig.testcolorEnabled = json["showtestcolor"];
        printerConfig.testColor = hex2rgb(json["testRGB"], json["testWW"], json["testCW"]);
        printerConfig.debugwifi = json["debugwifi"];
        // Options
        printerConfig.finishindication = json["finishindication"];
        printerConfig.finishColor = hex2rgb(json["finishColor"], json["finishWW"], json["finishCW"]);
        printerConfig.finishExit = json["finishExit"];
        printerConfig.finishTimeOut = json["finishTimerMins"];
        printerConfig.finish_check = json["finish_check"];
        printerConfig.inactivityEnabled = json["inactivityEnabled"];
        printerConfig.inactivityTimeOut = json["inactivityTimeOut"];
        printerConfig.controlChamberLight = json["controlChamberLight"]; //control chamber light
        // Debugging
        printerConfig.debuging = json["debuging"];
        printerConfig.debugingchange = json["debugingchange"];
        printerConfig.mqttdebug = json["mqttdebug"];
        // Printer Dependant
        printerVariables.isP1Printer = json["p1Printer"];
        printerVariables.useDoorSwtich = json["doorSwitch"];
        printerConfig.stage14Color = hex2rgb(json["stage14RGB"], json["stage14WW"], json["stage14CW"]);
        printerConfig.stage1Color = hex2rgb(json["stage1RGB"], json["stage1WW"], json["stage1CW"]);
        printerConfig.stage8Color = hex2rgb(json["stage8RGB"], json["stage8WW"], json["stage8CW"]);
        printerConfig.stage9Color = hex2rgb(json["stage9RGB"], json["stage9WW"], json["stage9CW"]);
        printerConfig.stage10Color = hex2rgb(json["stage10RGB"], json["stage10WW"], json["stage10CW"]);
        // Customise LED Colors
        printerConfig.errordetection = json["errordetection"];
        printerConfig.wifiRGB = hex2rgb(json["wifiRGB"], json["wifiWW"], json["wifiCW"]);

        printerConfig.pauseRGB = hex2rgb(json["pauseRGB"], json["pauseWW"], json["pauseCW"]);
        printerConfig.firstlayerRGB = hex2rgb(json["firstlayerRGB"], json["firstlayerWW"], json["firstlayerCW"]);
        printerConfig.nozzleclogRGB = hex2rgb(json["nozzleclogRGB"], json["nozzleclogWW"], json["nozzleclogCW"]);
        // 2.5: first-layer and clog problems got their own colors. Settings from older firmware
        // still have the old shared blue for them, so give them the new colors once.
        if (!(json["colorsV2"] | false))
        {
            if (strcasecmp(printerConfig.firstlayerRGB.RGBhex, "#0000FF") == 0 && !printerConfig.firstlayerRGB.ww && !printerConfig.firstlayerRGB.cw)
                printerConfig.firstlayerRGB = hex2rgb("#FF0090");
            if (strcasecmp(printerConfig.nozzleclogRGB.RGBhex, "#0000FF") == 0 && !printerConfig.nozzleclogRGB.ww && !printerConfig.nozzleclogRGB.cw)
                printerConfig.nozzleclogRGB = hex2rgb("#FFD000");
        }
        printerConfig.hmsSeriousRGB = hex2rgb(json["hmsSeriousRGB"], json["hmsSeriousWW"], json["hmsSeriousCW"]);
        printerConfig.hmsFatalRGB = hex2rgb(json["hmsFatalRGB"], json["hmsFatalWW"], json["hmsFatalCW"]);
        printerConfig.filamentRunoutRGB = hex2rgb(json["filamentRunoutRGB"], json["filamentRunoutWW"], json["filamentRunoutCW"]);
        printerConfig.frontCoverRGB = hex2rgb(json["frontCoverRGB"], json["frontCoverWW"], json["frontCoverCW"]);
        printerConfig.nozzleTempRGB = hex2rgb(json["nozzleTempRGB"], json["nozzleTempWW"], json["nozzleTempCW"]);
        printerConfig.bedTempRGB = hex2rgb(json["bedTempRGB"], json["bedTempWW"], json["bedTempCW"]);
        // HMS Error handling
        printerConfig.hmsIgnoreList = json["hmsIgnoreList"] | "";
        // Effects (defaults apply to configs saved by older firmware)
        printerConfig.fadeMs = constrain((int)(json["fadeMs"] | 600), 0, 5000);
        printerConfig.pauseFx = constrain((int)(json["pauseFx"] | 1), 0, 2);
        printerConfig.errorFx = constrain((int)(json["errorFx"] | 2), 0, 2);
        printerConfig.finishFx = constrain((int)(json["finishFx"] | 1), 0, 2);
        printerConfig.progressFx = json["progressFx"] | false;
        printerConfig.filamentColorFx = json["filamentColorFx"] | false;
        printerConfig.timelapseSteady = json["timelapseSteady"] | true;
        printerConfig.heatGlowFx = json["heatGlowFx"] | false;
        printerConfig.celebrateFx = json["celebrateFx"] | false;
        printerConfig.coolDownEnabled = json["coolDownEnabled"] | false;
        printerConfig.coolDownTemp = constrain((int)(json["coolDownTemp"] | 35), 20, 80);
        printerConfig.doorWorkLight = json["doorWorkLight"] | false;
        printerConfig.almostDoneEnabled = json["almostDoneEnabled"] | false;
        printerConfig.almostDoneMins = constrain((int)(json["almostDoneMins"] | 5), 1, 60);
        printerConfig.firstLayerSpotlight = json["firstLayerSpotlight"] | true;
        printerConfig.calR = constrain((int)(json["calR"] | 100), 10, 100);
        printerConfig.silentPwm = json["silentPwm"] | false;
        printerConfig.amsPulseCount = json["amsPulseCount"] | false;
        printerConfig.nozzleSwapFx = json["nozzleSwapFx"] | true;
        printerConfig.nozzleSwapRGB = hex2rgb(json["nozzleSwapRGB"] | "#00B4FF", json["nozzleSwapWW"] | 0, json["nozzleSwapCW"] | 0);
        printerConfig.calG = constrain((int)(json["calG"] | 100), 10, 100);
        printerConfig.calB = constrain((int)(json["calB"] | 100), 10, 100);
        printerConfig.needsYouEnabled = json["needsYouEnabled"] | true;
        printerConfig.needsYouRGB = hex2rgb(json["needsYouRGB"] | "#FF7A00", json["needsYouWW"] | 0, json["needsYouCW"] | 0);
        printerConfig.amsErrorRGB = hex2rgb(json["amsErrorRGB"] | "#9000FF", json["amsErrorWW"] | 0, json["amsErrorCW"] | 0);
        printerConfig.idleDimEnabled = json["idleDimEnabled"] | true;
        printerConfig.idleDimLevel = constrain((int)(json["idleDimLevel"] | 15), 1, 90);
        printerConfig.idleOffMins = constrain((int)(json["idleOffMins"] | 60), 0, 999);
        printerConfig.autoUpdate = json["autoUpdate"] | false;
        printerConfig.autoUpdateWindow = json["autoUpdateWindow"] | false;
        printerConfig.autoUpdateStart = constrain((int)(json["autoUpdateStart"] | 2 * 60), 0, 1439);
        printerConfig.autoUpdateEnd = constrain((int)(json["autoUpdateEnd"] | 5 * 60), 0, 1439);
        // Quiet hours
        printerConfig.quietEnabled = json["quietEnabled"] | false;
        printerConfig.quietStart = constrain((int)(json["quietStart"] | 23 * 60), 0, 1439);
        printerConfig.quietEnd = constrain((int)(json["quietEnd"] | 7 * 60), 0, 1439);
        printerConfig.quietBrightness = constrain((int)(json["quietBrightness"] | 10), 0, 100);
        strlcpy(printerConfig.tz, json["tz"] | "MST7MDT,M3.2.0,M11.1.0", sizeof(printerConfig.tz));

        LogSerial.println(F("[Filesystem] Loaded config"));
    }
    else
    {
        LogSerial.println(F("[Filesystem] Failed loading config"));
        LogSerial.println(F("[Filesystem] Clearing config"));
        LittleFS.remove(configPath);

        // LogSerial.println(F("Generating new password"));
        // char* pw = generateRandomString(8);
        // strcpy(printerConfig.webpagePassword, pw);
    }

    configFile.close();
}

void deleteFileSystem()
{
    LogSerial.println(F("[Filesystem] Deleting LittleFS"));
    LittleFS.remove(configPath);
    LittleFS.remove(configTmpPath);
    LittleFS.remove(configBakPath); // a reset really resets
}

bool hasFileSystem()
{
    return LittleFS.exists(configPath);
}

void setupFileSystem()
{
    LogSerial.println(F("[Filesystem] Mounting LittleFS"));
    if (!LittleFS.begin())
    {
        LogSerial.println(F("[Filesystem] Failed to mount LittleFS"));
        LittleFS.format();
        LogSerial.println(F("[Filesystem] Formatting LittleFS"));
        // Mount again rather than restarting: a freshly installed version restarting before it has
        // confirmed it works would make the chip go back to the previous version
        if (!LittleFS.begin())
        {
            LogSerial.println(F("[Filesystem] Restarting Device"));
            delay(1000);
            ESP.restart();
        }
    }
    LogSerial.println(F("[Filesystem] Mounted LittleFS"));
    recoverConfig();
};

#endif