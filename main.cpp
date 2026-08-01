// main.cpp — Cardputer ADV duel prototype.
//
// Glue only. All protocol logic lives in rpg_session.{h,cpp}, which knows
// nothing about this file's hardware and is exercised against a simulated
// lossy channel in test/.
#include <M5Cardputer.h>
#include <RadioLib.h>
#include <SPI.h>
// Not re-exported by M5Unified.hpp — only M5Unified.cpp includes it internally.
#include <utility/PI4IOE5V6408_Class.hpp>
#include "rpg_session.h"

// -------------------------------------------------------------- radio setup
// Pinout for the official Cap LoRa-1262 on the Cardputer ADV, per M5's Arduino
// tutorial for the cap (which instantiates Module(5, 4, 3, 6) in this order).
#define PIN_NSS    5
#define PIN_DIO1   4
#define PIN_RST    3
#define PIN_BUSY   6

// SPI is shared with the SD slot. These MUST be passed to SPI.begin()
// explicitly: RadioLib falls back to the board variant's default pins, and the
// m5stack_stamp_s3 variant we build against defines SCK/MISO/MOSI as -1, so the
// radio would otherwise sit on a bus wired to nothing.
#define PIN_SCK   40
#define PIN_MISO  39
#define PIN_MOSI  14

// VERIFY THIS: 915 MHz is US/AU. In EU you want 868.0 and you must respect
// the 1% duty cycle — at 22 dBm with a 2s beacon interval this is over budget.
#define RF_FREQ_MHZ 915.0

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);

// The Cap LoRa-1262 routes its antenna through a PI4IOE5V6408 expander on the
// ADV's internal I2C bus. P0 must be driven high before the radio is used or
// nothing reaches the air — begin() still succeeds, which makes this the most
// misleading failure available. Not present on the plain Cap LoRa868.
// Defaults are already address 0x43 at 400kHz on In_I2C, which is what the cap
// uses, so there is nothing to override here.
static m5::PI4IOE5V6408_Class gIoe;

// Set once in setup(). When the radio did not come up we stay usable as a
// UI-only build rather than halting: the session simply never hears a peer,
// which is an ordinary timeout path it already handles.
static bool gRadioOk = false;

// DIO1 fires for TxDone as well as RxDone, so this flag alone does not mean a
// packet is waiting. Every transmit clears it again before returning to RX.
static volatile bool rxFlag = false;
IRAM_ATTR void onDio1() { rxFlag = true; }

// ------------------------------------------------------------- adapters

struct RadioTransport : Transport {
  void send(const Packet& p) override {
    if (!gRadioOk) return;
    radio.transmit((uint8_t*)&p, sizeof(Packet));
    rxFlag = false;                    // swallow our own TxDone interrupt
    radio.startReceive();
  }

  bool recv(Packet& out) override {
    if (!gRadioOk || !rxFlag) return false;
    rxFlag = false;
    if (radio.getPacketLength() != sizeof(Packet)) {   // TxDone, or a stray
      radio.startReceive();
      return false;
    }
    int st = radio.readData((uint8_t*)&out, sizeof(Packet));
    radio.startReceive();
    return st == RADIOLIB_ERR_NONE;    // the session checks CRC and version
  }
};

struct ArduinoClock : Clock {
  uint32_t now() override { return millis(); }
};

struct CardputerUi : SessionUi {
  Session* s = nullptr;
  // Bring-up diagnostic. Serial alone is not enough: setup() prints before USB
  // CDC has enumerated, so the radio's return code — the most useful number
  // here — is usually gone before a monitor can attach. On screen it survives.
  const char* note = nullptr;

  // Off-screen buffer. Every redraw clears the screen and draws it again, so
  // straight to the panel it is a visible black flash — worst on the battle
  // screen, which redraws every turn. Composited here and pushed in one go.
  M5Canvas canvas{&M5Cardputer.Display};
  // False if the sprite could not be allocated; Frame then draws straight to
  // the panel. Not merely an optimisation to skip: a failed createSprite leaves
  // the clip rect empty, so drawing into it succeeds and shows nothing. A
  // flickering screen beats a blank one.
  bool buffered = false;

  // HP as last drawn, per side. -1 means "not seen yet" so the first draw of
  // a fresh match never flashes. Compared against on every battle() call to
  // flash a row whose HP just changed.
  int16_t lastHp[2] = {-1, -1};

  // Most recent nonzero HP change per side, held until the next one — this is
  // what makes "what just happened" readable after the single-frame flash has
  // already passed. 0 doubles as "no change seen yet": a delta is only ever
  // stored when hp actually differs, so a stored 0 can only mean unset, never
  // a real zero-change turn.
  int16_t lastDelta[2] = {0, 0};

  // Call once, after the panel's own text setup — the style is copied from it.
  bool beginDisplay() {
    // No PSRAM on this board (see platformio.ini), and M5Canvas's parent-taking
    // constructor opts into it, so this has to be turned back off explicitly.
    canvas.setPsram(false);
    // 8bpp (rgb332), not 16: 32KB of internal heap rather than 64KB. The panel
    // pins its own write depth to rgb565 either way, so the wider sprite would
    // cost RAM and save nothing on the bus.
    canvas.setColorDepth(8);
    // Sized from the panel, not from literals: the canvas has to match whatever
    // rotation M5GFX settled on, and a mismatch is a push that silently clips.
    buffered = canvas.createSprite(M5Cardputer.Display.width(),
                                   M5Cardputer.Display.height()) != nullptr;
    // Copied wholesale rather than re-specified, so size, colour and datum
    // cannot drift from the panel's — the buffered path is the one that renders.
    canvas.setTextStyle(M5Cardputer.Display.getTextStyle());
    return buffered;
  }

  // A whole frame, pushed on scope exit. The pairing is RAII rather than two
  // calls because a missed push is invisible: the screen simply stops updating,
  // which reads as a protocol fault rather than a drawing one.
  struct Frame {
    LovyanGFX& g;
    CardputerUi& ui;
    explicit Frame(CardputerUi& u)
      // LovyanGFX is the base M5GFX and M5Canvas share. Casting one arm is
      // enough; the conditional operator converts the other implicitly.
      : g(u.buffered ? static_cast<LovyanGFX&>(u.canvas) : M5Cardputer.Display),
        ui(u) {
      g.fillScreen(TFT_BLACK);
      g.setCursor(0, 0);
    }
    ~Frame() {
      // PROTO_VERSION, not a separate game-version scheme — the two units get
      // reflashed independently all evening, and this is the one number that
      // actually determines whether they can pair at all. Small and in the
      // corner: a sanity check on every screen, not something to compete with
      // the battle HUD for attention.
      g.setTextSize(1);
      g.setTextDatum(textdatum_t::bottom_right);
      char v[8];
      snprintf(v, sizeof(v), "v%u", PROTO_VERSION);
      g.drawString(v, g.width(), g.height());
      g.setTextDatum(textdatum_t::top_left);
      g.setTextSize(ui.kBodyTextSize);
      if (ui.buffered) ui.canvas.pushSprite(0, 0);
    }
    // A copy would push the same frame twice. Nothing does today; this is here
    // so nothing can.
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    LovyanGFX* operator->() { return &g; }
  };

  void status(const char* line) override {
    Frame d(*this);
    // Idle only: title (tools/designs/title_screen.json) over the menu
    // (tools/designs/main_menu.json), so booting doesn't drop straight into
    // what reads like a diagnostic screen. The two mockups are separate
    // drafts but there is only one LS_IDLE state to draw them from, so both
    // land in this one call — title on top, the (real, Host/Join-only) menu
    // below it.
    if (s && s->state() == LS_IDLE) {
      // A rematch rerolls classes, so the previous match's HP is meaningless
      // here — reset so the new match's first draw doesn't flash on a value
      // that was never actually a change.
      lastHp[0] = lastHp[1] = -1;
      lastDelta[0] = lastDelta[1] = 0;

      d.g.setTextDatum(textdatum_t::top_center);
      d.g.drawString("HILL-TOP HEROS", kPanelW / 2, 40);
      d.g.setTextDatum(textdatum_t::top_left);
      d->setCursor(12, 76);
      d->println(line);
      d->println("P=practice (1 player)");
      if (note) {
        d->setCursor(12, 112);
        d->println(note);
      }
      return;
    }
    d->println(line);
  }

  // The panel's usable width after rotation — see "Display" in README.md.
  // Also the fact behind "26 chars" below: 240px / 9px per glyph at
  // kBodyTextSize. Named once so hpBar and that line can't quietly drift.
  static constexpr int kPanelW = 240;

  // Squeezed down from size 2 (12x16px glyphs) to make room for the HP/MP
  // bars without losing the footer menu off the bottom of a 135px panel.
  // Applies everywhere text is drawn, not just the battle screen, so the
  // whole UI stays one consistent scale.
  static constexpr float kBodyTextSize = 1.5f;
  // Bar heights, tall enough to hold a centered size-1 (8px) label with a
  // little padding — HP gets more breathing room since its number matters
  // more moment to moment; MP is tight on purpose (see the height budget
  // note below).
  static constexpr int kHpBarH = 14;
  static constexpr int kMpBarH = 10;

  // Source of truth: tools/designs/palette.json. Values that happen to equal
  // a stock TFT_* macro (HP_LOW == TFT_RED, MP == TFT_BLUE, ...) are still
  // named from the palette rather than left as the macro, so this list stays
  // the one place that has to change if the palette is retuned.
  static constexpr uint16_t kHpFullColor = 0x07E0;  // green
  static constexpr uint16_t kHpMidColor  = 0xFFE0;  // yellow
  static constexpr uint16_t kHpLowColor  = 0xF800;  // red
  static constexpr uint16_t kMpColor     = 0x001F;  // blue

  // BUNYAN/DRIFTER/COYOTE/VOODOO palette slots, indexed by classId. Order
  // has to match the ClassId enum in rpg_link.cpp (CLS_BUNYAN=0 ..
  // CLS_VOODOO=3) by position — that enum isn't exposed in rpg_link.h, so
  // there's no way to reference it here by name. kClassColorCount tracks
  // classCount()'s value today (4), but the two aren't linked: a 5th class
  // (ROADMAP.md group 1 — its own PROTO_VERSION task, not started) needs a
  // 5th entry added here by hand. Bounds-checked against this array's own
  // size below, not against classCount() directly — that would pass a
  // classId this array has no color for and read out of bounds.
  static constexpr uint16_t kClassColor[] = {
    0x8A22,  // Bunyan  -- brown
    0x8800,  // Drifter -- dark red
    0xD5B1,  // Coyote  -- tan
    0x6013,  // Voodoo  -- purple
  };
  static constexpr uint8_t kClassColorCount =
      sizeof(kClassColor) / sizeof(kClassColor[0]);

  // Shared geometry for both bars below: a 1px white border, filled
  // interior inset 1px on every side so the fill never touches the border.
  // Height and fill color are the only things HP and MP disagree on.
  static void statBar(LovyanGFX& g, int y, int h, int val, int valMax, uint16_t fillColor) {
    int fill = valMax > 0 ? (kPanelW * val) / valMax : 0;
    g.drawRect(0, y, kPanelW, h, TFT_WHITE);
    if (fill > 2) g.fillRect(1, y + 1, fill - 2, h - 2, fillColor);
  }

  // The hp/mp numbers live inside the bar itself now rather than on the
  // name line above it, so this always draws in white: full-strength fill
  // colors (green, red, blue) are all chosen to keep white legible against
  // them, and white also reads fine against the plain black background
  // behind the unfilled part of the bar. Transparent (single-arg
  // setTextColor) so the fill/background shows through around the glyphs
  // instead of an opaque box stamping over the bar.
  static void barLabel(LovyanGFX& g, int y, int h, const char* text) {
    g.setTextSize(1);
    g.setTextColor(TFT_WHITE);
    g.setTextDatum(textdatum_t::middle_center);
    g.drawString(text, kPanelW / 2, y + h / 2);
    g.setTextDatum(textdatum_t::top_left);
    g.setTextSize(kBodyTextSize);
  }

  static void hpBar(LovyanGFX& g, int y, int hp, int hpMax) {
    // Three tiers, not two: full-color above 50%, a mid warning down to 25%,
    // then the low-HP red -- the bar itself carries the warning at a glance,
    // same reasoning as the hit/heal row-flash above. hpMax<=0 falls back to
    // full-color, matching the old two-tier code's fallback.
    uint16_t fillColor = kHpFullColor;
    if (hpMax > 0) {
      if (hp * 4 <= hpMax)      fillColor = kHpLowColor;   // <=25%
      else if (hp * 2 <= hpMax) fillColor = kHpMidColor;   // <=50%
    }
    statBar(g, y, kHpBarH, hp, hpMax, fillColor);
    char text[16];
    snprintf(text, sizeof(text), "%d/%d", hp, hpMax);
    barLabel(g, y, kHpBarH, text);
  }

  // Thinner and stacked directly under the HP bar, not given its own text
  // line: the panel has no vertical room to spare (see the height budget
  // note below), and MP swings less dramatically turn to turn than HP
  // does, so it doesn't need the HP bar's full weight.
  static void mpBar(LovyanGFX& g, int y, int mp, int mpMax) {
    statBar(g, y, kMpBarH, mp, mpMax, kMpColor);
    char text[16];
    snprintf(text, sizeof(text), "M %d/%d", mp, mpMax);
    barLabel(g, y, kMpBarH, text);
  }

  // Both HP rows plus the "what just happened" delta line — identical
  // between a real match and practice mode except which slot is "me" and
  // what that slot is called in the delta line. Shared here so the two
  // callers can't quietly drift on the flash/hpBar/layout logic.
  //
  // Height budget on a 135px panel at kBodyTextSize (12px/line): 12 turn
  // banner + 2 * (12 name line + 14 hp bar + 10 mp bar) + 12 delta line +
  // 3 * 12 footer = 132px. 3px of slack. The hp/mp numbers used to be text
  // on the name line; moving them into the bars is what bought this room
  // back after the HP bar's height was doubled.
  void drawCombatants(Frame& d, const BattleState& b, int me, const char* meLabel) {
    for (int i = 0; i < 2; i++) {
      // Flash (inverted colors) the row whose HP moved since the last draw —
      // hit or heal, no separate visual language for a first cut. Lasts only
      // until the next redraw, since battle()/practiceBattle() fire on every
      // poll or keypress, not just on turn resolution; a proper timed flash
      // would need its own clock, not attempted here.
      bool changed = lastHp[i] >= 0 && b.p[i].hp != lastHp[i];
      if (changed) {
        lastDelta[i] = b.p[i].hp - lastHp[i];
        d.g.setTextColor(TFT_BLACK, TFT_WHITE);
      } else {
        // Class tint on the name itself, rather than relying on whatever
        // color barLabel() last left ambient (it used to, by accident —
        // this makes it explicit). Falls back to white for an out-of-range
        // classId, same defensiveness as classDefOf()'s fallback to class 0.
        uint16_t tint = b.p[i].classId < kClassColorCount
                            ? kClassColor[b.p[i].classId] : TFT_WHITE;
        d.g.setTextColor(tint, TFT_BLACK);
      }
      // '>' marks the player's own row — same info the old code left the
      // player to infer from remembering which class they'd been dealt.
      // HP/MP numbers no longer sit here — see barLabel().
      d->printf("%c%s\n", i == me ? '>' : ' ', b.p[i].name);
      if (changed) d.g.setTextColor(TFT_WHITE, TFT_BLACK);
      lastHp[i] = b.p[i].hp;
      int y = d->getCursorY();
      hpBar(d.g, y, b.p[i].hp, b.p[i].hpMax);
      mpBar(d.g, y + kHpBarH, b.p[i].mp, b.p[i].mpMax);   // flush, no gap
      d->setCursor(0, y + kHpBarH + kMpBarH);   // flush before next line too
    }

    // What just happened, held over from the last resolved turn rather than
    // shown only for the one frame the flash above lasts — this is what
    // makes it possible to still tell what happened while thinking through
    // the next move.
    if (lastDelta[0] || lastDelta[1]) {
      d->print(' ');
      if (lastDelta[0]) d->printf("%s%+d  ", me == 0 ? meLabel : "Opp", lastDelta[0]);
      if (lastDelta[1]) d->printf("%s%+d", me == 1 ? meLabel : "Opp", lastDelta[1]);
      d->println();
    } else {
      d->println();  // keep the layout stable turn 0, before anything's happened
    }
  }

  void battle() override {
    if (!s) return;
    const BattleState& b = s->battle();
    int me = s->isHost() ? 0 : 1;
    Frame d(*this);

    // Turn banner: the single strongest signal for "whose move is it right
    // now", inverted so it reads at a glance instead of by parsing text.
    bool myMove = s->state() == LS_MY_TURN;
    d.g.setTextColor(TFT_BLACK, TFT_WHITE);
    d->printf("T%-3u%s\n", b.turn, myMove ? "YOUR MOVE" : "OPP'S MOVE...");
    d.g.setTextColor(TFT_WHITE, TFT_BLACK);

    drawCombatants(d, b, me, "You");

    // Full words + bracketed keys over the old single dense line: the thing
    // "button smashing" actually meant was not being able to read the menu
    // fast enough to trust a keypress, not that the keys themselves changed.
    if (s->state() == LS_WAIT_PEER) {
      d->println("Waiting for OPP...");
    } else {
      d->println("1)Attack  2)Guard");
      d->printf("3)Skill   4)Item x%d\n", b.p[me].items);
      d->println("5)Flee - FORFEITS");
    }
  }

  void log(const char* msg) override { Serial.println(msg); }

  // Local single-device demo: renders straight from a BattleState the caller
  // owns in main.cpp, bypassing Session/Transport entirely. The human is
  // always slot 0 — there's no host/joiner concept with one device — and
  // there's no "waiting on peer" state since the bot resolves instantly.
  void practiceBattle(const BattleState& b, int winner) {
    Frame d(*this);
    d->printf("PRACTICE T%-3u\n", b.turn);

    drawCombatants(d, b, 0, "You");

    if (winner >= 0) {
      const char* msg = winner == 0 ? "YOU WIN" : winner == 1 ? "YOU LOSE" : "DRAW";
      d->printf("%s - Q=menu\n", msg);
    } else {
      d->println("1)Attack  2)Guard");
      d->printf("3)Skill   4)Item x%d\n", b.p[0].items);
      d->println("5)Flee - FORFEITS");
    }
  }
};

// Out-of-class definition: this build's C++ standard (Arduino-ESP32's
// default, older than the host tests' -std=c++17) doesn't treat a static
// constexpr array member as implicitly inline, so indexing it in
// drawCombatants() odr-uses it and the linker needs a home for it.
constexpr uint16_t CardputerUi::kClassColor[];

static RadioTransport gTransport;
static ArduinoClock   gClock;
static CardputerUi    gUi;
static Session        gSession(gTransport, gClock, gUi);

// ----------------------------------------------------------- practice mode
// Single-device demo, no radio/Session involved: a bot plays slot 1 against
// battleInit()/battleResolve() directly, the same pure sim the real match
// uses. Exists so one person can see a full battle without a second unit.
static bool        gPracticeOn = false;
static BattleState gPracticeBattle{};
// Bot decisions only, never the battle rng — mixing the two would desync a
// real match if this code path ever ran during one (it can't: gPracticeOn
// and gSession are mutually exclusive), but keeping the streams separate is
// the rule this file follows everywhere else, not a special case for this.
static Rng gPracticeBotRng;

static ActionId practiceBotChoose(const BattleState& b) {
  const Combatant& me = b.p[1];
  if (me.hp * 3 < me.hpMax && me.items) return ACT_ITEM;    // heal when low
  if (me.mp >= skillCostOf(me) && gPracticeBotRng.range(0, 1) == 0)
    return ACT_SKILL;
  return ACT_ATTACK;
}

// --------------------------------------------------------------------- main

static uint32_t deviceId() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t id = (uint32_t)(mac ^ (mac >> 32));
  return id ? id : 1;                  // 0 is reserved for broadcast
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  // No setRotation() here: M5GFX autodetects the ADV panel and its rotation
  // itself (see "Display" in README.md). Setting it by hand fights the
  // library rather than configuring it.
  M5Cardputer.Display.setTextSize(CardputerUi::kBodyTextSize);
  Serial.begin(115200);

  if (!gUi.beginDisplay())
    Serial.println("canvas alloc failed - drawing direct, expect flicker");

  gUi.s = &gSession;

  // Antenna switch first — see the note on gIoe above for why the order here
  // is load-bearing rather than incidental.
  bool ioeOk = gIoe.begin();
  if (ioeOk) {
    gIoe.setDirection(0, true);        // P0 as output
    gIoe.setHighImpedance(0, false);
    gIoe.digitalWrite(0, true);        // enable the RF antenna switch
  } else {
    // Almost always a cap that is not seated. Worth saying out loud, because
    // every later symptom would otherwise be blamed on the radio or protocol.
    Serial.println("PI4IOE 0x43 not found - Cap LoRa-1262 seated?");
  }

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

  int st = radio.begin(RF_FREQ_MHZ, 125.0, 7, 5);
  gRadioOk = (st == RADIOLIB_ERR_NONE);
  if (gRadioOk) {
    radio.setOutputPower(22);
    // setRegulatorMode() is protected in RadioLib 7.x; this is the public form.
    radio.setRegulatorDCDC();
    radio.setDio1Action(onDio1);
    radio.startReceive();
  }
  Serial.printf("radio.begin=%d ioe=%d\n", st, (int)ioeOk);

  // Static: gUi.note holds this pointer for the lifetime of the program.
  static char note[32];
  snprintf(note, sizeof(note), "radio=%d ioe=%d", st, (int)ioeOk);
  gUi.note = note;

  gSession.begin(deviceId(), esp_random());   // hw RNG, only used pre-match
}

void loop() {
  M5Cardputer.update();
  if (!gPracticeOn) gSession.poll();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto ks = M5Cardputer.Keyboard.keysState();
    for (auto c : ks.word) {
      if (gPracticeOn) {
        int winner = battleWinner(gPracticeBattle);
        if (winner >= 0) {
          if (c == 'q') { gPracticeOn = false; gUi.status("H=host  J=join"); }
        } else if (c >= '1' && c <= '5') {
          ActionId my = (ActionId)(c - '0');
          battleResolve(gPracticeBattle, my, practiceBotChoose(gPracticeBattle));
          gUi.practiceBattle(gPracticeBattle, battleWinner(gPracticeBattle));
        }
        break;
      }

      LinkState before = gSession.state();
      if (gSession.state() == LS_OVER) {
        if (c == 'q') gSession.rematch(esp_random());
      } else if (gSession.state() == LS_IDLE && c == 'p') {
        battleInit(gPracticeBattle, esp_random());
        gPracticeBotRng.seed(esp_random());
        gUi.lastHp[0] = gUi.lastHp[1] = -1;
        gUi.lastDelta[0] = gUi.lastDelta[1] = 0;
        gPracticeOn = true;
        gUi.practiceBattle(gPracticeBattle, -1);
        break;  // entering practice mode is a transition too, even though
                // gSession's own state doesn't move
      } else {
        gSession.onKey(c);
      }
      if (gSession.state() != before) break;  // one transition per keypress
    }
  }
}
