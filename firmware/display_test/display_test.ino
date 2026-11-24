#include <Wire.h>

// Chunk-size sweep for the SSD1306 at 0x3C (GPIO5/6, 400 kHz).
//
// Finding so far: failures follow chunk LENGTH, not position - short final
// chunks never failed, 30-byte chunks failed ~17-20%. This sweep measures
// 8/16/24/32-byte chunks with and without a small delay and picks a winner.

#define SDA_PIN 5
#define SCL_PIN 6
#define OLED_ADDR 0x3C

static uint8_t fb[1024];
static uint32_t fails = 0, writes = 0;

void cmd2b(uint8_t c, uint8_t a) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(c); Wire.write(a);
  if (Wire.endTransmission()) fails++;
  writes++;
}

void cmd1(uint8_t c) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(c);
  if (Wire.endTransmission()) fails++;
  writes++;
}

void pushFrame(uint8_t chunk, uint32_t gapUs) {
  cmd2b(0x21, 0x00); cmd2b(0x21, 127);
  cmd2b(0x22, 0x00); cmd2b(0x22, 7);
  for (uint8_t p = 0; p < 8; p++) {
    for (uint8_t off = 0; off < 128; off += chunk) {
      uint8_t n = (128 - off < chunk) ? (128 - off) : chunk;
      Wire.beginTransmission(OLED_ADDR);
      Wire.write(0x40);
      Wire.write(&fb[p * 128 + off], n);
      if (Wire.endTransmission()) fails++;
      writes++;
      if (gapUs) delayMicroseconds(gapUs);
    }
  }
}

void pattern(int f) {
  memset(fb, 0, sizeof(fb));
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 20; x++)
      fb[(y >> 3) * 128 + ((f * 2 + x) % 128)] |= 1 << (y & 7);
}

void run(const char *label, uint8_t chunk, uint32_t gapUs, int frames) {
  fails = writes = 0;
  uint32_t t0 = micros();
  for (int f = 0; f < frames; f++) { pattern(f); pushFrame(chunk, gapUs); }
  uint32_t ms = (micros() - t0) / 1000;
  Serial.printf("%-22s chunk %2u gap %4luus: %5.2f ms/frame = %5.1f fps, %lu/%lu fail (%.1f%%)\n",
                label, chunk, (unsigned long)gapUs, (float)ms / frames,
                frames * 1000.0f / ms,
                (unsigned long)fails, (unsigned long)writes,
                100.0f * fails / writes);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== chunk sweep: SSD1306 128x64 @0x3C, GPIO5/6, 400 kHz ==="));

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  delay(20);

  Wire.beginTransmission(OLED_ADDR); Wire.write((uint8_t)0x00); Wire.write((uint8_t)0xAE);
  Serial.printf("panel present: %s\n", Wire.endTransmission() == 0 ? "ACK" : "NAK");
  cmd2b(0x8D, 0x14); cmd2b(0x20, 0x00); cmd1(0xAF);
  delay(50);

  run("baseline", 30, 0, 60);
  run("short chunks", 8, 0, 60);
  run("short chunks", 16, 0, 60);
  run("short chunks", 24, 0, 60);
  run("short chunks", 32, 0, 60);
  run("16 + gap", 16, 100, 60);
  run("16 + gap", 16, 200, 60);
  run("8 + gap", 8, 100, 60);
  run("8 + gap", 8, 200, 60);

  Serial.println(F("=== steady animation, chunk 16, no gap ==="));
}

void loop() {
  static int f = 0;
  pattern(f++);
  pushFrame(16, 0);
  delay(20);
}
