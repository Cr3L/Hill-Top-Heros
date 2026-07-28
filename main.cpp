// main.cpp — Cardputer ADV duel prototype.
//
// Glue only. All protocol logic lives in rpg_session.{h,cpp}, which knows
// nothing about this file's hardware and is exercised against a simulated
// lossy channel in test/.
#include <M5Cardputer.h>
#include <RadioLib.h>
#include "rpg_session.h"

// -------------------------------------------------------------- radio setup
// VERIFY THESE PINS against your ADV + LoRa module combination before flashing.
// The values below are placeholders carried over from a generic wiring.
#define PIN_NSS   10
#define PIN_DIO1   2
#define PIN_RST    3
#define PIN_BUSY   4

// VERIFY THIS TOO: 915 MHz is US/AU. In EU you want 868.0 and you must respect
// the 1% duty cycle — at 22 dBm with a 2s beacon interval this is over budget.
#define RF_FREQ_MHZ 915.0

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY);

// DIO1 fires for TxDone as well as RxDone, so this flag alone does not mean a
// packet is waiting. Every transmit clears it again before returning to RX.
static volatile bool rxFlag = false;
IRAM_ATTR void onDio1() { rxFlag = true; }

// ------------------------------------------------------------- adapters

struct RadioTransport : Transport {
  void send(const Packet& p) override {
    radio.transmit((uint8_t*)&p, sizeof(Packet));
    rxFlag = false;                    // swallow our own TxDone interrupt
    radio.startReceive();
  }

  bool recv(Packet& out) override {
    if (!rxFlag) return false;
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

  void status(const char* line) override {
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    d.setCursor(0, 0);
    d.println(line);
  }

  void battle() override {
    if (!s) return;
    const BattleState& b = s->battle();
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    d.setCursor(0, 0);
    d.printf("T%u %s\n", b.turn, s->isHost() ? "HOST" : "GUEST");
    for (int i = 0; i < 2; i++)
      d.printf("%s %d/%d mp%d\n", b.p[i].name, b.p[i].hp, b.p[i].hpMax, b.p[i].mp);
    d.println(s->state() == LS_WAIT_PEER ? "waiting for peer..."
                                         : "1atk 2grd 3skl 4itm");
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
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(2);
  Serial.begin(115200);

  gUi.s = &gSession;

  int st = radio.begin(RF_FREQ_MHZ, 125.0, 7, 5);
  if (st != RADIOLIB_ERR_NONE) {
    M5Cardputer.Display.printf("Radio fail %d", st);
    while (true) delay(100);
  }
  radio.setOutputPower(22);
  radio.setRegulatorMode(RADIOLIB_SX126X_REGULATOR_DC_DC);
  radio.setDio1Action(onDio1);
  radio.startReceive();

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
