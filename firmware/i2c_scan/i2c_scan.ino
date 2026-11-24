#include <Wire.h>

// Probe v9 - FULL free-pad matrix, any-device finder (KiCad netlist, authoritative):
//   J6 (1x4): pin1=GND, pin2=D2/GPIO3, pin3=D1/GPIO2 (XMOS reset control - NEVER use), pin4=D0/GPIO1
//   J5 (1x4): pin1=GND, pin2=D14/GPIO41, pin3=D13/GPIO40, pin4=D3/GPIO4
// Every legal pair on both headers is scanned in both orders @10 kHz (most forgiving
// clock), forever (~5 s rounds). Control-bus check (5,6 -> 0x18 codec + 0x2C XMOS)
// first each round proves the scanner works. No board pull-ups on these pads.

const char *who(uint8_t a) {
  switch (a) {
    case 0x18: case 0x19: return "LIS3DH/LIS2DH accel (or BMA400)";
    case 0x1C: case 0x1D: return "MMA8451 or ADXL343/345 (SDO high)";
    case 0x53: return "ADXL345/ADXL343 (SDO low)";
    case 0x68: case 0x69: return "MPU6050/6500/9250";
    case 0x6B: return "LSM6DS3/QMI8658";
    case 0x1E: return "LSM303/HMC5883 (compass half)";
    default: return "";
  }
}

void scanBus(uint8_t sda, uint8_t scl, uint32_t hz, const char *label) {
  Wire.end();
  delay(3);
  Wire.begin((int)sda, (int)scl);
  Wire.setClock(hz);
  delay(3);
  Serial.printf("=== %s (SDA=GPIO%u SCL=GPIO%u) @%luk ===\n", label, sda, scl, (unsigned long)(hz / 1000));
  uint8_t found = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      const char *w = who(a);
      if (w[0]) Serial.printf("  0x%02X ACK  <- %s\n", a, w);
      else      Serial.printf("  0x%02X ACK\n", a);
      found++;
    }
  }
  if (!found) Serial.println(F("  (nothing ACKs)"));
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println("I2C probe v9 - full J5+J6 free-pad matrix, ~5 s rounds");
}

void loop() {
  scanBus(5, 6, 50000, "control: codec bus");
  scanBus(3, 1, 10000, "J6: SDA=D2 SCL=D0");
  scanBus(1, 3, 10000, "J6: SDA=D0 SCL=D2");
  scanBus(41, 40, 10000, "J5: SDA=D14 SCL=D13");
  scanBus(40, 41, 10000, "J5: SDA=D13 SCL=D14");
  scanBus(41, 4, 10000, "J5: SDA=D14 SCL=D3");
  scanBus(4, 41, 10000, "J5: SDA=D3 SCL=D14");
  scanBus(40, 4, 10000, "J5: SDA=D13 SCL=D3");
  scanBus(4, 40, 10000, "J5: SDA=D3 SCL=D13");
  Serial.println(F("--- next in 2 s ---"));
  delay(2000);
}
