// rpg_link.h — P2P battle link layer for Cardputer ADV + SX1262
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Wire format. Everything is little-endian, packed, fixed size.
// Keep this struct <= 40 bytes: at SF7/BW125/CR5 that is ~50ms airtime.
// Current size: 37 bytes.
// ---------------------------------------------------------------------------

static const uint16_t PROTO_MAGIC   = 0x5247;  // 'RG'
// BUMP THIS ON ANY CHANGE TO THE Packet LAYOUT — adding, removing, resizing or
// reordering a field. Two builds that disagree about the layout must refuse to
// pair, not misread each other's fields. The static_assert below is there to
// make an accidental layout change fail the build rather than the link.
//   1: original
//   2: commit[8] added for seed commit-reveal
//   3: dead `target` byte removed (38 -> 37)
//   4: classes, item charges and a turn cap. The LAYOUT is unchanged at 37
//      bytes — this bump is about the sim. Two peers running different battle
//      rules pair fine and then abort on the first stateHash compare, blaming
//      a tampered match for what is really firmware skew. Refusing to pair is
//      the honest answer, so a sim change earns a bump too.
static const uint8_t  PROTO_VERSION = 4;

enum PktType : uint8_t {
  PKT_BEACON   = 0x01,  // "I'm open for a duel" + commitment to my seed half
  PKT_JOIN_REQ = 0x02,  // "challenging you" + my seed half in the clear
  PKT_JOIN_ACK = 0x03,  // "accepted", reveals the committed seed half
  PKT_READY    = 0x04,  // joiner confirms the reveal; host may start the sim
  PKT_ACTION   = 0x05,  // a turn input
  PKT_ACK      = 0x06,  // generic acknowledgement of a seq
  PKT_BYE      = 0x07,  // clean disconnect / forfeit
};

// Reason codes carried in PKT_BYE's `action` byte, so the peer can say why.
enum ByeReason : uint8_t {
  BYE_NONE = 0, BYE_FORFEIT, BYE_DESYNC, BYE_TIMEOUT, BYE_BAD_COMMIT,
  // "I resolved the final turn and the match is decided." The peer is holding
  // both halves of that same turn, so it can resolve locally and reach the same
  // verdict instead of grinding out retries on an action we already consumed.
  BYE_MATCH_OVER
};

struct __attribute__((packed)) Packet {
  uint16_t magic;
  uint8_t  version;
  uint8_t  type;
  uint32_t src;        // sender id
  uint32_t dst;        // 0 = broadcast
  uint16_t seq;        // monotonic per sender, never 0
  uint16_t ackSeq;     // seq being acknowledged, 0 if none
  uint32_t seedHalf;   // handshake reveal (JOIN_REQ / JOIN_ACK)
  uint8_t  commit[8];  // handshake commitment (BEACON only)
  uint16_t turn;       // which turn this action belongs to
  uint8_t  action;     // ActionId, or ByeReason on PKT_BYE
  uint32_t stateHash;  // sender's pre-turn hash, i.e. post-turn hash of turn-1
  uint16_t crc;        // over all preceding bytes — MUST stay the last member
};

// Wire layout is frozen per PROTO_VERSION. If this fires, you changed the
// packet: bump PROTO_VERSION above and update this size to match.
static_assert(sizeof(Packet) == 37, "Packet layout changed - bump PROTO_VERSION");
static_assert(sizeof(Packet) <= 40, "Packet too large for the airtime budget");

uint16_t crc16(const uint8_t* d, size_t n);
bool     packetValid(const Packet& p);
void     packetSeal(Packet& p);   // fills magic/version/crc

// True for a well-formed packet (right magic, intact CRC) from a peer running
// a different PROTO_VERSION — distinct from packetValid's false, which also
// covers plain noise/garbage. Lets a caller tell "wrong version" apart from
// "not a packet at all" without changing packetValid's own pass/fail meaning.
//
// Only reliable when the two versions share Packet's layout: the CRC is
// recomputed over *our* sizeof(Packet), so a peer whose version changed the
// struct size (see the PROTO_VERSION history above) sends bytes that don't
// line up with our fields at all, and this fails closed — returns false,
// same as packetValid, rather than reporting the mismatch. A same-size bump
// (like v4) is the case this actually catches.
bool packetVersionMismatch(const Packet& p);

// Commit-reveal for seed agreement. The host broadcasts commitOf(hostSeed) in
// its beacon and only reveals hostSeed in JOIN_ACK, so a joiner cannot pick its
// own half to steer the final seed. 64 bits of commitment is enough that
// grinding a favourable preimage is not worth it on this hardware.
void     seedCommit(uint32_t seed, uint8_t out[8]);
bool     seedCommitMatches(uint32_t seed, const uint8_t commit[8]);

// ---------------------------------------------------------------------------
// Deterministic PRNG. Both peers MUST get identical sequences.
// xorshift32 — cheap, adequate for damage rolls, never use for crypto.
//
// The battle RNG (BattleState::rng) may ONLY be consumed from battleResolve().
// Anything else — retry jitter, UI flourishes — needs its own Rng instance, or
// the two peers' streams diverge and every subsequent turn desyncs.
// ---------------------------------------------------------------------------

struct Rng {
  uint32_t s;
  void seed(uint32_t v) { s = v ? v : 0xA5A5A5A5u; }
  uint32_t next() {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
  }
  // inclusive range, no modulo bias concerns at these sizes
  uint32_t range(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;                 // guards the div-by-zero
    return lo + (next() % (hi - lo + 1));
  }
};

// ---------------------------------------------------------------------------
// Battle model. Integers only. No floats anywhere in this file.
// ---------------------------------------------------------------------------

enum ActionId : uint8_t {
  ACT_NONE = 0, ACT_ATTACK, ACT_GUARD, ACT_SKILL, ACT_ITEM, ACT_FLEE,
  ACT_COUNT
};

// Anything arriving over the radio must go through this before being cast.
inline bool actionValid(uint8_t a) { return a < (uint8_t)ACT_COUNT; }

struct Combatant {
  char     name[12];
  int16_t  hp, hpMax;
  int16_t  mp, mpMax;
  uint8_t  atk, def, spd;
  uint8_t  guarding;   // 0/1, cleared at end of turn
  uint8_t  alive;
  // ACT_ITEM charges left. Bounded on purpose: an unlimited heal that outpaces
  // sustained damage makes a match non-terminating, and there is no turn cap
  // anywhere in the session FSM to catch that.
  uint8_t  items;
  // Selects the ACT_SKILL formula, and with it how many rng.range() calls that
  // action consumes. Both peers must agree or the streams diverge silently, so
  // it is derived from the shared seed and hashed like any other field.
  uint8_t  classId;    // index into CLASS_TABLE in rpg_link.cpp
};

struct BattleState {
  Combatant p[2];      // p[0] = host, p[1] = joiner. Fixed, never swapped.
  uint16_t  turn;
  Rng       rng;
};

// Hard stop on match length. Nothing else guarantees a turn makes progress: a
// player can spend every turn on an empty item pouch with no MP, and two of
// them would sit there forever, in lockstep, with no way out. At the cap the
// lower-HP side falls and equal HP is a draw, so battleWinner() always reaches
// a verdict. Also keeps Packet::turn clear of its uint16_t ceiling.
static const uint16_t MAX_TURNS = 100;

void     battleInit(BattleState& b, uint32_t seed);
// MP an ACT_SKILL costs this combatant. Per-class, so neither the UI nor a test
// can assume a single number. Falls back to class 0 for an invalid classId.
uint8_t  skillCostOf(const Combatant& c);
// How many classes exist, so a test can cover all of them without hardcoding a
// number the class table would silently outgrow.
uint8_t  classCount();
// There are exactly two slots, so a combatant's target is always the other one.
// Targets are therefore not parameters — a target byte off the radio used to be
// able to make an attacker hit itself.
void     battleResolve(BattleState& b, ActionId a0, ActionId a1);
uint32_t battleHash(const BattleState& b);
int      battleWinner(const BattleState& b);  // -1 ongoing, 0/1 winner, 2 draw
