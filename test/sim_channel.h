// sim_channel.h — a deliberately hostile radio channel for two Sessions.
//
// Virtual time, so a match runs instantly and reproducibly. Everything random
// comes from SimChannel::rng, seeded per scenario, so a failure is a seed you
// can replay rather than a flake you hope to see again.
//
// Modelled, in rough order of how much it matters:
//   * airtime      — a packet occupies the channel for AIRTIME_MS
//   * collision    — overlapping transmissions destroy each other
//   * half duplex  — a peer cannot hear anything while it is transmitting
//   * blocking tx  — a send issued while still transmitting queues back-to-back
//   * loss         — independent per-packet drop
//   * duplication  — a delivered packet may arrive twice
//   * reordering   — extra per-packet delivery delay
//
// NOT modelled, and worth remembering before trusting a green run: real SX1262
// state transitions, DIO1 interrupt timing, capture effect (a strong signal
// winning a collision rather than both dying), RSSI-dependent loss.
#pragma once
#include "rpg_session.h"
#include <functional>
#include <vector>
#include <string>
#include <memory>

struct SimChannel {
  static constexpr uint32_t AIRTIME_MS = 50;   // 38 bytes @ SF7/BW125/CR5

  struct Inflight {
    Packet   p;
    int      from;
    uint32_t start, end;                 // occupies the channel over [start,end)
    bool     collided = false;
  };
  struct Queued { Packet p; uint32_t at; int16_t rssi; };

  uint32_t t = 0;                        // virtual milliseconds
  Rng      rng;

  uint32_t lossPct = 0;
  uint32_t dupPct = 0;
  uint32_t reorderMaxMs = 0;
  bool     collisionsEnabled = true;
  // Simulated signal strength per sender, as heard by the other peer. Not
  // modelled against distance or anything physical — a scenario sets this
  // directly to exercise RSSI-dependent display logic (see sim_channel.h's
  // header comment: real RSSI-dependent loss still isn't modelled).
  int16_t  rssiOf[2] = {-70, -70};

  uint32_t txFreeAt[2] = {0, 0};         // when each peer stops transmitting
  std::vector<Inflight> air;
  std::vector<Queued>   inbox[2];

  // Per-packet interception, for targeted scenarios. Return false to drop.
  // Mutating the packet is allowed (the harness re-seals it) so a test can
  // corrupt a field and still present a CRC-clean frame.
  std::function<bool(Packet&, int from)> filter;

  // Counters, for assertions and diagnostics.
  uint32_t sent = 0, delivered = 0, droppedLoss = 0, droppedCollision = 0;

  void reset(uint32_t seed) {
    t = 0;
    rng.seed(seed);
    txFreeAt[0] = txFreeAt[1] = 0;
    air.clear();
    inbox[0].clear();
    inbox[1].clear();
    sent = delivered = droppedLoss = droppedCollision = 0;
    filter = nullptr;
  }

  bool rollPct(uint32_t pct) { return pct && rng.range(1, 100) <= pct; }

  void send(int from, const Packet& pkt) {
    sent++;
    Packet p = pkt;
    if (filter && !filter(p, from)) return;    // dropped before it hits the air

    // A blocking transmit cannot overlap itself: queue back-to-back.
    uint32_t start = t > txFreeAt[from] ? t : txFreeAt[from];
    uint32_t end   = start + AIRTIME_MS;
    txFreeAt[from] = end;

    Inflight f{ p, from, start, end, false };

    if (collisionsEnabled) {
      for (auto& o : air) {
        if (o.from == from) continue;
        if (start < o.end && o.start < end) {  // overlap: both are destroyed
          o.collided = true;
          f.collided = true;
        }
      }
    }
    air.push_back(f);
  }

  // Move finished transmissions into the receiver's inbox.
  void settle() {
    for (size_t i = 0; i < air.size();) {
      Inflight& f = air[i];
      if (t < f.end) { i++; continue; }
      int to = f.from ^ 1;

      bool deliver = true;
      if (f.collided) { droppedCollision++; deliver = false; }
      // Half duplex: the receiver was transmitting over part of this packet.
      else if (txFreeAt[to] > f.start && txFreeAt[to] - AIRTIME_MS < f.end) {
        droppedCollision++;
        deliver = false;
      }
      else if (rollPct(lossPct)) { droppedLoss++; deliver = false; }

      if (deliver) {
        uint32_t delay = reorderMaxMs ? rng.range(0, reorderMaxMs) : 0;
        inbox[to].push_back({ f.p, f.end + delay, rssiOf[f.from] });
        if (rollPct(dupPct))
          inbox[to].push_back({ f.p, f.end + delay + rng.range(1, 40), rssiOf[f.from] });
      }
      air.erase(air.begin() + (long)i);
    }
  }

  bool recv(int who, Packet& out, int16_t* rssi = nullptr) {
    // A peer mid-transmission is deaf.
    if (txFreeAt[who] > t && txFreeAt[who] - AIRTIME_MS <= t) return false;
    for (size_t i = 0; i < inbox[who].size(); i++) {
      if (inbox[who][i].at <= t) {
        out = inbox[who][i].p;
        if (rssi) *rssi = inbox[who][i].rssi;
        inbox[who].erase(inbox[who].begin() + (long)i);
        delivered++;
        return true;
      }
    }
    return false;
  }
};

// ------------------------------------------------------------------ adapters

struct SimTransport : Transport {
  SimChannel& ch;
  int who;
  SimTransport(SimChannel& c, int w) : ch(c), who(w) {}
  void send(const Packet& p) override { ch.send(who, p); }
  bool recv(Packet& out, int16_t* rssi = nullptr) override { return ch.recv(who, out, rssi); }
};

struct SimClock : Clock {
  SimChannel& ch;
  explicit SimClock(SimChannel& c) : ch(c) {}
  uint32_t now() override { return ch.t; }
};

struct RecordingUi : SessionUi {
  std::vector<std::string> lines;
  void status(const char* l) override { lines.push_back(l); }
  void log(const char* m) override { lines.push_back(m); }
  bool saw(const char* needle) const {
    for (auto& l : lines) if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

// --------------------------------------------------------------------- rig

// Two sessions wired to one channel, plus a driver that plays the part of the
// human: it presses an attack key the moment a session offers a turn. That is
// the worst case on purpose — both peers submit at once and collide.
struct Rig {
  SimChannel  ch;
  SimTransport t0{ch, 0}, t1{ch, 1};
  SimClock     clk{ch};
  RecordingUi  ui0, ui1;
  Session      host{t0, clk, ui0};
  Session      join{t1, clk, ui1};

  static constexpr uint32_t STEP_MS = 5;

  // What key to press for a session that's offering a turn. Defaults to
  // always-attack, the worst case for collisions and the case every existing
  // scenario is tuned against; a scenario that needs the match to run long
  // (e.g. reaching MAX_TURNS) overrides this to play a healing mix instead.
  std::function<char(const Session&)> keyFor = [](const Session&) { return '1'; };

  // Whether run() plays the human who picks a peer off the scan list. The
  // scanning tests turn this off to inspect the list themselves.
  bool autoPair = true;

  void begin(uint32_t chanSeed, uint32_t hostSeed = 0xAAAA1111,
             uint32_t joinSeed = 0xBBBB2222) {
    ch.reset(chanSeed);
    ui0.lines.clear();
    ui1.lines.clear();
    host.begin(0x1001, hostSeed);
    join.begin(0x2002, joinSeed);
    // Both simply open — there is no host/join role any more. Deliberately
    // sends nothing here, so a filter installed right after begin() still
    // catches the very first packet on the air, same as before.
    host.startScanning(0);
    join.startScanning(1);
  }

  // Advance virtual time, polling both peers. Presses keys for whoever is
  // waiting on input. Stops early once both sides are finished.
  void run(uint32_t maxMs, bool autoPlay = true) {
    for (uint32_t elapsed = 0; elapsed < maxMs; elapsed += STEP_MS) {
      ch.t += STEP_MS;
      ch.settle();
      host.poll();
      join.poll();
      // Plays the part of the human who picks someone off the list. Only
      // `join` ever initiates: `host` stays open and passive and accepts the
      // challenge, mirroring the real flow where exactly one side presses a
      // key. Deliberately OUTSIDE the autoPlay gate: autoPlay means "press
      // battle keys", and pairing used to happen in the FSM itself
      // (LS_JOINING auto-joined the first beacon heard) regardless of it —
      // the autoPlay=false scenarios still expect to reach a live match.
      if (autoPair && join.state() == LS_SCANNING && join.sightingCount() > 0)
        join.joinSighting(join.sighting(0).hostId);
      if (autoPlay) {
        if (host.state() == LS_MY_TURN) host.onKey(keyFor(host));
        if (join.state() == LS_MY_TURN) join.onKey(keyFor(join));
      }
      if (bothDone()) return;
    }
  }

  bool bothDone() const {
    return host.state() == LS_OVER && join.state() == LS_OVER;
  }
  bool paired() const {
    return host.state() != LS_IDLE && host.state() != LS_SCANNING &&
           join.state() != LS_IDLE && join.state() != LS_SCANNING &&
           host.state() != LS_HANDSHAKE && join.state() != LS_HANDSHAKE;
  }
};

// Drop the Nth packet of a given type from a given sender. index 0 = the first.
inline std::function<bool(Packet&, int)> dropNth(uint8_t type, int n,
                                                 int fromWho = -1) {
  auto seen = std::make_shared<int>(0);
  return [type, n, fromWho, seen](Packet& p, int from) {
    if (p.type != type) return true;
    if (fromWho >= 0 && from != fromWho) return true;
    return (*seen)++ != n;
  };
}
