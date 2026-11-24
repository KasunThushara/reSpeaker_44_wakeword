#include "ESP_I2S.h"

// audio_rms - M0: prove the XVF3800 -> XIAO I2S path before touching WakeNet.
//
// Wiring is on-board (see PLAN.md fact 1), so only pin config is needed:
//   BCLK = D9/GPIO8, WS = D8/GPIO7, DIN = D6/GPIO43, XMOS is the I2S MASTER,
//   so the XIAO must run as SLAVE. The 44mm unit is flashed with the 16 kHz
//   2-channel I2S firmware, so rate = 16000 and no resampling is needed.
//
// Bit width of that firmware is not documented; the wiki test used 32-bit, so
// we open 32-bit and print the low 16 bits too, to see which half carries the
// signal. Per-channel RMS tells us which slot holds the beamformed mic.
//
// Expected: silence ~0-50, speech hundreds to a few thousand, and one channel
// clearly hotter than the other.

I2SClass i2s;

static const uint32_t RATE = 16000;
static const int PIN_BCK = 8;    // D9
static const int PIN_WS = 7;     // D8
static const int PIN_DIN = 43;   // D6

static int32_t buf[512];         // 128 stereo frames of 32-bit samples

static void report(const char *what, double rmsL, double rmsR, uint32_t frames) {
  Serial.printf("%-8s L=%7.0f  R=%7.0f  (%lu frames)\n", what, rmsL, rmsR,
                (unsigned long)frames);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Serial.println();
  Serial.println(F("=== audio_rms: I2S slave 16 kHz, 32-bit, stereo ==="));
  Serial.printf("pins: BCK=%d WS=%d DIN=%d\n", PIN_BCK, PIN_WS, PIN_DIN);

  i2s.setTimeout(1000);
  i2s.setPins(PIN_BCK, PIN_WS, -1, PIN_DIN);
  bool ok = i2s.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_32BIT,
                      I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT, I2S_ROLE_SLAVE);
  Serial.printf("i2s.begin: %s\n", ok ? "ok" : "FAILED");
  if (!ok) {
    Serial.println(F("check that the XVF3800 runs the c16k2ch I2S firmware"));
    return;
  }
  Serial.println(F("speak near the array: RMS should jump on one channel"));
}

void loop() {
  size_t got = i2s.readBytes((char *)buf, sizeof(buf));
  static uint32_t frames = 0, tLast = 0;
  static double accL = 0, accR = 0;

  if (got == 0) {
    if (millis() - tLast > 2000) {
      tLast = millis();
      Serial.println(F("no I2S data - is the XMOS in I2S mode?"));
    }
    delay(10);
    return;
  }

  int n = got / 4;                  // 32-bit samples
  for (int i = 0; i + 1 < n; i += 2) {
    // the signal is in the top 16 bits of each 32-bit slot
    int16_t l = (int16_t)(buf[i] >> 16);
    int16_t r = (int16_t)(buf[i + 1] >> 16);
    accL += (double)l * l;
    accR += (double)r * r;
    frames++;
  }

  if (millis() - tLast >= 500) {
    tLast = millis();
    double n = frames ? frames : 1;
    report("rms", sqrt(accL / n), sqrt(accR / n), frames);
    accL = accR = 0;
    frames = 0;
  }
}
