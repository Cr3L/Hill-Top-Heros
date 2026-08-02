# Hill-Top-Heros — systems roadmap

Big-picture list of what a shippable game needs beyond the current duel
prototype. This is a map of gaps, not a commitment to build all of it — each
item still enters the normal loop (`CLAUDE.md`) as its own single task when
picked up. Grouped by what's actually missing, checked against the code, not
guessed.

The status table below is only true if it is updated in the same pass as the
work, per `CLAUDE.md`. This file used to carry a "checked as of `<commit>`"
stamp, which is what let *Version-mismatch UX* sit at "Not started" for days
after it shipped — a stamp records when someone last looked, and reading it
tells you nothing about which individual row drifted. If a row looks wrong,
check the code and fix the row.

## What exists today

Two Cardputers, one duel: commit-reveal seed, lockstep sim, 4 player-chosen
classes, 5 actions (attack/guard/skill/item/flee), turn cap, HP bars, an
on-screen HOST/JOIN role tag, and a rematch prompt (`r` same role and class,
`q` back to the menu). That's the whole game. Everything below is what's not
there yet, organized by how close it sits to the tested core.

## Status at a glance

One row per item below, kept in sync with it — the prose in each group is
still the source of truth for *why*; this table is only for *where things
stand*. Update both together, same as everywhere else in this file.

| Group | Item | Status | Notes |
| --- | --- | --- | --- |
| 1 | `ACT_FLEE` | Done | |
| 1 | More classes / equipment / leveling | Shelved | Own `PROTO_VERSION` task, not started |
| 1 | Player-chosen class (existing 4) | Done | `PROTO_VERSION` 4→5; class rides `PKT_JOIN_REQ`/`PKT_JOIN_ACK`, `battleInit()` takes explicit ids; pick screen wired in `main.cpp`. Confirmed on hardware: 5 matches played two-unit with distinct chosen classes |
| 1 | Status effects | Shelved | Not started |
| 1 | Balance beyond the 4-cycle | Shelved | Depends on player-chosen class landing first |
| 2 | Rejoin after "peer unreachable" | Done | LS_LINGER + PKT_STATUS landed (sim-verified); both-sides-stuck sub-case deferred (livelock risk); real hardware disconnect test still open |
| 2 | More than one peer | Open | Not urgent — only 2-unit configs ever tested |
| 2 | Peer discovery / symmetric presence | Done | Host/join roles retired: one `LS_SCANNING` "open" state beacons *and* listens. Pick a player off the list to challenge; either side can initiate. Confirmed on hardware |
| 2 | Player identity (name entry) | Done | `PROTO_VERSION` 5→6, still 38 bytes — `name[7]` shares the pre-CRC bytes with turn/action/stateHash. Nearby list shows names; hex id is the fall-back. Wire names are sanitized on receive; names are cosmetic, not authenticated. Persisted to NVS (survives reboot); still not on the battle screen. Unverified on hardware |
| 2 | Version-mismatch UX | Done | `packetVersionMismatch()` + a status line during pairing (`327dd87`). Catches layout-preserving bumps only, which includes the live v5/v6 skew; a layout-changing peer still fails closed and silent |
| 3 | Visual confirmation (HP bars, flee prompt) | Done | HP/MP bars, hit/heal flash, and pairing confirmed in a real 2-unit match; flee prompt itself not exercised this run |
| 3 | Title/attract screen | Done | `GRAPHIC` art placeholder still empty |
| 3 | 16-color palette | Done | `DIM`/`BORDER`/`SELECT`/`SPARE` slots drafted but not yet consumed by anything |
| 3 | Hit/heal animation feedback | Done | Directional flash (HIT_FX/HEAL_FX), confirmed on hardware; true multi-frame fade/shake still open |
| 3 | Post-match rematch prompt + host/joiner indicator | Done | `Session::rematchKeepingRole()`, `r`/`q` split at `LS_OVER`, persistent HOST/JOIN corner tag. Sourced from playtest feedback ("hard to play again", "hard to know who is hosting") |
| 3 | Sound | Open | Blocked on checking whether the buzzer is even wired |
| 3 | Match history | Open | NVS is wired up now; what's missing is the data model, not the storage |
| 4 | Two-radio test | Done | Full match played to a verdict on real RF |
| 4 | EU frequency + duty cycle | Open | Only urgent before a unit goes on air outside US/AU |
| 4 | Power management | Open | Not started, untested |
| 4 | OTA firmware updates | Open | WiFi OTA vs. LoRa OTA unresolved; intended as reusable beyond this project |
| 5 | Radio-first mechanics (proximity/breadcrumb/intercept) | Partly unblocked | The passive-scan mechanism it was waiting on now exists (sighting log with RSSI). "Enemy detected" is largely landed as the nearby-players list; breadcrumb's NVS blocker is gone but it still needs a reboot-surviving time base, intercept still unscoped |

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
  seats) and `pio run` (firmware build) both green. **Confirmed on
  hardware**: two units played 5 matches back to back with distinct chosen
  classes on each side — surfaced the two pieces of feedback the rematch/
  role-indicator bullet below addresses.
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
- ~~**Player identity.**~~ Landed: `PROTO_VERSION` 5→6 for a 6-character
  player name, entered on its own `N` screen off the idle menu and shown in
  the nearby-players list in place of the hex id. The packet did **not** grow
  — it was at 38 of its 40-byte airtime ceiling, so `name[7]` shares the last
  seven bytes before the CRC with `turn`/`action`/`stateHash` in a union, and
  `classId` moved up beside `commit` to make that run contiguous. That is only
  sound because the two arms are used by disjoint packet types (handshake vs.
  sim), which is a rule the compiler cannot enforce: `testNameSharesTheSimBytes`
  in the link suite pins the overlap and the surviving fields, and the union's
  own comment states which types may write which arm. Sim-verified across four
  new session tests (beacon → sighting, both handshake seats, truncation and
  rematch survival, rename-bumps-the-list-version). Two pieces deliberately not
  built:
  - ~~**The name does not survive a power cycle.**~~ Landed: NVS via
    `Preferences.h`, the first thing this project stores across a reboot.
    `main.cpp` opens the `htheroes` namespace once in `setup()` and restores
    the name into `Session` after `begin()`; `saveName()` writes on the Enter
    commit only, never per keystroke. No `PROTO_VERSION` bump and no session
    change — `Session` still owns the name in RAM and cannot see NVS, per the
    layering rule. The stored value is not re-sanitized on read, deliberately:
    its only writer is the local entry screen, which already filters to
    printable ASCII, unlike a name off the air. **Unverified on hardware** —
    like the name feature itself.
  - **The name is not on the battle screen.** `drawCombatants()` is at its
    real ceiling of row 127 (see the HP-number entry in group 3), so putting a
    name beside the class there is part of that same budget rework, not a
    line to add.

  Two things a security pass over the change surfaced, one fixed and one not:

  - **Received names are sanitized; that is not optional.** A name arrives
    over an unauthenticated broadcast link, so any radio in range can put
    arbitrary bytes in that field and compute a valid CRC — `packetValid()`
    stops accidental corruption, not a hostile sender. `copyWireName()` is the
    single choke point every stored peer name goes through, and it forces both
    a terminator (the field is 7 raw bytes with no guaranteed NUL) and the
    same printable range the local name entry accepts. Covered by
    `testHostileNameIsSanitized`. Note the coupling this creates, since it is
    easy to "simplify" back out: the rename check in `recordSighting()` must
    compare *sanitized against sanitized*, or a peer whose name contains one
    unprintable byte differs from its own stored copy on every beacon and
    repaints the nearby list forever.
  - **A peer can still display any name it likes, including one that mimics
    another player's.** Names are cosmetic and are deliberately not bound to
    identity: pairing keys on `hostId`, and the name is never hashed into
    `seedCommit()` or `battleHash()`, so this buys no leverage over the shared
    seed. Making a name trustworthy means authenticating the link, which this
    project does not do and has never claimed to — worth stating plainly here
    rather than leaving someone to assume the name in the list is proof of who
    they are challenging.
- ~~**Version-mismatch UX.**~~ Landed in `327dd87`: `packetVersionMismatch()`
  is true for a well-formed packet (right magic, intact CRC) whose version
  isn't ours — distinct from `packetValid()`'s false, which also covers plain
  noise. `Session::pumpRx()` turns that into `"peer is on a different
  version"`, shown during pairing states only (mid-match it would be stray
  traffic, not worth interrupting a battle over) and once per pairing attempt
  rather than once per beacon.

  **The limit is structural and worth restating here**, because it is easy to
  read the feature as more complete than it is: the CRC is checked against
  *our* `sizeof(Packet)`, so this only catches a bump that preserved the
  packet's size. A peer on a layout-changing version sends bytes that don't
  line up with our fields at all, fails closed, and is silent — the exact
  symptom the feature exists to remove. It cannot be tested from a single
  binary either, which would need two `Packet` definitions at once.

  The current 5→6 bump falls on the good side of that line: `magic` and
  `version` are the first three bytes and did not move, and the packet stayed
  38 bytes, so a v5 unit is correctly reported rather than silently ignored.

## 3. Presentation (touches `main.cpp` only — no protocol risk)

Safe to build without touching anything the test suite defends.

- ~~**Visual confirmation of the HP bars and the flee prompt.**~~ Confirmed:
  both units flashed with current `main`, paired over LoRa, played a full
  match — HP/MP bars, class-tinted names, and the hit/heal flash colors
  (`HIT_FX`/`HEAL_FX`) all render correctly over real radio round-trips, not
  just single-unit practice mode. Not exercised this run: the flee prompt
  itself (`ACT_FLEE`, key `5`) — already covered by `testFleeForfeits` in the
  host suite, just not seen live yet.
- ~~A title/attract screen.~~ Landed: idle `status()` now draws a green title
  (name + `Frame`'s corner version stamp) with the open/practice prompt below
  it as a real menu section, per `tools/designs/boot_menu.json` — one draft of
  the one `LS_IDLE` screen, superseding the earlier split title_screen/
  main_menu pair. Still open: no actual art yet, and Settings/About were
  dropped from the menu draft since nothing in the session FSM backs them.
- ~~One consistent treatment across screens.~~ Landed: the four palette slots
  that were approved and then never wired (`DIM`/`BORDER`/`SELECT`/`SPARE`)
  now have names, every raw `TFT_*` literal in `main.cpp` goes through the
  palette, and `heading()`/`hint()`/`restoreText()` give the idle, class
  select and nearby-players screens one shared title and key-hint treatment
  instead of the three they had grown independently. `SPARE` is still unused
  — it is a spare, not a gap. Checked on a real panel, which settles the one
  open question from `4a4373c`: `BORDER` at 0x4208 is dark, but the bars stay
  legible against black, so the contrast drop from white is fine as chosen.
- ~~A standard 16-color palette.~~ Landed (`37f1077`, `492fcf7`):
  `tools/designs/palette.json` is a labeled legend draft (16 swatches, one
  per intended use) approved as-is and wired into named `CardputerUi`
  constants — `kHpFullColor`/`kHpMidColor`/`kHpLowColor`/`kMpColor`/
  `kClassColor[]`. Two slots are now genuinely in use beyond the original
  red/green/blue: HP_MID gives the HP bar a third tier (yellow, 25–50%,
  between full and low), and each combatant's name is tinted by class
  (BUNYAN/DRIFTER/COYOTE/VOODOO). Still open: `HEAL_FX`/`HIT_FX` (drafted,
  not consumed by anything yet — see the animations bullet below). `DIM`,
  `BORDER` and `SELECT` were wired later, in the uniformity pass above; only
  `SPARE` is still unclaimed, which is what a spare is for.
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
- ~~**Post-match rematch prompt + host/joiner indicator.**~~ Landed, straight
  from the first on-hardware class-pick playtest: `LS_OVER` was `q`-only
  (full reset back to the host/join menu, re-picking role and class every
  time) and nothing on screen showed which unit was hosting. Added `r`
  (`Session::rematchKeepingRole()` — captures `isHost_`/`myClassId_` before
  `rematch()` clears them, then re-enters via the existing `startHosting`/
  `startJoining`) alongside the untouched `q` path, a `"R=rematch  Q=menu"`
  prompt on the verdict screen, and a persistent HOST/JOIN corner tag
  (`Frame::~Frame()`, same pattern as the existing `v%u` version stamp).
  UI/session-orchestration only, no `PROTO_VERSION` bump. Verified: `make -C
  test` and `pio run` both green.
- **The HP number is legible, not good.** Reported from play: white at size 1
  over HP-FULL green was unreadable at full health. Outlining the glyphs and
  raising them to `kBodyTextSize` fixed the reported problem — verified on
  Unit 1, and the verdict was "works, wouldn't say it's fantastic", which is
  recorded here as-is rather than as a win. The real constraint is that the
  number sits *inside* the bar at all, which was only ever a response to the
  135px panel having nowhere else to put it; an outlined label crossing a
  moving fill boundary is a workaround for that, not a design. MP is still at
  size 1 for the same reason — its 10px bar has an 8px interior and the
  height budget in `drawCombatants()` is at 132 of 135. Doing this properly
  means re-cutting that budget: a shorter format (`100` over `100/100`), or
  moving the numbers back out to the name line now that it is no longer
  carrying them. Not attempted — it is a layout rework, not a tweak.
- ~~**The corner tag overdraws the flee line in a real match.**~~ Fixed: found
  by review, invisible in practice mode (which runs at `LS_IDLE`, where the
  bottom-left tag is not drawn), so it had survived every playtest. In
  `LS_MY_TURN` the footer's third line occupies rows 120-131 while
  `Frame::~Frame()` drew the `HOST`/`JOIN` tag opaquely at rows 127-134,
  cutting the bottom off `5)Flee - FORFEITS`. The tag is now suppressed for
  the duration of a turn rather than the layout shrunk — which side challenged
  is the least useful thing on screen mid-match, and finding 8px in a layout
  with 3px of slack would have cost real information. The budget comment in
  `drawCombatants()` now states that it counts content only and that the real
  ceiling is row 127, not 135.
- **The class-select lines wrap mid-word.** 240px at 9px/glyph is 26 characters
  (`kPanelW`), and three of the four blurb lines are 30-33, so LovyanGFX wraps
  them per character: the menu renders as seven ragged rows rather than four,
  with orphans like `urst` and `sher`. Shortening the blurbs is the cheap fix;
  the real one is the same budget rework as the HP-number entry above.
- **Sound.** Unexplored entirely — unclear if the Cardputer ADV's buzzer (if
  any) is even wired in `platformio.ini`'s lib set. Needs a hardware check
  before it's schedulable.
- **Match history / win-loss record.** Still open, but no longer blocked on
  persistence: NVS is wired up (see *Player identity* in group 2), the
  namespace is opened once in `setup()`, and the convention is one
  `loadX()`/`saveX()` pair per stored field. What is missing is the data
  model, not the storage — `rematch()` resets in memory and nothing counts
  wins in the first place.

## 4. Radio / compliance (touches `main.cpp`, hardware-gated)

- ~~The two-radio test itself.~~ Confirmed: host and joiner paired over actual
  RF and played a full match to a verdict both sides agreed on, first at six
  inches, then a clean match at 40-50ft indoors with no dropped input and no
  noticeable lag. Unblocks the items below that were waiting on it. Still open
  from here: real packet loss / rejoin behavior at range or with interference
  — the split-verdict rate in `test/test_session.cpp` is still simulation
  only, and 40-50ft indoors is not a stress test of maximum range,
  obstructions, or RF interference.
- **EU frequency + duty cycle.** `RF_FREQ_MHZ` is hardcoded 915.0; `CLAUDE.md`
  already flags 868.0 + duty-cycle budget as unsettled. Only urgent if a unit
  is going on air outside US/AU.
- **Power management.** No sleep/wake behavior explored; `startReceive()` runs
  continuously. Battery life on a handheld device is presumably a real
  constraint, untested.
- **OTA firmware updates.** Today, updating a unit means USB + `pio run -t
  upload`, which is fine for two units in one room but doesn't scale past
  that. Two candidate paths, unresolved:
  - **WiFi OTA** (`ArduinoOTA` / ESP `Update.h`, `--upload-protocol espota`) —
    the well-trodden ESP32 path. Needs an OTA-capable partition table (two
    app slots to flip between) and pulls in WiFi as a firmware dependency
    this project has otherwise avoided; would only be live during an
    explicit update mode, not during play, so it doesn't compromise the
    "no server, no access point" pitch in `README.md` — just adds a second
    radio stack to the build.
  - **LoRa OTA** — push the firmware image over the same SX1262 link
    already in use. Thematically consistent with the group-5 radio-first
    direction, but LoRa's kbps-range bandwidth makes a multi-hundred-KB
    image slow, and a dropped chunk mid-flash risks a bricked unit without
    a real chunking/resume/integrity protocol. Bigger lift, own design pass.
  - Explicitly asked for as **reusable infrastructure beyond this
    project** — not scoped as Hill-Top-Heros-specific, so whichever path is
    picked should stay separable from the game logic rather than getting
    wired into `main.cpp` in a one-off way.

## 5. Radio-first mechanics (touches `rpg_session.*`, hardware-gated)

Idea surfaced from outside review: instead of hiding the radio, make its
physical properties — beacons, RSSI, "someone was in range" — part of the
game itself. Unscoped; recorded here so it isn't lost, not because it's next.

All three flavors read from the same underlying mechanism, and **that
mechanism now exists**: `LS_SCANNING` logs `(hostId, commit, RSSI,
lastSeenAt)` per `Sighting` for every `PKT_BEACON` overheard, with expiry, and
no new wire packet types were needed. What's left below is what to *do* with
that log beyond listing it.

- ~~**"Enemy detected — distance ~400m, signal weak."**~~ Largely landed as
  the nearby-players list: a live beacon read back as a bucketed
  RSSI-to-strength label (weak/ok/strong), deliberately not real ranging.
  Thresholds in `CardputerUi::rssiBucket()` are a first guess, never
  calibrated against measured link quality — worth revisiting with real
  distance data before any UI claims more precision than "strong/ok/weak".
- **"A player passed through here 3 hours ago."** Same sighting log, read
  back later instead of live. The NVS layer it was waiting on now exists (see
  *Player identity* in group 2), so what is left is genuinely this feature:
  serializing a ring of sightings rather than a single string, and a wall-clock
  question the name never had — the Cardputer has no RTC, so "3 hours ago"
  needs a time base this project does not currently keep across a reboot.
- **"You intercepted another battle."** The least settled. `PKT_ACTION` /
  `PKT_STATUS` between two other units are unencrypted on the wire, so
  overhearing them is possible, but reconstructing something meaningful
  without the full handshake state (seeds, class ids) is real design work.
  Scope the first version as flavor text only — "a battle happened nearby,"
  not a full spectate — rather than committing to live intercept.

Layering note: this stays inside the existing rule. Passive scanning is
`Transport::recv()` plus an RSSI read, which crosses the hardware boundary
exactly where `Session` already does — new `LS_SCANNING` state and a small
sighting ring buffer, not a new interface.

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
- **Group 5 is not plan-ready.** It needs its own design pass (the passive
  scan state) before any sub-idea can be turned into a task, unlike the rest
  of this document which is mostly gaps in an already-agreed design.
- Nothing here is prioritized against anything else yet — that's a
  conversation for whoever's picking the next task, not something to bake
  into this document.
