#include <Wire.h>

// display_ab - run the panel on whichever pair it is plugged into.
//
// 2026-11-18 clock sweep (display_slow) killed the "slow SDA rise" theory and
// replaced it with a better one:
//
//     20 kHz -> init 8/17 ok, frame  0/68 ok
//     50 kHz -> init 12/17 ok, frame  9/68 ok
//    100 kHz -> init 15/17 ok, frame 60/68 ok
//    400 kHz -> init 15/17 ok, frame 63/68 ok   (link_check earlier)
//
// Reliability RISES with clock speed. Nothing about rise time can do that, but
// a SECOND MASTER can: slower transactions hold the bus longer, so collisions
// are more likely, and a colliding master pulling SDA low is indistinguishable
// from a valid ACK (which is how a NAK-free init can still deliver garbage).
//
// D4/D5 is the XVF3800 control bus (0x18 codec, 0x2C XMOS), so the OLED shares
// it with whatever the XMOS does. J6 (D2/D0) is a free pair with no other
// device - verified empty by the presence_check rise-time test.
//
// So: plug SDA into J6 pin 2 (D2/GPIO3), SCL into J6 pin 4 (D0/GPIO1),
// GND into J6 pin 1, VCC stays on TP15. This sketch finds it automatically.

#define OLED_ADDR 0x3C

static const uint8_t PAIRS[2][2] = {{5, 6}, {3, 1}};
static const char *NAMES[2] = {"J2  D4/D5 (shared with codec + XMOS)",
                               "J6  D2/D0 (free pair)"};

static int busSel = -1;
static uint32_t okC = 0, nakC = 0;
static uint8_t fb[1024];

static void busRecover() {
  Wire.end();
  pinMode(PAIRS[busSel][0], INPUT_PULLUP);
  pinMode(PAIRS[busSel][1], INPUT_PULLUP);
  delay(2);
  bool stuck = !digitalRead(PAIRS[busSel][0]) || !digitalRead(PAIRS[busSel][1]);
  if (stuck) {
    pinMode(PAIRS[busSel][1], OUTPUT);
    for (int i = 0; i < 9; i++) {
      digitalWrite(PAIRS[busSel][1], LOW);  delayMicroseconds(5);
      digitalWrite(PAIRS[busSel][1], HIGH); delayMicroseconds(5);
    }
    pinMode(PAIRS[busSel][0], OUTPUT);
    digitalWrite(PAIRS[busSel][0], LOW);  delayMicroseconds(5);
    digitalWrite(PAIRS[busSel][1], HIGH); delayMicroseconds(5);
    digitalWrite(PAIRS[busSel][0], HIGH); delayMicroseconds(5);
    pinMode(PAIRS[busSel][0], INPUT_PULLUP);
  }
  Wire.begin(PAIRS[busSel][0], PAIRS[busSel][1]);
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

// returns pair index hosting a real (non-ghost) 0x3C, or -1
static int findPanel() {
  for (int i = 0; i < 2; i++) {
    Wire.end();
    delay(3);
    Wire.begin(PAIRS[i][0], PAIRS[i][1]);
    Wire.setClock(100000);
    delay(5);
    int count = 0;
    bool oled = false;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { count++; if (a == OLED_ADDR) oled = true; }
    }
    Serial.printf("  %-38s %d ACK%s%s\n", NAMES[i], count,
                  oled ? ", 0x3C present" : "",
                  count > 20 ? "  (floating bus, ghost ACKs)" : "");
    if (oled && count <= 20) return i;
  }
  return -1;
}

static void runAt(uint32_t hz) {
  busRecover();
  Wire.setClock(hz);
  delay(10);
  okC = nakC = 0;
  initSSD1306();
  Serial.printf("  @%-4luk init %lu ok / %lu nak", (unsigned long)(hz / 1000),
                (unsigned long)okC, (unsigned long)nakC);
  w1(0xA5);
  Serial.printf(" | 0xA5 -> %s\n", nakC ? "maybe lost" : "sent clean");
  delay(7000);
  w1(0xA4); w1(0xAF);
  okC = nakC = 0;
  pushFrame(0xFF);
  Serial.printf("  @%-4luk frame %lu ok / %lu nak", (unsigned long)(hz / 1000),
                (unsigned long)okC, (unsigned long)nakC);
  pushFrame(0x00);
  Serial.printf("  (both frames sent)\n");
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== display_ab: looking for the panel on J2 (D4/D5) and J6 (D2/D0) ==="));

  busSel = findPanel();
  if (busSel < 0) {
    Serial.println(F("*** no panel found on either pair - check the 4 wires ***"));
    return;
  }
  Serial.printf("*** PANEL ON %s ***\n", NAMES[busSel]);
  runAt(100000);
  runAt(400000);
  Serial.println(F("=== entering 2 s on / 2 s off flash loop ==="));
}

void loop() {
  static uint32_t lastFind = 0;

  if (busSel < 0) {
    if (millis() - lastFind > 8000) {
      lastFind = millis();
      busSel = findPanel();
      if (busSel >= 0) {
        Serial.printf("*** PANEL ON %s ***\n", NAMES[busSel]);
        runAt(100000);
        runAt(400000);
      }
    }
    return;
  }

  okC = nakC = 0;
  w1(0xAF);
  pushFrame(0xFF);
  delay(2000);
  pushFrame(0x00);
  delay(2000);
  if (nakC) Serial.printf("  flash round: %lu nak\n", (unsigned long)nakC);

  if (millis() - lastFind > 30000) {
    lastFind = millis();
    int keep = busSel;
    Serial.println(F("--- re-detect (you can move the wires now) ---"));
    int f = findPanel();
    if (f >= 0 && f != keep) {
      busSel = f;
      Serial.printf("*** panel moved to %s ***\n", NAMES[busSel]);
      runAt(100000);
    } else if (f < 0) {
      Serial.println(F("  panel not found on either pair"));
    }
    Wire.end(); delay(3);
    Wire.begin(PAIRS[keep][0], PAIRS[keep][1]);
    Wire.setClock(400000);
    delay(10);
    initSSD1306();
  }
}
