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
    ~Frame() { if (ui.buffered) ui.canvas.pushSprite(0, 0); }
    // A copy would push the same frame twice. Nothing does today; this is here
    // so nothing can.
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    LovyanGFX* operator->() { return &g; }
  };

  void status(const char* line) override {
    Frame d(*this);
    d->println(line);
    // Idle only. It is a boot diagnostic, not something to carry into a match.
    if (note && s && s->state() == LS_IDLE) d->println(note);
  }

  // HP as a filled rect below each combatant's line. Text stays alongside it —
  // the exact number still drives decisions (a skill's mp cost, whether a hit
  // is lethal) — the bar is only there to make that number readable at a
  // glance instead of read digit by digit.
  static void hpBar(LovyanGFX* g, int y, int hp, int hpMax) {
    const int x = 0, w = 240, h = 4;
    int fill = hpMax > 0 ? (w * hp) / hpMax : 0;
    g->drawRect(x, y, w, h, TFT_WHITE);
    if (fill > 0) g->fillRect(x + 1, y + 1, fill - 2 > 0 ? fill - 2 : 0, h - 2, TFT_WHITE);
  }

  void battle() override {
    if (!s) return;
    const BattleState& b = s->battle();
    Frame d(*this);
    d->printf("T%u %s\n", b.turn, s->isHost() ? "HOST" : "GUEST");
    for (int i = 0; i < 2; i++) {
      d->printf("%s %d/%d mp%d\n", b.p[i].name, b.p[i].hp, b.p[i].hpMax, b.p[i].mp);
      hpBar(d.operator->(), d->getCursorY(), b.p[i].hp, b.p[i].hpMax);
      d->setCursor(0, d->getCursorY() + 6);   // bar height + a gap before next line
    }
    // Item charges are finite, so the count has to be visible or the player is
    // guessing. Trailing digit keeps this at 20 chars, the width of the panel
    // at text size 2 — see "Display" in README.md.
    if (s->state() == LS_WAIT_PEER) d->println("waiting for peer...");
    else d->printf("1atk 2grd 3skl 4itm%d\n", b.p[s->isHost() ? 0 : 1].items);
  }

  void log(const char* msg) override { Serial.println(msg); }
};

static RadioTransport gTransport;
static ArduinoClock   gClock;
static CardputerUi    gUi;
static Session        gSession(gTransport, gClock, gUi);

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
  M5Cardputer.Display.setTextSize(2);
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
  gSession.poll();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto ks = M5Cardputer.Keyboard.keysState();
    for (auto c : ks.word) {
      LinkState before = gSession.state();
      if (gSession.state() == LS_OVER) {
        if (c == 'q') gSession.rematch(esp_random());
      } else {
        gSession.onKey(c);
      }
      if (gSession.state() != before) break;  // one transition per keypress
    }
  }
}
