#include <unity.h>
#include "scoring.h"

void setUp() {}
void tearDown() {}

// 1 seeker, 2 bodies, 180 s hunt
static RoundResult base() {
  RoundResult r = {};
  r.huntMs = 180000;
  r.endMs = 180000;
  r.numSeekers = 1;
  for (int i = 0; i < 2; i++) {
    r.body[i].used = true;
    r.body[i].alive = true;
  }
  return r;
}

static void capture(RoundResult& r, int i, uint32_t at, uint8_t by = 0) {
  r.body[i].alive = false;
  r.body[i].outAtMs = at;
  r.body[i].foundBy = by;
}

void test_none_captured() {
  RoundResult r = base();
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(180 + PTS_GHOST, s.body[0]);
  TEST_ASSERT_EQUAL(180 + PTS_GHOST, s.body[1]);
  TEST_ASSERT_FALSE(s.lastChameleon[0]);
  TEST_ASSERT_EQUAL(0, s.seeker[0]);
  TEST_ASSERT_FALSE(s.cleanSweep);
}

void test_all_captured() {
  RoundResult r = base();
  capture(r, 0, 60000);
  capture(r, 1, 120000);
  r.endMs = 120000;
  RoundScore s = score_round(r);
  // 100+120 left, 100+60 left, clean sweep
  TEST_ASSERT_EQUAL(220 + 160 + PTS_CLEAN_SWEEP, s.seeker[0]);
  TEST_ASSERT_EQUAL(60, s.body[0]);
  TEST_ASSERT_EQUAL(120, s.body[1]);
  TEST_ASSERT_TRUE(s.cleanSweep);
  TEST_ASSERT_EQUAL(2, s.hits[0]);
}

void test_strike_out_counts_for_win_not_points() {
  RoundResult r = base();
  capture(r, 0, 30000);
  r.body[1].alive = false;
  r.body[1].struckOut = true;
  r.body[1].outAtMs = 50000;
  r.body[1].coverEvents = 3;
  r.endMs = 50000;
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(100 + 150 + PTS_CLEAN_SWEEP, s.seeker[0]);
  TEST_ASSERT_EQUAL(1, s.hits[0]);
  TEST_ASSERT_EQUAL(50 + 3 * PTS_COVER_EVENT, s.body[1]);
  TEST_ASSERT_TRUE(s.cleanSweep);
}

void test_sole_survivor_gets_last_chameleon_and_ghost() {
  RoundResult r = base();
  capture(r, 0, 90000);
  RoundScore s = score_round(r);
  TEST_ASSERT_TRUE(s.lastChameleon[1]);
  TEST_ASSERT_TRUE(s.ghost[1]);
  TEST_ASSERT_EQUAL(180 + PTS_GHOST + PTS_LAST_CHAMELEON, s.body[1]);
  TEST_ASSERT_FALSE(s.cleanSweep);
}

void test_single_body_is_not_last_chameleon() {
  RoundResult r = base();
  r.body[1].used = false;
  TEST_ASSERT_FALSE(score_round(r).lastChameleon[0]);
}

void test_cover_freezes_survival() {
  RoundResult r = base();
  r.body[0].coveredMs = 40000;
  r.body[0].coverEvents = 2;
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(140, s.survival[0]);
  TEST_ASSERT_EQUAL(140 + 2 * PTS_COVER_EVENT + PTS_GHOST, s.body[0]);
}

void test_whistle_hunter_window_edge() {
  TEST_ASSERT_TRUE(is_whistle_hunter(20000, 10000, true));   // exactly 10 s
  TEST_ASSERT_FALSE(is_whistle_hunter(20001, 10000, true));  // just over
  TEST_ASSERT_TRUE(is_whistle_hunter(10000, 10000, true));
  TEST_ASSERT_FALSE(is_whistle_hunter(5000, 0, false));      // never whistled
  RoundResult r = base();
  capture(r, 0, 60000);
  r.body[0].whistleHunter = true;
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(100 + 120 + PTS_WHISTLE_HUNTER, s.seeker[0]);
  TEST_ASSERT_EQUAL(1, s.whistleHunters[0]);
}

void test_missed_spot() {
  RoundResult r = base();
  r.body[0].missedSpotMs = 7900;  // whole seconds only
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(7 * PTS_MISSED_SPOT_PER_SEC, s.missedSpot[0]);
  TEST_ASSERT_EQUAL(180 + 35 + PTS_GHOST, s.body[0]);
}

void test_taunts() {
  RoundResult r = base();
  r.body[1].taunts = 3;
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(180 + 3 * PTS_TAUNT + PTS_GHOST, s.body[1]);
}

void test_seeker_out_of_hp_ends_early() {
  RoundResult r = base();
  capture(r, 0, 20000);
  r.endMs = 45000;  // HP ran out at 45 s
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(45 + PTS_GHOST + PTS_LAST_CHAMELEON, s.body[1]);
  TEST_ASSERT_EQUAL(100 + 160, s.seeker[0]);
}

void test_rotation_three_players() {
  uint8_t sk[4], hd[4];
  rotation(0, 3, 1, sk, hd);
  TEST_ASSERT_EQUAL(0, sk[0]); TEST_ASSERT_EQUAL(1, hd[0]); TEST_ASSERT_EQUAL(2, hd[1]);
  rotation(1, 3, 1, sk, hd);
  TEST_ASSERT_EQUAL(1, sk[0]); TEST_ASSERT_EQUAL(2, hd[0]); TEST_ASSERT_EQUAL(0, hd[1]);
  rotation(3, 3, 1, sk, hd);  // wraps
  TEST_ASSERT_EQUAL(0, sk[0]);
}

void test_three_round_totals_and_badges() {
  Session ss;
  // Round 0: player 0 seeks, finds body 0 (player 1) at 60 s after a whistle; player 2 survives.
  RoundResult r = base();
  r.shots[0] = 4;
  capture(r, 0, 60000);
  r.body[0].whistleHunter = true;
  r.body[1].missedSpotMs = 10000;
  session_add(ss, r, score_round(r));
  // Round 1: player 1 seeks, misses everything; player 2 (body 0) covers twice, player 0 (body 1) survives.
  r = base();
  r.shots[0] = 3;
  r.body[0].coverEvents = 2;
  r.body[0].coveredMs = 30000;
  session_add(ss, r, score_round(r));
  // Round 2: player 2 seeks, 1 shot 1 hit on body 1 (player 1) at 170 s.
  r = base();
  r.shots[0] = 1;
  capture(r, 1, 170000);
  session_add(ss, r, score_round(r));

  TEST_ASSERT_EQUAL(3, ss.round);
  TEST_ASSERT_EQUAL(0, ss.nextFirst);
  // p0: seek 100+120+25 = 245; hide body1 r1 180+100 = 280; hide body0 r2 180+100+50 = 330
  TEST_ASSERT_EQUAL(245 + 280 + 330, ss.p[0].points);
  // p1: hide body0 r0 = 60; seek r1 = 0; hide body1 r2 = 170
  TEST_ASSERT_EQUAL(60 + 170, ss.p[1].points);
  // p2: hide body1 r0 180+50+100+50 = 380; hide body0 r1 150-40+100 = 210; seek r2 100+10 = 110
  TEST_ASSERT_EQUAL(380 + 210 + 110, ss.p[2].points);

  TEST_ASSERT_EQUAL(0, badge_winner(ss, BADGE_DISGUISE));       // p0 360 vs p2 330
  TEST_ASSERT_EQUAL(2, badge_winner(ss, BADGE_SHARPSHOOTER));   // 1/1 beats 1/4
  TEST_ASSERT_EQUAL(0, badge_winner(ss, BADGE_EAGLE_EARS));
  TEST_ASSERT_EQUAL(2, badge_winner(ss, BADGE_COVER_CHEATER));
  TEST_ASSERT_EQUAL(2, badge_winner(ss, BADGE_DAREDEVIL));
}

void test_no_badge_when_nobody_qualifies() {
  Session ss;
  TEST_ASSERT_EQUAL(-1, badge_winner(ss, BADGE_SHARPSHOOTER));
  TEST_ASSERT_EQUAL(-1, badge_winner(ss, BADGE_COVER_CHEATER));
}

void test_two_seekers_three_bodies() {
  RoundResult r = base();
  r.numSeekers = 2;
  r.body[2].used = true;
  r.body[2].alive = true;
  capture(r, 0, 30000, 1);
  capture(r, 1, 60000, 0);
  capture(r, 2, 90000, 1);
  r.endMs = 90000;
  RoundScore s = score_round(r);
  TEST_ASSERT_EQUAL(100 + 120 + PTS_CLEAN_SWEEP, s.seeker[0]);
  TEST_ASSERT_EQUAL(100 + 150 + 100 + 90 + PTS_CLEAN_SWEEP, s.seeker[1]);

  Session ss;
  ss.numPlayers = 5;
  ss.numSeekers = 2;
  session_add(ss, r, s);
  // round 0: players 0,1 seek; players 2,3,4 own bodies 0,1,2
  TEST_ASSERT_EQUAL(s.seeker[1], ss.p[1].points);
  TEST_ASSERT_EQUAL(90, ss.p[4].points);
  uint8_t sk[4], hd[4];
  TEST_ASSERT_EQUAL(2, ss.nextFirst);
  rotation(ss.nextFirst, 5, 2, sk, hd);
  TEST_ASSERT_EQUAL(2, sk[0]); TEST_ASSERT_EQUAL(3, sk[1]); TEST_ASSERT_EQUAL(4, hd[0]); TEST_ASSERT_EQUAL(0, hd[1]);
}

void test_protocol_roundtrip_and_bad_crc() {
  uint8_t buf[64];
  FoundMsg f{1, 2};
  size_t n = proto_pack(buf, MSG_FOUND, 7, &f, sizeof f);
  MsgHeader h;
  const uint8_t* pl;
  TEST_ASSERT_EQUAL(sizeof f, proto_unpack(buf, n, &h, &pl));
  TEST_ASSERT_EQUAL(MSG_FOUND, h.type);
  TEST_ASSERT_EQUAL(7, h.seq);
  TEST_ASSERT_EQUAL(2, ((FoundMsg*)pl)->shooter);
  buf[5] ^= 1;
  TEST_ASSERT_EQUAL(-1, proto_unpack(buf, n, &h, &pl));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_none_captured);
  RUN_TEST(test_all_captured);
  RUN_TEST(test_strike_out_counts_for_win_not_points);
  RUN_TEST(test_sole_survivor_gets_last_chameleon_and_ghost);
  RUN_TEST(test_single_body_is_not_last_chameleon);
  RUN_TEST(test_cover_freezes_survival);
  RUN_TEST(test_whistle_hunter_window_edge);
  RUN_TEST(test_missed_spot);
  RUN_TEST(test_taunts);
  RUN_TEST(test_seeker_out_of_hp_ends_early);
  RUN_TEST(test_rotation_three_players);
  RUN_TEST(test_three_round_totals_and_badges);
  RUN_TEST(test_no_badge_when_nobody_qualifies);
  RUN_TEST(test_two_seekers_three_bodies);
  RUN_TEST(test_protocol_roundtrip_and_bad_crc);
  return UNITY_END();
}
