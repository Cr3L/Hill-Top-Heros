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
  // Everything from the joiner vanishes once the handshake is under way. The
  // JOIN_REQ itself must get through, or the host never leaves LS_BEACONING and
  // the scenario never actually starts.
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

// --------------------------------------------------------- protocol tamper

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
// spending the last of its own retry budget, the loser correctly concludes the
// link is dead and says so while the winner has a verdict. Closing that gap
// needs a linger-and-resume layer, not a bigger retry count. Measured at ~0.5%
// of matches at 20% loss, and it is one of the things the two-radio test should
// look at, since real capture effect makes long loss runs less likely than this
// model's independent per-packet drops.
static void testLossSweep() {
  printf("loss sweep\n");
  const uint32_t rates[] = { 0, 5, 20, 50 };
  const uint32_t seeds = 400;
  // A hard "zero splits at 5%" would pass on these 400 seeds and then flake:
  // a 3000-seed run with duplication mixed in produces a handful. Bound the
  // rate instead, which is the property actually being claimed.
  const uint32_t maxSplitPermille = 10;    // 1% of matches

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
          r.host.state() == LS_BEACONING) {
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
  testLossSweep();
  testSessionReachesTurnCap();
  testMoveTimerAutoAttacks();

  return testSummary("all session tests passed");
}
