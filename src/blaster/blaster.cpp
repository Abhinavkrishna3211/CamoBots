#include <Arduino.h>
#include "pins.h"

void blaster_setup() {
  pinMode(PIN_USER_LED, OUTPUT);
  Serial.println("[blaster] boot");
}

void blaster_loop() {
  static uint32_t last;
  static bool on;
  if (millis() - last >= 150) {  // faster blink than body so roles are easy to tell apart
    last = millis();
    on = !on;
    digitalWrite(PIN_USER_LED, on ? LOW : HIGH);
  }
  static uint32_t lastLog;
  if (millis() - lastLog >= 1000) {
    lastLog = millis();
    Serial.printf("[blaster] alive %lu\n", millis() / 1000);
  }
}
