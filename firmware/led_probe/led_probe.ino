#include <Wire.h>

// led_probe - N1: prove the 24x WS2812 ring and the DOA readout work over I2C.
//
// The ring is rendered by the XVF3800 itself; the XIAO only sends commands to
// the GPO servicer (RESID 20) on the same bus as the mic control (0x2C).
// Source: wiki "LED Effects for reSpeaker XVF3800 44mm with XIAO ESP32-S3 Plus".
//
//   GPO write (RESID 20, cmd 1)  {pin, value}   pin 33 = X0D33 = LED power rail
//   LED effect    (12)  1 byte  0 off, 1 breathing, 2 rainbow, 3 solid, 4 DOA, 5 ring
//   LED brightness(13)  1 byte  0-255
//   LED speed     (15)  1 byte
//   LED colour    (16)  4 bytes B, G, R, 0x00   (little-endian!)
//   DOA read      (20, cmd 18|0x80, 5) -> status + angle LE u16 + speech LE u16
//
// Every transaction is retried: this bus goes through 2.0 mm header pins with
// 2.54 mm duponts, which has been the source of every flaky I2C result so far.

#define XMOS_ADDR 0x2C
#define SDA_PIN 5      // D4
#define SCL_PIN 6      // D5

#define GPO_RESID 20
#define APP_RESID 48
#define CMD_GPO_WRITE_VALUE 1
#define CMD_LED_EFFECT      12
#define CMD_LED_BRIGHTNESS  13
#define CMD_LED_SPEED       15
#define CMD_LED_COLOR       16
#define CMD_DOA             18
#define CMD_VERSION         0
#define LED_POWER_PIN       33

enum : uint8_t {
  LED_OFF = 0, LED_BREATHING = 1, LED_RAINBOW = 2,
  LED_SOLID = 3, LED_DOA = 4, LED_RING = 5
};

static uint32_t i2cFails = 0;

static bool xmosWrite(uint8_t resid, uint8_t cmd, const uint8_t *val, uint8_t n) {
  for (int attempt = 0; attempt < 3; attempt++) {
    Wire.beginTransmission(XMOS_ADDR);
    Wire.write(resid);
    Wire.write(cmd);
    Wire.write(n);
    for (uint8_t i = 0; i < n; i++) Wire.write(val[i]);
    if (Wire.endTransmission() == 0) return true;
    delay(3);
  }
  i2cFails++;
  Serial.printf("  !! write failed RESID=%u CMD=%u\n", resid, cmd);
  return false;
}

static bool xmosRead(uint8_t resid, uint8_t cmd, uint8_t *payload, uint8_t n) {
  for (int attempt = 0; attempt < 3; attempt++) {
    Wire.beginTransmission(XMOS_ADDR);
    Wire.write(resid);
    Wire.write((uint8_t)(cmd | 0x80));
    Wire.write((uint8_t)(n + 1));
    if (Wire.endTransmission() == 0) {
      uint8_t got = Wire.requestFrom((uint8_t)XMOS_ADDR, (uint8_t)(n + 1));
      if (got == (uint8_t)(n + 1)) {
        uint8_t status = Wire.read();
        for (uint8_t i = 0; i < n; i++) payload[i] = Wire.read();
        if (status == 0) return true;
        Serial.printf("  !! read status 0x%02X (RESID=%u CMD=%u)\n", status, resid, cmd);
        return false;
      }
      while (Wire.available()) Wire.read();
    }
    delay(3);
  }
  i2cFails++;
  Serial.printf("  !! read failed RESID=%u CMD=%u\n", resid, cmd);
  return false;
}

static bool ledPower(bool on) {
  uint8_t p[2] = {LED_POWER_PIN, (uint8_t)(on ? 1 : 0)};
  bool ok = xmosWrite(GPO_RESID, CMD_GPO_WRITE_VALUE, p, 2);
  Serial.printf("  LED power rail: %s\n", ok ? (on ? "ON" : "OFF") : "FAILED");
  return ok;
}

static bool ledEffect(uint8_t e) {
  bool ok = xmosWrite(GPO_RESID, CMD_LED_EFFECT, &e, 1);
  Serial.printf("  effect -> %u (%s)\n", e, ok ? "ok" : "FAILED");
  return ok;
}

static bool ledBrightness(uint8_t b) {
  bool ok = xmosWrite(GPO_RESID, CMD_LED_BRIGHTNESS, &b, 1);
  Serial.printf("  brightness -> %u (%s)\n", b, ok ? "ok" : "FAILED");
  return ok;
}

static bool ledSpeed(uint8_t s) {
  return xmosWrite(GPO_RESID, CMD_LED_SPEED, &s, 1);
}

// colour is 0xRRGGBB; the wire order is B, G, R, 0
static bool ledColor(uint32_t rgb) {
  uint8_t p[4] = {
    (uint8_t)(rgb & 0xFF),
    (uint8_t)((rgb >> 8) & 0xFF),
    (uint8_t)((rgb >> 16) & 0xFF),
    0x00
  };
  bool ok = xmosWrite(GPO_RESID, CMD_LED_COLOR, p, 4);
  Serial.printf("  colour -> 0x%06lX (%s)\n", (unsigned long)(rgb & 0xFFFFFF), ok ? "ok" : "FAILED");
  return ok;
}

// reads back the colour the XMOS actually holds
static bool ledColorReadback(uint32_t &rgb) {
  uint8_t b[4] = {0};
  if (!xmosRead(GPO_RESID, CMD_LED_COLOR, b, 4)) return false;
  rgb = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
  return true;
}

static bool readDoa(uint16_t &angle, uint16_t &speech) {
  uint8_t b[4] = {0};
  if (!xmosRead(GPO_RESID, CMD_DOA, b, 4)) return false;
  angle = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  speech = (uint16_t)b[2] | ((uint16_t)b[3] << 8);
  return true;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Serial.println();
  Serial.println(F("=== led_probe (N1): WS2812 ring + DOA over I2C ==="));
  Serial.println(F("SDA=D4/GPIO5  SCL=D5/GPIO6  XMOS=0x2C"));

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  delay(100);

  // let the XMOS finish its own boot animation first
  Serial.println(F("waiting 2.5 s for the XVF3800 boot animation..."));
  delay(2500);

  uint8_t ver[3] = {0};
  if (xmosRead(APP_RESID, CMD_VERSION, ver, 3)) {
    Serial.printf("XMOS firmware: %u.%u.%u\n", ver[0], ver[1], ver[2]);
  } else {
    Serial.println(F("could not read the firmware version - check the 4 wires"));
  }

  ledPower(true);
  delay(100);
  ledBrightness(150);
  ledSpeed(60);
  delay(100);

  // colour write + readback: proves commands land and the link is two-way
  ledEffect(LED_SOLID);
  ledColor(0xFFFFFF);
  delay(50);
  uint32_t rb = 0;
  if (ledColorReadback(rb)) {
    Serial.printf("colour readback 0x%06lX -> %s\n",
                  (unsigned long)(rb & 0xFFFFFF),
                  (rb & 0xFFFFFF) == 0xFFFFFF ? "PASS" : "MISMATCH");
  }

  Serial.println(F("\n--- sweep (watch the ring, ~2 s each) ---"));
  Serial.println(F("1) OFF"));                       ledEffect(LED_OFF);        delay(2000);
  Serial.println(F("2) SOLID RED"));                 ledEffect(LED_SOLID); ledColor(0xFF0000); delay(2000);
  Serial.println(F("3) SOLID GREEN"));               ledColor(0x00FF00); delay(2000);
  Serial.println(F("4) SOLID BLUE"));                ledColor(0x0000FF); delay(2000);
  Serial.println(F("5) BREATHING (teal)"));          ledEffect(LED_BREATHING); ledColor(0x00FFFF); delay(2500);
  Serial.println(F("6) RAINBOW"));                   ledEffect(LED_RAINBOW); delay(2500);
  Serial.println(F("7) RING"));                      ledEffect(LED_RING); delay(2500);
  Serial.println(F("8) DOA indicator (default idle) - talk to it"));
  ledEffect(LED_DOA);
  ledBrightness(150);

  Serial.println(F("\n--- DOA polling: speak from different sides ---"));
  Serial.println(F("  expect the lit dot to follow your voice; SILENT when quiet"));
}

void loop() {
  static uint32_t lastDoa = 0, lastReport = 0;

  if (millis() - lastDoa >= 250) {
    lastDoa = millis();
    uint16_t angle = 0, speech = 0;
    if (readDoa(angle, speech)) {
      if (angle <= 359) {
        Serial.printf("DOA %3u deg | speech %s\n", angle,
                      speech == 1 ? "YES" : speech == 0 ? "no " : "?? ");
      } else {
        Serial.printf("DOA  N/A (0x%04X) | speech %s\n", angle,
                      speech == 1 ? "YES" : speech == 0 ? "no " : "?? ");
      }
    }
  }

  if (millis() - lastReport > 5000) {
    lastReport = millis();
    Serial.printf("[%lus] i2c failures so far: %lu\n",
                  (unsigned long)(millis() / 1000), (unsigned long)i2cFails);
  }
}
