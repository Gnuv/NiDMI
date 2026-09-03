#include "MappingEngine.h"
#include "../Globals.h"
#include "../midi/MidiSender.h"
#include "../server/ServerCore.h"

// INITIALISATION DES STATICS (Obligatoire dans le .cpp)
FluxRegistry::Entry FluxRegistry::entries[32];
int FluxRegistry::count = 0;

void FluxRegistry::update(const char* name, float val) {
    if (!name || name[0] == '\0') return;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            entries[i].value = val;
            return;
        }
    }
    if (count < 32) {
        strlcpy(entries[count].name, name, 16);
        entries[count].value = val;
        count++;
    }
}

float FluxRegistry::get(const char* name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) return entries[i].value;
    }
    return 0.0f;
}

bool FluxRegistry::has(const char* name) {
    if (!name || name[0] == '\0') return false;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) return true;
    }
    return false;
}
// Helper pour envoyer CC via MIDI
static void sendMidiControlChange(uint8_t cc, uint8_t value, uint8_t chan, MidiSender* sender) {
    if (sender) {
        sender->sendControlChange(chan, cc, value);
        Serial.printf("[MappingEngine] Sent MIDI CC:%d Chan:%d Val:%d\n", cc, chan, value);
    } else {
        Serial.printf("[MappingEngine] WARNING: No MIDI sender available for CC\n");
    }
}

// Helper pour envoyer Note On via MIDI
static void sendMidiNoteOn(uint8_t note, uint8_t channel, uint8_t velocity, MidiSender* sender) {
    if (sender) {
        sender->sendNoteOn(channel, note, velocity);
        Serial.printf("[MappingEngine] Sent MIDI Note On: Note:%d Chan:%d Vel:%d\n", note, channel, velocity);
    } else {
        Serial.printf("[MappingEngine] WARNING: No MIDI sender available for Note On\n");
    }
}

// Helper pour envoyer Note Off via MIDI
static void sendMidiNoteOff(uint8_t note, uint8_t channel, uint8_t velocity, MidiSender* sender) {
    if (sender) {
        sender->sendNoteOff(channel, note, velocity);
        Serial.printf("[MappingEngine] Sent MIDI Note Off: Note:%d Chan:%d Vel:%d\n", note, channel, velocity);
    } else {
        Serial.printf("[MappingEngine] WARNING: No MIDI sender available for Note Off\n");
    }
}
void MappingEngine::execute(const char* script, float inputVal, MidiSender* midi_sender) {
    if (!script || script[0] == '\0') return;

    float current = inputVal;
    String s = String(script);
    int start = 0;
    int end = s.indexOf(':');

    Serial.printf("[MappingEngine] execute script='%s' input=%.2f\n", script, inputVal);
    while (start < (int)s.length()) {
        int actualEnd = (end == -1) ? s.length() : end;
        String seg = s.substring(start, actualEnd);
        seg.trim();

        if (seg.startsWith("r(\"")) { 
            int closeIdx = seg.indexOf("\")");
            if (closeIdx != -1) {
                String target = seg.substring(3, closeIdx);
                if (FluxRegistry::has(target.c_str())) {
                    current = FluxRegistry::get(target.c_str());
                } else {
                    // Fallback: keep current input value if named source does not exist.
                    Serial.printf("[MappingEngine] WARNING: source '%s' not found, fallback to input %.2f\n", target.c_str(), current);
                }
            }
        } 
        else if (seg.startsWith("*(")) { 
            int closeIdx = seg.indexOf(")");
            if (closeIdx != -1) {
                float m = seg.substring(2, closeIdx).toFloat();
                current *= m;
            }
        }
        else if (seg.startsWith("+(")) { 
            int closeIdx = seg.indexOf(")");
            if (closeIdx != -1) {
                float a = seg.substring(2, closeIdx).toFloat();
                current += a;
            }
        }
        else if (seg.startsWith("-(")) { 
            int closeIdx = seg.indexOf(")");
            if (closeIdx != -1) {
                float s = seg.substring(2, closeIdx).toFloat();
                current -= s;
            }
        }
        else if (seg.startsWith("/(")) { 
            int closeIdx = seg.indexOf(")");
            if (closeIdx != -1) {
                float d = seg.substring(2, closeIdx).toFloat();
                if (d != 0) current /= d;
            }
        }
        else if (seg.startsWith("ctl.out(")) { 
            int comma = seg.indexOf(',');
            int closeIdx = seg.indexOf(')');
            if (comma != -1 && closeIdx != -1) {
                int cc = seg.substring(8, comma).toInt();
                int chan = seg.substring(comma + 1, closeIdx).toInt();
                
                // Clamp value to MIDI range (0-127)
                uint8_t midiValue = (uint8_t)constrain(current, 0, 127);
                
                // Send via MIDI helper
                sendMidiControlChange((uint8_t)cc, midiValue, (uint8_t)chan, midi_sender);
            }
        }
        else if (seg.startsWith("note.on(")) {
            int comma1 = seg.indexOf(',');
            int closeIdx = seg.indexOf(')');
            if (comma1 != -1 && closeIdx != -1) {
                int note = seg.substring(8, comma1).toInt();
                int chan = seg.substring(comma1 + 1, closeIdx).toInt();
                note = constrain(note, 0, 127);
                chan = constrain(chan, 1, 16);
                uint8_t vel = (uint8_t)constrain((int)current, 0, 127);
                sendMidiNoteOn((uint8_t)note, (uint8_t)chan, vel, midi_sender);
            }
        }
        // note.off(note, chan) — sends note off with velocity 127
        else if (seg.startsWith("note.off(")) {
            int comma1 = seg.indexOf(',');
            int closeIdx = seg.indexOf(')');
            if (comma1 != -1 && closeIdx != -1) {
                int note = seg.substring(9, comma1).toInt();
                int chan = seg.substring(comma1 + 1, closeIdx).toInt();
                note = constrain(note, 0, 127);
                chan = constrain(chan, 1, 16);
                uint8_t vel = 127;  // Fixed velocity for note off
                sendMidiNoteOff((uint8_t)note, (uint8_t)chan, vel, midi_sender);
            }
        }
        else if (seg.startsWith("seq.out(")) {
            int closeIdx = seg.indexOf(')');
            if (closeIdx != -1) {
                String source = "seq";
                int firstQuote = seg.indexOf('"');
                if (firstQuote != -1 && firstQuote < closeIdx) {
                    int secondQuote = seg.indexOf('"', firstQuote + 1);
                    if (secondQuote != -1 && secondQuote < closeIdx) {
                        source = seg.substring(firstQuote + 1, secondQuote);
                    }
                }
                String json = "{\"type\":\"seq_event\",\"source\":\"" + source + "\",\"value\":" + String(current > 0 ? 1 : 0) + "}";
                serverCore.websocket().textAll(json);
                Serial.printf("[MappingEngine] Sent seq event source='%s' value=%d\n", source.c_str(), current > 0 ? 1 : 0);
            }
        }
        // note.out(note, chan) — sends note.on when pressed (>0), note.off with vel 0 when released (<=0)
        else if (seg.startsWith("note.out(")) {
            int comma1 = seg.indexOf(',');
            int closeIdx = seg.indexOf(')');
            if (comma1 != -1 && closeIdx != -1) {
                int note = seg.substring(9, comma1).toInt();
                int chan = seg.substring(comma1 + 1, closeIdx).toInt();
                note = constrain(note, 0, 127);
                chan = constrain(chan, 1, 16);
                if (current > 0) {
                    uint8_t vel = (uint8_t)constrain((int)current, 1, 127);  // At least 1 for note on
                    sendMidiNoteOn((uint8_t)note, (uint8_t)chan, vel, midi_sender);
                } else {
                    sendMidiNoteOff((uint8_t)note, (uint8_t)chan, 0, midi_sender);
                }
            }
        }

        if (end == -1) break;
        start = end + 1;
        end = s.indexOf(':', start);
    }
}

// ── Traitement d'un evenement MIDI entrant ─────────────────────────────────
// Meme pipeline que execute(), mais la valeur courante part de la NOTE et peut
// etre republiee comme note. C'est ce qui permet a un .nms de mapping de
// transformer le MIDI ENTRANT — transposition, filtrage de canal, remappage —
// SUR LA CARTE, sans qu'un navigateur soit dans le chemin du son.
bool MappingEngine::executeMidiNote(const char* script,
                                    uint8_t noteIn, uint8_t veloIn, uint8_t canalIn,
                                    bool estNoteOff, SortieNote& sortie) {
    sortie.emise = false;
    if (!script || script[0] == '\0') return false;

    float courant = (float)noteIn;      // par defaut le pipeline porte la note
    String s = String(script);
    int debut = 0;
    int fin = s.indexOf(':');

    while (debut < (int)s.length()) {
        int finReelle = (fin == -1) ? s.length() : fin;
        String seg = s.substring(debut, finReelle);
        seg.trim();

        if (seg.startsWith("note.in(")) {
            courant = (float)noteIn;
        }
        else if (seg.startsWith("vel.in(") || seg.startsWith("velo.in(")) {
            courant = (float)veloIn;
        }
        else if (seg.startsWith("chan.in(") || seg.startsWith("ch.in(")) {
            courant = (float)canalIn;
        }
        else if (seg.startsWith("r(\"")) {
            // r("param","nom",min,max,defaut) : valeur reglee depuis l'app, tenue
            // par le FluxRegistry. Absente, on garde la valeur courante — un
            // script incomplet ne doit pas rendre la carte muette.
            int q1 = seg.indexOf('"');
            int q2 = seg.indexOf('"', q1 + 1);
            String cible = (q1 != -1 && q2 != -1) ? seg.substring(q1 + 1, q2) : String("");
            // Forme r("param","nom",...) : c'est le DEUXIEME nom qui identifie.
            if (cible == "param") {
                int q3 = seg.indexOf('"', q2 + 1);
                int q4 = seg.indexOf('"', q3 + 1);
                if (q3 != -1 && q4 != -1) cible = seg.substring(q3 + 1, q4);
            }
            if (cible.length() && FluxRegistry::has(cible.c_str())) {
                courant = FluxRegistry::get(cible.c_str());
            } else {
                // Valeur par defaut declaree en dernier argument, si presente.
                int derniereVirgule = seg.lastIndexOf(',');
                int par = seg.indexOf(')');
                if (derniereVirgule != -1 && par != -1 && par > derniereVirgule)
                    courant = seg.substring(derniereVirgule + 1, par).toFloat();
            }
        }
        else if (seg.startsWith("+(")) { int p = seg.indexOf(')'); if (p!=-1) courant += seg.substring(2,p).toFloat(); }
        else if (seg.startsWith("-(")) { int p = seg.indexOf(')'); if (p!=-1) courant -= seg.substring(2,p).toFloat(); }
        else if (seg.startsWith("*(")) { int p = seg.indexOf(')'); if (p!=-1) courant *= seg.substring(2,p).toFloat(); }
        else if (seg.startsWith("/(")) { int p = seg.indexOf(')'); if (p!=-1) { float d = seg.substring(2,p).toFloat(); if (d != 0) courant /= d; } }
        else if (seg.startsWith("note.out(")) {
            int par = seg.indexOf(')');
            if (par != -1) {
                String args = seg.substring(9, par);
                int virgule = args.indexOf(',');
                if (virgule == -1) {
                    // note.out(ch) — forme MIDI : la valeur courante EST la note.
                    sortie.note  = (uint8_t)constrain((int)lroundf(courant), 0, 127);
                    sortie.canal = (uint8_t)constrain(args.toInt(), 1, 16);
                } else {
                    // note.out(note, ch) — forme CAPTEUR : note litterale.
                    sortie.note  = (uint8_t)constrain(args.substring(0, virgule).toInt(), 0, 127);
                    sortie.canal = (uint8_t)constrain(args.substring(virgule + 1).toInt(), 1, 16);
                }
                sortie.velo  = estNoteOff ? 0 : veloIn;
                sortie.emise = true;
            }
        }

        if (fin == -1) break;
        debut = fin + 1;
        fin = s.indexOf(':', debut);
    }
    return sortie.emise;
}
