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

// ── Rouages communs aux deux pipelines MIDI ────────────────────────────────
// executeMidiNote et executeMidiCc partagent tout sauf leurs verbes d'entree et
// de sortie. Ces trois fonctions etaient des lambdas locales a la premiere ; les
// sortir evite de les recopier dans la seconde — et donc de les corriger deux
// fois, ce qui est exactement comme naissent deux comportements divergents.
namespace {

// Un .nms de FICHIER n'est pas une ligne de l'editeur de broche : il porte des
// commentaires « // … », des retours a la ligne et un « ; » final. Sans ce
// nettoyage, le premier segment valait « // Transpose …\nnote.in() » et
// note.in() passait inapercu.
String nettoyer(const char* script) {
    String s;
    String brut = String(script);
    bool enCommentaire = false;
    for (unsigned i = 0; i < brut.length(); i++) {
        const char c = brut[i];
        if (enCommentaire) { if (c == '\n') enCommentaire = false; continue; }
        if (c == '/' && i + 1 < brut.length() && brut[i+1] == '/') { enCommentaire = true; i++; continue; }
        if (c == '\n' || c == '\r' || c == '\t') { s += ' '; continue; }
        s += c;
    }
    s.trim();
    while (s.endsWith(";")) { s.remove(s.length() - 1); s.trim(); }
    return s;
}

// Position du prochain separateur AU NIVEAU ZERO de parentheses, ou -1.
// Un ':' dans « r("param","x",0,1) » ne separe pas deux segments, et un ';'
// dans un argument ne separe pas deux pipelines.
int prochainSeparateur(const String& s, int depuis, char sep) {
    int prof = 0;
    for (int i = depuis; i < (int)s.length(); i++) {
        if (s[i] == '(') prof++;
        else if (s[i] == ')') prof--;
        else if (s[i] == sep && prof == 0) return i;
    }
    return -1;
}

// Argument d'un operateur : soit un nombre litteral, soit une LECTURE r(...).
// « +(r("param","semitones",-24,24,0)) » est la forme normale d'un .nms — un
// operateur dont l'argument est lui-meme une lecture. Une version precedente
// coupait a la premiere « ) », donc au milieu du r(), et lisait 0.
float valeurArg(const String& arg) {
    String a = arg; a.trim();
    if (!a.startsWith("r(")) return a.toFloat();
    // r("nom")  ou  r("param","nom",min,max,defaut)
    int q1 = a.indexOf('"');
    int q2 = (q1 >= 0) ? a.indexOf('"', q1 + 1) : -1;
    if (q1 < 0 || q2 < 0) return 0.0f;
    String cible = a.substring(q1 + 1, q2);
    if (cible == "param") {
        const int q3 = a.indexOf('"', q2 + 1);
        const int q4 = (q3 >= 0) ? a.indexOf('"', q3 + 1) : -1;
        if (q3 >= 0 && q4 >= 0) cible = a.substring(q3 + 1, q4);
    }
    if (cible.length() && FluxRegistry::has(cible.c_str()))
        return FluxRegistry::get(cible.c_str());
    // Absent du registre : on prend le DEFAUT declare en dernier argument, ce
    // qui rend un script exploitable meme sans reglage pousse.
    const int derniere = a.lastIndexOf(',');
    const int par = a.lastIndexOf(')');
    if (derniere >= 0 && par > derniere) return a.substring(derniere + 1, par).toFloat();
    return 0.0f;
}

// Contenu de « op(...) », en comptant les parentheses pour ne pas s'arreter sur
// celle d'un r() imbrique.
String contenuParentheses(const String& seg, int depuis) {
    int prof = 0, debut = -1;
    for (int i = depuis; i < (int)seg.length(); i++) {
        if (seg[i] == '(') { if (prof++ == 0) debut = i + 1; }
        else if (seg[i] == ')') { if (--prof == 0 && debut >= 0) return seg.substring(debut, i); }
    }
    return String("");
}

// Les quatre operateurs arithmetiques, communs aux deux pipelines. Renvoie
// false si le segment n'en est pas un — a l'appelant de continuer son analyse.
bool operateur(const String& seg, float& courant) {
    if (seg.startsWith("+(")) { courant += valeurArg(contenuParentheses(seg, 1)); return true; }
    if (seg.startsWith("-(")) { courant -= valeurArg(contenuParentheses(seg, 1)); return true; }
    if (seg.startsWith("*(")) { courant *= valeurArg(contenuParentheses(seg, 1)); return true; }
    if (seg.startsWith("/(")) { const float d = valeurArg(contenuParentheses(seg, 1));
                                if (d != 0) courant /= d; return true; }
    if (seg.startsWith("r("))  { courant = valeurArg(seg); return true; }
    return false;
}

}  // namespace

// ── Traitement d'un evenement MIDI entrant ─────────────────────────────────
// Meme pipeline que execute(), mais la valeur courante part de la NOTE et peut
// etre republiee comme note. C'est ce qui permet a un .nms de mapping de
// transformer le MIDI ENTRANT — transposition, filtrage de canal, remappage —
// SUR LA CARTE, sans qu'un navigateur soit dans le chemin du son.
//
// Un .nms peut porter PLUSIEURS pipelines separes par ';'. On les parcourt
// tous ; le dernier a emettre l'emporte, puisque la sortie est unique.
bool MappingEngine::executeMidiNote(const char* script,
                                    uint8_t noteIn, uint8_t veloIn, uint8_t canalIn,
                                    bool estNoteOff, SortieNote& sortie) {
    sortie.emise = sortie.traite = false;
    if (!script || script[0] == '\0') return false;

    const String s = nettoyer(script);

    int debutPipe = 0;
    while (debutPipe <= (int)s.length()) {
        const int finPipe = prochainSeparateur(s, debutPipe, ';');
        const String pipe = s.substring(debutPipe, (finPipe == -1) ? s.length() : finPipe);

        float courant = (float)noteIn;
        int debut = 0;
        while (debut <= (int)pipe.length()) {
            const int fin = prochainSeparateur(pipe, debut, ':');
            String seg = pipe.substring(debut, (fin == -1) ? pipe.length() : fin);
            seg.trim();

            // Des qu'un verbe de NOTE apparait, le script declare s'occuper
            // des notes — c'est ce qui rend son silence significatif.
            if (seg.startsWith("note.in"))        { sortie.traite = true; courant = (float)noteIn; }
            else if (seg.startsWith("vel.in") || seg.startsWith("velo.in")) courant = (float)veloIn;
            else if (seg.startsWith("chan.in") || seg.startsWith("ch.in"))  courant = (float)canalIn;
            else if (operateur(seg, courant))     { /* traite */ }
            else if (seg.startsWith("note.out(")) {
                sortie.traite = true;
                const String args = contenuParentheses(seg, 8);
                const int virgule = args.indexOf(',');
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

            if (fin == -1) break;
            debut = fin + 1;
        }

        if (finPipe == -1) break;
        debutPipe = finPipe + 1;
    }
    return sortie.emise;
}

// ── Traitement d'un CC entrant ─────────────────────────────────────────────
// Pendant du precedent. Voir MappingEngine.h pour les verbes servis et pour
// l'asymetrie voulue avec les notes (un script de notes ne doit pas rendre
// muets les controleurs).
bool MappingEngine::executeMidiCc(const char* script,
                                  uint8_t ccIn, uint8_t valeurIn, uint8_t canalIn,
                                  SortieCc& sortie) {
    sortie.emise = sortie.traite = false;
    if (!script || script[0] == '\0') return false;

    const String s = nettoyer(script);

    int debutPipe = 0;
    while (debutPipe <= (int)s.length()) {
        const int finPipe = prochainSeparateur(s, debutPipe, ';');
        const String pipe = s.substring(debutPipe, (finPipe == -1) ? s.length() : finPipe);

        float courant = (float)valeurIn;
        bool  filtre  = false;     // un ctl.in de ce pipeline a refuse l'evenement
        int   debut   = 0;
        while (debut <= (int)pipe.length() && !filtre) {
            const int fin = prochainSeparateur(pipe, debut, ':');
            String seg = pipe.substring(debut, (fin == -1) ? pipe.length() : fin);
            seg.trim();

            if (seg.startsWith("ctl.in(")) {
                // Des qu'un ctl.in existe, le script DECLARE traiter les CC —
                // meme si ce pipeline-ci filtre l'evenement en cours.
                sortie.traite = true;
                const String args = contenuParentheses(seg, 6);
                const int virgule = args.indexOf(',');
                const String aCh = (virgule == -1) ? args : args.substring(0, virgule);
                const String aCc = (virgule == -1) ? String("") : args.substring(virgule + 1);
                // Argument vide ou 0 = pas de filtre, comme dans le moteur web.
                const int fCh = aCh.length() ? aCh.toInt() : 0;
                if (fCh != 0 && fCh != (int)canalIn) { filtre = true; break; }
                if (aCc.length()) {
                    const int fCc = aCc.toInt();
                    if (fCc != (int)ccIn) { filtre = true; break; }
                }
                courant = (float)valeurIn;
            }
            else if (seg.startsWith("chan.in") || seg.startsWith("ch.in")) courant = (float)canalIn;
            else if (operateur(seg, courant)) { /* traite */ }
            else if (seg.startsWith("ctl.out(")) {
                const String args = contenuParentheses(seg, 7);
                const int virgule = args.indexOf(',');
                const String aCh = (virgule == -1) ? args : args.substring(0, virgule);
                const String aCc = (virgule == -1) ? String("") : args.substring(virgule + 1);
                sortie.canal  = aCh.length() ? (uint8_t)constrain(aCh.toInt(), 1, 16)
                                             : (uint8_t)(canalIn ? canalIn : 1);
                sortie.cc     = aCc.length() ? (uint8_t)constrain(aCc.toInt(), 0, 127) : ccIn;
                sortie.valeur = (uint8_t)constrain((int)lroundf(courant), 0, 127);
                sortie.emise  = true;
            }

            if (fin == -1) break;
            debut = fin + 1;
        }

        if (finPipe == -1) break;
        debutPipe = finPipe + 1;
    }
    return sortie.emise;
}
