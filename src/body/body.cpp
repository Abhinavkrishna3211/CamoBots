#include <Arduino.h>
#include <algorithm>
#include "hw.h"
#include "radio.h"

#define PING_MS 500

static uint8_t hostMac[6];
static int myId = -1;

static Config cfg;
static BodyState state = BS_IDLE;
static uint32_t hideEnd, huntEnd, revealEnd;  // millis() end times

// Light sensing
static uint16_t samples[20];
static uint8_t nSamples;
static uint16_t baseline;
static bool dark, coverCheck, covered;
static uint32_t coverEdge;  // when the light last crossed the threshold
static uint8_t strikes;
static uint32_t missedSpotMs;

static uint32_t lastWhistle, lastBeep;

static void setState(BodyState s) {
  state = s;
  Serial.printf("[body %d] state %d\n", myId, s);
}

static void send(uint8_t type, const void* p, size_t n) {
  if (myId >= 0) radio_send(hostMac, type, p, n);
}

static void whistle(uint32_t now) {
  lastWhistle = now;
  hw_sound(SND_WHISTLE);
  WhistleMsg w{(uint8_t)myId};
  send(MSG_WHISTLE, &w, sizeof w);
}

static void startHide(const HideStartMsg& h, uint32_t now) {
  cfg = h.cfg;
  hideEnd = now + h.hideMs;
  revealEnd = hideEnd + h.huntMs + 10000u;  // fallback if END is lost; END sets the real one
  huntEnd = hideEnd + h.huntMs;
  nSamples = 0;
  dark = covered = false;
  coverCheck = true;
  strikes = 0;
  missedSpotMs = 0;
  lastBeep = 0;
  setState(BS_HIDE);
}

static void startHunt(uint32_t now) {
  if (nSamples) {
    std::sort(samples, samples + nSamples);
    baseline = samples[nSamples / 2];
  } else {
    baseline = hw_ldr();  // joined late: no samples taken
  }
  dark = baseline < DARK_ADC;
  coverCheck = !dark;
  Serial.printf("[body %d] hunt, baseline %u%s\n", myId, baseline, dark ? " TOO DARK, cover check off" : "");
  lastWhistle = now;
  setState(BS_HUNT);
}

static void captured() {
  hw_sound(SND_TAUNT);
  setState(BS_CAPTURED);
}

static void onRadio(const RadioMsg& m, uint32_t now) {
  switch (m.h.type) {
    case MSG_WELCOME:  // any time: the host may have rebooted and renumbered us
      memcpy(hostMac, m.mac, 6);
      myId = ((WelcomeMsg*)m.payload)->id;
      Serial.printf("[body] joined host as body %d\n", myId);
      break;
    case MSG_HIDE_START:
      startHide(*(HideStartMsg*)m.payload, now);
      break;
    case MSG_START:
      huntEnd = now + ((StartMsg*)m.payload)->huntMs;
      if (state == BS_HIDE) startHunt(now);
      break;
    case MSG_WHISTLE:  // manual whistle from the host page
      if (state == BS_HUNT) whistle(now);
      break;
    case MSG_END: {
      auto* e = (EndMsg*)m.payload;
      bool alive = e->aliveMask & (1 << myId);
      revealEnd = now + cfg.revealSec * 1000u;  // captured bodies keep flashing red until then
      if (alive && (state == BS_HUNT || state == BS_HIDE)) {
        hw_sound(SND_FANFARE);
        setState(BS_REVEAL);
      } else if (state != BS_REVEAL && state != BS_CAPTURED) {
        setState(BS_IDLE);
      }
      break;
    }
  }
}

static void hideTick(uint32_t now) {
  int32_t left = hideEnd - now;
  if (left <= 0) return startHunt(now);
  uint16_t l = hw_ldr();
  if (left <= 10000) {
    dark = l < DARK_ADC;
    // Countdown beeps: once a second, twice a second in the last 3 s.
    uint32_t gap = left <= 3000 ? 500 : 1000;
    if (now - lastBeep >= gap) {
      lastBeep = now;
      hw_sound(SND_BEEP);
    }
  }
  static uint32_t lastSample;
  if (left <= 2000 && nSamples < 20 && now - lastSample >= 100) {
    lastSample = now;
    samples[nSamples++] = l;
  }
}

static void huntTick(uint32_t now) {
  if ((int32_t)(now - huntEnd) > 5000) return setState(BS_IDLE);  // END never came

  int hit = hw_ir_hit();
  if (hit >= 0) {
    FoundMsg f{(uint8_t)myId, (uint8_t)hit};
    send(MSG_FOUND, &f, sizeof f);
    Serial.printf("[body %d] hit by seeker %d\n", myId, hit);
    return captured();
  }
  missedSpotMs += hw_beam_ms();

  if (coverCheck) {
    bool low = hw_ldr() < (uint32_t)baseline * cfg.coverPct / 100;
    if (low == covered) coverEdge = now;  // no change pending
    else if (now - coverEdge >= (covered ? COVER_OFF_MS : COVER_ON_MS)) {
      covered = low;
      coverEdge = now;
      if (covered) strikes++;
      bool out = covered && cfg.strikes && strikes >= cfg.strikes;
      CoveredMsg c{(uint8_t)myId, covered, strikes, out};
      send(MSG_COVERED, &c, sizeof c);
      Serial.printf("[body %d] %s, strikes %u%s\n", myId, covered ? "COVERED" : "clear", strikes,
                    out ? ", STRUCK OUT" : "");
      if (out) return captured();
      if (covered) whistle(now);
    }
  }

  uint32_t gap = cfg.whistleSec * 1000u;
  if (covered) gap = 1500;  // a buried body gives itself away
  else if ((int32_t)(huntEnd - now) <= PANIC_SEC * 1000) gap /= 2;
  if (gap && now - lastWhistle >= gap) whistle(now);
}

void body_setup() {
  Serial.println("[body] boot");
  hw_begin();
  radio_begin(false);
}

void body_loop() {
  uint32_t now = millis();
  hw_update();

  RadioMsg m;
  while (radio_poll(&m)) onRadio(m, now);

  // ignore stray hits outside the hunt
  if (state != BS_HUNT) {
    hw_ir_hit();
    hw_beam_ms();
  }

  switch (state) {
    case BS_IDLE: break;
    case BS_HIDE: hideTick(now); break;
    case BS_HUNT: huntTick(now); break;
    case BS_CAPTURED:
    case BS_REVEAL:
      if ((int32_t)(now - revealEnd) >= 0) setState(BS_IDLE);
      break;
  }

  Pix p = PIX_OFF;  // hunting and alive: dark, never gives the body away
  if (myId < 0) p = PIX_IDLE;
  else if (state == BS_HIDE) p = dark ? PIX_DARK : PIX_HIDE;
  else if (state == BS_HUNT && covered) p = PIX_COVERED;
  else if (state == BS_CAPTURED) p = PIX_CAPTURED;
  else if (state == BS_REVEAL) p = PIX_REVEAL;
  else if (state == BS_IDLE) p = PIX_IDLE;
  hw_pixel(p);

  String c;
  if (hw_cmd(c)) Serial.printf("[body %d] ch=%u rx=%lu ackfail=%lu state=%d ldr=%u baseline=%u strikes=%u spot=%lums (unknown cmd '%s')\n", myId,
                               radio_channel(), radio_rx_raw(), radio_ack_fails(),
                               state, hw_ldr(), baseline, strikes, missedSpotMs, c.c_str());

  static uint32_t lastTx;
  if (myId < 0) {
    if (now - lastTx >= 1000) {
      lastTx = now;
      HelloMsg h{UNIT_BODY, 0xFF};
      radio_send(BCAST, MSG_HELLO, &h, sizeof h);
    }
  } else if (now - lastTx >= PING_MS) {
    lastTx = now;
    PingMsg p{(uint8_t)myId, now, radio_ack_fails(), state,
              (uint8_t)((dark ? BF_DARK : 0) | (covered ? BF_COVERED : 0)), hw_ldr(), missedSpotMs};
    radio_send(hostMac, MSG_PING, &p, sizeof p);
  }
}
