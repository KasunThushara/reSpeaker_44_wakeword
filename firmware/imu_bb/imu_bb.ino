#include <Arduino.h>

// imu_bb - MPU6050 on D4/D5, software I2C, BOTH pin orders.
//
// Status 2026-11-19:
//   * board bus is healthy: 0x18 (codec) and 0x2C (XMOS) ACK every second
//   * MPU6050 never answers at 0x68 or 0x69
// The single most common cause of that is SDA/SCL swapped at the module, so
// this sketch drives the pair in both orders (5,6) then (6,5). I2C is
// open-drain, so trying the swapped order cannot damage anything.
//
// It also prints the idle levels and the two on-board devices every second, so
// "module silent" is instantly distinguishable from "bus broken".
//
// Wiring: VCC=3.3 V, GND, SDA=D4/GPIO5, SCL=D5/GPIO6, MPU6050 at 0x68.

#define PIN_A 5    // D4
#define PIN_B 6    // D5

#define REG_SMPLRT_DIV   0x19
#define REG_CONFIG       0x1A
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1   0x6B
#define REG_WHO_AM_I     0x75

static uint8_t pSDA = PIN_A, pSCL = PIN_B;
static uint8_t addr = 0;
static bool ready = false;
static const int T = 10;          // ~50 kHz

static void d(int us) { delayMicroseconds(us); }
static void sdaLow()     { pinMode(pSDA, OUTPUT); digitalWrite(pSDA, LOW); }
static void sdaRelease() { pinMode(pSDA, INPUT_PULLUP); }
static void sclLow()     { pinMode(pSCL, OUTPUT); digitalWrite(pSCL, LOW); }
static void sclRelease() { pinMode(pSCL, INPUT_PULLUP); }

static void i2cStart() { sdaRelease(); sclRelease(); d(T); sdaLow(); d(T); sclLow(); d(T); }
static void i2cStop()  { sdaLow(); d(T); sclRelease(); d(T); sdaRelease(); d(T); }

static bool i2cWrite(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if (b & (1 << i)) sdaRelease(); else sdaLow();
    d(T); sclRelease(); d(T); sclLow(); d(T);
  }
  sdaRelease(); d(T);
  sclRelease(); d(T);
  bool ack = (digitalRead(pSDA) == 0);
  sclLow(); d(T);
  return ack;
}

static uint8_t i2cRead(bool ackBit) {
  uint8_t b = 0;
  sdaRelease();
  for (int i = 0; i < 8; i++) {
    d(T); sclRelease(); d(T);
    b = (uint8_t)((b << 1) | (digitalRead(pSDA) ? 1 : 0));
    sclLow(); d(T);
  }
  if (ackBit) sdaLow(); else sdaRelease();
  d(T); sclRelease(); d(T); sclLow(); d(T);
  sdaRelease();
  return b;
}

static bool probe(uint8_t a) {
  i2cStart();
  bool ok = i2cWrite((uint8_t)(a << 1));
  i2cStop();
  return ok;
}

static bool writeReg(uint8_t reg, uint8_t val) {
  i2cStart();
  bool ok = i2cWrite((uint8_t)(addr << 1)) && i2cWrite(reg) && i2cWrite(val);
  i2cStop();
  return ok;
}

static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  i2cStart();
  if (!i2cWrite((uint8_t)(addr << 1))) { i2cStop(); return false; }
  if (!i2cWrite(reg)) { i2cStop(); return false; }
  i2cStart();
  if (!i2cWrite((uint8_t)((addr << 1) | 1))) { i2cStop(); return false; }
  for (uint8_t i = 0; i < n; i++) buf[i] = i2cRead(i < (uint8_t)(n - 1));
  i2cStop();
  return true;
}

// the module only answers ~1 in 8 right now, so retry before giving up
static bool readRegsRetry(uint8_t reg, uint8_t *buf, uint8_t n, int tries) {
  for (int i = 0; i < tries; i++) {
    if (readRegs(reg, buf, n)) return true;
    delay(2);
  }
  return false;
}

static void useOrder(uint8_t sda, uint8_t scl) {
  // release everything before switching pins
  pinMode(pSDA, INPUT);
  pinMode(pSCL, INPUT);
  pSDA = sda;
  pSCL = scl;
  pinMode(pSDA, INPUT_PULLUP);
  pinMode(pSCL, INPUT_PULLUP);
  delay(2);
}

// tries SDA=D4/SCL=D5 and the swapped order; true if an IMU answered
static bool findImu(bool verbose) {
  static const uint8_t order[2][2] = {{PIN_A, PIN_B}, {PIN_B, PIN_A}};
  for (int o = 0; o < 2; o++) {
    useOrder(order[o][0], order[o][1]);
    // 8 probes: 0/8 = absent, 1-7/8 = intermittent CONTACT, 8/8 = solid
    uint8_t found = 0;
    int acks = 0;
    for (int i = 0; i < 4; i++) {
      for (uint8_t a = 0x68; a <= 0x69; a++) {
        if (probe(a)) { acks++; found = a; }
      }
      delay(2);
    }
    if (verbose) {
      bool codec = probe(0x18), xmos = probe(0x2C);
      Serial.printf("[%3lus] SDA=D%d SCL=D%d | 0x18 %s 0x2C %s | IMU %d/8 ACK%s | idle %d/%d\n",
                    (unsigned long)(millis() / 1000),
                    order[o][0] == PIN_A ? 4 : 5, order[o][1] == PIN_A ? 4 : 5,
                    codec ? "ACK" : "---", xmos ? "ACK" : "---",
                    acks,
                    acks > 0 && acks < 8 ? "  <- FLAKY CONTACT" : "",
                    digitalRead(order[o][0]), digitalRead(order[o][1]));
    }
    if (found) {
      if (acks < 8) {
        Serial.println(F("  !!! the IMU answers only sometimes: that is a bad connection"));
        Serial.println(F("  !!! (loose dupont on a 2.0 mm pin, or a cold joint), not a pin error"));
      }
      if (order[o][0] != PIN_A) {
        Serial.println(F("!!! IMU only answers with SDA/SCL SWAPPED - your two signal"));
        Serial.println(F("!!! wires are crossed at the module or the header"));
      }
      addr = found;
      return true;
    }
  }
  return false;
}

static bool initImu() {
  uint8_t who = 0;
  Serial.println(F("--- WHO_AM_I ---"));
  if (!readRegsRetry(REG_WHO_AM_I, &who, 1, 20)) {
    Serial.println(F("  read FAILED 20 times (ACKs the address but returns no data)"));
    return false;
  }
  Serial.printf("  WHO_AM_I = 0x%02X  (%s)\n", who,
                who == 0x68 ? "MPU6050 (or clone)" :
                who == 0x70 ? "MPU6500" :
                who == 0x72 ? "MPU9250 family" :
                who == 0x98 ? "clone 0x98" : "unexpected");

  uint8_t pwr = 0xFF;
  readRegsRetry(REG_PWR_MGMT_1, &pwr, 1, 10);
  bool w = writeReg(REG_PWR_MGMT_1, 0x00);
  delay(80);
  uint8_t pwr2 = 0xFF;
  readRegsRetry(REG_PWR_MGMT_1, &pwr2, 1, 10);
  Serial.printf("  PWR_MGMT_1 0x%02X -> %s -> 0x%02X\n", pwr, w ? "ok" : "FAILED", pwr2);

  writeReg(REG_SMPLRT_DIV, 9);
  writeReg(REG_CONFIG, 3);
  writeReg(REG_GYRO_CONFIG, 0x00);
  writeReg(REG_ACCEL_CONFIG, 0x00);
  Serial.println(F("  100 Hz, +-250 dps, +-2 g"));
  Serial.println(F("       ax      ay      az     |g|    temp      gx      gy      gz"));
  return !(pwr2 & 0x40);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  useOrder(PIN_A, PIN_B);
  delay(50);

  Serial.println();
  Serial.println(F("=== imu_bb: MPU6050 on D4/D5, software I2C, both pin orders ==="));
  Serial.printf("idle levels: D4(SDA)=%d D5(SCL)=%d (both must be 1)\n",
                digitalRead(PIN_A), digitalRead(PIN_B));
  Serial.println(F("--- full scan in the normal order ---"));
  uint8_t n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (probe(a)) {
      n++;
      const char *tag = "";
      if (a == 0x68 || a == 0x69) tag = "  <- MPU6050";
      else if (a == 0x18) tag = "  <- TLV320 codec";
      else if (a == 0x2C) tag = "  <- XMOS";
      Serial.printf("  0x%02X ACK%s\n", a, tag);
    }
  }
  if (!n) Serial.println(F("  (nothing at all)"));

  Serial.println(F("--- IMU detect (both orders) ---"));
  if (findImu(true)) {
    Serial.printf("*** IMU found at 0x%02X ***\n", addr);
    ready = initImu();
    if (!ready) addr = 0;
  } else {
    Serial.println(F("*** MPU6050 NOT DETECTED in either pin order ***"));
    Serial.println(F("check: VCC=3.3V at the module, GND, and that the module's power LED is on"));
    Serial.println(F("watching live - reseat the wires and it will pick up automatically"));
  }
}

static uint32_t lastScan = 0, lastRead = 0;

void loop() {
  if (!ready) {
    if (millis() - lastScan < 1000) return;
    lastScan = millis();
    if (findImu(true)) {
      Serial.printf("*** IMU appeared at 0x%02X ***\n", addr);
      ready = initImu();
      if (!ready) addr = 0;
    }
    return;
  }

  if (millis() - lastRead < 200) return;
  lastRead = millis();

  uint8_t b[14];
  if (!readRegsRetry(REG_ACCEL_XOUT_H, b, 14, 3)) {
    Serial.println(F("  read FAILED - back to probing"));
    ready = false;
    addr = 0;
    return;
  }

  int16_t ax = (int16_t)(b[0] << 8 | b[1]);
  int16_t ay = (int16_t)(b[2] << 8 | b[3]);
  int16_t az = (int16_t)(b[4] << 8 | b[5]);
  int16_t tr = (int16_t)(b[6] << 8 | b[7]);
  int16_t gx = (int16_t)(b[8] << 8 | b[9]);
  int16_t gy = (int16_t)(b[10] << 8 | b[11]);
  int16_t gz = (int16_t)(b[12] << 8 | b[13]);

  float fax = ax / 16384.0f, fay = ay / 16384.0f, faz = az / 16384.0f;
  float mag = sqrtf(fax * fax + fay * fay + faz * faz);
  float tc = tr / 340.0f + 36.53f;

  Serial.printf("  %6d %6d %6d  %5.2fg %5.1fC %7d %7d %7d\n",
                ax, ay, az, mag, tc, gx, gy, gz);

  static bool warned = false;
  if (!warned && !ax && !ay && !az && !gx && !gy && !gz) {
    Serial.println(F("  ALL ZEROS - ACKs but not converting (I2C fine, sensor idle/dead)"));
    warned = true;
  }
}
