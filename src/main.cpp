#include <Arduino.h>

#if defined(ROLE_BODY)
void body_setup();
void body_loop();
#define role_setup body_setup
#define role_loop body_loop
#elif defined(ROLE_BLASTER)
void blaster_setup();
void blaster_loop();
#define role_setup blaster_setup
#define role_loop blaster_loop
#else
#error "Build with -DROLE_BODY=1 or -DROLE_BLASTER=1"
#endif

void setup() {
  Serial.begin(115200);
  // USB CDC: give the host a moment to open the port so boot logs are not lost.
  for (uint32_t t = millis(); !Serial && millis() - t < 1500;) delay(10);
  role_setup();
}

void loop() { role_loop(); }
