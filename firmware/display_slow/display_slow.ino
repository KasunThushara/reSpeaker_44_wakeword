#include <Wire.h>

// display_slow - separate "corrupt data on a marginal SDA" from "dead panel".
//
// Facts (2026-11-18):
//   * SDA (D4/GPIO5) rise 1.5-2.5 us and variable; SCL (D5/GPIO6) 0.42 us steady
//   * a NAK-free init is reachable, yet 0xA5 (hardware all-on) never lights it
//   * NAK-free only means the ACK bits were sampled as ACK - the slave can still
//     misread data bits when SDA rises too slowly for the clock used
//
// So run the SAME init + all-on + frame at 20 kHz, 50 kHz and 100 kHz.
//   lights at 20 kHz but not 100 kHz -> data corruption on SDA, panel is ALIVE
//   black at every clock          -> the module's glass/driver is dead
//
// 20 kHz is deliberately chosen: a 25 us half-period is far longer than the
// 2.5 us rise, so every bit has settled long before the slave samples it.

#define OLED_ADDR 0x3C
#define SDA_PIN 5
#define SCL_PIN 6

static const uint32_t CLOCKS[3] = {20000, 50000, 100000};
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
  delay(5);
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

static void runAt(uint32_t hz) {
  Serial.printf("\n--- %lu kHz ---\n", (unsigned long)(hz / 1000));
  busRecover();
  Wire.setClock(hz);
  delay(10);

  okC = nakC = 0;
  initSSD1306();
  Serial.printf("  init: %lu ok, %lu nak\n", (unsigned long)okC, (unsigned long)nakC);

  w1(0xA5);
  Serial.println(F("  >>> 0xA5 hardware all-on - panel solid white for 12 s if alive"));
  delay(12000);

  w1(0xA4); w1(0xAF);
  okC = nakC = 0;
  pushFrame(0xFF);
  Serial.printf("  0xFF frame: %lu ok, %lu nak - hold 6 s\n",
                (unsigned long)okC, (unsigned long)nakC);
  delay(6000);

  w1(0xAE);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== display_slow: same init at 20 / 50 / 100 kHz ==="));
  for (int i = 0; i < 3; i++) runAt(CLOCKS[i]);
  Serial.println(F("\n=== clock sweep complete; repeating in 5 s ==="));
}

void loop() {
  delay(5000);
  Serial.println(F("--- repeat ---"));
  for (int i = 0; i < 3; i++) runAt(CLOCKS[i]);
}
