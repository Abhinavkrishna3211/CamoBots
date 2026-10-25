// Part tests with the real drivers. Results go in docs/bench-results.md.
//   bench_body:    prints every IR frame, TSOP edges and the LDR once a second.
//   bench_blaster: trigger fires a shot; serial commands below.
// Both: "snd N" plays sound N (0 beep .. 11 last), "pix N" sets pixel state N (0 off .. 9 go).
#include <Arduino.h>
#include "hw.h"
#include "config.h"

static uint32_t count;
static bool scanOn;

void setup() {
  Serial.begin(115200);
  for (uint32_t t = millis(); !Serial && millis() - t < 1500;) delay(10);
  hw_begin();
#if defined(ROLE_BODY)
  Serial.println("[bench body] snd N, pix N, zero (reset shot count)");
#else
  Serial.println("[bench blaster] trigger = shot, burst N (N shots, 0.5 s apart), scan (toggle 5/s scans), snd N");
#endif
}

void loop() {
  uint32_t now = millis();
  hw_update();

  String c;
  if (hw_cmd(c)) {
    if (c.startsWith("snd ")) hw_sound((Snd)constrain(c.substring(4).toInt(), 0, SND_LAST));
    else if (c.startsWith("pix ")) hw_pixel((Pix)constrain(c.substring(4).toInt(), 0, PIX_GO));
    else if (c == "zero") count = 0;
#if defined(ROLE_BLASTER)
    else if (c.startsWith("burst ")) {
      for (int i = c.substring(6).toInt(); i > 0; i--) {
        hw_fire();
        Serial.printf("[bench] shot %lu\n", ++count);
        for (uint32_t t = millis(); millis() - t < 500;) hw_update();
      }
    } else if (c == "scan") {
      scanOn = !scanOn;
      Serial.printf("[bench] scans %s\n", scanOn ? "on" : "off");
    }
#endif
    else Serial.printf("[bench] unknown '%s'\n", c.c_str());
  }

#if defined(ROLE_BODY)
  int hit = hw_ir_hit();
  if (hit >= 0) {
    Serial.printf("[bench] SHOT from seeker %d, %lu shots so far\n", hit, ++count);
    hw_sound(SND_BEEP);  // short and non-blocking, so back-to-back shots all count
  }
  static uint32_t beam, lastLog, lastEdges;
  beam += hw_beam_ms();
  if (now - lastLog >= 1000) {
    lastLog = now;
    uint32_t edges, bad;
    hw_ir_stats(&edges, &bad);
    Serial.printf("[bench] ldr=%u edges/s=%lu bad=%lu beam=%lums shots=%lu\n", hw_ldr(), edges - lastEdges, bad,
                  beam, count);
    lastEdges = edges;
  }
#else
  if (hw_trigger()) {
    hw_fire();
    hw_sound(SND_FIRE);
    Serial.printf("[bench] shot %lu\n", ++count);
  }
  static uint32_t lastScan;
  if (scanOn && now - lastScan >= SCAN_MS) {
    lastScan = now;
    hw_scan();
  }
#endif
}
