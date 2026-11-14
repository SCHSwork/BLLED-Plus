#ifndef _BLLEDWEB_SERVER
#define _BLLEDWEB_SERVER

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include "leds.h"
#include "filesystem.h"

AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");

#include "../www/www.h"
#include "selfupdate.h"
#include "notify.h" // full edition only (empty in lite)
#ifdef BLLED_LITE
#include "layoutmigrate.h"
#endif
#include "hms_table.h" // full edition only (empty in lite)

unsigned long lastWsPush = 0;
const unsigned long wsPushInterval = 1000; // alle 1000ms

bool isAuthorized(AsyncWebServerRequest *request)
{
    if (strlen(securityVariables.HTTPUser) == 0 || strlen(securityVariables.HTTPPass) == 0)
    {
        return true;
    }
    return request->authenticate(securityVariables.HTTPUser, securityVariables.HTTPPass);
}

// Requests that change something must come from the controller's own pages: they carry an
// X-BLLED header. A page on another site can't add a custom header without the browser first
// asking this server (a CORS preflight), which never agrees, so it can't change settings,
// restore, re-flash or reset the controller from someone's browser. This matters most when
// no WebUI login is set, and a browser that has saved the login would otherwise send it too.
bool fromOwnPage(AsyncWebServerRequest *request)
{
    return request->hasHeader("X-BLLED");
}

void refuseForeign(AsyncWebServerRequest *request)
{
    request->send(403, "text/plain", "Refused: use the controller's own pages.");
}

void handleSetup(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, setupPage_html_gz_mime, setupPage_html_gz, setupPage_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleUpdatePage(AsyncWebServerRequest *request)
{
    // Opened by name (blled.local)? Switch to the controller's IP address first. Some phones lose
    // track of the .local name partway through a long upload, which makes the update fail.
    if (WiFi.status() == WL_CONNECTED && !(WiFi.getMode() & WIFI_AP))
    {
        String ip = WiFi.localIP().toString();
        String host = request->host();
        int colon = host.indexOf(':');
        if (colon >= 0)
            host = host.substring(0, colon);
        if (host.length() && host != ip && ip != "0.0.0.0")
            return request->redirect("http://" + ip + request->url());
    }
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, updatePage_html_gz_mime, updatePage_html_gz, updatePage_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleGetIcon(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, blled_svg_gz_mime, blled_svg_gz, blled_svg_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleGetfavicon(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, favicon_png_gz_mime, favicon_png_gz, favicon_png_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleGetPCC(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, particleCanvas_js_gz_mime, (const uint8_t *)particleCanvas_js_gz, particleCanvas_js_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleGetConfig(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }

    JsonDocument doc;

    doc["firmwareversion"] = globalVariables.FWVersion.c_str();
    doc["wifiStrength"] = WiFi.RSSI();
    doc["host"] = globalVariables.Host;
    doc["ip"] = printerConfig.printerIP;
    doc["id"] = printerConfig.serialNumber;
    doc["apMAC"] = printerConfig.BSSID;
    doc["brightness"] = printerConfig.brightness;
    doc["maintMode"] = printerConfig.maintMode;
    doc["discoMode"] = printerConfig.discoMode;
    doc["replicateled"] = printerConfig.replicatestate;
    doc["runningRGB"] = printerConfig.runningColor.RGBhex;
    doc["runningWW"] = printerConfig.runningColor.ww;
    doc["runningCW"] = printerConfig.runningColor.cw;
    doc["showtestcolor"] = printerConfig.testcolorEnabled;
    doc["testRGB"] = printerConfig.testColor.RGBhex;
    doc["testWW"] = printerConfig.testColor.ww;
    doc["testCW"] = printerConfig.testColor.cw;
    doc["debugwifi"] = printerConfig.debugwifi;
    doc["finishindication"] = printerConfig.finishindication;
    doc["finishColor"] = printerConfig.finishColor.RGBhex;
    doc["finishWW"] = printerConfig.finishColor.ww;
    doc["finishCW"] = printerConfig.finishColor.cw;
    doc["finishExit"] = printerConfig.finishExit;
    doc["finishTimerMins"] = (int)(printerConfig.finishTimeOut / 60000);
    doc["inactivityEnabled"] = printerConfig.inactivityEnabled;
    doc["inactivityMins"] = (int)(printerConfig.inactivityTimeOut / 60000);
    doc["debuging"] = printerConfig.debuging;
    doc["watchMsgs"] = printerConfig.watchMsgs;
#ifdef BLLED_LITE
    doc["noWatch"] = true;
#endif
    doc["cameraLight"] = printerConfig.cameraLight;
    doc["cameraLightSecs"] = printerConfig.cameraLightSecs;
    doc["debugingchange"] = printerConfig.debugingchange;
    doc["mqttdebug"] = printerConfig.mqttdebug;
    doc["p1Printer"] = printerVariables.isP1Printer;
    doc["doorSwitch"] = printerVariables.useDoorSwtich;
    doc["stage14RGB"] = printerConfig.stage14Color.RGBhex;
    doc["stage14WW"] = printerConfig.stage14Color.ww;
    doc["stage14CW"] = printerConfig.stage14Color.cw;
    doc["stage1RGB"] = printerConfig.stage1Color.RGBhex;
    doc["stage1WW"] = printerConfig.stage1Color.ww;
    doc["stage1CW"] = printerConfig.stage1Color.cw;
    doc["stage8RGB"] = printerConfig.stage8Color.RGBhex;
    doc["stage8WW"] = printerConfig.stage8Color.ww;
    doc["stage8CW"] = printerConfig.stage8Color.cw;
    doc["stage9RGB"] = printerConfig.stage9Color.RGBhex;
    doc["stage9WW"] = printerConfig.stage9Color.ww;
    doc["stage9CW"] = printerConfig.stage9Color.cw;
    doc["stage10RGB"] = printerConfig.stage10Color.RGBhex;
    doc["stage10WW"] = printerConfig.stage10Color.ww;
    doc["stage10CW"] = printerConfig.stage10Color.cw;
    doc["errordetection"] = printerConfig.errordetection;
    doc["wifiRGB"] = printerConfig.wifiRGB.RGBhex;
    doc["wifiWW"] = printerConfig.wifiRGB.ww;
    doc["wifiCW"] = printerConfig.wifiRGB.cw;
    doc["pauseRGB"] = printerConfig.pauseRGB.RGBhex;
    doc["pauseWW"] = printerConfig.pauseRGB.ww;
    doc["pauseCW"] = printerConfig.pauseRGB.cw;
    doc["firstlayerRGB"] = printerConfig.firstlayerRGB.RGBhex;
    doc["firstlayerWW"] = printerConfig.firstlayerRGB.ww;
    doc["firstlayerCW"] = printerConfig.firstlayerRGB.cw;
    doc["nozzleclogRGB"] = printerConfig.nozzleclogRGB.RGBhex;
    doc["nozzleclogWW"] = printerConfig.nozzleclogRGB.ww;
    doc["nozzleclogCW"] = printerConfig.nozzleclogRGB.cw;
    doc["hmsSeriousRGB"] = printerConfig.hmsSeriousRGB.RGBhex;
    doc["hmsSeriousWW"] = printerConfig.hmsSeriousRGB.ww;
    doc["hmsSeriousCW"] = printerConfig.hmsSeriousRGB.cw;
    doc["hmsFatalRGB"] = printerConfig.hmsFatalRGB.RGBhex;
    doc["hmsFatalWW"] = printerConfig.hmsFatalRGB.ww;
    doc["hmsFatalCW"] = printerConfig.hmsFatalRGB.cw;
    doc["filamentRunoutRGB"] = printerConfig.filamentRunoutRGB.RGBhex;
    doc["filamentRunoutWW"] = printerConfig.filamentRunoutRGB.ww;
    doc["filamentRunoutCW"] = printerConfig.filamentRunoutRGB.cw;
    doc["frontCoverRGB"] = printerConfig.frontCoverRGB.RGBhex;
    doc["frontCoverWW"] = printerConfig.frontCoverRGB.ww;
    doc["frontCoverCW"] = printerConfig.frontCoverRGB.cw;
    doc["nozzleTempRGB"] = printerConfig.nozzleTempRGB.RGBhex;
    doc["nozzleTempWW"] = printerConfig.nozzleTempRGB.ww;
    doc["nozzleTempCW"] = printerConfig.nozzleTempRGB.cw;
    doc["bedTempRGB"] = printerConfig.bedTempRGB.RGBhex;
    doc["bedTempWW"] = printerConfig.bedTempRGB.ww;
    doc["bedTempCW"] = printerConfig.bedTempRGB.cw;
    // HMS Error Handling
    doc["hmsIgnoreList"] = printerConfig.hmsIgnoreList;
    // control chamber light
    doc["controlChamberLight"] = printerConfig.controlChamberLight;
    // Effects
    doc["fadeMs"] = printerConfig.fadeMs;
    doc["pauseFx"] = printerConfig.pauseFx;
    doc["errorFx"] = printerConfig.errorFx;
    doc["finishFx"] = printerConfig.finishFx;
    doc["progressFx"] = printerConfig.progressFx;
    doc["filamentColorFx"] = printerConfig.filamentColorFx;
    doc["timelapseSteady"] = printerConfig.timelapseSteady;
    doc["heatGlowFx"] = printerConfig.heatGlowFx;
    doc["celebrateFx"] = printerConfig.celebrateFx;
    doc["coolDownEnabled"] = printerConfig.coolDownEnabled;
    doc["coolDownTemp"] = printerConfig.coolDownTemp;
    doc["doorWorkLight"] = printerConfig.doorWorkLight;
    doc["almostDoneEnabled"] = printerConfig.almostDoneEnabled;
    doc["almostDoneMins"] = printerConfig.almostDoneMins;
    doc["firstLayerSpotlight"] = printerConfig.firstLayerSpotlight;
    doc["silentPwm"] = printerConfig.silentPwm;
    doc["calR"] = printerConfig.calR;
    doc["amsPulseCount"] = printerConfig.amsPulseCount;
    doc["nozzleSwapFx"] = printerConfig.nozzleSwapFx;
    doc["nozzleSwapRGB"] = printerConfig.nozzleSwapRGB.RGBhex;
    doc["nozzleSwapWW"] = printerConfig.nozzleSwapRGB.ww;
    doc["nozzleSwapCW"] = printerConfig.nozzleSwapRGB.cw;
    doc["calG"] = printerConfig.calG;
    doc["calB"] = printerConfig.calB;
    doc["needsYouEnabled"] = printerConfig.needsYouEnabled;
    doc["needsYouRGB"] = printerConfig.needsYouRGB.RGBhex;
    doc["needsYouWW"] = printerConfig.needsYouRGB.ww;
    doc["needsYouCW"] = printerConfig.needsYouRGB.cw;
    doc["amsErrorRGB"] = printerConfig.amsErrorRGB.RGBhex;
    doc["amsErrorWW"] = printerConfig.amsErrorRGB.ww;
    doc["amsErrorCW"] = printerConfig.amsErrorRGB.cw;
    doc["idleDimEnabled"] = printerConfig.idleDimEnabled;
    doc["idleDimLevel"] = printerConfig.idleDimLevel;
    doc["idleOffMins"] = printerConfig.idleOffMins;
    // Quiet hours (times as HH:MM for <input type=time>)
    char hm[16];
    doc["autoUpdate"] = printerConfig.autoUpdate;
    doc["autoUpdateWindow"] = printerConfig.autoUpdateWindow;
    snprintf(hm, sizeof(hm), "%02d:%02d", printerConfig.autoUpdateStart / 60, printerConfig.autoUpdateStart % 60);
    doc["autoUpdateStart"] = hm;
    snprintf(hm, sizeof(hm), "%02d:%02d", printerConfig.autoUpdateEnd / 60, printerConfig.autoUpdateEnd % 60);
    doc["autoUpdateEnd"] = hm;
    doc["quietEnabled"] = printerConfig.quietEnabled;
    snprintf(hm, sizeof(hm), "%02d:%02d", printerConfig.quietStart / 60, printerConfig.quietStart % 60);
    doc["quietStart"] = hm;
    snprintf(hm, sizeof(hm), "%02d:%02d", printerConfig.quietEnd / 60, printerConfig.quietEnd % 60);
    doc["quietEnd"] = hm;
    doc["quietBrightness"] = printerConfig.quietBrightness;
    doc["tz"] = printerConfig.tz;
#ifndef BLLED_LITE
    // Notifications. The Discord and webhook addresses and the ntfy token work like passwords
    // (anyone who has them can post), so only whether one is stored is sent back.
    doc["hasDiscord"] = printerConfig.notifyDiscordUrl[0] != 0;
    doc["hasWebhook"] = printerConfig.notifyWebhookUrl[0] != 0;
    doc["hasNtfyToken"] = printerConfig.notifyNtfyToken[0] != 0;
    doc["nNtfyServer"] = printerConfig.notifyNtfyServer;
    doc["nNtfyTopic"] = printerConfig.notifyNtfyTopic;
    doc["nFinish"] = printerConfig.notifyFinish;
    doc["nFail"] = printerConfig.notifyFail;
    doc["nNeedsYou"] = printerConfig.notifyNeedsYou;
    doc["nStart"] = printerConfig.notifyStart;
    doc["nFirstLayer"] = printerConfig.notifyFirstLayer;
    doc["nProgress"] = printerConfig.notifyProgress;
    doc["nProgressStep"] = printerConfig.notifyProgressStep;
    doc["nBar"] = printerConfig.notifyBar;
    {
        JsonArray r = doc["nRoute"].to<JsonArray>();
        for (int i = 0; i < NT_COUNT; i++)
            r.add(printerConfig.notifyRoute[i]);
    }
    {
        // Wording: what's set (empty = default) and the defaults, for the boxes' placeholders
        JsonObject tpl = doc["nTpl"].to<JsonObject>();
        for (int i = 0; i < NT_COUNT; i++)
        {
            JsonArray a = tpl[notifyTplKey[i]].to<JsonArray>();
            a.add(printerConfig.notifyTitle[i]);
            a.add(printerConfig.notifyBody[i]);
            a.add(notifyDefTitle[i]);
            a.add(notifyDefBody[i]);
        }
    }
    doc["nRules"] = printerConfig.notifyRules;
#endif

    String jsonString;
    serializeJson(doc, jsonString);
    request->send(200, "application/json", jsonString);
}

void handlePrinterConfigJson(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    // Secrets (WiFi password, access code, WebUI password) are never sent back to the
    // browser - only whether one is stored. Blank fields on submit keep the stored value.
    JsonDocument doc;
    doc["ssid"] = globalVariables.SSID;
    doc["hasPass"] = strlen(globalVariables.APPW) > 0;
    doc["host"] = globalVariables.Host;
    doc["printerIP"] = printerConfig.printerIP;
    doc["printerSerial"] = printerConfig.serialNumber;
    doc["hasAccessCode"] = strlen(printerConfig.accessCode) > 0;
    doc["webUser"] = securityVariables.HTTPUser;
    doc["hasWebPass"] = strlen(securityVariables.HTTPPass) > 0;
    doc["isAPMode"] = (WiFi.getMode() & WIFI_AP);

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void handleStyleCss(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, style_css_gz_mime, style_css_gz, style_css_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleSubmitConfig(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    if (!fromOwnPage(request))
    {
        return refuseForeign(request);
    }

    auto getSafeParamValue = [](AsyncWebServerRequest *req, const char *name, const char *fallback = "") -> String
    {
        return req->hasParam(name, true) ? req->getParam(name, true)->value() : fallback;
    };

    auto getSafeParamInt = [](AsyncWebServerRequest *req, const char *name, int fallback = 0) -> int
    {
        return req->hasParam(name, true) ? req->getParam(name, true)->value().toInt() : fallback;
    };


    
        String newHost = getSafeParamValue(request, "deviceName", globalVariables.Host.c_str());
        newHost.trim();
        if (newHost.length() == 0)
        {
            newHost = "BLLED";
        }

        if (newHost != globalVariables.Host)
        {
            LogSerial.print(F("[MDNS] Updating hostname from "));
            LogSerial.print(globalVariables.Host);
            LogSerial.print(F(" to "));
            LogSerial.println(newHost);

            globalVariables.Host = newHost;

            MDNS.end();
            if (!MDNS.begin(globalVariables.Host.c_str()))
            {
                LogSerial.println(F("[MDNS] Error resetting MDNS responder!"));
            }
            else
            {
#ifndef BLLED_LITE // (lite: no room - add it in Home Assistant by its address)
                MDNS.addService("blled", "tcp", 80); // Home Assistant finds it by this
#endif
                LogSerial.print(F("[MDNS] Hostname now: "));
                LogSerial.println(globalVariables.Host);
            }
        }
    

    printerConfig.brightness = getSafeParamInt(request, "brightnessslider");
    printerConfig.rescanWiFiNetwork = request->hasParam("rescanWiFiNetwork", true);
    printerConfig.maintMode = request->hasParam("maintMode", true);
    printerConfig.discoMode = request->hasParam("discoMode", true);
    printerConfig.replicatestate = request->hasParam("replicateLedState", true);
    printerConfig.runningColor = hex2rgb(getSafeParamValue(request, "runningRGB").c_str(), getSafeParamInt(request, "runningWW"), getSafeParamInt(request, "runningCW"));
    printerConfig.testcolorEnabled = request->hasParam("showtestcolor", true);
    printerConfig.testColor = hex2rgb(getSafeParamValue(request, "testRGB").c_str(), getSafeParamInt(request, "testWW"), getSafeParamInt(request, "testCW"));
    printerConfig.debugwifi = request->hasParam("debugwifi", true);
    printerConfig.finishindication = request->hasParam("finishIndication", true);
    printerConfig.finishColor = hex2rgb(getSafeParamValue(request, "finishColor").c_str(), getSafeParamInt(request, "finishWW"), getSafeParamInt(request, "finishCW"));
    printerConfig.finishExit = !request->hasParam("finishEndTimer", true);
    printerConfig.finishTimeOut = getSafeParamInt(request, "finishTimerMins") * 60000;
    printerConfig.inactivityEnabled = request->hasParam("inactivityEnabled", true);
    printerConfig.inactivityTimeOut = getSafeParamInt(request, "inactivityMins") * 60000;
    printerConfig.debuging = request->hasParam("debuging", true);
    printerConfig.watchMsgs = request->hasParam("watchMsgs", true);
    printerConfig.cameraLight = request->hasParam("cameraLight", true);
    printerConfig.cameraLightSecs = constrain(getSafeParamInt(request, "cameraLightSecs"), 5, 3600);
    printerConfig.debugingchange = request->hasParam("debugingchange", true);
    printerConfig.mqttdebug = request->hasParam("mqttdebug", true);
    printerVariables.isP1Printer = request->hasParam("p1Printer", true);
    printerVariables.useDoorSwtich = request->hasParam("doorSwitch", true);

    printerConfig.stage14Color = hex2rgb(getSafeParamValue(request, "stage14RGB").c_str(), getSafeParamInt(request, "stage14WW"), getSafeParamInt(request, "stage14CW"));
    printerConfig.stage1Color = hex2rgb(getSafeParamValue(request, "stage1RGB").c_str(), getSafeParamInt(request, "stage1WW"), getSafeParamInt(request, "stage1CW"));
    printerConfig.stage8Color = hex2rgb(getSafeParamValue(request, "stage8RGB").c_str(), getSafeParamInt(request, "stage8WW"), getSafeParamInt(request, "stage8CW"));
    printerConfig.stage9Color = hex2rgb(getSafeParamValue(request, "stage9RGB").c_str(), getSafeParamInt(request, "stage9WW"), getSafeParamInt(request, "stage9CW"));
    printerConfig.stage10Color = hex2rgb(getSafeParamValue(request, "stage10RGB").c_str(), getSafeParamInt(request, "stage10WW"), getSafeParamInt(request, "stage10CW"));
    printerConfig.errordetection = request->hasParam("errorDetection", true);
    printerConfig.wifiRGB = hex2rgb(getSafeParamValue(request, "wifiRGB").c_str(), getSafeParamInt(request, "wifiWW"), getSafeParamInt(request, "wifiCW"));
    printerConfig.pauseRGB = hex2rgb(getSafeParamValue(request, "pauseRGB").c_str(), getSafeParamInt(request, "pauseWW"), getSafeParamInt(request, "pauseCW"));
    printerConfig.firstlayerRGB = hex2rgb(getSafeParamValue(request, "firstlayerRGB").c_str(), getSafeParamInt(request, "firstlayerWW"), getSafeParamInt(request, "firstlayerCW"));
    printerConfig.nozzleclogRGB = hex2rgb(getSafeParamValue(request, "nozzleclogRGB").c_str(), getSafeParamInt(request, "nozzleclogWW"), getSafeParamInt(request, "nozzleclogCW"));
    printerConfig.hmsSeriousRGB = hex2rgb(getSafeParamValue(request, "hmsSeriousRGB").c_str(), getSafeParamInt(request, "hmsSeriousWW"), getSafeParamInt(request, "hmsSeriousCW"));
    printerConfig.hmsFatalRGB = hex2rgb(getSafeParamValue(request, "hmsFatalRGB").c_str(), getSafeParamInt(request, "hmsFatalWW"), getSafeParamInt(request, "hmsFatalCW"));
    printerConfig.filamentRunoutRGB = hex2rgb(getSafeParamValue(request, "filamentRunoutRGB").c_str(), getSafeParamInt(request, "filamentRunoutWW"), getSafeParamInt(request, "filamentRunoutCW"));
    printerConfig.frontCoverRGB = hex2rgb(getSafeParamValue(request, "frontCoverRGB").c_str(), getSafeParamInt(request, "frontCoverWW"), getSafeParamInt(request, "frontCoverCW"));
    printerConfig.nozzleTempRGB = hex2rgb(getSafeParamValue(request, "nozzleTempRGB").c_str(), getSafeParamInt(request, "nozzleTempWW"), getSafeParamInt(request, "nozzleTempCW"));
    printerConfig.bedTempRGB = hex2rgb(getSafeParamValue(request, "bedTempRGB").c_str(), getSafeParamInt(request, "bedTempWW"), getSafeParamInt(request, "bedTempCW"));
    // HMS Error handling
    printerConfig.hmsIgnoreList = getSafeParamValue(request, "hmsIgnoreList");
    // Control Chamber Light
    printerConfig.controlChamberLight = request->hasParam("controlChamberLight", true);
    // Effects
    printerConfig.fadeMs = constrain(getSafeParamInt(request, "fadeMs", printerConfig.fadeMs), 0, 5000);
    printerConfig.pauseFx = constrain(getSafeParamInt(request, "pauseFx", printerConfig.pauseFx), 0, 2);
    printerConfig.errorFx = constrain(getSafeParamInt(request, "errorFx", printerConfig.errorFx), 0, 2);
    printerConfig.finishFx = constrain(getSafeParamInt(request, "finishFx", printerConfig.finishFx), 0, 2);
    printerConfig.progressFx = request->hasParam("progressFx", true);
    printerConfig.filamentColorFx = request->hasParam("filamentColorFx", true);
    printerConfig.timelapseSteady = request->hasParam("timelapseSteady", true);
    printerConfig.heatGlowFx = request->hasParam("heatGlowFx", true);
    printerConfig.celebrateFx = request->hasParam("celebrateFx", true);
    printerConfig.coolDownEnabled = request->hasParam("coolDownEnabled", true);
    printerConfig.coolDownTemp = constrain(getSafeParamInt(request, "coolDownTemp", printerConfig.coolDownTemp), 20, 80);
    printerConfig.doorWorkLight = request->hasParam("doorWorkLight", true);
    printerConfig.almostDoneEnabled = request->hasParam("almostDoneEnabled", true);
    printerConfig.almostDoneMins = constrain(getSafeParamInt(request, "almostDoneMins", printerConfig.almostDoneMins), 1, 60);
    printerConfig.firstLayerSpotlight = request->hasParam("firstLayerSpotlight", true);
    printerConfig.calR = constrain(getSafeParamInt(request, "calR", printerConfig.calR), 10, 100);
    printerConfig.amsPulseCount = request->hasParam("amsPulseCount", true);
    printerConfig.nozzleSwapFx = request->hasParam("nozzleSwapFx", true);
    if (request->hasParam("nozzleSwapRGB", true))
        printerConfig.nozzleSwapRGB = hex2rgb(getSafeParamValue(request, "nozzleSwapRGB").c_str(), getSafeParamInt(request, "nozzleSwapWW"), getSafeParamInt(request, "nozzleSwapCW"));
    printerConfig.calG = constrain(getSafeParamInt(request, "calG", printerConfig.calG), 10, 100);
    printerConfig.calB = constrain(getSafeParamInt(request, "calB", printerConfig.calB), 10, 100);
    printerConfig.needsYouEnabled = request->hasParam("needsYouEnabled", true);
    if (request->hasParam("needsYouRGB", true))
        printerConfig.needsYouRGB = hex2rgb(getSafeParamValue(request, "needsYouRGB").c_str(), getSafeParamInt(request, "needsYouWW"), getSafeParamInt(request, "needsYouCW"));
    if (request->hasParam("amsErrorRGB", true))
        printerConfig.amsErrorRGB = hex2rgb(getSafeParamValue(request, "amsErrorRGB").c_str(), getSafeParamInt(request, "amsErrorWW"), getSafeParamInt(request, "amsErrorCW"));
    printerConfig.idleDimEnabled = request->hasParam("idleDimEnabled", true);
    printerConfig.idleDimLevel = constrain(getSafeParamInt(request, "idleDimLevel", printerConfig.idleDimLevel), 1, 90);
    printerConfig.idleOffMins = constrain(getSafeParamInt(request, "idleOffMins", printerConfig.idleOffMins), 0, 999);
    // Quiet hours
    auto parseHM = [](const String &s, int fallback) -> int {
        int colon = s.indexOf(':');
        if (colon < 1)
            return fallback;
        int h = s.substring(0, colon).toInt(), m = s.substring(colon + 1).toInt();
        if (h < 0 || h > 23 || m < 0 || m > 59)
            return fallback;
        return h * 60 + m;
    };
    printerConfig.autoUpdate = request->hasParam("autoUpdate", true);
    printerConfig.autoUpdateWindow = request->hasParam("autoUpdateWindow", true);
    printerConfig.autoUpdateStart = parseHM(getSafeParamValue(request, "autoUpdateStart"), printerConfig.autoUpdateStart);
    printerConfig.autoUpdateEnd = parseHM(getSafeParamValue(request, "autoUpdateEnd"), printerConfig.autoUpdateEnd);
    printerConfig.quietEnabled = request->hasParam("quietEnabled", true);
    printerConfig.quietStart = parseHM(getSafeParamValue(request, "quietStart"), printerConfig.quietStart);
    printerConfig.quietEnd = parseHM(getSafeParamValue(request, "quietEnd"), printerConfig.quietEnd);
    printerConfig.quietBrightness = constrain(getSafeParamInt(request, "quietBrightness", printerConfig.quietBrightness), 0, 100);
    String newTz = getSafeParamValue(request, "tz", printerConfig.tz);
    if (newTz.length() > 0 && newTz != printerConfig.tz)
    {
        strlcpy(printerConfig.tz, newTz.c_str(), sizeof(printerConfig.tz));
        setenv("TZ", printerConfig.tz, 1);
        tzset();
        actLogTzReady = true;
    }

#ifndef BLLED_LITE
    // Notifications. The secret ones: a blank field keeps what's stored, "...Clear" removes it.
    if (request->hasParam("notifyForm", true)) // the notifications part of the form was there
    {
        auto secret = [&](const char *name, const char *clearName, char *dst, size_t n) {
            String v = getSafeParamValue(request, name);
            v.trim();
            if (request->hasParam(clearName, true))
                dst[0] = 0;
            else if (v.length())
                strlcpy(dst, v.c_str(), n);
        };
        secret("nDiscord", "nDiscordClear", printerConfig.notifyDiscordUrl, sizeof(printerConfig.notifyDiscordUrl));
        secret("nWebhook", "nWebhookClear", printerConfig.notifyWebhookUrl, sizeof(printerConfig.notifyWebhookUrl));
        secret("nNtfyToken", "nNtfyTokenClear", printerConfig.notifyNtfyToken, sizeof(printerConfig.notifyNtfyToken));
        String server = getSafeParamValue(request, "nNtfyServer");
        server.trim();
        if (server.length() == 0)
            server = "https://ntfy.sh";
        strlcpy(printerConfig.notifyNtfyServer, server.c_str(), sizeof(printerConfig.notifyNtfyServer));
        String topic = getSafeParamValue(request, "nNtfyTopic");
        topic.trim();
        strlcpy(printerConfig.notifyNtfyTopic, topic.c_str(), sizeof(printerConfig.notifyNtfyTopic));
        printerConfig.notifyFinish = request->hasParam("nFinish", true);
        printerConfig.notifyFail = request->hasParam("nFail", true);
        printerConfig.notifyNeedsYou = request->hasParam("nNeedsYou", true);
        printerConfig.notifyStart = request->hasParam("nStart", true);
        printerConfig.notifyFirstLayer = request->hasParam("nFirstLayer", true);
        printerConfig.notifyProgress = request->hasParam("nProgress", true);
        printerConfig.notifyProgressStep = constrain(getSafeParamInt(request, "nProgressStep", printerConfig.notifyProgressStep), 5, 50);
        printerConfig.notifyBar = request->hasParam("nBar", true);
        for (int i = 0; i < NT_COUNT; i++)
        {
            // Ticks in the "Where each one goes" grid: nR_<kind>_d / _n / _w
            String k = String("nR_") + notifyTplKey[i];
            printerConfig.notifyRoute[i] = (request->hasParam((k + "_d").c_str(), true) ? 1 : 0) |
                                           (request->hasParam((k + "_n").c_str(), true) ? 2 : 0) |
                                           (request->hasParam((k + "_w").c_str(), true) ? 4 : 0);
        }
        for (int i = 0; i < NT_COUNT; i++)
        {
            String k = String("nT_") + notifyTplKey[i], v = getSafeParamValue(request, k.c_str());
            v.trim();
            strlcpy(printerConfig.notifyTitle[i], v.c_str(), sizeof(printerConfig.notifyTitle[i]));
            k = String("nB_") + notifyTplKey[i];
            v = getSafeParamValue(request, k.c_str());
            v.trim();
            strlcpy(printerConfig.notifyBody[i], v.c_str(), sizeof(printerConfig.notifyBody[i]));
        }
        String rules = getSafeParamValue(request, "nRules");
        rules.replace("\r", "");
        rules.trim();
        printerConfig.notifyRules = rules.substring(0, 500);
    }
#endif

    printerConfig.silentPwm = request->hasParam("silentPwm", true);
    saveFileSystem();
    applyPwmMode();
    LogSerial.println(F("Packet received from setuppage"));
    printerConfig.inactivityStartms = millis();
    printerConfig.isIdleOFFActive = false;
    printerConfig.replicate_update = true;
    printerConfig.maintMode_update = true;
    printerConfig.discoMode_update = true;
    printerConfig.testcolor_update = true;
    requestLedUpdate(); // runs on the main task
    request->send(200, "text/plain", "OK");
}

void sendJsonToAll(JsonDocument &doc)
{
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
}

void handleWiFiScan(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i)
    {
        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["bssid"] = WiFi.BSSIDstr(i);
        net["rssi"] = WiFi.RSSI(i);
        net["enc"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? false : true;
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void handleWiFiSetupPage(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, wifiSetup_html_gz_mime, wifiSetup_html_gz, wifiSetup_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleSubmitWiFi(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    if (!fromOwnPage(request))
    {
        return refuseForeign(request);
    }

    auto param = [request](const char *name) -> String
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : "";
        value.trim();
        return value;
    };

    // Blank fields keep the stored value, because /config.json no longer sends secrets
    // back to the page.
    String ssid = param("ssid");
    String pass = param("pass");
    String bssid = param("bssid");
    if (ssid.length() > 0 && ssid != globalVariables.SSID && pass.length() == 0)
    {
        // The stored password belongs to the old network; don't silently reuse it.
        request->send(400, "text/plain", "Enter the WiFi password for the new network.");
        return;
    }
    if (ssid.length() > 0)
    {
        LogSerial.print(F("[WiFiSetup] SSID: "));
        LogSerial.println(ssid);
        strlcpy(globalVariables.SSID, ssid.c_str(), sizeof(globalVariables.SSID));
    }
    if (pass.length() > 0)
    {
        LogSerial.println(F("[WiFiSetup] WiFi password updated"));
        strlcpy(globalVariables.APPW, pass.c_str(), sizeof(globalVariables.APPW));
    }
    if (bssid.length() > 0)
        strlcpy(printerConfig.BSSID, bssid.c_str(), sizeof(printerConfig.BSSID));

    globalVariables.Host = param("host");

    String printerIP = param("printerIP");
    String printerSerial = param("printerSerial");
    String accessCode = param("accessCode");
    if (printerIP.length() > 0)
        strlcpy(printerConfig.printerIP, printerIP.c_str(), sizeof(printerConfig.printerIP));
    if (printerSerial.length() > 0)
        strlcpy(printerConfig.serialNumber, printerSerial.c_str(), sizeof(printerConfig.serialNumber));
    if (accessCode.length() > 0)
        strlcpy(printerConfig.accessCode, accessCode.c_str(), sizeof(printerConfig.accessCode));

    // WebUI login: an empty username turns protection off; an empty password keeps the
    // stored one.
    String webUser = param("webUser");
    String webPass = param("webPass");
    strlcpy(securityVariables.HTTPUser, webUser.c_str(), sizeof(securityVariables.HTTPUser));
    if (webUser.length() == 0)
        securityVariables.HTTPPass[0] = '\0';
    else if (webPass.length() > 0)
        strlcpy(securityVariables.HTTPPass, webPass.c_str(), sizeof(securityVariables.HTTPPass));

    saveFileSystem();

    request->send(200, "text/plain", "Settings saved, restarting...");
    shouldRestart = true;
    restartRequestTime = millis();
}

// Everything the status page, the setup page header and /api/status show.
// Field names used by the original setup page (wifi_rssi, printerConnection, ...) are kept.
void fillStatusJson(JsonDocument &doc)
{
    doc["fw"] = globalVariables.FWVersion;
#ifdef BLLED_LITE
    doc["edition"] = "lite";
#else
    doc["edition"] = "full";
#endif
    doc["host"] = globalVariables.Host;
    doc["mac"] = WiFi.macAddress(); // tells controllers apart (Home Assistant)
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime"] = millis() / 1000;
    doc["restartWhy"] = lastRestartWhy;
    doc["heap"] = ESP.getFreeHeap();
    doc["clients"] = ws.count();

    // Connection health
    doc["printerConnection"] = printerVariables.online;
    doc["mqttAuthFailed"] = printerVariables.mqttAuthFailed;
    doc["mqttConnects"] = printerVariables.mqttConnects;
    doc["wifiDrops"] = printerVariables.wifiDrops;
    doc["lastMsgAgo"] = printerVariables.lastMqttMsgMs ? (long)((millis() - printerVariables.lastMqttMsgMs) / 1000) : -1;
    doc["offlineFor"] = (!printerVariables.online && printerVariables.disconnectMQTTms) ? (long)((millis() - printerVariables.disconnectMQTTms) / 1000) : 0;

    // Which printer we're set up for, and what the network scan says is really at that address
    doc["printerIP"] = printerConfig.printerIP;
    doc["serial"] = printerConfig.serialNumber;
    const char *seen = bblSerialAt(printerConfig.printerIP);
    doc["discoveredSerial"] = seen;
    doc["serialMismatch"] = strlen(seen) > 0 && strcmp(seen, printerConfig.serialNumber) != 0;
    doc["serialSeenAt"] = bblIpOfSerial(printerConfig.serialNumber); // where our printer really is
    doc["brightness"] = printerConfig.brightness;
    doc["filamentColor"] = activeFilamentHex();
    doc["quietActive"] = (bool)quietNow;
    doc["coolWaiting"] = printerVariables.coolWaiting;
    doc["timelapse"] = printerVariables.timelapse;
    doc["workLight"] = (bool)workLightOn;
    doc["preview"] = previewActive();
    doc["almostDone"] = almostDoneBlend() > 0.0f;
    doc["idleDim"] = idleDimActive;
    doc["spotlight"] = spotlightWanted();
    if (clockSynced())
    {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        char clock[8];
        snprintf(clock, sizeof(clock), "%02d:%02d", t.tm_hour, t.tm_min);
        doc["clock"] = clock;
    }

    // Printer
    doc["stg_cur"] = printerVariables.stage;
    doc["gcodeState"] = printerVariables.gcodeState;
    doc["doorOpen"] = printerVariables.doorOpen;
    doc["chamberLight"] = printerVariables.printerledstate;
    bool hmsDismissed = printerVariables.dismissedHms && printerVariables.parsedHMScode == printerVariables.dismissedHms;
    doc["hms"] = hmsDismissed ? "" : printerVariables.parsedHMSlevel;
    if (printerVariables.parsedHMSlevel.length() > 0 && !hmsDismissed)
    {
        uint64_t c = printerVariables.parsedHMScode;
        char code[24];
        snprintf(code, sizeof(code), "%04X_%04X_%04X_%04X",
                 (int)(c >> 48), (int)((c >> 32) & 0xFFFF), (int)((c >> 16) & 0xFFFF), (int)(c & 0xFFFF));
        doc["hmsCode"] = code;
    }
    if (printerVariables.printError)
    {
        char pe[12];
        snprintf(pe, sizeof(pe), "%04X_%04X", (unsigned)(printerVariables.printError >> 16), (unsigned)(printerVariables.printError & 0xFFFF));
        doc["printError"] = pe;
    }
    doc["h2"] = printerVariables.perNozzleSlots;
#ifndef BLLED_LITE
    doc["canDismiss"] = true;
#endif // picks the H2 wording of an error description
    doc["percent"] = printerVariables.percent;
    doc["remainingMins"] = printerVariables.remainingMins;
    doc["layer"] = printerVariables.layer;
    doc["totalLayers"] = printerVariables.totalLayers;
    doc["nozzleTemp"] = printerVariables.nozzleTemp;
    doc["nozzleTarget"] = printerVariables.nozzleTarget;
    doc["bedTemp"] = printerVariables.bedTemp;
    doc["bedTarget"] = printerVariables.bedTarget;
    doc["chamberTemp"] = printerVariables.chamberTemp;
    doc["job"] = printerVariables.jobName;

    // What the strip is showing
    char hex[8];
    int ww, cw;
    const char *fx;
    ledGetOutput(hex, sizeof(hex), ww, cw, fx);
    doc["ledRGB"] = hex;
    doc["ledWW"] = ww;
    doc["ledCW"] = cw;
    doc["ledFx"] = fx;
    doc["mode"] = printerConfig.maintMode ? "maintenance" : printerConfig.discoMode ? "rgbcycle" : printerConfig.testcolorEnabled ? "test" : printerConfig.debugwifi ? "wifidebug" : printerConfig.isIdleOFFActive ? "idle-off" : "auto";
}

void websocketLoop()
{
    if (ws.count() == 0)
        return;
    if (millis() - lastWsPush > wsPushInterval)
    {
        lastWsPush = millis();
        ws.cleanupClients();

        JsonDocument doc;
        fillStatusJson(doc);
        sendJsonToAll(doc);
    }
}

void handleApiStatus(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    JsonDocument doc;
    fillStatusJson(doc);
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// One-tap fix: replace the saved serial with the one the printer at the configured IP announces
void handleUseDiscoveredSerial(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    if (!fromOwnPage(request))
        return refuseForeign(request);

    const char *seen = bblSerialAt(printerConfig.printerIP);
    if (strlen(seen) == 0)
    {
        request->send(409, "text/plain", "No printer has answered from that address yet.");
        return;
    }
    LogSerial.printf("[Setup] Serial changed from %s to %s (announced by %s). Restarting...\n",
                     printerConfig.serialNumber, seen, printerConfig.printerIP);
    strlcpy(printerConfig.serialNumber, seen, sizeof(printerConfig.serialNumber));
    saveFileSystem();
    request->send(200, "text/plain", "Serial updated. Restarting...");
    shouldRestart = true;
    restartRequestTime = millis();
}

// Quick controls from the Status page: light mode and brightness, saved like the settings page
void handleQuick(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    if (!fromOwnPage(request))
        return refuseForeign(request);

    if (request->hasParam("mode", true))
    {
        String mode = request->getParam("mode", true)->value();
        if (mode != "auto" && mode != "white" && mode != "rainbow")
        {
            request->send(400, "text/plain", "Unknown mode");
            return;
        }
        printerConfig.maintMode = (mode == "white");
        printerConfig.discoMode = (mode == "rainbow");
        printerConfig.testcolorEnabled = false;
        printerConfig.debugwifi = false;
        printerConfig.replicatestate = (mode == "auto"); // the modes are pick-one, as on the settings page
        printerConfig.isIdleOFFActive = false;
        printerConfig.inactivityStartms = millis();
    }
    if (request->hasParam("brightness", true))
    {
        printerConfig.brightness = constrain((int)request->getParam("brightness", true)->value().toInt(), 0, 100);
    }

    saveFileSystem();
    printerConfig.replicate_update = true;
    printerConfig.maintMode_update = true;
    printerConfig.discoMode_update = true;
    printerConfig.testcolor_update = true;
    requestLedUpdate();
    request->send(200, "text/plain", "OK");
}

// Preview a color + effect on the strip from the settings page (nothing is saved)
void handlePreview(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    if (!fromOwnPage(request))
        return refuseForeign(request);
    auto param = [&](const char *n, const char *d) -> String {
        return request->hasParam(n, true) ? request->getParam(n, true)->value() : String(d);
    };
    COLOR c;
    String what = param("special", "");
    if (what != "calibrate")
        calTrying = false; // other previews use the saved balance
    if (what == "filament")
    {
        // The real active spool's color, or a sample purple when none is loaded
        if (!filamentLedColor(c))
            c = hex2rgb("#7A1FD1");
        startPreview(c, FX_PULSE, 10000, false);
    }
    else if (what == "amscount")
    {
        // The AMS pulse count: the real spool and its AMS when known, otherwise AMS 2 in purple
        int unit = activeFilamentUnit();
        if (!filamentLedColor(c))
            c = hex2rgb("#7A1FD1");
        ledPulseCount = unit > 0 ? unit : 2;
        startPreview(c, FX_COUNT, 12000, false);
    }
    else if (what == "celebrate")
    {
        startPreview(printerConfig.finishColor, printerConfig.finishFx, 8000, true);
    }
    else if (what == "calibrate")
    {
        // Try a color balance on the strip: the sliders send their values with the test color.
        // Save keeps them; reloading the page or restarting without saving drops them.
        calTry[0] = constrain((int)param("calR", "100").toInt(), 10, 100);
        calTry[1] = constrain((int)param("calG", "100").toInt(), 10, 100);
        calTry[2] = constrain((int)param("calB", "100").toInt(), 10, 100);
        calTrying = true;
        c = hex2rgb(param("rgb", "#FF8000"));
        startPreview(c, FX_SOLID, 20000, false);
    }
    else if (what == "heat")
    {
        // The whole heat-up, cold to hot, then hold "hot" for a moment
        startPreview(heatColorAt(0.0f), FX_HEATDEMO, 8500, false);
    }
    else
    {
        c = hex2rgb(param("rgb", "#000000"), constrain((int)param("ww", "0").toInt(), 0, 255), constrain((int)param("cw", "0").toInt(), 0, 255));
        int fx = constrain((int)param("fx", "0").toInt(), 0, 5);
        startPreview(c, (uint8_t)fx, 10000, false);
    }
    request->send(200, "text/plain", "OK");
}

void handleStatusPage(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    AsyncWebServerResponse *response = request->beginResponse(200, statusPage_html_gz_mime, statusPage_html_gz, statusPage_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleConfigPage(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    AsyncWebServerResponse *response = request->beginResponse(200, backupRestore_html_gz_mime, backupRestore_html_gz, backupRestore_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handleDownloadConfigFile(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }

    if (!LittleFS.exists(configPath))
    {
        request->send(404, "text/plain", "Config file not found");
        return;
    }

    File configFile = LittleFS.open(configPath, "r");
    if (!configFile)
    {
        request->send(500, "text/plain", "Failed to open config file");
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error)
    {
        request->send(500, "text/plain", "Failed to parse config file");
        return;
    }

    String jsonString;
    serializeJsonPretty(doc, jsonString);

    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", jsonString);
    response->addHeader("Content-Disposition", "attachment; filename=\"blledconfig.json\"");
    request->send(response);
}

void handleWebSerialPage(AsyncWebServerRequest *request)
#ifdef BLLED_LITE
{
    request->redirect("/status#logCard"); // lite edition: the status page's Activity log instead
}
#else
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    AsyncWebServerResponse *response = request->beginResponse(200, webSerialPage_html_gz_mime, webSerialPage_html_gz, webSerialPage_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}
#endif

void handlePrinterList(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return request->requestAuthentication();
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < bblKnownPrinterCount; i++)
    {
        JsonObject obj = arr.add<JsonObject>();
        obj["ip"] = bblLastKnownPrinters[i].ip.toString();
        obj["usn"] = bblLastKnownPrinters[i].usn;
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// Opening /factoryreset in a browser shows a confirm button; the reset itself is a POST from
// that page (a link or image on another site can't trigger it).
void handleFactoryResetPage(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    request->send(200, "text/html",
                  // (kept short: every byte counts in the lite edition)
                  "<!DOCTYPE html><meta name=viewport content='width=device-width'><title>Factory reset</title>"
                  "<body style=font-family:sans-serif><h1>Factory reset</h1><p>Erases all settings and restarts the controller.<p>"
                  "<button onclick=\"fetch('/factoryreset',{method:'POST',headers:{'X-BLLED':1}}).then(r=>r.text(),()=>'Restarting')"
                  ".then(t=>m.textContent=t)\">Erase all settings</button><p id=m role=status>");
}

void handleFactoryReset(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
        return request->requestAuthentication();
    if (!fromOwnPage(request))
        return refuseForeign(request);

    LogSerial.println(F("[FactoryReset] Performing full reset..."));

    deleteFileSystem(); // delete LittleFS config
    request->send(200, "text/plain", "Factory reset complete. Restarting...");

    shouldRestart = true;
    restartRequestTime = millis();
}

void handleUploadConfigFileData(AsyncWebServerRequest *request, const String &filename,
                                size_t index, uint8_t *data, size_t len, bool final)
{
    static File uploadFile;

    if (!index)
    {
        LogSerial.println(F("[ConfigUpload] Start"));
        uploadFile = LittleFS.open(configPath, "w");
    }
    if (uploadFile)
    {
        uploadFile.write(data, len);
    }
    if (final)
    {
        uploadFile.close();
        LogSerial.println(F("[ConfigUpload] Finished"));
    }
    shouldRestart = true;
    restartRequestTime = millis();
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len)
{
    switch (type)
    {
    case WS_EVT_CONNECT:
        LogSerial.printf("[WS] Client connected: %u\n", client->id());
        lastWsPush = 0; // the main task sends a fresh update on its next pass
        break;
    case WS_EVT_DISCONNECT:
        LogSerial.printf("[WS] Client disconnected: %u\n", client->id());
        ws.cleanupClients();
        break;
    case WS_EVT_DATA:
        LogSerial.printf("[WS] Data received from client %u\n", client->id());
        break;
    case WS_EVT_PONG:
        LogSerial.printf("[WS] Pong received from %u\n", client->id());
        break;
    case WS_EVT_ERROR:
        LogSerial.printf("[WS] Error on connection %u\n", client->id());
        ws.cleanupClients();
        break;
    }
}

// ---------------------------------------------------------------------------
//  Settings-page self-check: once a minute the controller loads its own page. If that keeps
//  failing while Wi-Fi is fine, the web server has frozen (the lights would still work), so the
//  controller restarts - but never in the middle of a print, and only if the check has worked
//  before (so a network that can't do the check never causes restarts).
// ---------------------------------------------------------------------------
static bool webCheckOnce()
{
    WiFiClient c;
    c.setTimeout(4);
    if (!c.connect(WiFi.localIP(), 80, 4000))
        return false;
    c.print("GET /api/ping HTTP/1.0\r\nHost: blled\r\nConnection: close\r\n\r\n");
    unsigned long t0 = millis();
    while (!c.available() && c.connected() && millis() - t0 < 5000)
        vTaskDelay(pdMS_TO_TICKS(20));
    String line = c.readStringUntil('\n');
    c.stop();
    return line.indexOf(" 200") > 0;
}

static void webHealthTask(void *)
{
    bool everWorked = false;
    int fails = 0;
    vTaskDelay(pdMS_TO_TICKS(90000)); // let everything settle after boot
    for (;;)
    {
        if (otaActive)
            fails = 0; // a long upload keeps the page busy; that isn't a freeze
        else if (WiFi.status() == WL_CONNECTED && !(WiFi.getMode() & WIFI_AP))
        {
            if (webCheckOnce())
            {
                if (fails)
                    LogSerial.printf("[Web] Settings page answering again after %d failed checks\n", fails);
                everWorked = true;
                fails = 0;
            }
            else if (everWorked)
            {
                fails++;
                LogSerial.printf("[Web] Settings page didn't answer its own check (%d in a row)\n", fails);
                bool printing = printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE" ||
                                printerVariables.gcodeState == "PREPARE";
                if (fails >= 3 && !printing && !shouldRestart)
                {
                    LogSerial.println(F("[Web] Settings page is frozen - restarting the controller"));
                    noteRestart("Settings page froze");
                    restartRequestTime = millis();
                    shouldRestart = true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void startWebHealthCheck()
{
    xTaskCreate(webHealthTask, "webHealth", 4096, NULL, 1, NULL);
}

void setupWebserver()
{
    if (!MDNS.begin(globalVariables.Host.c_str()))
    {
        LogSerial.println(F("Error setting up MDNS responder!"));
        while (1)
            delay(500);
    }
#ifndef BLLED_LITE // (lite: no room - add it in Home Assistant by its address)
    MDNS.addService("blled", "tcp", 80); // Home Assistant finds it by this
#endif

    LogSerial.println(F("Setting up webserver"));

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
    if (WiFi.getMode() == WIFI_AP) {
        LogSerial.println(F("[WebServer] Captive Portal activ – redirect to /wifi"));
        request->redirect("/wifi");
    } else {
        handleSetup(request);
    } });
    webServer.on("/status", HTTP_GET, handleStatusPage);
    webServer.on("/api/status", HTTP_GET, handleApiStatus);
#ifndef BLLED_LITE
    // What each printer error code means (the lite edition's status page reads it from GitHub)
    webServer.on("/hms.json", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        AsyncWebServerResponse *response = request->beginResponse(200, "application/json", hmsTableGz, hmsTableGzLen);
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "max-age=604800");
        request->send(response); });
#endif
#ifndef BLLED_LITE
    // Notifications: how the last send went, and a test message from the settings page
    webServer.on("/api/notify", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        JsonDocument d;
        notifyStatusJson(d.to<JsonObject>());
        String s;
        serializeJson(d, s);
        request->send(200, "application/json", s); });
    webServer.on("/api/notify/test", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        if (!fromOwnPage(request))
            return refuseForeign(request);
        String event = request->hasParam("event", true) ? request->getParam("event", true)->value() : String("test");
        String service = request->hasParam("service", true) ? request->getParam("service", true)->value() : String("all");
        int r = notifyTest(event, service);
        if (r == 1)
            return request->send(409, "text/plain", service == "all" ? "Set up Discord, ntfy or a webhook and save first" : "That one isn't set up yet - fill it in and save first");
        if (r == 2)
            return request->send(409, "text/plain", "That kind isn't ticked for any service that's set up (see Where each one goes)");
        request->send(200, "text/plain", "Sending"); });
#endif
    webServer.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request)
                 { request->send(200, "text/plain", "ok"); });
    // How much of a firmware upload has actually arrived (the browser's own count runs ahead)
    webServer.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        char b[96];
        snprintf(b, sizeof(b), "{\"active\":%s,\"bytes\":%lu,\"total\":%lu}", otaActive ? "true" : "false",
                 (unsigned long)otaBytes, (unsigned long)otaTotal);
        request->send(200, "application/json", b); });
    // Activity log: recent log lines (kept across restarts)
    webServer.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        AsyncWebServerResponse *r = request->beginResponse(200, "text/plain; charset=utf-8", activityLogText());
        r->addHeader("Cache-Control", "no-store");
        request->send(r); });
    // Remote fixes from the status page: restart the controller, or just reconnect to the printer
    webServer.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        if (!fromOwnPage(request))
            return refuseForeign(request);
        if (otaActive)
            return request->send(409, "text/plain", "An update is installing - wait for it to finish");
        noteRestart("Restarted from the status page");
        shouldRestart = true;
        restartRequestTime = millis();
        request->send(200, "text/plain", "Restarting"); });
#ifndef BLLED_LITE
    // "Dismiss" on the status page's error alert: the lights stop showing this error
    webServer.on("/api/dismiss-error", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        if (!fromOwnPage(request))
            return refuseForeign(request);
        if (!printerVariables.hmsstate)
            return request->send(409, "text/plain", "No error to dismiss");
        printerVariables.dismissedHms = printerVariables.parsedHMScode; // the light code and the next report skip it
        printerConfig.replicate_update = true;
        LogSerial.printf("[LED] Error %04X_%04X_%04X_%04X dismissed - the lights ignore it until the printer clears it\n",
                         (int)(printerVariables.dismissedHms >> 48), (int)((printerVariables.dismissedHms >> 32) & 0xFFFF),
                         (int)((printerVariables.dismissedHms >> 16) & 0xFFFF), (int)(printerVariables.dismissedHms & 0xFFFF));
        requestLedUpdate();
        request->send(200, "text/plain", "Dismissed"); });
#endif
    webServer.on("/api/reconnect", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        if (!fromOwnPage(request))
            return refuseForeign(request);
        mqttReconnectNow = true;
        request->send(200, "text/plain", "Reconnecting"); });
#ifdef BLLED_LITE
    // Moving to the larger storage layout (lite edition on the original layout only)
    webServer.on("/api/layout", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        JsonDocument d;
        d["oldLayout"] = lmOnOldLayout();
        d["busy"] = (bool)lmBusy;
        d["step"] = lmStep;
        d["error"] = lmError;
        d["done"] = lmDone;
        d["bytes"] = (unsigned long)otaBytes;
        d["total"] = (unsigned long)otaTotal;
        String out;
        serializeJson(d, out);
        request->send(200, "application/json", out); });
    webServer.on("/api/layout", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        if (!fromOwnPage(request))
            return refuseForeign(request);
        if (!lmStart())
            return request->send(409, "text/plain", lmOnOldLayout() ? "Busy right now (an update may be running) - try again in a moment" : "Already on the larger layout");
        request->send(202, "text/plain", "Started"); });
#endif
    // Updates from GitHub: state, and check / install on request
    webServer.on("/api/github-update", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        JsonDocument d;
        d["state"] = suStateName[suState];
        d["busy"] = suBusy || suBootMode >= 0; // also while a job waits to run after a restart
        d["busyFor"] = suBusy ? (long)((millis() - suStartedMs) / 1000) : 0;
        d["running"] = STRVERSION;
        d["latest"] = suLatest;
        d["newer"] = suUpdateAvailable();
        d["hasFile"] = suAssetUrl[0] != 0;
        d["signed"] = suSigUrl[0] != 0;
        d["error"] = suError;
        d["memRestart"] = suMemRestart;
        d["rolledBack"] = suRolledBack;
        d["bad"] = suBadTag;
        d["bytes"] = (unsigned long)otaBytes;
        d["total"] = (unsigned long)otaTotal;
        d["auto"] = printerConfig.autoUpdate;
        d["checkedAgo"] = suChecked ? (long)((millis() - suLastCheckMs) / 1000) : -1;
        String out;
        serializeJson(d, out);
        request->send(200, "application/json", out); });
    webServer.on("/api/github-update", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request))
            return request->requestAuthentication();
        if (!fromOwnPage(request))
            return refuseForeign(request);
        String what = request->hasParam("do", true) ? request->getParam("do", true)->value() : "check";
        int mode = what == "install" ? 1 : what == "reinstall" ? 2 : 0;
        if (!suStart(mode, true))
            return request->send(409, "text/plain", suBusy ? "Already working on it" : "Busy right now - try again in a moment");
        request->send(202, "text/plain", "Started"); });
    webServer.on("/api/useDiscoveredSerial", HTTP_POST, handleUseDiscoveredSerial);
    webServer.on("/api/quick", HTTP_POST, handleQuick);
    webServer.on("/api/preview", HTTP_POST, handlePreview);
    webServer.on("/fwupdate", HTTP_GET, handleUpdatePage);
    webServer.on("/update", HTTP_GET, handleUpdatePage); // the address people expect; uploads POST here
    webServer.on("/getConfig", HTTP_GET, handleGetConfig);
    webServer.on("/submitConfig", HTTP_POST, handleSubmitConfig);
    webServer.on("/blled.svg", HTTP_GET, handleGetIcon);
    webServer.on("/favicon.ico", HTTP_GET, handleGetfavicon);
    webServer.on("/particleCanvas.js", HTTP_GET, handleGetPCC);
    webServer.on("/config.json", HTTP_GET, handlePrinterConfigJson);
    webServer.on("/wifi", HTTP_GET, handleWiFiSetupPage);
    webServer.on("/wifiScan", HTTP_GET, handleWiFiScan);
    webServer.on("/submitWiFi", HTTP_POST, handleSubmitWiFi);
    webServer.on("/style.css", HTTP_GET, handleStyleCss);
    webServer.on("/backuprestore", HTTP_GET, handleConfigPage);
    webServer.on("/configfile.json", HTTP_GET, handleDownloadConfigFile);
    webServer.on("/webserial", HTTP_GET, handleWebSerialPage);
    webServer.on("/printerList", HTTP_GET, handlePrinterList);
    webServer.on("/factoryreset", HTTP_GET, handleFactoryResetPage);
    webServer.on("/factoryreset", HTTP_POST, handleFactoryReset);
    // Uploads: the upload callback runs before the completion handler, so the login has
    // to be checked there too or the data is written before anyone is authenticated.
    webServer.on("/configrestore", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request)) {
            return request->requestAuthentication();
        }
        if (!fromOwnPage(request)) {
            return refuseForeign(request);
        }
        request->send(200, "text/plain", "Config uploaded. Restarting...");
        shouldRestart = true;
        restartRequestTime = millis(); }, [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
                 {
        static File uploadFile;
        static bool uploadAuthorized = false;

        if (!index) {
            uploadAuthorized = isAuthorized(request) && fromOwnPage(request);
            if (!uploadAuthorized) {
                LogSerial.println(F("[ConfigUpload] Rejected: not authorized, or not from the controller's pages"));
                return;
            }
            LogSerial.printf("[ConfigUpload] Start: %s\n", filename.c_str());
            uploadFile = LittleFS.open(configPath, "w");
        }
        if (!uploadAuthorized) {
            return;
        }
        if (uploadFile) {
            uploadFile.write(data, len);
        }
        if (final) {
            uploadFile.close();
            LogSerial.println(F("[ConfigUpload] Finished"));
        } });

    webServer.on("/update", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
        if (!isAuthorized(request)) {
            return request->requestAuthentication();
        }
        if (!fromOwnPage(request)) {
            return refuseForeign(request);
        }
        // Report a failed update instead of restarting into the old firmware as if it worked
        if (Update.hasError() || !Update.isFinished()) {
            String why = Update.hasError() ? String(Update.errorString()) : String("the file was incomplete");
            if (Update.getError() == UPDATE_ERROR_SPACE)
                why = "this file is too big for this controller's storage layout - use the -lite file instead";
            LogSerial.printf("[OTA] Failed: %s\n", why.c_str());
            Update.abort();
            otaActive = false;
            return request->send(500, "text/plain", "Update failed: " + why + ". The controller is still on its current version - try again.");
        }
        request->send(200, "text/plain", "OK");
        LogSerial.println(F("OTA Upload done. Marking for restart."));
        shouldRestart = true;
        restartRequestTime = millis(); }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
                 {
        static bool otaAuthorized = false;

        if (!index) {
            otaAuthorized = isAuthorized(request) && fromOwnPage(request);
            if (!otaAuthorized) {
                LogSerial.println(F("[OTA] Rejected: not authorized, or not from the controller's pages"));
                return;
            }
            LogSerial.printf("[OTA] Start: %s\n", filename.c_str());
            otaTotal = request->contentLength();
            otaBytes = 0;
            otaActive = true;
            // An earlier upload that was cut off leaves its update session open. Starting a new one
            // on top of it fails ("No Error"), the new file gets appended to the old half-written
            // one, and the result can't be activated. Close any leftover session first.
            if (Update.isRunning()) {
                LogSerial.println(F("[OTA] Closing an unfinished earlier upload"));
                Update.abort();
            }
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(LogSerial);
            }
        }
        if (!otaAuthorized) {
            return;
        }

        otaLastData = millis();
        otaBytes = index + len;
        if (Update.write(data, len) != len) {
            Update.printError(LogSerial);
        }

        if (final) {
            if (Update.end(true)) {
                LogSerial.printf("[OTA] Success (%u bytes). Awaiting reboot...\n", index + len);
            } else {
                Update.printError(LogSerial);
            }
        } });

    // The log can contain the printer IP/serial; protect it like the other pages.
    LogSerial.setAuthentication(securityVariables.HTTPUser, securityVariables.HTTPPass);
    LogSerial.begin(&webServer);

    ws.onEvent(onWsEvent);
    webServer.addHandler(&ws);

    webServer.begin();

    LogSerial.println(F("Webserver started"));
}

#endif
