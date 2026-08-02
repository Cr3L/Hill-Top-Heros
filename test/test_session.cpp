// Session FSM tests against the simulated lossy channel.
//
//   make -C test && test/run_session
//
// These cover the paths the on-hardware prototype cannot reach on demand: a
// lost handshake step, exhausted retries, colliding simultaneous turns, a
// tampered commitment, and a desync abort. See sim_channel.h for what the
// channel model does and does not represent.
#include "sim_channel.h"
#include "test_common.h"
#include <stdio.h>
#include <string.h>

static bool decided(const Session& s) {
  const char* m = s.overMsg();
  return !strcmp(m, "you win") || !strcmp(m, "you lose") || !strcmp(m, "draw");
}

// Both peers agreed on how the match ended.
static bool outcomesAgree(const Session& a, const Session& b) {
  if (!decided(a) || !decided(b)) return false;
  if (!strcmp(a.overMsg(), "draw"))    return !strcmp(b.overMsg(), "draw");
  if (!strcmp(a.overMsg(), "you win")) return !strcmp(b.overMsg(), "you lose");
  return !strcmp(b.overMsg(), "you win");
}

static bool sawDesync(const Rig& r) {
  return r.ui0.saw("DESYNC") || r.ui1.saw("DESYNC") ||
         r.ui0.saw("desync")  || r.ui1.saw("desync");
}

// Re-seal after mutating, so the frame is CRC-clean and the corruption has to
// be caught by protocol logic rather than the checksum.
static void reseal(Packet& p) { packetSeal(p); }

// ------------------------------------------------------------- clean channel

static void testCleanMatch() {
  printf("clean channel: full match\n");
  Rig r;
  r.begin(1);
  r.run(60000);
  CHECKM(r.bothDone(), "host=%s join=%s", linkStateName(r.host.state()),
         linkStateName(r.join.state()));
  CHECKM(outcomesAgree(r.host, r.join), "host said '%s', join said '%s'",
         r.host.overMsg(), r.join.overMsg());
  CHECK(!sawDesync(r));
  CHECK(r.host.battle().turn == r.join.battle().turn);
  CHECK(battleHash(r.host.battle()) == battleHash(r.join.battle()));
  printf("  ended turn %u: host '%s'\n", r.host.battle().turn, r.host.overMsg());
}

// ------------------------------------------------- single lost handshake step

// Each of these used to be, or could plausibly become, a deadlock. The match
// must still complete after losing exactly one handshake packet.
static void testLostHandshakeStep(const char* label, uint8_t type, int nth) {
  printf("lost %s: match still completes\n", label);
  Rig r;
  r.begin(2);
  r.ch.filter = dropNth(type, nth);
  r.run(90000);
  CHECKM(r.bothDone(), "%s: host=%s join=%s", label,
         linkStateName(r.host.state()), linkStateName(r.join.state()));
  CHECKM(outcomesAgree(r.host, r.join), "%s: host '%s' join '%s'", label,
         r.host.overMsg(), r.join.overMsg());
  CHECK(!sawDesync(r));
}

static void testLostHandshake() {
  testLostHandshakeStep("BEACON",   PKT_BEACON,   0);
  testLostHandshakeStep("JOIN_REQ", PKT_JOIN_REQ, 0);
  testLostHandshakeStep("JOIN_ACK", PKT_JOIN_ACK, 0);  // the original deadlock
  testLostHandshakeStep("READY",    PKT_READY,    0);
  testLostHandshakeStep("ACTION",   PKT_ACTION,   0);
  testLostHandshakeStep("ACK",      PKT_ACK,      0);
}

// ------------------------------------------------------------ dead peer

static void testDeadPeerTerminates() {
  printf("peer goes silent: both sides give up, neither hangs\n");
  Rig r;
  r.begin(3);
  // Everything from the challenger vanishes once the handshake is under way.
  // The JOIN_REQ itself must get through, or the other side never leaves
  // LS_SCANNING and the scenario never actually starts.
  bool started = false;
  r.ch.filter = [&started](Packet& p, int from) {
    if (started && from == 1) return false;
    if (p.type == PKT_JOIN_REQ) { started = true; return true; }
    return true;
  };
  r.run(120000);
  CHECKM(r.host.state() == LS_OVER, "host stuck in %s",
         linkStateName(r.host.state()));
  CHECKM(r.join.state() == LS_OVER, "join stuck in %s",
         linkStateName(r.join.state()));
  CHECK(!decided(r.host));           // nobody "won"
  printf("  host '%s', join '%s'\n", r.host.overMsg(), r.join.overMsg());
}

// --------------------------------------------------------- retry exhaustion

static void testRetryExhaustion() {
  printf("retries exhaust cleanly\n");
  Rig r;
  r.begin(4);
  r.ch.filter = [](Packet& p, int from) {
    (void)from;
    return p.type != PKT_JOIN_ACK;     // host's reveal never lands
  };
  r.run(120000);
  CHECK(r.host.state() == LS_OVER);
  CHECK(r.join.state() == LS_OVER);
  CHECK(!r.host.awaitingAck());
  CHECK(!r.join.awaitingAck());
}

// ------------------------------------------------------------ duplication

static void testDuplicatesDoNotDoubleResolve() {
  printf("duplicated packets do not double-resolve a turn\n");
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Rig r;
    r.begin(seed);
    r.ch.dupPct = 60;
    r.ch.reorderMaxMs = 30;
    r.run(120000);
    CHECKM(!sawDesync(r), "seed %u desynced under duplication", seed);
    CHECKM(r.bothDone(), "seed %u: host=%s join=%s", seed,
           linkStateName(r.host.state()), linkStateName(r.join.state()));
    if (decided(r.host) || decided(r.join))
      CHECKM(outcomesAgree(r.host, r.join), "seed %u: '%s' vs '%s'", seed,
             r.host.overMsg(), r.join.overMsg());
  }
}

// ---------------------------------------------------------------- collisions

// NEGATIVE RESULT, recorded deliberately.
//
// This started life as an end-to-end test: make both peers submit at the same
// instant, and assert the match completes with jitter and stalls without it.
// It completed 30/30 BOTH ways. The premise was wrong. The two peers stagger
// naturally — the host only reaches LS_MY_TURN when READY arrives, and after
// that each side resolves when its OWN ack lands — so their transmissions sit
// about one airtime apart and the lockstep collision never forms.
//
// So the pathology that motivated the jitter fix does not reproduce under this
// channel model, and simulation therefore does NOT validate that fix. What can
// be tested honestly is the mechanism: that the backoff is actually jittered.
// Whether that matters on real hardware is a question for two radios, where
// capture effect and true timing live.
static void collectRetryIntervals(bool jitter, std::vector<uint32_t>& out) {
  Rig r;
  r.begin(77);
  r.host.setJitter(jitter);
  r.join.setJitter(jitter);

  // Let the joiner's JOIN_REQ through, then black-hole the channel so its
  // retry timer is the only thing driving transmissions.
  std::vector<uint32_t> stamps;
  bool started = false;
  r.ch.filter = [&](Packet& p, int from) {
    if (p.type == PKT_JOIN_REQ && from == 1) {
      stamps.push_back(r.ch.t);
      started = true;
      return false;
    }
    return !started;
  };
  r.run(30000, /*autoPlay=*/false);

  out.clear();
  for (size_t i = 1; i < stamps.size(); i++)
    out.push_back(stamps[i] - stamps[i - 1]);
}

static void testRetryBackoffIsJittered() {
  printf("retry backoff is jittered (mechanism only — see comment)\n");

  std::vector<uint32_t> fixed, jittered;
  collectRetryIntervals(false, fixed);
  collectRetryIntervals(true, jittered);

  CHECKM(fixed.size() >= 2, "expected several retries, got %zu", fixed.size());
  CHECKM(jittered.size() >= 2, "expected several retries, got %zu",
         jittered.size());

  bool allSame = true;
  for (auto v : fixed)
    if (v != fixed[0]) allSame = false;
  CHECKM(allSame, "jitter disabled should give a constant retry interval");

  bool varies = false;
  for (auto v : jittered)
    if (v != jittered[0]) varies = true;
  CHECKM(varies, "jitter enabled should vary the retry interval");

  // And it must stay inside the advertised window.
  for (auto v : jittered)
    CHECKM(v >= Session::RETRY_MS &&
           v <= Session::RETRY_MS + Session::RETRY_JITTER_MS + Rig::STEP_MS,
           "retry interval %u outside [%u, %u]", v, Session::RETRY_MS,
           Session::RETRY_MS + Session::RETRY_JITTER_MS);

  printf("  fixed interval %ums; jittered ", fixed.empty() ? 0 : fixed[0]);
  for (auto v : jittered) printf("%u ", v);
  printf("\n");
}

// --------------------------------------------------------------- linger

// The exact split-verdict scenario the loss sweep measures: the winner's
// PKT_BYE (and every retry/PKT_STATUS reply that could tell the loser the
// real result) goes missing for a while, but the link recovers before either
// side's LINGER_MS window elapses. Both sides used to disagree forever here
// ("you win"/"you lose" vs "peer unreachable"); now they should reconcile.
static void testLingerReconcilesSplitVerdict() {
  printf("linger reconciles what used to be a split verdict\n");
  Rig r;
  r.begin(1);
  bool blocking = false;
  uint32_t blockUntil = 0;
  // Black-hole everything from the moment the first BYE goes out — that
  // covers the winner's BYE retries and both sides' PKT_STATUS traffic — for
  // long enough that the loser's action-retry budget exhausts (MAX_TRIES *
  // ~1.1s), but short of LINGER_MS, so the link is back before either side's
  // linger window runs out.
  r.ch.filter = [&](Packet& p, int from) {
    (void)from;
    if (!blocking && p.type == PKT_BYE) {
      blocking   = true;
      blockUntil = r.ch.t + 10000;
    }
    return !(blocking && r.ch.t < blockUntil);
  };
  r.run(180000);
  CHECKM(r.bothDone(), "host=%s join=%s", linkStateName(r.host.state()),
         linkStateName(r.join.state()));
  CHECK(!sawDesync(r));
  CHECKM(decided(r.host) && decided(r.join),
         "host '%s' join '%s' — expected both to reach a real verdict",
         r.host.overMsg(), r.join.overMsg());
  CHECKM(outcomesAgree(r.host, r.join), "host '%s' join '%s'",
         r.host.overMsg(), r.join.overMsg());
}

// A genuinely dead peer must still finalize on the locally-known outcome —
// LS_LINGER is a delay, not a new way to hang — within a bounded total time:
// the existing watchdog/retry path plus LINGER_MS, not a moment longer.
static void testLingerGenuineUnreachableBounded() {
  printf("genuinely dead peer still finalizes, within a bounded time\n");
  Rig r;
  r.begin(2);
  bool started = false;
  r.ch.filter = [&started](Packet& p, int from) {
    if (started && from == 1) return false;      // joiner vanishes for good
    if (p.type == PKT_JOIN_REQ) { started = true; return true; }
    return true;
  };
  r.run(Session::PEER_TIMEOUT_MS + Session::LINGER_MS + 20000);
  CHECKM(r.host.state() == LS_OVER, "host stuck in %s within the time bound",
         linkStateName(r.host.state()));
  CHECK(!decided(r.host));           // nobody actually won this
}

// --------------------------------------------------------------- protocol tamper

static void testCorruptActionByte() {
  printf("out-of-range action byte is rejected, not executed\n");
  Rig r;
  r.begin(5);
  r.ch.filter = [](Packet& p, int from) {
    (void)from;
    if (p.type == PKT_ACTION) { p.action = 99; reseal(p); }
    return true;
  };
  r.run(120000);
  // Nobody should be able to act on a nonsense action, and nobody should hang.
  CHECK(r.host.state() == LS_OVER);
  CHECK(r.join.state() == LS_OVER);
  CHECK(!sawDesync(r));
  CHECK(r.host.battle().turn == 0);
  CHECK(r.join.battle().turn == 0);
}

static void testTamperedStateHashAbortsBothSides() {
  printf("tampered stateHash aborts the match on both sides\n");
  Rig r;
  r.begin(6);
  // Corrupt EVERY action from the joiner once both sides are genuinely
  // in-battle. Tampering just the first send is unreliable: that packet may
  // collide and never arrive, and its retransmit would then be clean. Turn 0 is
  // excluded because an action can legitimately reach a peer that has not left
  // the handshake yet, where it is ignored rather than desync-checked.
  r.ch.filter = [](Packet& p, int from) {
    if (p.type == PKT_ACTION && p.turn >= 1 && from == 1) {
      p.stateHash ^= 1;
      reseal(p);
    }
    return true;
  };
  r.run(120000);
  CHECK(r.host.state() == LS_OVER);
  CHECK(r.join.state() == LS_OVER);
  CHECK(sawDesync(r));
  // The detector must tell the peer rather than dying quietly — that BYE is
  // the only thing stopping the other side waiting out its watchdog.
  CHECK(r.ui0.saw("desync") || r.ui1.saw("desync"));
}

static void testTamperedSeedRevealRejected() {
  printf("seed reveal that breaks the commitment is rejected\n");
  Rig r;
  r.begin(7);
  r.ch.filter = [](Packet& p, int from) {
    (void)from;
    if (p.type == PKT_JOIN_ACK) { p.seedHalf ^= 0xFFFF; reseal(p); }
    return true;
  };
  r.run(120000);
  CHECKM(r.ui1.saw("bad seed commit"), "joiner accepted a reveal that did not "
         "match the beacon commitment");
  CHECK(r.join.state() == LS_OVER);
  CHECK(r.join.battle().turn == 0);
}

// ------------------------------------------------------------------- sweep

// What is actually guaranteed, at every loss rate:
//
//   * the two BattleStates never diverge          — hard assert
//   * nobody hangs; every session terminates      — hard assert
//   * both peers report the same verdict          — always on a clean channel,
//                                                   and >99% of the time otherwise
//
// The third is best-effort by nature and the limit is real, not a test
// artefact. The winner resolves the final turn, ACKs, and sends a (retried)
// BYE; if that ACK and every BYE retransmission are lost while the loser is
// spending the last of its own retry budget, the loser used to conclude the
// link was dead and say so while the winner had a verdict. LS_LINGER
// (rpg_session.h/.cpp) closes most of that gap: PKT_STATUS lets either side
// answer "what actually happened?" for LINGER_MS after giving up, before
// finalizing — see testLingerReconcilesSplitVerdict for the scenario in
// isolation. It is bounded, not eliminated: a peer that stays unreachable
// through the whole linger window as well still reports what it locally
// knows, so a residual split rate is expected here, just much lower than the
// ~0.5% at 20% loss this used to measure before linger landed. It remains one
// of the things the two-radio test should look at, since real capture effect
// makes long loss runs less likely than this model's independent per-packet
// drops.
static void testLossSweep() {
  printf("loss sweep\n");
  const uint32_t rates[] = { 0, 5, 20, 50 };
  const uint32_t seeds = 400;
  // A hard "zero splits at 5%" would pass on these 400 seeds and then flake:
  // a 3000-seed run with duplication mixed in produces a handful. Bound the
  // rate instead, which is the property actually being claimed. Lowered from
  // 10 (1%) once LS_LINGER landed — see the comment above. Still not 0: the
  // 50% bucket is loss well beyond what linger targets (a healthy link that
  // dropped a few packets), and needs its own headroom.
  const uint32_t maxSplitPermille = 6;    // 0.6% of matches

  for (uint32_t rate : rates) {
    uint32_t completed = 0, abandoned = 0, hung = 0, split = 0;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
      Rig r;
      r.begin(seed);
      r.ch.lossPct = rate;
      r.run(180000);

      if (sawDesync(r)) {
        printf("  FAIL desync at loss=%u%% seed=%u\n", rate, seed);
        g_failures++;
        continue;
      }
      // A host that never found a challenger is still beaconing, which is
      // correct behaviour rather than a hang — at 50% loss the handshake
      // sometimes never gets off the ground at all.
      if (!r.bothDone() && r.host.peerId() == 0 &&
          r.host.state() == LS_SCANNING) {
        abandoned++;
        continue;
      }
      if (!r.bothDone()) {
        hung++;
        if (hung <= 3)
          printf("  FAIL hang at loss=%u%% seed=%u (host=%s join=%s)\n",
                 rate, seed, linkStateName(r.host.state()),
                 linkStateName(r.join.state()));
        g_failures++;
        continue;
      }
      if (decided(r.host) || decided(r.join)) {
        if (!outcomesAgree(r.host, r.join)) {
          split++;
          if (rate == 0) {               // a clean channel must never split
            printf("  FAIL split verdict on a CLEAN channel, seed=%u "
                   "('%s' vs '%s')\n", seed, r.host.overMsg(), r.join.overMsg());
            g_failures++;
          }
          continue;
        }
        completed++;
      } else {
        abandoned++;
      }
    }
    printf("  loss %2u%%: %u completed, %u abandoned cleanly, %u split, "
           "%u hung\n", rate, completed, abandoned, split, hung);
    CHECKM(split * 1000 <= maxSplitPermille * seeds,
           "loss %u%%: %u/%u matches split, above the %u%% bound", rate, split,
           seeds, maxSplitPermille / 10);
  }
}

// --------------------------------------------------------------- turn cap

// Every other scenario here autoplays ATTACK, which converges long before
// MAX_TURNS (README: ~13 turns average) — so nothing at the session level had
// ever exercised the cap path in rpg_link.cpp's battleResolve(), only the link
// layer's own testFightTerminatesUnderAllActions(). This drives the same
// pathological stalemate through the real FSM and simulated channel instead,
// which is the two things the link-level test cannot see: that a capped match
// still drives both peers to LS_OVER through the session's retry/ack machinery,
// and that they still agree once it does.
static void testSessionReachesTurnCap() {
  printf("session drives a stalemate match to the turn cap\n");
  uint32_t capped = 0;
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Rig r;
    r.begin(seed);
    // The pathological case the cap exists for, same as rpg_link.cpp's own
    // testFightTerminatesUnderAllActions: both sides press ITEM every turn.
    // ACT_ITEM never damages the opponent, so nobody's hp ever moves and
    // battleWinner() cannot resolve on its own — only the turn cap can end
    // this. Deterministic regardless of class or seed, unlike a "heal when
    // hurt" pilot, which just converges normally once nobody is hurt.
    r.keyFor = [](const Session&) { return '4'; };
    r.run(600000);            // generous: MAX_TURNS turns at worst-case retries

    CHECKM(r.bothDone(), "seed %u: host=%s join=%s", seed,
           linkStateName(r.host.state()), linkStateName(r.join.state()));
    CHECK(!sawDesync(r));
    CHECKM(outcomesAgree(r.host, r.join),
           "seed %u: host said '%s', join said '%s'", seed, r.host.overMsg(),
           r.join.overMsg());
    CHECK(r.host.battle().turn == r.join.battle().turn);
    CHECK(battleHash(r.host.battle()) == battleHash(r.join.battle()));
    CHECK(r.host.battle().turn <= MAX_TURNS);
    if (r.host.battle().turn == MAX_TURNS) capped++;
  }
  // Deterministic: neither side ever deals damage, so every seed must run to
  // exactly MAX_TURNS. Anything less means the cap path isn't being reached at
  // all — the failure mode this test exists to catch.
  printf("  %u/40 seeds ran to the cap\n", capped);
  CHECK(capped == 40);
}

// ------------------------------------------------------------------- scanning

// These drive both peers by hand rather than through Rig::begin(): reset the
// channel, begin() both sessions, startScanning() each, and set
// autoPair=false so run()'s stand-in human doesn't pick someone off the list
// before the test can inspect it. Rig::run() with autoPlay=false is still the
// right loop to advance time with — nobody here reaches LS_MY_TURN before the
// test takes over.
// REGRESSION, found the moment host/join collapsed into one symmetric open
// state. Both peers now beacon on the same period, so on a fixed interval
// they transmit at the same instant, destroy each other in a collision, wait
// the identical BEACON_MS, and collide again — forever, neither ever
// appearing in the other's list. Discovery is mutual here, so this asserts
// BOTH directions, and it is the reason pumpBeacon()'s jitter is not gated
// on setJitter().
static void testTwoOpenPeersDiscoverEachOther() {
  printf("scanning: two open peers both discover each other (no beacon lockstep)\n");
  Rig r;
  r.ch.reset(20);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;

  r.run(12000, /*autoPlay=*/false);

  CHECKM(r.join.sightingCount() == 1, "join saw %zu peers, expected 1",
         r.join.sightingCount());
  CHECKM(r.host.sightingCount() == 1, "host saw %zu peers, expected 1",
         r.host.sightingCount());
  CHECK(r.join.sighting(0).hostId == r.host.myId());
  CHECK(r.host.sighting(0).hostId == r.join.myId());
  // Neither side auto-paired: presence is not an invitation.
  CHECK(r.join.state() == LS_SCANNING);
  CHECK(r.host.state() == LS_SCANNING);
}

// Either side can be the one who initiates — there is no host-only seat any
// more. This is the mirror of testJoinSightingPairs: the peer that would once
// have been "the host" is the one who picks and challenges.
static void testEitherSideCanChallenge() {
  printf("scanning: the other side can initiate the challenge too\n");
  Rig r;
  r.ch.reset(21);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;

  r.run(12000, /*autoPlay=*/false);
  CHECKM(r.host.sightingCount() == 1, "host saw %zu peers, expected 1",
         r.host.sightingCount());

  r.host.joinSighting(r.join.myId());   // host initiates, for once
  CHECK(r.host.state() == LS_HANDSHAKE);
  CHECK(r.host.peerId() == r.join.myId());

  r.run(90000);
  CHECKM(r.bothDone(), "host=%s join=%s", linkStateName(r.host.state()),
         linkStateName(r.join.state()));
  CHECKM(outcomesAgree(r.host, r.join), "host '%s' join '%s'",
         r.host.overMsg(), r.join.overMsg());
  CHECK(!sawDesync(r));
}

// Symmetry hazard with no equivalent in the old host/join world: both peers
// pick each other in the same instant, so their JOIN_REQs cross on the wire
// and both are challengers waiting on a JOIN_ACK neither would send. Without
// the id tie-break in handlePacket's PKT_JOIN_REQ case both sides grind out
// their retry budget and report "peer unreachable" on a link that is
// perfectly healthy.
static void testSimultaneousMutualChallenge() {
  printf("scanning: simultaneous mutual challenge still pairs\n");
  Rig r;
  r.ch.reset(22);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;

  r.run(12000, /*autoPlay=*/false);
  CHECK(r.host.sightingCount() == 1);
  CHECK(r.join.sightingCount() == 1);

  // The crossing pair, issued back to back with no polling in between.
  r.host.joinSighting(r.join.myId());
  r.join.joinSighting(r.host.myId());

  r.run(90000);
  CHECKM(r.bothDone(), "host=%s join=%s", linkStateName(r.host.state()),
         linkStateName(r.join.state()));
  CHECKM(decided(r.host) && decided(r.join),
         "expected a real verdict, got host '%s' join '%s'",
         r.host.overMsg(), r.join.overMsg());
  CHECKM(outcomesAgree(r.host, r.join), "host '%s' join '%s'",
         r.host.overMsg(), r.join.overMsg());
  CHECK(!sawDesync(r));
  // Exactly one of them ended up hosting.
  CHECKM(r.host.isHost() != r.join.isHost(),
         "both sides think isHost()==%d", (int)r.host.isHost());
}

// REGRESSION. lastRxAt_ used to be stamped once at startScanning() and never
// refreshed while open, but the handshake watchdog measures against it. A
// player reading the list for longer than PEER_TIMEOUT_MS — nothing hurries
// them — would challenge someone and be told "peer timed out" on the very
// next poll, on a link that was never anything but healthy. Impossible before
// symmetry, when LS_JOINING auto-joined the first beacon it heard with no
// human pause in between.
static void testLongBrowseThenChallengeStillPairs() {
  printf("scanning: browsing past the peer timeout then challenging still pairs\n");
  Rig r;
  r.ch.reset(23);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;

  // Sit on the list well past the watchdog deadline, as a slow human would.
  r.run(Session::PEER_TIMEOUT_MS + 10000, /*autoPlay=*/false);
  CHECKM(r.join.state() == LS_SCANNING, "join left the list on its own: %s",
         linkStateName(r.join.state()));
  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting, got %zu",
         r.join.sightingCount());

  r.join.joinSighting(r.host.myId());
  r.run(90000);

  CHECKM(r.bothDone(), "host=%s join=%s", linkStateName(r.host.state()),
         linkStateName(r.join.state()));
  CHECKM(decided(r.host) && decided(r.join),
         "expected a real verdict, got host '%s' join '%s'",
         r.host.overMsg(), r.join.overMsg());
  CHECK(outcomesAgree(r.host, r.join));
  CHECK(!sawDesync(r));
}

static void testScanSeesBeaconingHost() {
  printf("scanning: sees a beaconing host's id and rssi, without joining\n");
  Rig r;
  r.ch.reset(10);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;   // these tests drive selection themselves
  r.ch.rssiOf[0] = -55;   // host's signal as heard by the joiner

  r.run(5000, /*autoPlay=*/false);   // long enough for two beacons (BEACON_MS = 2000)

  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting, got %zu",
         r.join.sightingCount());
  CHECKM(r.join.sighting(0).hostId == r.host.myId(),
         "sighting id 0x%x != host id 0x%x", r.join.sighting(0).hostId,
         r.host.myId());
  CHECKM(r.join.sighting(0).rssi == -55, "rssi %d != -55",
         r.join.sighting(0).rssi);
  // Still just listening — no handshake was ever started.
  CHECK(r.join.state() == LS_SCANNING);
  CHECK(r.join.peerId() == 0);
  CHECK(r.host.state() == LS_SCANNING);
}

static void testScanSightingExpires() {
  printf("scanning: a host that stops beaconing falls off the list\n");
  Rig r;
  r.ch.reset(11);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;   // these tests drive selection themselves

  r.run(3000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting before expiry, got %zu",
         r.join.sightingCount());

  r.host.rematch(0xCCCC3333);   // leaves LS_SCANNING; beacons stop
  r.run(Session::SCAN_EXPIRY_MS + 2000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 0,
         "stale sighting did not expire, count=%zu", r.join.sightingCount());
}

static void testJoinSightingPairs() {
  printf("scanning: joinSighting() pairs with the selected host\n");
  Rig r;
  r.ch.reset(12);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;   // these tests drive selection themselves

  r.run(3000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting, got %zu",
         r.join.sightingCount());

  r.join.joinSighting(r.host.myId());
  CHECK(r.join.state() == LS_HANDSHAKE);
  CHECK(r.join.peerId() == r.host.myId());

  r.run(90000);
  CHECKM(r.bothDone(), "host=%s join=%s", linkStateName(r.host.state()),
         linkStateName(r.join.state()));
  CHECKM(outcomesAgree(r.host, r.join), "host '%s' join '%s'",
         r.host.overMsg(), r.join.overMsg());
  CHECK(!sawDesync(r));
}

// Regression: joinSighting() used to take a raw array index, and
// pumpScanExpiry()'s swap-remove reorders sightings_ on expiry — a sighting
// picked before another one expires could silently resolve to whatever slid
// into that slot, joining the wrong host. Keyed by hostId now; an expired
// selection must be a safe no-op, not a wrong pairing.
static void testJoinSightingExpiredIsNoOp() {
  printf("scanning: joining a sighting that expired before confirm is a no-op\n");
  Rig r;
  r.ch.reset(14);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;   // these tests drive selection themselves

  r.run(3000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting, got %zu",
         r.join.sightingCount());
  uint32_t staleId = r.join.sighting(0).hostId;

  r.host.rematch(0xCCCC3333);   // stops beaconing; the sighting will expire
  r.run(Session::SCAN_EXPIRY_MS + 2000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 0, "sighting did not expire, count=%zu",
         r.join.sightingCount());

  r.join.joinSighting(staleId);
  CHECKM(r.join.state() == LS_SCANNING,
         "expired join must not pair — state is %s",
         linkStateName(r.join.state()));
  CHECK(r.join.peerId() == 0);
}

static void testScanCancelLeavesCleanState() {
  printf("scanning: cancel returns to idle with no leaked handshake\n");
  Rig r;
  r.ch.reset(13);
  r.host.begin(0x1001, 0xAAAA1111);
  r.join.begin(0x2002, 0xBBBB2222);
  r.host.startScanning(0);
  r.join.startScanning(1);
  r.autoPair = false;   // these tests drive selection themselves

  r.run(3000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() >= 1, "expected at least 1 sighting, got %zu",
         r.join.sightingCount());

  r.join.cancelScan();
  CHECK(r.join.state() == LS_IDLE);
  CHECK(r.join.sightingCount() == 0);
  CHECK(r.join.peerId() == 0);
}

// ------------------------------------------------------------- player names

static void testNameRidesTheBeacon() {
  printf("names: a beacon carries the sender's name into the nearby list\n");
  Rig r;
  r.begin(21);
  r.host.setName("ANNIE");
  r.autoPair = false;

  r.run(5000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting, got %zu",
         r.join.sightingCount());
  CHECKM(strcmp(r.join.sighting(0).name, "ANNIE") == 0,
         "sighting name '%s' != 'ANNIE'", r.join.sighting(0).name);
  // The other direction is unnamed and must stay legibly empty rather than
  // picking up stray bytes off the wire — that empty string is what the UI
  // keys its fall-back-to-hex-id on.
  CHECKM(r.host.sighting(0).name[0] == '\0', "unnamed peer read back as '%s'",
         r.host.sighting(0).name);
}

static void testNameSurvivesTheHandshake() {
  printf("names: both peers learn each other's name by the time they pair\n");
  Rig r;
  r.begin(22);
  r.host.setName("ANNIE");
  r.join.setName("BOWIE");

  r.run(20000, /*autoPlay=*/true);
  CHECKM(r.paired() || r.host.battle().turn > 0, "peers never paired");
  // The accepting side learns it from PKT_JOIN_REQ, the challenging side from
  // PKT_JOIN_ACK — two different code paths, so check both seats.
  CHECKM(strcmp(r.host.peerName(), "BOWIE") == 0,
         "host sees peer as '%s', want 'BOWIE'", r.host.peerName());
  CHECKM(strcmp(r.join.peerName(), "ANNIE") == 0,
         "join sees peer as '%s', want 'ANNIE'", r.join.peerName());
}

static void testNameIsTruncatedAndPersists() {
  printf("names: over-long names are truncated, and a name outlives a match\n");
  Rig r;
  r.ch.reset(23);
  r.host.begin(0x1001, 0xAAAA1111);

  r.host.setName("LONGNAMEHERE");
  CHECKM(strlen(r.host.myName()) == PLAYER_NAME_MAX, "kept %zu chars, want %zu",
         strlen(r.host.myName()), PLAYER_NAME_MAX);
  CHECKM(strcmp(r.host.myName(), "LONGNA") == 0, "truncated to '%s'",
         r.host.myName());

  // A name identifies the device, not the match, so rematch() must not clear
  // it — otherwise "r" off the verdict screen drops a player back to a hex id.
  r.host.rematch(0xCCCC3333);
  CHECKM(strcmp(r.host.myName(), "LONGNA") == 0,
         "rematch cleared the name to '%s'", r.host.myName());

  r.host.setName("");
  CHECK(r.host.myName()[0] == '\0');
}

static void testRenameRedrawsTheList() {
  printf("names: a rename bumps the list version, a steady name does not\n");
  Rig r;
  r.begin(24);
  r.host.setName("ANNIE");
  r.autoPair = false;

  r.run(5000, /*autoPlay=*/false);
  const uint32_t settled = r.join.sightingsVersion();
  // Several more beacons of the same name: the row on screen is unchanged, so
  // nothing should ask the UI to repaint.
  r.run(6000, /*autoPlay=*/false);
  CHECKM(r.join.sightingsVersion() == settled,
         "steady name bumped the version %u -> %u", settled,
         r.join.sightingsVersion());

  r.host.setName("ANNE");
  r.run(6000, /*autoPlay=*/false);
  CHECKM(strcmp(r.join.sighting(0).name, "ANNE") == 0,
         "rename not picked up, still '%s'", r.join.sighting(0).name);
  CHECKM(r.join.sightingsVersion() > settled,
         "rename did not bump the version (%u)", r.join.sightingsVersion());
}

// A name arrives over an unauthenticated broadcast link, so it is attacker-
// controlled: any radio in range can put arbitrary bytes in that field and
// compute a valid CRC. Rewriting the beacon in flight is the closest the sim
// gets to a hostile peer. Nothing here is a memory-safety test — the buffers
// are fixed-size and bounded either way — it pins that what reaches the screen
// is drawn from the same character set a locally typed name is.
static void testHostileNameIsSanitized() {
  printf("names: unprintable bytes from the wire are made safe\n");
  Rig r;
  r.begin(25);
  r.autoPair = false;
  r.ch.filter = [](Packet& p, int from) {
    if (p.type == PKT_BEACON && from == 0) {
      // Control bytes, a high byte, and no terminator anywhere in the field.
      const char hostile[7] = {'A', 0x01, 0x1B, (char)0xFF, '\n', 'Z', 'X'};
      memcpy(p.name, hostile, sizeof(p.name));
      packetSeal(p);                     // a hostile peer would send valid CRC
    }
    return true;
  };

  r.run(5000, /*autoPlay=*/false);
  CHECKM(r.join.sightingCount() == 1, "expected 1 sighting, got %zu",
         r.join.sightingCount());
  const char* got = r.join.sighting(0).name;
  // Terminated despite the wire field having no NUL in it at all, and every
  // byte printable. The 7th character is the one the terminator costs.
  CHECKM(strlen(got) == PLAYER_NAME_MAX, "length %zu, want %zu", strlen(got),
         PLAYER_NAME_MAX);
  CHECKM(strcmp(got, "A???\?Z") == 0, "sanitized to '%s'", got);
  for (size_t i = 0; i < strlen(got); i++)
    CHECKM(got[i] >= ' ' && got[i] <= '~', "byte %zu is 0x%02X", i,
           (unsigned char)got[i]);

  // And it must settle: a hostile name that sanitizes to the same string every
  // time is not a rename, so it must not repaint the list on every beacon.
  const uint32_t settled = r.join.sightingsVersion();
  r.run(6000, /*autoPlay=*/false);
  CHECKM(r.join.sightingsVersion() == settled,
         "sanitized name kept bumping the version %u -> %u", settled,
         r.join.sightingsVersion());
}

// --------------------------------------------------------- move timer

// Found on real hardware: nobody presses a key, so this exercises
// MOVE_TIMEOUT_MS the same way an idle or absent player would.
static void testMoveTimerAutoAttacks() {
  printf("nobody moves: MOVE_TIMEOUT_MS auto-attacks for both sides\n");
  Rig r;
  r.begin(4);
  r.run(Session::MOVE_TIMEOUT_MS + 5000, /*autoPlay=*/false);
  CHECKM(r.host.battle().turn > 0, "host stuck on turn %u",
         r.host.battle().turn);
  CHECK(r.host.battle().turn == r.join.battle().turn);
  CHECK(battleHash(r.host.battle()) == battleHash(r.join.battle()));
  // Nothing else fired the peer-unreachable path along the way.
  CHECK(r.host.state() == LS_MY_TURN || r.host.state() == LS_WAIT_PEER);
  CHECK(r.join.state() == LS_MY_TURN || r.join.state() == LS_WAIT_PEER);
}

int main() {
  testCleanMatch();
  testLostHandshake();
  testDeadPeerTerminates();
  testRetryExhaustion();
  testDuplicatesDoNotDoubleResolve();
  testRetryBackoffIsJittered();
  testCorruptActionByte();
  testTamperedStateHashAbortsBothSides();
  testTamperedSeedRevealRejected();
  testLingerReconcilesSplitVerdict();
  testLingerGenuineUnreachableBounded();
  testLossSweep();
  testSessionReachesTurnCap();
  testMoveTimerAutoAttacks();
  testTwoOpenPeersDiscoverEachOther();
  testEitherSideCanChallenge();
  testSimultaneousMutualChallenge();
  testLongBrowseThenChallengeStillPairs();
  testScanSeesBeaconingHost();
  testScanSightingExpires();
  testJoinSightingPairs();
  testJoinSightingExpiredIsNoOp();
  testScanCancelLeavesCleanState();
  testNameRidesTheBeacon();
  testNameSurvivesTheHandshake();
  testNameIsTruncatedAndPersists();
  testRenameRedrawsTheList();
  testHostileNameIsSanitized();

  return testSummary("all session tests passed");
}
