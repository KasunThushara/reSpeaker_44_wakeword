#include <Wire.h>

// display_drivers - try every plausible controller variant, in one run.
//
// Why: the panel ACKs at 0x3C, accepts a full SSD1306 init and even 0xA5
// (hardware all-on) with 0 NAKs, and stays completely black. The most common
// cause of that exact symptom is a controller whose charge pump was never
// switched on, because it is not an SSD1306:
//
//   SSD1306 / SSD1309 : charge pump = 0x8D 0x14   (SSD1309 needs 0xFD 0x12 first)
//   SH1106            : DC-DC       = 0xAD 0x8B   (0x8D means nothing to it!)
//
// An SH1106 fed an SSD1306 init stays black forever while answering I2C
// happily. Also tried: 132-column offset (SH1106 RAM is 132 wide) and 128x32
// COM pin config.
//
// Runs at 100 kHz on purpose: the 2026-11-18 rise-time test showed SDA
// (D4/GPIO5) climbing in 1.5-2.5 us vs 0.42 us on SCL, so fast mode is not
// trustworthy until that contact is fixed. 100 kHz writes were 70/70 clean.

#define OLED_ADDR 0x3C
#define SDA_PIN 5
#define SCL_PIN 6

static uint32_t okC = 0, nakC = 0;
static uint8_t fb[1024];

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

// ---- SSD1306 / SSD1309 (charge pump 0x8D) --------------------------------
static void initSSD1306(bool ssd1309) {
  w1(0xAE);
  if (ssd1309) { w1(0xFD); w1(0x12); }   // unlock (SSD1309 only)
  w2(0xD5, 0x80);
  w2(0xA8, 0x3F);
  w2(0xD3, 0x00);
  w1(0x40);
  w2(0x8D, 0x14);          // charge pump ON  <-- SSD1306 family
  w2(0x20, 0x00);          // horizontal addressing
  w1(0xA1);
  w1(0xC8);
  w2(0xDA, 0x12);
  w2(0x81, 0xFF);
  w2(0xD9, 0xF1);
  w2(0xDB, 0x40);
  w1(0xA4);
  w1(0xA6);
  w1(0x2E);
  delay(20);
  w1(0xAF);
  delay(20);
}

// ---- SH1106 (DC-DC 0xAD 0x8B, page addressing only, 132-column RAM) ------
static void initSH1106(uint8_t colOfs) {
  w1(0xAE);
  w2(0xD5, 0x80);
  w2(0xA8, 0x3F);
  w2(0xD3, 0x00);
  w1(0x40);
  w2(0xAD, 0x8B);          // DC-DC ON       <-- SH1106
  w1(0x33);                // pump voltage 9 V
  w1(0xA1);
  w1(0xC8);
  w2(0xDA, 0x12);
  w2(0x81, 0xFF);
  w2(0xD9, 0x22);
  w2(0xDB, 0x35);
  w1(0xA4);
  w1(0xA6);
  w1(0xAF);
  (void)colOfs;            // applied per page when pushing frames
  delay(20);
}

// page-addressed push (SH1106 has no 0x21/0x22 address window)
static void pushPagesSH1106(uint8_t fill, uint8_t colOfs) {
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

static void pushFrameSSD(uint8_t fill) {
  w2(0x21, 0x00); w2(0x21, 127);
  w2(0x22, 0x00); w2(0x22, 0x07);
  for (uint8_t p = 0; p < 8; p++) {
    for (uint8_t off = 0; off < 128; off += 16) {
      for (uint8_t k = 0; k < 16; k++) fb[p * 128 + off + k] = fill;
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(&fb[p * 128 + off], 16);
      if (Wire.endTransmission()) nakC++; else okC++;
    }
  }
}

static void banner(const char *name) {
  okC = nakC = 0;
  Serial.printf("\n>>> VARIANT: %s  (WATCH THE PANEL NOW)\n", name);
}

static void report() {
  Serial.printf("    %lu ok, %lu nak\n", (unsigned long)okC, (unsigned long)nakC);
}

// each variant: hardware all-on (0xA5), then a real frame, then off
static void variantSSD1306(bool ssd1309) {
  banner(ssd1309 ? "SSD1309 (0xFD 0x12 + 0x8D 0x14)" : "SSD1306 (0x8D 0x14)");
  initSSD1306(ssd1309);
  w1(0xA5);
  report();
  Serial.println(F("    A: 0xA5 hardware all-on - hold 8 s"));
  delay(8000);
  w1(0xA4); w1(0xAF);
  okC = nakC = 0;
  pushFrameSSD(0xFF);
  Serial.print(F("    B: 0xFF frame via GDDRAM"));
  report();
  delay(6000);
  okC = nakC = 0;
  pushFrameSSD(0xAA);
  Serial.print(F("    C: 0xAA checker frame"));
  report();
  delay(5000);
  w1(0xAE);
}

static void variantSH1106(uint8_t colOfs) {
  char name[64];
  snprintf(name, sizeof(name), "SH1106 (0xAD 0x8B), column offset %u", colOfs);
  banner(name);
  initSH1106(colOfs);
  w1(0xA5);
  report();
  Serial.println(F("    A: 0xA5 hardware all-on - hold 8 s"));
  delay(8000);
  w1(0xA4); w1(0xAF);
  okC = nakC = 0;
  pushPagesSH1106(0xFF, colOfs);
  Serial.print(F("    B: 0xFF frame via pages"));
  report();
  delay(6000);
  okC = nakC = 0;
  pushPagesSH1106(0xAA, colOfs);
  Serial.print(F("    C: 0xAA checker frame"));
  report();
  delay(5000);
  w1(0xAE);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);   // SDA rise time needs 100 kHz until fixed
  delay(30);

  Wire.beginTransmission(OLED_ADDR);
  Serial.printf("\n=== display_drivers: panel present? %s ===\n",
                Wire.endTransmission() == 0 ? "ACK at 0x3C" : "NAK - check wiring");
  Serial.println(F("Each variant runs ~20 s: hardware all-on, then two frames."));
  Serial.println(F("If ANY variant lights the panel, note which one."));
}

void loop() {
  variantSSD1306(false);
  variantSSD1306(true);
  variantSH1106(0x02);     // classic 0.96" SH1106 offset
  variantSH1106(0x00);
  Serial.println(F("\n=== cycle complete; repeating in 3 s ==="));
  delay(3000);
}
