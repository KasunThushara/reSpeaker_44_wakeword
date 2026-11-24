#include <Arduino.h>
#include <math.h>
#include "ESP_I2S.h"

// audio_play - N2: prove the speaker path, then play the wake chime.
//
// Facts this relies on:
//   * full duplex needs BOTH pins: setPins(BCK=8, WS=7, DOUT=44, DIN=43).
//     Our wake_probe passed -1 for DOUT, so TX was never created - and
//     I2SClass::initSTD only makes a tx channel when _dout >= 0.
//   * the XMOS is I2S master, so the XIAO runs I2S_ROLE_SLAVE and the master's
//     clock paces our writes.
//   * the 16 kHz firmware is unproven for playback - Seeed's examples that play
//     audio all use the 48 kHz image - hence the 440 Hz tone test FIRST.
//
// Sample format: 32-bit slots with the signal in the TOP 16 bits, matching what
// the RX path delivers (verified in M0 with audio_rms).
//
// Sequence: 3 short tone bursts (is it audible at all?), then the chime from
// wake_sound.h (16 kHz mono, generated from the Home Assistant Voice PE
// wake_word_triggered.flac).

#define I2S_BCK 8     // D9
#define I2S_WS  7     // D8
#define I2S_DOUT 44   // D7
#define I2S_DIN 43    // D6

#include "wake_sound.h"   // const int16_t WAKE_SOUND[], WAKE_SOUND_LEN

I2SClass i2s;

static void writeFrames(const int16_t *mono, size_t n, const char *label) {
  const size_t CHUNK = 256;             // frames per write
  static int32_t frame[CHUNK * 2];
  uint32_t t0 = millis();
  size_t done = 0;
  while (done < n) {
    size_t k = (n - done < CHUNK) ? (n - done) : CHUNK;
    for (size_t i = 0; i < k; i++) {
      int32_t s = (int32_t)mono[done + i] << 16;   // top 16 bits of the slot
      frame[i * 2] = s;
      frame[i * 2 + 1] = s;
    }
    i2s.write((uint8_t *)frame, k * 2 * sizeof(int32_t));
    done += k;
  }
  uint32_t ms = millis() - t0;
  Serial.printf("  %s: %u samples = %.2f s of audio, written in %lu ms (%.1fx realtime)\n",
                label, (unsigned)n, n / 16000.0f, (unsigned long)ms,
                (ms ? (n / 16.0f) / ms : 0));
}

// 440 Hz tone, ~10% amplitude: audible but not painful
static void tone(int ms, int amp) {
  static int16_t buf[1600];             // 100 ms at 16 kHz
  const float step = 2.0f * PI * 440.0f / 16000.0f;
  static float ph = 0;
  size_t total = (size_t)(16000UL * ms / 1000);
  size_t done = 0;
  while (done < total) {
    size_t k = (total - done < 1600) ? (total - done) : 1600;
    for (size_t i = 0; i < k; i++) {
      buf[i] = (int16_t)(sinf(ph) * amp);
      ph += step;
      if (ph > 2 * PI) ph -= 2 * PI;
    }
    writeFrames(buf, k, "tone");
    done += k;
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Serial.println();
  Serial.println(F("=== audio_play (N2): speaker path test ==="));
  Serial.printf("pins: BCK=%d WS=%d DOUT=%d DIN=%d\n", I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN);
  Serial.printf("chime: %u samples = %.2f s at 16 kHz\n",
                (unsigned)WAKE_SOUND_LEN, WAKE_SOUND_LEN / 16000.0f);

  i2s.setTimeout(1000);
  i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN);
  bool ok = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
                      I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT, I2S_ROLE_SLAVE);
  Serial.printf("i2s.begin (full duplex): %s\n", ok ? "ok" : "FAILED");
  if (!ok) {
    Serial.println(F("check the XVF3800 runs the I2S firmware"));
    return;
  }
  delay(300);

  Serial.println(F("\n--- test 1: 440 Hz tone, 3 bursts of 400 ms ---"));
  for (int i = 0; i < 3; i++) {
    Serial.printf("  burst %d\n", i + 1);
    tone(400, 3000);
    delay(400);
  }

  Serial.println(F("\n--- test 2: wake chime ---"));
  delay(300);
  writeFrames(WAKE_SOUND, WAKE_SOUND_LEN, "chime");

  Serial.println(F("\n--- done. repeating every 6 s ---"));
}

void loop() {
  delay(6000);
  Serial.println(F("--- chime ---"));
  writeFrames(WAKE_SOUND, WAKE_SOUND_LEN, "chime");
  delay(1000);
  Serial.println(F("--- tone ---"));
  tone(300, 3000);
}
