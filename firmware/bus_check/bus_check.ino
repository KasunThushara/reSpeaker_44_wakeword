#include <Wire.h>

// Bus check v2 - minimal: Wire.begin once, no end()/begin() churn, and a proper
// I2C bus recovery (9 clocks + STOP) before scanning, because the previous run
// wedged after a while: 3 devices ACKed, then the bus went dead at every clock.
//
// Suspect: this is the XVF3800 control bus. Codec 0x18 and XMOS 0x2C live here,
// so a bus that locks under sustained display traffic means contention or a
// marginal line, not a pin error.

#define SDA 5
#define SCL 6

void recoverBus(const char *why) {
  Wire.end();
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(1);
  bool stuck = (digitalRead(SDA) == 0) || (digitalRead(SCL) == 0);

  if (stuck) {
    // clock the slave free
    pinMode(SCL, OUTPUT);
    pinMode(SDA, INPUT_PULLUP);
    for (int i = 0; i < 9; i++) {
      digitalWrite(SCL, LOW);  delayMicroseconds(5);
      digitalWrite(SCL, HIGH); delayMicroseconds(5);
    }
    // STOP condition: SDA low -> high while SCL high
    pinMode(SDA, OUTPUT);
    digitalWrite(SDA, LOW);  delayMicroseconds(5);
    digitalWrite(SCL, HIGH); delayMicroseconds(5);
    digitalWrite(SDA, HIGH); delayMicroseconds(5);
    pinMode(SDA, INPUT_PULLUP);
  }

  Wire.begin(SDA, SCL);
  delay(2);
  Serial.printf("recover (%s): pre-recovery stuck=%d, now SDA=%d SCL=%d\n",
                why, stuck, digitalRead(SDA), digitalRead(SCL));
}

void scanAt(uint32_t hz, int rounds) {
  Wire.setClock(hz);
  delay(2);
  for (int r = 0; r < rounds; r++) {
    int n = 0, timeouts = 0;
    uint8_t list[8];
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      uint8_t e = Wire.endTransmission();
      if (e == 0 && n < 8) list[n++] = a;
      else if (e == 5) timeouts++;
    }
    Serial.printf("@%3lukHz r%d: %d ack%s", (unsigned long)(hz / 1000), r + 1, n, n ? "" : " (none)");
    if (timeouts) Serial.printf("  timeouts=%d", timeouts);
    for (int i = 0; i < n; i++) Serial.printf("  0x%02X", list[i]);
    Serial.println();
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== bus check v2: GPIO5/6, expect 0x18 codec, 0x2C xmos, 0x3C display ==="));

  recoverBus("startup");
  scanAt(100000, 3);
  scanAt(200000, 3);
  scanAt(400000, 5);

  // sustained-traffic test at 400 kHz: 300 frames of 40 writes to 0x3C
  Wire.setClock(400000);
  int ok = 0, nak = 0, to = 0;
  uint32_t t0b = micros();
  for (int f = 0; f < 300; f++) {
    for (int k = 0; k < 40; k++) {
      Wire.beginTransmission(0x3C);
      Wire.write((uint8_t)(f + k));
      uint8_t e = Wire.endTransmission();
      if (e == 0) ok++; else if (e == 5) to++; else nak++;
    }
  }
  uint32_t ms = (micros() - t0b) / 1000;
  Serial.printf("stress 400kHz: %d ok, %d nak, %d timeout in %lu ms\n", ok, nak, to, (unsigned long)ms);
  Serial.println(F("=== done; repeating every 20 s ==="));
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 20000) return;
  last = millis();
  Serial.println(F("--- repeat ---"));
  scanAt(400000, 3);
  recoverBus("periodic");
  scanAt(100000, 2);
}
