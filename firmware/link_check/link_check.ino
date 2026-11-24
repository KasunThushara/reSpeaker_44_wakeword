#include <Wire.h>
#include <esp_system.h>

// link_check v3 - diagnose WHY 0x3C NAKs, not just that it does.
//
// v1/v2 evidence (2026-11-18, panel on J2/D4-D5, VCC=TP15):
//   * J2 hosts 0x18 codec + 0x2C XMOS + 0x3C panel; J6 is empty
//   * panel never lights: 0xA5 (hardware all-on) also dark
//   * NAK behaviour is wildly inconsistent between runs:
//       v1: init 77 ok/8 nak, frame 137 ok/0 nak, still black
//       v2: init 17 ok/2 nak, frame   0 ok/68 nak
//     and the 400 kHz scan found nothing while 100 kHz found 0x2C + 0x3C.
//
// So the interesting variable is the BUS, not the panel. v3 prints the
// endTransmission() error code for every failure:
//   1 = data too long   2 = NACK on address   3 = NACK on data
//   4 = other bus error (arbitration lost!)    5 = timeout
// A code-4/5 storm means something else is driving the bus (the XMOS shares
// it). Code 2 means the module really did not answer its address.
//
// Each clock gets: 9-clock bus recovery, init, 0xA5 all-on hold, frame hold.
// Both clocks are tested back to back so the comparison is fair.

#define OLED_ADDR 0x3C
#define CHUNK 16

static const uint8_t PAIRS[2][2] = {{5, 6}, {3, 1}};
static const char *PAIRS_WHAT[2] = {
  "J2  SDA=D4/GPIO5  SCL=D5/GPIO6   (shared: 0x18 codec, 0x2C XMOS)",
  "J6  SDA=D2/GPIO3  SCL=D0/GPIO1   (free pads, nothing else attached)"
};
static const uint32_t CLOCKS[2] = {100000, 400000};

static int busSel = -1;
static uint32_t okC = 0, nakC = 0;
static uint32_t hist[6];
static uint8_t fb[1024];

static const char *who(uint8_t a) {
  switch (a) {
    case 0x3C: case 0x3D: return " <- SSD1306/SH1106 OLED";
    case 0x18: case 0x19: return " <- TLV320 codec (board audio)";
    case 0x2C: case 0x2D: return " <- XMOS control (XVF3800)";
    default: return "";
  }
}

// 9 clocks + STOP: frees a slave that is holding SDA low and resets the bus
static void recoverBus() {
  Wire.end();
  pinMode(PAIRS[busSel][0], INPUT_PULLUP);
  pinMode(PAIRS[busSel][1], INPUT_PULLUP);
  delay(1);
  int stuck = (digitalRead(PAIRS[busSel][0]) == 0) || (digitalRead(PAIRS[busSel][1]) == 0);
  if (stuck) {
    pinMode(PAIRS[busSel][1], OUTPUT);
    for (int i = 0; i < 9; i++) {
      digitalWrite(PAIRS[busSel][1], LOW);  delayMicroseconds(5);
      digitalWrite(PAIRS[busSel][1], HIGH); delayMicroseconds(5);
    }
    pinMode(PAIRS[busSel][0], OUTPUT);
    digitalWrite(PAIRS[busSel][0], LOW);   delayMicroseconds(5);
    digitalWrite(PAIRS[busSel][1], HIGH);  delayMicroseconds(5);
    digitalWrite(PAIRS[busSel][0], HIGH);  delayMicroseconds(5);
    pinMode(PAIRS[busSel][0], INPUT_PULLUP);
  }
  Wire.begin(PAIRS[busSel][0], PAIRS[busSel][1]);
  delay(3);
  Serial.printf("      bus recovery: stuck=%d, SDA=%d SCL=%d\n", stuck,
                digitalRead(PAIRS[busSel][0]), digitalRead(PAIRS[busSel][1]));
}

static void note(uint8_t e) {
  if (e) { nakC++; if (e < 6) hist[e]++; } else okC++;
}
static void write1(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  note(Wire.endTransmission());
}
static void write2(uint8_t c, uint8_t a) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.write(c);
  Wire.write(a);
  note(Wire.endTransmission());
}

static int scanAt(int i, uint32_t hz) {
  Wire.end();
  delay(5);
  Wire.begin(PAIRS[i][0], PAIRS[i][1]);
  Wire.setClock(hz);
  delay(10);
  int n = 0, printed = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      n++;
      if (printed < 8) { Serial.printf("      0x%02X ACK%s\n", a, who(a)); printed++; }
    }
  }
  if (n > 8) Serial.printf("      ... %d ACKs total%s\n", n,
                           n > 20 ? "  <- FLOATING BUS, no device here" : "");
  if (n == 0) Serial.println(F("      (nothing ACKs)"));
  return n;
}

static bool scanPair(int i) {
  Serial.printf("[%6lu ms] %s\n", (unsigned long)millis(), PAIRS_WHAT[i]);
  int n = scanAt(i, 400000);
  if (n == 0) { Serial.printf("      retry @100 kHz: "); n = scanAt(i, 100000); }
  return n > 0 && n < 20;
}

static void initPanel() {
  write1(0xAE);
  write2(0xD5, 0x80);
  write2(0xA8, 0x3F);
  write2(0xD3, 0x00);
  write1(0x40);
  write2(0x8D, 0x14);   // charge pump on
  write2(0x20, 0x00);
  write1(0xA1);
  write1(0xC8);
  write2(0xDA, 0x12);
  write2(0x81, 0xFF);
  write2(0xD9, 0xF1);
  write2(0xDB, 0x40);
  write1(0xA4);
  write1(0xA6);
  write1(0x2E);
  delay(20);
  write1(0xAF);
  delay(20);
}

static void pushFrame(uint8_t fill) {
  write2(0x21, 0x00); write2(0x21, 127);
  write2(0x22, 0x00); write2(0x22, 0x07);
  for (uint8_t p = 0; p < 8; p++) {
    for (uint8_t off = 0; off < 128; off += CHUNK) {
      for (uint8_t k = 0; k < CHUNK; k++) fb[p * 128 + off + k] = fill;
      Wire.beginTransmission(OLED_ADDR);
      Wire.write((uint8_t)0x40);
      Wire.write(&fb[p * 128 + off], CHUNK);
      note(Wire.endTransmission());
    }
  }
}

static void report(const char *label) {
  Serial.printf("      %s: %lu ok, %lu nak", label,
                (unsigned long)okC, (unsigned long)nakC);
  if (nakC) {
    Serial.print(F("   codes:"));
    for (int i = 1; i < 6; i++) if (hist[i]) Serial.printf(" [%d]=%lu", i, (unsigned long)hist[i]);
  }
  Serial.println();
}

static void hold(const char *label, int seconds) {
  Serial.printf("      HOLD %d s: %s\n", seconds, label);
  for (int s = seconds; s > 0; s -= 2) {
    Serial.printf("        %2d s\n", s);
    delay(2000);
  }
}

static void clockTest(uint32_t hz) {
  Serial.printf("\n--- %lu kHz ---\n", (unsigned long)(hz / 1000));
  recoverBus();
  Wire.setClock(hz);
  delay(10);

  okC = nakC = 0; memset(hist, 0, sizeof(hist));
  initPanel();
  write2(0x8D, 0x14);
  write1(0xA5);                       // hardware all-on: no GDDRAM involved
  report("init + 0xA5");
  Serial.println(F("        panel must be solid white NOW (hardware all-on)"));
  hold("0xA5 all-on", 8);

  okC = nakC = 0; memset(hist, 0, sizeof(hist));
  write1(0xA4);
  write1(0xAF);
  pushFrame(0xFF);
  report("frame 0xFF via GDDRAM");

  write1(0xAE);
}

static void measureRound() {
  Serial.println(F("\n=== ROUND: clock A/B ==="));
  for (int c = 0; c < 2; c++) clockTest(CLOCKS[c]);
  Serial.println(F("=== ROUND END ==="));
  Serial.println(F("code 2 = address NACK (device silent), 4 = bus/arbitration, 5 = timeout"));
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  esp_reset_reason_t rr = esp_reset_reason();
  Serial.println();
  Serial.println(F("=== link_check v3 ==="));
  Serial.printf("reset reason: %d (%s)\n", (int)rr,
                rr == ESP_RST_POWERON ? "power-on" :
                rr == ESP_RST_BROWNOUT ? "BROWNOUT - rail collapsed" :
                rr == ESP_RST_SW ? "software" : "other");

  bool a = scanPair(0);
  bool b = scanPair(1);
  busSel = a ? 0 : (b ? 1 : -1);

  if (busSel < 0) {
    Serial.println(F("\n*** no panel on either pair *** (re-scanning every 10 s)"));
    return;
  }
  Serial.printf("\n*** using %s ***\n", busSel == 0 ? "J2 (D4/D5)" : "J6 (D2/D0)");
  measureRound();
  Serial.println(F("=== idle 5 s, then rescan so you can unplug the panel ==="));
}

void loop() {
  static uint32_t lastScan = 0;

  if (busSel < 0) {
    if (millis() - lastScan > 10000) {
      lastScan = millis();
      bool a = scanPair(0);
      bool b = scanPair(1);
      busSel = a ? 0 : (b ? 1 : -1);
      if (busSel >= 0) {
        Serial.printf("*** panel found on %s ***\n", busSel == 0 ? "J2 (D4/D5)" : "J6 (D2/D0)");
        measureRound();
      }
    }
    return;
  }

  // every 20 s: rescan (unplug the panel here to prove 0x3C is really it)
  if (millis() - lastScan > 20000) {
    lastScan = millis();
    Serial.println(F("\n--- rescan (unplug the panel now if you want to test) ---"));
    int keep = busSel;
    bool a = scanPair(0);
    bool b = scanPair(1);
    int found = a ? 0 : (b ? 1 : -1);
    Serial.printf("      panel on %s\n", found < 0 ? "NOWHERE" : (found == 0 ? "J2" : "J6"));
    busSel = keep;
    measureRound();
    return;
  }

  delay(200);
}
