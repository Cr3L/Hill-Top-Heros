# Hill-Top-Heros

A 1v1 turn-based RPG fighter for the **M5Stack Cardputer ADV**. Two units pair
peer-to-peer over an **SX1262 LoRa** module and fight a lockstep-deterministic
match — no server, no access point, just the two radios.

> **Status: unflashed.** The protocol is heavily tested against a simulated
> lossy channel, but this has never run on real hardware. The radio pins are
> placeholders. See [Hardware bring-up](#hardware-bring-up) before you flash
> anything.

## How a match works

Both units run the same battle simulation and exchange only *inputs*, never
state. That keeps packets at 37 bytes, but it means the two sims must stay
bit-identical forever — so every action packet also carries the sender's
pre-turn state hash, and any mismatch voids the match immediately rather than
letting the two screens quietly drift apart.

```
host                                    joiner
  |-- BEACON (commit to my seed half) -->|      every 2s
  |<------- JOIN_REQ (my half) ----------|
  |-- JOIN_ACK (reveal my half) -------->|      joiner checks it against commit
  |<--------- READY ---------------------|
  |                                      |
  |<====== ACTION + stateHash =========>|      per turn, both directions
```

The seed is agreed by **commit-reveal**: the host broadcasts a commitment to its
seed half and only reveals the half in `JOIN_ACK`, so a joiner cannot see the
host's contribution and then pick its own to steer the result. It's 64 bits of
truncated SHA-256 — enough that grinding a favourable preimage isn't worth it on
this hardware, not a serious cryptographic commitment.

### Controls

| Where | Key | Does |
| --- | --- | --- |
| Idle | `h` / `j` | Host a duel / join one |
| Your turn | `1` `2` `3` `4` | Attack / Guard / Skill / Item |
| Match over | `q` | Rematch, same device ID, fresh seed |

`ACT_FLEE` exists in the enum and is deliberately unimplemented — it falls
through to a no-op.

## Layout

| File | Role |
| --- | --- |
| `rpg_link.{h,cpp}` | Wire format, CRC, seed commit-reveal, battle sim. No I/O. |
| `rpg_session.{h,cpp}` | Session FSM. No hardware — talks to injected interfaces. |
| `main.cpp` | Hardware glue: SX1262, M5 display and keyboard, entry points. |
| `test/` | Host test suite. No hardware, no Arduino toolchain. |

### The layering rule

`rpg_link.cpp` and `rpg_session.cpp` **contain no hardware dependencies** — no
RadioLib, no M5, no `millis()`, no `Serial`. They reach the outside world only
through `Transport`, `Clock` and `SessionUi` in `rpg_session.h`.

That is the whole reason the test suite exists: the same compiled protocol logic
runs on the device and against the simulated channel in `test/sim_channel.h`. If
a change needs hardware inside those two files, the change is wrong — extend the
interface, or keep it in `main.cpp`.

## Building and testing

```sh
make -C test          # builds and runs both binaries
make -C test clean
```

- `test/run` — wire format and battle sim.
- `test/run_session` — session FSM against a lossy simulated channel: packet
  loss, collisions, half-duplex deafness, duplication and reordering, in
  virtual time.

No Arduino toolchain needed; both are plain host binaries. They're fast, and
both must pass before a commit. Binaries under `test/` are gitignored.

Firmware itself builds under arduino-cli or PlatformIO with **M5Cardputer** and
**RadioLib** — though see the caveats below, because that build has never
actually been run.

### What the simulation found

The channel model turned up five real protocol bugs, all fixed. The root one:
the receiver would ACK packets it then discarded, stranding the sender in a
retry loop for a packet the peer had already thrown away. The rule that came out
of it is worth stating, since it's easy to get backwards — **never acknowledge a
packet you're going to ignore, and always acknowledge one you've already
processed.**

Current sweep, 24,000 matches: **zero desyncs, zero hangs from 0% to 70% loss.**

### Invariants the suite defends

- **Lockstep determinism.** Two peers, same seed, same actions → identical
  `battleHash()` every turn, forever. `lockstepHolds()` in
  `test/test_rpg_link.cpp` is the assertion that catches a sim edit introducing
  a desync. Treat a failure there as a release blocker.
- **No desyncs, no hangs, at any loss rate**, asserted unconditionally across
  the sweep.
- **The battle RNG (`BattleState::rng`) is consumed only from
  `battleResolve()`.** Retry jitter, UI, effects each use their own `Rng`.
  Touching the battle RNG off the sim path desyncs the match instantly and
  permanently.
- **Wire layout is frozen per `PROTO_VERSION`,** pinned by a `static_assert` on
  `sizeof(Packet)`. Change the packet and the *build* breaks, which is the point
  — a same-size layout change would otherwise sail past the receiver's length
  check and misread fields on air.

### Extending the sim

New combatant stats are desync-checked for free by `battleHash()` — **but only
if you add them to the hash's explicit field list** in `rpg_link.cpp`. It hashes
fields one at a time on purpose; hashing the raw struct would cover alignment
padding and was only ever deterministic by accident.

`battleInit()` currently gives both sides identical placeholder stats
(60 HP, 20 MP, 12 atk, 6 def, 9 spd). The class/loadout system goes there.

## Hardware bring-up

Unverified, in rough order of how badly each will bite:

- **Pin defines.** `PIN_NSS/DIO1/RST/BUSY` in `main.cpp` are placeholders from a
  generic wiring, confirmed against nothing. `radio.begin()` returning
  `RADIOLIB_ERR_NONE` is the check that matters.
- **Region.** `RF_FREQ_MHZ` is `915.0` (US/AU). **EU is 868.0** — and at 22 dBm
  with a 2 s beacon, the duty cycle is over the 1% limit, so raise `BEACON_MS`
  or cut power. That's a compliance problem, not a bug, and it wants settling
  before the radio goes on air.
- **M5 API surface.** Written against the original Cardputer; the ADV diverged.
  Specifically `M5Cardputer.begin` and `Keyboard.keysState().word`.
- **mbedtls.** `seedCommit()` uses SHA-256 on device but an FNV fallback on
  host, so the mbedtls path is untested *by construction* — the host suite never
  reaches it. Older ESP-IDF wants `mbedtls_sha256_ret()`.

## What simulation has NOT proven

The suite is green; that settles less than it looks like it does.

- **Retry jitter.** The end-to-end collision lockstep that motivated the jitter
  doesn't reproduce in the channel model — the peers stagger naturally. The
  tests prove the backoff *is* jittered, not that it *needs* to be. Open until
  two real radios say otherwise.
- **Split verdicts.** ~0.5% of matches at 20% modelled loss end with the winner
  knowing the result and the loser reporting "peer unreachable". The model drops
  packets independently; real links lose them in bursts, which could make this
  better or considerably worse. The proper fix is linger-and-resume, not a
  bigger retry budget — `MAX_TRIES` is already 4 → 6 and the remaining cases are
  genuine link death at the worst possible moment.
- The model covers no real SX1262 state transitions, DIO1 interrupt timing,
  capture effect, or RSSI-dependent loss. It was written from the same
  understanding of the protocol that produced the original bugs — so where that
  understanding is wrong, it will pass anyway.

## Contributing

Working agreement, test commands and the task loop are in
[`CLAUDE.md`](CLAUDE.md).
