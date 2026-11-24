#include <Wire.h>
#include <esp_cpu.h>

// display_pairs - find the panel on ANY free pin pair, then drive it.
//
// Wiring is not fixed yet, so this scans every legal pair on the reSpeaker
// 44mm board (KiCad netlist): J2 = D4/D5, J6 = D2/D0, J5 = D14/D13/D3.
//
// For each pair it prints the ACK count. A count above 20 means a floating bus
// (ghost ACKs), which is what an empty pair looks like on this chip.
//
// Once a real 0x3C is found it measures the rise time of both lines against an
// internal 45k pull-down (a healthy module pull-up rises in well under 1 us),
// then runs the SSD1306 init + 0xA5 hardware-all-on + frames at 100k and 400k.
//
// Note for the wiring: the 44mm headers are 2.0 mm pitch, so 2.54 mm dupont
// jumpers sit loose on them - exactly the kind of contact that produces a
// variable 1.5-2.5 us SDA rise and a bus that dies mid-scan.

#define OLED_ADDR 0x3C

struct Pair { const char *label; uint8_t sda; uint8_t scl; };
static const Pair PAIRS[] = {
  {"J2  SDA=D4/GPIO5   SCL=D5/GPIO6",  5, 6},
  {"J6  SDA=D2/GPIO3   SCL=D0/GPIO1",  3, 1},
  {"J5  SDA=D14/GPIO41 SCL=D13/GPIO40", 41, 40},
  {"J5  SDA=D13/GPIO40 SCL=D14/GPIO41", 40, 41},
  {"J5  SDA=D13/GPIO40 SCL=D3/GPIO4",  40, 4},
  {"J5  SDA=D3/GPIO4   SCL=D13/GPIO40", 4, 40},
};
static const int NPAIRS = sizeof(PAIRS) / sizeof(PAIRS[0]);

static int busSel = -1;
static uint32_t okC = 0, nakC = 0;
static uint8_t fb[1024];
static const uint32_t NEVER = 0xFFFFFFFFu;

static uint32_t riseCycles(uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(300);
  pinMode(pin, INPUT_PULLDOWN);
  uint32_t c0 = esp_cpu_get_cycle_count();
  uint32_t lim = c0 + 240000;
  while (digitalRead(pin) == 0 && esp_cpu_get_cycle_count() < lim) {}
  uint32_t c = esp_cpu_get_cycle_count();
  if (digitalRead(pin) == 0) return NEVER;
  return c - c0;
}

static void busRecover() {
  Wire.end();
  pinMode(PAIRS[busSel].sda, INPUT_PULLUP);
  pinMode(PAIRS[busSel].scl, INPUT_PULLUP);
  delay(2);
  bool stuck = !digitalRead(PAIRS[busSel].sda) || !digitalRead(PAIRS[busSel].scl);
  if (stuck) {
    pinMode(PAIRS[busSel].scl, OUTPUT);
    for (int i = 0; i < 9; i++) {
      digitalWrite(PAIRS[busSel].scl, LOW);  delayMicroseconds(5);
      digitalWrite(PAIRS[busSel].scl, HIGH); delayMicroseconds(5);
    }
    pinMode(PAIRS[busSel].sda, OUTPUT);
    digitalWrite(PAIRS[busSel].sda, LOW);  delayMicroseconds(5);
    digitalWrite(PAIRS[busSel].scl, HIGH); delayMicroseconds(5);
    digitalWrite(PAIRS[busSel].sda, HIGH); delayMicroseconds(5);
    pinMode(PAIRS[busSel].sda, INPUT_PULLUP);
  }
  Wire.begin(PAIRS[busSel].sda, PAIRS[busSel].scl);
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

static int findPanel() {
  Serial.println(F("\n--- scanning every pair @100 kHz ---"));
  for (int i = 0; i < NPAIRS; i++) {
    Wire.end();
    delay(3);
    Wire.begin(PAIRS[i].sda, PAIRS[i].scl);
    Wire.setClock(100000);
    delay(6);
    int count = 0;
    bool oled = false;
    String found = "";
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        count++;
        if (a == OLED_ADDR) oled = true;
        if (count <= 4) { found += " 0x"; found += String(a, HEX); }
      }
    }
    Serial.printf("  %-36s %2d ACK%s%s\n", PAIRS[i].label, count,
                  found.length() ? found.c_str() : "  -",
                  count > 20 ? "  (floating bus / ghosts)" : "");
    if (oled && count <= 20) {
      Wire.end();
      Serial.printf("     -> panel found here\n");
      return i;
    }
  }
  return -1;
}

static void testPair(uint32_t hz) {
  busRecover();
  Wire.setClock(hz);
  delay(10);
  okC = nakC = 0;
  initSSD1306();
  uint32_t iOk = okC, iNak = nakC;
  w1(0xA5);                       // hardware all-on
  Serial.printf("  @%luk init %lu ok/%lu nak | 0xA5 %s\n",
                (unsigned long)(hz / 1000), (unsigned long)iOk, (unsigned long)iNak,
                nakC == iNak ? "clean" : "LOST");
  delay(8000);
  w1(0xA4); w1(0xAF);
  okC = nakC = 0;
  pushFrame(0xFF);
  Serial.printf("  @%luk frame %lu ok/%lu nak  (2 s on / 2 s off)\n",
                (unsigned long)(hz / 1000), (unsigned long)okC, (unsigned long)nakC);
  pushFrame(0x00);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== display_pairs: scan J2 / J6 / J5, then drive the panel ==="));

  busSel = findPanel();
  if (busSel < 0) {
    Serial.println(F("*** no panel on any pair - wires not making contact ***"));
    return;
  }
  Serial.printf("*** using %s ***\n", PAIRS[busSel].label);

  uint32_t ra = riseCycles(PAIRS[busSel].sda);
  uint32_t rb = riseCycles(PAIRS[busSel].scl);
  Serial.printf("  rise SDA: %s\n", ra == NEVER ? "NEVER" : (String(ra / 240.0f, 2) + " us").c_str());
  Serial.printf("  rise SCL: %s\n", rb == NEVER ? "NEVER" : (String(rb / 240.0f, 2) + " us").c_str());

  testPair(100000);
  testPair(400000);
}

void loop() {
  static uint32_t lastFind = 0;

  if (busSel < 0) {
    if (millis() - lastFind > 8000) {
      lastFind = millis();
      busSel = findPanel();
      if (busSel >= 0) {
        Serial.printf("*** using %s ***\n", PAIRS[busSel].label);
        testPair(100000);
        testPair(400000);
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
  if (nakC > 8) Serial.printf("  flash round: %lu nak (bus unhealthy)\n", (unsigned long)nakC);

  if (millis() - lastFind > 30000) {
    lastFind = millis();
    int keep = busSel;
    int f = findPanel();
    if (f >= 0 && f != keep) {
      busSel = f;
      Serial.printf("*** panel moved to %s ***\n", PAIRS[busSel].label);
      testPair(100000);
      testPair(400000);
    } else if (f < 0) {
      Serial.println(F("  panel not found anywhere"));
    }
    Wire.end(); delay(3);
    Wire.begin(PAIRS[keep].sda, PAIRS[keep].scl);
    Wire.setClock(400000);
    delay(10);
    initSSD1306();
  }
}
