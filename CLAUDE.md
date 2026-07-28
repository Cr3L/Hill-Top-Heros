# CLAUDE.md

## Working agreement

Every task on this project runs through the same loop. This is deliberate — do
not skip steps because a change looks small, and do not batch several tasks into
one pass through the loop.

1. **Lay out the plan.** Write it down before touching code.
2. **Pick the task.** One task. If the plan has several, choose and say which.
3. **Execute.**
4. **Review.** Re-read what was actually written, not what was intended.
5. **Test for bugs.** Run the suite; add cases for anything the change touched.
6. **Fix.** Loop back to 4 until clean.
7. **`/simplify`** — reuse, simplification, altitude cleanups. Quality pass.
8. **`/code-review`** — bug hunt on the working diff. `/simplify` does not do
   this; the two are not interchangeable.
9. **`/compact`**
10. **Commit and push.**

This workflow will evolve. When it changes, update this file in the same pass —
a stale process doc is worse than none.

### Notes on the loop

- Steps 4–6 are a loop, not a sequence. A fix earns another review.
- `/simplify` before `/code-review`, in that order: simplifying churns the diff,
  so reviewing first wastes the review.
- If a step finds nothing, say so explicitly rather than silently moving on.
- Report failures with the actual output. A skipped step gets called out, not
  quietly dropped.

### Repository

`git@github.com:Cr3L/Hill-Top-Heros.git`, branch `main`, over SSH. Pushed to
directly; there is no PR flow set up.

Test binaries under `test/` are gitignored — never commit them.

## What this is

A 1v1 RPG fighter for the **M5Stack Cardputer ADV**, two units paired
peer-to-peer over an **SX1262 LoRa** module. Turn-based, lockstep, deterministic
on both ends.

### Layout

| File | Role |
| --- | --- |
| `rpg_link.{h,cpp}` | Wire format, CRC, seed commit-reveal, battle sim. No I/O. |
| `rpg_session.{h,cpp}` | Session FSM. No hardware — talks to injected interfaces. |
| `main.cpp` | Hardware glue: SX1262, M5 display and keyboard, entry points. |
| `test/` | Host test suite. No hardware, no Arduino toolchain. |

### The layering rule that matters

`rpg_link.cpp` and `rpg_session.cpp` **must stay free of hardware dependencies**
— no RadioLib, no M5, no `millis()`, no `Serial`. They reach the outside world
only through `Transport`, `Clock` and `SessionUi` in `rpg_session.h`. This is
what lets the same compiled protocol logic run on the device and against the
simulated channel in `test/sim_channel.h`.

If a change needs hardware inside those files, the change is wrong — add it to
the interface or keep it in `main.cpp`.

## Testing

```
make -C test          # builds and runs both binaries
make -C test clean
```

- `test/run` — wire format and battle sim.
- `test/run_session` — session FSM against a lossy simulated channel: packet
  loss, collisions, half-duplex deafness, duplication, reordering, in virtual
  time.

Both must pass before any commit. The suite is fast; there is no excuse for
skipping it.

### Invariants the suite defends

- **Lockstep determinism.** Two peers, same seed, same actions → identical
  `battleHash()` every turn, forever. `lockstepHolds()` in
  `test/test_rpg_link.cpp` is the assertion that catches a sim edit introducing
  a desync. Treat a failure there as a release blocker.
- **No desyncs, no hangs, at any loss rate.** The sweep in
  `test/test_session.cpp` asserts both, unconditionally.
- **The battle RNG (`BattleState::rng`) is consumed only from
  `battleResolve()`.** Anything else — retry jitter, UI, effects — uses its own
  `Rng`. Touching the battle RNG off the sim path desyncs the match instantly
  and permanently.

### Adding sim features

New combatant stats are covered by `battleHash()` automatically, so they get
desync-checked for free — but only if they are added to the hash's explicit
field list in `rpg_link.cpp`. It hashes fields one by one on purpose; hashing
the raw struct would cover padding and was only ever deterministic by accident.

## Hardware caveats — unverified, do not assume

- **Pin defines** in `main.cpp` (`PIN_NSS/DIO1/RST/BUSY`) are placeholders from
  a generic wiring. Nobody has confirmed them against real hardware.
- **`RF_FREQ_MHZ` is 915.0** (US/AU). EU is 868.0, and at 22 dBm with a 2 s
  beacon the duty cycle is over the 1% limit.
- **M5 API surface** is written against the original Cardputer; the ADV
  diverged. `M5Cardputer.begin`, `Keyboard.keysState().word`.
- **`seedCommit()` uses mbedtls SHA-256** on device but the FNV fallback on
  host, so the mbedtls path is untested by construction. Older ESP-IDF wants
  `mbedtls_sha256_ret()`.

## Things simulation has NOT proven

Be honest about these rather than citing the green suite as if it settles them.

- **Retry jitter.** The end-to-end collision lockstep does not reproduce in the
  channel model — the peers stagger naturally. The tests prove the backoff is
  jittered, not that it is necessary. Open until two radios say otherwise.
- **Split verdicts.** ~0.5% of matches at 20% modelled loss end with the winner
  knowing the result and the loser reporting "peer unreachable". The model drops
  packets independently; real links lose them in bursts.
- The model does not cover real SX1262 state transitions, DIO1 interrupt timing,
  capture effect, or RSSI-dependent loss. It was written from the same
  understanding that produced the original bugs, so where that understanding is
  wrong it will pass anyway.
