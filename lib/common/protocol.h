#pragma once
// ESP-NOW wire format. Pure C++ so native tests can include it.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PROTO_MAGIC   0xC4
#define PROTO_VERSION 1
#define WIFI_CHANNEL  1
#define MAX_BODIES    4   // 2-bit body id
#define MAX_SEEKERS   4   // 2-bit shooter id

enum MsgType : uint8_t {
  MSG_HELLO = 1,    // body/blaster -> broadcast: "I exist", carries role + saved id
  MSG_WELCOME,      // host -> unit: assigned id
  MSG_PING,         // unit -> host heartbeat (also used for loss test)
  MSG_CONFIG,       // host -> unit: game settings
  MSG_HIDE_START,   // host -> bodies
  MSG_START,        // host -> bodies: hunt begins
  MSG_FOUND,        // body -> host: IR hit decoded
  MSG_WHISTLE,      // both ways: body reports a whistle; host asks for one
  MSG_COVERED,      // body -> host: covered / uncovered
  MSG_SHOT,         // extra blaster -> host: trigger pulled
  MSG_END,          // host -> bodies: round over, alive flags
};

enum UnitRole : uint8_t { UNIT_BODY = 0, UNIT_BLASTER = 1 };

#pragma pack(push, 1)
struct MsgHeader {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t seq;
};

struct Packet {
  MsgHeader h;
  uint8_t payload[200];
};
#pragma pack(pop)

inline uint16_t crc16(const uint8_t* d, size_t n) {  // CRC-16/CCITT-FALSE
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

// Frame = header + payload + crc16. Returns total length.
inline size_t proto_pack(uint8_t* out, uint8_t type, uint16_t seq, const void* payload, size_t plen) {
  MsgHeader h{PROTO_MAGIC, PROTO_VERSION, type, seq};
  memcpy(out, &h, sizeof h);
  if (plen) memcpy(out + sizeof h, payload, plen);
  size_t n = sizeof h + plen;
  uint16_t c = crc16(out, n);
  out[n] = c & 0xFF;
  out[n + 1] = c >> 8;
  return n + 2;
}

// Returns payload length, or -1 if the frame is bad or from another version.
inline int proto_unpack(const uint8_t* in, size_t n, MsgHeader* h, const uint8_t** payload) {
  if (n < sizeof(MsgHeader) + 2) return -1;
  uint16_t c = in[n - 2] | (in[n - 1] << 8);
  if (crc16(in, n - 2) != c) return -1;
  memcpy(h, in, sizeof *h);
  if (h->magic != PROTO_MAGIC || h->version != PROTO_VERSION) return -1;
  *payload = in + sizeof *h;
  return (int)(n - 2 - sizeof *h);
}

#pragma pack(push, 1)
struct HelloMsg   { uint8_t role; uint8_t savedId; };   // savedId 0xFF = none
struct WelcomeMsg { uint8_t role; uint8_t id; };
struct PingMsg    { uint8_t id; uint32_t sent; uint32_t ackFails; };
#pragma pack(pop)
