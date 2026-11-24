#include <Wire.h>

// Display test on the FREE pads: J5 pin3 = D13/GPIO40, J5 pin2 = D14/GPIO41.
//
// Why: this pair has nothing else on it (no codec 0x18, no XMOS 0x2C, no board
// pull-ups - the module supplies its own). It isolates the panel from the
// shared control bus, which is the last software-side variable.
//
// Wiring:
//   module VCC -> TP15 (+3V3)      <-- J5 has NO 3.3 V pin
//   module GND -> J5 pin1 (GND)
//   module SDA -> J5 pin3 (D13/GPIO40)
//   module SCL -> J5 pin2 (D14/GPIO41)
//
// The sketch tries both pin orders and keeps whichever one ACKs, so a swapped
// SDA/SCL cannot produce a false "dead panel" result.

#define PIN_A_SDA 40   // D13
#define PIN_A_SCL 41   // D14
#define PIN_B_SDA 41
#define PIN_B_SCL 40

#define OLED_ADDR 0x3C

static uint8_t fb[1024];
static int sdaPin = PIN_A_SDA, sclPin = PIN_A_SCL;
static bool found = false;

uint8_t c1(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(c);
  return Wire.endTransmission();
}
void c2(uint8_t c, uint8_t a) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(c); Wire.write(a);
  Wire.endTransmission();
}

bool tryOrder(int sda, int scl, const char *label) {
  Wire.end();
  delay(5);
  Wire.begin(sda, scl);
  Wire.setClock(400000);
  delay(10);
  uint8_t e = c1(0xAE);           // real command write as the probe
  Serial.printf("try %s (SDA=GPIO%d SCL=GPIO%d): %s\n", label, sda, scl,
                e == 0 ? "ACK - using this order" : "no ACK");
  return e == 0;
}

void initPanel() {
  c1(0xAE);
  c2(0xD5, 0x80); c2(0xA8, 0x3F); c2(0xD3, 0x00); c1(0x40);
  c2(0x8D, 0x14); c2(0x20, 0x00); c1(0xA1); c1(0xC8);
  c2(0xDA, 0x12); c2(0x81, 0xCF); c2(0xD9, 0xF1); c2(0xDB, 0x40);
  c1(0xA4); c1(0xA6); c1(0x2E); c1(0xAF);
  delay(50);
}

void push() {
  c2(0x21, 0x00); c2(0x21, 127);
  c2(0x22, 0x00); c2(0x22, 7);
  for (uint8_t p = 0; p < 8; p++) {
    for (uint8_t off = 0; off < 128; off += 16) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write(0x40);
      Wire.write(&fb[p * 128 + off], 16);
      if (Wire.endTransmission()) { /* counted below */ }
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== free-pad display test: J5 D13/D14, VCC on TP15 ==="));

  if (tryOrder(PIN_A_SDA, PIN_A_SCL, "D13=SDA D14=SCL")) {
    sdaPin = PIN_A_SDA; sclPin = PIN_A_SCL; found = true;
  } else if (tryOrder(PIN_B_SDA, PIN_B_SCL, "D14=SDA D13=SCL")) {
    sdaPin = PIN_B_SDA; sclPin = PIN_B_SCL; found = true;
  } else {
    Serial.println(F("no ACK in either order - panel not responding on D13/D14"));
  }

  if (found) {
    initPanel();
    Serial.println(F("panel initialised; showing all-pixels-on <-> pattern every 2 s"));
  }
}

void loop() {
  static int n = 0;
  if (!found) { delay(5000); return; }

  memset(fb, (n & 1) ? 0x00 : 0xFF, sizeof(fb));
  if (n & 1) {   // pattern: border + X
    for (int x = 0; x < 128; x++) { fb[x] |= 0x01; fb[7 * 128 + x] |= 0x80; }
    for (int y = 0; y < 64; y++) { fb[(y >> 3) * 128] |= 1 << (y & 7); fb[(y >> 3) * 128 + 127] |= 1 << (y & 7); }
    for (int i = 0; i < 64; i++) {
      fb[(i >> 3) * 128 + i] |= 1 << (i & 7);
      fb[(i >> 3) * 128 + (127 - i)] |= 1 << (i & 7);
    }
  }
  push();
  Serial.printf("%s (SDA=GPIO%d SCL=GPIO%d, %lu s)\n",
                (n & 1) ? "pattern " : "all-on  ", sdaPin, sclPin,
                (unsigned long)(millis() / 1000));
  n++;
  delay(2000);
}
