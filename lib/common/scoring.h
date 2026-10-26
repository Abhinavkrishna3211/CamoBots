#pragma once
// Round scoring, session totals, rotation and badges. Pure C++ for native tests.
#include <stdint.h>
#include "protocol.h"

#define MAX_PLAYERS (MAX_SEEKERS + MAX_BODIES)

// Points (tune after the playtest)
#define PTS_CAPTURE         100   // plus 1 per second left on the clock
#define PTS_WHISTLE_HUNTER  25
#define WHISTLE_HUNTER_MS   10000
#define PTS_CLEAN_SWEEP     100
#define PTS_GHOST           100
#define PTS_LAST_CHAMELEON  50
#define PTS_COVER_EVENT     (-20)
#define PTS_MISSED_SPOT_PER_SEC 5
#define PTS_TAUNT           10    // per taunt from the hider's phone

struct BodyResult {
  bool used;
  bool alive;            // at the end of the round
  bool struckOut;
  uint8_t foundBy;       // seeker slot that captured it
  uint32_t outAtMs;      // hunt time when captured or struck out
  uint32_t coveredMs;    // total time covered (survival points freeze)
  uint8_t coverEvents;
  bool whistleHunter;    // captured soon after its own whistle
  uint32_t missedSpotMs; // time in the seeker's beam without being shot
  uint8_t taunts;        // TAUNT presses that played
};

struct RoundResult {
  uint32_t huntMs;       // configured hunt length
  uint32_t endMs;        // hunt time when the round ended
  uint8_t numSeekers;
  uint16_t shots[MAX_SEEKERS];
  BodyResult body[MAX_BODIES];
};

struct RoundScore {
  int16_t body[MAX_BODIES];
  int16_t seeker[MAX_SEEKERS];
  int16_t survival[MAX_BODIES];
  int16_t missedSpot[MAX_BODIES];
  bool ghost[MAX_BODIES];
  bool lastChameleon[MAX_BODIES];
  uint8_t hits[MAX_SEEKERS];
  uint8_t whistleHunters[MAX_SEEKERS];
  bool cleanSweep;
};

bool is_whistle_hunter(uint32_t foundAtMs, uint32_t lastWhistleMs, bool whistled);
RoundScore score_round(const RoundResult& r);

struct PlayerTotals {
  int32_t points, survival, missedSpot;
  uint16_t shots, hits, whistleHunters, coverEvents, ghosts;
};

struct Session {
  uint8_t numPlayers = 3, numSeekers = 1;
  uint8_t round = 0;     // rounds played so far
  uint8_t next = 0;      // player who seeks next round: a random hider, a volunteer or the Setup page
  // seat[0..S-1] hold blasters, seat[S+i] owns body i
  uint8_t seat[MAX_PLAYERS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  PlayerTotals p[MAX_PLAYERS] = {};
};

static_assert(MAX_PLAYERS == 12, "Session::seat initialiser");

// Adds a round played with s.seat.
void session_add(Session& s, const RoundResult& r, const RoundScore& sc);
// Round start: the next seeker swaps seats with seeker 0, so only those two trade.
// Returns the seat that changed hands, or -1.
int seat_round(Session& s);
void seat_reset(Session& s);
uint8_t random_hider(const Session& s, uint32_t rnd);

enum Badge : uint8_t { BADGE_DISGUISE, BADGE_SHARPSHOOTER, BADGE_EAGLE_EARS, BADGE_COVER_CHEATER, BADGE_DAREDEVIL,
                       BADGE_COUNT };
int badge_winner(const Session& s, Badge b);  // player index, or -1 if nobody earned it
