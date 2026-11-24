#include <Arduino.h>

// anim_bb - first animation on the panel, over software I2C.
//
// Why bit-bang: the module has no onboard pull-ups and J5 has none, so the bus
// runs on the ESP32's internal ~45k. Hardware Wire loses bytes above ~30 kHz
// (init 16/17, frames 0/68) but software I2C at 30 kHz was 60/60 and 1088/1088
// NAK-free. Two 4.7k resistors to 3.3 V would unlock 400 kHz and ~25 fps.
//
// Animation strategy: a full 128x64 frame is 1024 bytes (~0.9 s at 30 kHz), so
// only the changed rectangle is pushed. A 12 px ball moving 2 px/frame touches
// a ~16x16 aligned tile = 4 bytes = ~2 ms of bus time. That is why a tiny
// animation is smooth even on this crippled bus.
//
// SDA = D13/GPIO40, SCL = D14/GPIO41, VCC = TP15, GND = J5 pin 1.

#define SDA 40
#define SCL 41
#define OLED_ADDR 0x3C

static const int T = 12;          // us per clock phase (~40 kHz)
static uint32_t ackOk = 0, ackNak = 0;
static uint8_t fb[1024];          // 128x64, 8 pages of 128 bytes

static void d(int us) { delayMicroseconds(us); }
static void sdaLow()     { pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW); }
static void sdaRelease() { pinMode(SDA, INPUT_PULLUP); }
static void sclLow()     { pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW); }
static void sclRelease() { pinMode(SCL, INPUT_PULLUP); }

static void i2cStart() { sdaRelease(); sclRelease(); d(T); sdaLow(); d(T); sclLow(); d(T); }
static void i2cStop()  { sdaLow(); d(T); sclRelease(); d(T); sdaRelease(); d(T); }

static bool i2cWrite(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if (b & (1 << i)) sdaRelease(); else sdaLow();
    d(T); sclRelease(); d(T); sclLow(); d(T);
  }
  sdaRelease(); d(T);
  sclRelease(); d(T);
  bool ack = (digitalRead(SDA) == 0);
  sclLow(); d(T);
  if (ack) ackOk++; else ackNak++;
  return ack;
}

static int i2cSend(const uint8_t *buf, int len) {
  i2cStart();
  if (!i2cWrite((uint8_t)(OLED_ADDR << 1))) { i2cStop(); return 0; }
  int acked = 0;
  for (int i = 0; i < len; i++) if (i2cWrite(buf[i])) acked++;
  i2cStop();
  return acked;
}

static void cmd(uint8_t c) { uint8_t b[2] = {0x00, c}; i2cSend(b, 2); }
static void cmd2(uint8_t c, uint8_t a) { uint8_t b[3] = {0x00, c, a}; i2cSend(b, 3); }

static void initPanel() {
  cmd(0xAE);
  cmd2(0xD5, 0x80);
  cmd2(0xA8, 0x3F);
  cmd2(0xD3, 0x00);
  cmd(0x40);
  cmd2(0x8D, 0x14);
  cmd2(0x20, 0x00);
  cmd(0xA1);
  cmd(0xC8);
  cmd2(0xDA, 0x12);
  cmd2(0x81, 0xFF);
  cmd2(0xD9, 0xF1);
  cmd2(0xDB, 0x40);
  cmd(0xA4);
  cmd(0xA6);
  cmd(0x2E);
  cmd(0xAF);
}

// ---------- framebuffer drawing ----------
static void fbClear() { memset(fb, 0, sizeof(fb)); }
static void fbPixel(int x, int y, bool on) {
  if (x < 0 || x > 127 || y < 0 || y > 63) return;
  uint8_t m = 1 << (y & 7);
  if (on) fb[(y >> 3) * 128 + x] |= m;
  else    fb[(y >> 3) * 128 + x] &= ~m;
}
static void fbRect(int x, int y, int w, int h, bool on) {
  for (int i = 0; i < h; i++) { fbPixel(x, y + i, on); fbPixel(x + w - 1, y + i, on); }
  for (int i = 0; i < w; i++) { fbPixel(x + i, y, on); fbPixel(x + i, y + h - 1, on); }
}
static void fbDisc(int cx, int cy, int r, bool on) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r) fbPixel(cx + x, cy + y, on);
}
static void fbRing(int cx, int cy, int r, int thick) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      int d2 = x * x + y * y;
      if (d2 <= r * r && d2 >= (r - thick) * (r - thick)) fbPixel(cx + x, cy + y, true);
    }
}

// push an 8-aligned rectangle
static void pushRect(int x, int y, int w, int h) {
  x &= ~7; w = (w + 7) & ~7;
  y &= ~7; h = (h + 7) & ~7;
  if (x + w > 128) w = 128 - x;
  if (y + h > 64) h = 64 - y;
  cmd2(0x21, x); cmd2(0x21, x + w - 1);
  cmd2(0x22, y >> 3); cmd2(0x22, ((y + h - 1) >> 3));
  for (int p = y >> 3; p <= ((y + h - 1) >> 3); p++) {
    uint8_t buf[129];
    buf[0] = 0x40;
    memcpy(&buf[1], &fb[p * 128 + x], w);
    i2cSend(buf, w + 1);
  }
}

// ---------- animation ----------
struct Ball { int x, y, dx, dy; };
static Ball ball = {20, 20, 2, 1};

static void drawFrame() {
  fbClear();
  fbRect(0, 0, 128, 64, true);       // static border, drawn every time for simplicity
  fbDisc(ball.x + 6, ball.y + 6, 6, true);
  fbPixel(ball.x + 4, ball.y + 4, false);   // a little highlight
}

static void stepBall() {
  ball.x += ball.dx;
  ball.y += ball.dy;
  if (ball.x < 1 || ball.x > 128 - 13) { ball.dx = -ball.dx; ball.x += ball.dx; }
  if (ball.y < 1 || ball.y > 64 - 13)  { ball.dy = -ball.dy; ball.y += ball.dy; }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(50);

  Serial.println();
  Serial.println(F("=== anim_bb: ball animation over software I2C (D13=SDA D14=SCL) ==="));

  i2cStart();
  bool present = i2cWrite((uint8_t)(OLED_ADDR << 1));
  i2cStop();
  Serial.printf("probe 0x%02X: %s\n", OLED_ADDR, present ? "ACK" : "NAK");
  if (!present) { Serial.println(F("*** no panel - check wires ***")); return; }

  initPanel();
  Serial.println(F("init done"));

  // full frame once, so the border is on screen
  drawFrame();
  ackOk = ackNak = 0;
  uint32_t ms0 = millis();
  pushRect(0, 0, 128, 64);
  Serial.printf("first full frame: %lu ACKs %lu NAKs in %lu ms\n",
                (unsigned long)ackOk, (unsigned long)ackNak, (unsigned long)(millis() - ms0));
  Serial.println(F("=== animation running: a ball bouncing inside a border ==="));

  delay(500);
}

void loop() {
  static uint32_t frames = 0, fpsT = 0;
  int ox = ball.x, oy = ball.y;
  stepBall();

  // clear the old ball, draw the new one, then push the union rectangle
  fbClear();
  fbRect(0, 0, 128, 64, true);
  fbDisc(ball.x + 6, ball.y + 6, 6, true);
  fbPixel(ball.x + 4, ball.y + 4, false);

  int x0 = min(ox, ball.x) - 1;
  int y0 = min(oy, ball.y) - 1;
  int x1 = max(ox, ball.x) + 13;
  int y1 = max(oy, ball.y) + 13;

  // the previously drawn ball must be erased too: redraw from a clean buffer
  // (cheap - it is 1 KB of RAM and no bus traffic)
  pushRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);

  frames++;
  if (millis() - fpsT > 2000) {
    Serial.printf("  %lu fps, %lu NAKs total\n", (unsigned long)(frames * 1000 / (millis() - fpsT)),
                  (unsigned long)ackNak);
    frames = 0;
    fpsT = millis();
  }
}
