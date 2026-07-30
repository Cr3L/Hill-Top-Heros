// rpg_session.h — the duel session state machine.
//
// Deliberately free of hardware: no RadioLib, no M5, no millis(), no Serial.
// Everything the FSM needs from the outside arrives through the three
// interfaces below, so the exact same compiled logic runs on the Cardputer and
// against the simulated lossy channel in test/.
#pragma once
#include "rpg_link.h"

enum LinkState : uint8_t {
  LS_IDLE, LS_BEACONING, LS_JOINING, LS_HANDSHAKE, LS_MY_TURN,
  LS_WAIT_PEER, LS_OVER
};

const char* linkStateName(LinkState s);

// ---------------------------------------------------------------- injection

// send() is blocking and returns once the packet is on air; the radio is deaf
// for its duration. recv() is non-blocking and returns false when nothing is
// waiting. Neither is expected to validate anything — the session checks CRC
// and version itself, so a corrupt frame can be injected in tests.
struct Transport {
  virtual ~Transport() = default;
  virtual void send(const Packet& p) = 0;
  virtual bool recv(Packet& out) = 0;
};

struct Clock {
  virtual ~Clock() = default;
  virtual uint32_t now() = 0;          // millis()-equivalent, wraps at 2^32
};

struct SessionUi {
  virtual ~SessionUi() = default;
  virtual void status(const char* line) { (void)line; }
  virtual void battle() {}             // redraw from Session accessors
  virtual void log(const char* msg) { (void)msg; }
};

// ------------------------------------------------------------------ session

struct SeenEntry { uint32_t src; uint16_t seq; };

class Session {
 public:
  static constexpr uint32_t RETRY_MS        = 900;
  static constexpr uint32_t RETRY_JITTER_MS = 400;
  // 4 was measurably too few: a couple of unlucky ACK losses at the end of a
  // match left one peer stranded. See the loss sweep in test/test_session.cpp.
  static constexpr uint8_t  MAX_TRIES       = 6;
  // Found on real hardware, not in the sim: a human takes longer to pick a
  // move than any scripted test action does, and no packets flow while both
  // sides just sit on the menu. Must clear MOVE_TIMEOUT_MS with margin, or a
  // legitimate decision window trips this first and reports a live peer as
  // "peer timed out" / "peer left".
  static constexpr uint32_t PEER_TIMEOUT_MS = 40000;
  // Per-turn deadline to pick a move. Auto-attacks for whoever misses it, so
  // a slow or absent player can't stall the match indefinitely.
  static constexpr uint32_t MOVE_TIMEOUT_MS = 30000;
  static constexpr uint32_t BEACON_MS       = 2000;
  static constexpr uint8_t  SEEN_SLOTS      = 4;

  Session(Transport& t, Clock& c, SessionUi& u) : tx_(t), clk_(c), ui_(u) {}

  // id must be non-zero (0 is the broadcast address). seed is this device's
  // half of the battle seed and must come from a real entropy source.
  void begin(uint32_t id, uint32_t seed);
  void rematch(uint32_t seed);         // back to LS_IDLE, keeps the same id

  void startHosting();
  void startJoining();
  void onKey(char c);
  void poll();                         // call as often as you like

  LinkState          state()    const { return state_; }
  const BattleState& battle()   const { return b_; }
  bool               isHost()   const { return isHost_; }
  uint32_t           myId()     const { return myId_; }
  uint32_t           peerId()   const { return peerId_; }
  const char*        overMsg()  const { return overMsg_; }
  bool               awaitingAck() const { return pendingActive_; }
  uint8_t            tries()    const { return pendingTries_; }

  // Test hook only. Retry jitter is what stops two peers colliding forever in
  // lockstep; the harness disables it to prove the test can actually fail.
  void setJitter(bool on) { jitter_ = on; }

 private:
  void     resetMatchState();          // single source of truth for a fresh match
  void     txPacket(Packet& p, bool wantAck, uint8_t protoAck = 0);
  uint32_t nextRetryDelay();
  void     sendAck(uint16_t seq, uint32_t to);
  void     sendAction(ActionId a);
  void     sendReady();
  void     sendBye(ByeReason r);
  void     endMatch(const char* why, ByeReason reason, bool tellPeer);
  bool     seenContains(uint32_t src, uint16_t seq) const;
  void     seenRecord(uint32_t src, uint16_t seq);
  void     handlePacket(const Packet& p);
  void     pumpRx();
  void     pumpRetries();
  void     pumpWatchdog();
  void     pumpBeacon();
  void     pumpResolve();
  void     pumpMoveTimer();
  void     resolveTurn();              // both actions in hand; advance the sim
  void     enterMyTurn();              // LS_MY_TURN entry, every path
  void     chooseAction(ActionId a);   // shared by onKey and the move timer

  Transport&  tx_;
  Clock&      clk_;
  SessionUi&  ui_;

  LinkState state_ = LS_IDLE;
  uint32_t  myId_ = 0, peerId_ = 0;
  bool      isHost_ = false;
  uint32_t  mySeed_ = 0, peerSeed_ = 0;
  uint8_t   peerCommit_[8] = {0};      // from the beacon, checked at reveal
  uint16_t  txSeq_ = 1;

  // Replay suppression keyed by (src, seq). One slot is not enough once a
  // retransmit interleaves with a fresh packet.
  SeenEntry seen_[SEEN_SLOTS] = {};
  uint8_t   seenAt_ = 0;

  ActionId  myAction_ = ACT_NONE, peerAction_ = ACT_NONE;
  bool      peerActionIn_ = false;

  Packet    pending_{};                // awaiting acknowledgement
  bool      pendingActive_ = false;
  uint8_t   pendingProto_ = 0;         // if set, ONLY this packet type clears
                                       // the slot; a transport ACK does not
  uint32_t  pendingSentAt_ = 0;
  uint32_t  pendingDelay_ = 0;
  uint8_t   pendingTries_ = 0;

  uint32_t  lastRxAt_ = 0;             // last valid packet from the peer
  uint32_t  lastBeacon_ = 0;
  uint32_t  turnStartAt_ = 0;          // when we entered LS_MY_TURN, for MOVE_TIMEOUT_MS
  Rng       netRng_;                   // retry jitter ONLY, never the sim's rng
  bool      jitter_ = true;
  const char* overMsg_ = "";
  bool      versionMismatchShown_ = false;  // one status line per pairing attempt

  BattleState b_{};
};
