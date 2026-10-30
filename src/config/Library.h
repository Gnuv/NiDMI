#pragma once
// src/config/Library.h — THE USER'S LIBRARY ON THE CARD: script models and presets (MESURES §249).
//
// "SAVE NMS" (a script kept under a name, to start other blocks from) and "SAVE" of a preset
// (a block's parameter values) wrote to the computer's working folder — and, the app served
// by the card, with no working folder: the script was DOWNLOADED, the preset refused. The
// card is the instrument; what it is prepared with lives on it, like its sounds:
//
//   /library/scripts/<name>.nms              a script model
//   /library/presets/<engine>/<name>.json    a preset: {"plugin","name","params"}
//
// Common to every composition (what a BLOCK runs is frozen in its composition: the library
// is where blocks start from, never what a cue reads). Written at silence (Differe, §157):
// a library is prepared, not played — the routes are not concert gestures (§197). Listed
// from StorageIndex and what waits for the silence: no flash walk (§241).
#include <Arduino.h>

namespace Library {

constexpr const char* SCRIPTS = "/library/scripts";
constexpr const char* PRESETS = "/library/presets";
constexpr size_t FILE_MAX = 8192;          // a .nms is bounded by ScriptStore at 8 KB; a preset is smaller

/** The storage path of an entry; "" (and `why`) when the name or the engine is not one. */
String scriptPath(const String& name, String& why);
String presetPath(const String& engine, const String& name, String& why);

/** {"scripts":[{"name":"x.nms","bytes":12}],"presets":[{"engine":"plaits","name":"Pad","bytes":80}]} */
String listJson();

/** What the card holds at `path` (waiting for the silence, or the flash). */
bool read(const String& path, String& out);
/** Taken at once (read() gives it back), written at the first silence. */
bool write(const String& path, const char* data, size_t n, String& why);
bool remove(const String& path);

}  // namespace Library
