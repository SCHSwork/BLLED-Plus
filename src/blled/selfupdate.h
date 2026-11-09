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
#include "mbedtls/base64.h"

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
char suSigUrl[200] = "";
bool suForceFull = false;
uint8_t suInlineSig[80];              // signature found in the release notes (no separate download)
int suInlineSigLen = 0;             // moving to the larger layout: fetch the full edition               // the release's signature file
uint32_t suAssetSize = 0;
char suError[150] = "";
char suBadTag[24] = "";               // a release that failed to start: never auto-install it again
char suRolledBack[24] = "";           // shown on the update page after a rollback
unsigned long suLastCheckMs = 0;
unsigned long suStartedMs = 0;
bool suLowMem = false;                // the last connection failed for lack of memory
bool suMemRestart = false;            // restarting to try again with fresh memory
int suBootMode = -1;                  // run this update job right after start-up (see suTask)
bool suBootRun = false;               // this job is that run (don't restart for it again)
unsigned long suResumeMqttAt = 0;     // when to let the printer connection back after a check       // when the running check/download began
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
    // Restarted to get fresh memory for an update (see suTask): run it before the printer
    // connection takes memory again
    int boot = p.getInt("boot", 0);
    if (boot)
    {
        p.remove("boot");
        suBootMode = (boot - 1) >> 1;
        suManual = (boot - 1) & 1;
        otaPauseMqtt = true;
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

// What went wrong connecting, in words. "Connection refused" from the HTTP client means the
// secure connection didn't open; the encryption library's own error says why.
static WiFiClientSecure *suClient = nullptr; // the connection in use, for its error details
static String suConnErr(int code)
{
    suLowMem = false;
    char b[110];
    if (code != HTTPC_ERROR_CONNECTION_REFUSED)
    {
        snprintf(b, sizeof(b), "%s - free memory %u KB", HTTPClient::errorToString(code).c_str(), ESP.getFreeHeap() / 1024);
        return String(b);
    }
    char e[2];
    int tls = suClient ? suClient->lastError(e, sizeof(e)) : 0;
    unsigned heap = ESP.getFreeHeap() / 1024, block = ESP.getMaxAllocHeap() / 1024;
    IPAddress ip;
    suLowMem = (tls == -0x7F00 || tls == -0x0010 || tls == -0x3F80); // out of memory (TLS, numbers, keys)
    if (suLowMem)
        snprintf(b, sizeof(b), "not enough memory for a secure connection - free %u KB, largest block %u KB", heap, block);
    else if (tls == -0x2700)
        snprintf(b, sizeof(b), "GitHub's certificate couldn't be checked");
    else if (tls < -1)
        snprintf(b, sizeof(b), "secure connection failed, error -0x%04X - free %u KB, largest block %u KB", -tls, heap, block);
    else if (!WiFi.hostByName("api.github.com", ip))
        snprintf(b, sizeof(b), "couldn't look up GitHub's address - the router or internet may be down");
    else
        snprintf(b, sizeof(b), "no answer from GitHub in time - the internet connection may be slow or down");
    return String(b);
}

static bool suFetchLatestOnce(WiFiClientSecure &client)
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
    LogSerial.printf("[Update] Asking GitHub for the latest release (free memory %u KB, largest block %u KB)\n", ESP.getFreeHeap() / 1024, ESP.getMaxAllocHeap() / 1024);
    int code = http.GET();
    LogSerial.printf("[Update] GitHub answered %d\n", code);
    if (code != 200)
    {
        if (code == 404)
            strlcpy(suError, "No releases published yet", sizeof(suError));
        else if (code < 0)
            snprintf(suError, sizeof(suError), "Couldn't reach GitHub (%s)", suConnErr(code).c_str());
        else
            snprintf(suError, sizeof(suError), "GitHub answered %d", code);
        suFail(suError);
        http.end();
        return false;
    }
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["body"] = true; // the release notes carry the signatures (hidden comments)
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
    // Releases have a full edition and a smaller "-lite" one for controllers on the original
    // BLLED storage layout. Take the full one if it fits this controller's update space.
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    uint32_t room = slot ? slot->size : 0;
    String fullName, liteName, fullUrl, liteUrl, fullSig, liteSig;
    uint32_t fullSize = 0, liteSize = 0;
    for (JsonObject a : doc["assets"].as<JsonArray>())
    {
        String name = a["name"] | "";
        String url = a["browser_download_url"] | "";
        bool lite = name.indexOf("-lite") >= 0;
        if (name.endsWith(".ota"))
        {
            if (lite && liteUrl.isEmpty())
                liteName = name, liteUrl = url, liteSize = a["size"] | 0;
            else if (!lite && fullUrl.isEmpty())
                fullName = name, fullUrl = url, fullSize = a["size"] | 0;
        }
        else if (name.endsWith(".ota.sig"))
        {
            if (lite)
                liteSig = url;
            else
                fullSig = url;
        }
    }
    bool useFull = fullUrl.length() && (suForceFull || !room || fullSize <= room || liteUrl.isEmpty());
    strlcpy(suAssetUrl, (useFull ? fullUrl : liteUrl).c_str(), sizeof(suAssetUrl));
    strlcpy(suSigUrl, (useFull ? fullSig : liteSig).c_str(), sizeof(suSigUrl));
    suAssetSize = useFull ? fullSize : liteSize;
    if (suAssetUrl[0])
        LogSerial.printf("[Update] Using %s (%u bytes, room %u)\n", (useFull ? fullName : liteName).c_str(), (unsigned)suAssetSize, (unsigned)room);
    // Signature in the release notes: "<!-- blled-sig <file name> <base64> -->"
    suInlineSigLen = 0;
    {
        const char *body = doc["body"] | "";
        String key = "blled-sig " + (useFull ? fullName : liteName) + " ";
        const char *at = strstr(body, key.c_str());
        if (at && (useFull ? fullName : liteName).length())
        {
            at += key.length();
            size_t len = 0;
            while (at[len] && at[len] != ' ' && at[len] != '-' && at[len] != '\r' && at[len] != '\n' && len < 200)
                len++;
            size_t out = 0;
            if (mbedtls_base64_decode(suInlineSig, sizeof(suInlineSig), &out, (const unsigned char *)at, len) == 0 && out >= 8)
                suInlineSigLen = out;
        }
    }
    suChecked = true;
    LogSerial.printf("[Update] Latest on GitHub: %s (running %s)\n", suLatest, STRVERSION);
    return true;
}

// Download the small signature file into sig[] (DER-encoded ECDSA signature)
static int suSigCode = 0;
static int suFetchSignatureOnce(WiFiClientSecure &client, uint8_t *sig, size_t max)
{
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, suSigUrl))
        return -1;
    http.setUserAgent("BLLED-Plus/" STRVERSION);
    int code = http.GET();
    suSigCode = code;
    int n = -1;
    if (code == 200)
    {
        // Read exactly as many bytes as GitHub says the file has (reading "until the end"
        // picked up a stray extra byte)
        int size = http.getSize();
        if (size > 0 && (size_t)size < max)
            max = size;
        // Let the HTTP client read it: it knows from the headers where the file ends (reading
        // "until the connection closes" waited for the server and sometimes timed out)
        struct Sink : public Stream
        {
            uint8_t *b;
            size_t n = 0, max;
            Sink(uint8_t *buf, size_t m) : b(buf), max(m) {}
            size_t write(uint8_t c) override
            {
                if (n < max)
                    b[n++] = c;
                return 1;
            }
            size_t write(const uint8_t *d, size_t len) override
            {
                for (size_t i = 0; i < len; i++)
                    write(d[i]);
                return len;
            }
            int available() override { return 0; }
            int read() override { return -1; }
            int peek() override { return -1; }
            void flush() override {}
        } sink(sig, max);
        http.setTimeout(8000);
        int w = http.writeToStream(&sink);
        n = w < 0 && sink.n == 0 ? -1 : (int)sink.n;
    }
    http.end();
    client.stop();
    // The signature says its own length (DER: 0x30, length): ignore anything after it
    if (n >= 2 && sig[0] == 0x30 && sig[1] < 0x80 && n > sig[1] + 2)
        n = sig[1] + 2;
    return n;
}

// GitHub connections sometimes fail for a moment (Wi-Fi, memory): try a few times before giving up
static int suFetchSignature(WiFiClientSecure &client, uint8_t *sig, size_t max);
// The signature for the chosen file: from the release notes if it's there, else its .sig file
static int suGetSignature(WiFiClientSecure &client, uint8_t *sig, size_t max)
{
    if (suInlineSigLen >= 8 && (size_t)suInlineSigLen <= max)
    {
        memcpy(sig, suInlineSig, suInlineSigLen);
        LogSerial.println(F("[Update] Signature found in the release notes"));
        return suInlineSigLen;
    }
    if (!suSigUrl[0])
        return -2;
    return suFetchSignature(client, sig, max);
}
static int suFetchSignature(WiFiClientSecure &client, uint8_t *sig, size_t max)
{
    for (int attempt = 1;; attempt++)
    {
        int n = suFetchSignatureOnce(client, sig, max);
        if (n >= 8 || attempt >= 3)
            return n;
        LogSerial.printf("[Update] Getting the signature failed (%s) - trying again\n",
                         suSigCode < 0 ? suConnErr(suSigCode).c_str() : String(suSigCode).c_str());
        vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
    }
}

static bool suFetchLatest(WiFiClientSecure &client)
{
    SuState before = suState;
    for (int attempt = 1;; attempt++)
    {
        if (suFetchLatestOnce(client))
        {
            suState = before; // an earlier attempt may have marked it failed
            suError[0] = 0;
            return true;
        }
        client.stop();
        if (attempt >= 4 || strstr(suError, "No releases"))
            return false;
        LogSerial.printf("[Update] %s - trying again (%d of 4)\n", suError, attempt + 1);
        suState = before;
        vTaskDelay(pdMS_TO_TICKS(3000 * attempt));
    }
}

// True if sig is a valid signature of this SHA-256 hash by the release key. The mbedTLS result
// codes go to *why (0 = fine) so a failure can be told apart: key, signature or hash.
static int suKeyRc = 0, suVerifyRc = 0;
static bool suSignatureOk(const uint8_t hash[32], const uint8_t *sig, size_t sigLen)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    suKeyRc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)releaseSigningKey, sizeof(releaseSigningKey));
    suVerifyRc = suKeyRc ? 0 : mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sigLen);
    mbedtls_pk_free(&pk);
    return suKeyRc == 0 && suVerifyRc == 0;
}

// SHA-256 of the firmware as it was written to the update slot (what would actually run).
// The updater holds back the first 16 bytes until the end, so those come from 'head'.
static bool suHashWritten(size_t len, const uint8_t head[16], uint8_t out[32])
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || len < 16)
        return false;
    static uint8_t chunk[1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, head, 16);
    bool ok = true;
    for (size_t off = 16; off < len && ok;)
    {
        size_t n = min(sizeof(chunk), len - off);
        ok = esp_partition_read(part, off, chunk, n) == ESP_OK;
        if (ok)
            mbedtls_sha256_update(&sha, chunk, n);
        off += n;
    }
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
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
    LogSerial.println(F("[Update] Getting the signature"));
    int sigLen = suGetSignature(client, sig, sizeof(sig));
    if (sigLen < 8)
    {
        suFail(sigLen == -2 ? "This release isn't signed, so it wasn't installed" : "Couldn't download the release's signature");
        selfUpdating = false;
        otaActive = false;
        return false;
    }
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
    for (int attempt = 1; attempt <= 3; attempt++)
    {
        if (http.begin(client, suAssetUrl))
        {
            http.setUserAgent("BLLED-Plus/" STRVERSION);
            code = http.GET();
        }
        if (code == 200 || code > 0 || attempt == 3)
            break;
        LogSerial.printf("[Update] Starting the download failed (%s) - trying again\n", suConnErr(code).c_str());
        http.end();
        client.stop();
        vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
    }
    if (code != 200)
    {
        if (code < 0)
            snprintf(suError, sizeof(suError), "Download failed: %s", suConnErr(code).c_str());
        else
            snprintf(suError, sizeof(suError), "Download failed (GitHub answered %d)", code);
        http.end();
        Update.abort();
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
    static uint8_t head[16];
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
            for (int i = 0; i < n && got + i < sizeof(head); i++)
                head[got + i] = buf[i];
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
    if (ok && suAssetSize && got != suAssetSize)
    {
        snprintf(suError, sizeof(suError), "Got %u of %u bytes, so it wasn't installed", (unsigned)got, (unsigned)suAssetSize);
        ok = false;
    }
    uint8_t hash[32] = {0};
    if (ok && !suHashWritten(got, head, hash))
    {
        strlcpy(suError, "Couldn't read back the downloaded file", sizeof(suError));
        ok = false;
    }
    if (ok && !suSignatureOk(hash, sig, sigLen))
    {
        // Details help tell a bad download from a bad signature
        snprintf(suError, sizeof(suError), "The file isn't signed by BLLED Plus, so it wasn't installed (sig %d bytes %02x%02x, hash %02x%02x%02x%02x, key %d, check %d)",
                 sigLen, sig[0], sig[1], hash[0], hash[1], hash[2], hash[3], suKeyRc, suVerifyRc);
        ok = false;
        // Don't keep downloading a release that fails the check
        strlcpy(suBadTag, suLatest, sizeof(suBadTag));
    }
    if (ok)
        LogSerial.println(F("[Update] Signature checks out"));
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
    suMakeRoom(true); // the printer connection and GitHub together use more memory than there is
    {
        WiFiClientSecure client;
        client.setCACertBundle(certBundle);
        client.setHandshakeTimeout(20);
        suClient = &client;
        if (suFetchLatest(client))
        {
            bool allowed = suManual || strcmp(suLatest, suBadTag) != 0;
            if ((suMode == 1 && suUpdateAvailable() && allowed) || (suMode == 2 && suLatest[0]))
                suDownload(client);
            else if (suState == SU_CHECKING)
                suState = SU_IDLE;
        }
        client.stop();
        suClient = nullptr;
    }
    // Not enough memory in one piece for the secure connection: memory is never as clean as
    // right after a restart, so restart and do it then (once; not during a print, and not just
    // for a background check)
    if (suState == SU_FAILED && suLowMem && !suBootRun && (suManual || suMode == 1) && !printingState())
    {
        Preferences p;
        if (p.begin("blledsu", false))
        {
            p.putInt("boot", 1 + (suMode << 1) + (suManual ? 1 : 0));
            p.end();
            LogSerial.println(F("[Update] Not enough memory in one piece - restarting to try again with fresh memory"));
            suMemRestart = true;
            suState = SU_CHECKING;
            restartRequestTime = millis();
            shouldRestart = true;
            vTaskDelete(NULL); // stays "busy" until the restart
        }
    }
    suBootRun = false;
    suLastCheckMs = millis();
    suMode = 0;
    if (!shouldRestart)
    {
        // A check from the update page is often followed by "Install" within seconds: keep the
        // printer connection paused a little longer rather than reconnecting and dropping it
        // again right away (the X1C stops answering for a while after quick reconnects)
        if (suManual && suState != SU_FAILED && suUpdateAvailable())
            suResumeMqttAt = millis() + 20000;
        else
            otaPauseMqtt = false; // let the printer connection come back
    }
    suBusy = false;
    vTaskDelete(NULL);
}

// mode: 0 = just check, 1 = install if newer, 2 = reinstall the latest even if it's this version
bool suStart(int mode, bool manual)
{
    if (suBusy || otaActive || shouldRestart || WiFi.status() != WL_CONNECTED)
        return false;
    suBusy = true;
    suResumeMqttAt = 0;
    suStartedMs = millis();
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
    // The job that was waiting for fresh memory after a restart
    if (suBootMode >= 0 && !suBusy)
    {
        if (WiFi.status() == WL_CONNECTED && !(WiFi.getMode() & WIFI_AP))
        {
            int m = suBootMode;
            suBootMode = -1;
            LogSerial.println(F("[Update] Trying again with fresh memory"));
            if (suStart(m, suManual))
                suBootRun = true;
            else
                otaPauseMqtt = false;
        }
        else if (millis() > 90000)
        {
            suBootMode = -1; // no Wi-Fi: give up, let the printer connection back
            otaPauseMqtt = false;
        }
    }
    if (suResumeMqttAt && !suBusy && (long)(millis() - suResumeMqttAt) >= 0)
    {
        suResumeMqttAt = 0;
        if (!otaActive && !shouldRestart)
            otaPauseMqtt = false;
    }
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
