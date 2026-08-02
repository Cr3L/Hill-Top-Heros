#include "rpg_session.h"
#include <string.h>

const char* linkStateName(LinkState s) {
  switch (s) {
    case LS_IDLE:      return "IDLE";
    case LS_BEACONING: return "BEACONING";
    case LS_JOINING:   return "JOINING";
    case LS_HANDSHAKE: return "HANDSHAKE";
    case LS_MY_TURN:   return "MY_TURN";
    case LS_WAIT_PEER: return "WAIT_PEER";
    case LS_LINGER:    return "LINGER";
    case LS_OVER:      return "OVER";
  }
  return "?";
}

// Single source of truth for "what does my own seat's outcome string say",
// used to build both PKT_STATUS's wire enum and endMatch's message.
static const char* outcomeMsg(StatusOutcome o) {
  switch (o) {
    case STATUS_DRAW:   return "draw";
    case STATUS_I_WIN:  return "you win";
    case STATUS_I_LOSE: return "you lose";
    default:             return "";
  }
}

// ------------------------------------------------------------------ lifecycle

// Everything that belongs to one match, in one place. The header's member
// initialisers and this function would otherwise be two lists of the same
// fields drifting apart on every edit, so begin() calls this and rematch() is
// just begin() again.
void Session::resetMatchState() {
  peerId_   = 0;
  isHost_   = false;
  peerSeed_ = 0;
  peerClassId_ = 0;
  memset(peerCommit_, 0, sizeof(peerCommit_));
  txSeq_    = 1;
  memset(seen_, 0, sizeof(seen_));
  seenAt_   = 0;
  myAction_ = peerAction_ = ACT_NONE;
  peerActionIn_  = false;
  pendingActive_ = false;
  pendingProto_  = 0;
  pendingTries_  = 0;
  lastRxAt_      = 0;
  lastBeacon_    = 0;
  turnStartAt_   = 0;
  overMsg_       = "";
  versionMismatchShown_ = false;
  battleStarted_ = false;
  lingerReason_  = BYE_NONE;
  lingerStartAt_ = 0;
  lingerProbeAt_ = 0;
  memset(&b_, 0, sizeof(b_));
}

void Session::begin(uint32_t id, uint32_t seed) {
  resetMatchState();
  myId_   = id ? id : 1;               // 0 is reserved for broadcast
  mySeed_ = seed;
  netRng_.seed(myId_);                 // distinct jitter stream per device
  state_  = LS_IDLE;
  ui_.status("H=host  J=join");
}

void Session::rematch(uint32_t seed) {
  begin(myId_, seed);
}

void Session::rematchKeepingRole(uint32_t seed) {
  bool wasHost = isHost_;
  uint8_t classId = myClassId_;
  rematch(seed);
  if (wasHost) startHosting(classId);
  else         startJoining(classId);
}

void Session::startHosting(uint8_t classId) {
  state_    = LS_BEACONING;
  isHost_   = true;
  myClassId_ = classId;
  lastRxAt_ = clk_.now();
  ui_.status("hosting...");
}

void Session::startJoining(uint8_t classId) {
  state_    = LS_JOINING;
  isHost_   = false;
  myClassId_ = classId;
  lastRxAt_ = clk_.now();
  ui_.status("searching...");
}

// --------------------------------------------------------------------- send

uint32_t Session::nextRetryDelay() {
  // Both peers submit their turn at once and would otherwise collide, retry in
  // lockstep, and collide again forever. Jitter is seeded per-device so the two
  // schedules diverge.
  if (!jitter_) return RETRY_MS;
  return RETRY_MS + netRng_.range(0, RETRY_JITTER_MS);
}

void Session::txPacket(Packet& p, bool wantAck, uint8_t protoAck) {
  p.src = myId_;
  p.seq = txSeq_++;
  if (p.seq == 0) p.seq = txSeq_++;    // seq 0 means "none" in ackSeq
  packetSeal(p);
  tx_.send(p);
  if (wantAck) {
    pending_       = p;
    pendingActive_ = true;
    pendingProto_  = protoAck;
    pendingSentAt_ = clk_.now();
    pendingDelay_  = nextRetryDelay();
    pendingTries_  = 1;
  }
}

void Session::sendAck(uint16_t seq, uint32_t to) {
  Packet a{};
  a.type   = PKT_ACK;
  a.dst    = to;
  a.ackSeq = seq;
  txPacket(a, false);
}

void Session::sendAction(ActionId a) {
  Packet p{};
  p.type      = PKT_ACTION;
  p.dst       = peerId_;
  p.turn      = b_.turn;
  p.action    = (uint8_t)a;
  p.stateHash = battleHash(b_);
  txPacket(p, true);
}

// Deliberately unacknowledged. There is only one pending retry slot, and the
// joiner needs it for the battle turn it is about to submit — arming READY here
// would just be clobbered by that action. The host retransmits JOIN_ACK until
// it hears READY, so the host's retry timer is what recovers a lost READY.
void Session::sendReady() {
  Packet r{};
  r.type = PKT_READY;
  r.dst  = peerId_;
  txPacket(r, false);
}

void Session::sendBye(ByeReason r) {
  Packet p{};
  p.type   = PKT_BYE;
  p.dst    = peerId_;
  p.action = (uint8_t)r;
  // Retried like anything else. A dropped BYE is the difference between the
  // peer learning the result and the peer grinding out its retry budget and
  // reporting "peer unreachable" for a match that was actually decided.
  // pumpRetries keeps running in LS_OVER, so this still gets resent.
  txPacket(p, true);
}

void Session::endMatch(const char* why, ByeReason reason, bool tellPeer) {
  if (state_ == LS_OVER) return;       // first verdict wins; never overwrite it
  pendingActive_ = false;              // abandon whatever we were retrying
  state_   = LS_OVER;
  overMsg_ = why;
  if (tellPeer && peerId_) sendBye(reason);   // arms its own retry slot
  ui_.log(why);
  ui_.status(why);
}

// Like endMatch, but doesn't finalize yet — parks the outcome and keeps
// answering/asking PKT_STATUS for up to LINGER_MS, so a peer that only
// missed our BYE (or whose BYE we only missed) can still be reconciled with
// instead of the two of us disagreeing forever. pumpLinger() is what
// eventually calls the real endMatch(), either on reconciliation or once the
// window elapses.
void Session::enterLinger(const char* why, ByeReason reason, bool tellPeer) {
  if (state_ == LS_OVER) return;
  // Once we've recorded a REAL decided verdict (reason == BYE_MATCH_OVER),
  // a later call must not downgrade it back to a guess — but the reverse
  // must be allowed: the PKT_BYE self-resolve path calls resolveTurn() (and
  // so this, with BYE_MATCH_OVER) while already LS_LINGER on a guess like
  // "peer unreachable", and that real verdict has to win. Without this, the
  // stale guess would silently finalize instead — the exact split-verdict
  // case this feature exists to fix.
  if (state_ == LS_LINGER && lingerReason_ == BYE_MATCH_OVER) return;
  pendingActive_ = false;
  state_         = LS_LINGER;
  overMsg_       = why;
  lingerReason_  = reason;
  lingerStartAt_ = clk_.now();
  lingerProbeAt_ = 0;                  // fire the first probe on the next poll
  if (tellPeer && peerId_) sendBye(reason);
  ui_.log(why);
  ui_.status(why);
}

// --------------------------------------------------------------------- recv

// Lookup and insert are separate so a packet can be recognised as already
// handled without an ignored packet ever entering the ring.
bool Session::seenContains(uint32_t src, uint16_t seq) const {
  for (const auto& e : seen_)
    if (e.src == src && e.seq == seq) return true;
  return false;
}

void Session::seenRecord(uint32_t src, uint16_t seq) {
  seen_[seenAt_] = { src, seq };
  seenAt_ = (uint8_t)((seenAt_ + 1) % SEEN_SLOTS);
}

void Session::handlePacket(const Packet& p) {
  if (p.dst != 0 && p.dst != myId_) return;             // not for us
  if (peerId_ && p.src != peerId_) return;              // paired: peer only
  if (p.src == myId_) return;                           // our own echo

  if (peerId_) lastRxAt_ = clk_.now();

  // Transport-level ACK. Only clears the pending slot if that send was not
  // waiting on a specific protocol reply (see pendingProto_).
  if (p.type == PKT_ACK) {
    if (pendingActive_ && !pendingProto_ && p.ackSeq == pending_.seq)
      pendingActive_ = false;
    return;
  }

  // A protocol reply is a stronger acknowledgement than a bare PKT_ACK.
  if (pendingActive_ && pendingProto_ == p.type) pendingActive_ = false;

  // NEVER acknowledge a packet we are about to ignore. The sender treats an
  // ACK as proof of delivery and drops it from its retry slot, so ACKing
  // something we discard strands the sender waiting on a reply that is never
  // coming. Beacons are broadcast and unacknowledged; the handshake steps are
  // idempotent and safe to ACK on sight (a duplicate READY must still be ACKed
  // or the joiner retries it to exhaustion); PKT_ACTION acknowledges itself
  // inside its case, only once it has actually been accepted.
  if (p.type != PKT_BEACON && p.type != PKT_ACTION) sendAck(p.seq, p.src);

  switch (p.type) {
    case PKT_BEACON:
      if (state_ == LS_JOINING && !peerId_) {
        peerId_ = p.src;
        isHost_ = false;
        memcpy(peerCommit_, p.commit, sizeof(peerCommit_));
        Packet j{};
        j.type = PKT_JOIN_REQ; j.dst = peerId_; j.seedHalf = mySeed_;
        j.classId = myClassId_;
        txPacket(j, true, PKT_JOIN_ACK);
        state_ = LS_HANDSHAKE;
        ui_.status("joining...");
      }
      break;

    case PKT_JOIN_REQ:
      // Also accepted while already in LS_HANDSHAKE: that means our JOIN_ACK
      // was lost and the joiner is asking again. Re-send it.
      if (state_ == LS_BEACONING ||
          (state_ == LS_HANDSHAKE && isHost_ && p.src == peerId_)) {
        peerId_   = p.src;
        isHost_   = true;
        peerSeed_ = p.seedHalf;
        peerClassId_ = p.classId;
        Packet a{};
        a.type = PKT_JOIN_ACK; a.dst = peerId_; a.seedHalf = mySeed_;
        a.classId = myClassId_;
        txPacket(a, true, PKT_READY);  // wait for READY, not for a bare ACK
        state_ = LS_HANDSHAKE;
        ui_.status("challenger!");
      }
      break;

    case PKT_JOIN_ACK:
      if (state_ == LS_HANDSHAKE && !isHost_) {
        if (!seedCommitMatches(p.seedHalf, peerCommit_)) {
          endMatch("bad seed commit", BYE_BAD_COMMIT, true);
          break;
        }
        peerSeed_ = p.seedHalf;
        peerClassId_ = p.classId;
        battleInit(b_, mySeed_ ^ peerSeed_, hostClassId(), joinerClassId());
        battleStarted_ = true;
        sendReady();
        enterMyTurn();
      } else if (!isHost_ && b_.turn == 0 &&
                 (state_ == LS_MY_TURN || state_ == LS_WAIT_PEER)) {
        // Our READY was lost and the host is still asking. Say it again.
        // LS_WAIT_PEER matters as much as LS_MY_TURN: we enter the battle the
        // instant we send READY and may well have submitted a turn already, so
        // by the time the host re-asks we are usually no longer on the menu.
        sendReady();
      }
      break;

    case PKT_READY:
      if (state_ == LS_HANDSHAKE && isHost_) {
        battleInit(b_, mySeed_ ^ peerSeed_, hostClassId(), joinerClassId());
        battleStarted_ = true;
        enterMyTurn();
      }
      break;

    case PKT_ACTION: {
      // Already processed. A lost ACK is precisely why this was resent, so
      // acknowledge it again — but do not apply it twice. This runs before the
      // state gate below and deliberately still fires in LS_OVER: the peer that
      // has not finished yet is owed its acknowledgement, and going quiet on it
      // strands it until its retries run out.
      if (seenContains(p.src, p.seq)) { sendAck(p.seq, p.src); break; }

      // Not in a battle yet. The joiner enters LS_MY_TURN the moment it sends
      // READY, so if that READY was lost its first action arrives here while we
      // are still shaking hands with an all-zero BattleState — which used to be
      // read as a desync. Ignore it WITHOUT acking, and without recording it,
      // so the sender keeps retrying until we can genuinely accept it.
      if (state_ != LS_MY_TURN && state_ != LS_WAIT_PEER) break;

      if (p.turn != b_.turn) break;                     // stale or ahead
      // Checked from turn 0 on: a mismatch there means the seeds disagree,
      // which is worth catching before a single blow lands.
      if (p.stateHash != battleHash(b_)) {
        endMatch("DESYNC - match void", BYE_DESYNC, true);
        break;
      }
      if (!actionValid(p.action)) break;                // corrupt but CRC-clean

      peerAction_   = (ActionId)p.action;
      peerActionIn_ = true;
      seenRecord(p.src, p.seq);
      sendAck(p.seq, p.src);                            // accepted; confirm it
      break;
    }

    case PKT_BYE:
      // Already decided — whether finalized (LS_OVER) or still lingering on
      // our own real verdict (LS_LINGER with a resolved b_) — the peer's BYE
      // is just closing down too and tells us nothing new.
      if (state_ == LS_OVER ||
          (state_ == LS_LINGER && myOutcome() != STATUS_UNKNOWN))
        break;

      // The peer finished the final turn. If we are holding both halves of
      // that same turn — whether still LS_WAIT_PEER or already lingering on
      // "peer unreachable"/"peer timed out" without having cleared our own
      // action — we can resolve it ourselves and report the real result: the
      // sim is deterministic, so we necessarily reach the same verdict. The
      // only thing we were missing was the acknowledgement of our own
      // action, and the BYE is proof the peer consumed it.
      if (p.action == BYE_MATCH_OVER &&
          (state_ == LS_WAIT_PEER || state_ == LS_LINGER) &&
          peerActionIn_ && myAction_ != ACT_NONE) {
        pendingActive_ = false;        // it was received; stop retrying
        resolveTurn();
        if (state_ != LS_OVER && state_ != LS_LINGER)  // somehow not decided
          endMatch("peer left", BYE_NONE, false);
        break;
      }

      // Can't self-resolve, and haven't decided ourselves. If we're merely
      // lingering on a still-open guess, a bare BYE isn't enough to act on —
      // it doesn't carry the peer's actual verdict — so don't clobber that
      // guess with "peer left" here. Let the PKT_STATUS exchange (which does
      // carry it) or the linger deadline settle it instead.
      if (state_ == LS_LINGER) break;

      endMatch(p.action == BYE_DESYNC ? "peer saw desync" : "peer left",
               BYE_NONE, false);
      break;

    case PKT_STATUS: {
      // LS_OVER is included so a side that already finalized keeps
      // answering queries from a peer still reconciling in its own LINGER
      // window — b_ and myOutcome() are still valid there, only rematch()
      // clears them, and a stale, unanswered query is exactly the situation
      // this feature exists to avoid.
      if (state_ != LS_MY_TURN && state_ != LS_WAIT_PEER &&
          state_ != LS_LINGER && state_ != LS_OVER)
        break;

      if (p.action == STATUS_QUERY) {
        if (!peerId_) break;
        Packet r{};
        r.type      = PKT_STATUS;
        r.dst       = p.src;
        r.turn      = b_.turn;
        r.stateHash = battleHash(b_);
        r.action    = (uint8_t)myOutcome();
        txPacket(r, false);
        break;
      }

      if (state_ != LS_LINGER) break;  // a reply is only actionable while lingering

      if (p.action == STATUS_I_WIN || p.action == STATUS_I_LOSE ||
          p.action == STATUS_DRAW) {
        // The peer has a real, decided verdict — adopt it instead of our
        // stale guess. Mapped from the peer's seat to ours: their win is our
        // loss.
        StatusOutcome mine = p.action == STATUS_DRAW  ? STATUS_DRAW
                            : p.action == STATUS_I_WIN ? STATUS_I_LOSE
                                                        : STATUS_I_WIN;
        endMatch(outcomeMsg(mine), BYE_NONE, false);
        break;
      }

      // STATUS_UNKNOWN: the peer hasn't reached a verdict either — the rare
      // both-exhausted-before-resolving case. Deliberately not handled here:
      // an earlier version resent the action and rejoined the ordinary FSM,
      // but that resend arms its own fresh retry/watchdog cycle, and this
      // very packet type keeps lastRxAt_ current — so on a persistently bad
      // link (corrupted/one-way-blocked action) the two sides can cycle
      // LS_WAIT_PEER <-> LS_LINGER indefinitely without either timeout ever
      // firing, a genuine livelock LINGER_MS was supposed to bound, not
      // create. Left as a no-op: this case falls back to today's behavior
      // (each side finalizes on its own local guess once LINGER_MS elapses),
      // which is correct, just not improved. Closing it needs the resend
      // capped to a single attempt or an independent deadline, tracked as a
      // follow-up rather than risking it here.
      break;
    }

    default: break;
  }
}

// --------------------------------------------------------------------- pumps

void Session::pumpRx() {
  Packet p{};
  while (tx_.recv(p)) {
    if (packetValid(p)) {
      handlePacket(p);
    } else if (!versionMismatchShown_ && packetVersionMismatch(p) &&
               (state_ == LS_IDLE || state_ == LS_BEACONING ||
                state_ == LS_JOINING || state_ == LS_HANDSHAKE)) {
      // Only before/during pairing: this is the "why can't I find my peer"
      // moment version skew actually blocks. Mid-match it would just be
      // stray traffic, not something to interrupt a live battle over. Once
      // per attempt, not once per packet — a beaconing mismatch would
      // otherwise repeat every BEACON_MS forever.
      versionMismatchShown_ = true;
      ui_.status("peer is on a different version");
    }
    if (state_ == LS_OVER) return;
  }
}

void Session::pumpRetries() {
  if (!pendingActive_) return;
  if (clk_.now() - pendingSentAt_ < pendingDelay_) return;
  if (pendingTries_ >= MAX_TRIES) {
    pendingActive_ = false;
    // In LS_OVER or LS_LINGER this is just a closing BYE going unanswered —
    // the verdict is already decided (or being reconciled) and must not be
    // relabelled "peer unreachable". enterLinger is itself a no-op in both
    // of those states, so no need to check here too.
    enterLinger("peer unreachable", BYE_TIMEOUT, false);
    return;
  }
  // Re-send the same seq so the peer's replay filter recognises it.
  tx_.send(pending_);
  pendingSentAt_ = clk_.now();
  pendingDelay_  = nextRetryDelay();
  pendingTries_++;
}

void Session::pumpWatchdog() {
  // Only for states where the peer owes us something. LS_MY_TURN is excluded:
  // a human staring at the menu for 20s is not a dead link.
  if (state_ != LS_HANDSHAKE && state_ != LS_WAIT_PEER) return;
  if (clk_.now() - lastRxAt_ < PEER_TIMEOUT_MS) return;
  enterLinger("peer timed out", BYE_TIMEOUT, true);
}

void Session::pumpLinger() {
  if (state_ != LS_LINGER) return;
  if (clk_.now() - lingerStartAt_ >= LINGER_MS) {
    endMatch(overMsg_, lingerReason_, false);
    return;
  }
  if (!peerId_) return;
  if (lingerProbeAt_ && clk_.now() - lingerProbeAt_ < LINGER_PROBE_MS) return;
  lingerProbeAt_ = clk_.now();
  Packet p{};
  p.type      = PKT_STATUS;
  p.dst       = peerId_;
  p.turn      = b_.turn;
  p.action    = STATUS_QUERY;
  p.stateHash = battleHash(b_);
  txPacket(p, false);
}

void Session::pumpBeacon() {
  if (state_ != LS_BEACONING) return;
  if (lastBeacon_ && clk_.now() - lastBeacon_ < BEACON_MS) return;
  lastBeacon_ = clk_.now();
  Packet b{};
  b.type = PKT_BEACON; b.dst = 0;
  seedCommit(mySeed_, b.commit);       // commitment only, never the half itself
  txPacket(b, false);
}

void Session::resolveTurn() {
  ActionId a0 = isHost_ ? myAction_   : peerAction_;
  ActionId a1 = isHost_ ? peerAction_ : myAction_;

  battleResolve(b_, a0, a1);
  peerActionIn_ = false;
  myAction_ = peerAction_ = ACT_NONE;

  StatusOutcome outcome = myOutcome();
  if (outcome == STATUS_UNKNOWN) {
    enterMyTurn();
  } else {
    // Tell the peer. It is holding the same pair of actions and may still be
    // waiting on an acknowledgement we already sent; without this it grinds
    // through its whole retry budget and reports "peer unreachable" instead of
    // the result of a match it actually finished.
    enterLinger(outcomeMsg(outcome), BYE_MATCH_OVER, true);
  }
}

// battleStarted_ guards a zeroed BattleState (handshake failed before
// battleInit ever ran) reading as a false draw — see rpg_session.h.
StatusOutcome Session::myOutcome() const {
  if (!battleStarted_) return STATUS_UNKNOWN;
  int w = battleWinner(b_);
  if (w == -1) return STATUS_UNKNOWN;
  int me = isHost_ ? 0 : 1;
  return w == 2 ? STATUS_DRAW : (w == me ? STATUS_I_WIN : STATUS_I_LOSE);
}

void Session::pumpResolve() {
  if (state_ != LS_WAIT_PEER || !peerActionIn_ || pendingActive_) return;
  resolveTurn();
}

void Session::enterMyTurn() {
  state_ = LS_MY_TURN;
  turnStartAt_ = clk_.now();
  ui_.battle();
}

void Session::chooseAction(ActionId a) {
  myAction_ = a;
  sendAction(a);
  state_ = LS_WAIT_PEER;
  ui_.battle();
}

void Session::pumpMoveTimer() {
  if (state_ != LS_MY_TURN) return;
  if (clk_.now() - turnStartAt_ < MOVE_TIMEOUT_MS) return;
  chooseAction(ACT_ATTACK);
}

void Session::poll() {
  pumpRx();
  pumpRetries();
  pumpWatchdog();
  pumpBeacon();
  pumpResolve();
  pumpMoveTimer();
  pumpLinger();
}

// ----------------------------------------------------------------- keyboard

void Session::onKey(char c) {
  switch (state_) {
    // LS_IDLE has no key handling here: hosting/joining now need a
    // player-chosen classId, which this single-char interface can't carry.
    // main.cpp's class-pick gate calls startHosting()/startJoining()
    // directly once a class is confirmed, without going through onKey().
    case LS_MY_TURN: {
      ActionId a = ACT_NONE;
      if (c == '1') a = ACT_ATTACK;
      if (c == '2') a = ACT_GUARD;
      if (c == '3') a = ACT_SKILL;
      if (c == '4') a = ACT_ITEM;
      if (c == '5') a = ACT_FLEE;
      if (a != ACT_NONE) chooseAction(a);
      break;
    }

    default: break;
  }
}
