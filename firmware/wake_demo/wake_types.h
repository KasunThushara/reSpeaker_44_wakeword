#pragma once
#include <stdint.h>

// Shared types for wake_demo. Kept in a header because the Arduino build inserts
// generated function prototypes at the top of the .ino, before any local type
// declarations - a prototype that mentions Step or State would then fail.
struct Step {
  uint8_t effect;
  uint32_t color;
  uint8_t brightness;
  uint16_t ms;
};

enum State : uint8_t {
  ST_BOOT,
  ST_IDLE,
  ST_WAKE_ANIM,
  ST_LISTEN,
  ST_CMD_ANIM
};
