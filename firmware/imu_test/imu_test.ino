#include <Wire.h>
#include <esp_cpu.h>

// imu_test - MPU6050 on D4/D5 (SDA=GPIO5, SCL=GPIO6), VCC=3.3 V, GND.
//
// Step 1: is anything there?  full scan + targeted probe of 0x68 / 0x69.
// Step 2: identity.           WHO_AM_I (0x75): 0x68 MPU6050, 0x70 MPU6500,
//                             0x72 MPU9250 family, 0x98 some clones.
// Step 3: wake + configure.   PWR_MGMT_1 = 0 clears the sleep bit.
// Step 4: live data.          14 bytes from 0x3B: accel xyz, temp, gyro xyz.
//
// If the IMU is absent the sketch retries every 3 s (so you can rewire live)
// and prints whether 0x18/0x2C still answer, which tells us the bus itself is
// healthy independently of the sensor.
//
// D4/D5 is the board's shared bus: 0x18 (TLV320 codec) + 0x2C (XMOS) live
// there too. This sketch only ever writes to the IMU address.

#define SDA_PIN 5
#define SCL_PIN 6

#define REG_SMPLRT_DIV   0x19
#define REG_CONFIG       0x1A
#define REG_GYRO_CONFIG  0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1   0x6B
#define REG_WHO_AM_I     0x75

static uint8_t addr = 0;
static bool ready = false;
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
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  delay(2);
  bool stuck = !digitalRead(SDA_PIN) || !digitalRead(SCL_PIN);
  if (stuck) {
    pinMode(SCL_PIN, OUTPUT);
    for (int i = 0; i < 9; i++) {
      digitalWrite(SCL_PIN, LOW);  delayMicroseconds(5);
      digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    }
    pinMode(SDA_PIN, OUTPUT);
    digitalWrite(SDA_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    digitalWrite(SDA_PIN, HIGH); delayMicroseconds(5);
    pinMode(SDA_PIN, INPUT_PULLUP);
  }
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(2);
  if (millis() < 2000) Serial.printf("bus recovery: stuck=%d\n", stuck);
}

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// one read attempt. repeated=true -> Sr (combined), false -> STOP then START.
static bool rdOnce(uint8_t reg, uint8_t *buf, uint8_t n, bool repeated) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(repeated) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// both transaction styles, a few times each: this bus loses bytes at 100 kHz
static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (rdOnce(reg, buf, n, true)) return true;
    delay(2);
    if (rdOnce(reg, buf, n, false)) return true;
    delay(2);
  }
  return false;
}

// finds a clock + transaction style that actually reads WHO_AM_I
static bool findWorkingRead() {
  static const uint32_t speeds[3] = {100000, 50000, 20000};
  uint8_t who = 0;
  for (int s = 0; s < 3; s++) {
    Wire.setClock(speeds[s]);
    delay(10);
    int okSr = 0, okStop = 0;
    for (int i = 0; i < 5; i++) {
      if (rdOnce(REG_WHO_AM_I, &who, 1, true)) okSr++;
      if (rdOnce(REG_WHO_AM_I, &who, 1, false)) okStop++;
      delay(2);
    }
    Serial.printf("  @%3luk  Sr %d/5  STOP+START %d/5  WHO_AM_I=0x%02X\n",
                  (unsigned long)(speeds[s] / 1000), okSr, okStop, who);
    if (okSr || okStop) {
      Serial.printf("  -> using %luk with %s\n", (unsigned long)(speeds[s] / 1000),
                    okSr >= okStop ? "repeated start" : "STOP+START");
      return true;
    }
  }
  return false;
}

static void scanBus() {
  Serial.println(F("--- step 1: scan ---"));
  uint8_t n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      n++;
      const char *tag = "";
      if (a == 0x68 || a == 0x69) tag = "  <- MPU6050 address";
      else if (a == 0x18) tag = "  <- TLV320 codec (board audio)";
      else if (a == 0x2C) tag = "  <- XMOS control (XVF3800)";
      Serial.printf("  0x%02X ACK%s\n", a, tag);
    }
  }
  if (!n) Serial.println(F("  (nothing ACKs at all - bus problem, not just the IMU)"));
}

// returns true if an IMU answered
static bool detect() {
  addr = 0;
  for (uint8_t a = 0x68; a <= 0x69; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X responds\n", a);
      if (!addr) addr = a;
    }
  }
  return addr != 0;
}

static bool startImu() {
  Serial.println(F("  read test: WHO_AM_I at 3 clocks x 2 transaction styles"));
  if (!findWorkingRead()) {
    Serial.println(F("  every read style failed - the device ACKs its address but"));
    Serial.println(F("  never returns data (writes land, reads do not)"));
    return false;
  }

  uint8_t who = 0;
  if (!readRegs(REG_WHO_AM_I, &who, 1)) {
    Serial.println(F("  WHO_AM_I read FAILED even after calibration"));
    return false;
  }
  Serial.printf("  WHO_AM_I = 0x%02X  (%s)\n", who,
                who == 0x68 ? "MPU6050 (or a clone reporting 0x68)" :
                who == 0x70 ? "MPU6500" :
                who == 0x72 ? "MPU9250/9255 family" :
                who == 0x98 ? "clone reporting 0x98" : "unexpected value");

  uint8_t pwr = 0xFF;
  readRegs(REG_PWR_MGMT_1, &pwr, 1);
  Serial.printf("  PWR_MGMT_1 before = 0x%02X (sleep bit = %d)\n", pwr, !!(pwr & 0x40));

  writeReg(REG_PWR_MGMT_1, 0x00);
  delay(80);
  uint8_t pwr2 = 0xFF;
  readRegs(REG_PWR_MGMT_1, &pwr2, 1);
  Serial.printf("  PWR_MGMT_1 after  = 0x%02X\n", pwr2);
  if (pwr2 & 0x40) {
    Serial.println(F("  still asleep - write did not stick"));
    return false;
  }

  writeReg(REG_SMPLRT_DIV, 9);      // 100 Hz
  writeReg(REG_CONFIG, 3);          // DLPF ~44 Hz
  writeReg(REG_GYRO_CONFIG, 0x00);  // +-250 dps
  writeReg(REG_ACCEL_CONFIG, 0x00); // +-2 g
  Serial.println(F("  100 Hz, gyro +-250 dps, accel +-2 g"));
  Serial.println(F("--- step 4: live data (1 g at rest, temp ~30 C) ---"));
  Serial.println(F("       ax      ay      az     |g|    temp      gx      gy      gz"));
  return true;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Serial.println();
  Serial.println(F("=== imu_test: MPU6050 on D4/D5 (GPIO5/6), VCC 3.3 V ==="));

  busRecover();
  Wire.setClock(100000);
  delay(20);

  uint32_t rs = riseCycles(SDA_PIN);
  uint32_t rc = riseCycles(SCL_PIN);
  Serial.printf("rise SDA: %s | rise SCL: %s\n",
                rs == NEVER ? "NEVER" : (String(rs / 240.0f, 2) + " us").c_str(),
                rc == NEVER ? "NEVER" : (String(rc / 240.0f, 2) + " us").c_str());
  Serial.println(F("(pull-ups present = rises; NEVER = nothing pulling that line up)"));
  Serial.println();

  // riseCycles() leaves the pins as plain GPIO with pull-down, so re-attach
  // the I2C peripheral before any traffic.
  busRecover();
  Wire.setClock(100000);
  delay(20);

  scanBus();

  Serial.println(F("--- step 1b: MPU detect ---"));
  if (!detect()) {
    Serial.println(F("*** MPU6050 NOT DETECTED at 0x68/0x69 ***"));
    Serial.println(F("check VCC=3.3V, GND, SDA->D4, SCL->D5, and AD0 not floating"));
    Serial.println(F("retrying every 3 s - you can rewire while it runs"));
    return;
  }
  Serial.printf("*** device found at 0x%02X ***\n", addr);
  Serial.println(F("--- step 2/3: identity + wake ---"));
  ready = startImu();
  if (!ready) Serial.println(F("IMU present but not usable - see above"));
}

void loop() {
  static uint32_t last = 0;

  if (!ready) {
    if (millis() - last < 3000) return;
    last = millis();
    busRecover();
    Wire.setClock(100000);
    delay(10);
    uint8_t n = 0;
    for (uint8_t a = 0x68; a <= 0x69; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) n++;
    }
    if (n) {
      Serial.printf("[%lus] IMU appeared\n", (unsigned long)(millis() / 1000));
      if (!detect()) return;
      ready = startImu();
    } else {
      Wire.beginTransmission(0x18);
      bool codec = Wire.endTransmission() == 0;
      Wire.beginTransmission(0x2C);
      bool xmos = Wire.endTransmission() == 0;
      Serial.printf("[%lus] no IMU. bus: codec 0x18 %s, XMOS 0x2C %s\n",
                    (unsigned long)(millis() / 1000),
                    codec ? "ACK (bus is alive)" : "silent (bus wedged)",
                    xmos ? "ACK" : "silent");
    }
    return;
  }

  if (millis() - last < 200) return;
  last = millis();

  uint8_t b[14];
  if (!readRegs(REG_ACCEL_XOUT_H, b, 14)) {
    Serial.println(F("  read FAILED"));
    ready = false;
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
  if (!warned) {
    if (ax == 0 && ay == 0 && az == 0 && gx == 0 && gy == 0 && gz == 0) {
      Serial.println(F("  ALL ZEROS - device ACKs but is not converting (I2C fine, sensor dead)"));
      warned = true;
    } else if (mag < 0.5f || mag > 1.5f) {
      Serial.println(F("  note: |g| should be ~1.00 while sitting still"));
      warned = true;
    }
  }
}
