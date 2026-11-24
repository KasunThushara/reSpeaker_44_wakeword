#include <Wire.h>

// Last software chance for the dark 0.96" 128x64 module at 0x3C.
//
// Everything else is ruled out: VCC on TP15 (3.3 V), SDA/SCL idle high, the
// module ACKs at 0x3C. It stays dark with SSD1306 and SH1106 init, all-on and
// max contrast. So this sketch removes every remaining software variable:
//   * 10 kHz clock (maximum tolerance, no rise-time issues)
//   * 500 ms after power-up before the first byte
//   * the init sequence repeated every 2 s, then a full all-on frame
//   * charge pump explicitly on, display explicitly on, contrast max
//
// If the panel is alive in any way, it will glow. If it stays black, the module
// is faulty (bad header solder joints, dead panel, or dead controller).

#define SDA_PIN 5
#define SCL_PIN 6
#define OLED_ADDR 0x3C

static uint8_t on[1024];
static uint32_t nak = 0, ok = 0;

uint8_t c1(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(c);
  uint8_t e = Wire.endTransmission();
  if (e) nak++; else ok++;
  return e;
}
void c2(uint8_t c, uint8_t a) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(c); Wire.write(a);
  if (Wire.endTransmission()) nak++; else ok++;
}

void initPanel() {
  c1(0xAE);                 // off
  c2(0xD5, 0x80);           // clock divide 1
  c2(0xA8, 0x3F);           // multiplex 64
  c2(0xD3, 0x00);           // offset 0
  c1(0x40);
  c2(0x8D, 0x14);           // charge pump ON (needed: no external VCC for panel)
  c2(0x20, 0x00);           // horizontal addressing
  c1(0xA1); c1(0xC8);       // orientation
  c2(0xDA, 0x12);           // COM pins
  c2(0x81, 0xFF);           // contrast max
  c2(0xD9, 0xF1);
  c2(0xDB, 0x40);
  c1(0xA4); c1(0xA6); c1(0x2E);
  delay(20);
  c1(0xAF);                 // display ON
  delay(20);
}

void pushAllOn() {
  c2(0x21, 0x00); c2(0x21, 127);
  c2(0x22, 0x00); c2(0x22, 7);
  for (uint8_t p = 0; p < 8; p++) {
    for (uint8_t off = 0; off < 128; off += 16) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write(0x40);
      Wire.write(&on[p * 128 + off], 16);
      uint8_t e = Wire.endTransmission();
      if (e) nak++; else ok++;
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  memset(on, 0xFF, sizeof(on));

  Serial.println();
  Serial.println(F("=== dark-panel last check: 10 kHz, repeated init, all pixels on ==="));
  Serial.println(F("panel should be a solid white/green block within 2 s"));

  delay(500);                      // power settle
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(10000);            // extremely forgiving
  delay(50);
}

void loop() {
  static int round = 0;
  initPanel();
  pushAllOn();
  Serial.printf("round %d done: %lu ok, %lu nak (clock 10 kHz)\n", round,
                (unsigned long)ok, (unsigned long)nak);
  round++;
  delay(1800);
}
