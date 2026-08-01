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

## Status at a glance

One row per item below, kept in sync with it — the prose in each group is
still the source of truth for *why*; this table is only for *where things
stand*. Update both together, same as everywhere else in this file.

| Group | Item | Status | Notes |
| --- | --- | --- | --- |
| 1 | `ACT_FLEE` | Done | |
| 1 | More classes / equipment / leveling | Shelved | Own `PROTO_VERSION` task, not started |
| 1 | Player-chosen class (existing 4) | Done | `PROTO_VERSION` 4→5; class rides `PKT_JOIN_REQ`/`PKT_JOIN_ACK`, `battleInit()` takes explicit ids; pick screen wired in `main.cpp`. Host-suite + firmware build verified; not yet confirmed on hardware |
| 1 | Status effects | Shelved | Not started |
| 1 | Balance beyond the 4-cycle | Shelved | Depends on player-chosen class landing first |
| 2 | Rejoin after "peer unreachable" | Done | LS_LINGER + PKT_STATUS landed (sim-verified); both-sides-stuck sub-case deferred (livelock risk); real hardware disconnect test still open |
| 2 | More than one peer | Open | Not urgent — only 2-unit configs ever tested |
| 2 | Player identity (name entry) | Open | Small |
| 2 | Version-mismatch UX | Open | Not started |
| 3 | Visual confirmation (HP bars, flee prompt) | Done | HP/MP bars, hit/heal flash, and pairing confirmed in a real 2-unit match; flee prompt itself not exercised this run |
| 3 | Title/attract screen | Done | `GRAPHIC` art placeholder still empty |
| 3 | 16-color palette | Done | `DIM`/`BORDER`/`SELECT`/`SPARE` slots drafted but not yet consumed by anything |
| 3 | Hit/heal animation feedback | Done | Directional flash (HIT_FX/HEAL_FX), confirmed on hardware; true multi-frame fade/shake still open |
| 3 | Sound | Open | Blocked on checking whether the buzzer is even wired |
| 3 | Match history | Open | Needs NVS persistence — first persistence this project would have |
| 4 | Two-radio test | Done | Full match played to a verdict on real RF |
| 4 | EU frequency + duty cycle | Open | Only urgent before a unit goes on air outside US/AU |
| 4 | Power management | Open | Not started, untested |

## 1. Finish the combat loop (touches `rpg_link.*` — the tested core)

The highest-leverage group, because every item here is covered by the same
lockstep-determinism guarantee the suite already defends.

- ~~`ACT_FLEE`.~~ Fixed `0657b44` — unconditional forfeit, key `5`, covered by
  `testFleeForfeits` and a `testLockstep` step. No wire-format change.
- **Shelved for now** — more classes/equipment/leveling, status effects, and
  balance-beyond-the-4-cycle, below. Not dropped, just off the active list;
  revisit when picking the next group-1 task.
- **More than 4 classes**, or per-class equipment/leveling. Not sized like the
  other bullets here — `battleInit()` draws the class from exactly 2 bits of
  the seed *because* there are exactly 4 classes. Adding a 5th isn't a content
  add, it's a seed-layout redesign: how many bits, what happens to in-flight
  `seedCommit()` values from the old layout, whether old commits become
  unparseable across the bump. Scope it as its own `PROTO_VERSION` task before
  touching it, not as a line item alongside equipment/leveling.
- ~~**Player-chosen class, among the existing 4.**~~ Landed: `Packet` gained
  `classId` (37→38 bytes, `PROTO_VERSION` 4→5) riding the existing
  `PKT_JOIN_REQ`/`PKT_JOIN_ACK` round trip, in the clear — no fairness stake,
  so it doesn't touch `seedCommit()`/`seedCommitMatches()`. `battleInit()`
  now takes explicit `hostClassId`/`joinerClassId` args instead of deriving
  them from seed bits; the old `CLASS_COUNT == 4` `static_assert` is gone
  (class count is no longer sim-constrained, just limited by the pick UI).
  `main.cpp` gained a pre-match pick screen (`CardputerUi::classSelect()`,
  single-keypress `1`-`4`) gating `h`/`j`/`p` from `LS_IDLE` — a fresh layout
  in the spirit of `tools/designs/character_select.json` rather than a
  literal port, since that mockup previews at a different text size (see the
  UI design workflow note above). Verified: `make -C test` (host suite,
  including a rewritten `testClassChoice` covering every class in both
  seats) and `pio run` (firmware build) both green. Not yet confirmed on
  hardware — a two-unit match with two explicitly different chosen classes
  is a natural follow-up.
- **Status effects** (poison, stun, buffs). Nothing in `BattleState` models a
  multi-turn effect today; `guarding` is the only per-turn flag and it resets
  every turn. A real status system needs a duration field in `Combatant`,
  hashed like everything else, and a decision about whether effects apply
  before or after the initiative order.
- **Balance beyond the 4-cycle.** The current tuning (47–53%) is scripted-pilot
  only — no data from a human playing against another human. Anything added
  above needs the hill-climb sweep re-run, not hand-tuned. Depends on the class
  bullet above landing first if that lands at all — re-sweeping against 4
  classes and then changing the roster invalidates the sweep immediately.

## 2. Session / pairing UX (touches `rpg_session.*` and `main.cpp`)

The protocol works; the experience around it is minimal.

- ~~**Rejoin after "peer unreachable".**~~ Landed: `LS_LINGER`
  (`rpg_session.h`/`.cpp`) and a new `PKT_STATUS` packet give both sides a
  bounded 15s window to reconcile via query/reply before finalizing, instead
  of finalizing immediately on retry/watchdog exhaustion. No `PROTO_VERSION`
  bump — `PKT_STATUS` is a new discriminant on the unchanged `Packet` struct,
  the same way `PKT_BYE` already reuses `action` for `ByeReason`, so it's
  safe against an unupgraded peer (unrecognized `type` is already a no-op).
  Measured effect in `test/test_session.cpp`'s loss sweep: the split-verdict
  rate that used to be ~0.5% at 20% modelled loss is now within the
  tightened 0.6%-at-any-rate bound, including the 50% bucket. Not
  eliminated, bounded: a peer that stays unreachable through the whole
  linger window still reports what it locally knows, same as before linger
  existed. One case deliberately deferred rather than shipped half-tested:
  both sides exhausting their retry budget before either resolves a turn
  (as opposed to the single-lost-BYE case this was built for) — an earlier
  attempt at auto-resending the action from `LS_LINGER` created a genuine
  livelock (`PKT_STATUS` traffic kept the watchdog's `lastRxAt_` fresh, so
  neither timeout could ever fire on a persistently bad link), so that path
  is left a documented no-op for now. Still needs a real two-unit test of
  an actual mid-match disconnect — the sim models packet loss, not real
  SX1262 behavior — see "Still unverified" in `CLAUDE.md`.
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

- ~~**Visual confirmation of the HP bars and the flee prompt.**~~ Confirmed:
  both units flashed with current `main`, paired over LoRa, played a full
  match — HP/MP bars, class-tinted names, and the hit/heal flash colors
  (`HIT_FX`/`HEAL_FX`) all render correctly over real radio round-trips, not
  just single-unit practice mode. Not exercised this run: the flee prompt
  itself (`ACT_FLEE`, key `5`) — already covered by `testFleeForfeits` in the
  host suite, just not seen live yet.
- ~~A title/attract screen.~~ Landed: idle `status()` now draws a title
  (name + `Frame`'s corner version stamp) with the Host/Join prompt below it
  as a real menu section, per `tools/designs/title_screen.json` and
  `main_menu.json`. Still open: the title's `GRAPHIC` placeholder region —
  no actual art yet, and Settings/About were dropped from the menu draft
  since nothing in the session FSM backs them.
- ~~A standard 16-color palette.~~ Landed (`37f1077`, `492fcf7`):
  `tools/designs/palette.json` is a labeled legend draft (16 swatches, one
  per intended use) approved as-is and wired into named `CardputerUi`
  constants — `kHpFullColor`/`kHpMidColor`/`kHpLowColor`/`kMpColor`/
  `kClassColor[]`. Two slots are now genuinely in use beyond the original
  red/green/blue: HP_MID gives the HP bar a third tier (yellow, 25–50%,
  between full and low), and each combatant's name is tinted by class
  (BUNYAN/DRIFTER/COYOTE/VOODOO). Still open: `HEAL_FX`/`HIT_FX` (drafted,
  not consumed by anything yet — see the animations bullet below),
  `DIM`/`BORDER` (no de-emphasized UI exists to apply them to), `SELECT` (no
  selectable menu exists yet), `SPARE` (unclaimed). Wire more of these as
  the features that would use them get built, rather than forcing them in
  now with no real consumer.
- ~~**Feedback on hit/heal.**~~ Landed: `drawCombatants()`'s existing
  same-frame flash now uses `HIT_FX`/`HEAL_FX` (orange-red/cyan) instead of a
  generic invert, so "you got hit" reads differently from "you were healed"
  at a glance, not just from the numbers — confirmed on Unit 1 in practice
  mode. Still open: a true timed animation (fade/shake over multiple frames)
  — `main.cpp` has no per-frame ticking today (`loop()` only redraws
  reactively on Session's `battle()` callback), and building one was ruled
  out of scope for this pass. Skill-specific feedback (as opposed to a
  generic hit/heal) also not attempted — nothing in `BattleState` currently
  distinguishes "how" damage/healing happened, only the resulting numbers.
- **Sound.** Unexplored entirely — unclear if the Cardputer ADV's buzzer (if
  any) is even wired in `platformio.ini`'s lib set. Needs a hardware check
  before it's schedulable.
- **Match history / win-loss record.** Nothing persists across power cycles;
  `rematch()` resets in-memory only. Would need flash storage (NVS via
  `Preferences.h` is the usual ESP32 answer) — first persistence this project
  would have.

## 4. Radio / compliance (touches `main.cpp`, hardware-gated)

- ~~The two-radio test itself.~~ Confirmed: host and joiner paired over actual
  RF and played a full match to a verdict both sides agreed on. Unblocks the
  items below that were waiting on it. Still open from here: real packet loss
  / rejoin behavior at range or with interference — the split-verdict rate in
  `test/test_session.cpp` is still simulation only, and everything on hardware
  so far has been two units six inches apart with a clean channel.
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
- **The two-radio test (group 4) has landed.** Pairing over actual RF is
  confirmed, which unblocks live checks on several items above (rejoin UX,
  split verdicts, retry jitter, and the group-3 visual items) that were
  previously "verified in the sim only."
- Nothing here is prioritized against anything else yet — that's a
  conversation for whoever's picking the next task, not something to bake
  into this document.
