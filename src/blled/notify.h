#ifndef _BLLED_NOTIFY_H
#define _BLLED_NOTIFY_H

// ---------------------------------------------------------------------------
//  Print notifications (full edition): tells you on your phone or in a Discord channel when a
//  print finishes, fails, needs you, or (if you want) starts, finishes its first layer or
//  passes 25 / 50 / 75 %.
//
//  Services: a Discord webhook, ntfy (ntfy.sh or your own server), and a plain webhook that
//  gets a JSON POST (Home Assistant, Node-RED, your own script). Each one is optional; every
//  notification goes to all of the ones that are set up.
//
//  How it works:
//  - notifyLoop() runs on the main task and watches the printer state that the MQTT task
//    keeps up to date. It never sends anything itself; it puts messages in a small queue.
//  - A short-lived task sends the queue and then ends, so nothing waits on the internet.
//  - HTTPS needs a lot of memory and the printer connection also uses TLS. If there isn't
//    enough memory for both, the printer connection is put on hold for the few seconds the
//    send takes (the strip keeps showing the last state; it only reacts to a lost printer
//    after 30 s).
// ---------------------------------------------------------------------------

#ifndef BLLED_LITE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "types.h"
#include "cert_bundle.h"

bool printingState();
uint8_t hmsModule();

enum NotifyEvent : uint8_t
{
    NE_TEST,
    NE_STARTED,
    NE_FIRST_LAYER,
    NE_PROGRESS,
    NE_FINISHED,
    NE_FAILED,
    NE_NEEDS_YOU,
};

static const char *notifyEventName(uint8_t e)
{
    switch (e)
    {
    case NE_TEST: return "test";
    case NE_STARTED: return "started";
    case NE_FIRST_LAYER: return "first_layer";
    case NE_PROGRESS: return "progress";
    case NE_FINISHED: return "finished";
    case NE_FAILED: return "failed";
    case NE_NEEDS_YOU: return "needs_you";
    }
    return "unknown";
}

struct NotifyMsg
{
    uint8_t event;
    char title[64];
    char body[200];
    // Snapshot of the print when the event happened (the webhook gets these as fields)
    char job[64];
    int percent, layer, totalLayers, remainingMins;
    uint32_t printError;
    uint64_t hmsCode;
    uint8_t only; // test from the settings page: 1 Discord, 2 ntfy, 4 webhook (0 = all)
};

#define NOTIFY_QUEUE 4
static NotifyMsg nQueue[NOTIFY_QUEUE];
static volatile uint8_t nHead = 0, nCount = 0;
static portMUX_TYPE nMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool nBusy = false;

// Last result per service, for the settings page: 0 never, 1 sent, -1 failed
struct NotifyResult
{
    int8_t state = 0;
    unsigned long atMs = 0;
    char detail[72] = "";
};
static NotifyResult nResDiscord, nResNtfy, nResWebhook;

bool notifyConfigured()
{
    return printerConfig.notifyDiscordUrl[0] || printerConfig.notifyNtfyTopic[0] || printerConfig.notifyWebhookUrl[0];
}

static void notifyPush(const NotifyMsg &m)
{
    portENTER_CRITICAL(&nMux);
    if (nCount == NOTIFY_QUEUE)
    {
        // Full (the internet has been down a while): drop the oldest
        nHead = (nHead + 1) % NOTIFY_QUEUE;
        nCount--;
    }
    nQueue[(nHead + nCount) % NOTIFY_QUEUE] = m;
    nCount++;
    portEXIT_CRITICAL(&nMux);
    LogSerial.printf("[Notify] %s: %s\n", m.title, m.body);
}

static void notifyQueue(uint8_t event, const char *title, const char *body)
{
    NotifyMsg m = {};
    m.event = event;
    strlcpy(m.title, title, sizeof(m.title));
    strlcpy(m.body, body, sizeof(m.body));
    strlcpy(m.job, printerVariables.jobName, sizeof(m.job));
    m.percent = printerVariables.percent;
    m.layer = printerVariables.layer;
    m.totalLayers = printerVariables.totalLayers;
    m.remainingMins = printerVariables.remainingMins;
    m.printError = printerVariables.printError;
    m.hmsCode = printerVariables.hmsstate ? printerVariables.parsedHMScode : 0;
    if (event == NE_STARTED || event == NE_TEST)
    {
        // Progress still holds the previous print's numbers at this point
        m.percent = m.layer = m.totalLayers = m.remainingMins = -1;
        m.printError = 0;
    }
    notifyPush(m);
}

static bool notifyPop(NotifyMsg &out)
{
    bool got = false;
    portENTER_CRITICAL(&nMux);
    if (nCount)
    {
        out = nQueue[nHead];
        nHead = (nHead + 1) % NOTIFY_QUEUE;
        nCount--;
        got = true;
    }
    portEXIT_CRITICAL(&nMux);
    return got;
}

static bool notifyRequeueFront(const NotifyMsg &m)
{
    bool ok = false;
    portENTER_CRITICAL(&nMux);
    if (nCount < NOTIFY_QUEUE)
    {
        nHead = (nHead + NOTIFY_QUEUE - 1) % NOTIFY_QUEUE;
        nQueue[nHead] = m;
        nCount++;
        ok = true;
    }
    portEXIT_CRITICAL(&nMux);
    return ok;
}

// ---------------------------------------------------------------------------
//  Sending
// ---------------------------------------------------------------------------

// Link in each notification: the controller's name on the network (blled.local) rather than its
// IP address, which can change and doesn't open on every phone
static String nStatusUrl()
{
    String host = globalVariables.Host;
    host.toLowerCase();
    if (host.length() == 0)
        host = "blled";
    return "http://" + host + ".local/status";
}

static void nFmtHms(char *b, size_t n, uint64_t c)
{
    snprintf(b, n, "HMS_%04X_%04X_%04X_%04X", (int)(c >> 48), (int)((c >> 32) & 0xFFFF), (int)((c >> 16) & 0xFFFF), (int)(c & 0xFFFF));
}

static bool nEnoughMemoryForTls()
{
    return ESP.getMaxAllocHeap() > 50000 && ESP.getFreeHeap() > 90000;
}

// Hold the printer connection while sending, if there isn't memory for two secure connections
static bool nHeld = false;
static void nMakeRoom()
{
    if (nHeld || nEnoughMemoryForTls())
        return;
    nHeld = true;
    notifyPauseMqtt = true;
    for (int i = 0; i < 30 && printerVariables.online; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(300));
    LogSerial.printf("[Notify] Printer connection on hold for the send (free memory %u KB)\n", ESP.getFreeHeap() / 1024);
}

static void nRelease()
{
    if (nHeld)
    {
        nHeld = false;
        notifyPauseMqtt = false;
    }
}

static void nSetResult(NotifyResult &r, bool ok, const char *detail)
{
    r.state = ok ? 1 : -1;
    r.atMs = millis() ? millis() : 1;
    strlcpy(r.detail, detail, sizeof(r.detail));
}

// POST a body to a URL (http:// or https://). Returns the HTTP status, or a negative HTTPClient
// error. err gets a short description when it didn't work.
static int nPost(const char *url, const String &body, const char *contentType, const char *bearer, char *err, size_t errLen)
{
    bool https = strncasecmp(url, "https://", 8) == 0;
    if (!https && strncasecmp(url, "http://", 7) != 0)
    {
        strlcpy(err, "the address has to start with https:// or http://", errLen);
        return -100;
    }
    WiFiClientSecure *secure = nullptr;
    WiFiClient *plain = nullptr;
    if (https)
    {
        nMakeRoom();
        secure = new WiFiClientSecure();
        if (!secure)
        {
            strlcpy(err, "out of memory", errLen);
            return -101;
        }
        secure->setCACertBundle(certBundle);
        secure->setHandshakeTimeout(15);
    }
    else
    {
        plain = new WiFiClient();
        if (!plain)
        {
            strlcpy(err, "out of memory", errLen);
            return -101;
        }
    }
    int code;
    {
        HTTPClient http;
        http.setTimeout(10000);
        http.setConnectTimeout(8000);
        http.setReuse(false);
        bool begun = https ? http.begin(*secure, url) : http.begin(*plain, url);
        if (!begun)
        {
            strlcpy(err, "couldn't read the address", errLen);
            code = -102;
        }
        else
        {
            http.addHeader("Content-Type", contentType);
            http.setUserAgent("BLLED-Plus/" STRVERSION);
            if (bearer && bearer[0])
                http.addHeader("Authorization", String("Bearer ") + bearer);
            code = http.POST(body);
            if (code < 0)
            {
                char tlsErr[2];
                int tls = secure ? secure->lastError(tlsErr, sizeof(tlsErr)) : 0;
                if (tls == -0x7F00 || tls == -0x0010 || tls == -0x3F80)
                    snprintf(err, errLen, "not enough memory for a secure connection (free %u KB)", ESP.getFreeHeap() / 1024);
                else if (tls == -0x2700)
                    strlcpy(err, "the server's certificate couldn't be checked", errLen);
                else
                    snprintf(err, errLen, "%s", HTTPClient::errorToString(code).c_str());
            }
            else if (code >= 300)
            {
                String r = http.getString();
                r.replace("\n", " ");
                snprintf(err, errLen, "server answered %d %s", code, r.substring(0, 40).c_str());
            }
            http.end();
        }
    }
    if (secure)
    {
        secure->stop();
        delete secure;
    }
    if (plain)
    {
        plain->stop();
        delete plain;
    }
    return code;
}

static uint32_t nDiscordColor(uint8_t e)
{
    switch (e)
    {
    case NE_FINISHED: return 0x2ECC71;
    case NE_FAILED: return 0xE74C3C;
    case NE_NEEDS_YOU: return 0xFF7A00;
    case NE_STARTED: return 0x3498DB;
    case NE_FIRST_LAYER: return 0x1ABC9C;
    case NE_PROGRESS: return 0x5865F2;
    }
    return 0x95A5A6;
}

static bool nSendDiscord(const NotifyMsg &m)
{
    JsonDocument d;
    d["username"] = "BLLED Plus";
    JsonObject e = d["embeds"].add<JsonObject>();
    e["title"] = m.title;
    e["description"] = m.body;
    e["color"] = nDiscordColor(m.event);
    e["url"] = nStatusUrl();
    if (m.job[0] && m.event != NE_TEST)
    {
        JsonObject f = e["fields"].add<JsonObject>();
        f["name"] = "Print";
        f["value"] = m.job;
        f["inline"] = true;
        if (m.percent >= 0 && m.event != NE_FINISHED)
        {
            JsonObject p = e["fields"].add<JsonObject>();
            p["name"] = "Progress";
            char b[40];
            if (m.layer > 0 && m.totalLayers > 0)
                snprintf(b, sizeof(b), "%d%% (layer %d of %d)", m.percent, m.layer, m.totalLayers);
            else
                snprintf(b, sizeof(b), "%d%%", m.percent);
            p["value"] = b;
            p["inline"] = true;
        }
    }
    JsonObject foot = e["footer"].to<JsonObject>();
    foot["text"] = globalVariables.Host;
    String body;
    serializeJson(d, body);
    char err[72] = "";
    int code = nPost(printerConfig.notifyDiscordUrl, body, "application/json", nullptr, err, sizeof(err));
    bool ok = code >= 200 && code < 300;
    nSetResult(nResDiscord, ok, ok ? "sent" : err);
    if (!ok)
        LogSerial.printf("[Notify] Discord: %s\n", err);
    return ok || (code >= 400 && code < 500 && code != 429); // a wrong webhook won't get better by retrying
}

static bool nSendNtfy(const NotifyMsg &m)
{
    // JSON publishing (POST to the server root) handles any characters in the title
    String url = printerConfig.notifyNtfyServer[0] ? printerConfig.notifyNtfyServer : "https://ntfy.sh";
    while (url.endsWith("/"))
        url.remove(url.length() - 1);
    JsonDocument d;
    d["topic"] = printerConfig.notifyNtfyTopic;
    d["title"] = m.title;
    d["message"] = m.body;
    d["click"] = nStatusUrl();
    int prio = 3;
    const char *tag = "printer";
    switch (m.event)
    {
    case NE_FINISHED: tag = "white_check_mark"; prio = 4; break;
    case NE_FAILED: tag = "x"; prio = 4; break;
    case NE_NEEDS_YOU: tag = "warning"; prio = 5; break;
    case NE_STARTED: tag = "arrow_forward"; break;
    case NE_FIRST_LAYER: tag = "mag"; break;
    case NE_PROGRESS: tag = "hourglass_flowing_sand"; prio = 2; break;
    case NE_TEST: tag = "bell"; break;
    }
    d["priority"] = prio;
    d["tags"].add(tag);
    String body;
    serializeJson(d, body);
    char err[72] = "";
    int code = nPost(url.c_str(), body, "application/json", printerConfig.notifyNtfyToken, err, sizeof(err));
    bool ok = code >= 200 && code < 300;
    nSetResult(nResNtfy, ok, ok ? "sent" : err);
    if (!ok)
        LogSerial.printf("[Notify] ntfy: %s\n", err);
    return ok || (code >= 400 && code < 500 && code != 429);
}

static bool nSendWebhook(const NotifyMsg &m)
{
    JsonDocument d;
    d["event"] = notifyEventName(m.event);
    d["title"] = m.title;
    d["message"] = m.body;
    d["controller"] = globalVariables.Host;
    d["printerSerial"] = printerConfig.serialNumber;
    d["job"] = m.job;
    d["percent"] = m.percent;
    d["layer"] = m.layer;
    d["totalLayers"] = m.totalLayers;
    d["remainingMins"] = m.remainingMins;
    if (m.printError)
    {
        char b[16];
        snprintf(b, sizeof(b), "%04X_%04X", (unsigned)(m.printError >> 16), (unsigned)(m.printError & 0xFFFF));
        d["printError"] = b;
    }
    if (m.hmsCode)
    {
        char b[32];
        nFmtHms(b, sizeof(b), m.hmsCode);
        d["hms"] = b;
    }
    d["url"] = nStatusUrl();
    String body;
    serializeJson(d, body);
    char err[72] = "";
    int code = nPost(printerConfig.notifyWebhookUrl, body, "application/json", nullptr, err, sizeof(err));
    bool ok = code >= 200 && code < 300;
    nSetResult(nResWebhook, ok, ok ? "sent" : err);
    if (!ok)
        LogSerial.printf("[Notify] Webhook: %s\n", err);
    return ok || (code >= 400 && code < 500 && code != 429);
}

static void notifyTask(void *)
{
    NotifyMsg m;
    uint8_t doneMask = 0, tries = 0; // services already done for this message, attempts so far
    while (notifyPop(m))
    {
        bool allOk = true;
        uint8_t skip = m.only ? (uint8_t)(~m.only & 7) : 0; // a test for just one service
        doneMask |= skip;
        if (printerConfig.notifyDiscordUrl[0] && !(doneMask & 1))
        {
            if (nSendDiscord(m)) doneMask |= 1;
            else allOk = false;
        }
        if (printerConfig.notifyNtfyTopic[0] && !(doneMask & 2))
        {
            if (nSendNtfy(m)) doneMask |= 2;
            else allOk = false;
        }
        if (printerConfig.notifyWebhookUrl[0] && !(doneMask & 4))
        {
            if (nSendWebhook(m)) doneMask |= 4;
            else allOk = false;
        }
        if (allOk || ++tries >= 3)
        {
            if (!allOk)
                LogSerial.printf("[Notify] Gave up on \"%s\" after 3 tries\n", m.title);
            doneMask = 0;
            tries = 0;
        }
        else
        {
            // Try again in a bit (Wi-Fi blip, server busy). Let the printer connection back meanwhile.
            if (!notifyRequeueFront(m))
            {
                doneMask = 0; // newer messages filled the queue: this one is dropped
                tries = 0;
            }
            nRelease();
            vTaskDelay(pdMS_TO_TICKS(15000 * tries));
        }
    }
    nRelease();
    nBusy = false;
    vTaskDelete(NULL);
}

static void notifyKick()
{
    if (nBusy || !nCount || WiFi.status() != WL_CONNECTED || (WiFi.getMode() & WIFI_AP))
        return;
    // Stay out of the way of a firmware update (it needs the memory and holds the printer connection)
    if (otaActive || selfUpdating || otaPauseMqtt || shouldRestart || suBusy)
        return;
    static unsigned long retryAt = 0;
    if (retryAt && (long)(millis() - retryAt) < 0)
        return;
    nBusy = true;
    if (xTaskCreatePinnedToCore(notifyTask, "notify", 10240, NULL, 1, NULL, 1) != pdPASS)
    {
        nBusy = false;
        if (!retryAt)
            LogSerial.println(F("[Notify] Not enough memory to start sending - will keep trying"));
        retryAt = millis() + 10000;
    }
    else
        retryAt = 0;
}

// ---------------------------------------------------------------------------
//  Deciding what to send
// ---------------------------------------------------------------------------

static void nDuration(char *b, size_t n, unsigned long ms)
{
    unsigned long mins = ms / 60000;
    if (mins >= 60)
        snprintf(b, n, "%luh %02lum", mins / 60, mins % 60);
    else if (mins == 0)
        snprintf(b, n, "under a minute");
    else
        snprintf(b, n, "%lum", mins);
}

// Why the printer is waiting for you, in words. "" when it isn't.
static const char *nNeedsYouReason()
{
    int st = printerVariables.stage, ov = printerVariables.overridestage;
    bool paused = printerVariables.gcodeState == "PAUSE";
    if (st == 6 || ov == 6) return "Filament ran out";
    if (st == 17 || ov == 17) return "The front cover came off";
    if (st == 20 || ov == 20) return "Nozzle temperature problem";
    if (st == 21 || ov == 21) return "Bed temperature problem";
    if (st == 35) return "Nozzle clog detected - print paused";
    if (st == 34 || (paused && hmsModule() == 0x0C)) return "First layer / print quality problem - print paused";
    if (st == 26 || (paused && hmsModule() == 0x07)) return "AMS problem - print paused";
    if (printerVariables.parsedHMSlevel == "Fatal") return "Fatal printer error";
    if (printerVariables.parsedHMSlevel == "Serious") return "Serious printer warning";
    if (st == 16) return ""; // paused by you
    bool waitingStage = st == 5 || st == 23 || st == 27 || st == 28 || st == 30 || st == 32 || st == 33;
    if (paused || waitingStage)
        return "Print paused - the printer is waiting for you";
    return "";
}

// Test from the settings page: a made-up example of one kind of notification, to all services
// or just one ("discord", "ntfy", "webhook"). Returns false if that service isn't set up.
bool notifyTest(const String &event, const String &service)
{
    uint8_t only = service == "discord" ? 1 : service == "ntfy" ? 2 : service == "webhook" ? 4 : 0;
    uint8_t have = (printerConfig.notifyDiscordUrl[0] ? 1 : 0) | (printerConfig.notifyNtfyTopic[0] ? 2 : 0) | (printerConfig.notifyWebhookUrl[0] ? 4 : 0);
    if (!(only ? (have & only) : have))
        return false;

    NotifyMsg m = {};
    m.only = only;
    m.percent = m.layer = m.totalLayers = m.remainingMins = -1;
    strlcpy(m.job, "Test print", sizeof(m.job));
    const char *title, *body;
    if (event == "finished")
    {
        m.event = NE_FINISHED;
        title = "[Test] Print finished";
        body = "Test print finished in 2h 34m";
        m.percent = 100;
    }
    else if (event == "failed")
    {
        m.event = NE_FAILED;
        title = "[Test] Print failed";
        body = "Test print stopped - printer error 0300_8003 (details on the status page)";
        m.printError = 0x03008003;
        m.percent = 37;
    }
    else if (event == "needs_you")
    {
        m.event = NE_NEEDS_YOU;
        title = "[Test] Printer needs you";
        body = "Filament ran out - Test print";
        m.percent = 61, m.layer = 146, m.totalLayers = 240, m.remainingMins = 58;
    }
    else if (event == "started")
    {
        m.event = NE_STARTED;
        title = "[Test] Print started";
        body = "Test print started";
    }
    else if (event == "first_layer")
    {
        m.event = NE_FIRST_LAYER;
        title = "[Test] First layer done";
        body = "First layer of Test print is down - a good moment to check it";
        m.percent = 2, m.layer = 2, m.totalLayers = 240, m.remainingMins = 150;
    }
    else if (event == "progress")
    {
        m.event = NE_PROGRESS;
        title = "[Test] Print 50% done";
        body = "Test print is 50% done, about 1h 12m left";
        m.percent = 50, m.layer = 120, m.totalLayers = 240, m.remainingMins = 72;
    }
    else
    {
        m.event = NE_TEST;
        m.job[0] = 0;
        title = "BLLED Plus test";
        static char b[120];
        snprintf(b, sizeof(b), "Test from %s (BLLED Plus %s). If you can read this, notifications work.", globalVariables.Host.c_str(), STRVERSION);
        body = b;
    }
    strlcpy(m.title, title, sizeof(m.title));
    strlcpy(m.body, body, sizeof(m.body));
    notifyPush(m);
    notifyKick();
    return true;
}

void notifyLoop()
{
    static unsigned long last = 0;
    if (millis() - last < 500)
        return;
    last = millis();

    notifyKick(); // anything waiting (e.g. queued while an update was running)

    // State tracking runs even with nothing set up, so turning notifications on mid-print
    // doesn't fire a "started" for a print that's already going
    static bool known = false;        // seen the printer's state since connecting
    static bool inJob = false;
    static bool firstLayerDone = false;
    static int nextMilestone = 0;
    static unsigned long jobStartMs = 0; // 0 = didn't see it start
    static String lastReason = "";
    static unsigned long reasonSinceMs = 0, clearSinceMs = 0;
    static bool reasonSent = false;
    static bool lowPctSeen = false;      // seen this print's progress start low (not the last print's 100 %)
    static unsigned long onlineSince = 0, offlineSince = 0;

    if (!printerVariables.online)
    {
        known = false; // re-learn after reconnecting, without treating it as new events
        onlineSince = 0;
        if (!offlineSince)
            offlineSince = millis() ? millis() : 1;
        return;
    }
    // Give the printer a few seconds to send its full report after (re)connecting
    if (!onlineSince)
        onlineSince = millis() ? millis() : 1;
    if (millis() - onlineSince < 8000)
        return;
    // Away only briefly (a notification send, a reconnect): a print that ended meanwhile still
    // gets its message. Away longer (printer switched off): start fresh.
    bool briefGap = offlineSince && millis() - offlineSince < 120000;

    String g = printerVariables.gcodeState;
    bool printing = g == "RUNNING" || g == "PAUSE" || g == "PREPARE";
    int step = constrain(printerConfig.notifyProgressStep, 5, 50);
    int pct = printerVariables.percent;
    char b[200], d[24];

    if (!known)
    {
        known = true;
        if (printing && !inJob)
        {
            // Connected to a print that was already going: follow it from here, quietly
            inJob = true;
            jobStartMs = 0;
            firstLayerDone = printerVariables.layer > 1;
            nextMilestone = pct >= 0 ? (pct / step + 1) * step : step;
            lowPctSeen = true;
        }
        else if (!printing && !briefGap)
            inJob = false;
        offlineSince = 0;
        lastReason = nNeedsYouReason();
        reasonSent = lastReason.length() > 0; // already showing when we got here: don't announce
        if (!(inJob && !printing))
            return; // otherwise: the print ended while we were away - announce it below
    }

    // Started
    if (printing && !inJob)
    {
        inJob = true;
        jobStartMs = millis();
        firstLayerDone = false;
        nextMilestone = step;
        lowPctSeen = false;
        if (printerConfig.notifyStart)
        {
            snprintf(b, sizeof(b), "%s started", printerVariables.jobName[0] ? printerVariables.jobName : "A print");
            notifyQueue(NE_STARTED, "Print started", b);
        }
    }

    // Actually laying down plastic (not heating, leveling or calibrating). The layer and percent
    // can still hold the previous print's numbers for a moment after a new one starts, so the
    // first layer only counts at layer 2-3, and milestones only once the percent has started low.
    if (inJob && g == "RUNNING" && (printerVariables.stage == 0 || printerVariables.stage == -1))
    {
        if (pct >= 0 && pct < step)
            lowPctSeen = true;
        // First layer done
        if (!firstLayerDone && (printerVariables.layer == 2 || printerVariables.layer == 3))
        {
            firstLayerDone = true;
            if (printerConfig.notifyFirstLayer)
            {
                snprintf(b, sizeof(b), "First layer of %s is down - a good moment to check it",
                         printerVariables.jobName[0] ? printerVariables.jobName : "the print");
                notifyQueue(NE_FIRST_LAYER, "First layer done", b);
            }
        }
        // Progress milestones (not 100: that's "finished")
        if (lowPctSeen && pct >= nextMilestone && nextMilestone < 100)
        {
            int reached = (pct / step) * step;
            nextMilestone = reached + step;
            if (printerConfig.notifyProgress && reached > 0 && reached < 100)
            {
                char t[32];
                snprintf(t, sizeof(t), "Print %d%% done", reached);
                if (printerVariables.remainingMins >= 0)
                {
                    nDuration(d, sizeof(d), (unsigned long)printerVariables.remainingMins * 60000UL);
                    snprintf(b, sizeof(b), "%s is %d%% done, about %s left", printerVariables.jobName[0] ? printerVariables.jobName : "The print", pct, d);
                }
                else
                    snprintf(b, sizeof(b), "%s is %d%% done", printerVariables.jobName[0] ? printerVariables.jobName : "The print", pct);
                notifyQueue(NE_PROGRESS, t, b);
            }
        }
    }

    // Ended
    if (!printing && inJob)
    {
        inJob = false;
        const char *job = printerVariables.jobName[0] ? printerVariables.jobName : "The print";
        if (g == "FINISH")
        {
            if (printerConfig.notifyFinish)
            {
                if (jobStartMs)
                {
                    nDuration(d, sizeof(d), millis() - jobStartMs);
                    snprintf(b, sizeof(b), "%s finished in %s", job, d);
                }
                else
                    snprintf(b, sizeof(b), "%s finished", job);
                notifyQueue(NE_FINISHED, "Print finished", b);
            }
        }
        else if (g == "FAILED" || g == "IDLE")
        {
            if (printerConfig.notifyFail)
            {
                uint32_t pe = printerVariables.printError;
                if (pe == 0x0300400C || pe == 0)
                    snprintf(b, sizeof(b), "%s was cancelled%s", job, pe ? "" : " or stopped");
                else
                    snprintf(b, sizeof(b), "%s stopped - printer error %04X_%04X (details on the status page)", job,
                             (unsigned)(pe >> 16), (unsigned)(pe & 0xFFFF));
                notifyQueue(NE_FAILED, pe == 0x0300400C ? "Print cancelled" : "Print failed", b);
            }
        }
        lastReason = "";
        reasonSent = false;
    }

    // Needs you: a problem or a pause the printer made, held for a few seconds (so a pause you
    // make yourself, which shows as a plain PAUSE for a moment before the printer says it was
    // you, doesn't count). One message per problem; it can fire again once it's been clear a while.
    String reason = nNeedsYouReason();
    if (!printing)
    {
        // Between prints only a serious printer fault counts (not a stage left over from the print)
        const String &lv = printerVariables.parsedHMSlevel;
        reason = lv == "Fatal" ? "Fatal printer error" : lv == "Serious" ? "Serious printer warning" : "";
    }
    if (reason != lastReason)
    {
        if (reason.length())
            reasonSinceMs = millis();
        else
            clearSinceMs = millis();
        if (reason.length() && lastReason.length() && reasonSent)
            reasonSent = false; // a different problem: tell about it too
        lastReason = reason;
    }
    if (reason.length() == 0 && reasonSent && millis() - clearSinceMs > 30000)
        reasonSent = false;
    if (reason.length() && !reasonSent && millis() - reasonSinceMs > 5000)
    {
        reasonSent = true;
        if (printerConfig.notifyNeedsYou)
        {
            char h[32] = "";
            if (printerVariables.hmsstate)
            {
                char c[32];
                nFmtHms(c, sizeof(c), printerVariables.parsedHMScode);
                snprintf(h, sizeof(h), " (%s)", c);
            }
            if (printerVariables.jobName[0] && printing)
                snprintf(b, sizeof(b), "%s%s - %s", reason.c_str(), h, printerVariables.jobName);
            else
                snprintf(b, sizeof(b), "%s%s", reason.c_str(), h);
            notifyQueue(NE_NEEDS_YOU, "Printer needs you", b);
        }
    }

    if (!notifyConfigured())
    {
        // Nothing to send to: don't keep messages around
        NotifyMsg m;
        while (notifyPop(m))
            ;
    }
}

bool notifyBusy() { return nBusy; }

// For the settings page
void notifyStatusJson(JsonObject o)
{
    auto put = [&](const char *name, const NotifyResult &r) {
        JsonObject s = o[name].to<JsonObject>();
        s["state"] = r.state;
        s["ago"] = r.atMs ? (long)((millis() - r.atMs) / 1000) : -1;
        s["detail"] = r.detail;
    };
    put("discord", nResDiscord);
    put("ntfy", nResNtfy);
    put("webhook", nResWebhook);
    o["busy"] = (bool)nBusy;
    o["queued"] = (int)nCount;
}

#endif // BLLED_LITE
#endif
