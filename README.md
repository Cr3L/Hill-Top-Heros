# Hill-Top-Heros

A 1v1 turn-based RPG fighter for the **M5Stack Cardputer ADV**. Two units pair
peer-to-peer over an **SX1262 LoRa** module and fight a lockstep-deterministic
match — no server, no access point, just the two radios.

> **Status: one unit boots, nothing has been transmitted.** The protocol is
> heavily tested against a simulated lossy channel. A single Cardputer ADV with
> the official Cap LoRa-1262 now boots, initialises the radio cleanly and drives
> the UI — but no packet has ever crossed the air between two units. See
> [Hardware](#hardware).

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

The firmware itself builds under PlatformIO (`platformio.ini`, board
`m5stack-stamps3`, with **M5Cardputer** and **RadioLib**):

```sh
pio run              # build firmware
pio run -t upload    # flash
pio device monitor   # serial log, 115200 over USB CDC
```

It builds, flashes and boots on a Cardputer ADV.

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

### Classes

Each side is dealt one of four, from two bits of the shared seed — no RNG is
drawn, so a given seed produces the same battle stream it always did. `classId`
is hashed like any other field, because the class decides which SKILL formula
runs and *how many* RNG calls it makes; two peers disagreeing there would
diverge permanently on the first skill.

| | HP | MP | atk | def | spd | Skill | Cost |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Bunyan** | 102 | 21 | 10 | 9 | 5 | Timber Cleave — `atk + def + d5` | 6 |
| **Drifter** | 70 | 24 | 14 | 4 | 8 | Fan the Hammer — `atk×2 + d13` | 8 |
| **Coyote** | 87 | 24 | 11 | 5 | 14 | Twin Fangs — `atk×2 + d6 + d6` | 6 |
| **Voodoo** | 88 | 31 | 8 | 7 | 7 | Poppet Pin — `atk + foe.atk + d7`, drains ¼ | 7 |

Bunyan is the only skill that pays off DEF; Voodoo's is the only thing in the
sim that reads an *opposing* stat, which makes it the answer to the glass
cannons and useless against the tank. Skills ignore GUARD and are mitigated by
`def/2` — guarding answers ATTACK, HP and MP answer skills.

**Initiative goes to the higher `spd`; a tie alternates by turn parity.** That
matters because a quarter of pairings are mirrors, where `spd` is equal by
definition — breaking those ties the obvious way, always to slot 0, gave the
host **86%** of them. Parity reads only state both peers already agree on, so it
needs no RNG and cannot desync. Mirrors now sit at 47.7% over 50,000 matches,
pinned by `testMirrorsAreFair`.

Tuned against a scripted-pilot sweep of every pairing: all four sit between 47%
and 53%, matches average 13 turns. **The stat pools are the tuned part** — the
formulas and `atk`/`def`/`spd` carry the identities and were held fixed. Change
a number and re-run the sweep rather than guessing; the classes are tightly
coupled through it, and moving one moved every other by tens of points.

### Why the match is bounded

Two safeguards, both learned the hard way while tuning the classes above.

**`ACT_ITEM` gets 3 charges.** It heals 18–24, which is more than a sustained
attack deals against *any* class in the table. Unlimited, that is not a balance
wart — it is a match that never ends, and 32% of them didn't.

**Matches stop at `MAX_TURNS` (100).** Capping the pouch alone made things
worse: an empty pouch fizzles, so two players who keep choosing it with no MP
left deadlock completely. Nothing else guarantees a turn makes progress, so the
cap is what actually closes the hole — the charges are balance, the cap is the
safety net. It also keeps `Packet::turn` clear of its `uint16_t` ceiling.

How often it fires depends entirely on whether a player wastes turns on an
empty pouch, which is why the screen shows the count:

| Scripted pilot, 50,000 matches | Cap fires | Avg turns |
| --- | --- | --- |
| Heals only with a charge in hand | **0%** | 12.7 |
| Heals whenever hurt, charges or not | **16.6%** | 26.7 |

The cap decides on HP **as a fraction of each pool**, not raw HP — Bunyan's 102
would otherwise beat a Drifter at full health on 70, which is not the same
question as who was winning.

## Hardware

Target is a Cardputer ADV with the official **Cap LoRa-1262** (SX1262 + GNSS).
On boot the device prints `radio.begin=0 ioe=1` and shows the same on screen.

### Radio

**Pinout**, which cost more to establish than it looks:

| Signal | GPIO | |
| --- | --- | --- |
| NSS / DIO1 / RST / BUSY | 5 / 4 / 3 / 6 | |
| SCK / MISO / MOSI | 40 / 39 / 14 | shared with the SD slot |
| RF antenna switch | PI4IOE5V6408 @ I2C `0x43`, **P0 high** | |

Three things will each independently stop the radio working, and two of them
fail in ways that do not look like the radio:

- **The antenna switch must be enabled before `radio.begin()`.** It sits behind
  an I2C expander. Miss it and `begin()` *still returns 0* while nothing reaches
  the air — which looks exactly like a protocol bug and will send you debugging
  the wrong layer entirely.
- **SPI must be started explicitly.** M5's docs say RadioLib auto-maps the SPI
  pins and no setup is needed. That holds for M5's own board definition, not for
  the `m5stack-stamps3` one this project builds against, whose variant defines
  `SCK/MISO/MOSI` as `-1`.
- **The pins are not guessable.** `DIO1` is GPIO4; GPIO2, a plausible-looking
  guess, is the ADV's Port A I2C SDA.

### Display

**Draw to 240 × 135 landscape.** That is the only number a UI author needs; the
rest of the table is here so an outside file's claims can be checked against it.

| | |
| --- | --- |
| Controller | ST7789, native 135 (w) × 240 (h), colour-inverted |
| Window offset | x 52, y 40 — the panel is a cut-down of a 240 × 320 die |
| Usable area | **240 × 135** after `rotation = 1` |
| CS / RST / backlight | GPIO 37 / 33 / 38 (PWM) |
| Bus | `SPI3_HOST` — separate from the radio's |

M5GFX autodetects all of it and applies the rotation itself, so `main.cpp`
configures nothing (`board_M5CardputerADV` in M5GFX's autodetect). Code that
sets rotation or offsets by hand is fighting the library, and a layout written
for 240 × 320 runs off the bottom of the screen.

### Still open

- **Region.** `RF_FREQ_MHZ` is `915.0` (US/AU). **EU is 868.0** — and at 22 dBm
  with a 2 s beacon the duty cycle is over the 1% limit, so raise `BEACON_MS` or
  cut power. A compliance matter, not a bug, and worth settling before the radio
  goes on air in the EU.
- **mbedtls.** `seedCommit()` takes SHA-256 on device and an FNV fallback on
  host, and the two are never compared. Device-to-device they always agree, so
  this is latent. Older ESP-IDF wants `mbedtls_sha256_ret()`.

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
