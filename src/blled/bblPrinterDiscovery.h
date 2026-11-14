#ifndef _BBLPRINTERDISCOVERY_H
#define _BBLPRINTERDISCOVERY_H

#include <WiFi.h>
#include <WiFiUdp.h>
#include "filesystem.h" // for saveFileSystem()

// Finds Bambu printers on the network (they answer SSDP on port 2021 with their serial as the USN).
//
// The original version sent a search and then sat in a 5-second loop waiting for answers, every
// 10 seconds - so the main loop (LED state changes, live page updates) was frozen more than half
// the time. Now a search goes out every 30 s and answers are picked up whenever they arrive.

#define BBL_SSDP_PORT 2021
#define BBL_SSDP_MCAST_IP IPAddress(239, 255, 255, 250)
#define BBL_DISCOVERY_INTERVAL 30000UL
#define BBL_MAX_PRINTERS 10

struct BBLPrinter
{
  IPAddress ip;
  char usn[64];
  unsigned long lastSeenMs;
};

static WiFiUDP bblUdp;
static bool bblUdpInitialized = false;
static unsigned long bblLastDiscovery = 0;
static bool bblFirstSearch = true;

static BBLPrinter bblLastKnownPrinters[BBL_MAX_PRINTERS];
static int bblKnownPrinterCount = 0;

bool bblIsPrinterKnown(IPAddress ip, int *index = nullptr)
{
  for (int i = 0; i < bblKnownPrinterCount; i++)
  {
    if (bblLastKnownPrinters[i].ip == ip)
    {
      if (index)
        *index = i;
      return true;
    }
  }
  return false;
}

// Serial number the printer at this IP announced, or "" if none seen there.
const char *bblSerialAt(const char *ipStr)
{
  IPAddress ip;
  if (!ip.fromString(ipStr))
    return "";
  int idx;
  if (bblIsPrinterKnown(ip, &idx))
    return bblLastKnownPrinters[idx].usn;
  return "";
}

// Where the printer with this serial answered from, or "" if not seen.
String bblIpOfSerial(const char *serial)
{
  if (!serial[0])
    return "";
  for (int i = 0; i < bblKnownPrinterCount; i++)
    if (strcmp(bblLastKnownPrinters[i].usn, serial) == 0)
      return bblLastKnownPrinters[i].ip.toString();
  return "";
}

// A different printer answers from our saved address (the router reshuffled addresses): don't
// log in there, and search straight away for where ours went.
bool bblWrongPrinterAtIp()
{
  const char *seen = bblSerialAt(printerConfig.printerIP);
  return seen[0] && printerConfig.serialNumber[0] && strcmp(seen, printerConfig.serialNumber) != 0;
}

void bblSearchSoon()
{
  bblLastDiscovery = millis() - BBL_DISCOVERY_INTERVAL;
}

void bblPrintKnownPrinters()
{
  LogSerial.println("[BBLScan] Known printers:");
  if (bblKnownPrinterCount == 0)
  {
    LogSerial.println("  (none)");
  }
  for (int i = 0; i < bblKnownPrinterCount; i++)
  {
    LogSerial.printf("  [%d] IP: %s", i + 1, bblLastKnownPrinters[i].ip.toString().c_str());
    if (strlen(bblLastKnownPrinters[i].usn) > 0)
    {
      LogSerial.printf("  [USN: %s]", bblLastKnownPrinters[i].usn);
    }
    LogSerial.println();
  }
}

static void bblHandleAnswer(IPAddress senderIP, const String &response)
{
  String usnStr = "";
  int usnPos = response.indexOf("USN:");
  if (usnPos >= 0)
  {
    int end = response.indexOf("\r\n", usnPos);
    usnStr = response.substring(usnPos + 4, end);
    usnStr.trim();
  }

  // IP update check for stored USN
  if (strlen(printerConfig.serialNumber) > 0 && usnStr.length() > 0 &&
      strcmp(printerConfig.serialNumber, usnStr.c_str()) == 0)
  {
    String currentIP = senderIP.toString();
    // Only follow an IP change when the configured printer has been unreachable for a
    // while. Any device on the LAN can answer SSDP with the printer's serial, and the
    // MQTT login (the access code) would then be sent to it.
    bool printerUnreachable = !printerVariables.online && printerVariables.disconnectMQTTms != 0 &&
                              (millis() - printerVariables.disconnectMQTTms) > 120000UL;
    // ...or straight away when another printer now answers from the saved address
    if (!printerVariables.online && bblWrongPrinterAtIp())
      printerUnreachable = true;
    if (String(printerConfig.printerIP) != currentIP && !printerUnreachable)
    {
      if (printerConfig.debugingchange)
        LogSerial.printf("[BBLScan] Serial answered from %s, but our printer is online - ignored\n", currentIP.c_str());
    }
    else if (String(printerConfig.printerIP) != currentIP)
    {
      LogSerial.printf("[BBLScan] Detected matching USN with updated IP (%s → %s). Saving...\n", printerConfig.printerIP, currentIP.c_str());
      strlcpy(printerConfig.printerIP, currentIP.c_str(), sizeof(printerConfig.printerIP));
      saveFileSystem();
      mqttReconnectNow = true; // connect to it now, not after the retry wait
    }
  }

  int idx = -1;
  bool isNew = !bblIsPrinterKnown(senderIP, &idx);
  if (isNew)
  {
    if (bblKnownPrinterCount >= BBL_MAX_PRINTERS)
      return;
    idx = bblKnownPrinterCount++;
    bblLastKnownPrinters[idx].ip = senderIP;
    bblLastKnownPrinters[idx].usn[0] = 0;
  }
  BBLPrinter &p = bblLastKnownPrinters[idx];
  bool changed = strcmp(p.usn, usnStr.c_str()) != 0;
  if (usnStr.length())
    strlcpy(p.usn, usnStr.c_str(), sizeof(p.usn));
  p.lastSeenMs = millis();

  if (printerConfig.debuging || ((isNew || changed) && printerConfig.debugingchange))
  {
    LogSerial.printf("[BBLScan] Printer at %s  [USN: %s]\n", senderIP.toString().c_str(), p.usn);
  }
}

// Called every loop pass: never blocks.
void bblSearchPrinters()
{
  if (!bblUdpInitialized)
  {
    bblUdp.beginMulticast(BBL_SSDP_MCAST_IP, BBL_SSDP_PORT);
    bblUdpInitialized = true;
  }

  // Pick up any answers that have arrived
  for (int n = 0; n < 4; n++)
  {
    int size = bblUdp.parsePacket();
    if (!size)
      break;
    char buffer[512];
    int len = bblUdp.read(buffer, sizeof(buffer) - 1);
    if (len <= 0)
      continue;
    buffer[len] = 0;
    // Our own M-SEARCH comes back on the multicast group too; only NOTIFY / HTTP answers count
    if (strncmp(buffer, "M-SEARCH", 8) == 0)
      continue;
    bblHandleAnswer(bblUdp.remoteIP(), String(buffer));
  }

  unsigned long now = millis();
  if (!bblFirstSearch && now - bblLastDiscovery < BBL_DISCOVERY_INTERVAL)
    return;
  bblFirstSearch = false;
  bblLastDiscovery = now;

  static const char msearch[] =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:2021\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 5\r\n"
      "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
  bblUdp.beginPacket(BBL_SSDP_MCAST_IP, BBL_SSDP_PORT);
  bblUdp.print(msearch);
  bblUdp.endPacket();

  if (printerConfig.debuging)
  {
    LogSerial.println("[BBLScan] Searching for printers...");
    bblPrintKnownPrinters();
  }
}

#endif
