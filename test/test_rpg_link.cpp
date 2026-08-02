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

// Cheap way to walk every class across a seed loop without hardcoding which
// seed lands which class (there's no such mapping anymore — class is an
// explicit arg now). Host/joiner get adjacent classes so seed variety still
// exercises every (host, joiner) pairing over enough iterations.
static uint8_t seedHostClass(uint32_t seed, int nClasses) {
  return (uint8_t)(seed % nClasses);
}
static uint8_t seedJoinerClass(uint32_t seed, int nClasses) {
  return (uint8_t)((seed + 1) % nClasses);
}

// --------------------------------------------------------------- wire format

static void testPacketLayout() {
  printf("packet layout (sizeof = %zu)\n", sizeof(Packet));
  // Airtime budget: at SF7/BW125/CR5 this must stay small.
  CHECK(sizeof(Packet) <= 40);
  // packetSeal/packetValid checksum sizeof(Packet) - 2 bytes, which is only
  // the right range while crc is the final member.
  CHECK(offsetof(Packet, crc) == sizeof(Packet) - sizeof(uint16_t));
}

// The name field shares its bytes with turn/action/stateHash (see the union in
// rpg_link.h), which is only safe because no packet type carries both. This
// pins the two properties that safety rests on: the arms really do overlap,
// and nothing outside the union moves when a name is written.
//
// What it deliberately does NOT prove is the rule itself — that a given packet
// type only ever writes one arm. Offsets stay correct through exactly that
// bug. Session::txPacket() is where that rule is enforced, by enumerating the
// types allowed to carry a name.
static void testNameSharesTheSimBytes() {
  printf("name field overlays the sim fields, and nothing else\n");
  CHECK(offsetof(Packet, name) == offsetof(Packet, turn));
  // classId was moved out of that run precisely so it could ride the same
  // JOIN packets a name does. If this fails they are aliased and the class
  // silently becomes a character of the name.
  CHECK(offsetof(Packet, classId) < offsetof(Packet, name));

  Packet p{};
  p.type = PKT_JOIN_REQ; p.src = 9; p.dst = 4; p.seedHalf = 0xDEADBEEF;
  p.classId = 3;
  memcpy(p.commit, "12345678", 8);
  memcpy(p.name, "ANNIE", 6);

  CHECK(p.classId == 3);
  CHECK(memcmp(p.commit, "12345678", 8) == 0);
  CHECK(p.seedHalf == 0xDEADBEEF);
  // A name is a real name after a round trip through the CRC, not a turn.
  packetSeal(p);
  CHECK(packetValid(p));
  CHECK(strcmp(p.name, "ANNIE") == 0);

  // The overlap is the point, so assert it rather than leaving it implied: a
  // sim packet's turn number reads back as garbage in the name arm.
  Packet a{};
  a.type = PKT_ACTION; a.turn = 0x4141; a.action = ACT_ATTACK;
  CHECK(a.name[0] == 'A');
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
  // Not resealed: crc is now stale for its own bytes, same as a single-bit
  // flip. packetVersionMismatch must reject it exactly like packetValid does.
  CHECK(!packetVersionMismatch(wrongVersion));
}

static void testVersionMismatch() {
  printf("version mismatch reported distinctly from garbage\n");
  // packetSeal() always writes *this build's* PROTO_VERSION, so a peer on a
  // different version has to be simulated by hand: same crc formula, a
  // different version byte baked in before the crc is computed over it —
  // exactly what a differently-versioned peer's own packetSeal() would do.
  Packet p{};
  p.type = PKT_ACTION; p.src = 7; p.seq = 3; p.action = ACT_ATTACK;
  p.magic   = PROTO_MAGIC;
  p.version = PROTO_VERSION + 1;
  p.crc     = crc16((const uint8_t*)&p, sizeof(Packet) - sizeof(uint16_t));
  CHECK(!packetValid(p));
  CHECK(packetVersionMismatch(p));

  // Plain garbage (bad magic) must not be reported as a version mismatch.
  Packet garbage = p;
  garbage.magic ^= 0xFF;
  CHECK(!packetVersionMismatch(garbage));

  // Same version as us: not a mismatch, even though this path is only ever
  // reached after packetValid already failed for some other reason.
  Packet sameVersion = p;
  sameVersion.version = PROTO_VERSION;
  packetSeal(sameVersion);
  CHECK(!packetVersionMismatch(sameVersion));
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
  battleInit(a, seed, 0, 1);
  battleInit(b, seed, 0, 1);
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
    {ACT_ATTACK, ACT_FLEE},                            // ends the match
  };
  const int n = (int)(sizeof(script) / sizeof(script[0]));
  for (uint32_t seed : {1u, 0x1234u, 0xFFFFFFFFu, 0xA5A5A5A5u})
    CHECK(lockstepHolds(seed, script, n));

  // Different seeds must produce a different hash immediately, so a seed
  // disagreement is caught at turn 0 rather than after damage has landed.
  BattleState x, y;
  battleInit(x, 0x1111, 0, 1);
  battleInit(y, 0x2222, 0, 1);
  CHECK(battleHash(x) != battleHash(y));
}

// ----------------------------------------------------------------- sim rules

// battleInit() must fully overwrite whatever was in the buffer. battleHash()
// covers all 12 bytes of name[] and every trailing field, so any byte left
// unwritten — padding after a short class name, a field added and forgotten —
// makes two peers with the same seed disagree before a single turn is played.
// A caller handing us a dirty stack BattleState is the normal case, not an
// abuse: lockstepHolds() below does exactly that.
static void testInitScrubsBuffer() {
  printf("battleInit overwrites a dirty buffer\n");
  for (uint32_t seed : {1u, 5u, 0x1234u, 0xFFFFFFFFu}) {
    BattleState dirty, clean;
    memset(&dirty, 0xAB, sizeof(dirty));
    memset(&clean, 0x00, sizeof(clean));
    battleInit(dirty, seed, 2, 3);
    battleInit(clean, seed, 2, 3);
    // Strictly stronger than comparing hashes: this also catches a byte the
    // hash does not currently cover but a future field might.
    CHECK(memcmp(&dirty, &clean, sizeof(dirty)) == 0);
  }
}

// Every class must be selectable in either seat, independently of the other
// seat's choice, and every field battleInit() sets from it must be correct
// and hash-covered.
static void testClassChoice() {
  printf("battleInit takes an explicit class id per seat\n");
  const int nClasses = classCount();
  CHECK(nClasses > 0 && nClasses <= 8);

  for (uint8_t hc = 0; hc < nClasses; hc++) {
    for (uint8_t jc = 0; jc < nClasses; jc++) {
      BattleState b;
      battleInit(b, 1, hc, jc);
      CHECK(b.p[0].classId == hc);
      CHECK(b.p[1].classId == jc);
      for (int i = 0; i < 2; i++) {
        CHECK(b.p[i].hp == b.p[i].hpMax && b.p[i].hp > 0);
        CHECK(b.p[i].mp == b.p[i].mpMax);
        CHECK(b.p[i].name[0] != '\0');
        // A class must be able to afford its own skill at least once, or it
        // starts the match with a button that can never do anything.
        CHECK(skillCostOf(b.p[i]) > 0 && b.p[i].mp >= skillCostOf(b.p[i]));
      }
    }
  }

  // An out-of-range id (corrupt packet, untrusted caller) falls back to
  // class 0 rather than indexing off the end of CLASS_TABLE.
  BattleState oob;
  battleInit(oob, 1, (uint8_t)nClasses, (uint8_t)(nClasses + 3));
  CHECK(oob.p[0].classId == 0 && oob.p[1].classId == 0);

  // Every field that steers a decision must be hash-covered, or two peers can
  // disagree about it indefinitely without the stateHash exchange noticing.
  // classId picks the skill formula; items decides whether ACT_ITEM rolls.
  BattleState a, b;
  battleInit(a, 1, 0, 1);
  battleInit(b, 1, 0, 1);
  CHECK(a.p[0].items == b.p[0].items && a.p[0].items > 0);
  b.p[0].classId ^= 1;
  CHECK(battleHash(a) != battleHash(b));

  battleInit(b, 1, 0, 1);
  b.p[0].items--;
  CHECK(battleHash(a) != battleHash(b));
}

// The RNG-call contract, pinned in the suite rather than in a comment. Each
// class's ACT_SKILL must consume a fixed number of rng.range() calls decided
// only by classId — never by hp, mp or damage. A conditional roll added inside
// one formula would still pass lockstepHolds() (both peers roll identically
// when they hold the same class) and would only desync a mismatched pairing,
// which is exactly the bug that is hardest to reproduce. This catches it at
// the source: advance a reference Rng by the documented number of steps and
// require the sim's stream to land in the same place.
static void testSkillRngCallCounts() {
  printf("skill rng call counts are fixed per class\n");
  // Documented in the RNG CONTRACT block in rpg_link.cpp — keep in step.
  const int calls[] = { 1, 1, 2, 1 };   // Bunyan, Drifter, Coyote, Voodoo
  const int n = classCount();
  CHECK(n == (int)(sizeof(calls) / sizeof(calls[0])));

  for (int cid = 0; cid < n; cid++) {
    for (uint32_t seed = 1; seed <= 16; seed++) {
      // Only p[0] acts, and it is guaranteed to afford its skill on turn 0.
      BattleState b;
      battleInit(b, seed, (uint8_t)cid, 0);
      Rng expect = b.rng;
      for (int k = 0; k < calls[cid]; k++) expect.range(0, 1);
      battleResolve(b, ACT_SKILL, ACT_NONE);
      CHECK(b.rng.s == expect.s);

      // A fizzle must roll nothing at all, whatever the class.
      BattleState f;
      battleInit(f, seed, (uint8_t)cid, 0);
      f.p[0].mp = 0;
      uint32_t before = f.rng.s;
      battleResolve(f, ACT_SKILL, ACT_NONE);
      CHECK(f.rng.s == before);
    }
  }
}

static void testNoSelfDamage() {
  printf("attacker never damages itself\n");
  // The old sim took a target byte off the radio and indexed p[target & 1],
  // so a peer could make its opponent attack itself. Targets are gone now;
  // this pins the property down.
  BattleState b;
  battleInit(b, 1, 0, 1);
  int16_t hostBefore = b.p[0].hp;
  battleResolve(b, ACT_ATTACK, ACT_NONE);
  CHECK(b.p[0].hp == hostBefore);
  CHECK(b.p[1].hp < b.p[1].hpMax);
}

static void testFleeForfeits() {
  printf("flee forfeits unconditionally\n");

  // One side flees: they lose outright, no roll involved, no damage exchanged.
  {
    BattleState b;
    battleInit(b, 1, 0, 1);
    int16_t foeHpBefore = b.p[1].hp;
    battleResolve(b, ACT_FLEE, ACT_NONE);
    CHECK(battleWinner(b) == 1);
    CHECK(b.p[1].hp == foeHpBefore);    // the side that stayed took no damage
  }

  // Both flee the same turn: a draw via the same dead-heat path a double-KO
  // already uses, not a special case.
  {
    BattleState b;
    battleInit(b, 1, 0, 1);
    battleResolve(b, ACT_FLEE, ACT_FLEE);
    CHECK(battleWinner(b) == 2);
  }
  // Determinism is covered by testLockstep(), which includes an ACT_FLEE step.
}

static void testGuardReducesDamage() {
  printf("guard mitigates\n");
  uint32_t seed = 99;
  BattleState unguarded, guarded;
  battleInit(unguarded, seed, 0, 1);
  battleInit(guarded, seed, 0, 1);
  battleResolve(unguarded, ACT_ATTACK, ACT_NONE);
  battleResolve(guarded,   ACT_ATTACK, ACT_GUARD);
  CHECK(guarded.p[1].hp >= unguarded.p[1].hp);
  // Guard is a one-turn effect.
  CHECK(guarded.p[1].guarding == 0);
}

static void testSkillCostsMp() {
  printf("skill drains mp and fizzles when empty\n");
  BattleState b;
  battleInit(b, 5, 0, 1);
  int16_t mp0 = b.p[0].mp;
  uint8_t cost = skillCostOf(b.p[0]);  // per-class; do not hardcode
  CHECK(cost > 0);
  battleResolve(b, ACT_SKILL, ACT_NONE);
  CHECK(b.p[0].mp == mp0 - cost);

  b.p[0].mp = 2;                       // below every class's cost
  int16_t foeHp = b.p[1].hp;
  battleResolve(b, ACT_SKILL, ACT_NONE);
  CHECK(b.p[0].mp == 2);               // fizzled, nothing spent
  CHECK(b.p[1].hp == foeHp);           // and nothing landed
}

static void testItemCapsAtMax() {
  printf("item never overheals\n");
  BattleState b;
  battleInit(b, 7, 0, 1);
  b.p[0].hp = b.p[0].hpMax - 1;
  battleResolve(b, ACT_ITEM, ACT_NONE);
  CHECK(b.p[0].hp == b.p[0].hpMax);
}

// Initiative ties used to fall to slot 0 always, which handed the host ~86% of
// mirror matches — invisible while both placeholder rosters had the same spd,
// and a real fairness bug once classes made mirrors a quarter of all pairings.
// Ties now alternate on turn parity. Two things to hold: the alternation itself,
// and the outcome it exists to fix.
static void testMirrorsAreFair() {
  printf("mirror matches do not favour the host\n");

  // The mechanism. Equal spd, so parity alone decides, and turn 0 is the host's.
  // Read through damage: whoever strikes first in a mutual-kill turn survives.
  for (int turn = 0; turn < 2; turn++) {
    BattleState b;
    battleInit(b, 1, 0, 1);
    b.p[1] = b.p[0];                   // an exact mirror, so only parity differs
    b.turn = turn;
    b.p[0].hp = b.p[1].hp = 1;         // either blow ends it
    battleResolve(b, ACT_ATTACK, ACT_ATTACK);
    CHECK(battleWinner(b) == turn % 2);  // even turn -> host strikes first
  }

  // The outcome. Attack-only, so the match is decided by initiative and rng and
  // nothing else — the cleanest look at the bias there is. Every class gets a
  // mirror here since class is now chosen, not drawn — a wider check than the
  // old seed-hunted sample that only landed a mirror ~1/4 of the time.
  int wins[2] = {0, 0}, mirrors = 0;
  const int nClasses = classCount();
  for (int cid = 0; cid < nClasses; cid++) {
    for (uint32_t seed = 1; seed <= 1000; seed++) {
      BattleState b;
      battleInit(b, seed, (uint8_t)cid, (uint8_t)cid);
      mirrors++;
      int guard = 0;
      while (battleWinner(b) == -1 && guard++ < 500)
        battleResolve(b, ACT_ATTACK, ACT_ATTACK);
      int w = battleWinner(b);
      if (w >= 0) wins[w]++;
    }
  }
  CHECK(mirrors > 200);                // enough of a sample to mean anything
  // Wide on purpose. This is a regression bound, not a balance target: the old
  // tiebreak sat at ~86%, and pinning it tighter would make the test fail for
  // ordinary sim edits that are nobody's fault.
  int pct = wins[0] * 100 / (wins[0] + wins[1]);
  CHECK(pct >= 40 && pct <= 60);
}

static void testFightTerminates() {
  printf("fights terminate cleanly\n");
  const int nClasses = classCount();
  for (uint32_t seed = 1; seed <= 64; seed++) {
    BattleState b;
    battleInit(b, seed, seedHostClass(seed, nClasses), seedJoinerClass(seed, nClasses));
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

// testFightTerminates plays ATTACK-only, which converges for every pairing and
// so cannot see a stalemate. This plays the whole move set, including ITEM,
// which is the one action that ADDS hp. When ITEM was unlimited, ~a third of
// these never finished — a real hang on two radios, since neither the sim nor
// the session FSM caps the turn count.
static void testFightTerminatesUnderAllActions() {
  printf("fights terminate against a healing opponent\n");
  const ActionId moves[] = { ACT_ATTACK, ACT_GUARD, ACT_SKILL, ACT_ITEM };
  const int cap = MAX_TURNS * 4;       // generous; the sim should stop long first
  const int nClasses = classCount();
  int worst = 0;

  for (uint32_t seed = 1; seed <= 400; seed++) {
    // Two pilots. Uniform-random only grinds; the stalemate needs a player who
    // heals whenever hurt, which is also what a human actually does.
    for (int greedy = 0; greedy < 2; greedy++) {
      BattleState b;
      battleInit(b, seed, seedHostClass(seed, nClasses), seedJoinerClass(seed, nClasses));
      Rng pick;                        // NOT the battle rng — that would desync
      pick.seed(seed * 2654435761u + 1);
      int guard = 0;
      while (battleWinner(b) == -1 && guard++ < cap) {
        ActionId a[2];
        for (int i = 0; i < 2; i++)
          a[i] = greedy ? (b.p[i].hp * 3 < b.p[i].hpMax ? ACT_ITEM
                          : b.p[i].mp >= skillCostOf(b.p[i]) ? ACT_SKILL : ACT_ATTACK)
                        : moves[pick.range(0, 3)];
        battleResolve(b, a[0], a[1]);
      }
      if (guard > worst) worst = guard;
      CHECK(guard < cap);
    }
  }
  printf("  longest fight %d turns\n", worst);

  // The pathological case the turn cap exists for: both sides spend every turn
  // on an action that cannot do anything. Bounding the item pouch alone made
  // this WORSE, because an empty pouch fizzles instead of healing.
  for (uint32_t seed = 1; seed <= 64; seed++) {
    BattleState b;
    battleInit(b, seed, seedHostClass(seed, nClasses), seedJoinerClass(seed, nClasses));
    b.p[0].items = b.p[1].items = 0;
    b.p[0].mp    = b.p[1].mp    = 0;
    int guard = 0;
    while (battleWinner(b) == -1 && guard++ < cap)
      battleResolve(b, ACT_ITEM, ACT_ITEM);
    CHECK(guard < cap);
    CHECK(battleWinner(b) != -1);      // the cap must produce a verdict
  }

  // And the heal stays bounded, or the pouch is decorative.
  BattleState b;
  battleInit(b, 9, 0, 1);
  for (int i = 0; i < 10; i++) {
    b.p[0].hp = 1;
    battleResolve(b, ACT_ITEM, ACT_NONE);
  }
  CHECK(b.p[0].items == 0);
  CHECK(b.p[0].hp == 1);               // pouch empty: the last heals did nothing
}

static void testWinnerReporting() {
  printf("winner reporting\n");
  BattleState b;
  battleInit(b, 3, 0, 1);
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
  testNameSharesTheSimBytes();
  testSealAndValidate();
  testVersionMismatch();
  testSeedCommit();
  testLockstep();
  testInitScrubsBuffer();
  testClassChoice();
  testSkillRngCallCounts();
  testNoSelfDamage();
  testFleeForfeits();
  testGuardReducesDamage();
  testSkillCostsMp();
  testItemCapsAtMax();
  testMirrorsAreFair();
  testFightTerminates();
  testFightTerminatesUnderAllActions();
  testWinnerReporting();
  testRngGuards();
  testActionValidation();

  return testSummary("all tests passed");
}
