#include "hw.h"
#include "pins.h"

// ponytail: SIM only for now. Phase 5 adds the real TSOP/LDR/LEDC/NeoPixel drivers under #if !SIM.

static const char* PIX_NAME[] = {"off", "idle", "hide", "dark", "covered", "captured", "reveal", "live", "locked"};
static const char* SND_NAME[] = {"beep", "whistle", "taunt", "fanfare", "buzz", "celebrate", "fire"};

static Pix pix = PIX_OFF;
static uint16_t simLdr = 2000;
static int simHit = -1;
static uint32_t simBeam;
static bool simTrigger;
static String line, pending;
static bool havePending;

void hw_begin() {
  pinMode(PIN_USER_LED, OUTPUT);
  digitalWrite(PIN_USER_LED, HIGH);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
}

void hw_pixel(Pix p) {
  if (p == pix) return;
  pix = p;
  Serial.printf("[pix] %s\n", PIX_NAME[p]);
}

void hw_sound(Snd s) { Serial.printf("[snd] %s\n", SND_NAME[s]); }
uint16_t hw_ldr() { return simLdr; }
void hw_fire() { Serial.println("[ir] shot frame sent"); }

int hw_ir_hit() {
  int h = simHit;
  simHit = -1;
  return h;
}

uint32_t hw_beam_ms() {
  uint32_t b = simBeam;
  simBeam = 0;
  return b;
}

bool hw_trigger() {
  bool t = simTrigger;
  simTrigger = false;
  return t;
}

bool hw_cmd(String& out) {
  if (!havePending) return false;
  out = pending;
  havePending = false;
  return true;
}

// SIM commands: hit [shooter], scan N (seconds in the beam), ldr N, cover, uncover, dark, fire. BOOT = hit (body) / fire (blaster).
static void simCommand(String c) {
  if (c.startsWith("hit")) simHit = c.length() > 4 ? c.substring(4).toInt() : 0;
  else if (c.startsWith("scan ")) simBeam += c.substring(5).toInt() * 1000u;
  else if (c.startsWith("ldr ")) simLdr = c.substring(4).toInt();
  else if (c == "cover") simLdr = 200;
  else if (c == "uncover") simLdr = 2000;
  else if (c == "dark") simLdr = 300;
  else if (c == "fire") simTrigger = true;
  else { pending = c; havePending = true; return; }
  Serial.printf("[sim] %s\n", c.c_str());
}

void hw_update() {
  uint32_t now = millis();
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      line.trim();
      if (line.length()) simCommand(line);
      line = "";
    } else if (line.length() < 64) line += ch;
  }

  static bool bootWas = true;
  static uint32_t bootAt;
  bool boot = digitalRead(PIN_BOOT_BTN);
  if (!boot && bootWas && now - bootAt > 50) {
    bootAt = now;
#if defined(ROLE_BODY)
    simHit = 0;
#else
    simTrigger = true;
#endif
  }
  bootWas = boot;

  // User LED stands in for the NeoPixel / aim LED: period in ms, 0 = solid.
  static const int16_t period[] = {-1, 1000, 500, 80, 150, 0, 50, 0, 100};
  int16_t p = period[pix];
  bool on = p == 0 || (p > 0 && (now / p) % 2);
  digitalWrite(PIN_USER_LED, on ? LOW : HIGH);
}
