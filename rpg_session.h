// rpg_session.h — the duel session state machine.
//
// Deliberately free of hardware: no RadioLib, no M5, no millis(), no Serial.
// Everything the FSM needs from the outside arrives through the three
// interfaces below, so the exact same compiled logic runs on the Cardputer and
// against the simulated lossy channel in test/.
#pragma once
#include "rpg_link.h"

enum LinkState : uint8_t {
  // LS_SCANNING is "open": beaconing your own presence and listening for
  // others' at the same time. There is no separate host-only or join-only
  // state — either side of a pairing can be the one who sends the challenge.
  LS_IDLE, LS_SCANNING, LS_HANDSHAKE, LS_MY_TURN,
  LS_WAIT_PEER,
  // Terminal-pending: we have an outcome (a real verdict, or a giving-up
  // message) but are still willing to reconcile with the peer before
  // finalizing it. LS_OVER remains the true absorbing state — the only way
  // out of LS_LINGER is a successful reconciliation or LINGER_MS elapsing.
  LS_LINGER,
  LS_OVER
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
  // rssi, if non-null, is filled with the received frame's signal strength
  // (dBm) on a true reception. Purely informational — never validated or
  // used for anything but display, so a transport that can't measure it is
  // free to leave it untouched.
  virtual bool recv(Packet& out, int16_t* rssi = nullptr) = 0;
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

// One host seen while LS_SCANNING. Not a peer yet — no commitment to join
// until joinSighting() is called.
struct Sighting {
  uint32_t hostId     = 0;
  uint8_t  commit[8]  = {0};
  int16_t  rssi       = 0;
  uint32_t lastSeenAt = 0;
};

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
  // Two open devices beacon on the same period; without jitter they collide
  // in perfect lockstep and never discover each other. See pumpBeacon().
  static constexpr uint32_t BEACON_JITTER_MS = 700;
  // How long to keep answering/asking PKT_STATUS before giving up on the
  // locally-known outcome for good. Deliberately shorter than
  // PEER_TIMEOUT_MS: by the time LS_LINGER is entered, a full watchdog or
  // retry budget has already been spent failing to reach the peer, so the
  // case this recovers — a link that dropped only the last few packets —
  // resolves in one or two probe round-trips, not tens of seconds.
  static constexpr uint32_t LINGER_MS       = 15000;
  static constexpr uint32_t LINGER_PROBE_MS = 2000;
  static constexpr uint8_t  SEEN_SLOTS      = 4;
  // Fixed capacity for LS_SCANNING sightings. Plenty for a handheld's
  // realistic radio neighborhood; a full table just drops new arrivals until
  // a slot expires (see pumpScanExpiry()).
  static constexpr uint8_t  SEEN_HOSTS      = 6;
  // A host that stopped beaconing (paired up elsewhere, powered off, walked
  // out of range) should fall off the scan list rather than linger forever.
  static constexpr uint32_t SCAN_EXPIRY_MS  = BEACON_MS * 3;

  Session(Transport& t, Clock& c, SessionUi& u) : tx_(t), clk_(c), ui_(u) {}

  // id must be non-zero (0 is the broadcast address). seed is this device's
  // half of the battle seed and must come from a real entropy source.
  void begin(uint32_t id, uint32_t seed);
  void rematch(uint32_t seed);         // back to LS_IDLE, keeps the same id
  // Rematch, then immediately re-open with the same class rather than
  // waiting at the menu for a fresh pick — the fast path off LS_OVER's "r"
  // key. There's no "role" to preserve: everyone open is symmetric.
  void rematchAndReopen(uint32_t seed);

  // Open: beacons this device's own presence and listens for others', both
  // at once. classId is this device's player-chosen class (0..classCount()-1)
  // — set here rather than later because an incoming challenge can be
  // accepted at any time while open, and accepting one replies with it.
  void startScanning(uint8_t classId);
  void cancelScan();                   // back to LS_IDLE
  // Challenges the sighting with this hostId (if still present — it may have
  // expired since it was drawn on screen) and sends PKT_JOIN_REQ. Uses the
  // class already chosen at startScanning(). Looked up by id rather than
  // array index: pumpScanExpiry()'s swap-remove reorders sightings_, so an
  // index captured at draw time can point at a different player by the time
  // a key is pressed. A vanished hostId is a no-op — stays LS_SCANNING
  // rather than challenging whoever slid into that slot.
  void joinSighting(uint32_t hostId);
  size_t          sightingCount() const { return sightingCount_; }
  const Sighting& sighting(size_t i) const { return sightings_[i]; }
  // Bumped whenever a sighting is added or expires (not on an in-place RSSI
  // refresh) — lets a UI redraw only when the visible list actually changed,
  // instead of repainting an idle-heavy screen on a fixed timer.
  uint32_t        sightingsVersion() const { return sightingsVersion_; }

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
  void     enterLinger(const char* why, ByeReason reason, bool tellPeer);
  void     pumpLinger();
  bool     seenContains(uint32_t src, uint16_t seq) const;
  void     seenRecord(uint32_t src, uint16_t seq);
  void     handlePacket(const Packet& p, int16_t rssi = 0);
  void     pumpRx();
  void     pumpRetries();
  void     pumpWatchdog();
  void     pumpBeacon();
  void     pumpScanExpiry();
  void     recordSighting(uint32_t hostId, const uint8_t* commit, int16_t rssi);
  void     pumpResolve();
  void     pumpMoveTimer();
  // My own seat's outcome right now: STATUS_UNKNOWN until battleStarted_ and
  // the sim has actually decided. Single source of truth for both endMatch's
  // message and PKT_STATUS's wire reply — see outcomeMsg() in rpg_session.cpp.
  StatusOutcome myOutcome() const;
  // battleInit()'s host/joiner classId args, resolved from whichever of
  // myClassId_/peerClassId_ is actually sitting in that seat. Both
  // battleInit() call sites need the same pair, so this is one place
  // instead of the same two ternaries repeated at each.
  uint8_t  hostClassId()   const { return isHost_ ? myClassId_ : peerClassId_; }
  uint8_t  joinerClassId() const { return isHost_ ? peerClassId_ : myClassId_; }
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
  uint8_t   myClassId_ = 0, peerClassId_ = 0;  // player-chosen, rides the handshake
  uint8_t   peerCommit_[8] = {0};      // from the beacon, checked at reveal
  uint16_t  txSeq_ = 1;

  Sighting sightings_[SEEN_HOSTS] = {};
  size_t   sightingCount_ = 0;
  uint32_t sightingsVersion_ = 0;

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
  uint32_t  beaconDelay_ = 0;          // current jittered beacon interval
  uint32_t  turnStartAt_ = 0;          // when we entered LS_MY_TURN, for MOVE_TIMEOUT_MS
  Rng       netRng_;                   // retry jitter ONLY, never the sim's rng
  bool      jitter_ = true;
  const char* overMsg_ = "";
  bool      versionMismatchShown_ = false;  // one status line per pairing attempt
  // battleInit() has actually run. Guards PKT_STATUS's verdict reply: a
  // zeroed BattleState (handshake failed before battleInit) reads as a draw
  // to battleWinner() — both p[].alive are 0 — so without this a stuck
  // handshake could report a fake "draw" instead of "peer unreachable".
  bool      battleStarted_ = false;
  ByeReason lingerReason_  = BYE_NONE;      // reason to finalize with if LINGER_MS elapses
  uint32_t  lingerStartAt_ = 0;
  uint32_t  lingerProbeAt_ = 0;             // last PKT_STATUS probe sent, 0 = none yet

  BattleState b_{};
};
