#include <Wire.h>

// display_clean_init - the decisive test.
//
// The 2026-11-18 sweep (display_watch) showed every driver family being
// accepted at 0x3C but losing 2-5 bytes per sequence. A lost byte in the init
// sequence leaves the panel off, which explains "ACKs but black" without any
// need for a broken panel.
//
// So: retry the init until it completes with ZERO naks, then drive the panel
// with 0xA5 (hardware all-on, no GDDRAM) and hold it.
//
//   clean init + 0xA5 lights the panel  -> it was corrupted transfers, fix the
//                                          SDA line and everything works
//   clean init + 0xA5 stays black       -> the module's panel/driver is dead
//
// Also verifies that a clean init is even reachable at each clock, which tells
// us the highest safe clock for the animation.

#define OLED_ADDR 0x3C
#define SDA_PIN 5
#define SCL_PIN 6

static uint32_t okC = 0, nakC = 0;
static uint8_t fb[1024];

static void busRecover() {
  Wire.end();
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  delay(2);
  bool stuck = !digitalRead(SDA_PIN) || !digitalRead(SCL_PIN);
  if (stuck) {
    pinMode(SCL_PIN, OUTPUT);
    for (int i = 0; i < 9; i++) {
      digitalWrite(SCL_PIN, LOW);  delayMicroseconds(5);
      digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    }
    pinMode(SDA_PIN, OUTPUT);
    digitalWrite(SDA_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    digitalWrite(SDA_PIN, HIGH); delayMicroseconds(5);
    pinMode(SDA_PIN, INPUT_PULLUP);
  }
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(3);
}

static void w1(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  if (Wire.endTransmission()) nakC++; else okC++;
}
static void w2(uint8_t c, uint8_t a) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  Wire.write(a);
  if (Wire.endTransmission()) nakC++; else okC++;
}

static void initSSD1306() {
  w1(0xAE);
  w2(0xD5, 0x80);
  w2(0xA8, 0x3F);
  w2(0xD3, 0x00);
  w1(0x40);
  w2(0x8D, 0x14);     // charge pump ON
  w2(0x20, 0x00);
  w1(0xA1); w1(0xC8);
  w2(0xDA, 0x12);
  w2(0x81, 0xFF);
  w2(0xD9, 0xF1);
  w2(0xDB, 0x40);
  w1(0xA4); w1(0xA6); w1(0x2E);
  w1(0xAF);
}

static void initSH1106() {
  w1(0xAE);
  w2(0xD5, 0x80);
  w2(0xA8, 0x3F);
  w2(0xD3, 0x00);
  w1(0x40);
  w2(0xAD, 0x8B);     // DC-DC ON
  w1(0x33);
  w1(0xA1); w1(0xC8);
  w2(0xDA, 0x12);
  w2(0x81, 0xFF);
  w2(0xD9, 0x22);
  w2(0xDB, 0x35);
  w1(0xA4); w1(0xA6);
  w1(0xAF);
}

static void pushFrame(uint8_t fill) {
  w2(0x21, 0x00); w2(0x21, 127);
  w2(0x22, 0x00); w2(0x22, 0x07);
  for (uint8_t p = 0; p < 8; p++)
    for (uint8_t off = 0; off < 128; off += 16) {
      for (uint8_t k = 0; k < 16; k++) fb[p * 128 + off + k] = fill;
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(&fb[p * 128 + off], 16);
      if (Wire.endTransmission()) nakC++; else okC++;
    }
}

// retry the init until it lands with zero naks; returns tries used or 0 on fail
static int cleanInit(void (*fn)(), uint32_t hz, const char *label) {
  for (int t = 1; t <= 40; t++) {
    busRecover();
    Wire.setClock(hz);
    delay(3);
    okC = nakC = 0;
    fn();
    if (nakC == 0) {
      Serial.printf("  CLEAN %s @%luk after %d tries\n", label,
                    (unsigned long)(hz / 1000), t);
      return t;
    }
    delay(60);
  }
  Serial.printf("  could not get a clean %s @%luk in 40 tries\n", label,
                (unsigned long)(hz / 1000));
  return 0;
}

static void flashAndHold(const char *label) {
  Serial.printf("  %s: 0xA5 hardware all-on - PANEL SHOULD BE SOLID WHITE (10 s)\n", label);
  w1(0xA5);
  okC = nakC = 0;
  delay(10000);
  w1(0xA4); w1(0xAF);
  okC = nakC = 0;
  pushFrame(0xFF);
  Serial.printf("  %s: 0xFF frame -> %lu ok, %lu nak (5 s)\n", label,
                (unsigned long)okC, (unsigned long)nakC);
  delay(5000);
  w1(0xAE);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== display_clean_init: retry init until zero NAKs ==="));

  if (cleanInit(initSSD1306, 100000, "SSD1306")) {
    flashAndHold("SSD1306");
  }
  if (cleanInit(initSH1106, 100000, "SH1106")) {
    flashAndHold("SH1106");
  }
  Serial.println(F("=== test complete. repeating in 5 s ==="));
}

void loop() {
  delay(5000);
  Serial.println(F("--- repeat ---"));
  if (cleanInit(initSSD1306, 100000, "SSD1306")) flashAndHold("SSD1306");
}
