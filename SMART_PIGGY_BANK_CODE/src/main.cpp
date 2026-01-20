#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "LDC1614.h"

static constexpr int I2C_SDA = 21;
static constexpr int I2C_SCL = 22;

static constexpr uint8_t OLED_ADDR = 0x3C;
static constexpr int SCREEN_W = 128;
static constexpr int SCREEN_H = 64;
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

LDC1614 ldc(0x2A, Wire);

static const uint8_t SENSOR_PINS[8] = {34, 35, 14, 27, 26, 25, 33, 32};

static const uint16_t centsForCh[8] = {1, 2, 5, 10, 20, 50, 100, 200};

static constexpr bool OPT_ACTIVE_LOW = true;

Preferences prefs;

uint32_t totalCount = 0;
uint32_t totalCents = 0;
uint32_t countCh[8] = {0};

static constexpr uint32_t THRESH_HIGH = 10000000;
static constexpr uint32_t THRESH_LOW  = 5000000;
static constexpr uint32_t BASELINE_DEADBAND = 500000;

static constexpr uint32_t LDC_POLL_MS = 5;
static constexpr uint32_t EVENT_COOLDOWN_MS = 150;
static constexpr uint32_t TOKEN_WINDOW_MS = 3500;
static constexpr uint32_t OVERLAY_MS = 700;

static constexpr uint32_t MIN_OPT_DELAY_MS = 200;

static constexpr uint32_t RESET_HOLD_MS = 5000;
bool resetArmed = false;
uint32_t resetHoldStartMs = 0;
bool resetDoneThisHold = false;

uint32_t baseline = 0;
bool baselineReady = false;
bool inEvent = false;
uint32_t lastEventMs = 0;

uint8_t tokenCount = 0;
uint32_t tokenExpiryMs = 0;
uint32_t lastTokenIssuedMs = 0;

bool lastAuthTrue = false;
uint32_t overlayUntilMs = 0;

uint32_t lastPollMs = 0;
uint32_t ldcRaw = 0;
bool ldcOk = false;

uint32_t lastTrigMs[8] = {0};
bool lastActive[8] = {0};
static constexpr uint32_t OPT_LOCKOUT_MS = 120;

bool dirty = false;
uint32_t lastChangeMs = 0;
uint32_t lastWriteMs = 0;
static constexpr uint32_t SAVE_DELAY_MS = 800;
static constexpr uint32_t MIN_WRITE_MS  = 3000;


static bool optActive(uint8_t pin) {
  int v = digitalRead(pin);
  if (OPT_ACTIVE_LOW) return (v == LOW);
  return (v == HIGH);
}

static void loadNVS() {
  prefs.begin("coins", false);
  totalCount = prefs.getUInt("totalCount", 0);
  totalCents = prefs.getUInt("totalCents", 0);

  for (int i = 0; i < 8; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ch%d", i);
    countCh[i] = prefs.getUInt(key, 0);
  }
}

static void requestSave() {
  dirty = true;
  lastChangeMs = millis();
}

static void saveIfDue() {
  if (!dirty) return;

  uint32_t now = millis();
  if (now - lastChangeMs < SAVE_DELAY_MS) return;
  if (now - lastWriteMs  < MIN_WRITE_MS)  return;

  prefs.putUInt("totalCount", totalCount);
  prefs.putUInt("totalCents", totalCents);
  for (int i = 0; i < 8; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ch%d", i);
    prefs.putUInt(key, countCh[i]);
  }

  lastWriteMs = now;
  dirty = false;
}

static void drawOverlayText(const char* msg) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 18);
  display.print(msg);
  display.display();
}

static void doFactoryResetCounters() {
  totalCount = 0;
  totalCents = 0;
  for (int i = 0; i < 8; i++) countCh[i] = 0;

  tokenCount = 0;
  tokenExpiryMs = 0;

  prefs.putUInt("totalCount", 0);
  prefs.putUInt("totalCents", 0);
  for (int i = 0; i < 8; i++) {
    char key[8];
    snprintf(key, sizeof(key), "ch%d", i);
    prefs.putUInt(key, 0);
  }

  dirty = false;
  lastWriteMs = millis();

  overlayUntilMs = millis() + 1200;
  drawOverlayText("RESET");
}

static void drawMain() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  display.print("Coins: "); display.println(totalCount);

  display.print("Sum: ");
  uint32_t eur = totalCents / 100;
  uint32_t c   = totalCents % 100;
  display.print(eur);
  display.print(".");
  if (c < 10) display.print("0");
  display.print(c);
  display.println(" EUR");

  display.display();
}

static void drawOverlayAuth(bool ok) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 18);
  if (ok) display.print("ISTINSKA");
  else display.print("NEISTIN");

  display.display();
}

static void pollLdc() {
  uint32_t now = millis();
  if (now - lastPollMs < LDC_POLL_MS) return;
  lastPollMs = now;

  uint32_t v;
  if (!ldc.readChannel0(v)) { ldcOk = false; return; }
  ldcOk = true;
  ldcRaw = v;

  if (!baselineReady) { baseline = v; baselineReady = true; return; }

  uint32_t dev = (v > baseline) ? (v - baseline) : (baseline - v);

  if (!inEvent && dev < BASELINE_DEADBAND) {
    baseline = (baseline * 31 + v) / 32;
  }

  bool metalNow = (dev > THRESH_HIGH);

  if (metalNow) {
    if (!resetArmed) {
      resetArmed = true;
      resetDoneThisHold = false;
      resetHoldStartMs = now;
    } else if (!resetDoneThisHold && (now - resetHoldStartMs >= RESET_HOLD_MS)) {
      resetDoneThisHold = true;
      doFactoryResetCounters();
      tokenCount = 0;
      tokenExpiryMs = 0;
    }
  } else {
    resetArmed = false;
    resetDoneThisHold = false;
  }

  if (now - lastEventMs < EVENT_COOLDOWN_MS) return;

  if (!inEvent) {
    if (dev > THRESH_HIGH) {
      inEvent = true;
      lastEventMs = now;

      lastAuthTrue = true;
      overlayUntilMs = now + OVERLAY_MS;

      if (!resetDoneThisHold) {
        if (tokenCount < 10) tokenCount++;
        tokenExpiryMs = now + TOKEN_WINDOW_MS;
        lastTokenIssuedMs = now;
      }
    }
  } else {
    if (dev < THRESH_LOW) inEvent = false;
  }
}

static bool pollOpticalAndCount() {
  bool changed = false;
  uint32_t now = millis();

  if (tokenCount > 0 && now > tokenExpiryMs) {
    tokenCount = 0;
    changed = true;
  }

  for (int ch = 0; ch < 8; ch++) {
    bool cur = optActive(SENSOR_PINS[ch]);
    bool rising = cur && !lastActive[ch];
    lastActive[ch] = cur;

    if (!rising) continue;

    if (now - lastTrigMs[ch] < OPT_LOCKOUT_MS) continue;
    lastTrigMs[ch] = now;

    bool timeOk = (now - lastTokenIssuedMs) > MIN_OPT_DELAY_MS;
    bool canCount = (tokenCount > 0) && (now < tokenExpiryMs) && timeOk;

    if (canCount) {
      tokenCount--;

      countCh[ch]++;
      totalCount++;
      totalCents += centsForCh[ch];

      requestSave();
      changed = true;
    } else {
      lastAuthTrue = false;
      overlayUntilMs = now + OVERLAY_MS;
      changed = true;
    }
  }
  return changed;
}


void setup() {
  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  loadNVS();

  for (int i = 0; i < 8; i++) {
    pinMode(SENSOR_PINS[i], INPUT);
    lastTrigMs[i] = 0;
    lastActive[i] = optActive(SENSOR_PINS[i]);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED FAIL");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Boot...");
  display.display();

  bool ok = ldc.begin();
  Serial.println(ok ? "LDC OK" : "LDC FAIL");

  drawMain();
}

void loop() {
  pollLdc();
  bool changed = pollOpticalAndCount();

  Serial.printf("raw=%u baseline=%u dev=%u\n", ldcRaw, baseline,
                (ldcRaw > baseline ? ldcRaw - baseline : baseline - ldcRaw));

  if (millis() < overlayUntilMs) {
    drawOverlayAuth(lastAuthTrue);
  } else {
    static uint32_t lastDrawMs = 0;
    if (changed || millis() - lastDrawMs > 500) {
      lastDrawMs = millis();
      drawMain();
    }
  }

  saveIfDue();
}
