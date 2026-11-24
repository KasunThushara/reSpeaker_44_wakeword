#include <Wire.h>
#include <esp_cpu.h>

// presence_check v2 - is the OLED module ELECTRICALLY on the bus at all?
//
// Why: the 0x3C "ACK" is suspect. On this ESP32 an empty bus ghost-ACKs all
// 112 addresses at 100 kHz, and the J2 scan sometimes returns exactly that
// pattern, then 0x18+0x2C+0x3C again 30 s later. A real powered display does
// not come and go.
//
// Method (no ADC - analogRead() reconfigures the pin and kills the pull):
//   drive the line LOW, release into INPUT_PULLDOWN (~45k to GND) and count CPU
//   cycles until it reads HIGH. Any external pull-up has to fight that 45k.
//     module with 4.7k/10k pull-ups -> rises in ~0.3-1 us  (72-240 cycles)
//     nothing attached              -> never rises (pull-down wins)
//   Empty pads (J6, J5) are the control group and must read "never".
//
// Neither I2C line of a 4-pin OLED module is ever externally pulled down, so a
// fast rise on BOTH SDA and SCL proves the module is there and powered. A
// "never" on J2 means the wiring or the VCC feed is the fault, and 0x3C was a
// ghost all along.

struct Pair { const char *label; uint8_t a; uint8_t b; };
static const Pair PAIRS[4] = {
  {"J2  D4/D5   (panel is supposed to be here)", 5, 6},
  {"J6  D2/D0   (empty control)",                3, 1},
  {"J5  D14/D13 (empty control)",               41, 40},
  {"J5  D3/D14  (empty control)",                4, 41},
};

static const uint32_t NEVER = 0xFFFFFFFFu;

static uint32_t riseCycles(uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(300);
  pinMode(pin, INPUT_PULLDOWN);          // ~45k to GND, fights any pull-up
  uint32_t c0 = esp_cpu_get_cycle_count();
  uint32_t lim = c0 + 240000;            // ~1 ms at 240 MHz
  while (digitalRead(pin) == 0 && esp_cpu_get_cycle_count() < lim) {}
  uint32_t c = esp_cpu_get_cycle_count();
  if (digitalRead(pin) == 0) return NEVER;
  return c - c0;
}

static void probePair(const Pair &p) {
  uint32_t ra = riseCycles(p.a);
  uint32_t rb = riseCycles(p.b);
  Serial.printf("  %s\n", p.label);
  Serial.printf("    line A GPIO%-2u: %s\n", p.a, ra == NEVER ? "NEVER rises  -> no external pull-up" : "rises");
  if (ra != NEVER) Serial.printf("                     %lu cycles = %.2f us\n", (unsigned long)ra, ra / 240.0f);
  Serial.printf("    line B GPIO%-2u: %s\n", p.b, rb == NEVER ? "NEVER rises  -> no external pull-up" : "rises");
  if (rb != NEVER) Serial.printf("                     %lu cycles = %.2f us\n", (unsigned long)rb, rb / 240.0f);
  bool both = (ra != NEVER) && (rb != NEVER);
  Serial.printf("    => %s\n\n", both ? "EXTERNAL PULL-UPS ON BOTH LINES: a powered module is wired here"
                                        : "NOT a populated I2C bus");
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Wire.end();
  Serial.println();
  Serial.println(F("=== presence_check v2: rise time against an internal 45k pull-down ==="));
  for (int i = 0; i < 4; i++) probePair(PAIRS[i]);

  Serial.println(F("VERDICT KEY"));
  Serial.println(F("  J2 rises on both lines -> module present and powered"));
  Serial.println(F("                          -> 0x3C was real, the glass/driver is dead"));
  Serial.println(F("  J2 never rises         -> module NOT on the bus (wiring / VCC)"));
  Serial.println(F("                          -> 0x3C was a ghost ACK"));
  Serial.println(F("  control pairs must read NEVER; if a control rises, ignore the test"));

  // live I2C view for comparison with the rise test
  Wire.begin(5, 6);
  Wire.setClock(400000);
  delay(5);
  Serial.print(F("I2C scan on J2 @400 kHz:"));
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  if (!n) Serial.print(F(" (nothing)"));
  Serial.println();
  Serial.println(F("=== done; repeating every 15 s ==="));
}

void loop() {
  delay(15000);
  Serial.println(F("--- repeat ---"));
  Wire.end();
  for (int i = 0; i < 4; i++) probePair(PAIRS[i]);
}
