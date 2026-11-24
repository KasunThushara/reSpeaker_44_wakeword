#include <Arduino.h>

// power_check - steady 0xA5 hardware-all-on, no bus traffic, for a DMM.
//
// Everything else is proven (2026-11-18):
//   * panel answers at 0x3C, moves with the wires, so it is the display
//   * software I2C at 30 kHz: init 60/60, frame 1088/1088, ZERO lost bytes
//   * 0xA5 hardware all-on still produces no light
//
// So the remaining possibilities are the module's power path or its glass:
//   charge pump on + all pixels on, healthy  -> 20-30 mA at 3.3 V
//   ~0.5-1 mA                                -> charge pump never starts
//   ~0 mA                                    -> panel rail not reaching VCC
//
// This sketch sets 0xA5 once and then stays completely off the bus, so the
// current you measure is the panel's alone. It prints a heartbeat so you know
// it is alive, and it re-sends 0xA5 every 20 s (short burst) in case the
// controller dropped it.

#define SDA 40   // D13 / GPIO40
#define SCL 41   // D14 / GPIO41
#define OLED_ADDR 0x3C
static const int T = 12;

static void d(int us) { delayMicroseconds(us); }
static void sdaLow()     { pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW); }
static void sdaRelease() { pinMode(SDA, INPUT_PULLUP); }
static void sclLow()     { pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW); }
static void sclRelease() { pinMode(SCL, INPUT_PULLUP); }
static void i2cStart() { sdaRelease(); sclRelease(); d(T); sdaLow(); d(T); sclLow(); d(T); }
static void i2cStop()  { sdaLow(); d(T); sclRelease(); d(T); sdaRelease(); d(T); }
static bool i2cWrite(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if (b & (1 << i)) sdaRelease(); else sdaLow();
    d(T); sclRelease(); d(T); sclLow(); d(T);
  }
  sdaRelease(); d(T); sclRelease(); d(T);
  bool ack = (digitalRead(SDA) == 0);
  sclLow(); d(T);
  return ack;
}
static int i2cSend(const uint8_t *buf, int len) {
  i2cStart();
  if (!i2cWrite((uint8_t)(OLED_ADDR << 1))) { i2cStop(); return 0; }
  int a = 0;
  for (int i = 0; i < len; i++) if (i2cWrite(buf[i])) a++;
  i2cStop();
  return a;
}
static void cmd(uint8_t c) { uint8_t b[2] = {0x00, c}; i2cSend(b, 2); }
static void cmd2(uint8_t c, uint8_t a) { uint8_t b[3] = {0x00, c, a}; i2cSend(b, 3); }

static void initPanel() {
  cmd(0xAE);
  cmd2(0xD5, 0x80);
  cmd2(0xA8, 0x3F);
  cmd2(0xD3, 0x00);
  cmd(0x40);
  cmd2(0x8D, 0x14);    // charge pump ON
  cmd2(0x20, 0x00);
  cmd(0xA1); cmd(0xC8);
  cmd2(0xDA, 0x12);
  cmd2(0x81, 0xFF);    // contrast max
  cmd2(0xD9, 0xF1);    // precharge
  cmd2(0xDB, 0x40);    // VCOMH
  cmd(0xA5);           // ENTIRE DISPLAY ON (hardware)
  cmd(0x2E);           // no scroll
  cmd(0xAF);           // display on
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(50);

  Serial.println();
  Serial.println(F("=== power_check: 0xA5 hardware all-on, then silent ==="));
  i2cStart();
  bool present = i2cWrite((uint8_t)(OLED_ADDR << 1));
  i2cStop();
  Serial.printf("probe 0x3C: %s\n", present ? "ACK" : "NAK");
  if (!present) return;
  initPanel();
  Serial.println(F("0xA5 sent. Bus is now IDLE - the meter sees the panel alone."));
  Serial.println(F("  measure VCC at the module pin        (expect 3.3 V)"));
  Serial.println(F("  measure current in series with VCC   (mA range)"));
  Serial.println(F("  20-30 mA = panel driven but dark -> glass/flex dead"));
  Serial.println(F("  ~1 mA    = charge pump never starts"));
  Serial.println(F("  ~0 mA    = VCC not reaching the panel"));
}

void loop() {
  static uint32_t tPrint = 0, tRefresh = 0;
  if (millis() - tPrint > 5000) {
    tPrint = millis();
    Serial.printf("[%lu s] 0xA5 held, bus idle\n", (unsigned long)(millis() / 1000));
  }
  if (millis() - tRefresh > 20000) {   // refresh the command occasionally
    tRefresh = millis();
    cmd(0xA5);
  }
}
