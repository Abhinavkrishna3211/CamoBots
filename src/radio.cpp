#include "radio.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct RxItem {
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[250];
};

static QueueHandle_t rxq;
static volatile uint32_t ackOk, ackFail, rxRaw;
static uint16_t seq;
static bool apMode;

// Runs in the Wi-Fi task: copy out and return fast.
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  rxRaw++;
  if (len <= 0 || len > 250) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.len = len;
  memcpy(it.data, data, len);
  xQueueSend(rxq, &it, 0);
}

static void onSent(const uint8_t* mac, esp_now_send_status_t s) {
  if (memcmp(mac, BCAST, 6) == 0) return;  // broadcasts are never ACKed
  if (s == ESP_NOW_SEND_SUCCESS) ackOk++; else ackFail++;
}

static bool addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = WIFI_CHANNEL;
  p.ifidx = apMode ? WIFI_IF_AP : WIFI_IF_STA;
  return esp_now_add_peer(&p) == ESP_OK;
}

void radio_begin(bool ap) {
  apMode = ap;
  WiFi.persistent(false);
  if (ap) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("CamoBots", nullptr, WIFI_CHANNEL);
  } else {
    WiFi.mode(WIFI_STA);
    // A board with a router saved from an old sketch keeps scanning and hops off our channel.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  }
  esp_wifi_set_ps(WIFI_PS_NONE);  // modem sleep drops ESP-NOW frames
  rxq = xQueueCreate(16, sizeof(RxItem));
  if (esp_now_init() != ESP_OK) Serial.println("[radio] esp_now_init failed");
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  addPeer(BCAST);
  Serial.printf("[radio] %s mac=%s ch=%d\n", ap ? "AP" : "STA",
                (ap ? WiFi.softAPmacAddress() : WiFi.macAddress()).c_str(), WIFI_CHANNEL);
}

bool radio_send(const uint8_t* mac, uint8_t type, const void* payload, size_t len) {
  uint8_t buf[250];
  if (len > sizeof(Packet::payload) || !addPeer(mac)) return false;
  size_t n = proto_pack(buf, type, seq++, payload, len);
  esp_err_t e = esp_now_send(mac, buf, n);
  if (e != ESP_OK) Serial.printf("[radio] send type %u failed: %s\n", type, esp_err_to_name(e));
  return e == ESP_OK;
}

bool radio_poll(RadioMsg* out) {
  RxItem it;
  while (xQueueReceive(rxq, &it, 0) == pdTRUE) {
    const uint8_t* pl;
    int n = proto_unpack(it.data, it.len, &out->h, &pl);
    if (n < 0) continue;  // bad CRC or other version: drop silently
    memcpy(out->mac, it.mac, 6);
    memcpy(out->payload, pl, n);
    out->len = n;
    return true;
  }
  return false;
}

uint32_t radio_ack_fails() { return ackFail; }
uint32_t radio_ack_ok() { return ackOk; }
uint32_t radio_rx_raw() { return rxRaw; }

uint8_t radio_channel() {
  uint8_t ch;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&ch, &sec);
  return ch;
}
