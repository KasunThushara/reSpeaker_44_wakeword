#include <Arduino.h>

// bb_oled - software I2C on the weak internal pull-ups.
//
// Evidence (2026-11-18):
//   * panel on J5: SDA=D13/GPIO40, SCL=D14/GPIO41, answers at 0x3C
//   * rise-time probe: BOTH lines NEVER rise against an internal 45k pull-down
//     -> the module has NO onboard pull-ups and J5 has none either; the bus
//        runs on the ESP32's internal ~45k only
//   * a 45k pull-up with ~50 pF gives ~2.2 us rise: fine at 30 kHz, fatal at
//     400 kHz. Hardware Wire gave init 16/17 but frames 0/68.
//
// So drive the bus by hand, open-drain, with a 15 us delay per phase (~30 kHz).
// Every byte's ACK is reported, so we see data integrity directly instead of
// trusting Wire. A frame is ~1024 bytes -> roughly 0.5 s. Slow, but it answers
// the only question left: is the panel alive?
//
// If this lights the panel: the fix is two 4.7k resistors to 3.3 V (or a module
// that has them), and the animation can run at 400 kHz.

#define SDA 40   // D13 / GPIO40
#define SCL 41   // D14 / GPIO41
#define OLED_ADDR 0x3C

static const int T = 15;         // us per half period
static uint32_t ackOk = 0, ackNak = 0;

static void d(int us) { delayMicroseconds(us); }

static void sdaLow()     { pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW); }
static void sdaRelease() { pinMode(SDA, INPUT_PULLUP); }
static void sclLow()     { pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW); }
static void sclRelease() { pinMode(SCL, INPUT_PULLUP); }

static void i2cStart() {
  sdaRelease(); sclRelease(); d(T);
  sdaLow(); d(T);
  sclLow(); d(T);
}

static void i2cStop() {
  sdaLow(); d(T);
  sclRelease(); d(T);
  sdaRelease(); d(T);
}

// returns true on ACK
static bool i2cWrite(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if (b & (1 << i)) sdaRelease(); else sdaLow();
    d(T);
    sclRelease(); d(T);
    sclLow(); d(T);
  }
  sdaRelease(); d(T);        // release for the ACK bit
  sclRelease(); d(T);
  bool ack = (digitalRead(SDA) == 0);
  sclLow(); d(T);
  if (ack) ackOk++; else ackNak++;
  return ack;
}

// full transaction: addr + payload, returns number of ACKed bytes
static int i2cSend(const uint8_t *buf, int len) {
  i2cStart();
  if (!i2cWrite((uint8_t)(OLED_ADDR << 1))) { i2cStop(); return 0; }
  int acked = 0;
  for (int i = 0; i < len; i++) if (i2cWrite(buf[i])) acked++;
  i2cStop();
  return acked;
}

static void cmd(uint8_t c) {
  uint8_t b[2] = {0x00, c};
  i2cSend(b, 2);
}
static void cmd2(uint8_t c, uint8_t a) {
  uint8_t b[3] = {0x00, c, a};
  i2cSend(b, 3);
}

static uint8_t fb[1024];

static void pushFrame(uint8_t fill) {
  for (int p = 0; p < 8; p++) {
    for (int off = 0; off < 128; off += 32) {
      uint8_t b[33];
      b[0] = 0x40;
      for (int k = 0; k < 32; k++) b[1 + k] = fill;
      i2cSend(b, 33);
    }
  }
}

static void initPanel() {
  cmd(0xAE);
  cmd2(0xD5, 0x80);
  cmd2(0xA8, 0x3F);
  cmd2(0xD3, 0x00);
  cmd(0x40);
  cmd2(0x8D, 0x14);     // charge pump ON
  cmd2(0x20, 0x00);
  cmd(0xA1);
  cmd(0xC8);
  cmd2(0xDA, 0x12);
  cmd2(0x81, 0xFF);
  cmd2(0xD9, 0xF1);
  cmd2(0xDB, 0x40);
  cmd(0xA4);
  cmd(0xA6);
  cmd(0x2E);
  cmd(0xAF);
}

static const char *yn(bool b) { return b ? "ACK" : "NAK"; }

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(50);

  Serial.println();
  Serial.println(F("=== bb_oled: software I2C ~30 kHz, D13=SDA D14=SCL ==="));

  // 1. bare address probe
  i2cStart();
  bool present = i2cWrite((uint8_t)(OLED_ADDR << 1));
  i2cStop();
  Serial.printf("probe 0x%02X: %s\n", OLED_ADDR, yn(present));
  if (!present) {
    Serial.println(F("*** no answer by bit-bang either - check the 4 wires ***"));
    Serial.println(F("(SDA=D13/GPIO40, SCL=D14/GPIO41, GND, VCC on TP15)"));
    return;
  }

  // 2. init, watching ACK counts
  ackOk = ackNak = 0;
  initPanel();
  Serial.printf("init: %lu ACKs, %lu NAKs (want 0 NAKs to call it clean)\n",
                (unsigned long)ackOk, (unsigned long)ackNak);

  // 3. hardware all-on: no GDDRAM involved at all
  cmd(0xA5);
  Serial.println(F(">>> 0xA5 hardware all-on - PANEL SHOULD BE SOLID WHITE (15 s)"));
  delay(15000);

  // 4. a real frame through GDDRAM
  cmd(0xA4);
  cmd(0xAF);
  cmd2(0x21, 0x00); cmd2(0x21, 127);
  cmd2(0x22, 0x00); cmd2(0x22, 0x07);
  ackOk = ackNak = 0;
  uint32_t ms0 = millis();
  pushFrame(0xFF);
  Serial.printf("0xFF frame: %lu ACKs, %lu NAKs, %lu ms\n",
                (unsigned long)ackOk, (unsigned long)ackNak, (unsigned long)(millis() - ms0));
  Serial.println(F(">>> frame all-on (10 s)"));
  delay(10000);

  // 5. checkerboard so any lit pixels are obvious
  cmd2(0x21, 0x00); cmd2(0x21, 127);
  cmd2(0x22, 0x00); cmd2(0x22, 0x07);
  for (int p = 0; p < 8; p++) {
    for (int off = 0; off < 128; off += 32) {
      uint8_t b[33];
      b[0] = 0x40;
      for (int k = 0; k < 32; k++) b[1 + k] = ((p + k) & 1) ? 0xFF : 0x00;
      i2cSend(b, 33);
    }
  }
  Serial.println(F(">>> checkerboard (10 s)"));
  delay(10000);

  Serial.println(F("=== done; repeats every 20 s ==="));
}

void loop() {
  delay(20000);
  Serial.println(F("--- repeat: 0xA5 all-on 10 s ---"));
  cmd(0xA5);
  delay(10000);
  cmd(0xA4); cmd(0xAF);
  pushFrame(0xFF);
  delay(6000);
  pushFrame(0x00);
}
