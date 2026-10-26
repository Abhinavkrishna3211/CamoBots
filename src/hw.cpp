#include "hw.h"
#include "pins.h"
#include "config.h"

// SIM builds fake the sensors over serial and use the user LED for the pixel.
// Real builds drive the TSOP, LDR, IR LED (RMT), speaker (LEDC) and RGB LED.
// All timings here are first guesses: tune them on the bench (docs/bench-results.md).

static const char* PIX_NAME[] = {"off", "idle", "tick", "dark", "covered", "captured", "reveal", "live", "locked", "go"};
static const char* SND_NAME[] = {"beep", "whistle", "taunt", "fanfare", "buzz", "celebrate", "fire", "bell", "go", "tick", "combo", "last"};

static Pix pix = PIX_OFF;
static int irHit = -1;
static uint32_t beamMs;
static bool trigger;
static uint32_t downAt;  // when the trigger went down, 0 while up
static String line, pending;
static bool havePending;

// IR frame: header mark, then 8 bits MSB first, each a mark (short = 0, long = 1) and a fixed space.
// Byte = type:2 shooter:2 check:4, where check is the inverse of the top nibble.
#define IR_HDR_MARK  2400
#define IR_SPACE     600
#define IR_ZERO_MARK 600
#define IR_ONE_MARK  1200
#define IR_SHOT 0
#define IR_SCAN 1

static uint8_t irByte(uint8_t type, uint8_t shooter) {
  uint8_t hi = (type & 3) << 2 | (shooter & 3);
  return hi << 4 | (~hi & 0xF);
}

#if SIM
static uint16_t simLdr = 2000;

void hw_pixel(Pix p) {
  if (p == pix) return;
  pix = p;
  Serial.printf("[pix] %s\n", PIX_NAME[p]);
}
void hw_sound(Snd s) { Serial.printf("[snd] %s\n", SND_NAME[s]); }
uint16_t hw_ldr() { return simLdr; }
void hw_fire() { Serial.println("[ir] shot frame sent"); }
void hw_scan() {}
void hw_ir_stats(uint32_t* edges, uint32_t* bad) { *edges = *bad = 0; }
static void hwRealBegin() {}
static void hwRealUpdate(uint32_t) {}

#else
#include <driver/rmt.h>

// ---- Sound: a tiny player for glides. f0 -> f1 Hz over ms, f0 = 0 is a rest.
struct Step { uint16_t f0, f1, ms; };
static const Step S_BEEP[] = {{2500, 2500, 30}, {}};  // short, like a clock tick
static const Step S_TAUNT[] = {{600, 500, 150}, {0, 0, 40}, {550, 450, 150}, {0, 0, 40}, {500, 200, 500}, {}};
static const Step S_FANFARE[] = {{523, 523, 120}, {659, 659, 120}, {784, 784, 120}, {1047, 1047, 350}, {}};
static const Step S_BUZZ[] = {{180, 180, 400}, {}};
static const Step S_CELEBRATE[] = {{1047, 1047, 80}, {1319, 1319, 80}, {1568, 1568, 80}, {2093, 2093, 200}, {}};
static const Step S_FIRE[] = {{3000, 800, 90}, {}};
static const Step S_GO[] = {{880, 880, 150}, {0, 0, 350}, {880, 880, 150}, {0, 0, 350}, {1760, 1760, 600}, {}};  // ready, set, GO
static const Step S_TICK[] = {{2000, 2000, 60}, {}};  // last 10 s of the hunt
static const Step S_COMBO[] = {{1047, 1047, 60}, {1319, 1319, 60}, {1568, 1568, 60}, {2093, 2093, 60},
                               {1568, 1568, 60}, {2093, 2093, 60}, {2637, 2637, 300}, {}};
static const Step S_LAST[] = {{440, 440, 150}, {0, 0, 50}, {440, 440, 150}, {0, 0, 50}, {330, 330, 150}, {0, 0, 50},
                              {523, 523, 600}, {}};  // one left: final duel
static const Step* const SOUNDS[] = {S_BEEP, nullptr, S_TAUNT, S_FANFARE, S_BUZZ, S_CELEBRATE, S_FIRE, nullptr, S_GO, S_TICK, S_COMBO, S_LAST};
#define SPK_CH 0
static const Step* step;
static uint32_t stepAt;
static uint16_t lastFreq;

// Clips play as PCM: a 78 kHz PWM carrier whose duty follows 16 kHz samples. The speaker can't
// follow 78 kHz and only moves with the audio. Each role builds in only the clips it plays.
// A 16 kHz timer interrupt feeds the samples, so the loop keeps running during a clip.
struct Clip { const uint8_t* pcm; uint16_t n; };
#include "bell_pcm.h"  // hunt start, on every device
#if defined(ROLE_BODY)
#include "whistle_pcm.h"  // our own recorded wolf whistle
#include "splat_pcm.h"
static const Clip CLIPS[] = {{}, {WHISTLE_PCM, sizeof WHISTLE_PCM}, {SPLAT_PCM, sizeof SPLAT_PCM}, {}, {}, {}, {},
                             {BELL_PCM, sizeof BELL_PCM}};
#else
#include "paintball_pcm.h"
static const Clip CLIPS[] = {{}, {}, {}, {}, {}, {}, {PAINTBALL_PCM, sizeof PAINTBALL_PCM}, {BELL_PCM, sizeof BELL_PCM}};
#endif
static const uint8_t* volatile clipPcm;
static volatile uint16_t clipN, clipI;

static void clipIsr() {
  const uint8_t* p = clipPcm;
  if (!p) return;
  if (clipI < clipN) {
    ledcWrite(SPK_CH, p[clipI++]);
  } else {
    ledcWrite(SPK_CH, 0);
    clipPcm = nullptr;
  }
}

void hw_sound(Snd s) {
  const Clip* c = s < sizeof CLIPS / sizeof *CLIPS && CLIPS[s].pcm ? &CLIPS[s] : nullptr;  // tones sit past the end
  clipPcm = nullptr;  // a new sound cuts off the old one
  step = c ? nullptr : SOUNDS[s];
  stepAt = millis();
  lastFreq = 0xFFFF;  // force the tone player to set the channel again
  if (!c) return;
  ledcSetup(SPK_CH, 78125, 8);
  lastFreq = 0;
  clipI = 0;
  clipN = c->n;
  clipPcm = c->pcm;
}

static void soundBegin() {
  hw_timer_t* t = timerBegin(1, 5, true);  // 80 MHz / 5 = 16 MHz
  timerAttachInterrupt(t, clipIsr, true);
  timerAlarmWrite(t, 1000, true);  // 16 kHz
  timerAlarmEnable(t);
}

static void soundUpdate(uint32_t now) {
  if (clipPcm) return;  // the interrupt owns the speaker until the clip ends
  uint16_t f = 0;
  while (step && step->ms && now - stepAt >= step->ms) {
    stepAt += step->ms;
    step++;
  }
  if (step && !step->ms) step = nullptr;
  if (step) f = step->f0 ? step->f0 + ((int32_t)step->f1 - step->f0) * (int32_t)(now - stepAt) / step->ms : 0;
  if (f != lastFreq) {
    lastFreq = f;
    ledcWriteTone(SPK_CH, f);
  }
}

#if defined(ROLE_BODY)
// RGB LED on LEDC 2..4 (channel 0 is the speaker and shares a timer with 1).
static const uint8_t RGB_PIN[3] = {PIN_RGB_R, PIN_RGB_G, PIN_RGB_B};
#define RGB_CH 2

static void rgb(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t c[3] = {r, g, b};
  for (int i = 0; i < 3; i++) ledcWrite(RGB_CH + i, 255 - c[i] * RGB_BRIGHTNESS / 255);  // common anode: inverted
}

// Hue 0..255 around the colour wheel at full brightness.
static void wheel(uint8_t h) {
  uint8_t x = (h % 85) * 3;
  if (h < 85) rgb(255 - x, x, 0);
  else if (h < 170) rgb(0, 255 - x, x);
  else rgb(x, 0, 255 - x);
}

// ---- IR receive: TSOP is active low. The ISR times marks and builds the byte.
static volatile uint32_t rxEdges, rxBad, lastEdge;
static volatile int16_t rxFrame = -1;
static volatile uint8_t rxBits, rxN;
static volatile bool inFrame;

static void IRAM_ATTR tsopIsr() {
  uint32_t t = micros(), d = t - lastEdge;
  lastEdge = t;
  rxEdges++;
  if (!digitalRead(PIN_TSOP)) {  // a mark started, d was the space before it
    if (inFrame && d > 2 * IR_SPACE) inFrame = false;
    return;
  }
  // a mark ended, d was its length
  if (d > IR_HDR_MARK - 600 && d < IR_HDR_MARK + 800) {
    inFrame = true;
    rxN = rxBits = 0;
  } else if (inFrame) {
    if (d > IR_ZERO_MARK / 2 && d < (IR_ZERO_MARK + IR_ONE_MARK) / 2) rxBits <<= 1;
    else if (d >= (IR_ZERO_MARK + IR_ONE_MARK) / 2 && d < IR_ONE_MARK + 500) rxBits = rxBits << 1 | 1;
    else { inFrame = false; rxBad++; return; }
    if (++rxN == 8) {
      rxFrame = rxBits;
      inFrame = false;
    }
  }
}

void hw_fire() {}
void hw_scan() {}

static void pixelUpdate(uint32_t now) {
  static uint32_t last;
  if (now - last < 20) return;
  last = now;
  switch (pix) {
    case PIX_IDLE: rgb(0, 40, 30); break;
    case PIX_TICK: rgb(255, 255, 255); break;  // white flash with each hide tick
    case PIX_GO: rgb(0, 255, 0); break;  // hide just started: go paint
    case PIX_DARK: (now / 150) % 2 ? rgb(255, 120, 0) : rgb(0, 0, 0); break;
    case PIX_COVERED: (now / 250) % 2 ? rgb(255, 120, 0) : rgb(0, 0, 0); break;
    case PIX_CAPTURED: (now / 200) % 2 ? rgb(255, 0, 0) : rgb(0, 0, 0); break;
    case PIX_REVEAL: wheel(now / 8); break;
    default: rgb(0, 0, 0); break;
  }
}

static void hwRealBegin() {
  for (int i = 0; i < 3; i++) {
    ledcSetup(RGB_CH + i, 5000, 8);
    ledcAttachPin(RGB_PIN[i], RGB_CH + i);
  }
  rgb(0, 0, 0);
  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_TSOP, INPUT_PULLUP);
  attachInterrupt(PIN_TSOP, tsopIsr, CHANGE);
  ledcSetup(SPK_CH, 2000, 8);
  ledcAttachPin(PIN_BODY_SPK, SPK_CH);
  ledcWriteTone(SPK_CH, 0);
  soundBegin();
}

static void hwRealUpdate(uint32_t now) {
  soundUpdate(now);
  pixelUpdate(now);
  int16_t f = rxFrame;
  if (f < 0) return;
  rxFrame = -1;
  uint8_t hi = f >> 4;
  if ((f & 0xF) != (~hi & 0xF)) { rxBad++; return; }
  if (hi >> 2 == IR_SHOT) irHit = hi & 3;
  else if (hi >> 2 == IR_SCAN) beamMs += SCAN_MS;
}

uint16_t hw_ldr() {
  uint32_t s = 0;
  for (int i = 0; i < 4; i++) s += analogRead(PIN_LDR);
  return s / 4;
}

void hw_ir_stats(uint32_t* edges, uint32_t* bad) {
  *edges = rxEdges;
  *bad = rxBad;
}

#else  // blaster

// ---- IR send: RMT makes the 38 kHz carrier and the timing, so the loop never blocks.
#define IR_CH RMT_CHANNEL_0
#define SHOOTER_ID 0  // ponytail: host blaster only; extra blasters get their id from WELCOME later

static void irSend(uint8_t type, bool wait) {
  if (wait) rmt_wait_tx_done(IR_CH, pdMS_TO_TICKS(30));  // let a scan finish, the shot must go out
  else if (rmt_wait_tx_done(IR_CH, 0) != ESP_OK) return;   // busy: skip this scan
  static rmt_item32_t items[9];
  uint8_t b = irByte(type, SHOOTER_ID);
  items[0] = {{{IR_HDR_MARK, 1, IR_SPACE, 0}}};
  for (int i = 0; i < 8; i++)
    items[i + 1] = {{{(uint16_t)(b & (0x80 >> i) ? IR_ONE_MARK : IR_ZERO_MARK), 1, IR_SPACE, 0}}};
  rmt_write_items(IR_CH, items, 9, false);
}

#define MUZZLE_MS 80
static uint32_t muzzleAt;

void hw_fire() {
  irSend(IR_SHOT, true);
  digitalWrite(PIN_MUZZLE_LED, HIGH);
  muzzleAt = millis();
}
void hw_scan() { irSend(IR_SCAN, false); }
uint16_t hw_ldr() { return 0; }
void hw_ir_stats(uint32_t* edges, uint32_t* bad) { *edges = *bad = 0; }

static void hwRealBegin() {
  rmt_config_t c = RMT_DEFAULT_CONFIG_TX((gpio_num_t)PIN_IR_LED, IR_CH);
  c.clk_div = 80;  // 1 us ticks
  c.tx_config.carrier_en = true;
  c.tx_config.carrier_freq_hz = 38000;
  c.tx_config.carrier_duty_percent = 33;
  c.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
  c.tx_config.idle_output_en = true;
  c.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;  // LED off between frames
  rmt_config(&c);
  rmt_driver_install(IR_CH, 0, 0);
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  pinMode(PIN_MUZZLE_LED, OUTPUT);
  ledcSetup(SPK_CH, 2000, 8);
  ledcAttachPin(PIN_BLASTER_SPK, SPK_CH);
  ledcWriteTone(SPK_CH, 0);
  soundBegin();
}

static void hwRealUpdate(uint32_t now) {
  soundUpdate(now);
  if (now - muzzleAt >= MUZZLE_MS) digitalWrite(PIN_MUZZLE_LED, LOW);

  static bool was = true;
  static uint32_t at;
  bool t = digitalRead(PIN_TRIGGER);
  if (!t && was && now - at > 50) {
    at = now;
    trigger = true;
    downAt = now | 1;
  }
  if (t) downAt = 0;
  was = t;
}
#endif

void hw_pixel(Pix p) { pix = p; }
#endif  // SIM

void hw_begin() {
  pinMode(PIN_USER_LED, OUTPUT);
  digitalWrite(PIN_USER_LED, HIGH);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  hwRealBegin();
}

int hw_ir_hit() {
  int h = irHit;
  irHit = -1;
  return h;
}

uint32_t hw_beam_ms() {
  uint32_t b = beamMs;
  beamMs = 0;
  return b;
}

bool hw_trigger() {
  bool t = trigger;
  trigger = false;
  return t;
}

uint32_t hw_trigger_held() { return downAt ? millis() - downAt : 0; }

bool hw_cmd(String& out) {
  if (!havePending) return false;
  out = pending;
  havePending = false;
  return true;
}

// SIM commands: hit [shooter], scan N (seconds in the beam), ldr N, cover, uncover, dark, fire. BOOT = hit (body) / fire (blaster).
static void command(String c) {
#if SIM
  if (c.startsWith("hit")) irHit = c.length() > 4 ? c.substring(4).toInt() : 0;
  else if (c.startsWith("scan ")) beamMs += c.substring(5).toInt() * 1000u;
  else if (c.startsWith("ldr ")) simLdr = c.substring(4).toInt();
  else if (c == "cover") simLdr = 200;
  else if (c == "uncover") simLdr = 2000;
  else if (c == "dark") simLdr = 300;
  else if (c == "fire") trigger = true;
  else
#endif
  {
    pending = c;
    havePending = true;
    return;
  }
  Serial.printf("[sim] %s\n", c.c_str());
}

void hw_update() {
  uint32_t now = millis();
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      line.trim();
      if (line.length()) command(line);
      line = "";
    } else if (line.length() < 64) line += ch;
  }

  // BOOT is a fake hit on a SIM body and a spare trigger on any blaster.
  static bool bootWas = true;
  static uint32_t bootAt;
  bool boot = digitalRead(PIN_BOOT_BTN);
  if (!boot && bootWas && now - bootAt > 50) {
    bootAt = now;
#if defined(ROLE_BODY)
#if SIM
    irHit = 0;
#endif
#else
    trigger = true;
    downAt = now | 1;
#endif
  }
#if !defined(ROLE_BODY)
  if (boot && !bootWas) downAt = 0;
#endif
  bootWas = boot;

  hwRealUpdate(now);

  // User LED shows the pixel state on every build: period in ms, 0 = solid.
  static const int16_t period[] = {-1, 1000, 500, 80, 150, 0, 50, 0, 100};
  int16_t p = period[pix];
  bool on = p == 0 || (p > 0 && (now / p) % 2);
  digitalWrite(PIN_USER_LED, on ? LOW : HIGH);
}
