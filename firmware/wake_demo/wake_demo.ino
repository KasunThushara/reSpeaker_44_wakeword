#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "ESP_I2S.h"
#include "ESP_SR.h"
#include "wake_sound.h"
#include "wake_types.h"

// wake_demo v2 (N3.1) - tuned after the first live test (2026-11-19).
//
// What the first run showed:
//   * wake -> chime -> 1.0 s animation -> command mode. That is 1.7 s of dead
//     time, so a command spoken straight after "Hi ESP" was simply not heard.
//   * the wake cooldown was 2.5 s, so saying "Hi ESP" again felt ignored.
//   * MultiNet needed several attempts per phrase.
//
// Changes:
//   1. command mode starts IMMEDIATELY after the chime, animation runs after
//   2. wake cooldown 1.2 s (still kills the ~1.6x double trigger)
//   3. only 4 commands, maximally distinct (3 colours + one obvious word)
//   4. AIC3104 output codec unmuted at 0 dB so the chime is actually audible
//   5. phrase id printed, so a misrecognition is visible in the log
//
// Everything else is unchanged from the versions proven separately:
//   I2S slave 16 kHz full duplex (DOUT=44), ESP-SR "MN", LED ring via GPO 0x2C.

#define I2S_BCK  8
#define I2S_WS   7
#define I2S_DOUT 44
#define I2S_DIN  43
#define LED_PIN  21

// ---- XVF3800 LED ring (GPO servicer) ----
#define XMOS_ADDR 0x2C
#define GPO_RESID 20
#define CMD_GPO_WRITE_VALUE 1
#define CMD_LED_EFFECT      12
#define CMD_LED_BRIGHTNESS  13
#define CMD_LED_SPEED       15
#define CMD_LED_COLOR       16
#define CMD_DOA             18
#define LED_POWER_PIN       33

// ---- AIC3104 output codec ----
#define AIC_ADDR 0x18

// ---- PC link (UDP) ----
// Fill these in before flashing. Empty strings boot the sketch in local-only
// mode: wake word, chime and LED feedback still work, but nothing reaches the PC.
#define WIFI_SSID ""
#define WIFI_PASS ""
#define PC_IP     "192.168.7.217"
#define PC_PORT   9001
#define LOCAL_PORT 9002
#define AUDIO_PORT 9100          // PC -> XIAO PCM stream ("JA ..." / "JS")
#define AUDIO_RATE 16000
#define AUDIO_CHUNK 320          // 20 ms of mono int16
#define AUDIO_BUF_SECONDS 3

enum : uint8_t {
  LED_OFF = 0, LED_BREATHING = 1, LED_RAINBOW = 2,
  LED_SOLID = 3, LED_DOA = 4, LED_RING = 5
};

// WALL-E voice set: 8 phrases (the other 31 actions stay in the brain registry).
// Chosen for demo impact plus phonetic separation: a different verb each
// (go / spin / wave / give / do / charge / play / show) and no minimal pairs.
static const sr_cmd_t sr_commands[] = {
  {1,  "go straight ahead"},
  {5,  "spin in circles"},
  {9,  "wave your hand"},
  {11, "give me a hug"},
  {19, "do a dance"},
  {29, "charge your battery"},
  {35, "play hide and seek"},
  {39, "show me the weather"},
};

// index-parallel to sr_commands, for readable logs
static const char *PHRASE_TEXT[] = {
  "go straight ahead", "spin in circles", "wave your hand", "give me a hug",
  "do a dance", "charge your battery", "play hide and seek", "show me the weather"
};

#define N(a) (sizeof(a) / sizeof(a[0]))

static const Step A_PLAY[] = {
  // colour wheel as instant local feedback for "play some music"
  {LED_SOLID, 0xFF0000, 210, 130},
  {LED_SOLID, 0xFF8000, 210, 130},
  {LED_SOLID, 0xFFFF00, 210, 130},
  {LED_SOLID, 0x00FF00, 210, 130},
  {LED_SOLID, 0x00FFFF, 210, 130},
  {LED_SOLID, 0x0000FF, 210, 130},
  {LED_SOLID, 0xFF00FF, 210, 130},
};
static const Step A_STOP[] = {
  {LED_SOLID, 0xFFFFFF, 140, 170},
  {LED_SOLID, 0xFFFFFF, 60, 170},
  {LED_OFF,   0x000000, 0,   600},
};
static const Step A_PICTURE[] = {
  {LED_BREATHING, 0x00FFFF, 210, 900},
  {LED_BREATHING, 0x00FFFF, 80,  700},
};
static const Step A_JOKE[] = {
  {LED_RAINBOW, 0xFFFFFF, 210, 1000},
  {LED_RAINBOW, 0xFFFFFF, 120, 800},
};
static const Step A_MOOD[] = {
  {LED_SOLID, 0x8000FF, 210, 220},
  {LED_SOLID, 0xFF00AA, 210, 220},
  {LED_SOLID, 0x00FFC0, 210, 220},
};
static const Step A_WEATHER[] = {
  {LED_SOLID, 0x0066FF, 210, 350},
  {LED_SOLID, 0x00CCFF, 210, 350},
};
static const Step A_DEMO[] = {
  {LED_SOLID, 0xFFFFFF, 40,  140},
  {LED_SOLID, 0xFFFFFF, 120, 140},
  {LED_SOLID, 0xFFFFFF, 255, 380},
};
static const Step A_ASK[] = {
  {LED_SOLID, 0x00FFC0, 220, 200},
  {LED_SOLID, 0x00FFC0, 90,  200},
};
static const Step A_SHUT[] = {
  {LED_SOLID, 0xFFFFFF, 90, 160},
  {LED_OFF,   0x000000, 0,  500},
};
static const Step A_WAKE[] = {
  // Was LED_RING, which is the same effect as the "spin the ring" command, so
  // the wake pulse looked like that animation. LED_RING also appears to ignore
  // the colour register. Use SOLID (which honours colour) in a shade no command
  // uses: a bright mint flash, then dim, then the listening breathing takes over.
  {LED_SOLID, 0x00FFC0, 255, 160},
  {LED_SOLID, 0x00FFC0, 90,  140},
};

struct Anim { const Step *steps; uint8_t n; };
static const Anim ANIMS[] = {
  {nullptr, 0},
  {A_PLAY,    (uint8_t)N(A_PLAY)},
  {A_STOP,    (uint8_t)N(A_STOP)},
  {A_PICTURE, (uint8_t)N(A_PICTURE)},
  {A_JOKE,    (uint8_t)N(A_JOKE)},
  {A_MOOD,    (uint8_t)N(A_MOOD)},
  {A_WEATHER, (uint8_t)N(A_WEATHER)},
  {A_DEMO,    (uint8_t)N(A_DEMO)},
  {A_SHUT,    (uint8_t)N(A_SHUT)},
  {A_ASK,     (uint8_t)N(A_ASK)},
};

// WALL-E action id -> ring animation by category (the LEDs are just feedback;
// the character animation runs in the browser). Ids come from the brain.
static const Anim *animForId(int id) {
  if (id >= 1  && id <= 6)  return &ANIMS[1];   // Movement
  if (id >= 7  && id <= 12) return &ANIMS[4];   // Social
  if (id >= 13 && id <= 18) return &ANIMS[5];   // Emotions
  if (id >= 19 && id <= 23) return &ANIMS[5];   // Entertainment
  if (id >= 24 && id <= 28) return &ANIMS[9];   // Intelligence
  if (id >= 29 && id <= 33) return &ANIMS[6];   // Power
  if (id >= 34 && id <= 38) return &ANIMS[7];   // Funny
  if (id == 39)             return &ANIMS[6];   // Info / weather
  return nullptr;
}

I2SClass i2s;
WiFiUDP udp;
WiFiUDP udpAudio;            // second socket: PCM stream on AUDIO_PORT
static bool netReady = false;
static uint32_t netSeq = 0;
static uint32_t lastNetRetry = 0;
static volatile bool lastAckOk = true;
static volatile bool ackOk = false;
static volatile bool ackPending = false;
static volatile uint32_t pendingSeq = 0;
static uint32_t pendingUntil = 0;

// ---------------- PC link ----------------
static void wifiConnect() {
  if (WIFI_SSID[0] == 0 || WIFI_PASS[0] == 0) {
    Serial.println(F("WiFi: no credentials - edit WIFI_SSID / WIFI_PASS"));
    netReady = false;
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi: joining %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  netReady = (WiFi.status() == WL_CONNECTED);
  if (netReady) {
    udp.begin(LOCAL_PORT);
    Serial.printf("WiFi: ip=%s rssi=%d dBm -> PC %s:%u\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), PC_IP, PC_PORT);
    // Warm-up datagram: primes the ARP entry so the first real command is not
    // lost the way the first two were in the first field test.
    udp.beginPacket(IPAddress(192, 168, 7, 217), PC_PORT);
    udp.write((const uint8_t *)"JARVIS 0000 0\n", 15);
    udp.endPacket();
    delay(80);
    while (udp.parsePacket() > 0) {
      char b[32];
      udp.read(b, sizeof(b));
    }
  } else {
    Serial.println(F("WiFi: FAILED - running local-only (LED feedback, no PC)"));
  }
}

// Drain the socket; match ACKs against the pending sequence. Called every loop
// iteration, because the ACK can arrive hundreds of ms after the send (the
// ESP-SR audio task shares the CPU with lwIP).
static void pollNet() {
  if (!netReady) return;
  for (int i = 0; i < 4; i++) {
    int n = udp.parsePacket();
    if (n <= 0) break;
    char buf[48];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len < 0) len = 0;
    buf[len] = 0;
    Serial.printf("     <-     %s", buf);
    if (strncmp(buf, "ACK", 3) == 0) {
      uint32_t s = (uint32_t)strtoul(buf + 4, nullptr, 10);
      if (ackPending && s == pendingSeq) {
        ackOk = true;
        ackPending = false;
      }
    }
  }
  if (ackPending && (int32_t)(millis() - pendingUntil) >= 0) {
    ackPending = false;
    Serial.printf("[%6lu ms] no ACK for seq %lu\n",
                  (unsigned long)millis(), (unsigned long)pendingSeq);
  }
}

// fire "JARVIS <seq> <id>" and let pollNet() pick up the ACK
static uint32_t lastSentSeq = 0;
static int lastSentId = 0;

static void sendCommand(int id) {
  if (!netReady) {
    lastAckOk = false;
    return;
  }
  netSeq++;
  lastSentSeq = netSeq;
  lastSentId = id;
  pendingSeq = netSeq;
  ackOk = false;
  ackPending = true;
  pendingUntil = millis() + 2500;

  char msg[40];
  snprintf(msg, sizeof(msg), "JARVIS %04lu %d\n", (unsigned long)netSeq, id);
  udp.beginPacket(IPAddress(192, 168, 7, 217), PC_PORT);
  udp.write((const uint8_t *)msg, strlen(msg));
  udp.endPacket();
  Serial.printf("[%6lu ms] -> PC  %s", (unsigned long)millis(), msg);
}

// Retry the SAME sequence number. The brain dedupes by (ip, seq), so if it did
// receive the original it only re-ACKs - the action never runs twice.
static void resendCommand() {
  if (!netReady || lastSentSeq == 0) return;
  pendingSeq = lastSentSeq;
  ackOk = false;
  ackPending = true;
  pendingUntil = millis() + 800;
  char msg[40];
  snprintf(msg, sizeof(msg), "JARVIS %04lu %d\n",
           (unsigned long)lastSentSeq, lastSentId);
  udp.beginPacket(IPAddress(192, 168, 7, 217), PC_PORT);
  udp.write((const uint8_t *)msg, strlen(msg));
  udp.endPacket();
  Serial.printf("[%6lu ms] -> PC  %s (retry)", (unsigned long)millis(), msg);
}

// ---------------- I2C helpers ----------------
static bool xmosWrite(uint8_t resid, uint8_t cmd, const uint8_t *val, uint8_t n) {
  for (int a = 0; a < 3; a++) {
    Wire.beginTransmission(XMOS_ADDR);
    Wire.write(resid); Wire.write(cmd); Wire.write(n);
    for (uint8_t i = 0; i < n; i++) Wire.write(val[i]);
    if (Wire.endTransmission() == 0) return true;
    delay(2);
  }
  return false;
}

// ---------------- XVF3800 audio tuning ----------------
// The wake word and the command recogniser hear the XVF3800's processed output.
// Stock gains are conservative: at 1.5-2 m the model's input sits below its
// detection probability. These are the same knobs the ESPHome integration
// exposes (PP_AGCMAXGAIN on the comms path, AEC_ASROUTGAIN on the ASR beams).
#define AEC_RESID       33
#define AEC_ASR_ONOFF   35
#define AEC_ASR_GAIN    36
#define PP_RESID        17
#define PP_AGC_MAXGAIN  11

static void xmosFloat(uint8_t resid, uint8_t cmd, float v) {
  uint8_t b[4];
  memcpy(b, &v, 4);
  xmosWrite(resid, cmd, b, 4);
}

static void setupAudioTuning() {
  // more AGC headroom for quiet/distant speech (comms path)
  xmosFloat(PP_RESID, PP_AGC_MAXGAIN, 64.0f);
  // ASR beams on the second output, with a mild boost (stock is 1.0)
  uint8_t on[4] = {1, 0, 0, 0};
  xmosWrite(AEC_RESID, AEC_ASR_ONOFF, on, 4);
  xmosFloat(AEC_RESID, AEC_ASR_GAIN, 1.5f);
  Serial.println(F("audio tuning: AGCMAXGAIN=64, ASROUTGAIN=1.5, ASR on"));
}

static void ledEffect(uint8_t e) { xmosWrite(GPO_RESID, CMD_LED_EFFECT, &e, 1); }
static void ledBrightness(uint8_t b) { xmosWrite(GPO_RESID, CMD_LED_BRIGHTNESS, &b, 1); }
static void ledSpeed(uint8_t s) { xmosWrite(GPO_RESID, CMD_LED_SPEED, &s, 1); }

// one-byte XMOS read: [resid, cmd|0x80, 2] then status + value
static bool xmosRead1(uint8_t resid, uint8_t cmd, uint8_t *val) {
  Wire.beginTransmission(XMOS_ADDR);
  Wire.write(resid);
  Wire.write((uint8_t)(cmd | 0x80));
  Wire.write((uint8_t)2);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)XMOS_ADDR, (uint8_t)2) != 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t status = Wire.read();
  *val = Wire.read();
  return status == 0;
}

// Write the effect and confirm it landed. If the read is unsupported we trust
// the write; if the readback disagrees we retry - a silently lost effect write
// left the ring showing the previous command's pattern.
static void ledEffectRobust(uint8_t e) {
  for (int i = 0; i < 4; i++) {
    if (xmosWrite(GPO_RESID, CMD_LED_EFFECT, &e, 1)) {
      uint8_t back = 0xFF;
      if (!xmosRead1(GPO_RESID, CMD_LED_EFFECT, &back)) return;   // read unsupported
      if (back == e) return;
    }
    delay(5);
  }
  Serial.printf("  !! LED effect %u would not stick\n", e);
}

static void ledColor(uint32_t rgb) {
  uint8_t p[4] = {(uint8_t)(rgb & 0xFF), (uint8_t)((rgb >> 8) & 0xFF),
                  (uint8_t)((rgb >> 16) & 0xFF), 0x00};
  xmosWrite(GPO_RESID, CMD_LED_COLOR, p, 4);
}

static void ledPowerOn() {
  uint8_t p[2] = {LED_POWER_PIN, 1};
  xmosWrite(GPO_RESID, CMD_GPO_WRITE_VALUE, p, 2);
}

static bool readDoa(uint16_t &angle, uint16_t &speech) {
  uint8_t b[4] = {0};
  Wire.beginTransmission(XMOS_ADDR);
  Wire.write((uint8_t)GPO_RESID);
  Wire.write((uint8_t)(CMD_DOA | 0x80));
  Wire.write((uint8_t)5);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)XMOS_ADDR, (uint8_t)5) != 5) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t status = Wire.read();
  for (uint8_t i = 0; i < 4; i++) b[i] = Wire.read();
  if (status) return false;
  angle = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
  speech = (uint16_t)b[2] | ((uint16_t)b[3] << 8);
  return true;
}

// ---------------- AIC3104 output codec + volume (0.5 dB/step DAC) ----------------
// Levels are percentages of full-scale amplitude (the way the demo talks about
// them): 70 % while music plays or an announcement speaks, 30 % while the
// device is listening so the mic hears the command over the music.
// -20*log10(pct/100) dB, in 0.5 dB DAC steps: 70 % -> 0x06, 30 % -> 0x15.
#define MUSIC_PERCENT 70
#define DUCK_PERCENT  30

static int targetPercent = MUSIC_PERCENT;   // effective level
static volatile int volRequest = -1;        // "JV <pct>" from the brain

static bool aicWrite(uint8_t reg, uint8_t val) {
  for (int i = 0; i < 3; i++) {
    Wire.beginTransmission(AIC_ADDR);
    Wire.write(reg); Wire.write(val);
    if (Wire.endTransmission() == 0) return true;
    delay(2);
  }
  return false;
}

static bool aicRead(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(AIC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)AIC_ADDR, (uint8_t)1) != 1) return false;
  val = Wire.read();
  return true;
}

static uint8_t dacForPercent(int pct) {
  if (pct >= 100) return 0;
  if (pct < 5) pct = 5;
  float db = -20.0f * log10f((float)pct / 100.0f);
  int steps = (int)(db * 2.0f + 0.5f);
  return (uint8_t)constrain(steps, 0, 127);
}

// I2C is shared with the XVF3800 servicer, so call this only from the main
// loop (never from the audio tasks).
static void applyVolume(const char *why) {
  aicWrite(0x00, 0x00);                         // page 0
  uint8_t dac = dacForPercent(targetPercent);
  bool ok = true;
  ok &= aicWrite(0x2B, dac);   // left  DAC
  ok &= aicWrite(0x2C, dac);   // right DAC
  ok &= aicWrite(0x33, 0x0D);  // HPLOUT: 0 dB, unmuted, powered
  ok &= aicWrite(0x41, 0x0D);  // HPROUT
  ok &= aicWrite(0x56, 0x09);  // LEFT_LOP
  ok &= aicWrite(0x5D, 0x09);  // RIGHT_LOP
  Serial.printf("volume: %d%% (%.1f dB, DAC 0x%02X)%s%s\n",
                targetPercent, -(dac * 0.5f), dac,
                why ? " " : "", why ? why : "");
  if (!ok) Serial.println(F("volume: WRITE FAILED"));
}

static void setupAic() {
  aicWrite(0x00, 0x00);   // page 0
  applyVolume("boot");
}

// ---------------- state ----------------
static volatile uint32_t evWake = 0;
static volatile int evCmd = 0;
static volatile int evPhrase = -1;
static uint32_t lastWakeAccept = 0;
static uint32_t suppressed = 0;
static uint32_t wakeCount = 0;

static State state = ST_BOOT;
static const Step *curSteps = nullptr;
static uint8_t curN = 0, curIdx = 0;
static uint32_t stepDeadline = 0;
static State afterAnim = ST_IDLE;
static uint32_t listenStart = 0;
static uint32_t cmdIgnoreUntil = 0;   // settle window after the chime

static void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_WAKEWORD:
      if (millis() - lastWakeAccept < 1200) { suppressed++; break; }
      lastWakeAccept = millis();
      evWake = millis();
      digitalWrite(LED_PIN, HIGH);
      break;
    case SR_EVENT_COMMAND:
      evCmd = command_id;
      evPhrase = phrase_id;
      break;
    default:
      break;
  }
}

// ---------------- PCM stream from the PC (player / speaker.py) ----------------
// The PC decodes to 16 kHz mono int16, 20 ms per UDP packet ("JA" | seq u32 |
// samples u16 | pcm).  The XIAO keeps AUDIO_BUF_SECONDS in PSRAM and feeds the
// I2S output from a high-priority task, so WiFi jitter never reaches the DAC.
// "JS" flushes the buffer. The I2S is shared with the chime under i2sMux.
static StreamBufferHandle_t sbAudio = nullptr;
static SemaphoreHandle_t i2sMux = nullptr;
static volatile bool streamOn = false;
static volatile uint32_t audioPkts = 0;
static volatile uint32_t audioUnderruns = 0;
static volatile uint32_t audioLastPkt = 0;
static volatile uint32_t audioGaps = 0;
static volatile uint32_t audioStale = 0;
static uint32_t audioSeqLast = 0;
static bool audioSeqValid = false;

// caller must hold i2sMux (i2s.write from two tasks would interleave frames)
static void writeFrames(const int16_t *mono, size_t n, int32_t *scratch) {
  const size_t CHUNK = 256;
  size_t done = 0;
  while (done < n) {
    size_t k = (n - done < CHUNK) ? (n - done) : CHUNK;
    for (size_t i = 0; i < k; i++) {
      int32_t s = (int32_t)mono[done + i] << 16;
      scratch[i * 2] = s;
      scratch[i * 2 + 1] = s;
    }
    i2s.write((uint8_t *)scratch, k * 2 * sizeof(int32_t));
    done += k;
  }
}

static void playPcm(const int16_t *mono, size_t n) {
  static int32_t scratch[256 * 2];
  // the chime is feedback for the user: always at the normal 70 %, even while
  // the music is ducked for listening, then restore the current target
  int saved = targetPercent;
  if (saved != MUSIC_PERCENT) {
    targetPercent = MUSIC_PERCENT;
    applyVolume("chime");
  }
  if (i2sMux) xSemaphoreTake(i2sMux, portMAX_DELAY);
  writeFrames(mono, n, scratch);
  if (i2sMux) xSemaphoreGive(i2sMux);
  if (saved != MUSIC_PERCENT) {
    targetPercent = saved;
    applyVolume("music");
  }
}

static void audioRxTask(void *) {
  udpAudio.begin(AUDIO_PORT);
  static uint8_t pkt[1200];
  Serial.printf("audio: listening on %u\n", (unsigned)AUDIO_PORT);
  for (;;) {
    int n = udpAudio.parsePacket();
    if (n <= 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    int len = udpAudio.read(pkt, sizeof(pkt));
    if (len >= 8 && pkt[0] == 'J' && pkt[1] == 'A') {
      uint16_t ns = (uint16_t)pkt[6] | ((uint16_t)pkt[7] << 8);
      size_t bytes = (size_t)ns * 2;
      if (ns && len >= 8 + (int)bytes) {
        uint32_t seq = (uint32_t)pkt[2] | ((uint32_t)pkt[3] << 8) |
                       ((uint32_t)pkt[4] << 16) | ((uint32_t)pkt[5] << 24);
        bool accept = true;
        if (seq == 0) {                       // a new stream always starts at 0
          if (sbAudio) xStreamBufferReset(sbAudio);
          audioSeqLast = 0;
          audioSeqValid = true;
        } else if (audioSeqValid) {
          int32_t d = (int32_t)(seq - audioSeqLast);
          if (d <= 0) {
            // late/duplicate packet: its audio slot has already been played.
            // This is what killed the broadcast experiment - the AP delivered
            // minutes-old broadcast frames in the middle of a live stream.
            audioStale++;
            accept = false;
          } else {
            if (d > 1) audioGaps++;
            audioSeqLast = seq;
          }
        } else {
          audioSeqLast = seq;
          audioSeqValid = true;
        }
        if (accept) {
          if (sbAudio) xStreamBufferSend(sbAudio, pkt + 8, bytes, 0);
          audioPkts++;
          audioLastPkt = millis();
          streamOn = true;
        }
      }
    } else if (len >= 4 && pkt[0] == 'J' && pkt[1] == 'V') {
      // "JV <pct>" - the brain sets the level (it owns everything after a
      // command is accepted). Applied by the main loop: I2C single-owner.
      char vb[8];
      int nb = len - 3;
      if (nb > 7) nb = 7;
      memcpy(vb, pkt + 3, nb);
      vb[nb] = 0;
      int pct = atoi(vb);
      if (pct >= 10 && pct <= 100) {
        volRequest = pct;
        Serial.printf("audio: volume request %d%%\n", pct);
      }
    } else if (len >= 2 && pkt[0] == 'J' && pkt[1] == 'S') {
      if (sbAudio) xStreamBufferReset(sbAudio);
      streamOn = false;
      audioSeqValid = false;
      Serial.println("audio: STOP");
    }
  }
}

static void audioOutTask(void *) {
  static int16_t pcm[AUDIO_CHUNK];
  static int32_t scratch[256 * 2];
  bool primed = false;
  bool needFlush = false;

  for (;;) {
    if (!streamOn) {
      if (needFlush) {
        // one silence chunk so the DMA does not repeat the last 20 ms forever
        memset(pcm, 0, sizeof(pcm));
        if (i2sMux && xSemaphoreTake(i2sMux, pdMS_TO_TICKS(300)) == pdTRUE) {
          writeFrames(pcm, AUDIO_CHUNK, scratch);
          xSemaphoreGive(i2sMux);
        }
        needFlush = false;
      }
      primed = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (!primed) {
      // wait for ~160 ms of cushion before the first write
      if (xStreamBufferBytesAvailable(sbAudio) < (size_t)(AUDIO_RATE * 2 / 1000 * 160)) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      primed = true;
    }

    size_t got = xStreamBufferReceive(sbAudio, pcm, sizeof(pcm), pdMS_TO_TICKS(200));
    if (got < sizeof(pcm)) {
      memset((uint8_t *)pcm + got, 0, sizeof(pcm) - got);   // pad with silence
      if (got == 0) {
        audioUnderruns++;
        if (millis() - audioLastPkt > 800) {               // sender went away
          streamOn = false;
          needFlush = true;
          continue;
        }
      }
    }
    if (i2sMux && xSemaphoreTake(i2sMux, pdMS_TO_TICKS(500)) == pdTRUE) {
      writeFrames(pcm, AUDIO_CHUNK, scratch);
      xSemaphoreGive(i2sMux);
    }
  }
}

static void startAudioTasks() {
  i2sMux = xSemaphoreCreateMutex();
  size_t bytes = (size_t)AUDIO_RATE * 2 * AUDIO_BUF_SECONDS;
  uint8_t *mem = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!mem) {
    bytes = (size_t)AUDIO_RATE * 2 * 1;
    mem = (uint8_t *)malloc(bytes);
    Serial.println(F("audio: PSRAM alloc failed, using 1 s in internal RAM"));
  }
  static StaticStreamBuffer_t sbStruct;
  if (mem) {
    // trigger level = one packet, so a read never returns half a chunk
    sbAudio = xStreamBufferCreateStatic(bytes, AUDIO_CHUNK * 2, mem, &sbStruct);
  }
  Serial.printf("audio: ring %u bytes (%s)\n", (unsigned)bytes,
                mem ? "ok" : "FAILED");
  xTaskCreatePinnedToCore(audioRxTask, "audioRx", 4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(audioOutTask, "audioOut", 4096, nullptr, 6, nullptr, 1);
}

static void startAnim(const Step *steps, uint8_t n, State next) {
  curSteps = steps; curN = n; curIdx = 0;
  afterAnim = next;
  stepDeadline = 0;
  state = ST_CMD_ANIM;
}

static void toIdleLook(const char *why) {
  ledEffectRobust(LED_DOA);
  ledBrightness(150);
  ledSpeed(60);
  Serial.printf("  -> DOA idle (%s)\n", why);
}

static void toListenLook() {
  // colour BEFORE effect: the XVF3800 latches the colour when an effect starts,
  // so sending the effect first renders it in the previous colour (this is what
  // produced a red flash at the start of the green wake pulse).
  ledColor(0x00FF00);
  ledEffectRobust(LED_BREATHING);
  ledBrightness(70);
  ledSpeed(40);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println(F("=== wake_demo walle: Hi Walle -> chime -> action -> PC + LED -> DOA ==="));
  Serial.println(F("say \"Hi Walle\", then one of the 8 voice actions:"));
  Serial.println(F("  go straight ahead / spin in circles / wave your hand / give me a hug"));
  Serial.println(F("  do a dance / charge your battery / play hide and seek / show me the weather"));

  Wire.begin(5, 6);
  Wire.setClock(100000);
  delay(100);

  wifiConnect();

  setupAic();

  i2s.setTimeout(1000);
  i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN);
  bool iok = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
                       I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT, I2S_ROLE_SLAVE);
  Serial.printf("i2s full duplex: %s\n", iok ? "ok" : "FAILED");
  if (!iok) return;
  i2s.configureRX(16000, I2S_DATA_BIT_WIDTH_32BIT,
                  I2S_SLOT_MODE_STEREO, I2S_RX_TRANSFORM_32_TO_16);

  startAudioTasks();           // PCM stream from the PC -> speaker

  delay(2000);                 // let the XMOS finish its boot animation
  setupAudioTuning();          // AGC headroom + ASR gain for the wake word
  ledPowerOn();
  ledBrightness(150);
  ledSpeed(60);
  toIdleLook("boot");

  ESP_SR.onEvent(onSrEvent);
  bool sok = ESP_SR.begin(i2s, sr_commands, N(sr_commands),
                          SR_CHANNELS_STEREO, SR_MODE_WAKEWORD, "MN");
  Serial.printf("ESP_SR: %s\n", sok ? "ok" : "FAILED");
  if (!sok) return;

  state = ST_IDLE;
  Serial.println(F("ready - say Hi Walle, then a command right away"));
}

void loop() {
  pollNet();          // ACKs are collected continuously, never in a blocking burst

  static uint32_t lastAudioLog = 0;
  if (streamOn && millis() - lastAudioLog > 5000) {
    lastAudioLog = millis();
    Serial.printf("  audio: %lu pkts, %lu underruns, %lu gaps, %lu stale\n",
                  (unsigned long)audioPkts, (unsigned long)audioUnderruns,
                  (unsigned long)audioGaps, (unsigned long)audioStale);
  }

  // level requests from the brain ("JV <pct>")
  int req = volRequest;
  if (req > 0) {
    volRequest = -1;
    targetPercent = req;
    applyVolume(req >= MUSIC_PERCENT ? "music" : "listening");
  }

  // the XVF3800 firmware / onboard buttons can rewrite the same codec
  // registers (documented on the Seeed wiki) - verify while streaming
  static uint32_t lastVolCheck = 0;
  if (streamOn && millis() - lastVolCheck > 5000) {
    lastVolCheck = millis();
    uint8_t v = 0;
    uint8_t want = dacForPercent(targetPercent);
    if (aicRead(0x2B, v) && v != want) {
      Serial.printf("volume: overridden (DAC 0x%02X != 0x%02X), restoring\n",
                    v, want);
      applyVolume("restore");
    }
  }

  // serial tuning at the booth: + / - / v (no reflash needed)
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '+' && targetPercent < 100) { targetPercent += 5; applyVolume("serial +"); }
    else if (c == '-' && targetPercent > 20) { targetPercent -= 5; applyVolume("serial -"); }
    else if (c == 'v' || c == 'V') { applyVolume("serial"); }
  }

  if (evWake && state == ST_IDLE) {
    evWake = 0;
    wakeCount++;
    Serial.printf("\n[%6lu ms] *** \"HI WALLE\" #%lu ***\n",
                  (unsigned long)millis(), (unsigned long)wakeCount);

    // Paint the wake look BEFORE the chime. Otherwise the ring keeps showing
    // whatever the last command left behind for the whole 820 ms of chime+drain.
    // Mint SOLID, matching A_WAKE's first step (LED_RING would look like the
    // "spin the ring" command and also ignores the colour register).
    ledColor(0x00FFC0);
    ledEffectRobust(LED_SOLID);
    ledBrightness(255);

    playPcm(WAKE_SOUND, WAKE_SOUND_LEN);          // 0.74 s chime
    digitalWrite(LED_PIN, LOW);
    Serial.printf("[%6lu ms] chime done\n", (unsigned long)millis());

    // Give the AFE 150 ms to drain the chime, then ignore any command event for
    // 350 ms. Without this the chime's tail could be matched to phrase 1
    // ("turn it red") and light the ring red straight after the wake word.
    delay(150);
    cmdIgnoreUntil = millis() + 350;
    ESP_SR.setMode(SR_MODE_COMMAND);
    Serial.printf("[%6lu ms] command mode open\n", (unsigned long)millis());

    startAnim(A_WAKE, (uint8_t)N(A_WAKE), ST_LISTEN);
    return;
  }
  evWake = 0;

  if (evCmd) {
    int id = evCmd, pid = evPhrase;
    evCmd = 0; evPhrase = -1;
    if ((int32_t)(millis() - cmdIgnoreUntil) < 0) {
      Serial.printf("[%6lu ms] ignored cmd %d/phrase %d (settle window)\n",
                    (unsigned long)millis(), id, pid);
    } else if (state == ST_LISTEN) {
      // MultiNet returns command_id == phrase_id and 1-based phrase numbers
      // (observed: 1..4 for a 4-phrase list, and phrase 4 = the 4th phrase).
      // Resolve through the phrase table so the animation follows what was
      // actually heard, not whatever the ID happens to mean in this core build.
      // MultiNet reports the command_id we registered (1, 5, 9, ...), NOT the
      // 1-based index in the table. With the old Jarvis table (ids 1..9) both
      // were the same number, which hid this; with sparse ids, indexing by it
      // silently dropped every command above 8.
      int mapped = (id > 0) ? id : pid;
      const char *txt = "?";
      bool known = false;
      for (size_t k = 0; k < N(sr_commands); k++) {
        if (sr_commands[k].command_id == mapped) {
          txt = sr_commands[k].str;
          known = true;
          break;
        }
      }
      Serial.printf("[%6lu ms] raw id=%d phrase=%d -> \"%s\" -> cmd %d\n",
                    (unsigned long)millis(), id, pid, txt, known ? mapped : 0);

      const Anim *a = known ? animForId(mapped) : nullptr;
      if (known && a && a->steps) {
        lastAckOk = false;
        sendCommand(mapped);                // ACK collected asynchronously
        startAnim(a->steps, a->n, ST_IDLE);
      } else {
        ESP_SR.setMode(SR_MODE_WAKEWORD);
        toIdleLook("unmapped command");
        state = ST_IDLE;
      }
    }
  }

  switch (state) {
    case ST_CMD_ANIM: {
      if (curIdx >= curN) break;
      if (stepDeadline == 0) {
        const Step &s = curSteps[curIdx];
        // COLOUR FIRST - see toListenLook(): the effect latches the old colour
        ledColor(s.color);
        ledEffectRobust(s.effect);
        ledBrightness(s.brightness);
        stepDeadline = millis() + s.ms;
        break;
      }
      if ((int32_t)(millis() - stepDeadline) >= 0) {
        curIdx++;
        stepDeadline = 0;
        if (curIdx >= curN) {
          curSteps = nullptr;
          if (afterAnim == ST_LISTEN) {
            toListenLook();           // mode already COMMAND
            listenStart = millis();   // window starts when we are actually ready
            // duck the music so the mics hear the command over it
            targetPercent = DUCK_PERCENT;
            applyVolume("listening");
            Serial.printf("[%6lu ms] listening for a command (6.5 s)\n",
                          (unsigned long)millis());
            state = ST_LISTEN;
          } else {
            ESP_SR.setMode(SR_MODE_WAKEWORD);
            // an ACK can lag behind the ESP-SR audio task; give it a moment
            uint32_t w = millis();
            while (ackPending && millis() - w < 700) {
              pollNet();
              delay(5);
            }
            // UDP right after a reboot sometimes drops the datagram (seen
            // live: seq 1 and 3 lost, seq 4 delivered). Resend the SAME seq:
            // the brain dedupes, so a retry can never run the action twice.
            for (int r = 0; r < 2 && !ackOk; r++) {
              resendCommand();
              w = millis();
              while (ackPending && millis() - w < 600) {
                pollNet();
                delay(5);
              }
            }
            lastAckOk = ackOk;
            if (!lastAckOk) {
              // PC never confirmed: ONE amber blink (the "not understood"
              // feedback is a DOUBLE blink, so the two are distinguishable).
              ledColor(0xFF8000);
              ledEffectRobust(LED_SOLID);
              ledBrightness(110);
              delay(140);
              ledEffectRobust(LED_OFF);
              delay(90);
            }
            toIdleLook(lastAckOk ? "animation done" : "done - no ACK from PC");
            state = ST_IDLE;
          }
        }
      }
      break;
    }

    case ST_LISTEN: {
      if (millis() - listenStart > 6500) {
        ESP_SR.setMode(SR_MODE_WAKEWORD);
        // "not understood" is AMBER, never red: red is reserved for the
        // "turn it red" animation, and having both made a miss look like a hit.
        Serial.printf("[%6lu ms] no command - amber double blink\n",
                      (unsigned long)millis());
        // Diagnostic: tell the PC the wake word fired but nothing matched, so
        // the brain log can distinguish "phrase missed" from "wake missed".
        sendCommand(99);
        // nobody will send "JV 70" if no command was accepted: restore here
        targetPercent = MUSIC_PERCENT;
        applyVolume("listening timeout");
        for (int i = 0; i < 2; i++) {
          ledColor(0xFF8000);
          ledEffectRobust(LED_SOLID);
          ledBrightness(110);
          delay(130);
          ledEffectRobust(LED_OFF);
          delay(110);
        }
        toIdleLook("back to DOA");
        state = ST_IDLE;
      }
      break;
    }

    case ST_IDLE:
    default: {
      static uint32_t lastDoa = 0;
      if (millis() - lastDoa > 500) {
        lastDoa = millis();
        uint16_t ang = 0, sp = 0;
        if (readDoa(ang, sp) && ang <= 359 && sp == 1) {
          Serial.printf("  DOA %3u deg\n", ang);
        }
      }
      break;
    }
  }
  delay(5);
}
