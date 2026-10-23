#pragma once
#include <Arduino.h>
#include "protocol.h"

struct RadioMsg {
  uint8_t mac[6];
  MsgHeader h;
  uint8_t payload[200];
  int len;
};

extern const uint8_t BCAST[6];

// ap=true: host runs SoftAP "CamoBots" on WIFI_CHANNEL; ESP-NOW rides the AP interface.
void radio_begin(bool ap);
bool radio_send(const uint8_t* mac, uint8_t type, const void* payload, size_t len);
bool radio_poll(RadioMsg* out);  // non-blocking; false when queue empty
uint32_t radio_ack_fails();      // unicast sends with no MAC-level ACK
uint32_t radio_ack_ok();
