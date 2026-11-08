#ifndef _BLLED_LAYOUTMIGRATE_H
#define _BLLED_LAYOUTMIGRATE_H

// ---------------------------------------------------------------------------
//  Move to the larger storage layout, without a USB cable (lite edition only).
//
//  Controllers still on the original BLLED layout have 1.25 MB per firmware slot. This moves them
//  to the layout BLLED Plus is built for (1.875 MB per slot) and installs the full edition:
//
//   1. if running from the second slot, copy this firmware to the first slot and restart there
//   2. save the settings in NVS (a part of the chip that doesn't move)
//   3. download the full edition from GitHub into where the new second slot will be, and check
//      its signature against what was actually written
//   4. clear the new settings area and tell the start-up code to boot the new second slot
//   5. write the new storage table (the only risky step: ~0.05 s, see below)
//   6. restart: the full edition starts, finds no settings file and restores them from NVS
//
//  If the power goes out during any step but 5, the controller still starts (on this version,
//  settings restored from NVS) and the move can be tried again. If it goes out during the ~0.05 s
//  of step 5, the chip has no storage table and needs a USB re-flash. If the full edition doesn't
//  start properly, the chip boots this lite version again (in the new layout).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp32/rom/spi_flash.h"
#include "esp_spi_flash.h"
#include "layout_table.h"
#include "selfupdate.h"

extern "C"
{
    void spi_flash_disable_interrupts_caches_and_other_cpu(void);
    void spi_flash_enable_interrupts_caches_and_other_cpu(void);
}

#define LM_OLD_SLOT 0x140000u    // firmware slot size on the original layout
#define LM_NEW_SLOT 0x1E0000u    // ... on the larger layout
#define LM_NEW_APP1 0x1F0000u    // where the larger layout's second slot starts
#define LM_NEW_DATA 0x3D0000u    // the larger layout's settings area (128 KB)
#define LM_DATA_SIZE 0x20000u
#define LM_OTADATA 0xE000u
#define LM_TABLE 0x8000u

volatile bool lmBusy = false;
char lmStep[64] = "";
char lmError[150] = "";
bool lmDone = false;

// Running on the original layout?
bool lmOnOldLayout()
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && run->size <= LM_OLD_SLOT;
}

static void lmSay(const char *step)
{
    strlcpy(lmStep, step, sizeof(lmStep));
    LogSerial.printf("[Layout] %s\n", step);
    Serial.flush();
}

static bool lmFail(const char *why)
{
    strlcpy(lmError, why, sizeof(lmError));
    LogSerial.printf("[Layout] Stopped: %s\n", why);
    Serial.flush();
    return false;
}

// ---- Step 5: write the storage table with the chip's own ROM routines (the normal flash API
// refuses to touch it on purpose). Everything here runs from RAM with the flash cache off. ----
static DRAM_ATTR uint32_t *lmTableBuf = nullptr; // 3 KB, borrowed from the heap while writing
static DRAM_ATTR int lmUnlockRc = 0;
static IRAM_ATTR __attribute__((noinline)) int lmWriteTableRaw()
{
    spi_flash_disable_interrupts_caches_and_other_cpu();
    lmUnlockRc = esp_rom_spiflash_unlock(); // clears write protection if set; not every chip needs it
    int r = esp_rom_spiflash_erase_sector(LM_TABLE / 4096);
    if (r == 0)
        r = esp_rom_spiflash_write(LM_TABLE, lmTableBuf, 3072);
    spi_flash_enable_interrupts_caches_and_other_cpu();
    return r;
}

// Writes 3 KB from src as the storage table and checks it reads back
static bool lmWriteTable(const uint8_t *src)
{
    HeapBuf table(3072), checkBuf(3072);
    if (!table.p || !checkBuf.p)
        return false;
    lmTableBuf = (uint32_t *)table.p;
    memcpy(lmTableBuf, src, 3072);
    uint8_t *check = checkBuf.p;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int r = lmWriteTableRaw();
        if (r == 0 && esp_flash_read(NULL, check, LM_TABLE, 3072) == ESP_OK &&
            memcmp(check, lmTableBuf, 3072) == 0)
        {
            lmTableBuf = nullptr;
            return true;
        }
        LogSerial.printf("[Layout] Storage table write attempt %d didn't check out (write %d, unlock %d)\n", attempt + 1, r, lmUnlockRc);
        Serial.flush();
    }
    return false;
}

// ---- Step 4: boot the second slot next (the start-up code's own record, "otadata") ----
static bool lmBootSecondSlot()
{
    struct
    {
        uint32_t seq;
        uint8_t label[20];
        uint32_t state;
        uint32_t crc;
    } entry;
    memset(&entry, 0xFF, sizeof(entry));
    entry.seq = 2;   // sequence 2 = second slot
    entry.state = 0; // "new": if it doesn't start properly, the start-up code goes back to the first slot
    entry.crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&entry.seq, 4);
    if (esp_flash_erase_region(NULL, LM_OTADATA, 0x2000) != ESP_OK)
        return false;
    return esp_flash_write(NULL, &entry, LM_OTADATA, sizeof(entry)) == ESP_OK;
}

// ---- Step 1: run from the first slot ----
static bool lmMoveToFirstSlot()
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *first = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!run || !first)
        return lmFail("Couldn't find the firmware slots");
    if (run->address == first->address)
        return true;
    lmSay("Copying this version to the first slot");
    esp_ota_handle_t h;
    if (esp_ota_begin(first, OTA_SIZE_UNKNOWN, &h) != ESP_OK)
        return lmFail("Couldn't prepare the first slot");
    HeapBuf mem(4096);
    uint8_t *buf = mem.p;
    if (!buf)
    {
        esp_ota_abort(h);
        return lmFail("Not enough memory to copy");
    }
    for (uint32_t off = 0; off < run->size; off += 4096)
    {
        if (esp_partition_read(run, off, buf, 4096) != ESP_OK || esp_ota_write(h, buf, 4096) != ESP_OK)
        {
            esp_ota_abort(h);
            return lmFail("Copying to the first slot failed");
        }
        otaBytes = off;
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(first) != ESP_OK)
        return lmFail("The copy in the first slot didn't check out");
    Preferences p;
    if (p.begin("blledmig", false))
    {
        p.putBool("resume", true); // carry on by itself after the restart
        p.end();
    }
    lmSay("Restarting into the first slot, then carrying on");
    noteRestart("Moving to the larger layout (step 1 of 2)");
    suConfirmFirmware();
    shouldRestart = true;
    restartRequestTime = millis();
    return false; // not an error: continues after the restart
}

// ---- Step 2: keep the settings where the move won't touch them ----
static bool lmSaveSettings()
{
    lmSay("Saving your settings");
    File f = LittleFS.open(configPath, "r");
    if (!f)
        return lmFail("Couldn't read your settings");
    size_t n = f.size();
    if (n < 10 || n > 6000)
    {
        f.close();
        return lmFail("Your settings file looks wrong");
    }
    HeapBuf mem(n), backMem(n);
    uint8_t *cfg = mem.p, *back = backMem.p;
    if (!cfg || !back)
    {
        f.close();
        return lmFail("Not enough memory to save your settings");
    }
    size_t got = f.read(cfg, n);
    f.close();
    if (got != n)
        return lmFail("Couldn't read your settings");
    Preferences p;
    if (!p.begin("blledmig", false))
        return lmFail("Couldn't open the safe storage");
    bool ok = p.putBytes("cfg", cfg, n) == n;
    ok = ok && p.getBytes("cfg", back, n) == n && memcmp(back, cfg, n) == 0;
    p.end();
    return ok ? true : lmFail("Saving your settings to safe storage failed");
}

// ---- Step 3: the full edition, straight into the new second slot ----
#ifdef BLLED_MIGRATE_TEST
// Emulator test: no network there, so "download" a firmware placed at 0x150000 instead
// (copied back to front, since source and destination overlap)
static bool lmFetchFull(WiFiClientSecure &)
{
    const uint32_t src = 0x150000, size = LM_OLD_SLOT;
    lmSay("Test: copying the new firmware into place");
    HeapBuf mem(4096);
    uint8_t *buf = mem.p;
    for (int32_t off = size - 4096; off >= 0 && buf; off -= 4096)
    {
        if (esp_flash_read(NULL, buf, src + off, 4096) != ESP_OK ||
            esp_flash_erase_region(NULL, LM_NEW_APP1 + off, 4096) != ESP_OK ||
            esp_flash_write(NULL, buf, LM_NEW_APP1 + off, 4096) != ESP_OK)
            return lmFail("Test copy failed");
    }
    return true;
}
#else
static bool lmFetchFull(WiFiClientSecure &client)
{
    lmSay("Asking GitHub for the full edition");
    suForceFull = true;
    bool found = suFetchLatest(client);
    suForceFull = false;
    if (!found || !suAssetUrl[0] || strstr(suAssetUrl, "-lite"))
        return lmFail(found ? "The latest release has no full edition" : "Couldn't reach GitHub");
    if (suAssetSize < 100000 || suAssetSize > LM_NEW_SLOT)
        return lmFail("The full edition's size looks wrong");
    if (!suSigUrl[0])
        return lmFail("The full edition isn't signed");
    client.stop();

    lmSay("Getting the signature");
    static uint8_t sig[80];
    int sigLen = suFetchSignature(client, sig, sizeof(sig));
    if (sigLen < 8)
        return lmFail("Couldn't download the signature");

    lmSay("Making room");
    uint32_t span = (suAssetSize + 4095) & ~4095u;
    if (esp_flash_erase_region(NULL, LM_NEW_APP1, span) != ESP_OK)
        return lmFail("Couldn't clear the space for the new version");

    lmSay("Downloading the full edition");
    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = -1;
    if (http.begin(client, suAssetUrl))
    {
        http.setUserAgent("BLLED-Plus/" STRVERSION);
        code = http.GET();
    }
    if (code != 200)
    {
        http.end();
        snprintf(lmError, sizeof(lmError), "Download failed (%d)", code);
        return lmFail(lmError);
    }
    otaTotal = suAssetSize;
    WiFiClient *s = http.getStreamPtr();
    static uint8_t buf[1460];
    size_t got = 0;
    unsigned long lastData = millis();
    while (got < suAssetSize)
    {
        size_t avail = s->available();
        if (avail)
        {
            int n = s->readBytes(buf, min(min(avail, sizeof(buf)), (size_t)(suAssetSize - got)));
            if (n <= 0)
                continue;
            if (esp_flash_write(NULL, buf, LM_NEW_APP1 + got, n) != ESP_OK)
            {
                http.end();
                return lmFail("Writing the new version failed");
            }
            got += n;
            otaBytes = got;
            otaLastData = lastData = millis();
        }
        else if (millis() - lastData > 15000)
        {
            http.end();
            return lmFail("The download stopped arriving");
        }
        else
            vTaskDelay(pdMS_TO_TICKS(5));
    }
    http.end();

    // Check what was actually written: hash it back from flash and check the signature
    lmSay("Checking the signature");
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    static uint8_t chunk[1024];
    for (uint32_t off = 0; off < suAssetSize; off += sizeof(chunk))
    {
        size_t n = min((uint32_t)sizeof(chunk), suAssetSize - off);
        if (esp_flash_read(NULL, chunk, LM_NEW_APP1 + off, n) != ESP_OK)
        {
            mbedtls_sha256_free(&sha);
            return lmFail("Couldn't read back the new version");
        }
        mbedtls_sha256_update(&sha, chunk, n);
    }
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (!suSignatureOk(hash, sig, sigLen))
        return lmFail("The full edition isn't signed by BLLED Plus, so it wasn't used");
    return true;
}
#endif

static void lmTask(void *)
{
    lmError[0] = 0;
    bool ok = false;
    otaActive = true; // lights show the update color; other updates wait
    selfUpdating = true;
    otaBytes = 0;
    otaTotal = 0;
    otaLastData = millis();
    do
    {
        if (!lmOnOldLayout())
        {
            lmFail("This controller is already on the larger layout");
            break;
        }
        if (spi_flash_get_chip_size() < 4 * 1024 * 1024)
        {
            lmFail("This controller's chip is smaller than 4 MB");
            break;
        }
        if (!lmMoveToFirstSlot())
            break; // restarting to carry on, or stopped
        if (!lmSaveSettings())
            break;
        suMakeRoom(true); // the download needs the memory of the printer connection
        {
            WiFiClientSecure client;
            client.setCACertBundle(certBundle);
            client.setHandshakeTimeout(20);
            ok = lmFetchFull(client);
            client.stop();
        }
        if (!ok)
            break;
        ok = false;
        lmSay("Clearing the new settings area");
        if (esp_flash_erase_region(NULL, LM_NEW_DATA, LM_DATA_SIZE) != ESP_OK)
        {
            lmFail("Couldn't clear the new settings area");
            break;
        }
        lmSay("Setting the new version to start");
        if (!lmBootSecondSlot())
        {
            lmFail("Couldn't set the new version to start");
            break;
        }
        HeapBuf oldMem(3072), newMem(3072);
        uint8_t *oldTable = oldMem.p, *newTable = newMem.p;
        if (!oldTable || !newTable || esp_flash_read(NULL, oldTable, LM_TABLE, 3072) != ESP_OK)
        {
            lmFail("Couldn't read the current storage table");
            esp_ota_set_boot_partition(esp_ota_get_running_partition());
            break;
        }
        memset(newTable, 0xFF, 3072);
        memcpy(newTable, largeLayoutTableUsed, sizeof(largeLayoutTableUsed));
        lmSay("Writing the new storage table - keep it plugged in");
        if (!lmWriteTable(newTable))
        {
            // Put the old table back and keep starting this version
            bool restored = lmWriteTable(oldTable);
            esp_ota_set_boot_partition(esp_ota_get_running_partition());
            lmFail(restored ? "Writing the storage table didn't work, so nothing was changed. You can try again."
                            : "Writing the storage table didn't work and the old one couldn't be put back. Don't unplug it - ask for help.");
            break;
        }
        ok = true;
        lmDone = true;
        lmSay("Done - restarting into the full edition");
        noteRestart("Moved to the larger layout");
        shouldRestart = true;
        restartRequestTime = millis();
    } while (false);

    if (!ok && !shouldRestart)
    {
        otaActive = false;
        selfUpdating = false;
        otaPauseMqtt = false;
    }
    lmBusy = false;
    vTaskDelete(NULL);
}

bool lmStart()
{
    if (lmBusy || suBusy || otaActive || shouldRestart || !lmOnOldLayout())
        return false;
#ifndef BLLED_MIGRATE_TEST
    if (WiFi.status() != WL_CONNECTED)
        return false;
#endif
    lmBusy = true;
    if (xTaskCreatePinnedToCore(lmTask, "layoutMove", 12288, NULL, 1, NULL, 1) != pdPASS)
    {
        lmBusy = false;
        return false;
    }
    return true;
}

#ifdef BLLED_MIGRATE_TEST
// Emulator test, run during start-up (the emulator has no Wi-Fi, and starting it crashes)
void lmTestRun()
{
    if (!lmOnOldLayout())
    {
        LogSerial.printf("[Layout] Test: running %s on the larger layout, host=%s\n", STRVERSION, globalVariables.Host.c_str());
    }
    else
    {
        globalVariables.Host = "migtest"; // a setting to look for after the move
        saveFileSystem();
        lmStart();
    }
    for (;;)
    {
        if (shouldRestart && millis() - restartRequestTime > 1500)
            ESP.restart();
        delay(100);
    }
}
#endif

// From the main loop: carry on after the restart into the first slot
void lmLoop()
{
    static bool checked = false;
    if (checked)
        return;
#ifdef BLLED_MIGRATE_TEST
    checked = true;
    return;
#else
    if (millis() < 15000 || WiFi.status() != WL_CONNECTED)
        return;
    checked = true;
    Preferences p;
    bool resume = false;
    if (p.begin("blledmig", false))
    {
        resume = p.getBool("resume", false);
        if (resume)
            p.remove("resume");
        p.end();
    }
    if (resume)
    {
        LogSerial.println(F("[Layout] Carrying on with the move to the larger layout"));
        lmStart();
    }
#endif
}

#endif
