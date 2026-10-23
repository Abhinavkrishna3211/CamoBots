#include <Arduino.h>
#include "pins.h"
#include "radio.h"

#ifndef PING_MS
#define PING_MS 100  // 10 Hz for the link test; game heartbeat will be slower
#endif

static uint8_t hostMac[6];
static int myId = -1;

void body_setup() {
  pinMode(PIN_USER_LED, OUTPUT);
  Serial.println("[body] boot");
  radio_begin(false);
}

void body_loop() {
  uint32_t now = millis();
  RadioMsg m;
  while (radio_poll(&m)) {
    // Accept WELCOME any time: the host may have rebooted and renumbered us.
    if (m.h.type == MSG_WELCOME) {
      auto* w = (WelcomeMsg*)m.payload;
      memcpy(hostMac, m.mac, 6);
      myId = w->id;
      Serial.printf("[body] joined host as body %d\n", myId);
    }
  }

  static uint32_t lastTx;
  if (myId < 0) {
    if (now - lastTx >= 1000) {
      lastTx = now;
      HelloMsg h{UNIT_BODY, 0xFF};
      radio_send(BCAST, MSG_HELLO, &h, sizeof h);
    }
  } else if (now - lastTx >= PING_MS) {
    lastTx = now;
    PingMsg p{(uint8_t)myId, now, radio_ack_fails()};
    radio_send(hostMac, MSG_PING, &p, sizeof p);
  }

  // LED: fast blink while searching for host, slow once joined.
  static uint32_t lastLed;
  static bool on;
  if (now - lastLed >= (myId < 0 ? 100u : 500u)) {
    lastLed = now;
    on = !on;
    digitalWrite(PIN_USER_LED, on ? LOW : HIGH);
  }

  static uint32_t lastLog;
  if (now - lastLog >= 5000) {
    lastLog = now;
    Serial.printf("[body %d] ack ok=%lu fail=%lu\n", myId, radio_ack_ok(), radio_ack_fails());
  }
}
