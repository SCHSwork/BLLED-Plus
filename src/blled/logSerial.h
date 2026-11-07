#ifndef LOGSERIAL_H
#define LOGSERIAL_H

#include <Arduino.h>
#include <MycilaWebSerial.h>

class AsyncWebServer;

// ---- Activity log: the last few KB of log lines, kept in memory that survives a restart (not a
// power cut), so the status page can show what happened even after the controller rebooted ----
#define ACTLOG_SIZE 3584
#define ACTLOG_MAGIC 0xB11ED10Cu
struct ActivityLog
{
    uint32_t magic;
    uint16_t head; // next write position
    uint16_t used; // bytes in the ring
    char buf[ACTLOG_SIZE];
};
RTC_NOINIT_ATTR ActivityLog actLog;
static portMUX_TYPE actLogMux = portMUX_INITIALIZER_UNLOCKED;

static void actLogPushRaw(const char *p, size_t n)
{
    portENTER_CRITICAL(&actLogMux);
    if (actLog.magic != ACTLOG_MAGIC || actLog.head >= ACTLOG_SIZE || actLog.used > ACTLOG_SIZE)
    {
        actLog.magic = ACTLOG_MAGIC;
        actLog.head = 0;
        actLog.used = 0;
    }
    for (size_t i = 0; i < n; i++)
    {
        actLog.buf[actLog.head] = p[i];
        actLog.head = (actLog.head + 1) % ACTLOG_SIZE;
        if (actLog.used < ACTLOG_SIZE)
            actLog.used++;
    }
    portEXIT_CRITICAL(&actLogMux);
}

// One finished line: stamp it with the time (or time since start-up) and store it
static void actLogLine(const char *line, size_t n)
{
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
        n--;
    if (!n || line[0] == '{')
        return; // skip blank lines and raw printer messages
    char stamp[16];
    time_t now = time(nullptr);
    if (now > 1700000000)
    {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
    }
    else
    {
        unsigned long s = millis() / 1000;
        snprintf(stamp, sizeof(stamp), "+%lu:%02lu ", s / 60, s % 60);
    }
    actLogPushRaw(stamp, strlen(stamp));
    actLogPushRaw(line, n);
    actLogPushRaw("\n", 1);
}

// The stored lines, oldest first (a line cut in half by the ring wrapping is dropped)
String activityLogText()
{
    String out;
    portENTER_CRITICAL(&actLogMux);
    bool ok = actLog.magic == ACTLOG_MAGIC && actLog.head < ACTLOG_SIZE && actLog.used <= ACTLOG_SIZE;
    uint16_t used = ok ? actLog.used : 0, head = ok ? actLog.head : 0;
    portEXIT_CRITICAL(&actLogMux);
    if (!used)
        return out;
    out.reserve(used + 1);
    size_t start = (head + ACTLOG_SIZE - used) % ACTLOG_SIZE;
    bool skipPartial = used == ACTLOG_SIZE;
    for (size_t i = 0; i < used; i++)
    {
        char c = actLog.buf[(start + i) % ACTLOG_SIZE];
        if (skipPartial)
        {
            if (c == '\n')
                skipPartial = false;
            continue;
        }
        if ((c >= 32 && c < 127) || c == '\n')
            out += c;
    }
    return out;
}

void activityLogNote(const char *text) { actLogLine(text, strlen(text)); }

class LogSerialClass : public Stream {
private:
WebSerial webSerial;
public:
  void begin(unsigned long baud = 115200) {
    Serial.begin(baud);
  }

  void begin(AsyncWebServer* server, unsigned long baud = 115200, size_t bufferSize = 100) {
    Serial.begin(baud);
    webSerial.begin(server);
    webSerial.setBuffer(bufferSize);
    webSerial.onMessage([](const std::string& msg) {
      Serial.print("[WebSerial RX] ");
      Serial.println(msg.c_str());
    });
  }

  void onMessage(std::function<void(const std::string&)> cb) {
    webSerial.onMessage(cb);
  }

  // Must be called before begin(): protects the log WebSocket with the WebUI login.
  void setAuthentication(const char* user, const char* pass) {
    webSerial.setAuthentication(user, pass);
  }

  void setBuffer(size_t size) {
    webSerial.setBuffer(size);
  }

  int available() override { return Serial.available(); }
  int read() override { return Serial.read(); }
  int peek() override { return Serial.peek(); }
  void flush() override { Serial.flush(); }

  size_t write(uint8_t b) override {
    Serial.write(b);
    webSerial.write(&b, 1);
    keep(&b, 1);
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    Serial.write(buffer, size);
    webSerial.write(buffer, size);
    keep(buffer, size);
    return size;
  }

private:
  // Collect characters into lines for the activity log (several tasks log, so this is guarded)
  char line[120];
  size_t lineLen = 0;
  bool lineTooLong = false;
  portMUX_TYPE lineMux = portMUX_INITIALIZER_UNLOCKED;
  void keep(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      char c = (char)p[i];
      if (c == '\n') {
        char copy[120];
        size_t len;
        portENTER_CRITICAL(&lineMux);
        len = lineLen;
        memcpy(copy, line, len);
        lineLen = 0;
        lineTooLong = false;
        portEXIT_CRITICAL(&lineMux);
        actLogLine(copy, len);
      } else {
        portENTER_CRITICAL(&lineMux);
        if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
        else if (!lineTooLong) { lineTooLong = true; line[sizeof(line) - 4] = '.'; line[sizeof(line) - 3] = '.'; line[sizeof(line) - 2] = '.'; }
        portEXIT_CRITICAL(&lineMux);
      }
    }
  }
public:

  using Print::write;
  operator bool() { return (bool)Serial; }


};

LogSerialClass LogSerial;

#endif // LOGSERIAL_H
