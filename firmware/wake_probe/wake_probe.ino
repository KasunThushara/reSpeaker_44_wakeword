#include "ESP_I2S.h"
#include "ESP_SR.h"

// wake_probe - WakeNet "Hi ESP" on the reSpeaker XVF3800 44mm + XIAO ESP32-S3 Plus.
//
// Audio path is proven (2026-11-19, firmware/audio_rms): I2S slave, 16 kHz,
// 32-bit slots, stereo, and the beamformed mic sits on the LEFT slot
// (L rms ~3000 when speaking vs R ~50). Hence input_format = "MN".
//
// Board settings REQUIRED for this sketch (Arduino IDE menus / CLI FQBN):
//   board        ESP32S3 Dev Module
//   Flash Size   16MB (128Mb)
//   PSRAM        OPI PSRAM
//   Partition    ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)
//   USB CDC On Boot  Enabled   (Serial must come out of the native USB port,
//                               and GPIO43 is our I2S data pin)
// FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,CDCOnBoot=cdc
//
// The model partition holds srmodels.bin (3.34 MB) and is flashed automatically
// by the core's upload.extra_flags at 0xC10000.

#define I2S_BCK 8     // D9
#define I2S_WS  7     // D8
#define I2S_DIN 43    // D6
#define LED_PIN 21    // XIAO user LED

I2SClass i2s;

// Command list: required by the API, and MultiNet is initialised unconditionally
// by the core (CONFIG_SR_MN_EN_NONE is not set in the prebuilt libs). A made-up
// word ("unused") crashes the FST builder, so use the phrases the upstream Basic
// example ships with. Wake-word mode ignores them.
enum {
  SR_CMD_LIGHT_ON,
  SR_CMD_LIGHT_OFF,
  SR_CMD_FAN_ON,
  SR_CMD_FAN_OFF,
};
static const sr_cmd_t sr_commands[] = {
  {SR_CMD_LIGHT_ON, "Turn on the light"},
  {SR_CMD_LIGHT_ON, "Switch on the light"},
  {SR_CMD_LIGHT_ON, "Lights on"},
  {SR_CMD_LIGHT_OFF, "Turn off the light"},
  {SR_CMD_LIGHT_OFF, "Switch off the light"},
  {SR_CMD_LIGHT_OFF, "Lights off"},
  {SR_CMD_LIGHT_OFF, "Go dark"},
  {SR_CMD_FAN_ON, "Start fan"},
  {SR_CMD_FAN_ON, "Fan on"},
  {SR_CMD_FAN_OFF, "Stop fan"},
  {SR_CMD_FAN_OFF, "Fan off"},
};

static volatile uint32_t wakeCount = 0;
static volatile uint32_t suppressed = 0;
static volatile uint32_t lastWake = 0;
static volatile uint32_t lastAccept = 0;

// WakeNet re-triggers while the same utterance is still in its window, so one
// "Hi ESP" produced ~1.6 events in the first test. One event per utterance:
#define COOLDOWN_MS 2000

static void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_WAKEWORD:
      if (millis() - lastAccept < COOLDOWN_MS) {
        suppressed++;
        break;
      }
      lastAccept = millis();
      wakeCount++;
      lastWake = millis();
      digitalWrite(LED_PIN, HIGH);
      Serial.printf("\n*** \"HI ESP\" DETECTED  (#%lu) ***\n", (unsigned long)wakeCount);
      break;
    case SR_EVENT_WAKEWORD_CHANNEL:
      Serial.printf("    channel %d verified\n", command_id);
      break;
    case SR_EVENT_TIMEOUT:
      Serial.println(F("    (timeout)"));
      break;
    case SR_EVENT_COMMAND:
      Serial.printf("    command %d (phrase %d) - ignored, wake-word only\n", command_id, phrase_id);
      break;
    default:
      Serial.printf("    event %d\n", (int)event);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println(F("=== wake_probe: WakeNet \"Hi ESP\" on XVF3800 + XIAO S3 Plus ==="));
  Serial.printf("PSRAM: %u bytes total, %u free\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  Serial.printf("free heap: %u\n", (unsigned)ESP.getFreeHeap());
  if (ESP.getPsramSize() == 0) {
    Serial.println(F("!! no PSRAM: set Tools > PSRAM > OPI PSRAM"));
  }

  i2s.setTimeout(1000);
  i2s.setPins(I2S_BCK, I2S_WS, -1, I2S_DIN);
  bool iok = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
                       I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT, I2S_ROLE_SLAVE);
  Serial.printf("i2s.begin: %s\n", iok ? "ok" : "FAILED");
  if (!iok) {
    Serial.println(F("XVF3800 must run the c16k2ch I2S firmware and act as master"));
    return;
  }
  // 32-bit slots -> 16-bit samples, which is what ESP_SR wants
  bool tok = i2s.configureRX(16000, I2S_DATA_BIT_WIDTH_32BIT,
                             I2S_SLOT_MODE_STEREO, I2S_RX_TRANSFORM_32_TO_16);
  Serial.printf("configureRX 32->16: %s\n", tok ? "ok" : "FAILED");

  ESP_SR.onEvent(onSrEvent);
  bool sok = ESP_SR.begin(i2s, sr_commands,
                          sizeof(sr_commands) / sizeof(sr_cmd_t),
                          SR_CHANNELS_STEREO, SR_MODE_WAKEWORD, "MN");
  Serial.printf("ESP_SR.begin: %s\n", sok ? "ok" : "FAILED");
  if (!sok) {
    Serial.println(F("check: partition scheme = ESP SR 16M, PSRAM = OPI, srmodels.bin flashed"));
    return;
  }
  Serial.println(F("listening for \"Hi ESP\" - say it clearly, 0.5-2 m away"));
}

void loop() {
  static uint32_t tLed = 0, tBeat = 0;

  // 100 ms LED pulse on detection
  if (digitalRead(LED_PIN) && millis() - lastWake > 100) digitalWrite(LED_PIN, LOW);

  if (millis() - tBeat > 5000) {
    tBeat = millis();
    Serial.printf("[%5lus] listening... wakes=%lu (suppressed %lu) heap=%u\n",
                  (unsigned long)(millis() / 1000), (unsigned long)wakeCount,
                  (unsigned long)suppressed, (unsigned)ESP.getFreeHeap());
  }
  delay(10);
}
