#include "SdCard.h"

#include "SdSpiDisk.h"
#include <SPI.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../audio/AudioEngine.h"
#include "../audio/SampleStore.h"
#include "../audio/SdStream.h"
#include "../server/WebDebugConsole.h"
#include "../server/ServerCore.h"       // nidmi_ws_pousser: the board ANNOUNCES the card's state

namespace SdCard {
namespace {

uint8_t _cs = 255, _sck = 255, _miso = 255, _mosi = 255;
volatile bool     _declared = false;
volatile bool     _mounted  = false;
volatile uint64_t _capacity = 0;
uint32_t          _hz       = DEFAULT_FREQUENCY_HZ;

/* WHAT IS LEFT TO DO, set by those who may not wait. The task reads these flags;
 * whoever raises one (web server, boot-time restoration) restarts it right away. */
volatile bool     _wantUnmount = false;
volatile bool     _wantMount   = false;
volatile bool     _wantLoad    = false;
volatile bool     _wantMeasure = false;
volatile bool     _wantWiring  = false;
volatile bool     _wantHeads   = false;
volatile uint32_t _taskAlive   = 0;         // the task lives: only one at a time
volatile uint32_t _lastAttemptMs = 0;
volatile uint32_t _attempts    = 0;
/* THE CARD'S LIFE. "lost": it WAS mounted and its reads now fail in a row (pulled out,
 * lost contact) — until it is mounted again. */
volatile bool     _lost        = false;
volatile uint32_t _lostCount   = 0;         // times it was lost since boot
volatile uint32_t _remounts    = 0;         // times it came back after a loss
constexpr uint32_t RETRY_MS    = 5000;      // between two mounting attempts, headless
/* PROBES sector-0 reads, PROBE_GAP_MS apart, that must ALL fail to declare the card lost — about a
 * second of silence from it. Shorter would unmount a card that comes back 0.3 s later and kill
 * the streams the ring (0.74 s) and the reader's retries would have carried through. */
constexpr uint8_t  PROBES      = 5;
constexpr uint32_t PROBE_GAP_MS = 250;
volatile bool     _wantCheck   = false;     // a read failed: is the card still there?

/* THE DIAGNOSTICS, written by the task, read by the web server (diagnostics()).
 * Plain fields: a slightly stale read breaks nothing. */
char          _reason[112] = "";           // why the last mount failed
volatile int  _cmd0 = -1;                  // R1 of CMD0 (0x01: the card answers), -1: not probed
volatile int  _cmd8 = -1;                  // R1 of CMD8
volatile uint32_t _cmd8Echo = 0;           // the 4 bytes after it (0x000001AA: SD v2)
volatile int  _misoLow = -1, _misoHigh = -1;   // MISO level at rest, pulled low / high
volatile int  _swappedCmd0 = -1;           // R1 of CMD0 with MISO and MOSI swapped
volatile int  _bbCmd0 = -1, _bbCmd8 = -1, _bbSwappedCmd0 = -1;   // the same, without the SPI peripheral
volatile bool _wiringDone = false;
/* THE RAW TRACE of the by-hand initialisation: the 12 bytes read after each
 * command, in hexadecimal. `[0]` CMD0, `[1]` CMD8, `[2]` CMD58, `[3]` CMD55,
 * `[4]` ACMD41 (first round), `[5]` ACMD41 (last round). */
char          _raw[6][40];
volatile int  _acmd41Rounds = -1;
volatile int  _acmd41R1 = -1;
/* The card's four pins, measured alone (CS, SCK, MISO, MOSI): read pulled low,
 * pulled high, then read back after DRIVING them to 0 and to 1. A pin that does
 * not follow what it drives is shorted (to GND or 3V3) or heavily loaded.
 * -1: not measured. */
volatile int8_t _level[4][4] = {{-1,-1,-1,-1},{-1,-1,-1,-1},{-1,-1,-1,-1},{-1,-1,-1,-1}};

/* THE READ MEASUREMENT. State: 0 never, 1 running, 2 finished. The JSON is
 * posted in one go, the state last. */
volatile uint8_t _measState = 0;
char     _measJson[1024] = "";
char     _measName[SampleStore::NOM_MAX] = "";
uint32_t _measHz = 0;

/* Task stack: the FAT mount, an array of 24 names (1.1 KB), the Strings of the
 * read and a measurement. Transient — given back when the task dies. */
constexpr uint32_t STACK_BYTES = 8192;

void _startTask();

void _unmount() {
  if (!_mounted) return;
  _mounted = false;
  _capacity = 0;
  if (SdSpiDisk::frequency() && SdSpiDisk::frequency() < _hz) _hz = SdSpiDisk::frequency();   // a fallback is kept: the wire has not improved
  SdStream::stopAll();                   // no open file under the unmount
  SdSpiDisk::unmount();
  NIDMI_WEB_LOG("[SD] demontee");
}

/* AN SD COMMAND BY HAND: 0xFF lead-in, six bytes, then up to ten bytes waiting for
 * the R1 reply (bit 7 drops). `tr` exchanges one byte — the hardware SPI, or the
 * "by hand" probe (bit-bang). */
template <typename T> uint8_t _commandT(T tr, uint8_t cmd, uint32_t arg, uint8_t crc) {
  tr(0xFF);
  tr(0x40 | cmd);
  tr((uint8_t)(arg >> 24)); tr((uint8_t)(arg >> 16));
  tr((uint8_t)(arg >> 8));  tr((uint8_t)arg);
  tr(crc);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && (r & 0x80); i++) r = tr(0xFF);
  return r;
}

/* PROBE at 400 kHz or less, as a card initialisation does: 80 pulses with CS
 * high, CMD0 (back to idle), CMD8 (SD v2). What the SD library knows but does not
 * say: how far the conversation goes. */
template <typename T> void _probeT(T tr, int& r0, int& r8, uint32_t& echo) {
  r0 = r8 = -1; echo = 0;
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  for (int i = 0; i < 10; i++) tr(0xFF);
  digitalWrite(_cs, LOW);
  r0 = _commandT(tr, 0, 0, 0x95);
  digitalWrite(_cs, HIGH); tr(0xFF);
  digitalWrite(_cs, LOW);
  const uint8_t v8 = _commandT(tr, 8, 0x1AA, 0x87);
  if (!(v8 & 0x80) && !(v8 & 0x04)) for (int i = 0; i < 4; i++) echo = (echo << 8) | tr(0xFF);
  digitalWrite(_cs, HIGH); tr(0xFF);
  r8 = v8;
}

void _probeOn(SPIClass& b, int& r0, int& r8, uint32_t& echo) {
  b.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  _probeT([&](uint8_t o) { return b.transfer(o); }, r0, r8, echo);
  b.endTransaction();
}

/* THE SAME EXCHANGE WITHOUT THE SPI PERIPHERAL: the pins are toggled by hand, mode
 * 0 (data set, clock rising edge, MISO sampled). If the card answers this way and
 * not through the hardware SPI, the fault is in how we use the SPI; if it does not
 * answer either, it is in the wiring. `sckPin`/`misoPin`/`mosiPin`: the real pin
 * of each role (swappable). */
void _probeByHand(uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin, int& r0, int& r8, uint32_t& echo) {
  pinMode(sckPin, OUTPUT);  digitalWrite(sckPin, LOW);
  pinMode(mosiPin, OUTPUT); digitalWrite(mosiPin, HIGH);
  pinMode(misoPin, INPUT_PULLUP);
  auto tr = [&](uint8_t o) -> uint8_t {
    uint8_t r = 0;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(mosiPin, (o >> i) & 1);
      delayMicroseconds(3);
      digitalWrite(sckPin, HIGH);
      delayMicroseconds(3);
      r = (uint8_t)((r << 1) | (digitalRead(misoPin) ? 1 : 0));
      digitalWrite(sckPin, LOW);
    }
    return r;
  };
  _probeT(tr, r0, r8, echo);
  pinMode(sckPin, INPUT); pinMode(mosiPin, INPUT); pinMode(misoPin, INPUT);
}

/* One command, then the 12 bytes that follow, kept raw. Returns the first valid R1
 * (bit 7 low), 0xFF if none. */
int _rawCmd(uint8_t cmd, uint32_t arg, uint8_t crc, char* hex) {
  SPI.transfer(0xFF);
  SPI.transfer(0x40 | cmd);
  SPI.transfer((uint8_t)(arg >> 24)); SPI.transfer((uint8_t)(arg >> 16));
  SPI.transfer((uint8_t)(arg >> 8));  SPI.transfer((uint8_t)arg);
  SPI.transfer(crc);
  int r1 = 0xFF;
  for (int i = 0; i < 12; i++) {
    const uint8_t o = SPI.transfer(0xFF);
    if (hex) snprintf(hex + i * 3, 4, "%02X ", (unsigned)o);
    if (r1 == 0xFF && !(o & 0x80)) r1 = o;
  }
  return r1;
}

/* THE BY-HAND INITIALISATION, to see how far the card gets: CMD0, CMD8, CMD58
 * (OCR), then CMD55 + ACMD41 in a loop (HCS, 1 s at most) — what the library
 * does, keeping what the card says instead of a bare failure. Card not mounted. */
void _probeRaw() {
  for (auto& h : _raw) h[0] = 0;
  _acmd41Rounds = 0; _acmd41R1 = -1;
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 20; i++) SPI.transfer(0xFF);
  digitalWrite(_cs, LOW);
  _rawCmd(0, 0, 0x95, _raw[0]);
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
  _rawCmd(8, 0x1AA, 0x87, _raw[1]);
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
  _rawCmd(58, 0, 0x01, _raw[2]);
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
  const uint32_t t0 = millis();
  int r = 0xFF;
  do {
    digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
    _rawCmd(55, 0, 0x01, _acmd41Rounds == 0 ? _raw[3] : nullptr);
    digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
    r = _rawCmd(41, 0x40000000, 0x01, _acmd41Rounds == 0 ? _raw[4] : _raw[5]);
    _acmd41Rounds = _acmd41Rounds + 1;
    delay(10);
  } while (r == 0x01 && millis() - t0 < 1000);
  _acmd41R1 = r;
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF);
  SPI.endTransaction();
  NIDMI_WEB_LOG("[SD] init a la main : ACMD41 -> 0x%02X apres %d tour(s) ; CMD8 brut : %s",
                (unsigned)_acmd41R1, (int)_acmd41Rounds, _raw[1]);
}

void _probe() {
  int r0, r8; uint32_t echo;
  _probeOn(SPI, r0, r8, echo);
  _cmd0 = r0; _cmd8 = r8; _cmd8Echo = echo;
  NIDMI_WEB_LOG("[SD] sonde du bus : CMD0 -> 0x%02X, CMD8 -> 0x%02X (0x%08lX)",
                (unsigned)_cmd0, (unsigned)_cmd8, (unsigned long)echo);
}

/* THE WIRING PROBE, on demand, card not mounted. Three questions that CMD0 at
 * 0xFF does not settle:
 *   - MISO: at rest, pulled low then pulled high. If it follows the pull nothing
 *     holds it — wire not connected, or module unpowered without its own pull;
 *     if it stays high despite the pull-down, something holds it (module powered).
 *   - MISO and MOSI SWAPPED: a second bus (HSPI) with the two data wires
 *     exchanged — the classic DI/DO vs MOSI/MISO confusion. A card that answers
 *     this way is wired backwards.
 * The main bus is closed during the probe, then reopened. */
void _measurePin(uint8_t pin, volatile int8_t* r) {
  pinMode(pin, INPUT_PULLDOWN); delay(3); r[0] = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);   delay(3); r[1] = digitalRead(pin);
  pinMode(pin, OUTPUT);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);   // read it back while it is an output
  digitalWrite(pin, LOW);  delay(3); r[2] = digitalRead(pin);
  digitalWrite(pin, HIGH); delay(3); r[3] = digitalRead(pin);
  pinMode(pin, INPUT);
}

void _probeWiring() {
  SPI.end();
  pinMode(_miso, INPUT_PULLDOWN); delay(3); _misoLow  = digitalRead(_miso);
  pinMode(_miso, INPUT_PULLUP);   delay(3); _misoHigh = digitalRead(_miso);
  pinMode(_miso, INPUT);
  _measurePin(_cs, _level[0]);
  pinMode(_cs, OUTPUT); digitalWrite(_cs, HIGH);     // card deselected during the other measurements
  _measurePin(_sck, _level[1]);
  _measurePin(_miso, _level[2]); _measurePin(_mosi, _level[3]);
  {
    SPIClass swapped(HSPI);
    swapped.begin(_sck, _mosi, _miso, -1);           // the MOSI pin reads, the MISO pin writes
    int r0, r8; uint32_t echo;
    _probeOn(swapped, r0, r8, echo);
    _swappedCmd0 = r0;
    swapped.end();
  }
  {
    int r0, r8; uint32_t echo;
    _probeByHand(_sck, _miso, _mosi, r0, r8, echo);          // by hand, no SPI
    _bbCmd0 = r0; _bbCmd8 = r8;
    _probeByHand(_sck, _mosi, _miso, r0, r8, echo);          // same, MISO/MOSI swapped
    _bbSwappedCmd0 = r0;
  }
  SPI.begin(_sck, _miso, _mosi, -1);
  NIDMI_WEB_LOG("[SD] cablage : MISO bas -> %d, haut -> %d ; SPI MISO/MOSI inverses CMD0 -> 0x%02X ; a la main CMD0 -> 0x%02X, inverses -> 0x%02X",
                _misoLow, _misoHigh, (unsigned)_swappedCmd0, (unsigned)_bbCmd0, (unsigned)_bbSwappedCmd0);
  _wiringDone = true;
}

void _mount() {
  if (!_declared || _mounted) return;
  _lastAttemptMs = millis();
  _attempts++;
  _reason[0] = 0;
  /* The bus first, with its pins and WITHOUT a hardware CS (-1): our SPI-SD driver
   * drives CS itself (digitalWrite). The LIS3DH in SPI mode shares this bus on its
   * own CS — begin() does nothing if it was already opened. */
  SPI.begin(_sck, _miso, _mosi, -1);
  String cause;
  if (!SdSpiDisk::mount(SPI, _cs, _hz, "/sd", cause)) {
    /* It does not answer at all: the raw probe says what the bus sees. It answers
     * but does not initialise: the trace of the by-hand initialisation. */
    if (cause.startsWith("CMD0")) _probe();
    else { _probe(); if (_cmd0 == 0x01) _probeRaw(); }
    snprintf(_reason, sizeof(_reason), "%s", cause.c_str());
    /* Headless, the board retries every RETRY_MS for as long as the card is missing: the
     * console says it for the first attempts, then once a minute (it is a ring, not a log). */
    if (_attempts <= 3 || _attempts % 12 == 0)
      NIDMI_WEB_LOG("[SD] pas de carte (CS=%u SCK=%u MISO=%u MOSI=%u, %lu MHz) : %s",
                    (unsigned)_cs, (unsigned)_sck, (unsigned)_miso, (unsigned)_mosi,
                    (unsigned long)(_hz / 1000000UL), cause.c_str());
    return;
  }
  _capacity = SdSpiDisk::capacityBytes();
  _mounted = true;
  if (_lost) { _lost = false; _remounts = _remounts + 1; NIDMI_WEB_LOG("[SD] carte retrouvee"); }
  NIDMI_WEB_LOG("[SD] montee : %s, %lu Mo, a %lu MHz (CMD8 : 0x%lX)",
                SdSpiDisk::type(), (unsigned long)(_capacity / (1024ULL * 1024ULL)),
                (unsigned long)(_hz / 1000000UL), (unsigned long)SdSpiDisk::cmd8Echo());
  _wantLoad = true;
  _wantHeads = true;                                    // clip heads may have been waiting for the card
}

/* The card's sounds, one by one: each goes through echantillonArrive(), the same
 * path as an uploaded sound — published in the store, and the clips that name it
 * find it. Nothing if the store was never loaded: chargerTout() will call us back
 * when a sound engine needs it. */
void _loadSounds() {
  if (!_mounted || !SampleStore::charge()) return;
  char names[SAMPLES_MAX][SampleStore::NOM_MAX];
  const uint8_t n = SampleStore::sdSoundNames(names, SAMPLES_MAX);
  uint8_t good = 0;
  for (uint8_t i = 0; i < n && _mounted; i++) {
    String reason;
    if (AudioEngine::echantillonArrive(names[i], reason, true)) good++;
    else { SampleStore::noteRefused(names[i], reason.c_str()); NIDMI_WEB_LOG("[SD] %s ignore : %s", names[i], reason.c_str()); }
  }
  NIDMI_WEB_LOG("[SD] %u son(s) precharge(s) sur %u dans %s", (unsigned)good, (unsigned)n, FOLDER);
}

/* THE MEASUREMENT: read a .wav end to end, as the streamed reading would — in
 * 16 KB chunks, into a PSRAM buffer, yielding the CPU the same way between two.
 * Every read is timed: the AVERAGE gives the throughput, the WORST says how much
 * lead a buffer needs. Bounded (12 MB or 12 s). */
void _measure() {
  constexpr size_t   CHUNK = 16384;
  constexpr size_t   MAX_BYTES = 12u * 1024u * 1024u;
  constexpr uint32_t MAX_MS = 12000;
  auto finish = [](const char* error) {
    snprintf(_measJson, sizeof(_measJson), "{\"state\":\"finished\",\"error\":\"%s\"}", error);
    __sync_synchronize();
    _measState = 2;
  };
  /* Another frequency than the bus RUNS at — the driver may have fallen back by itself, `_hz`
   * is only what the next mount will ask for: remount. */
  if (_measHz && _measHz != (_mounted ? SdSpiDisk::frequency() : _hz)) {
    _unmount();
    _hz = _measHz;
    _mount();
  }
  if (!_mounted) return finish("carte non montee (voir le diagnostic)");
  const String path = String(FOLDER) + "/" + _measName;
  File f = SdSpiDisk::open(path.c_str());
  if (!f || f.isDirectory()) return finish("fichier introuvable dans /samples");
  const size_t fileSize = f.size();
  uint16_t channels = 0; uint32_t freq = 0, dataBytes = 0; String reason;
  const bool wav = SampleStore::wavHeader(f, channels, freq, dataBytes, reason);
  if (!wav) { f.seek(0); dataBytes = fileSize; channels = 0; freq = 0; }
  const size_t dataStart = f.position();
  uint8_t* buffer = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
  if (!buffer) { f.close(); return finish("PSRAM"); }

  uint32_t n = 0, over10 = 0, over30 = 0, over100 = 0, maxUs = 0;
  uint64_t sumUs = 0;
  size_t total = 0;
  const uint32_t t0 = millis();
  while (total < dataBytes && total < MAX_BYTES && millis() - t0 < MAX_MS) {
    const uint32_t a = micros();
    const size_t k = f.read(buffer, CHUNK);
    const uint32_t d = micros() - a;
    if (!k) break;
    total += k; n++; sumUs += d;
    if (d > maxUs) maxUs = d;
    if (d > 10000) over10++;
    if (d > 30000) over30++;
    if (d > 100000) over100++;
    vTaskDelay(1);                                   // the real read's CPU yield
  }
  const uint32_t wallMs = millis() - t0;
  uint32_t seekUs = 0;
  if (dataBytes > 2 * CHUNK) {                       // open in the middle: a clip's start
    const uint32_t a = micros();
    f.seek(dataStart + dataBytes / 2);
    f.read(buffer, CHUNK);
    seekUs = micros() - a;
  }
  f.close();
  heap_caps_free(buffer);

  const double raw   = sumUs ? (double)total * 1e6 / (double)sumUs / 1024.0 : 0.0;
  const double real  = wallMs ? (double)total * 1000.0 / (double)wallMs / 1024.0 : 0.0;
  const double needed = (wav && freq) ? (double)freq * channels * 2 / 1024.0 : 0.0;
  snprintf(_measJson, sizeof(_measJson),
           "{\"state\":\"finished\",\"file\":\"%s\",\"bytes\":%u,\"wav\":%s,\"bus_hz\":%lu,\"freq\":%lu,\"channels\":%u,"
           "\"bytes_read\":%u,\"chunk\":%u,\"reads\":%lu,\"avg_us\":%lu,\"max_us\":%lu,"
           "\"over_10ms\":%lu,\"over_30ms\":%lu,\"over_100ms\":%lu,\"middle_seek_us\":%lu,"
           "\"raw_kb_s\":%.0f,\"with_yield_kb_s\":%.0f,\"needed_kb_s\":%.0f,"
           "\"possible_streams\":%.1f}",
           _measName, (unsigned)fileSize, wav ? "true" : "false", (unsigned long)SdSpiDisk::frequency(), (unsigned long)freq,
           (unsigned)channels, (unsigned)total, (unsigned)CHUNK, (unsigned long)n,
           (unsigned long)(n ? sumUs / n : 0), (unsigned long)maxUs,
           (unsigned long)over10, (unsigned long)over30, (unsigned long)over100, (unsigned long)seekUs,
           raw, real, needed, needed > 0 ? real / needed : 0.0);
  __sync_synchronize();
  _measState = 2;
  NIDMI_WEB_LOG("[SD] mesure %s : %.0f Ko/s (%.0f avec cession), pire lecture %lu us, saut %lu us",
                _measName, raw, real, (unsigned long)maxUs, (unsigned long)seekUs);
}

/* A read failed: does the card still answer? A glitch (one sector, a corrupted transfer, an
 * outage of a few hundred ms) passes the probe and nothing happens; a card that does not answer
 * PROBES times in a row is
 * LOST — unmounted here, in the task; the supervisor remounts it. */
void _check() {
  if (!_mounted) return;
  for (uint8_t i = 0; i < PROBES; i++) {
    if (SdSpiDisk::probe()) return;
    vTaskDelay(pdMS_TO_TICKS(PROBE_GAP_MS));
  }
  _lost = true;
  _lostCount = _lostCount + 1;
  NIDMI_WEB_LOG("[SD] carte PERDUE : elle ne repond plus — demontee, nouvelles tentatives toutes les %lu s",
                (unsigned long)(RETRY_MS / 1000));
  _unmount();
}

void _task(void*) {
  for (;;) {
    if (_wantCheck)   { _wantCheck   = false; _check();    continue; }
    if (_wantUnmount) { _wantUnmount = false; _unmount();  continue; }
    if (_wantMount)   { _wantMount   = false; _mount();    continue; }
    if (_wantMeasure) { _wantMeasure = false; _measure();  continue; }
    if (_wantWiring)  { _wantWiring  = false; if (_declared && !_mounted) _probeWiring(); continue; }
    if (_wantHeads)   { _wantHeads   = false; SdStream::loadHeads(); continue; }
    if (_wantLoad)    { _wantLoad    = false; _loadSounds(); continue; }
    break;
  }
  __sync_lock_release(&_taskAlive);
  // A flag raised between the last turn and the release: start again.
  if (_wantCheck || _wantUnmount || _wantMount || _wantLoad || _wantMeasure || _wantWiring || _wantHeads) _startTask();
  vTaskDelete(nullptr);
}

void _startTask() {
  if (__sync_lock_test_and_set(&_taskAlive, 1)) return;      // it is running: it will see the flags
  /* Core 0, priority 1: below WiFi, the TCP/IP stack, the audio (core 1) and the
   * loop — it only runs when nothing else needs the CPU. */
  if (xTaskCreatePinnedToCore(_task, "sd-card", STACK_BYTES, nullptr, 1, nullptr, 0) != pdPASS) {
    __sync_lock_release(&_taskAlive);
    NIDMI_WEB_LOG("[SD] tache impossible (memoire) — la carte ne sera pas montee");
  }
}

}  // namespace

void declare(uint8_t cs, uint8_t sck, uint8_t miso, uint8_t mosi) {
  const bool same = _declared && cs == _cs && sck == _sck && miso == _miso && mosi == _mosi;
  if (same) {
    if (!_mounted) { _wantMount = true; _startTask(); }
    return;
  }
  const bool changed = _declared;                        // other pins: start again from scratch
  _cs = cs; _sck = sck; _miso = miso; _mosi = mosi;
  _declared = true;
  if (changed) _wantUnmount = true;
  _wantMount = true;
  _startTask();
}

void undeclare() {
  _declared = false;
  _cs = _sck = _miso = _mosi = 255;
  _wantMount = false;
  _wantUnmount = true;
  _startTask();
}

uint32_t attempts()      { return _attempts; }
uint32_t lostCount()     { return _lostCount; }
uint32_t frequency()     { return _mounted ? SdSpiDisk::frequency() : 0; }
bool     declared()      { return _declared; }
bool     mounted()       { return _mounted; }
uint64_t capacityBytes() { return _capacity; }

void retryIfDue() {
  if (!_declared || _mounted || _taskAlive) return;
  if (millis() - _lastAttemptMs < 5000) return;
  _wantMount = true;
  _startTask();
}

/* THE CARD'S SUPERVISION, from the loop, every 250 ms — it only raises flags and wakes the
 * task, never touches the bus. HEADLESS: nothing here waits for an app or a page.
 *   - the card is not mounted: a new attempt every RETRY_MS (a card inserted after the
 *     boot, a module that powers up late, a contact that comes back);
 *   - a read failed: the task CHECKS the card (sector 0, PROBES probes); if it does not
 *     answer it was pulled out or lost its contact — it is unmounted (the streams go silent,
 *     the files close) and remounted by the attempts above, then its sounds are read again.
 *     A reader that fails stops reading: no "streak" would ever build up by itself;
 *   - a failed head (a glitch) is read again, a few seconds later;
 *   - every change of state is ANNOUNCED (NIDMI_SD): the app shows it, nobody polls. */
const char* state() {
  if (!_declared) return "off";
  if (_mounted)   return "ok";
  return _lost ? "lost" : "absent";
}

void service() {
  static uint32_t last = 0, lastHeads = 0;
  static const char* announced = "";
  const uint32_t now = millis();
  if (now - last < 250) return;
  last = now;
  if (_declared && _mounted && SdSpiDisk::failStreak() >= 1 && !_wantCheck && !_wantUnmount) {
    _wantCheck = true;                    // a read failed: the task checks whether the card still answers
    _startTask();
  } else if (_declared && !_mounted && !_taskAlive && !_wantMount && !_wantUnmount
             && now - _lastAttemptMs >= RETRY_MS) {
    _wantMount = true;
    _startTask();
  }
  if (_declared && _mounted && now - lastHeads >= 5000) {
    lastHeads = now;
    if (SdStream::failedHeads()) loadHeads();
  }
  const char* st = state();
  if (strcmp(st, announced)) {
    announced = st;
    char frame[24];
    snprintf(frame, sizeof frame, "NIDMI_SD:%s", st);
    nidmi_ws_pousser(frame);
  }
}

void simulateNoise(uint32_t blocks) {
  if (_declared && _mounted) SdSpiDisk::simulateNoise(blocks);
}

void simulateLoss(uint32_t ms) {
  if (_declared && _mounted) SdSpiDisk::simulateOutage(ms);
}

void probeWiring() {
  if (!_declared || _mounted) return;
  _wantWiring = true;
  _startTask();
}

void tryMount() {
  if (!_declared || _mounted) return;
  _wantMount = true;
  _startTask();
}

void loadHeads() {
  _wantHeads = true;
  _startTask();
}

void loadSounds() {
  if (!_mounted) return;
  _wantLoad = true;
  _startTask();
}

bool measure(const char* name, uint32_t hz) {
  if (!_declared || !name || !*name || strlen(name) >= sizeof(_measName) || _measState == 1) return false;
  if (SdStream::anyActive()) {
    /* A measure reads the card flat out for 12 s: it would take the bandwidth of the sounds that
     * play. Said, not done. */
    snprintf(_measJson, sizeof(_measJson), "{\"state\":\"finished\",\"error\":\"des sons de la carte jouent : la mesure leur volerait le debit\"}");
    __sync_synchronize();
    _measState = 2;
    return false;
  }
  strlcpy(_measName, name, sizeof(_measName));
  _measHz = hz;
  _measState = 1;
  _wantMeasure = true;
  _startTask();
  return true;
}

File openForWeb(const char* path) {
  if (!_mounted || _wantCheck || SdSpiDisk::failStreak() > 0) return File();
  return open(path);
}

File open(const char* path) {
  if (!_mounted) return File();
  return SdSpiDisk::open(path);
}

String diagnostics() {
  String j = "{\"declared\":" + String(_declared ? "true" : "false")
           + ",\"state\":\"" + String(state()) + "\""
           + ",\"lost_count\":" + String((unsigned long)_lostCount)
           + ",\"remounts\":" + String((unsigned long)_remounts)
           + ",\"mounted\":" + String(_mounted ? "true" : "false")
           + ",\"pins\":{\"cs\":" + String(_cs) + ",\"sck\":" + String(_sck)
           + ",\"miso\":" + String(_miso) + ",\"mosi\":" + String(_mosi) + "}"
           + ",\"hz\":" + String((unsigned long)(_mounted ? SdSpiDisk::frequency() : _hz))
           + ",\"hz_fallbacks\":" + String((unsigned long)(_mounted ? SdSpiDisk::fallbacks() : 0))
           + ",\"attempts\":" + String((unsigned long)_attempts)
           + ",\"task\":" + String(_taskAlive ? "true" : "false")
           + ",\"total_kb\":" + String((unsigned long)(_capacity / 1024ULL));
  if (_attempts) j += ",\"last_attempt_ms_ago\":" + String((unsigned long)(millis() - _lastAttemptMs));
  if (_reason[0]) j += ",\"reason\":\"" + String(_reason) + "\"";
  if (_cmd0 >= 0)
    j += ",\"probe\":{\"cmd0\":" + String(_cmd0) + ",\"cmd8\":" + String(_cmd8)
       + ",\"cmd8_echo\":" + String((unsigned long)_cmd8Echo) + "}";
  if (_mounted)
    j += ",\"driver\":{\"type\":\"" + String(SdSpiDisk::type()) + "\",\"cmd8_echo\":" + String((unsigned long)SdSpiDisk::cmd8Echo())
       + ",\"blocks_read\":" + String((unsigned long)SdSpiDisk::blocksRead())
       + ",\"crc_errors\":" + String((unsigned long)SdSpiDisk::crcErrors())
       + ",\"crc_rejected\":" + String((unsigned long)SdSpiDisk::crcRejected())
       + ",\"fail_streak\":" + String((unsigned long)SdSpiDisk::failStreak())
       + ",\"fail_total\":" + String((unsigned long)SdSpiDisk::failTotal())
       + ",\"retries\":" + String((unsigned long)SdSpiDisk::retries()) + "}";
  j += ",\"stream\":" + SdStream::diagnostics();
  if (_acmd41Rounds >= 0) {
    j += ",\"manual_init\":{\"cmd0\":\"" + String(_raw[0]) + "\",\"cmd8\":\"" + String(_raw[1])
       + "\",\"cmd58\":\"" + String(_raw[2]) + "\",\"cmd55\":\"" + String(_raw[3])
       + "\",\"acmd41_first\":\"" + String(_raw[4]) + "\",\"acmd41_last\":\"" + String(_raw[5])
       + "\",\"acmd41_rounds\":" + String((int)_acmd41Rounds) + ",\"acmd41_r1\":" + String((int)_acmd41R1) + "}";
  }
  if (_wiringDone) {
    // A pin that does not follow what it drives: short circuit or load.
    const char* pinName[4] = {"CS", "SCK", "MISO", "MOSI"};
    String suspect;
    for (int b = 0; b < 4; b++)
      if (_level[b][2] == 1 || _level[b][3] == 0) suspect += String(suspect.length() ? ", " : "") + pinName[b];
    static char buffer[160];
    const char* verdict =
        suspect.length()
            ? (snprintf(buffer, sizeof(buffer), "%s ne suit pas ce que la carte pilote : court-circuit vers GND/3V3, ou fil sur une mauvaise broche", suspect.c_str()), buffer)
        :
        _bbCmd0 >= 0 && _bbCmd0 != 0xFF && (_cmd0 < 0 || _cmd0 == 0xFF)
            ? "la carte repond A LA MAIN mais pas par le SPI materiel : le defaut est dans notre usage du SPI"
        : _bbSwappedCmd0 >= 0 && _bbSwappedCmd0 != 0xFF
            ? "la carte repond MISO et MOSI echanges : les deux fils de donnees sont inverses"
        : _swappedCmd0 >= 0 && _swappedCmd0 != 0xFF
            ? "la carte repond en SPI MISO et MOSI echanges : les deux fils de donnees sont inverses"
        : _misoLow == 1
            ? "silence meme a la main : MISO est tenu haut (module sous tension) mais la carte n'entend rien — SCK, MOSI ou CS n'arrivent pas, ou carte non alimentee/mal enfoncee"
            : "silence : MISO flotte (suit le tirage) — fil MISO non branche, ou module hors tension";
    j += ",\"wiring\":{\"miso_pulled_low\":" + String(_misoLow) + ",\"miso_pulled_high\":" + String(_misoHigh)
       + ",\"cmd0_miso_mosi_swapped\":" + String(_swappedCmd0)
       + ",\"bitbang_cmd0\":" + String(_bbCmd0) + ",\"bitbang_cmd8\":" + String(_bbCmd8)
       + ",\"bitbang_swapped_cmd0\":" + String(_bbSwappedCmd0)
       + ",\"pins\":{" + [&]() {
           String t;
           for (int b = 0; b < 4; b++)
             t += String(b ? "," : "") + "\"" + pinName[b] + "\":{\"pulled_low\":" + String(_level[b][0])
                + ",\"pulled_high\":" + String(_level[b][1]) + ",\"driven_0_read\":" + String(_level[b][2])
                + ",\"driven_1_read\":" + String(_level[b][3]) + "}";
           return t; }() + "}"
       + ",\"verdict\":\"" + String(verdict) + "\"}";
  }
  j += ",\"measure\":";
  if (_measState == 0)      j += "null";
  else if (_measState == 1) j += "{\"state\":\"running\"}";
  else                      j += String(_measJson);
  j += "}";
  return j;
}

}  // namespace SdCard
