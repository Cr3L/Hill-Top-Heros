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

static bool packetCrcOk(const Packet& p) {
  return p.crc == crc16((const uint8_t*)&p, sizeof(Packet) - sizeof(uint16_t));
}

bool packetValid(const Packet& p) {
  return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION && packetCrcOk(p);
}

bool packetVersionMismatch(const Packet& p) {
  return p.magic == PROTO_MAGIC && p.version != PROTO_VERSION && packetCrcOk(p);
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

// ------------------------------------------------------------------- classes
//
// RNG CONTRACT. The number of rng.range() calls ACT_SKILL makes depends only on
// classId — never on hp, mp, guarding or damage. Both peers hold the same
// classId (it comes from the shared seed and is hashed), so both make the same
// calls in the same order. A skill that rolled an extra die "if the foe is
// below half" would happen to stay in sync today and would be a trap for the
// next edit; don't introduce one.
//
//   classId  name     rng.range() calls, in order
//   0        Bunyan   (0,4)
//   1        Drifter  (0,12)
//   2        Coyote   (0,5), (0,5)
//   3        Voodoo   (0,6)
//
// The MP check happens before any roll, so a fizzle consumes nothing.

// Ordered to match CLASS_TABLE. Reordering either without the other silently
// remaps every formula, so the switch below labels its cases with these.
enum ClassId : uint8_t { CLS_BUNYAN, CLS_DRIFTER, CLS_COYOTE, CLS_VOODOO };

// ACT_ITEM heals 18-24, which is more than a sustained ATTACK deals against
// every class in the table. Unlimited, that is not a balance wart but a
// non-terminating match: with the pools below, a third of random-action fights
// never end, and nothing in the session FSM caps the turn count. Three is
// enough to matter and few enough that damage always wins eventually.
static const uint8_t ITEM_CHARGES = 3;

struct ClassDef {
  const char* name;    // string literal; <= 8 chars, the display is 20 wide
  int16_t hpMax, mpMax;
  uint8_t atk, def, spd;
  uint8_t skillCost;
  uint8_t drainDiv;    // heal dmg/drainDiv on a skill; 0 = no drain
};

// Tuned against a scripted-pilot sweep of every pairing; the numbers and the
// method are in README.md under "Classes", one copy. The pools are what was
// tuned; atk/def/spd and the formulas carry the identities and were held fixed.
// Changing any number here is a balance change — re-run the sweep, don't guess.
static const ClassDef CLASS_TABLE[] = {
  /* name       hpMax mpMax atk def spd cost drain */
  { "Bunyan",   102,   21,  10,   9,   5,   6,   0 },
  { "Drifter",   70,   24,  14,   4,   8,   8,   0 },
  { "Coyote",    87,   24,  11,   5,  14,   6,   0 },
  { "Voodoo",    88,   31,   8,   7,   7,   7,   4 },
};
static const uint8_t CLASS_COUNT = sizeof(CLASS_TABLE) / sizeof(CLASS_TABLE[0]);
// battleInit() takes two seed bits per side, so the table has to be exactly
// four entries. Add a fifth class and the draw silently never picks it.
static_assert(CLASS_COUNT == 4, "class draw uses 2 seed bits per side");

// One bounds policy for a corrupt classId, in one place. Two call sites with
// two different fallbacks would invent a third behaviour between them.
static const ClassDef& classDefOf(const Combatant& c) {
  return CLASS_TABLE[c.classId < CLASS_COUNT ? c.classId : 0];
}

uint8_t classCount() { return CLASS_COUNT; }
uint8_t skillCostOf(const Combatant& c) { return classDefOf(c).skillCost; }

void battleInit(BattleState& b, uint32_t seed) {
  // Must stay first. battleHash() covers all 12 bytes of name[] and every
  // trailing field, and callers routinely hand us an uninitialised stack
  // BattleState — without this, two peers with the same seed hash differently.
  memset(&b, 0, sizeof(b));
  b.rng.seed(seed);   // turn and every other field are already zeroed above

  for (int i = 0; i < 2; i++) {
    // Class comes from seed bits, not from the rng: drawing here would shift
    // every subsequent damage roll and invalidate the streams the tests pin.
    uint8_t cid = (uint8_t)((seed >> (i * 2)) & 3);
    const ClassDef& cd = CLASS_TABLE[cid];
    Combatant& c = b.p[i];
    strncpy(c.name, cd.name, sizeof(c.name) - 1);
    c.hp = c.hpMax = cd.hpMax;
    c.mp = c.mpMax = cd.mpMax;
    c.atk = cd.atk; c.def = cd.def; c.spd = cd.spd;
    c.alive = 1;
    c.items = ITEM_CHARGES;
    c.classId = cid;
  }
}

// The sim's two damage rules — a floor of 1, and clamped healing — stated once
// each so ATTACK, SKILL and ITEM cannot drift apart.
static int32_t dealDamage(Combatant& foe, int32_t raw, int32_t mit) {
  int32_t dmg = raw - mit;
  if (dmg < 1) dmg = 1;
  foe.hp -= (int16_t)dmg;
  return dmg;              // what actually landed, for drain
}

static void healBy(Combatant& c, int32_t amount) {
  c.hp += (int16_t)amount;
  if (c.hp > c.hpMax) c.hp = c.hpMax;
}

static void applyAction(BattleState& b, int self, ActionId a) {
  Combatant& me  = b.p[self];
  Combatant& foe = b.p[self ^ 1];   // two slots; never aliases `me`
  if (!me.alive) return;

  switch (a) {
    case ACT_ATTACK: {
      int32_t base = me.atk + (int32_t)b.rng.range(0, 5);
      int32_t mit  = foe.def + (foe.guarding ? foe.def : 0);
      dealDamage(foe, base, mit / 2);
      break;
    }
    case ACT_GUARD:
      // Already raised in battleResolve, before anyone acted. Nothing to do.
      break;
    case ACT_SKILL: {
      const ClassDef& cd = classDefOf(me);
      if (me.mp < cd.skillCost) break;   // fizzle, costs the turn, rolls nothing
      me.mp -= cd.skillCost;

      // Skills ignore GUARD and are mitigated by half of what an attack is —
      // guarding is the answer to ATTACK, HP and MP are the answer to skills.
      int32_t mit = foe.def / 2;
      int32_t raw = 0;
      switch (me.classId) {
        case CLS_BUNYAN:   // Timber Cleave — the only skill that pays off DEF,
                           // which is what lets the tank stat do something on
                           // offence rather than only soaking.
          raw = me.atk + me.def + (int32_t)b.rng.range(0, 4);
          break;
        case CLS_DRIFTER:  // Fan the Hammer — biggest and swingiest, and the
                           // priciest, on the thinnest HP pool in the table.
          raw = me.atk * 2 + (int32_t)b.rng.range(0, 12);
          break;
        case CLS_COYOTE:   // Twin Fangs — two rolls rather than one wide one,
                           // so the same average lands on a tighter spread.
          raw = me.atk * 2 + (int32_t)b.rng.range(0, 5)
                           + (int32_t)b.rng.range(0, 5);
          break;
        case CLS_VOODOO:   // Poppet Pin — scales off the FOE's atk. Nothing
                           // else in the sim reads an opposing stat; that is
                           // the whole identity. It punishes the glass cannons
                           // and does least to the tank that can grind it out.
          raw = me.atk + foe.atk + (int32_t)b.rng.range(0, 6);
          break;
        default:           // Unreachable while classId is hashed; a mismatch is
                           // a desync we would already have caught. classDefOf()
                           // charged class 0's MP, so land its floor and stop.
          break;
      }

      int32_t dmg = dealDamage(foe, raw, mit);
      // Drain lands even on a killing blow. Table-driven, so a second draining
      // class is a column edit rather than another branch here.
      if (cd.drainDiv) healBy(me, dmg / cd.drainDiv);
      break;
    }
    case ACT_ITEM:
      // Charge check before the roll, exactly like the skill fizzle: an empty
      // pouch costs the turn and consumes no RNG. `items` is hashed, so both
      // peers run out on the same turn and stay in step.
      if (!me.items) break;
      me.items--;
      healBy(me, 18 + (int32_t)b.rng.range(0, 6));
      break;
    case ACT_FLEE:
      // Immediate, unconditional forfeit. Deterministic and rng-free like
      // every other fizzle path, so both peers agree the instant this packet
      // lands rather than needing the stateHash round-trip to catch a
      // disagreement. If both sides flee the same turn, both end up !alive
      // and battleWinner() reports it as the existing dead-heat draw — no
      // separate case needed.
      me.alive = 0;
      break;
    default: break;                      // ACT_NONE: not yet a move
  }
  if (foe.hp <= 0) { foe.hp = 0; foe.alive = 0; }
}

void battleResolve(BattleState& b, ActionId a0, ActionId a1) {
  // Guard resolves before anything else, regardless of speed.
  if (a0 == ACT_GUARD) b.p[0].guarding = 1;
  if (a1 == ACT_GUARD) b.p[1].guarding = 1;

  // Initiative: higher spd first. A tie alternates by turn parity rather than
  // always falling to slot 0 — with classes, mirrors are common, and a fixed
  // tiebreak handed the host ~86% of them. Parity is read from state both peers
  // already agree on, so it costs no rng and cannot desync.
  // Never break ties with rng here unless BOTH sides consume it identically.
  bool hostFirst = b.p[0].spd != b.p[1].spd ? b.p[0].spd > b.p[1].spd
                                            : (b.turn % 2) == 0;

  if (hostFirst) { applyAction(b, 0, a0); applyAction(b, 1, a1); }
  else           { applyAction(b, 1, a1); applyAction(b, 0, a0); }

  b.p[0].guarding = 0;
  b.p[1].guarding = 0;
  b.turn++;

  // Time limit. Decided on HP so it is not a coin flip, and computed from state
  // both peers already agree on, so it needs no RNG and cannot desync.
  if (b.turn >= MAX_TURNS && b.p[0].alive && b.p[1].alive) {
    // Compared as a FRACTION of each pool, cross-multiplied to stay in integers.
    // Raw hp would hand the win to whoever has the bigger pool: Bunyan's 102
    // beats a Drifter at full health on 70, which is not "who was winning".
    // Peaks around 10k, nowhere near int32_t.
    int32_t lhs = (int32_t)b.p[0].hp * b.p[1].hpMax;
    int32_t rhs = (int32_t)b.p[1].hp * b.p[0].hpMax;
    if (lhs != rhs) {
      Combatant& loser = (lhs < rhs) ? b.p[0] : b.p[1];
      loser.hp = 0;
      loser.alive = 0;
    } else {
      b.p[0].alive = b.p[1].alive = 0;   // dead heat, battleWinner() reports 2
    }
  }
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
    hashBytes(h, &c.items, sizeof(c.items));
    hashBytes(h, &c.classId, sizeof(c.classId));
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
