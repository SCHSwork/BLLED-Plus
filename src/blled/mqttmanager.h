#ifndef _MQTTMANAGER
#define _MQTTMANAGER

#define TASK_RAM 20480 // 20kB for MQTT Task Stack Size

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "mqttparsingutility.h"
#include "AutoGrowBufferStream.h"
#include "types.h"
#include "leds.h"

WiFiClientSecure wifiSecureClient;
PubSubClient mqttClient(wifiSecureClient);

String device_topic;
String report_topic;
String request_topic;
String clientId = "BLLED-";

AutoGrowBufferStream stream;

unsigned long lastMQTTupdate = millis();

TaskHandle_t mqttTaskHandle = NULL;
bool mqttTaskRunning = false;

// ---------------------------------------------------------------------------
//  Connection management
//
//  - Reconnects back off exponentially (2 s -> 60 s) instead of hammering the
//    printer every 3 s; a refused login waits the full 60 s.
//  - A watchdog drops and rebuilds a connection that has gone silent.
//  - After every connect the printer is asked for a full status report
//    ("pushall"), so the LEDs are right immediately instead of waiting for the
//    next change.
//  - Only this task ever touches mqttClient. PubSubClient is not thread-safe,
//    so other tasks queue chamber-light commands instead of publishing directly.
// ---------------------------------------------------------------------------

const uint32_t MQTT_BACKOFF_MIN_MS = 2000;
const uint32_t MQTT_BACKOFF_MAX_MS = 60000;
const uint32_t MQTT_SILENCE_PROBE_MS = 90000;    // ask for a report after this much silence
const uint32_t MQTT_SILENCE_TIMEOUT_MS = 150000;  // reconnect if the ask went unanswered too

uint32_t mqttBackoffMs = MQTT_BACKOFF_MIN_MS;
unsigned long mqttNextAttemptMs = 0;
volatile int8_t pendingChamberLight = -1; // -1 none, 0 off, 1 on
volatile bool pendingPushAll = false;

// With a Default BLLED
// Expected information when viewing MQTT status messages

// gcode_state	stg_cur	    BLLED LED control	    Comments
//------------------------------------------------------------------------
// IDLE	        -1	        White	                Printer just powered on
// RUNNING	    -1	        White	                Printer sent print file
// RUNNING	     2	        White	                PREHEATING BED
// RUNNING	    14	        OFF (for Lidar)	        CLEANING NOZZLE
// RUNNING	     1	        OFF (for Lidar)	        BED LEVELING
// RUNNING	     8	        OFF (for Lidar)	        CALIBRATING EXTRUSION
// RUNNING	     0	        White	                All the printing happens here
// FINISH	    -1	        Green	                After bed is lowered and filament retracted
// FINISH	    -1	        Green	                BLLED logic waits for a door interaction
// FINISH	    -1	        White	                After door interaction
// FINISH	    -1	        OFF                     Inactivity after 30mins

bool connectMqtt()
{
    if (WiFi.status() != WL_CONNECTED || WiFi.getMode() != WIFI_MODE_STA)
        return false;

    if (strlen(printerConfig.printerIP) == 0 || strlen(printerConfig.accessCode) == 0)
    {
        static unsigned long lastWarn = 0;
        if (millis() - lastWarn > 60000)
        {
            lastWarn = millis();
            LogSerial.println(F("[MQTT] Not connecting: printer IP or access code is empty"));
        }
        return false;
    }

    LogSerial.printf("[MQTT] Connecting to %s ...\n", printerConfig.printerIP);
    unsigned long t0 = millis();

    if (mqttClient.connect(clientId.c_str(), "bblp", printerConfig.accessCode))
    {
        LogSerial.printf("[MQTT] Connected in %lu ms, subscribing to %s\n", millis() - t0, report_topic.c_str());
        mqttClient.subscribe(report_topic.c_str());
        printerVariables.online = true;
        printerVariables.mqttAuthFailed = false;
        printerVariables.disconnectMQTTms = 0;
        printerVariables.lastMqttMsgMs = millis();
        printerVariables.mqttConnects++;
        printerVariables.lightKnownMs = 0; // light state unknown until the printer reports it
        pendingPushAll = true;
        requestLedUpdate();
        return true;
    }

    int state = mqttClient.state();
    LogSerial.printf("[MQTT] Connect failed after %lu ms, state %d: ", millis() - t0, state);
    ParseMQTTState(state);

    bool authFailed = (state == MQTT_CONNECT_BAD_CREDENTIALS || state == MQTT_CONNECT_UNAUTHORIZED);
    if (authFailed != printerVariables.mqttAuthFailed)
    {
        printerVariables.mqttAuthFailed = authFailed;
        requestLedUpdate();
    }
    if (authFailed)
    {
        LogSerial.println(F("[MQTT] Printer refused the access code - check it on the printer screen (it changes if LAN mode is toggled)"));
    }
    return false;
}

void publishChamberLight(bool on)
{
    JsonDocument doc;
    JsonObject system = doc["system"].to<JsonObject>();
    system["command"] = "ledctrl";
    system["sequence_id"] = "blled_auto";
    system["led_node"] = "chamber_light";
    system["led_mode"] = on ? "on" : "off";
    system["led_on_time"] = 500;
    system["led_off_time"] = 500;
    system["loop_times"] = 0;
    system["interval_time"] = 1000;

    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(request_topic.c_str(), payload.c_str());

    LogSerial.printf("[MQTT] Chamber Light %s sent\n", on ? "ON" : "OFF");
}

void publishPushAll()
{
    // Ask the printer for a complete status report
    mqttClient.publish(request_topic.c_str(), "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}");
    if (printerConfig.debugingchange)
        LogSerial.println(F("[MQTT] Requested full status (pushall)"));
}

void mqttTask(void *parameter)
{
    mqttTaskRunning = true;

    while (true)
    {
        if (otaActive)
        {
            // Firmware update uploading: stay out of its way (no TLS work, no parsing)
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (WiFi.status() != WL_CONNECTED || WiFi.getMode() != WIFI_MODE_STA)
        {
            if (mqttClient.connected())
                mqttClient.disconnect();
            if (printerVariables.online)
            {
                printerVariables.online = false;
                printerVariables.disconnectMQTTms = millis();
                requestLedUpdate();
            }
            mqttBackoffMs = MQTT_BACKOFF_MIN_MS;
            mqttNextAttemptMs = millis() + 1000; // give Wi-Fi a moment once it's back
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!mqttClient.connected())
        {
            if (printerVariables.online || printerVariables.disconnectMQTTms == 0)
            {
                printerVariables.online = false;
                printerVariables.disconnectMQTTms = millis();
                LogSerial.println(F("[MQTT Task] Disconnected"));
                ParseMQTTState(mqttClient.state());
                requestLedUpdate();
            }
            pendingChamberLight = -1; // don't replay stale commands after a reconnect

            if ((long)(millis() - mqttNextAttemptMs) >= 0)
            {
                if (connectMqtt())
                {
                    mqttBackoffMs = MQTT_BACKOFF_MIN_MS;
                }
                else
                {
                    uint32_t wait = printerVariables.mqttAuthFailed ? MQTT_BACKOFF_MAX_MS : mqttBackoffMs;
                    mqttNextAttemptMs = millis() + wait;
                    LogSerial.printf("[MQTT] Next attempt in %lu s\n", (unsigned long)(wait / 1000));
                    mqttBackoffMs = min(mqttBackoffMs * 2, MQTT_BACKOFF_MAX_MS);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        mqttClient.loop();

        if (pendingPushAll)
        {
            pendingPushAll = false;
            publishPushAll();
        }

        int8_t light = pendingChamberLight;
        if (light >= 0)
        {
            pendingChamberLight = -1;
            publishChamberLight(light == 1);
        }

        // Watchdog: a TCP connection can look alive while the printer has stopped talking to us.
        // Some printers go quiet when idle, so first ask for a report, and only reconnect if
        // that gets no answer either.
        static bool silenceProbeSent = false;
        unsigned long quietMs = millis() - printerVariables.lastMqttMsgMs;
        if (quietMs < MQTT_SILENCE_PROBE_MS)
        {
            silenceProbeSent = false;
        }
        else if (!silenceProbeSent)
        {
            silenceProbeSent = true;
            if (printerConfig.debugingchange)
                LogSerial.println(F("[MQTT] Printer quiet for 90 s - asking for a status report"));
            publishPushAll();
        }
        else if (quietMs > MQTT_SILENCE_TIMEOUT_MS)
        {
            LogSerial.printf("[MQTT] No data for %lu s - reconnecting\n", quietMs / 1000);
            silenceProbeSent = false;
            mqttClient.disconnect();
            mqttNextAttemptMs = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    mqttTaskRunning = false;
    vTaskDelete(NULL);
}

void ParseCallback(char *topic, byte *payload, unsigned int length)
{
    printerVariables.lastMqttMsgMs = millis();

    JsonDocument messageobject;
    JsonDocument filter;
    // Rather than showing the entire message to Serial - grabbing only the pertinent bits for BLLED.
    // Device Status
    filter["print"]["command"] = true;
    filter["print"]["fail_reason"] = true;
    filter["print"]["gcode_state"] = true;
    filter["print"]["print_gcode_action"] = true;
    filter["print"]["print_real_action"] = true;
    filter["print"]["hms"] = true;
    filter["print"]["home_flag"] = true;
    filter["print"]["lights_report"] = true;
    filter["print"]["stg_cur"] = true;
    filter["print"]["print_error"] = true;
    filter["print"]["wifi_signal"] = true;
    // Live print data (status page + progress effect)
    filter["print"]["mc_percent"] = true;
    filter["print"]["mc_remaining_time"] = true;
    filter["print"]["layer_num"] = true;
    filter["print"]["total_layer_num"] = true;
    filter["print"]["nozzle_temper"] = true;
    filter["print"]["nozzle_target_temper"] = true;
    filter["print"]["bed_temper"] = true;
    filter["print"]["bed_target_temper"] = true;
    filter["print"]["chamber_temper"] = true;
    filter["print"]["subtask_name"] = true;
    // Filament colors: AMS units/slots, the active slot, and the external spool
    filter["print"]["ams"]["tray_now"] = true;
    filter["print"]["ams"]["tray_tar"] = true;
    filter["print"]["ams"]["ams"][0]["id"] = true;
    filter["print"]["ams"]["ams"][0]["tray"][0]["id"] = true;
    filter["print"]["ams"]["ams"][0]["tray"][0]["tray_color"] = true;
    filter["print"]["vt_tray"]["tray_color"] = true;
    // H2 series: active slot per nozzle, and the external spools
    filter["print"]["device"]["extruder"]["state"] = true;
    filter["print"]["device"]["extruder"]["info"][0]["id"] = true;
    filter["print"]["device"]["extruder"]["info"][0]["snow"] = true;
    filter["print"]["device"]["extruder"]["info"][0]["star"] = true;
    filter["print"]["device"]["extruder"]["info"][0]["temp"] = true;
    filter["print"]["vir_slot"][0]["id"] = true;
    filter["print"]["vir_slot"][0]["tray_color"] = true;
    filter["print"]["ipcam"]["timelapse"] = true;
    filter["system"]["command"] = true;
    filter["system"]["led_mode"] = true;

    auto deserializeError = deserializeJson(messageobject, payload, length, DeserializationOption::Filter(filter));
    if (deserializeError)
    {
        LogSerial.print(F("Deserialize error while parsing mqtt: "));
        LogSerial.println(deserializeError.c_str());
        return;
    }

    if (printerConfig.debuging)
    {
        LogSerial.printf("Mqtt message received. FreeHeap: %u\n", ESP.getFreeHeap());
    }

    bool Changed = false;

    if (!messageobject["print"]["command"].isNull())
    {
        if (messageobject["print"]["command"] == "gcode_line"           // gcode_line used a lot during print initialisations - Skip these
            || messageobject["print"]["command"] == "project_prepare"   // 1 message per print
            || messageobject["print"]["command"] == "project_file"      // 1 message per print
            || messageobject["print"]["command"] == "clean_print_error" // During error (no info)
            || messageobject["print"]["command"] == "resume"            // After error or pause
            || messageobject["print"]["command"] == "get_accessories"   // After error or pause
            || messageobject["print"]["command"] == "prepare" || messageobject["print"]["command"] == "extrusion_cali_get")
        { // 1 message per print
            return;
        }
    }
    if (messageobject.size() == 0)
    {
        // Null or Filtered message that are not 'Print' or 'System' payload - Ignore these
        return;
    }

    bool filamentChanged = false;
    bool targetChanged = false; // bed / nozzle target temperature set or cleared

    // Live print data - recorded even in override modes so the status page stays current
    JsonObject print = messageobject["print"];
    if (!print.isNull())
    {
        if (!print["mc_percent"].isNull())
            printerVariables.percent = print["mc_percent"].as<int>();
        if (!print["mc_remaining_time"].isNull())
            printerVariables.remainingMins = print["mc_remaining_time"].as<int>();
        if (!print["layer_num"].isNull())
            printerVariables.layer = print["layer_num"].as<int>();
        if (!print["total_layer_num"].isNull())
            printerVariables.totalLayers = print["total_layer_num"].as<int>();
        if (!print["nozzle_temper"].isNull())
            printerVariables.nozzleTemp = print["nozzle_temper"].as<float>();
        if (!print["nozzle_target_temper"].isNull())
        {
            float t = print["nozzle_target_temper"].as<float>();
            if (t != printerVariables.nozzleTarget)
                targetChanged = true;
            printerVariables.nozzleTarget = t;
        }
        if (!print["bed_temper"].isNull())
            printerVariables.bedTemp = print["bed_temper"].as<float>();
        if (!print["bed_target_temper"].isNull())
        {
            float t = print["bed_target_temper"].as<float>();
            if (t != printerVariables.bedTarget)
                targetChanged = true;
            printerVariables.bedTarget = t;
        }
        if (!print["chamber_temper"].isNull())
            printerVariables.chamberTemp = print["chamber_temper"].as<float>();
        if (!print["subtask_name"].isNull())
            strlcpy(printerVariables.jobName, print["subtask_name"] | "", sizeof(printerVariables.jobName));

        // Filament colors. tray_color is "RRGGBBAA"; only the RGB part is kept.
        auto setHex = [&](char *dst, const char *src) {
            char hex[7] = "";
            if (src && strlen(src) >= 6)
            {
                memcpy(hex, src, 6);
                hex[6] = 0;
            }
            if (strcmp(dst, hex) != 0)
            {
                strcpy(dst, hex);
                return true;
            }
            return false;
        };
        const char *before = activeFilamentHex();
        char beforeHex[7];
        strlcpy(beforeHex, before, sizeof(beforeHex));

        JsonObject ams = print["ams"];
        if (!ams.isNull())
        {
            if (!ams["tray_now"].isNull())
                printerVariables.trayNow = ams["tray_now"].as<String>().toInt();
            if (!ams["tray_tar"].isNull())
                printerVariables.trayTar = ams["tray_tar"].as<String>().toInt();
            for (JsonObject unit : ams["ams"].as<JsonArray>())
            {
                int unitId = unit["id"].as<String>().toInt();
                if (unitId >= 128 && unitId < 136)
                {
                    // AMS HT: one slot
                    for (JsonObject tray : unit["tray"].as<JsonArray>())
                        setHex(printerVariables.htColor[unitId - 128], tray["tray_color"] | "");
                    continue;
                }
                if (unitId < 0 || unitId > 3)
                    continue; // only standard AMS units map onto tray_now 0-15
                for (JsonObject tray : unit["tray"].as<JsonArray>())
                {
                    int trayId = tray["id"].as<String>().toInt();
                    if (trayId < 0 || trayId > 3)
                        continue;
                    // A slot listed without a color is empty
                    setHex(printerVariables.trayColor[unitId * 4 + trayId], tray["tray_color"] | "");
                }
            }
        }
        if (!print["ipcam"]["timelapse"].isNull())
        {
            bool tl = (print["ipcam"]["timelapse"] == "enable");
            if (tl != printerVariables.timelapse)
                filamentChanged = true; // the filament effect depends on it, so refresh the strip
            printerVariables.timelapse = tl;
        }

        if (!print["vt_tray"].isNull())
            setHex(printerVariables.extColor, print["vt_tray"]["tray_color"] | "");

        // H2 series: which nozzle is active, and which slot feeds each nozzle
        JsonObject ext = print["device"]["extruder"];
        if (!ext.isNull())
        {
            if (!ext["state"].isNull())
            {
                int n = ((ext["state"].as<int>() >> 4) & 0xF) ? 1 : 0;
                // Switched nozzles during a print: show the nozzle swap for a few seconds
                if (n != printerVariables.activeNozzle && printerVariables.gcodeState == "RUNNING")
                {
                    printerVariables.nozzleSwapUntil = millis() + 8000;
                    if (printerVariables.nozzleSwapUntil == 0)
                        printerVariables.nozzleSwapUntil = 1;
                    Changed = true;
                }
                printerVariables.activeNozzle = n;
            }
            for (JsonObject e : ext["info"].as<JsonArray>())
            {
                int id = e["id"] | -1;
                if (id < 0 || id > 1)
                    continue;
                if (!e["snow"].isNull())
                {
                    printerVariables.nozzleNow[id] = e["snow"].as<int>();
                    printerVariables.perNozzleSlots = true;
                }
                if (!e["star"].isNull())
                    printerVariables.nozzleTar[id] = e["star"].as<int>();
                if (!e["temp"].isNull())
                {
                    // low 16 bits: current, high 16 bits: target
                    uint32_t t = e["temp"].as<uint32_t>();
                    printerVariables.nozTemp[id] = (float)(t & 0xFFFF);
                    printerVariables.nozTarget[id] = (float)((t >> 16) & 0xFFFF);
                    printerVariables.perNozzleTemps = true;
                }
            }
        }
        for (JsonObject v : print["vir_slot"].as<JsonArray>())
        {
            int id = v["id"].as<String>().toInt();
            if (id == 255 || id == 254)
                setHex(printerVariables.virColor[255 - id], v["tray_color"] | "");
        }

        if (strcmp(beforeHex, activeFilamentHex()) != 0)
        {
            filamentChanged = true;
            // Outside a print, briefly show the new spool's color (filament preview)
            bool printingNow = printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE";
            if (!printingNow && strlen(activeFilamentHex()) == 6)
                printerVariables.filamentPreviewUntil = millis() + 30000;
            if (printerConfig.debugingchange)
                LogSerial.printf("[MQTT] Active filament color now: %s (slot %d)\n",
                                 strlen(activeFilamentHex()) ? activeFilamentHex() : "none", printerVariables.trayNow);
        }
    }

    // Output Filtered MQTT message
    if (printerConfig.mqttdebug)
    {
        LogSerial.print(F("(Filtered) MQTT payload, ["));
        LogSerial.print(stream.current_length());
        LogSerial.print(F("], "));
        serializeJson(messageobject, LogSerial);
        LogSerial.println();
    }

    if (printerConfig.maintMode || printerConfig.testcolorEnabled || printerConfig.discoMode || printerConfig.debugwifi)
    {
        if (printerConfig.mqttdebug)
        {
            LogSerial.print(F("[MQTT] Message Ignored while in "));
            if (printerConfig.maintMode)
                LogSerial.print(F("Maintenance"));
            if (printerConfig.testcolorEnabled)
                LogSerial.print(F("Test Color"));
            if (printerConfig.discoMode)
                LogSerial.print(F("RGB Cycle"));
            if (printerConfig.debugwifi)
                LogSerial.print(F("Wifi Debug"));
            LogSerial.println(F(" mode"));
        }
        return;
    }

    // Check for Door Status
    if (!messageobject["print"]["home_flag"].isNull())
    {
        long homeFlag = messageobject["print"]["home_flag"];
        bool doorState = bitRead(homeFlag, 23); // Bit 23 = door open

        if (printerVariables.doorOpen != doorState)
        {
            printerVariables.doorOpen = doorState;

            if (printerConfig.debugingchange)
            {
                LogSerial.print(F("[MQTT] Door "));
                LogSerial.println(doorState ? F("Opened") : F("Closed"));
            }

            if (doorState) // Door opened
            {
                printerVariables.lastdoorOpenms = millis();

                // Remember if the strip was dark when the door opened, so closing the door can put it
                // back to dark (the door only "borrows" the light). Not during a print.
                bool stripDark = currentRed == 0 && currentGreen == 0 && currentBlue == 0 && currentWarm == 0 && currentCold == 0;
                bool printing = printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE" || printerVariables.gcodeState == "PREPARE";
                printerVariables.ledWasForcedByDoor = stripDark && !printing;

                // If light is off, turn it on and lock it
                if (printerConfig.controlChamberLight && !printerVariables.printerledstate)
                {
                    printerVariables.chamberLightLocked = true;
                    printerVariables.printerledstate = true;
                    printerConfig.replicate_update = false;
                    controlChamberLight(true);
                    printerVariables.stage = 255;
                    LogSerial.println(F("[MQTT] Door opened – Light forced ON"));
                }
            }
            else // Door closed
            {
                printerVariables.lastdoorClosems = millis();

                if (printerConfig.controlChamberLight)
                {
                    printerVariables.chamberLightLocked = false;
                }

                if (!printerConfig.inactivityEnabled)
                {
                    // Turn off LED bar immediately
                    printerVariables.printerledstate = false;
                    printerConfig.replicate_update = false;
                    printerVariables.stage = 999;
                    requestLedOff();
                    controlChamberLight(false);
                    LogSerial.println(F("[MQTT] Door closed – LED bar OFF (inactivity disabled)"));
                }

                // Double-close detection
                if ((millis() - printerVariables.lastdoorOpenms) < 2000)
                {
                    printerVariables.doorSwitchTriggered = true;
                }
            }

            // Restart inactivity timer
            printerConfig.inactivityStartms = millis();
            printerConfig.isIdleOFFActive = false;
            Changed = true;
        }
    }

    // Check BBLP Stage
    if (!messageobject["print"]["stg_cur"].isNull())
    {
        if (printerVariables.stage != messageobject["print"]["stg_cur"].as<int>())
        {
            printerVariables.stage = messageobject["print"]["stg_cur"];

            if (printerConfig.debugingchange || printerConfig.debuging)
            {
                LogSerial.print(F("[MQTT] update - stg_cur now: "));
                LogSerial.println(printerVariables.stage);
            }
            Changed = true;
        }
    }

    // Check BBLP GCode State
    if (!messageobject["print"]["gcode_state"].isNull() && ((millis() - lastMQTTupdate) > 3000))
    {
        String mqttgcodeState = messageobject["print"]["gcode_state"].as<String>();

        if (mqttgcodeState == "RUNNING" || mqttgcodeState == "PAUSE")
        {
            // Never turn off light (due to idle timer) while in this state
            printerConfig.inactivityStartms = millis();
        }
        if (mqttgcodeState == "RUNNING" && printerConfig.controlChamberLight && printerVariables.printerledstate == false)
        {
            controlChamberLight(true); // Turn chamber light ON at print start
            LogSerial.println(F("[MQTT] Print started – Chamber Light ON requested"));
        }

        // Onchange of gcodeState...
        if (printerVariables.gcodeState != mqttgcodeState)
        {
            if (mqttgcodeState == "RUNNING")
            {
                printerVariables.overridestage = 999; // Reset after special HMS override
                printerVariables.coolWaiting = false;  // a new print supersedes the last one's cool-down
            }

            if (mqttgcodeState == "FINISH")
            {
                printerVariables.finished = true;
                printerVariables.waitingForDoor = true;
                printerConfig.finishStartms = millis();
                printerConfig.finish_check = true;
                printerVariables.celebratePending = true;
                // Safe-to-remove: hold the finish color until the bed cools (only for a print
                // that just ended here, not when the controller boots into a finished printer)
                if (printerConfig.coolDownEnabled && printerConfig.finishindication && printerVariables.gcodeState == "RUNNING")
                {
                    printerVariables.coolWaiting = true;
                    printerVariables.coolStartMs = millis();
                    LogSerial.printf("[LED] Print finished - holding the finish color until the bed is below %d C\n", printerConfig.coolDownTemp);
                }
            }
            printerVariables.gcodeState = mqttgcodeState;

            if (printerConfig.debugingchange || printerConfig.debuging)
            {
                LogSerial.print(F("[MQTT] update - gcode_state now: "));
                LogSerial.println(printerVariables.gcodeState);
            }
            Changed = true;
        }
    }

    // Pause Command - quicker, but Only for user generated pause - error & code pauses don't trigger this.
    if (!messageobject["print"]["command"].isNull())
    {
        if (messageobject["print"]["command"] == "pause")
        {
            lastMQTTupdate = millis();
            LogSerial.println(F("[MQTT] update - manual PAUSE"));
            printerVariables.gcodeState = "PAUSE";
            Changed = true;
        }
    }

    // Added a delay so the slower MQTT status message doesn't reverse the "system" commands
    if (!messageobject["print"]["lights_report"].isNull() && ((millis() - lastMQTTupdate) > 3000))
    {
        JsonArray lightsReport = messageobject["print"]["lights_report"];
        for (JsonObject light : lightsReport)
        {
            if (light["node"] == "chamber_light")
            {
                if (printerVariables.lightKnownMs == 0)
                    printerVariables.lightKnownMs = millis() ? millis() : 1;
                if (printerVariables.printerledstate != (light["mode"] == "on"))
                {
                    printerVariables.printerledstate = (light["mode"] == "on");
                    printerConfig.replicate_update = true;
                    if (printerConfig.debugingchange || printerConfig.debuging)
                    {
                        LogSerial.print(F("[MQTT] chamber_light now: "));
                        LogSerial.println(printerVariables.printerledstate);
                    }
                    if (printerVariables.waitingForDoor && printerConfig.finish_check)
                    {
                        printerVariables.finished = true;
                    }
                    Changed = true;
                }
            }
        }
    }
    // System Commands are sent quicker than the push_status
    // Message only sent onChange
    if (!messageobject["system"]["command"].isNull())
    {
        if (messageobject["system"]["command"] == "ledctrl")
        {
            // Ignore Printer sending attempts to turn light on when already on.
            if (printerVariables.printerledstate != (messageobject["system"]["led_mode"] == "on"))
            {
                printerVariables.printerledstate = (messageobject["system"]["led_mode"] == "on");
                printerConfig.replicate_update = true;
                lastMQTTupdate = millis();
                if (printerConfig.debugingchange || printerConfig.debuging)
                {
                    LogSerial.print(F("[MQTT] led_mode now: "));
                    LogSerial.println(printerVariables.printerledstate);
                }
                if (printerVariables.waitingForDoor && printerConfig.finish_check)
                {
                    printerVariables.finished = true;
                }

                Changed = true;
            }
        }
    }

    // Bambu Health Management System (HMS)
    if (!messageobject["print"]["hms"].isNull())
    {
        String oldHMSlevel = printerVariables.parsedHMSlevel;

        printerVariables.hmsstate = false;
        printerVariables.parsedHMSlevel = "";

        // Normalize the ignore list once (replace '-' with '_', remove whitespace, one code per entry)
        String normalizedIgnoreList = printerConfig.hmsIgnoreList;
        normalizedIgnoreList.replace("-", "_");
        normalizedIgnoreList.replace(" ", "");
        normalizedIgnoreList.replace("\r", "");
        normalizedIgnoreList.replace("\n", ",");
        normalizedIgnoreList.toUpperCase();
        normalizedIgnoreList.replace("HMS_", "");
        normalizedIgnoreList = "," + normalizedIgnoreList + ",";

        for (const auto &hms : messageobject["print"]["hms"].as<JsonArray>())
        {
            uint64_t code = ((uint64_t)hms["attr"] << 32) + (uint64_t)hms["code"];

            char strHMScode[24];
            snprintf(strHMScode, sizeof(strHMScode), "%04X_%04X_%04X_%04X",
                     (int)(code >> 48), (int)((code >> 32) & 0xFFFF), (int)((code >> 16) & 0xFFFF), (int)(code & 0xFFFF));

            // Check if code is in ignore list (accepts with or without the HMS_ prefix)
            if (normalizedIgnoreList.indexOf("," + String(strHMScode) + ",") >= 0)
            {
                if (printerConfig.debugingchange)
                    LogSerial.printf("[MQTT] Ignored HMS Code: HMS_%s\n", strHMScode);
                continue;
            }

            if (ParseHMSSeverity(hms["code"]) != "")
            {
                printerVariables.hmsstate = true;
                printerVariables.parsedHMSlevel = ParseHMSSeverity(hms["code"]);
                printerVariables.parsedHMScode = code;
            }
        }

        if (oldHMSlevel != printerVariables.parsedHMSlevel)
        {
            // example, cap fall off
            // Error Code: 0300_1200_0002_0001 **
            //"hms":[{"attr":50336256,"code":131073}]
            if (printerVariables.parsedHMScode == 0x0C0003000003000B)
                printerVariables.overridestage = 10;
            if (printerVariables.parsedHMScode == 0x0300120000020001)
                printerVariables.overridestage = 17;
            if (printerVariables.parsedHMScode == 0x0700200000030001)
                printerVariables.overridestage = 6;
            if (printerVariables.parsedHMScode == 0x0300020000010001)
                printerVariables.overridestage = 20;
            if (printerVariables.parsedHMScode == 0x0300010000010007)
                printerVariables.overridestage = 21;

            if (printerVariables.parsedHMSlevel.length() == 0)
            {
                printerVariables.overridestage = 999; // error cleared
            }

            if (printerConfig.debuging || printerConfig.debugingchange)
            {
                LogSerial.print(F("[MQTT] update - parsedHMSlevel now: "));
                if (printerVariables.parsedHMSlevel.length() > 0)
                {
                    uint64_t c = printerVariables.parsedHMScode;
                    LogSerial.printf("%s      Error Code: HMS_%04X_%04X_%04X_%04X\n", printerVariables.parsedHMSlevel.c_str(),
                                     (int)(c >> 48), (int)((c >> 32) & 0xFFFF), (int)((c >> 16) & 0xFFFF), (int)(c & 0xFFFF));
                }
                else
                {
                    LogSerial.println(F("NULL"));
                }
            }
            Changed = true;
        }
    }

    // An AMS swap mid-print changes the strip color when filament color mode is on
    if (filamentChanged && printerConfig.filamentColorFx)
        Changed = true;
    // Setting a temperature on the printer screen wakes the lights (and starts the heat-up glow).
    // Outside a print it also counts as "dealing with the printer", like opening the door, so it
    // clears a finished print's color hold.
    if (targetChanged && printerConfig.heatGlowFx)
    {
        Changed = true;
        bool printingNow = printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE" || printerVariables.gcodeState == "PREPARE";
        bool heatingRequested = printerVariables.bedTarget > 30 || printerVariables.nozzleTarget > 30;
        if (!printingNow && heatingRequested)
        {
            printerVariables.waitingForDoor = false;
            printerConfig.finish_check = false;
            printerVariables.finished = false;
            printerVariables.coolWaiting = false;
        }
    }

    if (Changed == true)
    {
        printerConfig.inactivityStartms = millis(); // restart idle timer
        printerConfig.isIdleOFFActive = false;
        if (printerConfig.debuging)
        {
            LogSerial.println(F("Change from mqtt"));
        }
        printerConfig.maintMode_update = true;
        printerConfig.discoMode_update = true;
        printerConfig.replicate_update = true;
        printerConfig.testcolor_update = true;

        requestLedUpdate(); // handled on the main task, keeps this task free to service MQTT
    }
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    ParseCallback(topic, (byte *)stream.get_buffer(), stream.current_length());
    stream.flush();
}

// Safe to call from any task: the MQTT task sends it.
void controlChamberLight(bool on)
{
    if (!printerConfig.controlChamberLight)
        return;
    printerVariables.printerledstate = on; // <-- Set state flag to avoid replicate overwrite
    if (!printerVariables.online)
    {
        LogSerial.println(F("[MQTT] Skipped chamber_light control – MQTT not connected"));
        return;
    }
    pendingChamberLight = on ? 1 : 0;
}

void setupMqtt()
{
    clientId += String(random(0xffff), HEX);
    LogSerial.print(F("Setting up MQTT with Bambu Lab Printer IP address: "));
    LogSerial.println(printerConfig.printerIP);

    device_topic = String("device/") + printerConfig.serialNumber;
    report_topic = device_topic + String("/report");
    request_topic = device_topic + String("/request");

    wifiSecureClient.setInsecure();
    wifiSecureClient.setTimeout(15);
    mqttClient.setSocketTimeout(17);
    mqttClient.setKeepAlive(30);
    mqttClient.setBufferSize(1024);
    mqttClient.setServer(printerConfig.printerIP, 8883);
    mqttClient.setStream(stream);
    mqttClient.setCallback(mqttCallback);

    LogSerial.println(F("Finished setting up MQTT"));

    if (mqttTaskHandle == NULL)
    {
        BaseType_t result;

#if CONFIG_FREERTOS_UNICORE
        result = xTaskCreate(mqttTask, "mqttTask", TASK_RAM, NULL, 1, &mqttTaskHandle);
#else
        result = xTaskCreatePinnedToCore(mqttTask, "mqttTask", TASK_RAM, NULL, 1, &mqttTaskHandle, 1);
#endif

        if (result == pdPASS)
        {
            LogSerial.println(F("[MQTT] task successfully started"));
        }
        else
        {
            LogSerial.println(F("Failed to create MQTT task!"));
        }
    }
}

#endif
