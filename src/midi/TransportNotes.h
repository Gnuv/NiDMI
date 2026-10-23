// src/midi/TransportNotes.h — THE TRANSPORT NOTES: a pedal, a pad, a key that drives the cues
// (MESURES §215).
//
// Four gestures, each bound to a note on a channel (0: any channel): play/stop, the next cue, the
// previous one, the first one. They were the BROWSER's — it listened to its own MIDI inputs and moved
// its own cases —, and a pedal plugged into the card never reached it. The card holds them now: part of
// the INTERFACE (the controller in front of you, not the show), NVS `nidmi-cues`, key `tnotes`.
//
// A bound note-on does its gesture (the sys.* gesture of the same name, run by the loop) and is
// SWALLOWED, before any script: it plays nothing. Its note-off too — only when its note-on was
// swallowed: a note bound while it is held keeps its note-off (a lost note-off is a stuck note).
//
// LEARN: the next note-on that reaches the card, on any input, becomes the action's; it is swallowed
// too. The loop writes it (at the silence, `Differe`) and announces it: NIDMI_TRANSPORT:<json>.
#pragma once
#include <Arduino.h>

namespace TransportNotes {

enum Action : uint8_t { PLAY_STOP = 0, NEXT, PREV, FIRST, COUNT };

/** The action named `playstop`, `next`, `prev` or `first`; COUNT if none. */
Action fromName(const char* name);

/** Once, at boot, before MIDI flows: the bindings the NVS keeps. */
void begin();

/** Any task a note enters by (MidiRouter::noteEntrante): true = the note is a transport note, or
 *  the one that teaches — swallowed. Lock-free, never blocks. */
bool take(uint8_t channel, uint8_t note, uint8_t velocity, bool noteOff);

/** The web task: bind (channel 0..16, 0 = any; note 0..127), or clear. A note bound to another
 *  action leaves it — one note, one gesture. Written at the silence, announced. */
bool set(Action a, int channel, int note, String& reason);
void clear(Action a);

/** Learn the next note-on for `a` (30 s at most), or stop learning. Announced. */
void learn(Action a);
void cancelLearn();

/** {"playstop":{"ch":10,"note":35}|null, …, "learning":"next"|null} */
String json();

/** nidmi_loop: what a learnt note left to do — write it, say it; the learning's time out. */
void service();

}  // namespace TransportNotes
