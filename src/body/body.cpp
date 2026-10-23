#include <Arduino.h>
#include "pins.h"

void body_setup() {
  pinMode(PIN_USER_LED, OUTPUT);
  Serial.println("[body] boot");
}

void body_loop() {
  static uint32_t last;
  static bool on;
  if (millis() - last >= 500) {
    last = millis();
    on = !on;
    digitalWrite(PIN_USER_LED, on ? LOW : HIGH);
    Serial.printf("[body] alive %lu\n", millis() / 1000);
  }
}
