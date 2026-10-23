// src/midi/TransportNotes.cpp — see TransportNotes.h (MESURES §215).
#include "TransportNotes.h"
#include <Preferences.h>
#include "../config/EcrituresDifferees.h"
#include "../server/ServerCore.h"   // nidmi_ws_pousser

extern "C" void nidmi_sys_recevoir(const char* nom, float valeur);   // NiDMI.cpp: the sys.* gestures

namespace TransportNotes {
namespace {

constexpr const char* NVS_SPACE = "nidmi-cues";   // the interface (§190): with the transport's options
constexpr const char* NVS_KEY   = "tnotes";
constexpr uint32_t    LEARN_MS  = 30000;
constexpr uint16_t    NONE      = 0xFFFF;

const char* const NAMES[COUNT]    = {"playstop", "next", "prev", "first"};
const char* const GESTURES[COUNT] = {"sys.playstop", "sys.nextcue", "sys.prevcue", "sys.firstcue"};

/* channel << 8 | note, NONE if unbound: 16 bits, read and written whole by any task. */
volatile uint16_t g_bindings[COUNT] = {NONE, NONE, NONE, NONE};
volatile bool     g_held[COUNT] = {false, false, false, false};   // its note-on swallowed, its note-off to come
volatile int8_t   g_learning = -1;
volatile uint32_t g_learnSince = 0;
volatile bool     g_write = false;      // a binding changed: the loop writes it
volatile bool     g_announce = false;   // ... and says it

inline uint16_t pack(uint8_t ch, uint8_t note) { return (uint16_t)((ch << 8) | note); }

/* One note, one gesture: the others leave it. */
void release(uint16_t b, uint8_t except) {
  for (uint8_t a = 0; a < COUNT; a++)
    if (a != except && g_bindings[a] == b) { g_bindings[a] = NONE; g_held[a] = false; }
}

String serialise() {
  String s;
  for (uint8_t a = 0; a < COUNT; a++) {
    const uint16_t b = g_bindings[a];
    s += String(a ? "," : "") + NAMES[a] + "=";
    if (b != NONE) s += String(b >> 8) + ":" + String(b & 0xFF);
  }
  return s;
}

void parse(const String& s) {
  int from = 0;
  while (from < (int)s.length()) {
    int end = s.indexOf(',', from);
    if (end < 0) end = s.length();
    const String item = s.substring(from, end);
    from = end + 1;
    const int eq = item.indexOf('='), colon = item.indexOf(':');
    if (eq < 0 || colon < eq) continue;
    const Action a = fromName(item.substring(0, eq).c_str());
    const int ch = item.substring(eq + 1, colon).toInt(), note = item.substring(colon + 1).toInt();
    if (a < COUNT && ch >= 0 && ch <= 16 && note >= 0 && note <= 127) g_bindings[a] = pack(ch, note);
  }
}

}  // namespace

Action fromName(const char* name) {
  for (uint8_t a = 0; a < COUNT; a++) if (name && !strcmp(name, NAMES[a])) return (Action)a;
  return COUNT;
}

void begin() {
  Preferences p;
  if (p.begin(NVS_SPACE, true)) {          // never written: no binding
    parse(p.getString(NVS_KEY, ""));
    p.end();
  }
}

bool take(uint8_t channel, uint8_t note, uint8_t velocity, bool noteOff) {
  const bool on = !noteOff && velocity > 0;
  const int8_t l = g_learning;
  if (on && l >= 0 && l < COUNT) {
    const uint16_t b = pack(channel, note);
    release(b, (uint8_t)l);
    g_bindings[l] = b;
    g_held[l] = true;
    g_learning = -1;
    g_write = true;
    g_announce = true;
    return true;                            // the note that teaches plays nothing
  }
  for (uint8_t a = 0; a < COUNT; a++) {
    const uint16_t b = g_bindings[a];
    if (b == NONE || (b & 0xFF) != note) continue;
    const uint8_t bc = (uint8_t)(b >> 8);
    if (bc && bc != channel) continue;
    if (on) {
      g_held[a] = true;
      nidmi_sys_recevoir(GESTURES[a], 1.0f);   // a gesture: the loop runs it at its next turn
      return true;
    }
    if (g_held[a]) { g_held[a] = false; return true; }
    return false;                           // the note-off of a note that played before it was bound
  }
  return false;
}

bool set(Action a, int channel, int note, String& reason) {
  if (a >= COUNT) { reason = "action inconnue (playstop, next, prev, first)"; return false; }
  if (channel < 0 || channel > 16) { reason = "canal 0 a 16 (0 : tous)"; return false; }
  if (note < 0 || note > 127) { reason = "note 0 a 127"; return false; }
  const uint16_t b = pack((uint8_t)channel, (uint8_t)note);
  release(b, a);
  g_held[a] = false;
  g_bindings[a] = b;
  if (g_learning == (int8_t)a) g_learning = -1;
  g_write = true;
  g_announce = true;
  return true;
}

void clear(Action a) {
  if (a >= COUNT) return;
  g_bindings[a] = NONE;
  g_held[a] = false;
  if (g_learning == (int8_t)a) g_learning = -1;
  g_write = true;
  g_announce = true;
}

void learn(Action a) {
  if (a >= COUNT) return;
  g_learnSince = millis();
  g_learning = (int8_t)a;
  g_announce = true;
}

void cancelLearn() {
  if (g_learning < 0) return;
  g_learning = -1;
  g_announce = true;
}

String json() {
  String j = "{";
  for (uint8_t a = 0; a < COUNT; a++) {
    const uint16_t b = g_bindings[a];
    j += String(a ? "," : "") + "\"" + NAMES[a] + "\":";
    if (b == NONE) j += "null";
    else j += "{\"ch\":" + String(b >> 8) + ",\"note\":" + String(b & 0xFF) + "}";
  }
  const int8_t l = g_learning;
  j += ",\"learning\":";
  j += (l >= 0 && l < COUNT) ? String("\"") + NAMES[l] + "\"" : String("null");
  return j + "}";
}

void service() {
  if (g_learning >= 0 && millis() - g_learnSince > LEARN_MS) cancelLearn();
  if (g_write) {
    g_write = false;
    Differe::nvsChaine(NVS_SPACE, NVS_KEY, serialise());   // at the silence (§157)
  }
  if (g_announce) {
    g_announce = false;
    const String t = "NIDMI_TRANSPORT:" + json();
    nidmi_ws_pousser(t.c_str());
  }
}

}  // namespace TransportNotes
