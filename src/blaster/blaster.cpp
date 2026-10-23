#include <Arduino.h>
#include <WebServer.h>
#include "pins.h"
#include "radio.h"

struct BodyLink {
  uint8_t mac[6];
  bool used;
  uint32_t rx, lost, lastSeen, peerAckFails;
  uint16_t lastSeq;
  bool haveSeq;
};

static BodyLink bodies[MAX_BODIES];
static WebServer web(80);
static uint32_t pageHits, slowestPageMs;

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

static void handleRoot() {
  uint32_t t0 = millis();
  String s = F("<!doctype html><meta name=viewport content='width=device-width'>"
               "<meta http-equiv=refresh content=1><body style='background:#113;color:#7f7;font:16px monospace'>"
               "<h2>CamoBots link test</h2><table border=1 cellpadding=4><tr><th>body<th>rx<th>lost<th>loss %<th>ack fails<th>last seen ms</tr>");
  for (int i = 0; i < MAX_BODIES; i++) {
    auto& b = bodies[i];
    if (!b.used) continue;
    float loss = b.rx + b.lost ? 100.0f * b.lost / (b.rx + b.lost) : 0;
    s += "<tr><td>" + String(i) + "<td>" + b.rx + "<td>" + b.lost + "<td>" + String(loss, 2) +
         "<td>" + b.peerAckFails + "<td>" + (millis() - b.lastSeen) + "</tr>";
  }
  s += "</table><p>uptime " + String(millis() / 1000) + " s, page hits " + pageHits +
       ", slowest page " + slowestPageMs + " ms</p>";
  web.send(200, "text/html", s);
  pageHits++;
  slowestPageMs = std::max<uint32_t>(slowestPageMs, millis() - t0);
}

void blaster_setup() {
  pinMode(PIN_USER_LED, OUTPUT);
  Serial.println("[blaster] boot");
  radio_begin(true);
  web.on("/", handleRoot);
  web.begin();
  Serial.println("[blaster] web on http://192.168.4.1");
}

void blaster_loop() {
  uint32_t now = millis();
  web.handleClient();

  RadioMsg m;
  while (radio_poll(&m)) {
    if (m.h.type == MSG_HELLO && ((HelloMsg*)m.payload)->role == UNIT_BODY) {
      int id = bodyIdFor(m.mac);
      if (id < 0) continue;
      WelcomeMsg w{UNIT_BODY, (uint8_t)id};
      radio_send(m.mac, MSG_WELCOME, &w, sizeof w);
      bodies[id].haveSeq = false;  // body rebooted: restart gap tracking
      Serial.printf("[blaster] body %d joined %02X:%02X:%02X\n", id, m.mac[3], m.mac[4], m.mac[5]);
    } else if (m.h.type == MSG_PING) {
      // Trust the MAC, not the id in the payload: after a host reboot the table is empty.
      int id = bodyIdFor(m.mac);
      if (id < 0) continue;
      if (id != ((PingMsg*)m.payload)->id) {
        WelcomeMsg w{UNIT_BODY, (uint8_t)id};
        radio_send(m.mac, MSG_WELCOME, &w, sizeof w);
        bodies[id].haveSeq = false;
        Serial.printf("[blaster] body %d re-joined\n", id);
      }
      auto& b = bodies[id];
      if (b.haveSeq) b.lost += (uint16_t)(m.h.seq - b.lastSeq - 1);
      b.lastSeq = m.h.seq;
      b.haveSeq = true;
      b.rx++;
      b.lastSeen = now;
      b.peerAckFails = ((PingMsg*)m.payload)->ackFails;
    }
  }

  static uint32_t lastLed;
  static bool on;
  if (now - lastLed >= 150) {
    lastLed = now;
    on = !on;
    digitalWrite(PIN_USER_LED, on ? LOW : HIGH);
  }

  static uint32_t lastLog;
  if (now - lastLog >= 5000) {
    lastLog = now;
    for (int i = 0; i < MAX_BODIES; i++)
      if (bodies[i].used)
        Serial.printf("[blaster] body %d rx=%lu lost=%lu ackfail=%lu seen=%lums ago\n", i, bodies[i].rx,
                      bodies[i].lost, bodies[i].peerAckFails, now - bodies[i].lastSeen);
    Serial.printf("[blaster] stations=%d page hits=%lu slowest=%lums\n", WiFi.softAPgetStationNum(),
                  pageHits, slowestPageMs);
  }
}
