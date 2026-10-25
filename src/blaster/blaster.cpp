#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "pins.h"
#include "hw.h"
#include "radio.h"
#include "scoring.h"

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
  // round (times are ms since hunt start)
  bool alive, covered, struckOut, whistled;
  uint8_t strikes, foundBy;
  uint32_t foundAt, lastWhistle, coverOnAt, coveredMs, missedSpotMs;
};

static Body bodies[MAX_BODIES];
static WebServer web(80);
static Preferences nvs;

static Config cfg;
static Phase phase = PH_IDLE;
static uint32_t phaseStart, phaseEnd;  // millis()
static uint32_t shotAt, lockUntil, missAt;
static bool shotPending;
static uint16_t shots, misses;
static uint8_t hp;
static int8_t killBody = -1;  // last capture, shown big on the OLED
static uint32_t killAt;

// Session: names, totals, the last round's result
#define NAME_LEN 12
static Session session;
static char names[MAX_PLAYERS][NAME_LEN + 1];
static uint8_t roundFirst;
static bool haveLast, lastWon, scoredWon;
static const char* lastWhy = "";
static const char* scoredWhy = "";
static RoundResult lastRes;
static RoundScore lastScore;

// Event feed for the page: sounds, kill feed. The page tracks the last n it saw.
enum EvKind : uint8_t { EV_JOIN, EV_ROUND, EV_HUNT, EV_SHOT, EV_MISS, EV_FOUND, EV_WHISTLE, EV_COVER, EV_CLEAR,
                        EV_OUT, EV_END };
static const char* EV_NAME[] = {"join", "round", "hunt", "shot", "miss", "found", "whistle", "cover", "clear", "out", "end"};
struct Event { uint16_t n; uint8_t kind; int8_t body, who; uint32_t t; };
#define NUM_EVENTS 16
static Event events[NUM_EVENTS];
static uint16_t evCount;

static void event(EvKind k, int body = -1, int who = -1) {
  Event& e = events[evCount % NUM_EVENTS];
  e = {++evCount, k, (int8_t)body, (int8_t)who, phase == PH_HUNT ? millis() - phaseStart : 0};
}

static void saveConfig() {
  nvs.putBytes("cfg", &cfg, sizeof cfg);
  nvs.putBytes("names", names, sizeof names);
}

static void saveSession() { nvs.putBytes("sess", &session, sizeof session); }

static void load() {
  nvs.begin("camobots");
  for (int i = 0; i < MAX_PLAYERS; i++) snprintf(names[i], sizeof names[i], "PLAYER %d", i + 1);
  if (nvs.getBytesLength("cfg") == sizeof cfg) nvs.getBytes("cfg", &cfg, sizeof cfg);
  if (nvs.getBytesLength("names") == sizeof names) nvs.getBytes("names", names, sizeof names);
  if (nvs.getBytesLength("sess") == sizeof session) nvs.getBytes("sess", &session, sizeof session);
}

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

static void closeCover(Body& b, uint32_t t) {
  if (!b.covered) return;
  b.covered = false;
  b.coveredMs += t - b.coverOnAt;
}

static void startRound(uint32_t now) {
  setPhase(PH_HIDE, now, cfg.hideSec * 1000u);
  shots = misses = 0;
  hp = cfg.hp;
  shotPending = false;
  lockUntil = missAt = 0;
  roundFirst = session.nextFirst;
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    b.alive = true;
    b.covered = b.struckOut = b.whistled = false;
    b.strikes = 0;
    b.foundAt = b.lastWhistle = b.coveredMs = b.missedSpotMs = 0;
    sendHideStart(i, now);
  }
  event(EV_ROUND);
}

static void scoreRound(uint32_t endMs) {
  RoundResult& r = lastRes;
  r = {};
  r.huntMs = cfg.huntSec * 1000u;
  r.endMs = endMs;
  r.numSeekers = session.numSeekers;
  r.shots[0] = shots;  // ponytail: host blaster only; extra blasters report MSG_SHOT later
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    closeCover(b, b.alive ? endMs : b.foundAt);
    BodyResult& o = r.body[i];
    o.used = true;
    o.alive = b.alive;
    o.struckOut = b.struckOut;
    o.foundBy = b.foundBy;
    o.outAtMs = b.foundAt;
    o.coveredMs = b.coveredMs;
    o.coverEvents = b.strikes;
    o.whistleHunter = !b.alive && is_whistle_hunter(b.foundAt, b.lastWhistle, b.whistled);
    o.missedSpotMs = b.missedSpotMs;
  }
  lastScore = score_round(r);
  scoredWon = lastWon;
  scoredWhy = lastWhy;
  session_add(session, r, lastScore);
  saveSession();
  haveLast = true;

  uint8_t sk[MAX_PLAYERS], hd[MAX_PLAYERS];
  rotation(roundFirst, session.numPlayers, session.numSeekers, sk, hd);
  for (int k = 0; k < session.numSeekers; k++)
    Serial.printf("[score] seeker %s: %d (hits %u)\n", names[sk[k]], lastScore.seeker[k], lastScore.hits[k]);
  for (int i = 0; i < session.numPlayers - session.numSeekers && i < MAX_BODIES; i++)
    if (r.body[i].used)
      Serial.printf("[score] body %d %s: %d (survived %d s, spot %d%s%s)\n", i, names[hd[i]], lastScore.body[i],
                    lastScore.survival[i], lastScore.missedSpot[i], lastScore.ghost[i] ? ", GHOST" : "",
                    lastScore.lastChameleon[i] ? ", LAST CHAMELEON" : "");
  for (int p = 0; p < session.numPlayers; p++)
    Serial.printf("[score] total %s: %ld\n", names[p], session.p[p].points);
}

// scored = false for a round stopped by hand: it does not count.
static void endRound(uint32_t now, bool seekerWon, const char* why, bool scored = true) {
  uint32_t endMs = huntMs(now);
  if (phase == PH_HIDE) scored = false;
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
  lastWon = seekerWon;
  lastWhy = why;
  if (scored) scoreRound(endMs);
  event(EV_END, -1, seekerWon);
  hw_sound(seekerWon ? SND_CELEBRATE : SND_FANFARE);
}

static void checkAllFound(uint32_t now) {
  if (phase == PH_HUNT && aliveMask() == 0) endRound(now, true, "all bodies captured");
}

// FOUND, or a ping saying the body was hit (the FOUND was lost).
static void found(int id, uint8_t shooter, uint32_t now) {
  auto& b = bodies[id];
  if (phase != PH_HUNT || !b.alive) return;  // duplicate or late
  b.alive = false;
  b.foundBy = shooter;
  b.foundAt = huntMs(now);
  closeCover(b, b.foundAt);
  if (shotPending) shotPending = false;  // the shot hit
  else if (missAt && now - missAt < 1500) {  // it hit, but the news came after the window: undo the miss
    misses--;
    if (cfg.hp) hp++;
    lockUntil = missAt = 0;
    Serial.println("[game] late FOUND, miss refunded");
  }
  Serial.printf("[game] FOUND body %d by seeker %u at %lu ms", id, shooter, b.foundAt);
  if (is_whistle_hunter(b.foundAt, b.lastWhistle, b.whistled)) Serial.print(" WHISTLE HUNTER");
  Serial.println();
  event(EV_FOUND, id, shooter);
  killBody = id;
  killAt = now;
  hw_sound(SND_CELEBRATE);
  checkAllFound(now);
}

// COVERED, or a ping whose cover flag disagrees with ours.
static void setCovered(int id, bool on, uint8_t strikes, bool out, uint32_t now) {
  auto& b = bodies[id];
  if (phase != PH_HUNT || !b.alive) return;
  uint32_t t = huntMs(now);
  if (on != b.covered || strikes != b.strikes) {
    if (on && !b.covered) b.coverOnAt = t;
    if (!on) closeCover(b, t);
    b.covered = on;
    b.strikes = strikes;
    Serial.printf("[game] body %d %s, strikes %u\n", id, on ? "COVERED" : "clear", strikes);
    event(on ? EV_COVER : EV_CLEAR, id, strikes);
  }
  if (out) {
    b.alive = false;
    b.struckOut = true;
    b.foundAt = t;
    closeCover(b, t);
    Serial.printf("[game] body %d STRUCK OUT\n", id);
    event(EV_OUT, id);
    checkAllFound(now);
  }
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
    event(EV_JOIN, id);
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
      if (phase == PH_HUNT && b.alive) {
        b.missedSpotMs = p->beamMs;
        if (p->hitBy < MAX_SEEKERS) found(id, p->hitBy, now);
        else setCovered(id, p->flags & BF_COVERED, p->strikes, p->hitBy == HIT_STRUCK_OUT, now);
      }
      // Self-heal: a body that missed a phase message (or rebooted) gets it again.
      bool bodyIdle = p->state == BS_IDLE;
      if ((phase == PH_HIDE || phase == PH_HUNT) && b.alive && bodyIdle) sendHideStart(id, now);
      else if (phase == PH_HUNT && p->state == BS_HIDE) {
        StartMsg s{phaseEnd - now};
        radio_send(m.mac, MSG_START, &s, sizeof s);
      } else if (phase == PH_IDLE && (p->state == BS_HIDE || p->state == BS_HUNT)) sendEnd(id, false);
      break;
    }
    case MSG_FOUND:
      found(id, ((FoundMsg*)m.payload)->shooter, now);
      break;
    case MSG_WHISTLE:
      if (phase == PH_HUNT && b.alive) {
        b.lastWhistle = huntMs(now);
        b.whistled = true;
        event(EV_WHISTLE, id);
      }
      break;
    case MSG_COVERED: {
      auto* c = (CoveredMsg*)m.payload;
      setCovered(id, c->on, c->strikes, c->out, now);
      break;
    }
  }
}

static void manualWhistle(int i) {
  if (phase != PH_HUNT || i < 0 || i >= MAX_BODIES || !bodies[i].used || !bodies[i].alive) return;
  radio_send(bodies[i].mac, MSG_WHISTLE, nullptr, 0);
  Serial.printf("[game] manual whistle -> body %d\n", i);
}

static void resetSession() {
  uint8_t np = session.numPlayers, ns = session.numSeekers;
  session = Session();
  session.numPlayers = np;
  session.numSeekers = ns;
  haveLast = false;
  saveSession();
  Serial.println("[game] session reset");
}

// Settings shared by serial "set k v" and the page's /config. Returns false for an unknown key.
static bool setting(const String& k, int v) {
  if (k == "hide") cfg.hideSec = constrain(v, 5, 600);
  else if (k == "hunt") cfg.huntSec = constrain(v, 10, 1800);
  else if (k == "whistle") cfg.whistleSec = v <= 0 ? 0 : constrain(v, 5, 300);
  else if (k == "reveal") cfg.revealSec = constrain(v, 1, 60);
  else if (k == "lock") cfg.missLockMs = constrain(v, 0, 10000);
  else if (k == "hp") cfg.hp = constrain(v, 0, 20);
  else if (k == "strikes") cfg.strikes = constrain(v, 0, 9);
  else if (k == "cover") cfg.coverPct = constrain(v, 10, 90);
  else if (k == "players") session.numPlayers = constrain(v, 2, MAX_PLAYERS);
  else if (k == "seekers") session.numSeekers = constrain(v, 1, 1);  // ponytail: 1 until extra blasters exist
  else if (k == "first") session.nextFirst = constrain(v, 0, MAX_PLAYERS - 1);
  else return false;
  session.nextFirst %= session.numPlayers;
  return true;
}

static void setName(int i, const String& v) {
  if (i < 0 || i >= MAX_PLAYERS) return;
  // Only plain characters: names go straight into JSON and the page.
  int n = 0;
  for (size_t k = 0; k < v.length() && n < NAME_LEN; k++) {
    char c = v[k];
    if (isalnum(c) || c == ' ' || c == '-' || c == '_') names[i][n++] = toupper(c);
  }
  if (n) names[i][n] = 0;
}

static String stateJson();

// Serial: start, stop, reset, whistle N, set <key> N, name N X, json, status
static void command(String c, uint32_t now) {
  bool between = phase == PH_IDLE || phase == PH_REVEAL;
  if (c == "start") {
    if (between) startRound(now);
  } else if (c == "stop") {
    if (!between) endRound(now, false, "stopped", false);
  } else if (c == "reset") {
    if (between) resetSession();
  } else if (c == "json") {
    Serial.println(stateJson());
  } else if (c.startsWith("whistle ")) {
    manualWhistle(c.substring(8).toInt());
  } else if (c.startsWith("set ") || c.startsWith("name ")) {
    int a = c.indexOf(' '), sp = c.indexOf(' ', a + 1);
    String k = c.substring(a + 1, sp), v = c.substring(sp + 1);
    if (!between) return (void)Serial.println("[game] settings locked during a round");
    if (c[0] == 'n') setName(k.toInt(), v);
    else if (!setting(k, v.toInt())) return (void)Serial.printf("[game] unknown setting '%s'\n", k.c_str());
    saveConfig();
    saveSession();
    Serial.printf("[game] %s = %s\n", k.c_str(), v.c_str());
  } else {
    Serial.printf("[game] ch=%u ack ok=%lu fail=%lu phase=%s hide=%u hunt=%u whistle=%u reveal=%u lock=%u hp=%u "
                  "strikes=%u cover=%u players=%u first=%u round=%u\n",
                  radio_channel(), radio_ack_ok(), radio_ack_fails(), PHASE_NAME[phase], cfg.hideSec, cfg.huntSec,
                  cfg.whistleSec, cfg.revealSec, cfg.missLockMs, cfg.hp, cfg.strikes, cfg.coverPct,
                  session.numPlayers, session.nextFirst, session.round);
  }
}

// ---- web ----

extern const char index_html[] asm("_binary_web_index_html_start");
extern const char index_html_end[] asm("_binary_web_index_html_end");

static void add(String& s, const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  s += buf;
}

static String stateJson() {
  uint32_t now = millis();
  String s;
  s.reserve(2560);
  int32_t left = phase == PH_IDLE ? 0 : max<int32_t>(0, phaseEnd - now);
  int32_t lock = phase == PH_HUNT ? max<int32_t>(0, lockUntil - now) : 0;
  add(s, "{\"ph\":\"%s\",\"left\":%ld,\"len\":%lu,\"round\":%u,\"first\":%u,\"hp\":%u,\"hpMax\":%u,\"shots\":%u,"
         "\"miss\":%u,\"lock\":%ld,\"won\":%d,\"why\":\"%s\",",
      PHASE_NAME[phase], left, phaseEnd - phaseStart, session.round, phase == PH_IDLE ? session.nextFirst : roundFirst,
      hp, cfg.hp, shots, misses, lock, lastWon, lastWhy);
  add(s, "\"cfg\":{\"hide\":%u,\"hunt\":%u,\"whistle\":%u,\"reveal\":%u,\"lock\":%u,\"hp\":%u,\"strikes\":%u,"
         "\"cover\":%u},\"np\":%u,\"ns\":%u,\"next\":%u,",
      cfg.hideSec, cfg.huntSec, cfg.whistleSec, cfg.revealSec, cfg.missLockMs, cfg.hp, cfg.strikes, cfg.coverPct,
      session.numPlayers, session.numSeekers, session.nextFirst);
  s += "\"names\":[";
  for (int i = 0; i < MAX_PLAYERS; i++) add(s, "%s\"%s\"", i ? "," : "", names[i]);
  s += "],\"tot\":[";
  for (int i = 0; i < session.numPlayers; i++) {
    auto& t = session.p[i];
    add(s, "%s[%ld,%u,%u,%u,%u,%ld,%ld,%u]", i ? "," : "", t.points, t.shots, t.hits, t.whistleHunters,
        t.coverEvents, t.survival, t.missedSpot, t.ghosts);
  }
  s += "],\"badges\":[";
  for (int b = 0; b < BADGE_COUNT; b++) add(s, "%s%d", b ? "," : "", badge_winner(session, (Badge)b));
  s += "],\"bodies\":[";
  bool first = true;
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    uint32_t t = huntMs(now);
    add(s, "%s{\"id\":%d,\"st\":\"%s\",\"alive\":%d,\"cov\":%d,\"strk\":%u,\"dark\":%d,\"ldr\":%u,\"seen\":%lu,"
           "\"ms\":%lu,\"wh\":%ld}",
        first ? "" : ",", i, STATE_NAME[b.state % 5], b.alive, b.covered, b.strikes, (b.flags & BF_DARK) != 0, b.ldr,
        now - b.lastSeen, b.missedSpotMs, phase == PH_HUNT && b.whistled ? (long)(t - b.lastWhistle) : -1L);
    first = false;
  }
  s += "],\"ev\":[";
  for (int k = 0; k < NUM_EVENTS && k < evCount; k++) {
    auto& e = events[(evCount - 1 - k) % NUM_EVENTS];
    add(s, "%s[%u,\"%s\",%d,%d,%lu]", k ? "," : "", e.n, EV_NAME[e.kind], e.body, e.who, e.t);
  }
  s += "],\"last\":";
  if (!haveLast) s += "null";
  else {
    auto& r = lastRes;
    auto& c = lastScore;
    add(s, "{\"won\":%d,\"why\":\"%s\",\"first\":%u,\"sweep\":%d,\"end\":%lu,\"sk\":[", scoredWon, scoredWhy, roundFirst,
        c.cleanSweep, r.endMs);
    for (int k = 0; k < r.numSeekers; k++) add(s, "%s[%d,%u,%u]", k ? "," : "", c.seeker[k], c.hits[k], r.shots[k]);
    s += "],\"bd\":[";
    for (int i = 0; i < MAX_BODIES; i++) {
      auto& o = r.body[i];
      add(s, "%s[%d,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%d,%u]", i ? "," : "", o.used, c.body[i], c.survival[i],
          c.missedSpot[i], c.ghost[i], c.lastChameleon[i], o.alive, o.struckOut, o.foundBy, o.outAtMs,
          o.whistleHunter, o.coverEvents);
    }
    s += "]}";
  }
  s += "}";
  return s;
}

static void handleState() {
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", stateJson());
}

static void handleConfig() {
  if (phase == PH_HIDE || phase == PH_HUNT) return web.send(409, "text/plain", "round running");
  for (int i = 0; i < web.args(); i++) {
    String k = web.argName(i);
    if (k.startsWith("n") && k.length() == 2 && isdigit(k[1])) setName(k[1] - '0', web.arg(i));
    else setting(k, web.arg(i).toInt());
  }
  saveConfig();
  saveSession();
  Serial.println("[game] config from page");
  web.send(204);
}

static void action(const char* cmd) {
  command(cmd, millis());
  web.send(204);
}

// ---- Optional 0.96" OLED: an arcade-style screen with the same game info as the phone.
// Frames are drawn into RAM every 80 ms and only pushed when they changed; a push takes about 25 ms.
static Adafruit_SSD1306 oled(128, 64, &Wire);
static bool haveOled;
#define KILL_MS 3000
static const uint16_t WH = SSD1306_WHITE, BK = SSD1306_BLACK;
static const uint8_t HEART[] PROGMEM = {0x6C, 0xFE, 0xFE, 0x7C, 0x38, 0x10};
static const uint8_t HEART_EMPTY[] PROGMEM = {0x6C, 0x92, 0x82, 0x44, 0x28, 0x10};
static const uint8_t PADLOCK[] PROGMEM = {0x38, 0x44, 0x44, 0xFE, 0xEE, 0xEE, 0xFE};

static void oledBegin() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);
  Wire.beginTransmission(0x3C);
  haveOled = Wire.endTransmission() == 0 && oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oled.setTextWrap(false);
  Serial.printf("[blaster] OLED %s\n", haveOled ? "found" : "not found, skipped");
}

// What the seeker earned for one capture; matches score_round.
static int capturePts(const Body& b) {
  if (b.alive || b.struckOut) return 0;
  uint32_t huntLen = cfg.huntSec * 1000u;
  return PTS_CAPTURE + (huntLen > b.foundAt ? (huntLen - b.foundAt) / 1000 : 0) +
         (is_whistle_hunter(b.foundAt, b.lastWhistle, b.whistled) ? PTS_WHISTLE_HUNTER : 0);
}

static const char* hiderName(int body) {
  uint8_t sk[MAX_PLAYERS], hd[MAX_PLAYERS];
  rotation(roundFirst, session.numPlayers, session.numSeekers, sk, hd);
  return body < session.numPlayers - session.numSeekers ? names[hd[body]] : "?";
}

// Text centred on the screen. Big text gets arcade scanlines cut through it.
static void ctext(const char* s, int y, int size, bool inv = false, int dx = 0) {
  int w = strlen(s) * 6 * size - size, x = (128 - w) / 2 + dx;
  oled.setTextSize(size);
  oled.setTextColor(inv ? BK : WH);
  oled.setCursor(x, y);
  oled.print(s);
  if (size >= 2)
    for (int r = y + size + 1; r < y + 7 * size; r += 2 * size) oled.drawFastHLine(x, r, w, inv ? WH : BK);
  oled.setTextSize(1);
  oled.setTextColor(WH);
}

// Inverted title bar across the top, like a cabinet marquee.
static void bar(const char* l, const char* r) {
  oled.fillRect(0, 0, 128, 10, WH);
  oled.setTextColor(BK);
  oled.setCursor(2, 1);
  oled.print(l);
  oled.setCursor(127 - strlen(r) * 6, 1);
  oled.print(r);
  oled.setTextColor(WH);
}

// Our chameleon, facing right, 22x12 at s = 1. Caught ones are an outline with a cross for an eye.
static void cham(int x, int y, int s, bool alive, int look = 0) {
  if (alive) {
    oled.fillRoundRect(x + 5 * s, y + 2 * s, 11 * s, 6 * s, 3 * s, WH);
    oled.fillTriangle(x + 14 * s, y + s, x + 14 * s, y + 8 * s, x + 21 * s, y + 6 * s, WH);
    oled.fillCircle(x + 16 * s, y + 4 * s, 2 * s, BK);
    oled.fillCircle(x + 16 * s + look * s, y + 4 * s, s - 1, WH);
  } else {
    oled.drawRoundRect(x + 5 * s, y + 2 * s, 11 * s, 6 * s, 3 * s, WH);
    oled.fillTriangle(x + 15 * s, y + 3 * s, x + 15 * s, y + 7 * s, x + 19 * s, y + 6 * s, BK);
    oled.drawTriangle(x + 14 * s, y + s, x + 14 * s, y + 8 * s, x + 21 * s, y + 6 * s, WH);
    oled.drawLine(x + 15 * s, y + 3 * s, x + 17 * s, y + 5 * s, WH);
    oled.drawLine(x + 17 * s, y + 3 * s, x + 15 * s, y + 5 * s, WH);
  }
  oled.drawCircle(x + 3 * s, y + 8 * s, 3 * s, WH);  // curled tail
  oled.drawLine(x + 7 * s, y + 8 * s, x + 6 * s, y + 11 * s, WH);
  oled.drawLine(x + 13 * s, y + 8 * s, x + 14 * s, y + 11 * s, WH);
}

// One chameleon per body: looking around while hiding, blinking "!!" while covered, crossed out once caught.
static void bodyRow(uint32_t now, int y) {
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    int x = i * 32 + 5;
    if (b.alive && b.covered && (now / 250) % 2) {
      oled.setCursor(x + 6, y + 2);
      oled.print("!!");
    } else {
      cham(x, y, 1, b.alive, (int)((now / 700 + i) % 3) - 1);
    }
  }
}

static void drawIdle(uint32_t now) {
  int joined = 0;
  for (auto& b : bodies) joined += b.used;
  int page = now / 4000 % (session.round ? 3 : 2);
  if (page == 0) {  // title: the chameleon snaps a fly with its tongue every 3 s
    ctext("CAMOBOTS", 2, 2);
    oled.drawFastHLine(16, 20, 96, WH);
    uint32_t t = now % 3000;
    int len = t < 2200 ? 0 : t < 2500 ? (t - 2200) * 54 / 300 : t < 2800 ? (2800 - t) * 54 / 300 : 0;
    cham(4, 26, 2, true, t < 2800 ? 1 : 0);
    if (len) {
      oled.fillRect(46, 37, len, 2, WH);
      oled.fillCircle(46 + len, 38, 2, WH);
    }
    if (t < 2200) {  // the fly buzzes about
      int fx = 100 + (int)(t / 70 % 5) - 2, fy = 36 + (int)(t / 110 % 4) - 2;
      oled.fillRect(fx, fy, 2, 2, WH);
      oled.drawPixel(fx + ((t / 40) % 2 ? -1 : 2), fy - 1, WH);
    }
    if ((now / 500) % 2) ctext(joined ? "PULL TRIGGER" : "WAITING FOR BOTS", 56, 1);
  } else if (page == 1) {
    bar("JOIN THE GAME", "");
    oled.setCursor(0, 14);
    oled.print("WIFI  CamoBots");
    oled.setCursor(0, 24);
    oled.print("OPEN  192.168.4.1");
    bodyRow(now, 38);
    char s[20];
    snprintf(s, sizeof s, "%d BOT%s READY", joined, joined == 1 ? "" : "S");
    ctext(s, 56, 1);
  } else {  // session leaderboard
    bar("HIGH SCORES", "");
    uint8_t ord[MAX_PLAYERS], n = session.numPlayers;
    for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++)
      for (int j = i; j > 0 && session.p[ord[j]].points > session.p[ord[j - 1]].points; j--) std::swap(ord[j], ord[j - 1]);
    static const char* RANK[] = {"1ST", "2ND", "3RD", "4TH"};
    for (int i = 0; i < n && i < 4; i++) {
      int y = 14 + i * 12;
      if (i == 0) oled.fillRect(0, y - 2, 128, 11, WH);
      oled.setTextColor(i ? WH : BK);
      oled.setCursor(2, y);
      oled.printf("%s %-10.10s%6ld", RANK[i], names[ord[i]], (long)session.p[ord[i]].points);
      oled.setTextColor(WH);
    }
  }
}

static void drawHide(uint32_t now) {
  int32_t ms = max<int32_t>(0, phaseEnd - now), sec = (ms + 999) / 1000;
  char r[8];
  snprintf(r, sizeof r, "R%u", session.round + 1);
  if (sec <= 3) {  // giant final countdown inside a closing ring
    bar("GET READY", r);
    char d[2] = {(char)('0' + sec), 0};
    int rr = 22 + (ms % 1000) * 6 / 1000;
    oled.drawCircle(64, 37, rr, WH);
    oled.drawCircle(64, 37, rr + 1, WH);
    ctext(d, 20, 5);
    return;
  }
  bar("HIDE!", r);
  oled.drawBitmap(60, 1, PADLOCK, 7, 7, BK);  // trigger locked
  char t[8];
  snprintf(t, sizeof t, "%ld:%02ld", (long)sec / 60, (long)sec % 60);
  ctext(t, 14, 3);
  static const char MSG[] = "PAINT YOUR BOT, HIDE IT, STAY STILL!     ";
  int w = (sizeof MSG - 1) * 6;
  oled.setCursor(128 - (int)(now / 25 % (128 + w)), 41);
  oled.print(MSG);
  bodyRow(now, 52);
}

static void drawHunt(uint32_t now, bool& inv) {
  uint32_t t0 = now - phaseStart;
  if (t0 < 1200) {  // round start: HUNT! flashing
    ctext("HUNT!", 14, 4);
    ctext("FIND THEM!", 52, 1);
    inv = (t0 / 150) % 2 == 0;
    return;
  }
  int32_t left = max<int32_t>(0, phaseEnd - now) / 1000;
  bool panic = left < 30, locked = (int32_t)(now - lockUntil) < 0;
  char r[8];
  snprintf(r, sizeof r, "R%u", session.round + 1);
  bar(panic ? ((now / 250) % 2 ? "PANIC!" : "") : locked ? "LOCKED" : "HUNT", r);
  for (int i = 0; i < cfg.hp; i++)
    oled.drawBitmap(64 - cfg.hp * 9 / 2 + i * 9, 2, i < hp ? HEART : HEART_EMPTY, 7, 6, BK);

  char t[8];
  snprintf(t, sizeof t, "%ld:%02ld", (long)left / 60, (long)left % 60);
  bool flash = panic && (now / 250) % 2;
  if (flash) oled.fillRect(24, 12, 80, 25, WH);
  ctext(t, 13, 3, flash);

  if (missAt && now - missAt < 900) {
    ctext("MISS!", 36, 2, false, ((int)(now / 40 % 3) - 1) * 3);  // shake
  } else if (locked) {
    oled.setCursor(4, 40);
    oled.print("RELOAD");
    oled.drawRect(44, 39, 80, 9, WH);
    oled.fillRect(46, 41, 76 - 76 * (lockUntil - now) / max<uint32_t>(cfg.missLockMs, 1), 5, WH);
  } else if (shots && now - shotAt < 350) {
    ctext("- BANG! -", 40, 1);
  } else if (shots) {
    char s[24];
    int hits = shots - misses - shotPending;
    snprintf(s, sizeof s, "HIT %d/%u  %d%%", hits, shots, hits * 100 / shots);
    ctext(s, 40, 1);
  } else {
    ctext("AIM AND FIRE", 40, 1);
  }
  bodyRow(now, 52);
  inv = shots && now - shotAt < 90;  // muzzle flash
}

static void drawKill(uint32_t now, bool& inv) {
  uint32_t t = now - killAt;
  auto& b = bodies[killBody];
  if (t < 600) {  // splat: rays burst out from the middle
    for (int a = 0; a < 16; a++) {
      float c = cosf(a * PI / 8), s = sinf(a * PI / 8);
      int r0 = t / 8, r1 = 12 + t / 4;
      oled.drawLine(64 + c * r0, 32 + s * r0, 64 + c * r1, 32 + s * r1, WH);
    }
    oled.fillRect(0, 19, 128, 26, BK);
    ctext("GOTCHA!", 21, 3);
    inv = t < 320 && (t / 80) % 2 == 0;
    return;
  }
  ctext("GOTCHA!", 0, 2);
  char s[24];
  snprintf(s, sizeof s, "%.10s FOUND %lu:%02lu", hiderName(killBody), b.foundAt / 60000, b.foundAt / 1000 % 60);
  ctext(s, 18, 1);
  snprintf(s, sizeof s, "+%d", capturePts(b) * (int)min<uint32_t>(t - 600, 600) / 600);  // points roll up
  ctext(s, 28, 2);
  if (is_whistle_hunter(b.foundAt, b.lastWhistle, b.whistled) && (now / 200) % 2) ctext("WHISTLE HUNTER +25", 46, 1);
  int left = 0;
  for (auto& o : bodies) left += o.used && o.alive;
  if (left) snprintf(s, sizeof s, "%d LEFT TO FIND", left);
  else snprintf(s, sizeof s, "CLEAN SWEEP! +%d", PTS_CLEAN_SWEEP);
  ctext(s, 56, 1);
}

static void drawReveal(uint32_t now) {
  if (!haveLast || (now - phaseStart) / 3000 % 2 == 0) {  // winner with falling confetti
    for (int i = 0; i < 28; i++) {
      int x = (i * 37 + (int)(now / 120 + i) % 5) % 128, y = (i * 23 + now / (12 + i % 4 * 6)) % 64;
      oled.fillRect(x, y, 1 + i % 2, 1 + i % 2, WH);
    }
    const char* l1 = lastWon ? "SEEKER" : "HIDERS";
    const char* l2 = lastWon ? "WINS!" : "WIN!";
    oled.fillRect((128 - (int)strlen(l1) * 18) / 2 - 3, 5, strlen(l1) * 18 + 4, 27, BK);
    oled.fillRect((128 - (int)strlen(l2) * 18) / 2 - 3, 33, strlen(l2) * 18 + 4, 27, BK);
    ctext(l1, 7, 3);
    ctext(l2, 35, 3);
    return;
  }
  char h[16];
  snprintf(h, sizeof h, "ROUND %u", session.round);
  bar(h, "SCORE");
  uint8_t sk[MAX_PLAYERS], hd[MAX_PLAYERS];
  rotation(roundFirst, session.numPlayers, session.numSeekers, sk, hd);
  int y = 13;  // > seeker, * survived, x caught
  for (int k = 0; k < session.numSeekers && y < 60; k++, y += 10) {
    oled.setCursor(2, y);
    oled.printf("> %-12.12s%6d", names[sk[k]], lastScore.seeker[k]);
  }
  for (int i = 0; i < session.numPlayers - session.numSeekers && y < 60; i++, y += 10) {
    oled.setCursor(2, y);
    oled.printf("%c %-12.12s%6d", bodies[i].alive ? '*' : 'x', names[hd[i]], lastScore.body[i]);
  }
}

static void oledUpdate(uint32_t now) {
  static uint32_t last;
  if (!haveOled || now - last < 80) return;
  last = now;
  oled.clearDisplay();
  bool inv = false;
  if (killBody >= 0 && now - killAt < KILL_MS) drawKill(now, inv);
  else if (phase == PH_IDLE) drawIdle(now);
  else if (phase == PH_HIDE) drawHide(now);
  else if (phase == PH_HUNT) drawHunt(now, inv);
  else drawReveal(now);

  static bool wasInv;
  if (inv != wasInv) oled.invertDisplay(wasInv = inv);  // one command, no frame push
  static uint8_t prev[128 * 64 / 8];
  if (memcmp(prev, oled.getBuffer(), sizeof prev)) {
    memcpy(prev, oled.getBuffer(), sizeof prev);
    oled.display();
  }
}

void blaster_setup() {
  Serial.println("[blaster] boot");
  load();
  hw_begin();
  radio_begin(true);
  oledBegin();
  web.on("/", [] {
    web.send_P(200, "text/html", index_html, index_html_end - index_html - 1);  // txt embed adds a trailing 0
  });
  web.on("/state", handleState);
  web.on("/config", handleConfig);
  web.on("/start", [] { action("start"); });
  web.on("/stop", [] { action("stop"); });
  web.on("/reset", [] { action("reset"); });
  web.on("/whistle", [] {
    manualWhistle(web.arg("b").toInt());
    web.send(204);
  });
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
    case PH_IDLE:
      if (fired)  // trigger starts a round once a body has joined
        for (auto& b : bodies)
          if (b.used) {
            startRound(now);
            break;
          }
      break;
    case PH_HIDE:
      if (fired) Serial.println("[game] trigger locked during hide");
      if ((int32_t)(now - phaseEnd) >= 0) {
        setPhase(PH_HUNT, now, cfg.huntSec * 1000u);
        StartMsg s{cfg.huntSec * 1000u};
        for (int i = 0; i < MAX_BODIES; i++)
          if (bodies[i].used) radio_send(bodies[i].mac, MSG_START, &s, sizeof s);
        event(EV_HUNT);
        hw_sound(SND_BELL);
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
          event(EV_SHOT, -1, 0);
        }
      }
      if (shotPending && now - shotAt >= SHOT_WINDOW_MS) {
        shotPending = false;
        misses++;
        missAt = now;
        lockUntil = now + cfg.missLockMs;
        hw_sound(SND_BUZZ);
        event(EV_MISS, -1, 0);
        Serial.printf("[game] MISS (%u of %u shots), locked %u ms\n", misses, shots, cfg.missLockMs);
        if (cfg.hp && --hp == 0) {
          endRound(now, false, "seeker out of HP");
          break;
        }
      }
      static uint32_t lastScan;
      if (!shotPending && now - lastScan >= SCAN_MS) {  // never scan inside a shot window
        lastScan = now;
        hw_scan();
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
  oledUpdate(now);

  static uint32_t lastLog;
  if (now - lastLog >= 5000) {
    lastLog = now;
    for (int i = 0; i < MAX_BODIES; i++)
      if (bodies[i].used)
        Serial.printf("[link] body %d %s rx=%lu lost=%lu seen=%lums ago\n", i, STATE_NAME[bodies[i].state % 5],
                      bodies[i].rx, bodies[i].lost, now - bodies[i].lastSeen);
  }
}
