#include "rpg_link.h"
#include <string.h>

#if defined(__has_include)
#  if __has_include("mbedtls/sha256.h")
#    include "mbedtls/sha256.h"
#    define RPG_HAVE_MBEDTLS 1
#  endif
#endif

// --------------------------------------------------------------- CRC + frame

uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++)
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

void packetSeal(Packet& p) {
  p.magic   = PROTO_MAGIC;
  p.version = PROTO_VERSION;
  p.crc     = crc16((const uint8_t*)&p, sizeof(Packet) - sizeof(uint16_t));
}

bool packetValid(const Packet& p) {
  if (p.magic != PROTO_MAGIC || p.version != PROTO_VERSION) return false;
  return p.crc == crc16((const uint8_t*)&p, sizeof(Packet) - sizeof(uint16_t));
}

// ------------------------------------------------------------ seed commitment

void seedCommit(uint32_t seed, uint8_t out[8]) {
  // Domain-separated so a commitment can never be replayed as anything else.
  uint8_t in[8] = { 'R','P','G','S',
                    (uint8_t)(seed), (uint8_t)(seed >> 8),
                    (uint8_t)(seed >> 16), (uint8_t)(seed >> 24) };
#ifdef RPG_HAVE_MBEDTLS
  uint8_t full[32];
  mbedtls_sha256(in, sizeof(in), full, 0);
  memcpy(out, full, 8);
#else
  // Fallback: two independent FNV-1a passes. Weaker than SHA-256 but still
  // costs a grinder real work for 64 bits, and this path is only taken off-ESP32.
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < sizeof(in); i++) { h ^= in[i]; h *= 1099511628211ull; }
  for (int i = 0; i < 8; i++) out[i] = (uint8_t)(h >> (8 * i));
#endif
}

bool seedCommitMatches(uint32_t seed, const uint8_t commit[8]) {
  uint8_t expect[8];
  seedCommit(seed, expect);
  return memcmp(expect, commit, 8) == 0;
}

// ------------------------------------------------------------------- battle

static void mkCombatant(Combatant& c, const char* nm,
                        int16_t hp, int16_t mp,
                        uint8_t atk, uint8_t def, uint8_t spd) {
  memset(&c, 0, sizeof(c));
  strncpy(c.name, nm, sizeof(c.name) - 1);
  c.hp = c.hpMax = hp;
  c.mp = c.mpMax = mp;
  c.atk = atk; c.def = def; c.spd = spd;
  c.alive = 1;
}

void battleInit(BattleState& b, uint32_t seed) {
  memset(&b, 0, sizeof(b));
  b.rng.seed(seed);
  b.turn = 0;
  // Placeholder rosters — swap for your class/loadout system later.
  mkCombatant(b.p[0], "HOST", 60, 20, 12, 6, 9);
  mkCombatant(b.p[1], "GUEST", 60, 20, 12, 6, 9);
}

static void applyAction(BattleState& b, int self, ActionId a) {
  Combatant& me  = b.p[self];
  Combatant& foe = b.p[self ^ 1];   // two slots; never aliases `me`
  if (!me.alive) return;

  switch (a) {
    case ACT_ATTACK: {
      int32_t base = me.atk + (int32_t)b.rng.range(0, 5);
      int32_t mit  = foe.def + (foe.guarding ? foe.def : 0);
      int32_t dmg  = base - (mit / 2);
      if (dmg < 1) dmg = 1;
      foe.hp -= (int16_t)dmg;
      break;
    }
    case ACT_GUARD:
      // Already raised in battleResolve, before anyone acted. Nothing to do.
      break;
    case ACT_SKILL: {
      if (me.mp < 6) break;              // fizzle, still costs the turn
      me.mp -= 6;
      int32_t dmg = (me.atk * 3) / 2 + (int32_t)b.rng.range(0, 8);
      if (dmg < 1) dmg = 1;
      foe.hp -= (int16_t)dmg;
      break;
    }
    case ACT_ITEM: {
      int32_t heal = 18 + (int32_t)b.rng.range(0, 6);
      me.hp += (int16_t)heal;
      if (me.hp > me.hpMax) me.hp = me.hpMax;
      break;
    }
    default: break;                      // ACT_NONE / ACT_FLEE: not yet a move
  }
  if (foe.hp <= 0) { foe.hp = 0; foe.alive = 0; }
}

void battleResolve(BattleState& b, ActionId a0, ActionId a1) {
  // Guard resolves before anything else, regardless of speed.
  if (a0 == ACT_GUARD) b.p[0].guarding = 1;
  if (a1 == ACT_GUARD) b.p[1].guarding = 1;

  // Initiative: higher spd first; tie broken deterministically by slot 0.
  // Never break ties with rng here unless BOTH sides consume it identically.
  bool hostFirst = (b.p[0].spd >= b.p[1].spd);

  if (hostFirst) { applyAction(b, 0, a0); applyAction(b, 1, a1); }
  else           { applyAction(b, 1, a1); applyAction(b, 0, a0); }

  b.p[0].guarding = 0;
  b.p[1].guarding = 0;
  b.turn++;
}

// FNV-1a over the meaningful fields. Hashing the raw struct would also cover
// alignment padding, which only happens to be zero because battleInit memsets —
// an invariant no future edit is obliged to preserve. Field-by-field instead.
static void hashBytes(uint32_t& h, const void* p, size_t n) {
  const uint8_t* d = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
}

uint32_t battleHash(const BattleState& b) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 2; i++) {
    const Combatant& c = b.p[i];
    hashBytes(h, c.name, sizeof(c.name));
    hashBytes(h, &c.hp, sizeof(c.hp));       hashBytes(h, &c.hpMax, sizeof(c.hpMax));
    hashBytes(h, &c.mp, sizeof(c.mp));       hashBytes(h, &c.mpMax, sizeof(c.mpMax));
    hashBytes(h, &c.atk, sizeof(c.atk));     hashBytes(h, &c.def, sizeof(c.def));
    hashBytes(h, &c.spd, sizeof(c.spd));
    hashBytes(h, &c.guarding, sizeof(c.guarding));
    hashBytes(h, &c.alive, sizeof(c.alive));
  }
  hashBytes(h, &b.turn, sizeof(b.turn));
  hashBytes(h, &b.rng.s, sizeof(b.rng.s));   // rng position is part of the state
  return h;
}

int battleWinner(const BattleState& b) {
  bool a = b.p[0].alive, c = b.p[1].alive;
  if (a && c)   return -1;
  if (!a && !c) return 2;
  return a ? 0 : 1;
}
