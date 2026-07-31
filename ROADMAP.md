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
- **More than 4 classes**, or per-class equipment/leveling. Not sized like the
  other bullets here — `battleInit()` draws the class from exactly 2 bits of
  the seed *because* there are exactly 4 classes. Adding a 5th isn't a content
  add, it's a seed-layout redesign: how many bits, what happens to in-flight
  `seedCommit()` values from the old layout, whether old commits become
  unparseable across the bump. Scope it as its own `PROTO_VERSION` task before
  touching it, not as a line item alongside equipment/leveling.
- **Player-chosen class, among the existing 4.** Distinct from the bullet
  above — this doesn't touch the seed layout at all. `battleInit()` currently
  draws class from 2 raw seed bits specifically because it was never meant to
  be a choice; picking one of the 4 that already exist is a decoupling, not a
  redesign:
  - Class has no fairness stake, unlike the RNG seed — there's nothing to gain
    by lying about which class you want — so it can be exchanged in the clear,
    same as the joiner's `seedHalf` already is. It rides the existing
    `PKT_JOIN_REQ`/`PKT_JOIN_ACK` round trip, one new `uint8_t classChoice`
    each way, and does not interact with `seedCommit()`/`seedCommitMatches()`
    at all.
  - `Packet` is 37 bytes today against a hard 40-byte cap
    (`static_assert`, ~50ms airtime budget at SF7/BW125/CR5) — one byte per
    direction fits with room to spare.
  - Still needs a `PROTO_VERSION` bump (4→5): this project's stated policy is
    that any wire *or sim-rule* change earns one, and swapping seed-derived
    class for a chosen one is a sim-rule change even though the seed layout
    itself doesn't move.
  - `battleInit()` changes to take two explicit `classId` args instead of
    deriving them from seed bits; drop the `(seed >> (i*2)) & 3` line and its
    `static_assert(CLASS_COUNT == 4, ...)` tie.
  - Needs a pre-handshake class-pick UI — `tools/designs/character_select.json`
    is a drafted mockup for exactly this, landed ahead of the feature per the
    "mockups aren't wired to runtime state" note in `CLAUDE.md`. Landing the
    JSON did not land this item.
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

- **Rejoin after "peer unreachable" — the highest-value item in this group.**
  Unlike the other three bullets below, this isn't speculative: the loss sweep
  in `test/test_session.cpp` already measures a ~0.5% split-verdict rate at 20%
  modelled loss, a quantified problem with real players, not a nice-to-have.
  The other three (multi-peer, identity, version UX) only start to matter once
  there's more than one pair of testers. Once hardware is confirmed, put this
  ahead of group 3.
  Today "peer unreachable" is terminal — `q` starts a fresh rematch with a new
  seed, there's no "try to reconnect to the same match." A linger-and-resume
  path was already flagged in `CLAUDE.md` as the real fix, not a bigger retry
  budget.
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
- ~~A title/attract screen.~~ Landed: idle `status()` now draws a title
  (name + `Frame`'s corner version stamp) with the Host/Join prompt below it
  as a real menu section, per `tools/designs/title_screen.json` and
  `main_menu.json`. Still open: the title's `GRAPHIC` placeholder region —
  no actual art yet, and Settings/About were dropped from the menu draft
  since nothing in the session FSM backs them.
- **A standard 16-color palette.** No longer monochrome — the HP/MP bars now
  use `TFT_RED`/`TFT_GREEN`/`TFT_BLUE`/`TFT_WHITE`, picked ad hoc (readable
  fill vs. embedded label text, nothing more principled) while squeezing the
  practice-mode battle screen. Still not a real palette: those are hardcoded
  LovyanGFX constants in `main.cpp`, not sourced from anywhere. Not a
  hardware limit either way — the panel is full rgb565, and
  `tools/designs/*.json` drafts already carry a 16-entry `palette16`, but it
  has never been wired into a `CardputerUi` draw call — same "mockup, not
  runtime state" gap as the rest of those drafts.
  **Workflow for this one, once picked:** tune the 16 colors visually in
  `tools/ui_designer.html`'s palette editor and save the draft — the tool
  supports this today. Translating the saved hex values into actual
  `CardputerUi` color constants is a separate follow-up step; the tool does
  not write C++ or touch `main.cpp` itself.
  Settling on the 16 colors (HP bar states, class tints, hit/heal feedback)
  is its own design pass, not a quick wiring change once picked — flagged as
  open-ended rather than sized.
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
