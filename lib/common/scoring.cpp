#include "scoring.h"

bool is_whistle_hunter(uint32_t foundAtMs, uint32_t lastWhistleMs, bool whistled) {
  return whistled && foundAtMs >= lastWhistleMs && foundAtMs - lastWhistleMs <= WHISTLE_HUNTER_MS;
}

RoundScore score_round(const RoundResult& r) {
  RoundScore s = {};
  int nUsed = 0, nAlive = 0, lastAlive = -1;
  for (int i = 0; i < MAX_BODIES; i++) {
    const BodyResult& b = r.body[i];
    if (!b.used) continue;
    nUsed++;
    if (b.alive) nAlive++, lastAlive = i;

    uint32_t until = b.alive ? r.endMs : b.outAtMs;
    uint32_t covered = b.coveredMs < until ? b.coveredMs : until;
    s.survival[i] = (until - covered) / 1000;
    s.missedSpot[i] = b.missedSpotMs / 1000 * PTS_MISSED_SPOT_PER_SEC;
    s.ghost[i] = b.alive;
    s.body[i] = s.survival[i] + s.missedSpot[i] + b.coverEvents * PTS_COVER_EVENT + (b.alive ? PTS_GHOST : 0) +
                b.taunts * PTS_TAUNT;

    // Strike-outs count for the win but earn the seeker nothing.
    if (!b.alive && !b.struckOut && b.foundBy < r.numSeekers) {
      uint32_t left = r.huntMs > b.outAtMs ? (r.huntMs - b.outAtMs) / 1000 : 0;
      s.seeker[b.foundBy] += PTS_CAPTURE + left;
      s.hits[b.foundBy]++;
      if (b.whistleHunter) {
        s.seeker[b.foundBy] += PTS_WHISTLE_HUNTER;
        s.whistleHunters[b.foundBy]++;
      }
    }
  }
  if (nAlive == 1 && nUsed > 1) {
    s.lastChameleon[lastAlive] = true;
    s.body[lastAlive] += PTS_LAST_CHAMELEON;
  }
  s.cleanSweep = nUsed > 0 && nAlive == 0;
  if (s.cleanSweep)
    for (int k = 0; k < r.numSeekers; k++) s.seeker[k] += PTS_CLEAN_SWEEP;
  return s;
}

int seat_round(Session& s) {
  for (int k = s.numSeekers; k < s.numPlayers; k++)
    if (s.seat[k] == s.next) {
      s.seat[k] = s.seat[0];
      s.seat[0] = s.next;
      return k;
    }
  return -1;  // already seeking
}

void seat_reset(Session& s) {
  for (int k = 0; k < MAX_PLAYERS; k++) s.seat[k] = k;
  s.next %= s.numPlayers;
}

uint8_t random_hider(const Session& s, uint32_t rnd) {
  int n = s.numPlayers - s.numSeekers;
  return n > 0 ? s.seat[s.numSeekers + rnd % n] : s.seat[0];
}

void session_add(Session& s, const RoundResult& r, const RoundScore& sc) {
  const uint8_t* seekers = s.seat;
  const uint8_t* hiders = s.seat + s.numSeekers;
  for (int k = 0; k < s.numSeekers; k++) {
    PlayerTotals& t = s.p[seekers[k]];
    t.points += sc.seeker[k];
    t.shots += r.shots[k];
    t.hits += sc.hits[k];
    t.whistleHunters += sc.whistleHunters[k];
  }
  for (int i = 0; i < s.numPlayers - s.numSeekers && i < MAX_BODIES; i++) {
    PlayerTotals& t = s.p[hiders[i]];
    t.points += sc.body[i];
    t.survival += sc.survival[i];
    t.missedSpot += sc.missedSpot[i];
    t.coverEvents += r.body[i].coverEvents;
    t.ghosts += sc.ghost[i];
  }
  s.round++;
}

static int32_t badgeValue(const PlayerTotals& t, Badge b) {
  switch (b) {
    case BADGE_DISGUISE: return t.survival;
    case BADGE_EAGLE_EARS: return t.whistleHunters;
    case BADGE_COVER_CHEATER: return t.coverEvents;
    case BADGE_DAREDEVIL: return t.missedSpot;
    default: return 0;
  }
}

// Ties go to the lower player index.
int badge_winner(const Session& s, Badge b) {
  int best = -1;
  for (int i = 0; i < s.numPlayers; i++) {
    const PlayerTotals& t = s.p[i];
    if (b == BADGE_SHARPSHOOTER) {
      if (!t.hits) continue;
      if (best < 0) { best = i; continue; }
      // higher hits/shots wins; cross-multiply to stay in integers, more hits breaks a tie
      const PlayerTotals& o = s.p[best];
      uint32_t a = (uint32_t)t.hits * o.shots, c = (uint32_t)o.hits * t.shots;
      if (a > c || (a == c && t.hits > o.hits)) best = i;
    } else if (badgeValue(t, b) > (best < 0 ? 0 : badgeValue(s.p[best], b))) {
      best = i;
    }
  }
  return best;
}
