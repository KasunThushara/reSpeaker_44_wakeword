#include <Wire.h>

// display_watch - fair test of every controller variant, with bus recovery.
//
// 2026-11-18 findings that shaped this sketch:
//   * panel ACKs at 0x3C, accepts 0xA5 (hardware all-on), stays black
//   * SDA (D4/GPIO5) rise time varies 1.5-2.5 us vs 0.42 us on SCL -> bad contact
//   * after a burst of NAKs the ESP32 I2C peripheral STAYS dead until Wire is
//     re-initialised (the previous sweep measured 3 variants on a dead bus)
//
// So: 9-clock recovery + Wire.end()/begin() before every variant, 100 kHz only.
//
// Controller families, and the one register that matters:
//   SSD1306/SSD1309 : charge pump 0x8D 0x14  (SSD1309 also needs 0xFD 0x12)
//   SH1106          : DC-DC      0xAD 0x8B  (0x8D does nothing on an SH1106)
// An SH1106 driven with an SSD1306 init stays black forever while ACKing.

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
  Wire.setClock(100000);
  delay(3);
  Serial.printf("    [bus recovery: stuck=%d]\n", stuck);
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

static bool alive() {
  Wire.beginTransmission(OLED_ADDR);
  return Wire.endTransmission() == 0;
}

static void initSSD1306(bool ssd1309) {
  w1(0xAE);
  if (ssd1309) { w1(0xFD); w1(0x12); }
  w2(0xD5, 0x80);
  w2(0xA8, 0x3F);
  w2(0xD3, 0x00);
  w1(0x40);
  w2(0x8D, 0x14);          // charge pump ON
  w2(0x20, 0x00);
  w1(0xA1); w1(0xC8);
  w2(0xDA, 0x12);
  w2(0x81, 0xFF);
  w2(0xD9, 0xF1);
  w2(0xDB, 0x40);
  w1(0xA4); w1(0xA6); w1(0x2E);
  delay(20);
  w1(0xAF);
  delay(20);
}

static void initSH1106() {
  w1(0xAE);
  w2(0xD5, 0x80);
  w2(0xA8, 0x3F);
  w2(0xD3, 0x00);
  w1(0x40);
  w2(0xAD, 0x8B);          // DC-DC ON  (SH1106)
  w1(0x33);
  w1(0xA1); w1(0xC8);
  w2(0xDA, 0x12);
  w2(0x81, 0xFF);
  w2(0xD9, 0x22);
  w2(0xDB, 0x35);
  w1(0xA4); w1(0xA6);
  w1(0xAF);
  delay(20);
}

static void pushSSD(uint8_t fill) {
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

static void pushSH(uint8_t fill, uint8_t colOfs) {
  for (uint8_t p = 0; p < 8; p++) {
    w1(0xB0 | p);
    w1(0x00 | (colOfs & 0x0F));
    w1(0x10 | (colOfs >> 4));
    for (uint8_t off = 0; off < 128; off += 16) {
      for (uint8_t k = 0; k < 16; k++) fb[p * 128 + off + k] = fill;
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(&fb[p * 128 + off], 16);
      if (Wire.endTransmission()) nakC++; else okC++;
    }
  }
}

static void report(const char *what) {
  Serial.printf("    %-28s %3lu ok, %3lu nak\n", what,
                (unsigned long)okC, (unsigned long)nakC);
}

static void runVariant(int which) {
  Serial.println();
  switch (which) {
    case 0:
      Serial.println(F(">>> VARIANT 1: SSD1306 init + charge pump 0x8D 0x14"));
      busRecover();
      okC = nakC = 0;
      initSSD1306(false);
      report("init");
      w1(0xA5); report("0xA5 hardware all-on (8 s)");
      delay(8000);
      w1(0xA4); w1(0xAF);
      okC = nakC = 0; pushSSD(0xFF); report("0xFF frame (6 s)");
      delay(6000);
      w1(0xAE);
      break;
    case 1:
      Serial.println(F(">>> VARIANT 2: SSD1309 (0xFD 0x12 unlock) + 0x8D 0x14"));
      busRecover();
      okC = nakC = 0;
      initSSD1306(true);
      report("init");
      w1(0xA5); report("0xA5 hardware all-on (8 s)");
      delay(8000);
      w1(0xA4); w1(0xAF);
      okC = nakC = 0; pushSSD(0xFF); report("0xFF frame (6 s)");
      delay(6000);
      w1(0xAE);
      break;
    case 2:
      Serial.println(F(">>> VARIANT 3: SH1106 init, DC-DC 0xAD 0x8B, column offset 2"));
      busRecover();
      okC = nakC = 0;
      initSH1106();
      report("init");
      w1(0xA5); report("0xA5 hardware all-on (8 s)");
      delay(8000);
      w1(0xA4); w1(0xAF);
      okC = nakC = 0; pushSH(0xFF, 0x02); report("0xFF pages (6 s)");
      delay(6000);
      if (alive()) { okC = nakC = 0; pushSH(0xAA, 0x02); report("0xAA pages (5 s)"); delay(5000); }
      w1(0xAE);
      break;
    case 3:
      Serial.println(F(">>> VARIANT 4: SH1106 init, DC-DC 0xAD 0x8B, column offset 0"));
      busRecover();
      okC = nakC = 0;
      initSH1106();
      report("init");
      w1(0xA5); report("0xA5 hardware all-on (8 s)");
      delay(8000);
      w1(0xA4); w1(0xAF);
      okC = nakC = 0; pushSH(0xFF, 0x00); report("0xFF pages (6 s)");
      delay(6000);
      w1(0xAE);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== display_watch: 4 driver variants, bus recovered between each ==="));
  Serial.println(F("Each variant ~20 s. WATCH THE PANEL."));
}

void loop() {
  for (int i = 0; i < 4; i++) runVariant(i);
  Serial.println(F("\n=== sweep done; repeating in 3 s ==="));
  delay(3000);
}
