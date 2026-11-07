#ifndef _BLLED_SELFUPDATE_H
#define _BLLED_SELFUPDATE_H

// ---------------------------------------------------------------------------
//  Updates straight from GitHub: the controller checks the latest BLLED Plus release, downloads
//  the .bin.ota itself and installs it - no file on your phone. Automatic installs only happen
//  while the printer is idle (and, if set, inside a time window).
//
//  Safety:
//  - The download is over HTTPS and the certificate is checked against Mozilla's root list, so
//    nobody on the network can hand the controller a fake firmware.
//  - The new version is written next to the running one; the switch only happens if the whole
//    file arrived and checks out. A failed download changes nothing.
//  - After the restart the new version has to prove it runs (about a minute). If it crashes or
//    hangs before that, the ESP32 boots the previous version again, and that release is never
//    installed automatically again.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "types.h"
#include "cert_bundle.h"
#include "signing_key.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"

#define SU_REPO "SCHSwork/BLLED-Plus"

void noteRestart(const char *why);
bool printingState();
bool clockSynced();

enum SuState : uint8_t { SU_IDLE, SU_CHECKING, SU_DOWNLOADING, SU_INSTALLED, SU_FAILED };
static const char *suStateName[] = {"idle", "checking", "downloading", "installed", "failed"};

volatile SuState suState = SU_IDLE;
volatile int suMode = 0;              // 0 check, 1 install if newer, 2 reinstall the latest (manual)
volatile bool suManual = false;       // started from the update page (may install a release that failed before)
volatile bool suBusy = false;         // a check or download is running
char suLatest[24] = "";               // latest release tag on GitHub, e.g. "v2.8.5"
char suAssetUrl[200] = "";
char suSigUrl[200] = "";               // the release's signature file
uint32_t suAssetSize = 0;
char suError[150] = "";
char suBadTag[24] = "";               // a release that failed to start: never auto-install it again
char suRolledBack[24] = "";           // shown on the update page after a rollback
unsigned long suLastCheckMs = 0;
bool suChecked = false;

// Version compare: "v2.8.10" > "2.8.9"
static void suParse(const char *v, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    while (*v && (*v < '0' || *v > '9'))
        v++;
    sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
}
bool suNewer(const char *a, const char *b)
{
    int x[3], y[3];
    suParse(a, x);
    suParse(b, y);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i])
            return x[i] > y[i];
    return false;
}
bool suUpdateAvailable() { return suLatest[0] && suNewer(suLatest, STRVERSION); }

// ---- Rollback protection ----
// Tell the Arduino core not to mark a freshly installed version as good at boot; we do it once
// it has run for a while (see selfUpdateLoop) or right before a planned restart.
bool verifyRollbackLater() { return true; }

bool suFirmwareConfirmed = false;
void suConfirmFirmware()
{
    if (suFirmwareConfirmed)
        return;
    suFirmwareConfirmed = true;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
    {
        esp_ota_mark_app_valid_cancel_rollback();
        LogSerial.println(F("[Update] New firmware runs fine - keeping it"));
    }
}

// At start-up: did the last GitHub install come up, or did the ESP32 go back to the old version?
void suBootCheck()
{
    Preferences p;
    if (!p.begin("blledsu", false))
        return;
    String pending = p.getString("pending", "");
    String bad = p.getString("bad", "");
    strlcpy(suBadTag, bad.c_str(), sizeof(suBadTag));
    if (pending.length())
    {
        int want[3], have[3];
        suParse(pending.c_str(), want);
        suParse(STRVERSION, have);
        if (want[0] != have[0] || want[1] != have[1] || want[2] != have[2])
        {
            // Installed, but it didn't start properly, so the previous version came back
            LogSerial.printf("[Update] %s didn't start properly - went back to %s\n", pending.c_str(), STRVERSION);
            p.putString("bad", pending);
            strlcpy(suBadTag, pending.c_str(), sizeof(suBadTag));
            strlcpy(suRolledBack, pending.c_str(), sizeof(suRolledBack));
        }
        p.remove("pending");
    }
    p.end();
}

static void suFail(const char *why)
{
    if (why != suError)
        strlcpy(suError, why, sizeof(suError));
    LogSerial.printf("[Update] %s\n", why);
    suState = SU_FAILED;
}

// Free up memory for HTTPS: the printer connection also uses TLS. Drop it if memory is tight.
static void suMakeRoom(bool always)
{
    if (!always && ESP.getMaxAllocHeap() > 50000 && ESP.getFreeHeap() > 90000)
        return;
    otaPauseMqtt = true;
    for (int i = 0; i < 30 && printerVariables.online; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(300));
}

static bool suFetchLatest(WiFiClientSecure &client)
{
    HTTPClient http;
    http.useHTTP10(true); // no chunked transfer, so the JSON can be read straight off the stream
    http.setTimeout(15000);
    if (!http.begin(client, "https://api.github.com/repos/" SU_REPO "/releases/latest"))
    {
        suFail("Couldn't reach GitHub");
        return false;
    }
    http.setUserAgent("BLLED-Plus/" STRVERSION);
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if (code != 200)
    {
        if (code == 404)
            strlcpy(suError, "No releases published yet", sizeof(suError));
        else if (code < 0)
            snprintf(suError, sizeof(suError), "Couldn't reach GitHub (%s)", HTTPClient::errorToString(code).c_str());
        else
            snprintf(suError, sizeof(suError), "GitHub answered %d", code);
        suFail(suError);
        http.end();
        return false;
    }
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["size"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (e || !doc["tag_name"].is<const char *>())
    {
        suFail("GitHub's answer couldn't be read");
        return false;
    }
    strlcpy(suLatest, doc["tag_name"], sizeof(suLatest));
    suAssetUrl[0] = 0;
    suSigUrl[0] = 0;
    suAssetSize = 0;
    for (JsonObject a : doc["assets"].as<JsonArray>())
    {
        String name = a["name"] | "";
        if (name.endsWith(".ota") && !suAssetUrl[0])
        {
            strlcpy(suAssetUrl, a["browser_download_url"] | "", sizeof(suAssetUrl));
            suAssetSize = a["size"] | 0;
        }
        else if (name.endsWith(".ota.sig") && !suSigUrl[0])
            strlcpy(suSigUrl, a["browser_download_url"] | "", sizeof(suSigUrl));
    }
    suChecked = true;
    LogSerial.printf("[Update] Latest on GitHub: %s (running %s)\n", suLatest, STRVERSION);
    return true;
}

// Download the small signature file into sig[] (DER-encoded ECDSA signature)
static int suFetchSignature(WiFiClientSecure &client, uint8_t *sig, size_t max)
{
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, suSigUrl))
        return -1;
    http.setUserAgent("BLLED-Plus/" STRVERSION);
    int code = http.GET();
    int n = -1;
    if (code == 200)
    {
        WiFiClient *s = http.getStreamPtr();
        unsigned long t0 = millis();
        n = 0;
        while ((size_t)n < max && millis() - t0 < 10000)
        {
            if (s->available())
                sig[n++] = s->read();
            else if (!s->connected())
                break;
            else
                vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    http.end();
    client.stop();
    return n;
}

// True if sig is a valid signature of this SHA-256 hash by the release key
static bool suSignatureOk(const uint8_t hash[32], const uint8_t *sig, size_t sigLen)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)releaseSigningKey, sizeof(releaseSigningKey)) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

static bool suDownload(WiFiClientSecure &client)
{
    if (!suAssetUrl[0])
    {
        suFail("That release has no .bin.ota file attached");
        return false;
    }
    // The download needs a lot of memory: always close the printer connection first (it comes
    // back after), and set up the install before the download connection takes its share
    client.stop();
    suState = SU_DOWNLOADING;
    otaActive = true; // pauses the self-check and the update page's upload handling
    selfUpdating = true;
    suMakeRoom(true);

    // Only firmware signed with the BLLED Plus release key gets installed
    static uint8_t sig[80];
    int sigLen = suSigUrl[0] ? suFetchSignature(client, sig, sizeof(sig)) : -2;
    if (sigLen < 8)
    {
        suFail(sigLen == -2 ? "This release isn't signed, so it wasn't installed" : "Couldn't download the release's signature");
        selfUpdating = false;
        otaActive = false;
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    otaTotal = suAssetSize;
    otaBytes = 0;
    otaLastData = millis();
    if (Update.isRunning())
        Update.abort();
    if (!Update.begin(suAssetSize > 0 ? (size_t)suAssetSize : UPDATE_SIZE_UNKNOWN))
    {
        snprintf(suError, sizeof(suError), "Couldn't start installing: %s (free memory %u KB, largest block %u KB)",
                 Update.getError() ? Update.errorString() : "out of memory", ESP.getFreeHeap() / 1024, ESP.getMaxAllocHeap() / 1024);
        suFail(suError);
        selfUpdating = false;
        otaActive = false;
        return false;
    }
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GitHub sends the file from its download servers
    int code = -1;
    if (http.begin(client, suAssetUrl))
    {
        http.setUserAgent("BLLED-Plus/" STRVERSION);
        code = http.GET();
    }
    if (code != 200)
    {
        if (code < 0)
            snprintf(suError, sizeof(suError), "Download failed: %s (free memory %u KB)", HTTPClient::errorToString(code).c_str(), ESP.getFreeHeap() / 1024);
        else
            snprintf(suError, sizeof(suError), "Download failed (GitHub answered %d)", code);
        http.end();
        Update.abort();
        mbedtls_sha256_free(&sha);
        suFail(suError);
        selfUpdating = false;
        otaActive = false;
        return false;
    }
    int len = http.getSize();
    if (len <= 0)
        len = suAssetSize;
    otaTotal = len > 0 ? len : 0;
    LogSerial.printf("[Update] Downloading %s (%d bytes)\n", suLatest, len);
    WiFiClient *s = http.getStreamPtr();
    static uint8_t buf[1460];
    unsigned long lastData = millis();
    size_t got = 0;
    bool ok = true;
    while (len <= 0 || got < (size_t)len)
    {
        size_t avail = s->available();
        if (avail)
        {
            int n = s->readBytes(buf, min(avail, sizeof(buf)));
            if (n <= 0)
                continue;
            if (Update.write(buf, n) != (size_t)n)
            {
                snprintf(suError, sizeof(suError), "Writing failed: %s", Update.errorString());
                ok = false;
                break;
            }
            mbedtls_sha256_update(&sha, buf, n);
            got += n;
            otaBytes = got;
            otaLastData = lastData = millis();
        }
        else if (!s->connected() && len <= 0)
            break; // no size given: the end of the connection is the end of the file
        else if (millis() - lastData > 15000)
        {
            strlcpy(suError, "The download stopped arriving", sizeof(suError));
            ok = false;
            break;
        }
        else
            vTaskDelay(pdMS_TO_TICKS(5));
    }
    http.end();
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (ok && !suSignatureOk(hash, sig, sigLen))
    {
        strlcpy(suError, "The file isn't signed by BLLED Plus, so it wasn't installed", sizeof(suError));
        ok = false;
    }
    if (ok && !Update.end(true))
    {
        snprintf(suError, sizeof(suError), "The file didn't check out: %s", Update.errorString());
        ok = false;
    }
    if (!ok)
    {
        Update.abort();
        suFail(suError);
        selfUpdating = false;
        otaActive = false;
        return false;
    }
    // Remember what we installed, to notice if it has to be rolled back
    Preferences p;
    if (p.begin("blledsu", false))
    {
        p.putString("pending", suLatest);
        p.end();
    }
    LogSerial.printf("[Update] Installed %s (%u bytes) - restarting\n", suLatest, (unsigned)got);
    suState = SU_INSTALLED;
    char note[40];
    snprintf(note, sizeof(note), "Updated from GitHub to %s", suLatest);
    noteRestart(note);
    shouldRestart = true;
    restartRequestTime = millis();
    return true;
}

static void suTask(void *)
{
    suState = SU_CHECKING;
    suError[0] = 0;
    suMakeRoom(false);
    {
        WiFiClientSecure client;
        client.setCACertBundle(certBundle);
        client.setHandshakeTimeout(20);
        if (suFetchLatest(client))
        {
            bool allowed = suManual || strcmp(suLatest, suBadTag) != 0;
            if ((suMode == 1 && suUpdateAvailable() && allowed) || (suMode == 2 && suLatest[0]))
                suDownload(client);
            else if (suState == SU_CHECKING)
                suState = SU_IDLE;
        }
        client.stop();
    }
    suLastCheckMs = millis();
    suMode = 0;
    if (!shouldRestart)
        otaPauseMqtt = false; // let the printer connection come back
    suBusy = false;
    vTaskDelete(NULL);
}

// mode: 0 = just check, 1 = install if newer, 2 = reinstall the latest even if it's this version
bool suStart(int mode, bool manual)
{
    if (suBusy || otaActive || shouldRestart || WiFi.status() != WL_CONNECTED)
        return false;
    suBusy = true;
    suMode = mode;
    suManual = manual;
    if (xTaskCreatePinnedToCore(suTask, "selfUpdate", 12288, NULL, 1, NULL, 1) != pdPASS)
    {
        suBusy = false;
        suFail("Not enough memory to start");
        return false;
    }
    return true;
}

static bool suInWindow()
{
    if (!printerConfig.autoUpdateWindow || printerConfig.autoUpdateStart == printerConfig.autoUpdateEnd)
        return true;
    if (!clockSynced())
        return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    int m = t.tm_hour * 60 + t.tm_min, s = printerConfig.autoUpdateStart, e = printerConfig.autoUpdateEnd;
    return (s < e) ? (m >= s && m < e) : (m >= s || m < e);
}

// Safe to restart the controller on its own: printer known and idle
static bool suPrinterIdle()
{
    if (!printerVariables.online || printingState())
        return false;
    const String &g = printerVariables.gcodeState;
    return g == "IDLE" || g == "FINISH" || g == "FAILED" || g == "";
}

void selfUpdateLoop()
{
    // A new version that talks to the printer for a bit, or simply keeps running for a minute,
    // is good: keep it. (Crashes and hangs show up well before that.)
    if (!suFirmwareConfirmed && (millis() > 60000 || (millis() > 20000 && printerVariables.online)))
        suConfirmFirmware();

    // Only when automatic updates are on, so a controller with the feature off never contacts
    // GitHub by itself
    if (!printerConfig.autoUpdate || suBusy || WiFi.status() != WL_CONNECTED || (WiFi.getMode() & WIFI_AP))
        return;

    // First look 3 minutes after start-up, then at most every 10 minutes
    static unsigned long lastTry = 0;
    if (lastTry == 0 ? millis() < 180000 : millis() - lastTry < 600000UL)
        return;
    bool fresh = suChecked && millis() - suLastCheckMs < 6UL * 3600000UL; // checked GitHub recently
    bool newer = suUpdateAvailable() && strcmp(suLatest, suBadTag) != 0;
    if (fresh && !newer)
        return; // up to date; next look in 6 hours
    bool goodMoment = suPrinterIdle() && suInWindow();
    if (fresh && newer && !goodMoment)
        return; // a new version is waiting for an idle printer / the time window
    lastTry = millis();
    suStart(goodMoment ? 1 : 0, false);
}

#endif
