#ifndef _LED
#define _LED

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "mqttparsingutility.h"
void controlChamberLight(bool on); // Forward declaration

// ---------------------------------------------------------------------------
//  LED engine
//
//  The original firmware faded colors with a blocking loop (~500 ms of delay()
//  per change) that was often called from inside the MQTT task, stalling the
//  MQTT connection. Its integer step math also rounded every step to 0, so it
//  never actually faded: it waited, then jumped.
//
//  Now tweenToColor() only records a target. A small FreeRTOS task renders the
//  output ~100 times a second: smooth cross-fades and effects (breathe, blink,
//  print-progress blend, rainbow) on 12-bit PWM. Fades and effects run in
//  perceptual (gamma) space, but a steady color lands on exactly the same duty
//  cycle as the original 8-bit firmware, so existing color and brightness
//  settings look the same.
// ---------------------------------------------------------------------------

const int redPin = 19;
const int greenPin = 18;
const int bluePin = 21;
const int warmPin = 22;
const int coldPin = 23;

const int redChannel = 0;
const int greenChannel = 1;
const int blueChannel = 2;
const int warmChannel = 3;
const int coldChannel = 4;

const int pwmFreq = 5000;
const int pwmResolution = 12; // 12-bit PWM = 0-4095 (5 kHz supports up to 13 bits)
const int pwmMax = (1 << pwmResolution) - 1;
const float LED_GAMMA = 2.2f;

enum LedFx : uint8_t
{
    FX_SOLID = 0,
    FX_BREATHE = 1,
    FX_BLINK = 2,
    FX_PROGRESS = 3,
    FX_RAINBOW = 4,
    FX_PULSE = 5, // slow, shallow pulse used for filament color
    FX_HEATDEMO = 6, // preview: plays the whole heat-up, cold to hot, over 7 s
    FX_ACCENT = 7    // printing: normal light, with a filament-color swell now and then
};

const char *ledFxName(uint8_t fx)
{
    switch (fx)
    {
    case FX_BREATHE: return "breathe";
    case FX_BLINK: return "blink";
    case FX_PROGRESS: return "progress";
    case FX_RAINBOW: return "rainbow";
    case FX_PULSE: return "pulse";
    case FX_HEATDEMO: return "heat";
    case FX_ACCENT: return "accent";
    default: return "solid";
    }
}

// Logical (brightness-scaled, 0-255) target. Kept for compatibility with the
// state logic below, e.g. the double-door-close toggle checks currentWarm/Cold.
int currentRed = 0;
int currentGreen = 0;
int currentBlue = 0;
int currentWarm = 0;
int currentCold = 0;

unsigned long lastUpdatems = 0;
unsigned long oldms = 0;

// Render state, shared with the render task (guarded by ledMux)
static portMUX_TYPE ledMux = portMUX_INITIALIZER_UNLOCKED;
static float ledFrom[5] = {0, 0, 0, 0, 0};   // perceptual 0..1
static float ledTo[5] = {0, 0, 0, 0, 0};     // perceptual 0..1
static float ledAlt[5] = {0, 0, 0, 0, 0};    // perceptual 0..1, end color for FX_PROGRESS
static float ledOut[5] = {0, 0, 0, 0, 0};    // perceptual 0..1, last rendered frame
static uint32_t ledFadeStart = 0;
static uint32_t ledFadeMs = 0;
static uint8_t ledFx = FX_SOLID;
static uint8_t ledNextFx = FX_SOLID;         // applied by the next tweenToColor()
static TaskHandle_t ledTaskHandle = NULL;
static volatile uint32_t celebrateUntil = 0; // finish celebration: fast rainbow until this time

// Preview from the settings page: show a color + effect on the real strip for a few seconds
static volatile uint32_t previewUntil = 0;
static COLOR previewColor;
static volatile uint8_t previewFx = 0;
static volatile bool previewStarted = false;

void startPreview(const COLOR &c, uint8_t fx, uint32_t ms, bool celebrate)
{
    previewColor = c;
    previewFx = fx;
    previewUntil = millis() + ms;
    previewStarted = true;
    if (celebrate)
        celebrateUntil = millis() + 3000;
}

bool previewActive()
{
    return previewUntil && (int32_t)(previewUntil - millis()) > 0;
}

// Requests from other tasks (MQTT, web) - handled in ledsloop() on the main task
volatile bool ledUpdateRequested = false;
volatile bool ledOffRequested = false;

void requestLedUpdate() { ledUpdateRequested = true; }
void requestLedOff() { ledOffRequested = true; }

static inline float toPerceptual(int v255)
{
    if (v255 <= 0)
        return 0.0f;
    if (v255 >= 255)
        return 1.0f;
    return powf((float)v255 / 255.0f, 1.0f / LED_GAMMA);
}

static inline uint32_t toDuty(float p)
{
    if (p <= 0.0f)
        return 0;
    if (p >= 1.0f)
        return pwmMax;
    return (uint32_t)(powf(p, LED_GAMMA) * pwmMax + 0.5f);
}

// ---------------------------------------------------------------------------
//  Quiet hours: during a nightly window the strip is capped at a lower brightness.
//  Maintenance (always-on white) ignores it - someone is working in the printer.
// ---------------------------------------------------------------------------
volatile bool quietNow = false;

bool clockSynced()
{
    return time(nullptr) > 1700000000; // NTP has set the clock
}

bool computeQuiet()
{
    if (!printerConfig.quietEnabled || printerConfig.quietStart == printerConfig.quietEnd || !clockSynced())
        return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    int m = t.tm_hour * 60 + t.tm_min;
    int s = printerConfig.quietStart, e = printerConfig.quietEnd;
    return (s < e) ? (m >= s && m < e) : (m >= s || m < e); // window may cross midnight
}

volatile bool workLightOn = false; // door work light showing: full brightness
bool spotlightOn = false;          // set only while the first-layer spotlight color is being applied
bool idleDimActive = false;        // idle timer ran out and the strip is dimmed (not off yet)

int effectiveBrightness()
{
    if (workLightOn)
        return 100;
    if (spotlightOn && !quietNow)
        return 100;
    if (quietNow && !printerConfig.maintMode)
        return min(printerConfig.brightness, printerConfig.quietBrightness);
    return printerConfig.brightness;
}

// ---------------------------------------------------------------------------
//  Filament color: the color the active AMS slot / external spool reports
// ---------------------------------------------------------------------------
bool filamentSwitching()
{
    int st = printerVariables.stage;
    return st == 4 || st == 22 || st == 24; // changing / unloading / loading filament
}

const char *activeFilamentHex()
{
    int n = printerVariables.trayNow;
    // While switching, show the spool being loaded (tray_now is often "none" mid-swap)
    int tar = printerVariables.trayTar;
    if (filamentSwitching() && ((tar >= 0 && tar < 16) || tar == 254))
        n = tar;
    if (n >= 0 && n < 16)
        return printerVariables.trayColor[n];
    if (n == 254)
        return printerVariables.extColor;
    return "";
}

// Turns a filament color into a good strip color. Returns false when the strip should use the
// normal running color instead: no filament info, or black / white filament (black would mean
// a dark chamber, and real white looks better on the white LED channels).
bool filamentLedColor(COLOR &out)
{
    const char *hex = activeFilamentHex();
    if (strlen(hex) != 6)
        return false;
    long v = strtol(hex, NULL, 16);
    int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
    int hi = max(r, max(g, b)), lo = min(r, min(g, b));
    if (hi < 40)
        return false; // black / very dark
    if (lo > 190 && hi - lo < 40)
        return false; // white / light grey
    // Filament colors are often dark (navy, forest green). Scale so the brightest channel is
    // full, keeping the hue, so every filament lights the chamber equally well.
    float k = 255.0f / hi;
    out.r = (short)(r * k);
    out.g = (short)(g * k);
    out.b = (short)(b * k);
    out.ww = 0;
    out.cw = 0;
    snprintf(out.RGBhex, sizeof(out.RGBhex), "#%02X%02X%02X", out.r, out.g, out.b);
    return true;
}

COLOR heatColorAt(float p); // heat-up colors, defined further down
float calGain(int ch);      // color balance, defined further down

COLOR hex2rgb(String hex, short ww_value = 0, short cw_value = 0)
{
    COLOR color;
    long hexcolor;
    strlcpy(color.RGBhex, hex.c_str(), sizeof(color.RGBhex));
    if (hex.charAt(0) == '#')
    {
        hex.remove(0, 1);
    }
    if (hex.length() > 6)
    {
        hex = hex.substring(0, 6);
    }
    while (hex.length() < 6)
    {
        hex += "0";
    }
    hexcolor = strtol(hex.c_str(), NULL, 16);
    color.r = (hexcolor & 0xFF0000) >> 16;
    color.g = (hexcolor & 0x00FF00) >> 8;
    color.b = (hexcolor & 0x0000FF);
    color.ww = constrain(ww_value, 0, 255);
    color.cw = constrain(cw_value, 0, 255);

    return color;
}

// The effect the next tweenToColor() call should use. Resets to solid after use,
// so every existing tweenToColor() call keeps behaving as a plain color.
void ledSetNextFx(uint8_t fx)
{
    ledNextFx = fx;
}

// Color the FX_PROGRESS effect blends towards as the print advances.
void ledSetProgressTarget(const COLOR &c)
{
    float b = (float)effectiveBrightness() / 100.0f;
    float alt[5] = {
        toPerceptual((int)(c.r * b * calGain(0))), toPerceptual((int)(c.g * b * calGain(1))), toPerceptual((int)(c.b * b * calGain(2))),
        toPerceptual((int)(c.ww * b)), toPerceptual((int)(c.cw * b))};
    portENTER_CRITICAL(&ledMux);
    memcpy(ledAlt, alt, sizeof(ledAlt));
    portEXIT_CRITICAL(&ledMux);
}

// Color balance: LED strips differ in how strong each color is (green is often much brighter than
// red), which shifts mixed colors - orange looks yellow, purple looks blue. These scale each RGB
// channel so colors look the way they're picked. White LEDs aren't affected.
bool previewActive();
int calTry[3] = {100, 100, 100}; // balance being tried from the settings page (not saved yet)
bool calTrying = false;          // ...used only while that preview is showing

float calGain(int ch)
{
    int v = ch == 0 ? printerConfig.calR : ch == 1 ? printerConfig.calG : printerConfig.calB;
    if (calTrying && previewActive())
        v = calTry[ch];
    return constrain(v, 10, 100) / 100.0f;
}

void tweenToColor(int targetRed, int targetGreen, int targetBlue, int targetWarm, int targetCold, int duration = -1)
{
    float brightness = (float)effectiveBrightness() / 100.0;

    int brightenedRed = (int)(constrain(targetRed, 0, 255) * brightness * calGain(0));
    int brightenedGreen = (int)(constrain(targetGreen, 0, 255) * brightness * calGain(1));
    int brightenedBlue = (int)(constrain(targetBlue, 0, 255) * brightness * calGain(2));
    int brightenedWarm = (int)(constrain(targetWarm, 0, 255) * brightness);
    int brightenedCold = (int)(constrain(targetCold, 0, 255) * brightness);

    uint8_t fx = ledNextFx;
    ledNextFx = FX_SOLID;

    if (brightenedRed == currentRed && brightenedGreen == currentGreen && brightenedBlue == currentBlue &&
        brightenedWarm == currentWarm && brightenedCold == currentCold && fx == ledFx)
    {
        if (printerConfig.debuging)
        {
            LogSerial.printf("LEDS already at color: (%d, %d, %d, %d, %d) fx:%s Brightness: %d\n",
                             currentRed, currentGreen, currentBlue, currentWarm, currentCold,
                             ledFxName(fx), printerConfig.brightness);
        }
        return;
    }

    currentRed = brightenedRed;
    currentGreen = brightenedGreen;
    currentBlue = brightenedBlue;
    currentWarm = brightenedWarm;
    currentCold = brightenedCold;

    float to[5] = {toPerceptual(currentRed), toPerceptual(currentGreen), toPerceptual(currentBlue),
                   toPerceptual(currentWarm), toPerceptual(currentCold)};

    portENTER_CRITICAL(&ledMux);
    memcpy(ledFrom, ledOut, sizeof(ledFrom)); // fade from whatever is showing right now
    memcpy(ledTo, to, sizeof(ledTo));
    ledFadeStart = millis();
    ledFadeMs = (duration < 0) ? (uint32_t)max(0, printerConfig.fadeMs) : (uint32_t)duration;
    ledFx = fx;
    portEXIT_CRITICAL(&ledMux);
}

// Helper functions to allow changes colors by using a COLOR, string or integer hex code
void tweenToColor(COLOR targetcolor, int duration = -1)
{
    tweenToColor(targetcolor.r, targetcolor.g, targetcolor.b, targetcolor.ww, targetcolor.cw, duration);
}
// Example:  tweenToColor("#33FD67")
void tweenToColor(String strTargetColor, short ww_value = 0, short cw_value = 0)
{
    COLOR targetcolor = hex2rgb(strTargetColor, ww_value, cw_value);
    tweenToColor(targetcolor.r, targetcolor.g, targetcolor.b, targetcolor.ww, targetcolor.cw);
}
// Example:  tweenToColor(0xFFACA5)
void tweenToColor(int hexValue, short ww_value = 0, short cw_value = 0)
{
    tweenToColor((hexValue >> 16) & 0xFF, (hexValue >> 8) & 0xFF, hexValue & 0xFF, ww_value, cw_value);
}

static void hsvToPerceptual(float h, float s, float v, float out[3])
{
    h = fmodf(h, 360.0f) / 60.0f;
    int i = (int)h;
    float f = h - i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i)
    {
    case 0: out[0] = v; out[1] = t; out[2] = p; break;
    case 1: out[0] = q; out[1] = v; out[2] = p; break;
    case 2: out[0] = p; out[1] = v; out[2] = t; break;
    case 3: out[0] = p; out[1] = q; out[2] = v; break;
    case 4: out[0] = t; out[1] = p; out[2] = v; break;
    default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

static void ledRenderFrame()
{
    float from[5], to[5], alt[5];
    uint32_t fadeStart, fadeMs;
    uint8_t fx;

    portENTER_CRITICAL(&ledMux);
    memcpy(from, ledFrom, sizeof(from));
    memcpy(to, ledTo, sizeof(to));
    memcpy(alt, ledAlt, sizeof(alt));
    fadeStart = ledFadeStart;
    fadeMs = ledFadeMs;
    fx = ledFx;
    portEXIT_CRITICAL(&ledMux);

    uint32_t now = millis();
    float t = (fadeMs == 0) ? 1.0f : min(1.0f, (float)(now - fadeStart) / (float)fadeMs);
    float ease = t * t * (3.0f - 2.0f * t); // smoothstep

    float out[5];
    for (int i = 0; i < 5; i++)
    {
        out[i] = from[i] + (to[i] - from[i]) * ease;
    }

    switch (fx)
    {
    case FX_BREATHE:
    {
        // 3 s period, never fully dark so the chamber stays visible
        float k = 0.15f + 0.85f * (0.5f - 0.5f * cosf(2.0f * PI * (float)(now % 3000) / 3000.0f));
        for (int i = 0; i < 5; i++)
            out[i] *= k;
        break;
    }
    case FX_PULSE:
    {
        // 5 s period, dips to ~45% - a steady, soft pulse rather than a flash
        float k = 0.45f + 0.55f * (0.5f + 0.5f * cosf(2.0f * PI * (float)(now % 5000) / 5000.0f));
        for (int i = 0; i < 5; i++)
            out[i] *= k;
        break;
    }
    case FX_ACCENT:
    {
        // Normal light most of the time. Once a minute, two slow swells into the filament color
        // (ledAlt) and back, 10 s in total.
        const uint32_t period = 60000, swell = 10000;
        uint32_t ph = now % period;
        if (ph < swell)
        {
            float w = 0.5f - 0.5f * cosf(4.0f * PI * (float)ph / (float)swell);
            for (int i = 0; i < 5; i++)
                out[i] = out[i] + (alt[i] * ease - out[i]) * w;
        }
        break;
    }
    case FX_BLINK:
    {
        float k = ((now / 500) & 1) ? 1.0f : 0.08f;
        for (int i = 0; i < 5; i++)
            out[i] *= k;
        break;
    }
    case FX_PROGRESS:
    {
        int pct = printerVariables.percent;
        if (pct >= 0)
        {
            float p = constrain(pct, 0, 100) / 100.0f;
            for (int i = 0; i < 5; i++)
                out[i] = out[i] + (alt[i] - out[i]) * p * ease;
        }
        break;
    }
    case FX_HEATDEMO:
    {
        COLOR h = heatColorAt((float)(now - fadeStart) / 7000.0f);
        float k = (float)effectiveBrightness() / 100.0f;
        out[0] = toPerceptual((int)(h.r * k * calGain(0)));
        out[1] = toPerceptual((int)(h.g * k * calGain(1)));
        out[2] = toPerceptual((int)(h.b * k * calGain(2)));
        out[3] = 0;
        out[4] = 0;
        break;
    }
    case FX_RAINBOW:
    {
        float v = toPerceptual((int)(255 * effectiveBrightness() / 100.0f));
        float rgb[3];
        hsvToPerceptual((float)(now % 12000) * 360.0f / 12000.0f, 1.0f, v, rgb);
        for (int i = 0; i < 3; i++)
        {
            rgb[i] *= powf(calGain(i), 1.0f / LED_GAMMA); // gain applies to light, rgb is perceptual
            out[i] = from[i] + (rgb[i] - from[i]) * ease;
        }
        out[3] = from[3] * (1.0f - ease);
        out[4] = from[4] * (1.0f - ease);
        break;
    }
    default:
        break;
    }

    // Finish celebration: a fast rainbow sweep, blending into the finish color over the last 0.6 s
    uint32_t cu = celebrateUntil;
    if (cu && (int32_t)(cu - now) > 0)
    {
        float w = min(1.0f, (float)(cu - now) / 600.0f);
        float v = toPerceptual((int)(255 * effectiveBrightness() / 100.0f));
        float rgb[3];
        hsvToPerceptual((float)(now % 1200) * 360.0f / 1200.0f, 1.0f, v, rgb);
        for (int i = 0; i < 3; i++)
            out[i] = rgb[i] * w + out[i] * (1.0f - w);
        out[3] *= (1.0f - w);
        out[4] *= (1.0f - w);
    }

    portENTER_CRITICAL(&ledMux);
    memcpy(ledOut, out, sizeof(ledOut));
    portEXIT_CRITICAL(&ledMux);

    ledcWrite(redChannel, toDuty(out[0]));
    ledcWrite(greenChannel, toDuty(out[1]));
    ledcWrite(blueChannel, toDuty(out[2]));
    ledcWrite(warmChannel, toDuty(out[3]));
    ledcWrite(coldChannel, toDuty(out[4]));
}

static void ledTask(void *)
{
    for (;;)
    {
        ledRenderFrame();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// What the strip is showing right now, for the status page. Hex RGB + WW/CW in 0-255 duty terms.
void ledGetOutput(char *hexOut, size_t len, int &ww, int &cw, const char *&fxName)
{
    float out[5];
    uint8_t fx;
    portENTER_CRITICAL(&ledMux);
    memcpy(out, ledOut, sizeof(out));
    fx = ledFx;
    portEXIT_CRITICAL(&ledMux);
    int v[5];
    for (int i = 0; i < 5; i++)
        v[i] = (int)(powf(constrain(out[i], 0.0f, 1.0f), LED_GAMMA) * 255.0f + 0.5f);
    snprintf(hexOut, len, "#%02X%02X%02X", v[0], v[1], v[2]);
    ww = v[3];
    cw = v[4];
    fxName = ledFxName(fx);
}

// RGB Cycle ("disco") mode is now rendered by the LED task as FX_RAINBOW
void RGBCycle()
{
    if (printerConfig.discoMode == false)
    {
        printerConfig.discoMode_update = true;
        return;
    }
    if (printerConfig.discoMode_update)
    {
        printerConfig.discoMode_update = false;
        if (printerConfig.debugingchange)
        {
            LogSerial.printf("[%lu] ** RGB Cycle Mode **\n", millis());
        }
    }

    if (printerVariables.online == false)
    {
        if (ledFx == FX_RAINBOW || currentRed || currentGreen || currentBlue || currentWarm || currentCold)
            tweenToColor(0, 0, 0, 0, 0); // OFF while printer is offline
        return;
    }
    if (ledFx != FX_RAINBOW)
    {
        ledSetNextFx(FX_RAINBOW);
        tweenToColor(255, 255, 255, 0, 0, 1500);
    }
}

void printLogs(String Desc, COLOR thisColor)
{
    static COLOR lastColor = {-1, -1, -1, -1, -1};
    static String lastDesc = "";
    static unsigned long lastPrintTime = 0;

    // Skip if same state and printed less than 3 seconds ago
    if (Desc == lastDesc &&
        memcmp(&thisColor, &lastColor, sizeof(COLOR)) == 0 &&
        millis() - lastPrintTime < 3000)
    {
        return;
    }

    if (printerConfig.debuging || printerConfig.debugingchange)
    {
        LogSerial.printf("%s - Turning LEDs to:", Desc.c_str());
        if ((thisColor.r + thisColor.g + thisColor.b + thisColor.ww + thisColor.cw) == 0)
        {
            LogSerial.println(" OFF");
        }
        else
        {
            LogSerial.printf(" r:%d g:%d b:%d ww:%d cw:%d Brightness: %d fx:%s\n",
                             thisColor.r, thisColor.g, thisColor.b,
                             thisColor.ww, thisColor.cw, printerConfig.brightness, ledFxName(ledFx));
        }
    }

    lastColor = thisColor;
    lastDesc = Desc;
    lastPrintTime = millis();
}

void printLogs(String Desc, short r, short g, short b, short ww, short cw)
{
    COLOR tempColor;
    tempColor.r = r;
    tempColor.g = g;
    tempColor.b = b;
    tempColor.ww = ww;
    tempColor.cw = cw;
    printLogs(Desc, tempColor);
}

// Is the finished-print color being held (until the door is used, a timer runs out, or the bed cools)?
bool holdingFinish()
{
    return (printerConfig.finishExit && printerVariables.waitingForDoor) ||
           (printerConfig.finishExit == false && ((millis() - printerConfig.finishStartms) < (unsigned long)printerConfig.finishTimeOut)) ||
           printerVariables.coolWaiting;
}

bool printingState()
{
    return printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE" || printerVariables.gcodeState == "PREPARE";
}

// A part is heating when it has a target and is still clearly below it
bool bedHeating() { return printerVariables.bedTarget > 30 && printerVariables.bedTemp < printerVariables.bedTarget - 3; }
bool nozzleHeating() { return printerVariables.nozzleTarget > 30 && printerVariables.nozzleTemp < printerVariables.nozzleTarget - 5; }

// Heat-up glow applies while the printer preheats for a print (stage 2 bed, 7 hotend), or whenever
// the bed or nozzle is heating outside a print (e.g. set from the printer screen)
bool isLidarStage(int st);

bool heatGlowActive()
{
    if (!printerConfig.heatGlowFx)
        return false;
    if (printerVariables.stage == 2 || printerVariables.stage == 7)
        return true;
    bool heating = bedHeating() || nozzleHeating();
    if (printingState())
    {
        // Any heating before the first layer starts (printers differ in which startup step heats),
        // but not while printing, paused, or during a lidar scan that needs a dark chamber
        return heating && printerVariables.stage != 0 && !isLidarStage(printerVariables.stage) &&
               printerVariables.gcodeState != "PAUSE";
    }
    // Outside a print (e.g. a temperature set on the printer screen); setting a temperature also
    // clears a finished print's hold, see the MQTT handler
    return heating && !printerVariables.coolWaiting;
}

// The heat-up color for progress p (0 = cold, 1 = at temperature): blue fades down, then orange
// fades up. Mixing blue and orange directly gives greys and whites, and going around the color
// wheel passes through pink one way and green the other, so instead the change happens while
// both are dim: blue 100% -> 30% -> out, then orange in -> 30% -> 100%.
COLOR heatColorAt(float p)
{
    p = constrain(p, 0.0f, 1.0f);
    const float lo = 0.30f, w = 0.02f; // dimmest point, and half-width of the hand-over
    float blue = 0.0f, orange = 0.0f;
    if (p < 0.5f - w)
        blue = 1.0f - (1.0f - lo) * p / (0.5f - w);
    else if (p > 0.5f + w)
        orange = lo + (1.0f - lo) * (p - 0.5f - w) / (0.5f - w);
    else
    {
        // Blue finishes fading out before orange starts, so the two never mix into a pinkish color
        float t = (p - (0.5f - w)) / (2.0f * w); // 0..1 across the hand-over
        // (down to a faint 6%, never fully dark, so it doesn't look like the light went off)
        const float faint = 0.06f;
        if (t < 0.5f)
            blue = lo - (lo - faint) * 2.0f * t;
        else
            orange = faint + (lo - faint) * (2.0f * t - 1.0f);
    }
    COLOR c;
    c.r = (short)(255.0f * orange + 0.5f);
    // Reddish orange (#FF2800): LED strips make green look strong, so a "normal" orange like
    // #FF6A00 comes out yellowish on the strip
    c.g = (short)(64.0f * blue + 40.0f * orange + 0.5f);
    c.b = (short)(255.0f * blue + 0.5f);
    c.ww = 0;
    c.cw = 0;
    snprintf(c.RGBhex, sizeof(c.RGBhex), "#%02X%02X%02X", c.r, c.g, c.b);
    return c;
}

// Heat-up glow: blue -> orange as the heating parts approach their targets
COLOR heatGlowColor()
{
    float sum = 0;
    int n = 0;
    bool any = bedHeating() || nozzleHeating();
    // Only parts still heating count (a bed already at temperature shouldn't make a cold nozzle look warm)
    if (printerVariables.bedTarget > 30 && (!any || bedHeating()))
    {
        sum += constrain((printerVariables.bedTemp - 25.0f) / (printerVariables.bedTarget - 25.0f), 0.0f, 1.0f);
        n++;
    }
    if (printerVariables.nozzleTarget > 30 && (!any || nozzleHeating()))
    {
        sum += constrain((printerVariables.nozzleTemp - 25.0f) / (printerVariables.nozzleTarget - 25.0f), 0.0f, 1.0f);
        n++;
    }
    float p = n ? sum / n : 0.0f;
    return heatColorAt(p);
}

// 0 outside the "almost done" window; rises to 1 as the print's remaining time runs out
float almostDoneBlend()
{
    if (!printerConfig.almostDoneEnabled || printerVariables.gcodeState != "RUNNING" || printerVariables.stage != 0)
        return 0.0f;
    int left = printerVariables.remainingMins, window = max(1, printerConfig.almostDoneMins);
    if (left < 0 || left > window || printerVariables.percent <= 0)
        return 0.0f;
    // At "window" minutes left: 0.2 (a visible first step); at 0 left: 1.0
    return 0.2f + 0.8f * (float)(window - left) / (float)window;
}

bool isLidarStage(int st)
{
    return st == 1 || st == 8 || st == 9 || st == 10 || st == 12 || st == 14;
}

// The part of the printer an active HMS warning is about (0x07 AMS, 0x0C camera / AI checks), 0 if none
uint8_t hmsModule()
{
    if (!printerVariables.hmsstate)
        return 0;
    return (uint8_t)((printerVariables.parsedHMScode >> 56) & 0xFF);
}

// First-layer spotlight: full white while the first layer prints. Only in plain printing (stage 0),
// so every lidar step - bed scan, extrusion calibration, first-layer inspection - keeps its own
// (dark) color.
bool spotlightWanted()
{
    return printerConfig.firstLayerSpotlight && printerVariables.online &&
           printerVariables.gcodeState == "RUNNING" && printerVariables.stage == 0 &&
           printerVariables.overridestage != 10 && printerVariables.layer == 1;
}

// How long the idle timer has been running out
unsigned long idleForMs() { return millis() - printerConfig.inactivityStartms; }
bool idleDimExpired()
{
    return printerConfig.idleOffMins > 0 &&
           idleForMs() > (unsigned long)printerConfig.inactivityTimeOut + (unsigned long)printerConfig.idleOffMins * 60000UL;
}

// Shorthand: show a color with an effect
static void showFx(uint8_t fx, const COLOR &c)
{
    ledSetNextFx(fx);
    tweenToColor(c);
}

void updateleds()
{
    if (previewActive())
    {
        ledSetNextFx(previewFx);
        tweenToColor(previewColor, 300);
        printLogs("Preview", previewColor);
        return;
    }

    // Prevent replicate OFF immediately after door event
    if ((millis() - printerVariables.lastdoorOpenms) < 1000 || (millis() - printerVariables.lastdoorClosems) < 1000)
    {
        printerConfig.replicate_update = false;
    }

    // Maintenance Mode - White lights on regardless of printer power, WiFi or MQTT connection
    // priortised over Wifi Strength Display or Custom TEST color
    if (printerConfig.maintMode && printerConfig.maintMode_update)
    {
        tweenToColor(0, 0, 0, 255, 255); // WHITE
        printerConfig.maintMode_update = false;
        printLogs("Maintenance Mode", 0, 0, 0, 255, 255);
        LogSerial.printf("[%lu] ** Maintenance Mode **\n", millis());
        return;
    }

    // Use LED to show WIFI Strength (enabled via Setup Webpage)
    if (printerConfig.debugwifi)
    {
        //<=-50 dBm Green, <= -60 dBm LightGreen, <= -70 dBm Yellow, <= -80 dBm Orange, >80 Red
        if (WiFi.status() == WL_CONNECTED)
        {
            long wifiNow = WiFi.RSSI();
            if (printerConfig.debugingchange)
            {
                LogSerial.print(F("WiFi Strength Visialisation, display LEDs for: "));
                LogSerial.println(wifiNow);
            }
            if (wifiNow >= -50)
                tweenToColor(0, 255, 0, 0, 0); // GREEN
            else if (wifiNow >= -60)
                tweenToColor(128, 255, 0, 0, 0); // LIGHTGREEN
            else if (wifiNow >= -70)
                tweenToColor(255, 255, 0, 0, 0); // YELLOW
            else if (wifiNow >= -80)
                tweenToColor(255, 128, 0, 0, 0); // ORANGE
            else
                tweenToColor(255, 0, 0, 0, 0); // RED
        };
        return;
    }

    // TEST Color Enabled - LED ON regardless of printer state
    if (printerConfig.testcolorEnabled && printerConfig.testcolor_update)
    {
        tweenToColor(printerConfig.testColor); // Variable Test Color
        printLogs("LED Test ON", printerConfig.testColor);
        LogSerial.printf("[%lu] ** Test Color Mode **\n", millis());
        printerConfig.testcolor_update = false;
        return;
    }

    // From here the BBLP status sets the colors
    if (printerConfig.debuging == true)
    {
        char ledDbgStr[128];
        snprintf(ledDbgStr, sizeof(ledDbgStr),
                 "[LED] Stage:%d gcodeState:%s printerLedState:%s HMSErr:%s ParsedHMS:%s",
                 printerVariables.stage,
                 printerVariables.gcodeState.c_str(),
                 printerVariables.printerledstate ? "true" : "false",
                 printerVariables.hmsstate ? "true" : "false",
                 printerVariables.parsedHMSlevel.c_str());
        LogSerial.println(ledDbgStr);
    }

    bool wasDim = idleDimActive;
    idleDimActive = false; // the idle block below sets it again while it applies

    // Initial Boot
    if (printerVariables.initalisedLEDs == false)
    {
        printerVariables.initalisedLEDs = true;     // Run once per boot
        printerConfig.inactivityStartms = millis(); // restart idle timer
        printerConfig.isIdleOFFActive = false;
        printerVariables.waitingForDoor = false;
        printerConfig.finish_check = false;
        printerVariables.lastdoorClosems = millis();
        LogSerial.println(F("Initial Boot"));
        return;
    }

    if (printerConfig.testcolorEnabled || printerConfig.maintMode || printerConfig.debugwifi || printerConfig.discoMode)
    {
        // Skip trying to set a color as it's in one of the override states
        return;
    }

    // Door work light: bright white while the door is open (not while the lidar needs darkness)
    bool wantWorkLight = printerConfig.doorWorkLight && printerVariables.doorOpen && !isLidarStage(printerVariables.stage) && printerVariables.online;
    if (wantWorkLight)
    {
        workLightOn = true;
        tweenToColor(0, 0, 0, 255, 255, 300); // quick, it's a light switch
        printLogs("Door open - work light", 0, 0, 0, 255, 255);
        return;
    }
    // The door was opened while the lights were off: closing it turns them off again
    if (printerVariables.ledWasForcedByDoor && !printerVariables.doorOpen && !printerVariables.doorSwitchTriggered)
    {
        printerVariables.ledWasForcedByDoor = false;
        bool printing = printerVariables.gcodeState == "RUNNING" || printerVariables.gcodeState == "PAUSE" || printerVariables.gcodeState == "PREPARE";
        if (!printing)
        {
            workLightOn = false;
            tweenToColor(0, 0, 0, 0, 0, 1500);
            // Back to the idle-off state, as if the inactivity timer had run out
            printerConfig.isIdleOFFActive = true;
            printerConfig.inactivityStartms = millis() - printerConfig.inactivityTimeOut - 1;
            if (printerConfig.controlChamberLight && printerVariables.chamberLightLocked)
            {
                controlChamberLight(false);
                printerVariables.chamberLightLocked = false;
            }
            printLogs("Door closed - back to off", 0, 0, 0, 0, 0);
            return;
        }
    }

    if (workLightOn)
    {
        workLightOn = false;
        // Fall through: whatever the printer state calls for is applied below
        printerConfig.replicate_update = true;
        if (holdingFinish() && printerConfig.finishindication)
            printerVariables.finished = true; // re-show the finish color after the door closes
    }

    // TOGGLE LIGHTS via DOOR
    // If door is closed twice within 2 seconds, it will flip the state of the lights
    if (printerVariables.doorSwitchTriggered == true)
    {
        printerVariables.ledWasForcedByDoor = false;
        bool ledsAreOff = (currentWarm == 0 && currentCold == 0);
        bool chamberLightIsOff = (printerVariables.printerledstate == false);

        if (printerConfig.debugingchange)
        {
            LogSerial.print(F("Door closed twice within 2 seconds - "));
            LogSerial.print(ledsAreOff ? F("Turning LEDs ON") : F("Turning LEDs OFF"));
            if (printerConfig.controlChamberLight)
            {
                if (ledsAreOff && chamberLightIsOff)
                    LogSerial.println(F(" + Chamber Light ON"));
                else if (!ledsAreOff)
                    LogSerial.println(F(" + Chamber Light OFF"));
                else
                    LogSerial.println();
            }
            else
            {
                LogSerial.println();
            }
        }

        if (ledsAreOff)
        {
            tweenToColor(0, 0, 0, 255, 255); // WHITE
            printerConfig.isIdleOFFActive = false;

            if (printerConfig.controlChamberLight && chamberLightIsOff)
            {
                controlChamberLight(true); // Turn ON chamber light only if off
            }
        }
        else
        {
            tweenToColor(0, 0, 0, 0, 0); // OFF
            printerConfig.isIdleOFFActive = true;
            printerConfig.inactivityStartms = millis() - printerConfig.inactivityTimeOut;

            if (printerConfig.controlChamberLight)
            {
                controlChamberLight(false); // Always OFF on manual off
            }
        }

        printerVariables.doorSwitchTriggered = false;
        return;
    }

    // Wrong access code: blink dim red until it's fixed (printer refuses the MQTT login)
    if (printerVariables.online == false && printerVariables.mqttAuthFailed)
    {
        ledSetNextFx(FX_BLINK);
        tweenToColor(127, 0, 0, 0, 0);
        printLogs("MQTT login refused (check access code)", 127, 0, 0, 0, 0);
        return;
    }

    // RED -- RED -- RED -- RED

    // allow errordetection to turn ledstrip red
    if (printerConfig.errordetection == true)
    {
        // Fillament runout
        if (printerVariables.stage == 6 || printerVariables.overridestage == 6)
        {
            showFx(printerConfig.errorFx, printerConfig.filamentRunoutRGB);
            printLogs("Stage 6, FILAMENT RUNOUT", printerConfig.filamentRunoutRGB);
            return;
        };

        // Front Cover Removed
        if (printerVariables.stage == 17 || printerVariables.overridestage == 17)
        {
            showFx(printerConfig.errorFx, printerConfig.frontCoverRGB);
            printLogs("Stage 17, FRONT COVER REMOVED", printerConfig.frontCoverRGB);
            return;
        };

        // Nozzle Temp fail
        if (printerVariables.stage == 20 || printerVariables.overridestage == 20)
        {
            showFx(printerConfig.errorFx, printerConfig.nozzleTempRGB);
            printLogs("Stage 20, NOZZLE TEMP FAIL", printerConfig.nozzleTempRGB);
            return;
        };

        // Bed Temp Fail
        if (printerVariables.stage == 21 || printerVariables.overridestage == 21)
        {
            showFx(printerConfig.errorFx, printerConfig.bedTempRGB);
            printLogs("Stage 21, BED TEMP FAIL", printerConfig.bedTempRGB);
            return;
        };
        // SERIOUS HMS state
        if (printerVariables.parsedHMSlevel == "Serious")
        {
            if (hmsModule() == 0x07)
            {
                showFx(printerConfig.errorFx, printerConfig.amsErrorRGB);
                printLogs("AMS PROBLEM", printerConfig.amsErrorRGB);
                return;
            }
            showFx(printerConfig.errorFx, printerConfig.hmsSeriousRGB);
            LogSerial.printf("HMS SERIOUS Severity      Error Code: %016llX\n", printerVariables.parsedHMScode);
            printLogs("PROBLEM", printerConfig.hmsSeriousRGB);
            return;
        };

        // FATAL HMS state
        if (printerVariables.parsedHMSlevel == "Fatal")
        {
            if (hmsModule() == 0x07)
            {
                showFx(printerConfig.errorFx, printerConfig.amsErrorRGB);
                printLogs("AMS PROBLEM", printerConfig.amsErrorRGB);
                return;
            }
            showFx(printerConfig.errorFx, printerConfig.hmsFatalRGB);
            LogSerial.printf("HMS FATAL Severity      Error Code: %016llX\n", printerVariables.parsedHMScode);
            printLogs("PROBLEM", printerConfig.hmsFatalRGB);
            return;
        };
    };

    // PAUSED / PROBLEMS -- what kind of problem decides the color

    {
        int st = printerVariables.stage;
        bool paused = printerVariables.gcodeState == "PAUSE" || st == 5 || st == 16 || st == 30;

        // First-layer problem, or the AI camera check (spaghetti etc.) paused the print
        if (st == 34 || (paused && hmsModule() == 0x0C))
        {
            showFx(printerConfig.pauseFx, printerConfig.firstlayerRGB);
            printLogs("PAUSED - first layer / print quality problem", printerConfig.firstlayerRGB);
            return;
        }
        // Nozzle clog
        if (st == 35)
        {
            showFx(printerConfig.pauseFx, printerConfig.nozzleclogRGB);
            printLogs("Stage 35, NOZZLE CLOG, PAUSED", printerConfig.nozzleclogRGB);
            return;
        }
        // AMS problem
        if (printerConfig.errordetection && (st == 26 || (paused && hmsModule() == 0x07)))
        {
            showFx(printerConfig.errorFx, printerConfig.amsErrorRGB);
            printLogs("PAUSED - AMS problem", printerConfig.amsErrorRGB);
            return;
        }
        // Paused by you (screen or app): the normal pause color
        if (st == 16 || (paused && !printerConfig.needsYouEnabled))
        {
            showFx(printerConfig.pauseFx, printerConfig.pauseRGB);
            printLogs("PAUSED by you", printerConfig.pauseRGB);
            return;
        }
        // Paused for any other reason - a pause in the file, a cutter or nozzle problem, an AMS
        // that needs a hand: the printer is waiting for you
        bool waitingStage = st == 5 || st == 23 || st == 27 || st == 28 || st == 30 || st == 32 || st == 33;
        if (paused || (waitingStage && printingState()))
        {
            showFx(FX_BREATHE, printerConfig.needsYouRGB);
            printLogs("PAUSED - waiting for you", printerConfig.needsYouRGB);
            return;
        }
    }

    // HEAT / FILAMENT PREVIEW -- these also run outside a print, so they come before the "off" states

    // Switching filament (AMS swap or load / unload): pulse in the incoming spool's color. Comes
    // before the heat-up glow, since the nozzle often reheats during a swap.
    {
        COLOR c;
        if (printerVariables.online && printerConfig.filamentColorFx && filamentSwitching() && filamentLedColor(c))
        {
            ledSetNextFx(FX_PULSE);
            tweenToColor(c, 1500);
            printLogs("Switching filament - filament color", c);
            return;
        }
    }

    // Heat-up glow while preheating for a print, or while the bed / nozzle heats from the screen
    if (printerVariables.online && heatGlowActive())
    {
        COLOR c = heatGlowColor();
        tweenToColor(c, 1500);
        printLogs("Heating - heat-up glow", c);
        return;
    }

    // Filament preview: for 30 s after a different spool becomes active outside a print
    {
        COLOR c;
        bool preview = !printingState() && (int32_t)(printerVariables.filamentPreviewUntil - millis()) > 0;
        if (printerVariables.online && printerConfig.filamentColorFx && preview && filamentLedColor(c))
        {
            ledSetNextFx(FX_PULSE);
            tweenToColor(c, 1500);
            printLogs("New spool - filament color preview", c);
            return;
        }
    }

    // OFF -- OFF -- OFF -- OFF

    // printer offline and MQTT disconnected for more than 30 seconds.
    if (printerVariables.online == false && (millis() - printerVariables.disconnectMQTTms) >= 30000)
    {
        tweenToColor(0, 0, 0, 0, 0); // OFF
        printLogs("Printer offline", 0, 0, 0, 0, 0);
        return;
    }

    // replicate printer behaviour OFF
    if (printerConfig.replicatestate && printerConfig.replicate_update && printerVariables.printerledstate == false)
    {
        tweenToColor(0, 0, 0, 0, 0); // OFF
        printLogs("LED Replication OFF", 0, 0, 0, 0, 0);
        printerConfig.replicate_update = false;
        return;
    }

    // Cleaning nozzle
    if (printerVariables.stage == 14)
    {
        tweenToColor(printerConfig.stage14Color); // Customisable - Default is OFF
        printLogs("Stage 14, CLEANING NOZZLE", printerConfig.stage14Color);
        return;
    }

    // Auto Bed Leveling
    if (printerVariables.stage == 1)
    {
        tweenToColor(printerConfig.stage1Color); // Customisable - Default is OFF
        printLogs("Stage 1, BED LEVELING", printerConfig.stage1Color);
        return;
    }

    // Calibrating Extrusion
    if (printerVariables.stage == 8)
    {
        tweenToColor(printerConfig.stage8Color); // Customisable - Default is OFF
        printLogs("Stage 8, CALIBRATING EXTRUSION", printerConfig.stage8Color);
        return;
    }

    // Scaning surface
    if (printerVariables.stage == 9)
    {
        tweenToColor(printerConfig.stage9Color); // Customisable - Default is OFF
        printLogs("Stage 9, SCANNING BED SURFACE", printerConfig.stage9Color);
        return;
    }

    // Inspecting First Layer
    if (printerVariables.stage == 10 || printerVariables.overridestage == 10)
    {
        tweenToColor(printerConfig.stage10Color); // Customisable - Default is OFF
        printLogs("Stage 10 / HMS 0C00, FIRST LAYER INSPECTION", printerConfig.stage10Color);
        return;
    }

    // Calibrating  MicroLidar
    if (printerVariables.stage == 12)
    {
        tweenToColor(printerConfig.stage10Color);
        printLogs("Stage 12, CALIBRATING MICRO LIDAR", printerConfig.stage10Color);
        return;
    }

    // Idle Timeout (Has to be enabled): dim first if wanted, then off
    bool idleTimedOut = (printerVariables.stage == -1 || printerVariables.stage == 255) && !holdingFinish() &&
                        idleForMs() > (unsigned long)printerConfig.inactivityTimeOut && printerConfig.inactivityEnabled;
    if (idleTimedOut && printerConfig.idleDimEnabled && printerConfig.isIdleOFFActive == false && !idleDimExpired())
    {
        COLOR d = printerConfig.runningColor;
        float k = printerConfig.idleDimLevel / 100.0f;
        d.r = (short)(d.r * k); d.g = (short)(d.g * k); d.b = (short)(d.b * k);
        d.ww = (short)(d.ww * k); d.cw = (short)(d.cw * k);
        // Keep at least a faint glow, even with a low brightness setting
        if (d.r + d.g + d.b + d.ww + d.cw == 0)
            d.ww = d.cw = 3;
        idleDimActive = true;
        tweenToColor(d, 3000);
        if (!wasDim)
        {
            controlChamberLight(false);
            printLogs("Idle - dimmed", d);
        }
        return;
    }
    if (idleTimedOut && printerConfig.isIdleOFFActive == false)
    {
        tweenToColor(0, 0, 0, 0, 0, 2000); // OFF, slow fade
        controlChamberLight(false);        // Turn off chamber light via MQTT
        printerConfig.isIdleOFFActive = true;
        if (printerConfig.debuging || printerConfig.debugingchange)
        {
            LogSerial.printf("Idle Timeout [%d mins] - Turning LEDs OFF\n", (int)(printerConfig.inactivityTimeOut / 60000));
        };
        return;
    }

    // ON -- ON -- ON -- ON

    // Preheating Bed
    if (printerVariables.stage == 2)
    {
        tweenToColor(printerConfig.runningColor); // Customisable - Default is WHITE
        printLogs("Stage 2, PREHEATING BED", printerConfig.runningColor);
        return;
    }

    // First layer: full-brightness white so you (and the camera) can see it go down
    if (spotlightWanted())
    {
        spotlightOn = true;
        tweenToColor(0, 0, 0, 255, 255, 800);
        spotlightOn = false;
        printLogs("First layer - spotlight", 0, 0, 0, 255, 255);
        return;
    }

    // Printing or Resume after Pausing
    if (printerVariables.stage == 0 && printerVariables.gcodeState == "RUNNING")
    {
        COLOR base = printerConfig.runningColor; // Customisable - Default is WHITE
        COLOR fil;
        bool useFilament = printerConfig.filamentColorFx && filamentLedColor(fil);
        // Almost done: in the last minutes, blend toward the finish color as a heads-up
        if (almostDoneBlend() > 0.0f && !printerConfig.progressFx)
        {
            float k = almostDoneBlend();
            const COLOR &f = printerConfig.finishColor;
            base.r = (short)(base.r + (f.r - base.r) * k);
            base.g = (short)(base.g + (f.g - base.g) * k);
            base.b = (short)(base.b + (f.b - base.b) * k);
            base.ww = (short)(base.ww + (f.ww - base.ww) * k);
            base.cw = (short)(base.cw + (f.cw - base.cw) * k);
        }
        bool accent = useFilament && !(printerConfig.timelapseSteady && printerVariables.timelapse);
        if (printerConfig.progressFx && accent)
        {
            // Both on: the base color follows the progress glow, and the filament-color swells
            // still play on top of it once a minute
            float p = constrain(printerVariables.percent, 0, 100) / 100.0f;
            const COLOR &f = printerConfig.finishColor;
            base.r = (short)(base.r + (f.r - base.r) * p);
            base.g = (short)(base.g + (f.g - base.g) * p);
            base.b = (short)(base.b + (f.b - base.b) * p);
            base.ww = (short)(base.ww + (f.ww - base.ww) * p);
            base.cw = (short)(base.cw + (f.cw - base.cw) * p);
            ledSetProgressTarget(fil);
            showFx(FX_ACCENT, base);
        }
        else if (printerConfig.progressFx)
        {
            ledSetProgressTarget(printerConfig.finishColor);
            showFx(FX_PROGRESS, base);
        }
        else if (accent)
        {
            // Normal light, with a short filament-color swell once a minute. Skipped while a
            // timelapse records with "steady for timelapse" on, so frames don't change color.
            ledSetProgressTarget(fil);
            showFx(FX_ACCENT, base);
        }
        else
        {
            tweenToColor(base);
        }
        printLogs(useFilament ? "Stage 0, PRINTING - filament color" : "Stage 0, PRINTING - gcodeState RUNNING", base);
        return;
    }

    // for IDLE - P1 uses 255, X1 uses -1
    if ((printerVariables.stage == -1 || printerVariables.stage == 255) && !holdingFinish() && (millis() - printerConfig.inactivityStartms < printerConfig.inactivityTimeOut))
    {
        tweenToColor(printerConfig.runningColor); // Customisable - Default is WHITE
        printLogs("Stage -1/255, IDLE", printerConfig.runningColor);
        return;
    }

    // User Cancelled Print
    if (printerVariables.gcodeState == "FAILED")
    {
        tweenToColor(printerConfig.runningColor); // Customisable - Default is WHITE
        printLogs("Stage -1/255, FAILED", printerConfig.runningColor);
        return;
    }
    // Print file just sent
    if (printerVariables.gcodeState == "PREPARE")
    {
        tweenToColor(printerConfig.runningColor); // Customisable - Default is WHITE
        printLogs("Stage -1/255, PREPARE", printerConfig.runningColor);
        return;
    }

    // Homing ToolHead
    if (printerVariables.stage == 13)
    {
        // No color change assigned
        if (printerConfig.debugingchange)
            LogSerial.println(F("STAGE 13, HOMING TOOL HEAD"));
        return;
    }

    // Offline
    if (printerVariables.gcodeState == "OFFLINE" || printerVariables.stage == -2)
    {
        tweenToColor(printerConfig.runningColor); // Customisable - Default is WHITE
        printLogs("Stage -1/255, OFFLINE", printerConfig.runningColor);
        return;
    }

    // GREEN -- GREEN -- GREEN -- GREEN

    // Sets to green when print finishes AND user wants Finish Indication enabled
    if (printerVariables.finished == true && printerConfig.finishindication == true)
    {
        if (printerConfig.celebrateFx && printerVariables.celebratePending)
        {
            printerVariables.celebratePending = false;
            celebrateUntil = millis() + 3000;
        }
        showFx(printerConfig.finishFx, printerConfig.finishColor);
        printLogs("Finished print", printerConfig.finishColor);
        printerVariables.finished = false;
        return;
    }

    // replicate printer behaviour ON
    if (printerConfig.replicatestate && printerConfig.replicate_update && printerVariables.printerledstate && !holdingFinish())
    {
        tweenToColor(printerConfig.runningColor); // Customisable - Default is WHITE
        printLogs("LED Replication ON", printerConfig.runningColor);
        printerConfig.replicate_update = false;
        return;
    }

    // Ensure doorSwitchTriggered is processed immediately
    if (printerVariables.doorSwitchTriggered)
    {
        updateleds();
    }
}

void setupLeds()
{
    LogSerial.println(F("Updating from setupleds"));

    ledcSetup(redChannel, pwmFreq, pwmResolution);
    ledcSetup(greenChannel, pwmFreq, pwmResolution);
    ledcSetup(blueChannel, pwmFreq, pwmResolution);
    ledcSetup(warmChannel, pwmFreq, pwmResolution);
    ledcSetup(coldChannel, pwmFreq, pwmResolution);

    ledcAttachPin(redPin, redChannel);
    ledcAttachPin(greenPin, greenChannel);
    ledcAttachPin(bluePin, blueChannel);
    ledcAttachPin(warmPin, warmChannel);
    ledcAttachPin(coldPin, coldChannel);

    if (ledTaskHandle == NULL)
    {
#if CONFIG_FREERTOS_UNICORE
        xTaskCreate(ledTask, "ledTask", 3072, NULL, 2, &ledTaskHandle);
#else
        xTaskCreatePinnedToCore(ledTask, "ledTask", 3072, NULL, 2, &ledTaskHandle, 1);
#endif
    }
}

void ledsloop()
{
    // Work requested by the MQTT task or web server runs here, on the main task,
    // so the LED state logic is never run from two tasks at once.
    if (ledUpdateRequested)
    {
        ledUpdateRequested = false;
        updateleds();
    }
    if (ledOffRequested)
    {
        ledOffRequested = false;
        tweenToColor(0, 0, 0, 0, 0);
    }

    // Quiet hours: check the clock every few seconds and re-apply the current state when the
    // window starts or ends, with a slow fade.
    static unsigned long lastQuietCheck = 0;
    if (millis() - lastQuietCheck > 5000)
    {
        lastQuietCheck = millis();
        bool q = computeQuiet();
        if (q != quietNow)
        {
            quietNow = q;
            LogSerial.println(q ? F("[Quiet] Quiet hours started - dimming") : F("[Quiet] Quiet hours ended"));
            printerConfig.replicate_update = true;
            printerConfig.maintMode_update = true;
            printerConfig.discoMode_update = true;
            printerConfig.testcolor_update = true;
            int saved = printerConfig.fadeMs;
            printerConfig.fadeMs = 3000;
            updateleds();
            printerConfig.fadeMs = saved;
        }
    }

    RGBCycle();
    if ((millis() - lastUpdatems) > 30000 && (printerConfig.maintMode || printerConfig.testcolorEnabled || printerConfig.discoMode || printerConfig.debugwifi))
    {
        LogSerial.printf("[%lu]", millis());
        if (printerConfig.maintMode)
            LogSerial.println(F(" Maintenance Mode - next update in 30 seconds"));
        if (printerConfig.testcolorEnabled)
            LogSerial.println(F(" Test Color - next update in 30 seconds"));
        if (printerConfig.discoMode)
            LogSerial.println(F(" RGB Cycle Mode - next update in 30 seconds"));
        if (printerConfig.debugwifi)
        {
            LogSerial.println(F(" Wifi Debug Mode - next update in 30 seconds"));
            updateleds(); // refresh the signal-strength color
        }
        lastUpdatems = millis();
    }

    // Turn off GREEN if... finished and Door OPENED or CLOSED in last 5 secs AND user wants Finish Indication enabled
    // Can't use Boolean finished as printer defaults back to IDLE via MQTT
    if ((printerVariables.waitingForDoor && !printerVariables.coolWaiting && printerConfig.finishindication && printerConfig.finishExit && ((millis() - printerVariables.lastdoorClosems) < 6000 || (millis() - printerVariables.lastdoorOpenms) < 6000)))
    {
        if (printerConfig.debuging || printerConfig.debugingchange)
        {
            LogSerial.println(F("Updating from finishloop after Door interaction - Starting IDLE timer"));
        }
        printerVariables.waitingForDoor = false;
        printerConfig.inactivityStartms = millis();
        printerConfig.isIdleOFFActive = false;
        updateleds();
    }

    if ((printerConfig.finish_check && !printerVariables.coolWaiting && printerConfig.finishindication && printerConfig.finishExit == false && ((millis() - printerConfig.finishStartms) > printerConfig.finishTimeOut)))
    {
        if (printerConfig.debuging || printerConfig.debugingchange)
        {
            LogSerial.println(F("Updating from finishloop after Finish timer expired - Starting IDLE timer"));
        }
        printerConfig.finish_check = false;
        printerConfig.inactivityStartms = millis();
        printerConfig.isIdleOFFActive = false;
        updateleds();
        controlChamberLight(false); // Turn off chamber light via MQTT
    }

    // Safe-to-remove: once the bed has cooled (or after an hour, in case readings stop), let go of
    // the finish color - the strip changes to the normal color, meaning the part can come off.
    if (printerVariables.coolWaiting)
    {
        bool cool = printerVariables.bedTemp > 0 && printerVariables.bedTemp < printerConfig.coolDownTemp;
        bool tooLong = millis() - printerVariables.coolStartMs > 3600000UL;
        if (cool || tooLong || !printerConfig.coolDownEnabled)
        {
            printerVariables.coolWaiting = false;
            printerVariables.waitingForDoor = false;
            printerConfig.finish_check = false;
            printerVariables.finished = false;
            printerConfig.inactivityStartms = millis();
            printerConfig.isIdleOFFActive = false;
            printerConfig.replicate_update = true;
            LogSerial.printf("[LED] Bed at %.0f C - safe to remove the part\n", printerVariables.bedTemp);
            updateleds();
        }
    }

    // Heat-up glow follows the temperatures, which change without any state change
    // (also catches the moment heating finishes, so the strip returns to normal)
    static unsigned long lastHeatRefresh = 0;
    static bool wasHeating = false;
    bool heatingNow = heatGlowActive();
    if ((heatingNow || wasHeating) && millis() - lastHeatRefresh > 2000)
    {
        lastHeatRefresh = millis();
        if (!heatingNow)
        {
            printerConfig.replicate_update = true; // let the normal state re-apply
            printerConfig.isIdleOFFActive = false;
            printerConfig.inactivityStartms = millis();
        }
        wasHeating = heatingNow;
        updateleds();
    }

    // Settings-page preview: apply when started, restore the real state when it ends
    static bool previewWasOn = false;
    if (previewStarted)
    {
        previewStarted = false;
        previewWasOn = true;
        updateleds();
    }
    else if (previewWasOn && !previewActive())
    {
        previewWasOn = false;
        previewUntil = 0;
        printerConfig.replicate_update = true;
        printerConfig.maintMode_update = true;
        printerConfig.discoMode_update = true;
        printerConfig.testcolor_update = true;
        printerVariables.finished = holdingFinish() && printerConfig.finishindication;
        updateleds();
    }

    // Almost done: the remaining time changes without a state change, so re-check now and then
    static unsigned long lastAlmostCheck = 0;
    if (printerConfig.almostDoneEnabled && millis() - lastAlmostCheck > 10000)
    {
        lastAlmostCheck = millis();
        if (almostDoneBlend() > 0.0f)
            updateleds();
    }

    // Progress glow + filament color together: the base color is worked out from the percentage,
    // so re-apply it as the print advances
    static int lastPct = -1;
    if (printerConfig.progressFx && printerConfig.filamentColorFx && printerVariables.percent != lastPct)
    {
        lastPct = printerVariables.percent;
        if (printerVariables.gcodeState == "RUNNING" && printerVariables.stage == 0)
            updateleds();
    }

    // Filament preview ends after 30 s
    static bool filPreviewWasOn = false;
    bool filPreviewOn = (int32_t)(printerVariables.filamentPreviewUntil - millis()) > 0;
    if (filPreviewWasOn && !filPreviewOn)
    {
        printerConfig.replicate_update = true;
        updateleds();
    }
    filPreviewWasOn = filPreviewOn;

    // Need an trigger action to run updateleds() so lights turn off
    // There is no change in the printer STATE, just monitoring the timer and triggering when over a threshhold
    if (printerConfig.inactivityEnabled && (millis() - printerConfig.inactivityStartms) > printerConfig.inactivityTimeOut && (printerVariables.finished == false || !printerConfig.finishindication) && !printerVariables.coolWaiting && printerConfig.isIdleOFFActive == false && !idleDimActive)
    {
        // Opening or Closing the Door will turn LEDs back on and restart the timer.
        updateleds();
    }
    // Dimmed while idle: turn off once that time is up too
    if (idleDimActive && idleDimExpired())
    {
        idleDimActive = false;
        updateleds();
    }

    // First-layer spotlight starts and ends with the layer number, which isn't a state change
    static bool spotWas = false;
    bool spotNow = spotlightWanted();
    if (spotNow != spotWas)
    {
        spotWas = spotNow;
        updateleds();
    }

    // Printer light sync: outside prints, the printer's own light follows the strip turning on or
    // off (dimmed counts as off). Only on a change, so switching the light on the screen still works.
    // Never during a print - the printer handles its light itself (camera, lidar).
    static bool syncLit = false, syncSeeded = false;
    static unsigned long syncChangeMs = 0;
    bool litNow = !idleDimActive && (currentRed || currentGreen || currentBlue || currentWarm || currentCold);
    if (!syncSeeded)
    {
        syncSeeded = true;
        syncLit = litNow;
    }
    if (litNow != syncLit)
    {
        if (syncChangeMs == 0)
            syncChangeMs = millis();
        else if (millis() - syncChangeMs > 3000) // settled for 3 s
        {
            syncLit = litNow;
            syncChangeMs = 0;
            if (printerConfig.controlChamberLight && printerVariables.online && !printingState() &&
                !previewActive() && printerVariables.printerledstate != litNow)
            {
                LogSerial.printf("[LED] Printer light sync: %s\n", litNow ? "on" : "off");
                controlChamberLight(litNow);
            }
        }
    }
    else
        syncChangeMs = 0;

    // Offline check: the 30 s grace period ends without any MQTT event, so re-evaluate periodically
    static unsigned long lastOfflineCheck = 0;
    if (!printerVariables.online && millis() - lastOfflineCheck > 5000)
    {
        lastOfflineCheck = millis();
        updateleds();
    }

    // Auto turn off chamber light after timeout when door is closed
    if (printerVariables.chamberLightLocked &&
        !printerVariables.doorOpen &&
        (millis() - printerConfig.inactivityStartms > printerConfig.inactivityTimeOut))
    {
        controlChamberLight(false);
        printerVariables.chamberLightLocked = false;

        if (printerConfig.debugingchange)
            LogSerial.println(F("[LED] Timeout – Chamber light OFF and lock released"));
    }
    delay(10);
}

#endif
