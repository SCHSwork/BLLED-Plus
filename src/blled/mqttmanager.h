#ifndef _MQTTMANAGER
#define _MQTTMANAGER

#define TASK_RAM 20480 // 20kB for MQTT Task Stack Size

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mqtt/BLLEDMqtt.h"
#include <set>
#include <map>
#include <string>
#include <ArduinoJson.h>

#include "mqttparsingutility.h"
#include "AutoGrowBufferStream.h"
#include "types.h"
#include "leds.h"

WiFiClientSecure wifiSecureClient;
BLLEDMqttClient mqttClient(wifiSecureClient);

String device_topic;
String report_topic;
String request_topic;
String clientId = "BLLED-";

AutoGrowBufferStream stream;

unsigned long lastMQTTupdate = millis();
unsigned long lastLightCmdMs = 0; // when we last told the printer to switch its light

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
    lastLightCmdMs = millis() ? millis() : 1;

    LogSerial.printf("[MQTT] Chamber Light %s sent\n", on ? "ON" : "OFF");
}

// Door opened: wake the lights, remember whether they were dark, borrow the printer's light
#define DOOR_BOUNCE_MS 400
static bool doorOpenPending = false;     // opened right after closing: maybe a bounce, not acted on yet
static unsigned long doorOpenPendingMs = 0;

static void doorOpened(unsigned long when)
{
    printerVariables.lastdoorOpenms = when;

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
    // Restart inactivity timer
    printerConfig.inactivityStartms = millis();
    printerConfig.isIdleOFFActive = false;
}

static void doorClosed()
{
    printerVariables.lastdoorClosems = millis();

    if (printerConfig.controlChamberLight)
        printerVariables.chamberLightLocked = false;

    // (The original firmware turned the strip off here whenever "Idle after" was off -
    // even mid-print. Now closing the door puts back whatever was showing before; if the
    // lights were off when the door opened, they go back off - see ledWasForcedByDoor.)

    // Double-close detection
    if ((millis() - printerVariables.lastdoorOpenms) < 2000)
        printerVariables.doorSwitchTriggered = true;

    printerConfig.inactivityStartms = millis();
    printerConfig.isIdleOFFActive = false;
}

// The door stayed open: it was a real open after all
static void doorLoop()
{
    if (doorOpenPending && printerVariables.doorOpen && millis() - doorOpenPendingMs >= DOOR_BOUNCE_MS)
    {
        doorOpenPending = false;
        doorOpened(doorOpenPendingMs);
        requestLedUpdate();
    }
}

// "Lights on for the camera": turn the printer's light on (the strip follows it) when someone
// opens the camera, and back off a while after the last time it was opened. The printer doesn't
// say when the camera is closed, so it's a timer. During a print the printer runs its own light.
static bool printingNow()
{
    return printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE" || printerVariables.gcodeState == "PREPARE";
}

void cameraOpened()
{
    LogSerial.println(F("[Camera] Someone opened the camera"));
    if (!printerConfig.cameraLight || printingNow())
        return;
    printerConfig.inactivityStartms = millis(); // wake the strip if it went idle
    requestLedUpdate();
    // Handy often says "opened" twice in the same second: send the light command once
    bool justSent = printerVariables.cameraLitByUs && millis() - printerVariables.cameraLitMs < 5000;
    if (!printerVariables.printerledstate && !justSent)
    {
        publishChamberLight(true);
        printerVariables.cameraLitByUs = true;
        LogSerial.printf("[Camera] Light on for %d s\n", printerConfig.cameraLightSecs);
    }
    if (printerVariables.cameraLitByUs)
        printerVariables.cameraLitMs = millis();
}

static void cameraLightLoop()
{
    if (!printerVariables.cameraLitByUs)
        return;
    // Someone turned it off, a print started, or the setting was switched off: not ours any more
    if (!printerConfig.cameraLight || printingNow() ||
        (!printerVariables.printerledstate && millis() - printerVariables.cameraLitMs > 10000))
    {
        printerVariables.cameraLitByUs = false;
        return;
    }
    if (millis() - printerVariables.cameraLitMs > (unsigned long)printerConfig.cameraLightSecs * 1000UL)
    {
        printerVariables.cameraLitByUs = false;
        if (printerVariables.printerledstate)
        {
            publishChamberLight(false);
            LogSerial.println(F("[Camera] Light off again"));
        }
    }
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
        if (mqttReconnectNow)
        {
            mqttReconnectNow = false;
            if (mqttClient.connected())
                mqttClient.disconnect();
            wifiSecureClient.stop();
            LogSerial.println(F("[MQTT] Reconnecting to the printer (asked from the status page)"));
            if (printerVariables.online)
            {
                printerVariables.online = false;
                printerVariables.disconnectMQTTms = millis();
            }
            mqttBackoffMs = MQTT_BACKOFF_MIN_MS;
            mqttNextAttemptMs = millis();
        }
#ifndef BLLED_LITE
        if (otaPauseMqtt || notifyPauseMqtt)
#else
        if (otaPauseMqtt)
#endif
        {
            // Updating from GitHub (or sending a notification): give the memory of the printer
            // connection back for a moment
            if (mqttClient.connected())
            {
                mqttClient.disconnect();
                wifiSecureClient.stop();
                stream.release();
#ifndef BLLED_LITE
                if (notifyPauseMqtt && !otaPauseMqtt)
                    LogSerial.println(F("[MQTT] Paused while a notification is sent"));
                else
#endif
                LogSerial.println(F("[MQTT] Paused for the firmware download"));
            }
            if (printerVariables.online)
            {
                printerVariables.online = false;
                printerVariables.disconnectMQTTms = millis();
            }
            mqttNextAttemptMs = millis();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
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

        cameraLightLoop();
        doorLoop();

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
    // H2C Vortek hotend rack: what the rack is doing (from Bambu Studio's DevNozzleRack)
    filter["print"]["device"]["ctc"]["info"]["temp"] = true; // newer printer firmware: chamber temperature
    filter["print"]["device"]["bed"]["info"]["temp"] = true;  // ...and bed temperature
    filter["print"]["device"]["holder"]["stat"] = true;
    filter["print"]["device"]["holder"]["job"] = true;
    filter["print"]["ams_status"] = true;
    filter["print"]["ipcam"]["timelapse"] = true;
    filter["system"]["command"] = true;
    filter["system"]["led_mode"] = true;
    filter["liveview"]["command"] = true;

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

    // Someone opened the camera (Bambu Handy asks the printer to get the live view ready)
    if (messageobject["liveview"]["command"] == "prepare")
    {
        cameraOpened();
        return;
    }

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
        // Newer printer firmware reports these in "device" instead, packed as
        // (target << 16) | current
        if (!print["device"]["ctc"]["info"]["temp"].isNull())
            printerVariables.chamberTemp = print["device"]["ctc"]["info"]["temp"].as<uint32_t>() & 0xFFFF;
        if (!print["device"]["bed"]["info"]["temp"].isNull())
        {
            uint32_t v = print["device"]["bed"]["info"]["temp"].as<uint32_t>();
            float t = (v >> 16) & 0xFFFF;
            if (t != printerVariables.bedTarget)
                targetChanged = true;
            printerVariables.bedTemp = v & 0xFFFF;
            printerVariables.bedTarget = t;
        }
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

        // H2C Vortek: the hotend rack reports each pick/place step in device.holder
        JsonObject holder = print["device"]["holder"];
        if (!holder.isNull())
        {
            int st = holder["stat"] | printerVariables.rackStat;
            int job = holder["job"] | printerVariables.rackJob;
            if (st != printerVariables.rackStat || job != printerVariables.rackJob)
                Changed = true;
            printerVariables.rackStat = st;
            printerVariables.rackJob = job;
        }
        if (!print["ams_status"].isNull())
        {
            // high byte is the main AMS state; 0x08 = switching the hotend
            bool sw = ((print["ams_status"].as<int>() >> 8) & 0xFF) == 0x08;
            if (sw != printerVariables.amsSwitchHotend)
                Changed = true;
            printerVariables.amsSwitchHotend = sw;
        }

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
                // Right after a close this may be the door bouncing off the switch (closed hard):
                // wait a moment and see whether it closes again before doing anything
                if (millis() - printerVariables.lastdoorClosems < 1000)
                {
                    doorOpenPending = true;
                    doorOpenPendingMs = millis();
                }
                else
                {
                    doorOpened(millis());
                    Changed = true;
                }
            }
            else if (doorOpenPending) // closed again straight away: it bounced
            {
                doorOpenPending = false;
                LogSerial.printf("[MQTT] Door bounced (open for %lu ms) - ignored\n", millis() - doorOpenPendingMs);
            }
            else // Door closed
            {
                doorClosed();
                Changed = true;
            }
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

    // After the first-layer scan some printers stop sending stg_cur, so the stage stays at
    // 9 / 10 (a lidar step, dark) for the rest of the print. print_real_action 0 while RUNNING
    // means it's printing again. (Fix from BLLEDController-NG by softwarecrash.)
    if (messageobject["print"]["stg_cur"].isNull() && !messageobject["print"]["print_real_action"].isNull())
    {
        int action = messageobject["print"]["print_real_action"].as<int>();
        String gcodeNow = messageobject["print"]["gcode_state"].isNull() ? printerVariables.gcodeState
                                                                         : messageobject["print"]["gcode_state"].as<String>();
        if (action == 0 && gcodeNow == "RUNNING" && (printerVariables.stage == 9 || printerVariables.stage == 10))
        {
            printerVariables.stage = 0;
            LogSerial.println(F("[MQTT] No stage after the first-layer scan - treating as printing"));
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

    // Added a delay so the slower MQTT status message doesn't reverse the "system" commands.
    // Also for a few seconds after we switched the light ourselves (door, camera): the printer's
    // status can still show the state from before our command, and following that made the strip
    // flash on for a moment after the door closed.
    if (!messageobject["print"]["lights_report"].isNull() && ((millis() - lastMQTTupdate) > 3000) &&
        (lastLightCmdMs == 0 || millis() - lastLightCmdMs > 4000))
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

    // Why the printer stopped or paused a print (shown on the status page with its description)
    if (!messageobject["print"]["print_error"].isNull())
    {
        uint32_t pe = messageobject["print"]["print_error"].as<uint32_t>();
        if (pe != printerVariables.printError)
        {
            printerVariables.printError = pe;
            if (pe && (printerConfig.debugingchange || printerConfig.debuging))
                LogSerial.printf("[MQTT] Printer error %04X_%04X\n", (unsigned)(pe >> 16), (unsigned)(pe & 0xFFFF));
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

#ifndef BLLED_LITE // left out of the lite edition to save space
// Testing aid ("Watch printer messages"): reads each message in full and logs what's new - a field
// not seen in the status reports before, a change in the camera (ipcam) or AI camera (xcam)
// settings, or a message that isn't a status report. Used to look for undocumented signals,
// like whether the printer says anything when someone opens the camera.
static void watchMessage(const char *payload, unsigned int length)
{
    if (length > 20000)
    {
        LogSerial.printf("[Watch] Skipped a %u-byte message\n", length);
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, payload, length))
        return;
    static std::set<std::string> seen;
    static bool learned = false;
    static std::map<std::string, std::string> lastCam[2]; // subfield -> value
    const char *camKeys[2] = {"ipcam", "xcam"};
    for (JsonPair top : doc.as<JsonObject>())
    {
        const char *k = top.key().c_str();
        if (strcmp(k, "print") != 0)
        {
            String v;
            if (strcmp(k, "liveview") == 0)
                v = top.value()["command"] | "?"; // the rest is the camera's session password
            else
                serializeJson(top.value(), v);
            LogSerial.printf("[Watch] '%s' message: %.80s\n", k, v.c_str());
            continue;
        }
        JsonObject p = top.value().as<JsonObject>();
        const char *cmd = p["command"] | "";
        if (cmd[0] && strcmp(cmd, "push_status") != 0)
            LogSerial.printf("[Watch] print command: %s\n", cmd);
        for (JsonPair f : p)
        {
            std::string key = f.key().c_str();
            if (seen.insert(key).second && learned)
            {
                String v;
                serializeJson(f.value(), v);
                LogSerial.printf("[Watch] new field %s = %.70s\n", key.c_str(), v.c_str());
            }
        }
        if (p.size() > 30 && !learned)
        {
            learned = true; // the first full report sets what's normal
            LogSerial.printf("[Watch] Learned %u normal fields - now watching\n", (unsigned)seen.size());
        }
        for (int i = 0; i < 2; i++)
        {
            if (p[camKeys[i]].isNull())
                continue;
            // Log just the parts that changed, so nothing gets cut off
            bool first = lastCam[i].empty();
            for (JsonPair f : p[camKeys[i]].as<JsonObject>())
            {
                String v;
                serializeJson(f.value(), v);
                std::string &was = lastCam[i][f.key().c_str()];
                if (was != v.c_str())
                {
                    if (!first)
                        LogSerial.printf("[Watch] %s.%s: %.40s -> %.60s\n", camKeys[i], f.key().c_str(), was.c_str(), v.c_str());
                    was = v.c_str();
                }
            }
            if (first)
                LogSerial.printf("[Watch] %s: watching %u settings\n", camKeys[i], (unsigned)lastCam[i].size());
        }
    }
}

#endif

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    if (stream.overflowed())
    {
        // Not enough memory for this one message: skip it (the next full report replaces it)
        static unsigned long lastNote = 0;
        if (millis() - lastNote > 60000)
        {
            lastNote = millis();
            LogSerial.printf("[MQTT] Skipped a %u-byte printer message - not enough free memory (%u KB free, largest block %u KB)\n",
                             (unsigned)stream.wanted(), ESP.getFreeHeap() / 1024, ESP.getMaxAllocHeap() / 1024);
        }
        stream.flush();
        return;
    }
#ifndef BLLED_LITE
    if (printerConfig.watchMsgs)
        watchMessage(stream.get_buffer(), stream.current_length());
#endif
    ParseCallback(topic, (byte *)stream.get_buffer(), stream.current_length());
    stream.flush();
}

// Safe to call from any task: the MQTT task sends it.
void controlChamberLight(bool on)
{
    if (!printerConfig.controlChamberLight)
        return;
    printerVariables.printerledstate = on; // <-- Set state flag to avoid replicate overwrite
    lastLightCmdMs = millis() ? millis() : 1; // the printer's status lags behind this for a moment
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
    mqttPayloadStart = [](uint32_t n) { stream.begin(n); };
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
