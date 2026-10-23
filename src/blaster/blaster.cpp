#include <Arduino.h>
#include <WebServer.h>
#include "hw.h"
#include "radio.h"

enum Phase : uint8_t { PH_IDLE, PH_HIDE, PH_HUNT, PH_REVEAL };
static const char* PHASE_NAME[] = {"idle", "hide", "hunt", "reveal"};
static const char* STATE_NAME[] = {"idle", "hide", "hunt", "captured", "reveal"};

struct Body {
  uint8_t mac[6];
  bool used;
  // link
  uint32_t rx, lost, lastSeen, peerAckFails;
  uint16_t lastSeq;
  bool haveSeq;
  uint8_t state, flags;
  uint16_t ldr;
  // round
  bool alive, covered, struckOut;
  uint8_t strikes, foundBy;
  uint32_t foundAt, lastWhistle;  // ms since hunt start
};

static Body bodies[MAX_BODIES];
static WebServer web(80);

static Config cfg;
static Phase phase = PH_IDLE;
static uint32_t phaseStart, phaseEnd;  // millis()
static uint32_t shotAt, lockUntil;
static bool shotPending;
static uint16_t shots, misses;
static uint8_t hp;

static int bodyIdFor(const uint8_t* mac) {
  for (int i = 0; i < MAX_BODIES; i++)
    if (bodies[i].used && memcmp(bodies[i].mac, mac, 6) == 0) return i;
  for (int i = 0; i < MAX_BODIES; i++)
    if (!bodies[i].used) {
      bodies[i] = {};
      bodies[i].used = true;
      memcpy(bodies[i].mac, mac, 6);
      return i;
    }
  return -1;
}

static uint32_t huntMs(uint32_t now) { return phase == PH_HUNT ? now - phaseStart : 0; }

static void setPhase(Phase p, uint32_t now, uint32_t lenMs) {
  phase = p;
  phaseStart = now;
  phaseEnd = now + lenMs;
  Serial.printf("[game] phase %s (%lu s)\n", PHASE_NAME[p], lenMs / 1000);
}

static void sendHideStart(int i, uint32_t now) {
  HideStartMsg h;
  h.cfg = cfg;
  h.hideMs = phase == PH_HIDE ? phaseEnd - now : 0;
  h.huntMs = phase == PH_HIDE ? cfg.huntSec * 1000u : phaseEnd - now;
  radio_send(bodies[i].mac, MSG_HIDE_START, &h, sizeof h);
}

static uint8_t aliveMask() {
  uint8_t m = 0;
  for (int i = 0; i < MAX_BODIES; i++)
    if (bodies[i].used && bodies[i].alive) m |= 1 << i;
  return m;
}

static void sendEnd(int i, bool seekerWon) {
  EndMsg e{aliveMask(), seekerWon};
  radio_send(bodies[i].mac, MSG_END, &e, sizeof e);
}

static void startRound(uint32_t now) {
  setPhase(PH_HIDE, now, cfg.hideSec * 1000u);
  shots = misses = 0;
  hp = cfg.hp;
  shotPending = false;
  lockUntil = 0;
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    b.alive = true;
    b.covered = b.struckOut = false;
    b.strikes = 0;
    b.foundAt = b.lastWhistle = 0;
    sendHideStart(i, now);
  }
}

static void endRound(uint32_t now, bool seekerWon, const char* why) {
  setPhase(PH_REVEAL, now, cfg.revealSec * 1000u);
  Serial.printf("[game] END: %s (%s). shots=%u misses=%u\n", seekerWon ? "SEEKER wins" : "HIDERS win", why, shots,
                misses);
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    Serial.printf("[game]   body %d: %s", i, b.alive ? "survived" : b.struckOut ? "struck out" : "found");
    if (!b.alive && !b.struckOut) Serial.printf(" by seeker %u at %lu ms", b.foundBy, b.foundAt);
    Serial.printf(", strikes %u\n", b.strikes);
    sendEnd(i, seekerWon);
  }
  hw_sound(seekerWon ? SND_CELEBRATE : SND_FANFARE);
}

static void checkAllFound(uint32_t now) {
  if (phase == PH_HUNT && aliveMask() == 0) endRound(now, true, "all bodies captured");
}

static void onRadio(const RadioMsg& m, uint32_t now) {
  if (m.h.type == MSG_HELLO) {
    if (((HelloMsg*)m.payload)->role != UNIT_BODY) return;  // extra blasters: later
    int id = bodyIdFor(m.mac);
    if (id < 0) return;
    WelcomeMsg w{UNIT_BODY, (uint8_t)id};
    radio_send(m.mac, MSG_WELCOME, &w, sizeof w);
    bodies[id].haveSeq = false;
    Serial.printf("[link] body %d joined %02X:%02X:%02X\n", id, m.mac[3], m.mac[4], m.mac[5]);
    return;
  }

  // Trust the MAC, not the id in the payload: after a host reboot the table is empty.
  int id = bodyIdFor(m.mac);
  if (id < 0) return;
  auto& b = bodies[id];

  switch (m.h.type) {
    case MSG_PING: {
      auto* p = (PingMsg*)m.payload;
      if (id != p->id) {
        WelcomeMsg w{UNIT_BODY, (uint8_t)id};
        radio_send(m.mac, MSG_WELCOME, &w, sizeof w);
        b.haveSeq = false;
        Serial.printf("[link] body %d re-joined\n", id);
        return;
      }
      if (b.haveSeq) b.lost += (uint16_t)(m.h.seq - b.lastSeq - 1);
      b.lastSeq = m.h.seq;
      b.haveSeq = true;
      b.rx++;
      b.lastSeen = now;
      b.peerAckFails = p->ackFails;
      b.state = p->state;
      b.flags = p->flags;
      b.ldr = p->ldr;
      // Self-heal: a body that missed a phase message (or rebooted) gets it again.
      bool bodyIdle = p->state == BS_IDLE;
      if ((phase == PH_HIDE || phase == PH_HUNT) && b.alive && bodyIdle) sendHideStart(id, now);
      else if (phase == PH_HUNT && p->state == BS_HIDE) {
        StartMsg s{phaseEnd - now};
        radio_send(m.mac, MSG_START, &s, sizeof s);
      } else if (phase == PH_IDLE && (p->state == BS_HIDE || p->state == BS_HUNT)) sendEnd(id, false);
      break;
    }
    case MSG_FOUND: {
      auto* f = (FoundMsg*)m.payload;
      if (phase != PH_HUNT || !b.alive) return;  // duplicate or late
      b.alive = false;
      b.foundBy = f->shooter;
      b.foundAt = huntMs(now);
      shotPending = false;  // the shot hit
      Serial.printf("[game] FOUND body %d by seeker %u at %lu ms", id, f->shooter, b.foundAt);
      if (b.lastWhistle && b.foundAt - b.lastWhistle <= 10000)
        Serial.printf(" (%lu ms after its whistle)", b.foundAt - b.lastWhistle);
      Serial.println();
      hw_sound(SND_CELEBRATE);
      checkAllFound(now);
      break;
    }
    case MSG_WHISTLE:
      if (phase == PH_HUNT) b.lastWhistle = huntMs(now);
      break;
    case MSG_COVERED: {
      auto* c = (CoveredMsg*)m.payload;
      if (phase != PH_HUNT || !b.alive) return;
      b.covered = c->on;
      b.strikes = c->strikes;
      Serial.printf("[game] body %d %s, strikes %u\n", id, c->on ? "COVERED" : "clear", c->strikes);
      if (c->out) {
        b.alive = false;
        b.struckOut = true;
        b.foundAt = huntMs(now);
        Serial.printf("[game] body %d STRUCK OUT\n", id);
        checkAllFound(now);
      }
      break;
    }
  }
}

static void manualWhistle(int i) {
  if (phase != PH_HUNT || i < 0 || i >= MAX_BODIES || !bodies[i].used || !bodies[i].alive) return;
  radio_send(bodies[i].mac, MSG_WHISTLE, nullptr, 0);
  Serial.printf("[game] manual whistle -> body %d\n", i);
}

// Serial: start, stop, whistle N, set hide|hunt|whistle|reveal|lock|hp|strikes N, status
static void command(String c, uint32_t now) {
  if (c == "start") {
    if (phase == PH_IDLE) startRound(now);
  } else if (c == "stop") {
    if (phase == PH_HIDE || phase == PH_HUNT) endRound(now, false, "stopped");
  } else if (c.startsWith("whistle ")) {
    manualWhistle(c.substring(8).toInt());
  } else if (c.startsWith("set ")) {
    int sp = c.indexOf(' ', 4);
    String k = c.substring(4, sp);
    int v = c.substring(sp + 1).toInt();
    if (k == "hide") cfg.hideSec = v;
    else if (k == "hunt") cfg.huntSec = v;
    else if (k == "whistle") cfg.whistleSec = v;
    else if (k == "reveal") cfg.revealSec = v;
    else if (k == "lock") cfg.missLockMs = v;
    else if (k == "hp") cfg.hp = v;
    else if (k == "strikes") cfg.strikes = v;
    else return (void)Serial.printf("[game] unknown setting '%s'\n", k.c_str());
    Serial.printf("[game] %s = %d\n", k.c_str(), v);
  } else {
    Serial.printf("[game] ch=%u ack ok=%lu fail=%lu phase=%s hide=%u hunt=%u whistle=%u reveal=%u lock=%u hp=%u strikes=%u\n",
                  radio_channel(), radio_ack_ok(), radio_ack_fails(), PHASE_NAME[phase], cfg.hideSec, cfg.huntSec, cfg.whistleSec, cfg.revealSec, cfg.missLockMs,
                  cfg.hp, cfg.strikes);
  }
}

static void handleRoot() {
  uint32_t now = millis();
  int32_t left = phase == PH_IDLE ? 0 : (int32_t)(phaseEnd - now) / 1000;
  String s = F("<!doctype html><meta name=viewport content='width=device-width'>"
               "<meta http-equiv=refresh content=1><body style='background:#113;color:#7f7;font:16px monospace'>"
               "<h2>CamoBots</h2>");
  s += "<p>phase <b>" + String(PHASE_NAME[phase]) + "</b>, " + left + " s left, shots " + shots + ", misses " +
       misses + (cfg.hp ? ", HP " + String(hp) : "") + "</p>";
  s += F("<table border=1 cellpadding=4><tr><th>body<th>state<th>alive<th>strikes<th>light<th>lost<th>seen ms</tr>");
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    s += "<tr><td>" + String(i) + "<td>" + STATE_NAME[b.state % 5] + (b.flags & BF_DARK ? " DARK" : "") +
         (b.flags & BF_COVERED ? " COVERED" : "") + "<td>" + (b.alive ? "yes" : "no") + "<td>" + b.strikes +
         "<td>" + b.ldr + "<td>" + b.lost + "<td>" + (now - b.lastSeen) + "</tr>";
  }
  s += F("</table><p><a href=/start style=color:#ff7>start</a> &nbsp; <a href=/stop style=color:#ff7>stop</a></p>");
  web.send(200, "text/html", s);
}

static void redirectHome() {
  web.sendHeader("Location", "/");
  web.send(303);
}

void blaster_setup() {
  Serial.println("[blaster] boot");
  hw_begin();
  radio_begin(true);
  web.on("/", handleRoot);
  web.on("/start", [] { command("start", millis()); redirectHome(); });
  web.on("/stop", [] { command("stop", millis()); redirectHome(); });
  web.begin();
  Serial.println("[blaster] web on http://192.168.4.1");
}

void blaster_loop() {
  uint32_t now = millis();
  web.handleClient();
  hw_update();

  RadioMsg m;
  while (radio_poll(&m)) onRadio(m, now);

  String c;
  if (hw_cmd(c)) command(c, now);

  bool fired = hw_trigger();
  switch (phase) {
    case PH_IDLE: break;
    case PH_HIDE:
      if (fired) Serial.println("[game] trigger locked during hide");
      if ((int32_t)(now - phaseEnd) >= 0) {
        setPhase(PH_HUNT, now, cfg.huntSec * 1000u);
        StartMsg s{cfg.huntSec * 1000u};
        for (int i = 0; i < MAX_BODIES; i++)
          if (bodies[i].used) radio_send(bodies[i].mac, MSG_START, &s, sizeof s);
      }
      break;
    case PH_HUNT:
      if (fired) {
        if ((int32_t)(now - lockUntil) < 0 || shotPending) {
          Serial.println("[game] trigger locked");
        } else {
          shots++;
          shotPending = true;
          shotAt = now;
          hw_fire();
          hw_sound(SND_FIRE);
        }
      }
      if (shotPending && now - shotAt >= SHOT_WINDOW_MS) {
        shotPending = false;
        misses++;
        lockUntil = now + cfg.missLockMs;
        hw_sound(SND_BUZZ);
        Serial.printf("[game] MISS (%u of %u shots), locked %u ms\n", misses, shots, cfg.missLockMs);
        if (cfg.hp && --hp == 0) {
          endRound(now, false, "seeker out of HP");
          break;
        }
      }
      if ((int32_t)(now - phaseEnd) >= 0) endRound(now, false, "time up");
      break;
    case PH_REVEAL:
      if ((int32_t)(now - phaseEnd) >= 0) setPhase(PH_IDLE, now, 0);
      break;
  }

  Pix p = PIX_IDLE;
  if (phase == PH_HIDE || (phase == PH_HUNT && (int32_t)(now - lockUntil) < 0)) p = PIX_LOCKED;
  else if (phase == PH_HUNT) p = PIX_LIVE;
  else if (phase == PH_REVEAL) p = PIX_REVEAL;
  hw_pixel(p);

  static uint32_t lastLog;
  if (now - lastLog >= 5000) {
    lastLog = now;
    for (int i = 0; i < MAX_BODIES; i++)
      if (bodies[i].used)
        Serial.printf("[link] body %d %s rx=%lu lost=%lu seen=%lums ago\n", i, STATE_NAME[bodies[i].state % 5],
                      bodies[i].rx, bodies[i].lost, now - bodies[i].lastSeen);
  }
}
