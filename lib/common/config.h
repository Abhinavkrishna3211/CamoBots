#pragma once
// Game settings. Sent to bodies inside HIDE_START. Pure C++ for native tests.
#include <stdint.h>

#pragma pack(push, 1)
struct Config {
  uint16_t hideSec = 60;
  uint16_t huntSec = 180;
  uint16_t whistleSec = 45;     // auto-whistle interval, 0 = off; halves in the last 30 s
  uint16_t missLockMs = 3000;
  uint8_t revealSec = 8;        // the OLED shows the winner, then round scores after 3 s
  uint8_t coverPct = 40;        // covered when light < this % of baseline
  uint8_t strikes = 3;          // cover events before strike-out, 0 = never
  uint8_t hp = 0;               // seeker HP (misses allowed), 0 = unlimited
  uint8_t numBodies = 2;
};
#pragma pack(pop)

#define DARK_ADC        400     // baseline below this = too dark, cover check off (tune on bench)
#define COVER_ON_MS     1000
#define COVER_OFF_MS    2000
#define PANIC_SEC       30
#define SHOT_WINDOW_MS  300     // no FOUND within this after a shot = miss
#define HOLD_STOP_MS    3000    // hold the trigger this long to stop a round
#define SCAN_MS         200     // blaster sends a Missed Spot scan frame this often in the hunt
