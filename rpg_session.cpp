#include "rpg_session.h"
#include <string.h>

const char* linkStateName(LinkState s) {
  switch (s) {
    case LS_IDLE:      return "IDLE";
    case LS_SCANNING:  return "SCANNING";
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

// ----------------------------------------------------------------- identity

// Reads a name off the wire, and the only place that does — everything that
// stores a peer name goes through here, so this is where a received name gets
// made safe to hold and to draw.
//
// Nothing about an incoming packet is trustworthy: the link is unauthenticated
// broadcast, so any radio in range can put arbitrary bytes in this field with a
// valid CRC. Two things are therefore forced rather than assumed:
//
//   - The terminator. The field is 7 raw bytes and a hostile sender can fill
//     every one of them. Costs the 7th character, which no honest sender uses
//     — setName() caps what goes out at PLAYER_NAME_MAX.
//   - Printability. Same range the local name entry in main.cpp accepts, so a
//     name off the air can't render as something a typed one couldn't be.
//     Control bytes would otherwise reach the panel as garbage glyphs.
//
// Stops at the first NUL, so a name is a C string in the ordinary sense and
// bytes hidden past a terminator are dropped rather than stored.
static void copyWireName(char (&dst)[PLAYER_NAME_MAX + 1], const char* wire) {
  memset(dst, 0, sizeof(dst));
  for (size_t i = 0; i < PLAYER_NAME_MAX && wire[i]; i++)
    dst[i] = (wire[i] >= ' ' && wire[i] <= '~') ? wire[i] : '?';
}

// Zero-filled first, not just terminated: txPacket() copies the whole field
// onto the wire, so a shorter name replacing a longer one would otherwise
// transmit the old tail past the terminator. Harmless to display, but it puts
// a stale name on the air and gives two identical names different CRCs.
void Session::setName(const char* name) {
  memset(myName_, 0, sizeof(myName_));
  if (name) memcpy(myName_, name, strnlen(name, PLAYER_NAME_MAX));
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
  // myName_ is deliberately absent: like myClassId_ it belongs to the device,
  // not the match, so it survives rematch() and a player names themself once.
  memset(peerName_, 0, sizeof(peerName_));
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
  beaconDelay_   = BEACON_MS;
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
  ui_.status("P=practice  O=open");
}

void Session::rematch(uint32_t seed) {
  begin(myId_, seed);
}

void Session::rematchAndReopen(uint32_t seed) {
  uint8_t classId = myClassId_;
  rematch(seed);
  startScanning(classId);
}

void Session::startScanning(uint8_t classId) {
  state_    = LS_SCANNING;
  myClassId_ = classId;
  sightingCount_ = 0;
  // Re-armed per attempt, which is what "once per pairing attempt" has to mean
  // if opening the list is the attempt. resetMatchState() alone is not enough:
  // cancelScan() drops to LS_IDLE without going through it, so a player who
  // opened, backed out and opened again would never be told a second time.
  versionMismatchShown_ = false;
  lastRxAt_ = clk_.now();
  ui_.status("open...");
}

void Session::cancelScan() {
  if (state_ != LS_SCANNING) return;
  state_ = LS_IDLE;
  sightingCount_ = 0;
  ui_.status("P=practice  O=open");
}

void Session::joinSighting(uint32_t hostId) {
  if (state_ != LS_SCANNING) return;   // e.g. a challenge landed first
  const Sighting* sg = nullptr;
  for (size_t i = 0; i < sightingCount_; i++)
    if (sightings_[i].hostId == hostId) { sg = &sightings_[i]; break; }
  if (!sg) return;   // expired between selection and confirmation

  peerId_    = sg->hostId;
  isHost_    = false;
  memcpy(peerCommit_, sg->commit, sizeof(peerCommit_));
  // Already known from the beacon that produced this sighting, so the peer has
  // a name here before its JOIN_ACK arrives — which is what lets the
  // "challenging..." screen name who is being challenged.
  copyWireName(peerName_, sg->name);
  sightingCount_ = 0;
  // The handshake watchdog measures silence from the peer, and it starts
  // counting now — not from whenever this device went open. A player can sit
  // reading the list for longer than PEER_TIMEOUT_MS (nothing hurries them),
  // and without this the very first poll after challenging would declare a
  // perfectly healthy peer "timed out".
  lastRxAt_ = clk_.now();

  Packet j{};
  j.type = PKT_JOIN_REQ; j.dst = peerId_; j.seedHalf = mySeed_;
  j.classId = myClassId_;
  txPacket(j, true, PKT_JOIN_ACK);
  state_ = LS_HANDSHAKE;
  ui_.status("challenging...");
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
  // Stamped here alongside src for the same reason — it is who we are, not
  // part of any one message — and, more importantly, so the list of packet
  // types allowed to write the name arm of Packet's union exists once, in
  // code, instead of at each construction site under a comment. A new
  // handshake packet that should carry a name is added here.
  if (p.type == PKT_BEACON || p.type == PKT_JOIN_REQ || p.type == PKT_JOIN_ACK)
    memcpy(p.name, myName_, sizeof(p.name));
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

void Session::handlePacket(const Packet& p, int16_t rssi) {
  if (p.dst != 0 && p.dst != myId_) return;             // not for us
  if (peerId_ && p.src != peerId_) return;              // paired: peer only
  if (p.src == myId_) return;                           // our own echo

  // While open, another open device's beacon is presence, not an invitation:
  // record it and stop. Never auto-joins — the player picks from the list.
  // Everything else (notably an incoming PKT_JOIN_REQ, i.e. someone else
  // picking US off their list) falls through and is handled normally.
  if (state_ == LS_SCANNING && p.type == PKT_BEACON) {
    recordSighting(p.src, p.commit, p.name, rssi);
    return;
  }

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
    // Beacons outside LS_SCANNING are just other open devices we aren't
    // looking at right now — recorded above while open, ignored otherwise.
    case PKT_BEACON:
      break;

    case PKT_JOIN_REQ:
      // Accepted while open — someone picked us off their list. Also accepted
      // while already in LS_HANDSHAKE and hosting: that means our JOIN_ACK was
      // lost and the challenger is asking again. Re-send it.
      //
      // The third case only exists because presence is symmetric now: two
      // open peers can pick each other in the same instant, and their
      // JOIN_REQs cross on the wire. Both are then challengers
      // (isHost_ == false) waiting on a JOIN_ACK neither will ever send, and
      // both grind to "peer unreachable". Broken by a rule both sides
      // evaluate identically on the same two ids: the higher id yields and
      // becomes the host, so exactly one of them switches seats and the
      // handshake completes. Ids are device-unique (deviceId() in main.cpp),
      // and p.src == myId_ was already rejected at the top of handlePacket.
      if (state_ == LS_SCANNING ||
          (state_ == LS_HANDSHAKE && isHost_ && p.src == peerId_) ||
          (state_ == LS_HANDSHAKE && !isHost_ && p.src == peerId_ &&
           myId_ > p.src)) {
        peerId_   = p.src;
        isHost_   = true;
        peerSeed_ = p.seedHalf;
        peerClassId_ = p.classId;
        copyWireName(peerName_, p.name);
        sightingCount_ = 0;            // no longer relevant once paired
        lastRxAt_ = clk_.now();        // watchdog starts here, not at open

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
        // Overwrites what joinSighting() copied from the beacon: same peer,
        // but this is the fresher of the two and the only one an accepting
        // peer ever gets.
        copyWireName(peerName_, p.name);
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
  int16_t rssi = 0;
  while (tx_.recv(p, &rssi)) {
    if (packetValid(p)) {
      handlePacket(p, rssi);
    } else if (!versionMismatchShown_ && packetVersionMismatch(p) &&
               (state_ == LS_SCANNING || state_ == LS_HANDSHAKE)) {
      // Only while actually looking for a peer: this is the "why can't I find
      // my peer" moment version skew actually blocks. Mid-match it would just
      // be stray traffic, not something to interrupt a live battle over. Once
      // per attempt, not once per packet — a beaconing mismatch would
      // otherwise repeat every BEACON_MS forever.
      //
      // LS_IDLE is deliberately absent, and that is load-bearing rather than an
      // oversight: the idle screen is a fixed menu that has no line to render a
      // status string into, so reporting from there burned this one-shot latch
      // on a message nobody saw. A unit sitting on the menu within earshot of a
      // mismatched beacon would then open and find an empty list, permanently
      // and silently — the exact symptom this feature exists to remove. The
      // latch is cleared by resetMatchState(), so an idle unit is still armed
      // for the first mismatch it hears after opening.
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
  // Open == beaconing. An open device advertises itself the whole time it is
  // also listening, so anyone else who opens the game can see it without
  // either side having chosen a role.
  if (state_ != LS_SCANNING) return;
  // Jittered, and it is load-bearing here in a way it was not when only one
  // side beaconed: two symmetric open devices on a fixed period transmit at
  // the same instant, destroy each other in a collision, wait the same
  // BEACON_MS, and collide again — forever, neither ever seeing the other.
  // netRng_ is seeded per-device (begin()) so the two schedules diverge.
  // Never the battle rng: that would desync the match (see CLAUDE.md).
  // Deliberately NOT gated on jitter_: that hook exists to prove the retry
  // backoff can be made constant, and switching beacons to lockstep along
  // with it would just stop two open peers from ever finding each other.
  if (lastBeacon_ && clk_.now() - lastBeacon_ < beaconDelay_) return;
  lastBeacon_  = clk_.now();
  beaconDelay_ = BEACON_MS + netRng_.range(0, BEACON_JITTER_MS);
  Packet b{};
  b.type = PKT_BEACON; b.dst = 0;
  seedCommit(mySeed_, b.commit);       // commitment only, never the half itself
  txPacket(b, false);
}

void Session::recordSighting(uint32_t hostId, const uint8_t* commit,
                             const char* name, int16_t rssi) {
  for (size_t i = 0; i < sightingCount_; i++) {
    if (sightings_[i].hostId == hostId) {
      sightings_[i].rssi       = rssi;
      sightings_[i].lastSeenAt = clk_.now();
      memcpy(sightings_[i].commit, commit, sizeof(sightings_[i].commit));
      // Refreshed rather than kept: a player who renames themself while open
      // should show up renamed, without having to drop out of the list first.
      // Unlike the RSSI refresh above this changes what's on screen, so it
      // bumps the version — but only when the name really moved, or an idle
      // list would repaint on every beacon.
      //
      // The comparison has to be sanitized-against-sanitized, which is what
      // the temporary is for. Comparing the stored name directly against the
      // wire bytes looks equivalent and isn't: copyWireName() rewrites
      // anything unprintable, so a peer whose name contains one such byte
      // would differ from its own stored copy on every single beacon and
      // repaint the list forever.
      char fresh[PLAYER_NAME_MAX + 1];
      copyWireName(fresh, name);
      if (strcmp(sightings_[i].name, fresh) != 0) {
        memcpy(sightings_[i].name, fresh, sizeof(fresh));
        sightingsVersion_++;
      }
      return;
    }
  }
  if (sightingCount_ >= SEEN_HOSTS) return;  // table full; drop until a slot expires
  Sighting& sg = sightings_[sightingCount_++];
  sg.hostId     = hostId;
  sg.rssi       = rssi;
  sg.lastSeenAt = clk_.now();
  memcpy(sg.commit, commit, sizeof(sg.commit));
  copyWireName(sg.name, name);
  sightingsVersion_++;
}

void Session::pumpScanExpiry() {
  if (state_ != LS_SCANNING) return;
  for (size_t i = 0; i < sightingCount_; ) {
    if (clk_.now() - sightings_[i].lastSeenAt > SCAN_EXPIRY_MS) {
      sightings_[i] = sightings_[--sightingCount_];
      sightingsVersion_++;
    } else {
      i++;
    }
  }
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
  pumpScanExpiry();
  pumpResolve();
  pumpMoveTimer();
  pumpLinger();
}

// ----------------------------------------------------------------- keyboard

void Session::onKey(char c) {
  switch (state_) {
    // LS_IDLE and LS_SCANNING have no key handling here: opening needs a
    // player-chosen classId and selecting a peer needs a host id, neither of
    // which this single-char interface can carry. main.cpp calls
    // startScanning()/joinSighting() directly instead.
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
