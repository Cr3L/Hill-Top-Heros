// Host-side tests for the wire format and the battle sim.
//
// The link layer (main.cpp) needs two radios and cannot be tested here. What
// CAN be tested here is the part that must never drift: two peers fed the same
// seed and the same action stream have to stay bit-identical forever. If
// lockstepHolds() ever fails, a sim edit introduced a desync.
//
//   make -C test && test/run
//
#include "rpg_link.h"
#include "test_common.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

// --------------------------------------------------------------- wire format

static void testPacketLayout() {
  printf("packet layout (sizeof = %zu)\n", sizeof(Packet));
  // Airtime budget: at SF7/BW125/CR5 this must stay small.
  CHECK(sizeof(Packet) <= 40);
  // packetSeal/packetValid checksum sizeof(Packet) - 2 bytes, which is only
  // the right range while crc is the final member.
  CHECK(offsetof(Packet, crc) == sizeof(Packet) - sizeof(uint16_t));
}

static void testSealAndValidate() {
  printf("seal / validate\n");
  Packet p{};
  p.type = PKT_ACTION; p.src = 7; p.seq = 3; p.action = ACT_ATTACK;
  packetSeal(p);
  CHECK(p.magic == PROTO_MAGIC);
  CHECK(p.version == PROTO_VERSION);
  CHECK(packetValid(p));

  // Every single-bit flip in the body must be rejected.
  for (size_t byte = 0; byte < sizeof(Packet) - sizeof(uint16_t); byte++) {
    for (int bit = 0; bit < 8; bit++) {
      Packet q = p;
      ((uint8_t*)&q)[byte] ^= (uint8_t)(1 << bit);
      if (packetValid(q)) {
        printf("  FAIL bit flip byte %zu bit %d survived CRC\n", byte, bit);
        g_failures++;
      }
    }
  }

  Packet wrongVersion = p;
  wrongVersion.version = PROTO_VERSION + 1;
  CHECK(!packetValid(wrongVersion));
}

// ------------------------------------------------------------ commit-reveal

static void testSeedCommit() {
  printf("seed commit-reveal\n");
  uint8_t c[8];
  seedCommit(0xDEADBEEF, c);
  CHECK(seedCommitMatches(0xDEADBEEF, c));
  CHECK(!seedCommitMatches(0xDEADBEEE, c));   // one bit off
  CHECK(!seedCommitMatches(0, c));

  // Distinct seeds must not collide over a decent sample.
  uint8_t a[8], b[8];
  seedCommit(1, a);
  seedCommit(2, b);
  CHECK(memcmp(a, b, 8) != 0);

  // A commitment must not be trivially all-zero for a zero seed.
  uint8_t z[8];
  seedCommit(0, z);
  bool allZero = true;
  for (int i = 0; i < 8; i++) if (z[i]) allZero = false;
  CHECK(!allZero);
}

// -------------------------------------------------------------- determinism

// The core invariant: two independent states, same seed, same actions, always
// the same hash. This is what the on-air stateHash check is protecting.
static bool lockstepHolds(uint32_t seed, const ActionId (*script)[2], int n) {
  BattleState a, b;
  battleInit(a, seed);
  battleInit(b, seed);
  if (battleHash(a) != battleHash(b)) return false;
  for (int i = 0; i < n; i++) {
    battleResolve(a, script[i][0], script[i][1]);
    battleResolve(b, script[i][0], script[i][1]);
    if (battleHash(a) != battleHash(b)) {
      printf("  desync at turn %u\n", a.turn);
      return false;
    }
  }
  return true;
}

static void testLockstep() {
  printf("lockstep determinism\n");
  const ActionId script[][2] = {
    {ACT_ATTACK, ACT_GUARD}, {ACT_SKILL,  ACT_ATTACK},
    {ACT_ITEM,   ACT_SKILL}, {ACT_ATTACK, ACT_ATTACK},
    {ACT_GUARD,  ACT_SKILL}, {ACT_ATTACK, ACT_ITEM},
    {ACT_NONE,   ACT_ATTACK}, {ACT_SKILL, ACT_SKILL},
  };
  const int n = (int)(sizeof(script) / sizeof(script[0]));
  for (uint32_t seed : {1u, 0x1234u, 0xFFFFFFFFu, 0xA5A5A5A5u})
    CHECK(lockstepHolds(seed, script, n));

  // Different seeds must produce a different hash immediately, so a seed
  // disagreement is caught at turn 0 rather than after damage has landed.
  BattleState x, y;
  battleInit(x, 0x1111);
  battleInit(y, 0x2222);
  CHECK(battleHash(x) != battleHash(y));
}

// ----------------------------------------------------------------- sim rules

static void testNoSelfDamage() {
  printf("attacker never damages itself\n");
  // The old sim took a target byte off the radio and indexed p[target & 1],
  // so a peer could make its opponent attack itself. Targets are gone now;
  // this pins the property down.
  BattleState b;
  battleInit(b, 1);
  int16_t hostBefore = b.p[0].hp;
  battleResolve(b, ACT_ATTACK, ACT_NONE);
  CHECK(b.p[0].hp == hostBefore);
  CHECK(b.p[1].hp < b.p[1].hpMax);
}

static void testGuardReducesDamage() {
  printf("guard mitigates\n");
  uint32_t seed = 99;
  BattleState unguarded, guarded;
  battleInit(unguarded, seed);
  battleInit(guarded, seed);
  battleResolve(unguarded, ACT_ATTACK, ACT_NONE);
  battleResolve(guarded,   ACT_ATTACK, ACT_GUARD);
  CHECK(guarded.p[1].hp >= unguarded.p[1].hp);
  // Guard is a one-turn effect.
  CHECK(guarded.p[1].guarding == 0);
}

static void testSkillCostsMp() {
  printf("skill drains mp and fizzles when empty\n");
  BattleState b;
  battleInit(b, 5);
  int16_t mp0 = b.p[0].mp;
  battleResolve(b, ACT_SKILL, ACT_NONE);
  CHECK(b.p[0].mp == mp0 - 6);

  b.p[0].mp = 2;                       // below cost
  int16_t foeHp = b.p[1].hp;
  battleResolve(b, ACT_SKILL, ACT_NONE);
  CHECK(b.p[0].mp == 2);               // fizzled, nothing spent
  CHECK(b.p[1].hp == foeHp);           // and nothing landed
}

static void testItemCapsAtMax() {
  printf("item never overheals\n");
  BattleState b;
  battleInit(b, 7);
  b.p[0].hp = b.p[0].hpMax - 1;
  battleResolve(b, ACT_ITEM, ACT_NONE);
  CHECK(b.p[0].hp == b.p[0].hpMax);
}

static void testFightTerminates() {
  printf("fights terminate cleanly\n");
  for (uint32_t seed = 1; seed <= 64; seed++) {
    BattleState b;
    battleInit(b, seed);
    int guard = 0;
    while (battleWinner(b) == -1 && guard++ < 500)
      battleResolve(b, ACT_ATTACK, ACT_ATTACK);
    CHECK(guard < 500);
    // hp is clamped at 0 — a negative value would render as garbage and would
    // also change the state hash in ways the two peers could disagree on.
    CHECK(b.p[0].hp >= 0 && b.p[1].hp >= 0);
    CHECK(!(b.p[0].alive && b.p[1].alive));
  }
}

static void testWinnerReporting() {
  printf("winner reporting\n");
  BattleState b;
  battleInit(b, 3);
  CHECK(battleWinner(b) == -1);
  b.p[1].alive = 0; CHECK(battleWinner(b) == 0);
  b.p[0].alive = 0; CHECK(battleWinner(b) == 2);
  b.p[1].alive = 1; CHECK(battleWinner(b) == 1);
}

// ----------------------------------------------------------------- rng guard

static void testRngGuards() {
  printf("rng guards\n");
  Rng r;
  r.seed(0);
  CHECK(r.s != 0);                     // a zero state would lock xorshift up
  r.seed(5);
  CHECK(r.range(3, 3) == 3);           // used to be a divide by zero
  CHECK(r.range(9, 2) == 9);           // inverted bounds must not crash
  for (int i = 0; i < 1000; i++) {
    uint32_t v = r.range(2, 7);
    CHECK(v >= 2 && v <= 7);
  }
}

static void testActionValidation() {
  printf("action validation\n");
  CHECK(actionValid(ACT_NONE));
  CHECK(actionValid(ACT_FLEE));
  CHECK(!actionValid(ACT_COUNT));
  CHECK(!actionValid(99));
  CHECK(!actionValid(255));
}

int main() {
  testPacketLayout();
  testSealAndValidate();
  testSeedCommit();
  testLockstep();
  testNoSelfDamage();
  testGuardReducesDamage();
  testSkillCostsMp();
  testItemCapsAtMax();
  testFightTerminates();
  testWinnerReporting();
  testRngGuards();
  testActionValidation();

  return testSummary("all tests passed");
}
