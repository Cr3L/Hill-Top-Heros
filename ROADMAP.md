# Hill-Top-Heros — systems roadmap

Big-picture list of what a shippable game needs beyond the current duel
prototype. This is a map of gaps, not a commitment to build all of it — each
item still enters the normal loop (`CLAUDE.md`) as its own single task when
picked up. Grouped by what's actually missing, checked against the code as of
`0657b44`, not guessed.

## What exists today

Two Cardputers, one duel: commit-reveal seed, lockstep sim, 4 classes, 5
actions (attack/guard/skill/item/flee), turn cap, HP bars, rematch on `q`.
That's the whole game. Everything below is what's not there yet, organized by
how close it sits to the tested core.

## 1. Finish the combat loop (touches `rpg_link.*` — the tested core)

The highest-leverage group, because every item here is covered by the same
lockstep-determinism guarantee the suite already defends.

- ~~`ACT_FLEE`.~~ Fixed `0657b44` — unconditional forfeit, key `5`, covered by
  `testFleeForfeits` and a `testLockstep` step. No wire-format change.
- **More than 4 classes**, or per-class equipment/leveling. `battleInit()`
  draws from 2 bits of the seed specifically because there are exactly 4
  classes — this is a real wire-format decision (`PROTO_VERSION`-worthy), not
  a content add.
- **Status effects** (poison, stun, buffs). Nothing in `BattleState` models a
  multi-turn effect today; `guarding` is the only per-turn flag and it resets
  every turn. A real status system needs a duration field in `Combatant`,
  hashed like everything else, and a decision about whether effects apply
  before or after the initiative order.
- **Balance beyond the 4-cycle.** The current tuning (47–53%) is scripted-pilot
  only — no data from a human playing against another human. Anything added
  above needs the hill-climb sweep re-run, not hand-tuned.

## 2. Session / pairing UX (touches `rpg_session.*` and `main.cpp`)

The protocol works; the experience around it is minimal.

- **Rejoin after "peer unreachable."** Today that message is terminal — `q`
  starts a fresh rematch with a new seed, there's no "try to reconnect to the
  same match." Given the split-verdict risk already documented (~0.5% at 20%
  loss), a linger-and-resume path was already flagged in `CLAUDE.md` as the
  real fix, not a bigger retry budget.
- **More than one peer.** `startHosting()`/`startJoining()` assume exactly two
  radios in range. No discovery UX for "which of three nearby hosts do I
  join," no room codes, nothing yet needs it since two units is the only
  configuration ever tested.
- **Player identity.** `deviceId()` is a MAC-derived number; there's no name
  entry, so both screens show generic class names, not player names. Small,
  but it's the first thing a second player will ask about.
- **Version-mismatch UX.** A `PROTO_VERSION` bump silently fails to pair
  (`BYE_BAD_COMMIT` territory) rather than reporting "peer is running a
  different version." Matters more as this gets updated on two units
  independently.

## 3. Presentation (touches `main.cpp` only — no protocol risk)

Safe to build without touching anything the test suite defends.

- **Visual confirmation of the HP bars and the flee prompt** — both landed
  (`91e3117`, `0657b44`) but never seen live; blocked on a second Cardputer.
  First thing to close once hardware allows.
- **A title/attract screen.** Right now boot goes straight to the idle
  `status()` diagnostic. Fine for bring-up, not for handing the device to
  someone else.
- **Animations or feedback on hit/heal/skill.** Currently a battle() redraw is
  the only signal a turn resolved — no flash, no shake, no distinction between
  "you got hit" and "nothing happened this turn" beyond reading the numbers.
- **Sound.** Unexplored entirely — unclear if the Cardputer ADV's buzzer (if
  any) is even wired in `platformio.ini`'s lib set. Needs a hardware check
  before it's schedulable.
- **Match history / win-loss record.** Nothing persists across power cycles;
  `rematch()` resets in-memory only. Would need flash storage (NVS via
  `Preferences.h` is the usual ESP32 answer) — first persistence this project
  would have.

## 4. Radio / compliance (touches `main.cpp`, hardware-gated)

- **The two-radio test itself.** Still nothing has radiated. This blocks or
  informs almost everything above it can't be scheduled around.
- **EU frequency + duty cycle.** `RF_FREQ_MHZ` is hardcoded 915.0; `CLAUDE.md`
  already flags 868.0 + duty-cycle budget as unsettled. Only urgent if a unit
  is going on air outside US/AU.
- **Power management.** No sleep/wake behavior explored; `startReceive()` runs
  continuously. Battery life on a handheld device is presumably a real
  constraint, untested.

## Sequencing notes

- **Everything in group 1 changes the wire format or the hash.** Each one is
  its own `PROTO_VERSION` bump and needs the full suite re-run, not a
  drive-by addition alongside something else.
- **Group 2 and 3 don't share files with group 1** except where `main.cpp`
  calls into `rpg_session.h` — low collision risk to interleave.
- **The two-radio test (group 4) isn't really "a task"** — it's a
  precondition that changes what "done" means for several items above
  (rejoin UX, split verdicts, retry jitter) once it's possible to run.
- Nothing here is prioritized against anything else yet — that's a
  conversation for whoever's picking the next task, not something to bake
  into this document.
