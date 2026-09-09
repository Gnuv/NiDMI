#include "MappingEngine.h"

/* NMS_BANC_HOTE : compilation sur le poste de developpement, pour le banc de
 * conformite (hardware/bench/nms/ du depot nidmi). Le pipeline .nms est du
 * calcul pur — il ne demande ni WiFi, ni I2S, ni MidiSender. L'isoler derriere
 * cette garde permet de le compiler et de l'EPROUVER sans carte, en comparant
 * ses sorties a celles du moteur web qui fait reference. Sans ca, la seule
 * verification possible etait de flasher. */
#ifndef NMS_BANC_HOTE
#include "../Globals.h"
#include "../midi/MidiSender.h"
#include "../server/ServerCore.h"
#endif

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

// ═══════════════════════════════════════════════════════════════════════════
// PIPELINE .nms — portage du moteur web (engines/core/midi-script/web/index.js)
//
// La reference est le moteur web : c'est lui qui DEFINIT la langue. Tout ecart
// ici est un bug, pas un choix. Le banc hardware/bench/nms/ fait passer les
// memes cas aux deux et diffe les sorties.
//
// Ce qui n'est PAS porte, et pourquoi :
//   - tout ce qui demande une horloge (lag, ramp, del, debounce, makenote,
//     metro, loadbang) : il faut un tick regulier depuis nidmi_loop, plus une
//     file d'attente. C'est une seconde vague, pas un oubli ;
//   - osc.in / osc.out : la couche OSC vit dans ServerCore, hors de ce calcul
//     pur — les brancher ici ferait rentrer le reseau dans le pipeline ;
//   - le bus inter-blocs r("plugin.x") : il n'a pas de sens sur une carte qui
//     n'heberge qu'un script.
// ═══════════════════════════════════════════════════════════════════════════

#include <math.h>

namespace {

// ── Etat par pipeline ──────────────────────────────────────────────────────
// toggle, counter, seq, sel/map, lp, drunk, hysteresis, change gardent une
// memoire d'un evenement au suivant. Le moteur web la range dans une Map
// indexee par numero de pipeline ; ici un tableau fixe suffit et ne coute rien.
// Etat du SCRIPT MIDI (celui de MidiRouter). Les composants apportent le leur :
// un seul tableau pour tout le monde ferait avancer le compteur d'un bouton
// quand une note arrive.
constexpr int MAX_PIPELINES = 12;
MappingEngine::Etat g_etats[MAX_PIPELINES];
MappingEngine::Impression g_impression = nullptr;

// ── Outils de chaine ───────────────────────────────────────────────────────

// Retire les commentaires « // … » et normalise les blancs. Le moteur web fait
// exactement ceci (script.replace(/\/\/[^\n]*/g,'')) avant de decouper.
String nettoyer(const char* script) {
    String s;
    const String brut = String(script);
    bool enCommentaire = false;
    for (unsigned i = 0; i < brut.length(); i++) {
        const char c = brut[i];
        if (enCommentaire) { if (c == '\n') enCommentaire = false; continue; }
        if (c == '/' && i + 1 < brut.length() && brut[i + 1] == '/') { enCommentaire = true; i++; continue; }
        if (c == '\n' || c == '\r' || c == '\t') { s += ' '; continue; }
        s += c;
    }
    return s;
}

// Position du prochain separateur au niveau ZERO de parentheses et HORS chaine.
// Les guillemets comptent : sans eux, osc.out("/a:b") se couperait en deux
// segments. Le moteur web fait la meme chose (_splitSegs / _splitArgs).
int prochainSep(const String& s, int depuis, char sep) {
    int prof = 0;
    bool chaine = false;
    for (int i = depuis; i < (int)s.length(); i++) {
        const char c = s[i];
        if (c == '"') { chaine = !chaine; continue; }
        if (chaine) continue;
        if (c == '(' || c == '[') prof++;
        else if (c == ')' || c == ']') prof--;
        else if (c == sep && prof == 0) return i;
    }
    return -1;
}

// Contenu de « verbe(...) », parentheses equilibrees.
String contenuParentheses(const String& seg, int depuis) {
    int prof = 0, debut = -1;
    for (int i = depuis; i < (int)seg.length(); i++) {
        if (seg[i] == '(') { if (prof++ == 0) debut = i + 1; }
        else if (seg[i] == ')') { if (--prof == 0 && debut >= 0) return seg.substring(debut, i); }
    }
    return String("");
}

// Le segment est-il « nom(...) » ? Si oui, args recoit le contenu.
// Compare le NOM EXACT : sans ca « note.out » repondrait a « note.out.vel ».
bool verbe(const String& seg, const char* nom, String& args) {
    const int n = (int)strlen(nom);
    if ((int)seg.length() < n + 2) return false;
    for (int i = 0; i < n; i++) if (seg[i] != nom[i]) return false;
    if (seg[n] != '(' || seg[(int)seg.length() - 1] != ')') return false;
    args = contenuParentheses(seg, n);
    return true;
}

// Decoupe une liste d'arguments sur les virgules de niveau zero.
int decouperArgs(const String& s, String* sortie, int max) {
    int n = 0, debut = 0;
    while (n < max) {
        const int fin = prochainSep(s, debut, ',');
        String a = s.substring(debut, (fin == -1) ? s.length() : fin);
        a.trim();
        if (a.length()) sortie[n++] = a;
        if (fin == -1) break;
        debut = fin + 1;
    }
    return n;
}

bool estNombre(const String& s) {
    if (!s.length()) return false;
    int i = (s[0] == '-') ? 1 : 0;
    if (i >= (int)s.length()) return false;
    bool point = false, chiffre = false;
    for (; i < (int)s.length(); i++) {
        const char c = s[i];
        if (c >= '0' && c <= '9') { chiffre = true; continue; }
        if (c == '.' && !point) { point = true; continue; }
        return false;
    }
    return chiffre;
}

// Valeur d'un argument : un litteral, ou une lecture r(...).
// « +(r("param","semitones",-24,24,0)) » est la forme normale d'un .nms.
float valeurArg(const String& arg) {
    String a = arg; a.trim();
    if (!a.length()) return 0.0f;
    if (estNombre(a)) return a.toFloat();
    String dedans;
    if (verbe(a, "r", dedans) || verbe(a, "receive", dedans)) {
        String morceaux[6];
        const int n = decouperArgs(dedans, morceaux, 6);
        if (n == 0) return 0.0f;
        // r("nom")  ou  r("param","nom",min,max[,defaut])
        auto sansGuillemets = [](String x) {
            x.trim();
            if (x.length() >= 2 && x[0] == '"' && x[(int)x.length() - 1] == '"')
                return x.substring(1, (int)x.length() - 1);
            return x;
        };
        String cible = sansGuillemets(morceaux[0]);
        int    iDefaut = -1;
        if (cible == "param" && n >= 2) { cible = sansGuillemets(morceaux[1]); iDefaut = (n >= 5) ? 4 : -1; }
        if (cible.length() && FluxRegistry::has(cible.c_str()))
            return FluxRegistry::get(cible.c_str());
        // Absent du registre : le DEFAUT declare, ce qui rend un script
        // exploitable meme sans reglage pousse. (Le moteur web rend 0 ; ici on
        // est plus utile sans etre incompatible — un registre alimente donne le
        // meme resultat des deux cotes.)
        if (iDefaut >= 0) return morceaux[iDefaut].toFloat();
        return 0.0f;
    }
    return a.toFloat();
}

// ToInt32 de JavaScript : troncature vers zero puis repliement sur 32 bits.
// Les operateurs binaires du moteur web passent par « current|0 » ; sans cette
// conversion, &(255) sur une valeur fractionnaire divergerait.
int32_t versInt32(float v) {
    if (!isfinite(v)) return 0;
    const double t = trunc((double)v);
    const double m = fmod(t, 4294967296.0);
    double u = (m < 0) ? m + 4294967296.0 : m;
    if (u >= 2147483648.0) u -= 4294967296.0;
    return (int32_t)u;
}

}  // namespace

void MappingEngine::surImpression(Impression fn) { g_impression = fn; }

void MappingEngine::reinitialiser() {
    for (int i = 0; i < MAX_PIPELINES; i++) g_etats[i].reinitialiser();
}

namespace {

using Evt    = MappingEngine::Evenement;
using Etat   = MappingEngine::Etat;
using Sortie = MappingEngine::Sortie;

struct Source { float valeur = 0; bool declenche = false; };

// Le genre d'evenement auquel une SOURCE repond. Sert a `traite` : un pipeline
// pilote par f(), counter() ou une lecture de registre n'a pas « pris en
// charge » la note qui l'a fait tourner, et ne doit donc pas la bloquer.
enum class Famille { Aucune, Note, Cc, Bend, Touch, PolyTouch, Pgm };

Famille familleSource(const String& seg) {
    if (seg.startsWith("note.in") || seg.startsWith("note.on") ||
        seg.startsWith("note.off") || seg.startsWith("vel.in") ||
        seg.startsWith("notechan.in"))                          return Famille::Note;
    if (seg.startsWith("ctl.in") || seg.startsWith("ccnum.in") ||
        seg.startsWith("ctlchan.in"))                           return Famille::Cc;
    if (seg.startsWith("bend.in"))                              return Famille::Bend;
    if (seg.startsWith("touch.in"))                             return Famille::Touch;
    if (seg.startsWith("polytouch.in"))                         return Famille::PolyTouch;
    if (seg.startsWith("pgm.in"))                               return Famille::Pgm;
    return Famille::Aucune;
}

Famille familleEvenement(const Evt& e) {
    switch (e.type) {
        case Evt::NoteOn: case Evt::NoteOff: return Famille::Note;
        case Evt::Cc:        return Famille::Cc;
        case Evt::Bend:      return Famille::Bend;
        case Evt::Touch:     return Famille::Touch;
        case Evt::PolyTouch: return Famille::PolyTouch;
        case Evt::Pgm:       return Famille::Pgm;
        default:             return Famille::Aucune;
    }
}

// Deux filtres optionnels « verbe(a,b) » : a vide ou 0 = tous, b absent = tous.
void lireDeuxFiltres(const String& args, int& a, int& b) {
    String m[2];
    const int n = decouperArgs(args, m, 2);
    a = (n >= 1 && m[0].length()) ? (int)m[0].toInt() : 0;
    b = (n >= 2 && m[1].length()) ? (int)m[1].toInt() : -1;
}

Source evaluerSource(const String& seg, const Evt& e, MappingEngine::Etat& st) {
    Source non;

    /* SUR UN BATTEMENT, SEULES LES SOURCES D'HORLOGE TIRENT.
     *
     * r(), f(), i() et les litteraux se declenchent « sur n'importe quel
     * evenement » — c'est leur definition, et elle est juste tant que les
     * evenements sont du MIDI. Avec un battement a 100 Hz, ces memes pipelines
     * emettraient cent fois par seconde. Le moteur web ne s'y trompe pas : son
     * tick() ne fait tourner QUE les pipelines a minuterie et les lectures
     * inter-blocs, jamais tous les pipelines.
     *
     * Pour lire le registre au rythme qu'on choisit, on l'ecrit : le r() se met
     * APRES l'horloge — « metro(50) : r("bouton") : ... » — ce qui a le merite
     * de dire sa cadence dans le script. */
    if (e.type == Evt::Tick || e.type == Evt::Init) {
        const bool horloge = (seg == "loadbang()") || seg.startsWith("metro(");
        if (!horloge) return non;
    }

    if (estNombre(seg)) return { seg.toFloat(), true };

    String args;
    // f(x) / i(x) — une constante, ou une lecture. Se declenche sur tout.
    if (verbe(seg, "f", args)) return { valeurArg(args.length() ? args : String("0")), true };
    if (verbe(seg, "i", args)) return { roundf(valeurArg(args.length() ? args : String("0"))), true };
    // r(...) / receive(...) — lecture du registre, se declenche sur tout.
    if (verbe(seg, "r", args) || verbe(seg, "receive", args)) return { valeurArg(seg), true };

    /* ── SOURCES D'HORLOGE : ce qui fait tourner un pipeline sans MIDI ────────
     * Heritage assume du DSL, passe de « script MIDI » a script generaliste :
     * un pipeline qui pilote du DMX ou de l'OSC n'a aucune raison d'attendre
     * une note. La reponse est celle de Max/Pd — une source d'horloge, ecrite
     * dans le script, pas un mecanisme cache. */

    /* loadbang() — un bang unique apres un chargement.
     * PAS pilote par le tick : le moteur web lui donne un declencheur propre
     * (triggerLoadbang(), « after UI is ready »). On garde la meme forme, un
     * evenement Init distinct, plutot que de le faire tirer au premier
     * battement : sinon les deux moteurs diraient deja deux choses. */
    if (seg == "loadbang()") {
        /* Tire a CHAQUE Init, sans verrou. C'est la semantique du moteur web :
         * triggerLoadbang() emet a chaque appel, et c'est l'APPELANT qui n'en
         * fait qu'un apres le chargement. Le verrou que j'avais ajoute ici
         * deplacait la responsabilite dans le moteur et faisait diverger les
         * deux — le banc l'a signale au premier passage. */
        if (e.type != Evt::Init) return non;
        return { 1.0f, true };
    }

    // metro(ms) — un bang tous les `ms`. Comme [metro] de Max/Pd, PAS de bang
    // immediat : le premier battement planifie, le suivant tire.
    if (verbe(seg, "metro", args)) {
        if (e.type != Evt::Tick) return non;
        const long ms = (long)valeurArg(args.length() ? args : String("0"));
        if (ms <= 0) return non;
        if (!st.metroArme) { st.metroArme = true; st.metroProchain = e.instant + (uint32_t)ms; return non; }
        if ((int32_t)(e.instant - st.metroProchain) < 0) return non;
        /* Replanification : « prochain += ms », donc SANS DERIVE — et
         * resynchronisation sur maintenant seulement si l'on a plus d'une
         * periode de retard. C'est mot pour mot la regle du moteur web ; y
         * mettre « instant + ms » aurait fait deriver la carte a chaque
         * battement un peu tardif.
         * Le rattrapage (metro(ms,catchup[,max]), qui tire plusieurs bangs dans
         * un meme battement) n'est PAS porte : ici un pipeline ne tourne qu'une
         * fois par appel. Divergence declaree dans le vocabulaire, pas tue. */
        if ((int32_t)(e.instant - st.metroProchain) > (int32_t)ms) st.metroProchain = e.instant;
        st.metroProchain += (uint32_t)ms;
        return { 1.0f, true };
    }
    /* raw.in() — la LECTURE DU CAPTEUR dans sa resolution native.
     *
     *   r("in")    valeur mise a l'echelle MIDI, 0..127
     *   raw.in()   meme lecture avant cette mise a l'echelle : 0..4095 pour un
     *              capteur analogique, 0/1 pour un contact.
     *
     * Ce n'est pas un contournement du conditionnement : filtre, course utile et
     * hysteresis s'appliquent dans les deux cas — ils relevent de la lecture du
     * capteur, pas de l'intention musicale (MESURES.md §55). Seule la
     * quantification en 0..127 est ecartee, ce qui rend la pleine resolution
     * disponible pour un pitch bend ou une rampe.
     *
     * « raw » est un nom RESERVE du registre. */
    if (seg == "raw.in()") return { FluxRegistry::get("raw"), true };

    const bool note = (e.type == Evt::NoteOn || e.type == Evt::NoteOff);
    int f1, f2;

    if (verbe(seg, "note.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (note && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.a, true };
        return non;
    }
    if (verbe(seg, "note.on", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::NoteOn && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.a, true };
        return non;
    }
    if (verbe(seg, "note.off", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::NoteOff && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.a, true };
        return non;
    }
    // vel.in([ch[,note]]) — canal d'abord, filtre de note ensuite.
    if (verbe(seg, "vel.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::NoteOn && (f1 == 0 || f1 == (int)e.canal)
            && (f2 < 0 || f2 == (int)e.a)) return { (float)e.b, true };
        return non;
    }
    if (verbe(seg, "notechan.in", args)) {
        if (e.type == Evt::NoteOn && args.toInt() == (long)e.a) return { (float)e.canal, true };
        return non;
    }
    if (verbe(seg, "ctl.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::Cc && (f1 == 0 || f1 == (int)e.canal)
            && (f2 < 0 || f2 == (int)e.a)) return { (float)e.b, true };
        return non;
    }
    // ccnum.in([ch]) — la valeur courante est le NUMERO, pas la valeur.
    if (verbe(seg, "ccnum.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::Cc && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.a, true };
        return non;
    }
    if (verbe(seg, "ctlchan.in", args)) {
        if (e.type == Evt::Cc && args.toInt() == (long)e.a) return { (float)e.canal, true };
        return non;
    }
    if (verbe(seg, "bend.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::Bend && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.valeur14, true };
        return non;
    }
    if (verbe(seg, "touch.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::Touch && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.a, true };
        return non;
    }
    if (verbe(seg, "polytouch.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::PolyTouch && (f1 == 0 || f1 == (int)e.canal)
            && (f2 < 0 || f2 == (int)e.a)) return { (float)e.b, true };
        return non;
    }
    if (verbe(seg, "pgm.in", args)) {
        lireDeuxFiltres(args, f1, f2);
        if (e.type == Evt::Pgm && (f1 == 0 || f1 == (int)e.canal)) return { (float)e.a, true };
        return non;
    }
    return non;
}

}  // namespace

namespace {

// Les verbes de SORTIE du moteur web n'acceptent leurs arguments que sous forme
// de CHIFFRES NUS (regex \d*). « ctl.out(r("fader1"),r("fader2")) » — pourtant
// present dans toto.nms — ne correspond donc a rien cote web : le verbe y est
// ignore et la valeur passe. Et « note.out(60,1) », la forme du moteur de
// Patrice, n'y correspond pas davantage.
// En etre plus permissif ici ferait dire a un meme script DEUX choses
// differentes selon qu'il tourne dans l'app ou sur la carte — le pire des
// resultats. On refuse donc exactement ce que le web refuse ; le segment
// retombe alors sur « verbe inconnu = passage transparent ».
bool chiffresNus(const String& a) {
    if (!a.length()) return false;
    for (int i = 0; i < (int)a.length(); i++)
        if (a[i] < '0' || a[i] > '9') return false;
    return true;
}
// Un seul argument : vide (→ defaut) ou chiffres nus. false = pas la forme web.
bool canalSeul(const String& a, int defaut, int& ch) {
    String t = a; t.trim();
    if (!t.length()) { ch = defaut; return true; }
    if (!chiffresNus(t)) return false;
    ch = (int)t.toInt();
    return true;
}

void emettre(Sortie* sorties, int max, int& n, const Sortie& s) {
    if (n < max) sorties[n++] = s;   // au-dela, on ecrete plutot que deborder
}

uint8_t sept(float v) { return (uint8_t)constrain((int)lroundf(v), 0, 127); }

// Evalue UN segment. Renvoie false si le segment bloque le pipeline — c'est le
// `null` du moteur web (sel qui ne trouve pas, block, spigot ferme, change sans
// changement) : tout ce qui suit dans ce pipeline est abandonne.
bool evaluerSegment(const String& seg, float& courant, const Evt& e,
                    Sortie* sorties, int max, int& n,
                    Etat& st, bool srcEstCcNum) {
    String a;

    // ── Arithmetique ────────────────────────────────────────────────────────
    if (verbe(seg, "*", a))   { courant *= valeurArg(a); return true; }
    if (verbe(seg, "+", a))   { courant += valeurArg(a); return true; }
    if (verbe(seg, "-", a))   { courant -= valeurArg(a); return true; }
    if (verbe(seg, "/", a))   { const float v = valeurArg(a); if (v) courant /= v; return true; }
    if (verbe(seg, "%", a))   { const float v = valeurArg(a); if (v) courant = fmodf(courant, v); return true; }
    if (verbe(seg, "div", a)) { const float v = valeurArg(a); if (v) courant = truncf(courant / v); return true; }
    if (verbe(seg, "mod", a)) { const float v = valeurArg(a); if (v) courant = fmodf(courant, v); return true; }
    if (verbe(seg, "pow", a)) { courant = powf(courant, valeurArg(a)); return true; }
    if (verbe(seg, "min", a)) { const float v = valeurArg(a); if (v < courant) courant = v; return true; }
    if (verbe(seg, "max", a)) { const float v = valeurArg(a); if (v > courant) courant = v; return true; }
    if (verbe(seg, "wrap", a)) {
        const float nn = a.length() ? valeurArg(a) : 1.0f;
        courant = nn ? fmodf(fmodf(courant, nn) + nn, nn) : 0.0f;
        return true;
    }
    if (verbe(seg, "atan2", a)) { courant = atan2f(courant, valeurArg(a)); return true; }

    // ── Binaire (sur des entiers 32 bits, comme « current|0 » cote web) ─────
    if (verbe(seg, "&", a))  { courant = (float)(versInt32(courant) &  versInt32(valeurArg(a))); return true; }
    if (verbe(seg, "|", a))  { courant = (float)(versInt32(courant) |  versInt32(valeurArg(a))); return true; }
    if (verbe(seg, "^", a))  { courant = (float)(versInt32(courant) ^  versInt32(valeurArg(a))); return true; }
    if (verbe(seg, "<<", a)) { courant = (float)(versInt32(courant) << (versInt32(valeurArg(a)) & 31)); return true; }
    if (verbe(seg, ">>", a)) { courant = (float)(versInt32(courant) >> (versInt32(valeurArg(a)) & 31)); return true; }

    // ── Comparaisons — rendent 1 ou 0 ───────────────────────────────────────
    if (verbe(seg, ">=", a)) { courant = (courant >= valeurArg(a)) ? 1.f : 0.f; return true; }
    if (verbe(seg, "<=", a)) { courant = (courant <= valeurArg(a)) ? 1.f : 0.f; return true; }
    if (verbe(seg, "==", a)) { courant = (courant == valeurArg(a)) ? 1.f : 0.f; return true; }
    if (verbe(seg, "!=", a)) { courant = (courant != valeurArg(a)) ? 1.f : 0.f; return true; }
    if (verbe(seg, ">", a))  { courant = (courant >  valeurArg(a)) ? 1.f : 0.f; return true; }
    if (verbe(seg, "<", a))  { courant = (courant <  valeurArg(a)) ? 1.f : 0.f; return true; }

    // ── Logique ─────────────────────────────────────────────────────────────
    if (verbe(seg, "and", a) || verbe(seg, "or", a)) {
        const bool et = seg.startsWith("and(");
        String m[2];
        if (decouperArgs(a, m, 2) == 2) {
            const float x = valeurArg(m[0]), y = valeurArg(m[1]);
            courant = et ? (((courant != 0) && (x != 0) && (y != 0)) ? 1.f : 0.f)
                         : (((courant != 0) || (x != 0) || (y != 0)) ? 1.f : 0.f);
        }
        return true;
    }

    // ── Unaires ─────────────────────────────────────────────────────────────
    if (seg == "not()")   { courant = (courant == 0) ? 1.f : 0.f; return true; }
    if (seg == "abs()")   { courant = fabsf(courant); return true; }
    if (seg == "round()") { courant = roundf(courant); return true; }
    if (seg == "floor()") { courant = floorf(courant); return true; }
    if (seg == "ceil()")  { courant = ceilf(courant);  return true; }
    if (seg == "inv()")   { courant = 1.0f - courant;  return true; }
    if (seg == "neg()")   { courant = -courant;        return true; }
    if (seg == "sqrt()")  { courant = sqrtf(fabsf(courant)); return true; }
    if (seg == "sin()")   { courant = sinf(courant);   return true; }
    if (seg == "cos()")   { courant = cosf(courant);   return true; }
    if (seg == "tan()")   { courant = tanf(courant);   return true; }
    if (seg == "atan()")  { courant = atanf(courant);  return true; }
    if (seg == "log()")   { courant = (courant > 0) ? logf(courant) : 0.f; return true; }
    if (seg == "exp()")   { courant = expf(courant);   return true; }
    if (seg == "int()")   { courant = truncf(courant); return true; }
    if (seg == "float()") { return true; }

    // ── Conversions musicales (formules de Pd, comme le moteur web) ─────────
    if (seg == "mtof()")    { courant = 440.f * powf(2.f, (courant - 69.f) / 12.f); return true; }
    if (seg == "ftom()")    { courant = 69.f + 12.f * log2f(fmaxf(1e-10f, courant) / 440.f); return true; }
    if (seg == "dbtopow()") { courant = powf(10.f, courant / 10.f); return true; }
    if (seg == "powtodb()") { courant = 10.f * log10f(fmaxf(1e-10f, courant)); return true; }
    if (seg == "dbtorms()") { courant = powf(10.f, courant / 20.f); return true; }
    if (seg == "rmstodb()") { courant = 20.f * log10f(fmaxf(1e-10f, courant)); return true; }

    // ── Mise a l'echelle ────────────────────────────────────────────────────
    if (verbe(seg, "scale", a)) {
        String m[4];
        if (decouperArgs(a, m, 4) == 4) {
            const float imin = valeurArg(m[0]), imax = valeurArg(m[1]);
            const float omin = valeurArg(m[2]), omax = valeurArg(m[3]);
            courant = (imax == imin) ? omin : omin + (courant - imin) / (imax - imin) * (omax - omin);
        }
        return true;
    }
    if (verbe(seg, "clamp", a)) {
        String m[2];
        if (decouperArgs(a, m, 2) == 2) {
            const float lo = valeurArg(m[0]), hi = valeurArg(m[1]);
            courant = fmaxf(lo, fminf(hi, courant));
        }
        return true;
    }
    // curve(exp) — courbe de puissance sur la plage 0..127.
    if (verbe(seg, "curve", a)) {
        const float ex = valeurArg(a);
        const float norme = fmaxf(0.f, fminf(1.f, courant / 127.f));
        courant = powf(norme, ex) * 127.f;
        return true;
    }

    // ── Aiguillage : sel / map / block ──────────────────────────────────────
    if (verbe(seg, "sel", a)) {
        String m[12];
        const int k = decouperArgs(a, m, 12);
        for (int i = 0; i < k; i++)
            if (valeurArg(m[i]) == courant) { st.selIdx = (int16_t)i; return true; }
        return false;                       // non selectionne : on bloque
    }
    if (verbe(seg, "map", a)) {
        String m[12];
        const int k = decouperArgs(a, m, 12);
        if (st.selIdx < 0 || st.selIdx >= k) return false;
        courant = valeurArg(m[st.selIdx]);
        return true;
    }
    if (verbe(seg, "block", a)) {
        String m[12];
        const int k = decouperArgs(a, m, 12);
        for (int i = 0; i < k; i++) if (valeurArg(m[i]) == courant) return false;
        return true;
    }
    if (seg == "stripnote()") {
        if (e.type == Evt::NoteOff) return false;
        if (e.type == Evt::NoteOn && e.b == 0) return false;
        return true;
    }

    // ── Registre ────────────────────────────────────────────────────────────
    // s("param","f",min,max) est un afficheur : passage transparent. Teste
    // AVANT s("x"), qui l'avalerait.
    if (verbe(seg, "s", a) || verbe(seg, "send", a)) {
        String m[6];
        const int k = decouperArgs(a, m, 6);
        if (k == 0) return true;
        String nom = m[0]; nom.trim();
        if (nom.length() >= 2 && nom[0] == '"' && nom[(int)nom.length() - 1] == '"')
            nom = nom.substring(1, (int)nom.length() - 1);
        if (nom == "param") {
            if (k >= 2) {                    // s("param","fader",min,max) : affichage
                String f = m[1]; f.trim();
                if (f.length() >= 2 && f[0] == '"' && f[(int)f.length() - 1] == '"')
                    f = f.substring(1, (int)f.length() - 1);
                // Rien a afficher sur une carte sans ecran, mais on PUBLIE la
                // valeur : c'est ce qui permet a l'app de la lire, et a un
                // autre pipeline de la relire par r("fader").
                FluxRegistry::update(f.c_str(), courant);
            }
            return true;
        }
        FluxRegistry::update(nom.c_str(), courant);
        return true;
    }
    if (verbe(seg, "r", a) || verbe(seg, "receive", a)) { courant = valeurArg(seg); return true; }

    // ── Etat ────────────────────────────────────────────────────────────────
    if (seg == "toggle()") { st.toggle = st.toggle ? 0 : 1; courant = (float)st.toggle; return true; }
    if (verbe(seg, "counter", a)) {
        String m[3];
        const int k = decouperArgs(a, m, 3);
        if (k >= 2) {
            const float mn = valeurArg(m[0]), mx = valeurArg(m[1]);
            const float pas = (k >= 3) ? valeurArg(m[2]) : 1.0f;
            float v = isnan(st.compteur) ? mn : st.compteur;
            v += pas;
            if (v > mx) v = mn;
            st.compteur = v;
            courant = v;
        }
        return true;
    }
    if (verbe(seg, "seq", a)) {
        String m[16];
        const int k = decouperArgs(a, m, 16);
        if (k > 0) {
            st.seq = (int16_t)((st.seq + 1) % k);
            courant = valeurArg(m[st.seq]);
        }
        return true;
    }
    if (seg == "rand()") { courant = (float)rand() / (float)RAND_MAX; return true; }
    if (verbe(seg, "rand", a)) {
        String m[2];
        if (decouperArgs(a, m, 2) == 2) {
            const float mn = valeurArg(m[0]), mx = valeurArg(m[1]);
            courant = mn + ((float)rand() / (float)RAND_MAX) * (mx - mn);
        }
        return true;
    }
    if (verbe(seg, "drunk", a)) {
        String m[3];
        if (decouperArgs(a, m, 3) == 3) {
            const float pas = valeurArg(m[0]), mn = valeurArg(m[1]), mx = valeurArg(m[2]);
            float v = isnan(st.drunk) ? (mn + mx) / 2.f : st.drunk;
            v += ((float)rand() / (float)RAND_MAX * 2.f - 1.f) * pas;
            v = fmaxf(mn, fminf(mx, v));
            st.drunk = v;
            courant = v;
        }
        return true;
    }
    if (seg == "change()") {
        if (!isnan(st.change) && st.change == courant) return false;
        st.change = courant;
        return true;
    }
    if (verbe(seg, "spigot", a) || verbe(seg, "gate", a)) return valeurArg(a) != 0;
    if (verbe(seg, "lp", a)) {
        const float c = fmaxf(0.f, fminf(1.f, valeurArg(a)));
        const float prec = isnan(st.lp) ? courant : st.lp;
        st.lp = prec + c * (courant - prec);
        courant = st.lp;
        return true;
    }
    if (verbe(seg, "hysteresis", a)) {
        String m[2];
        if (decouperArgs(a, m, 2) == 2) {
            const float lo = valeurArg(m[0]), hi = valeurArg(m[1]);
            if (courant >= hi) st.hyst = 1;
            else if (courant <= lo) st.hyst = 0;
            courant = (float)st.hyst;
        }
        return true;
    }

    // ── Affichage — passage transparent ─────────────────────────────────────
    // print() va au JOURNAL, pas dans la liste des sorties : cote web il
    // n'engendre aucun outEvent, et en faire un ici consommerait une place et
    // ferait diverger les deux moteurs.
    if (verbe(seg, "print", a)) {
        String etiquette = a; etiquette.trim();
        if (etiquette.length() >= 2 && etiquette[0] == '"')
            etiquette = etiquette.substring(1, (int)etiquette.length() - 1);
        if (g_impression)
            g_impression(etiquette.length() ? etiquette.c_str() : "out", courant);
        return true;
    }
    if (verbe(seg, "num", a) || verbe(seg, "n", a) || verbe(seg, "number", a)) return true;
    if (verbe(seg, "bang", a) || verbe(seg, "b", a)) return true;

    // ── Sorties ─────────────────────────────────────────────────────────────
    // Toutes reconstruisent un message complet meme quand l'evenement d'entree
    // ne porte pas les champs attendus (source counter, seq, f...). Defauts :
    // velocite 127, canal 1 — comme le moteur web.
    if (verbe(seg, "note.out.vel", a)) {
        int ch; if (!canalSeul(a, 1, ch)) return true;
        Sortie s;
        s.canal = (uint8_t)ch;
        s.a = (e.type == Evt::NoteOn || e.type == Evt::NoteOff || e.type == Evt::PolyTouch)
              ? e.a : 60;
        if (e.type == Evt::NoteOff) { s.type = Sortie::NoteOff; s.b = 0; }
        else                        { s.type = Sortie::Note;    s.b = sept(courant); }
        emettre(sorties, max, n, s);
        return true;
    }
    if (verbe(seg, "note.out", a)) {
        int ch; if (!canalSeul(a, 1, ch)) return true;
        Sortie s;
        s.canal = (uint8_t)ch;
        s.a = sept(courant);
        if (e.type == Evt::NoteOff) { s.type = Sortie::NoteOff; s.b = 0; }
        else {
            s.type = Sortie::Note;
            s.b = (e.type == Evt::NoteOn) ? e.b : 127;
        }
        emettre(sorties, max, n, s);
        return true;
    }
    if (verbe(seg, "noteoff.out", a)) {
        int chn; if (!canalSeul(a, 1, chn)) return true;
        const uint8_t ch = (uint8_t)chn;
        const int v = (int)lroundf(courant);
        if (v == 128) {                       // 128 = toutes les notes
            for (int i = 0; i < 128; i++) {
                Sortie s; s.type = Sortie::NoteOff; s.canal = ch; s.a = (uint8_t)i; s.b = 0;
                emettre(sorties, max, n, s);
            }
        } else {
            Sortie s; s.type = Sortie::NoteOff; s.canal = ch; s.a = sept((float)v); s.b = 0;
            emettre(sorties, max, n, s);
        }
        return true;
    }
    if (verbe(seg, "ctl.out", a)) {
        String m[3];
        const int k = decouperArgs(a, m, 3);
        if (k > 2) return true;                                  // pas la forme web
        if (k >= 1 && !chiffresNus(m[0])) return true;
        if (k >= 2 && !chiffresNus(m[1])) return true;
        Sortie s; s.type = Sortie::Cc;
        s.canal = (k >= 1) ? (uint8_t)m[0].toInt()
                           : (uint8_t)(e.canal ? e.canal : 1);
        if (k >= 2)          { s.a = sept((float)m[1].toInt()); s.b = sept(courant); }
        else if (srcEstCcNum){ s.a = sept(courant);             s.b = (e.type == Evt::Cc) ? e.b : 0; }
        else                 { s.a = (e.type == Evt::Cc) ? e.a : 0; s.b = sept(courant); }
        emettre(sorties, max, n, s);
        return true;
    }
    if (verbe(seg, "bend.out", a)) {
        int ch; if (!canalSeul(a, 1, ch)) return true;
        Sortie s; s.type = Sortie::Bend;
        s.canal = (uint8_t)ch;
        s.valeur14 = (int16_t)constrain((int)lroundf(courant), -8192, 8191);
        emettre(sorties, max, n, s);
        return true;
    }
    if (verbe(seg, "touch.out", a)) {
        int ch; if (!canalSeul(a, 1, ch)) return true;
        Sortie s; s.type = Sortie::Touch;
        s.canal = (uint8_t)ch;
        s.a = sept(courant);
        emettre(sorties, max, n, s);
        return true;
    }
    if (verbe(seg, "pgm.out", a)) {
        int ch; if (!canalSeul(a, 1, ch)) return true;
        Sortie s; s.type = Sortie::Pgm;
        s.canal = (uint8_t)ch;
        s.a = sept(courant);
        emettre(sorties, max, n, s);
        return true;
    }

    // Verbe inconnu : passage transparent, comme le moteur web (qui rend
    // `current` a la fin). Un .nms ecrit pour le web ne casse donc pas sur la
    // carte — il fait seulement moins.
    return true;
}

}  // namespace

int MappingEngine::executer(const char* script, const Evenement& evt,
                            Sortie* sorties, int max, bool& traite,
                            Etat* etats, int nEtats) {
    if (!etats || nEtats <= 0) { etats = g_etats; nEtats = MAX_PIPELINES; }
    traite = false;
    int n = 0;
    if (!script || script[0] == '\0' || !sorties || max <= 0) return 0;

    const String s = nettoyer(script);
    const Famille fEvt = familleEvenement(evt);

    int pi = 0, debutPipe = 0;
    while (debutPipe <= (int)s.length()) {
        const int finPipe = prochainSep(s, debutPipe, ';');
        const String pipe = s.substring(debutPipe, (finPipe == -1) ? s.length() : finPipe);

        // Le premier segment est la SOURCE : c'est lui qui decide si le
        // pipeline tourne pour cet evenement.
        int finSrc = prochainSep(pipe, 0, ':');
        String src = pipe.substring(0, (finSrc == -1) ? pipe.length() : finSrc);
        src.trim();

        if (src.length()) {
            // L'etat du pipeline AVANT d'evaluer la source : metro() et
            // loadbang() y tiennent leur minuterie.
            Etat& stSrc = etats[(pi < nEtats) ? pi : nEtats - 1];
            const Source d = evaluerSource(src, evt, stSrc);
            if (d.declenche) {
                // « Pris en charge » seulement si la source repond au GENRE de
                // l'evenement. Un pipeline pilote par f() ou counter() tourne
                // aussi, mais n'a pas pris la note en charge et ne doit donc
                // pas la bloquer.
                if (fEvt != Famille::Aucune && familleSource(src) == fEvt) traite = true;

                // Au-dela du nombre de slots, les pipelines partagent le dernier : un
                // script plus long garde un etat *coherent*, faute d'etre separe.
                // (Deja resolu au-dessus pour metro()/loadbang().)
                Etat& st = stSrc;
                const bool srcCcNum = src.startsWith("ccnum.in");
                float courant = d.valeur;

                int debut = (finSrc == -1) ? (int)pipe.length() + 1 : finSrc + 1;
                while (debut <= (int)pipe.length()) {
                    const int fin = prochainSep(pipe, debut, ':');
                    String seg = pipe.substring(debut, (fin == -1) ? pipe.length() : fin);
                    seg.trim();
                    if (seg.length() &&
                        !evaluerSegment(seg, courant, evt, sorties, max, n, st, srcCcNum))
                        break;                      // segment bloquant
                    if (fin == -1) break;
                    debut = fin + 1;
                }
            }
            pi++;
        }

        if (finPipe == -1) break;
        debutPipe = finPipe + 1;
    }
    return n;
}

// Canal MIDI valide. Le PIPELINE ne borne rien — le moteur web ne borne pas
// non plus, et s'en ecarter ferait diverger les deux sur « ctl.out(74,1) ».
// Mais un canal hors 1..16 ne doit jamais atteindre un octet de statut : la
// garde vit donc ICI, au bord, la ou le MIDI est reellement ecrit.
static uint8_t canalValide(uint8_t c) { return (uint8_t)constrain((int)c, 1, 16); }

// ── Enveloppes historiques ─────────────────────────────────────────────────
// MidiRouter parle encore en « une note » / « un CC ». Elles prennent le
// PREMIER evenement de leur genre dans la liste. Les garder evite de reecrire
// les appelants dans le meme mouvement que le moteur — et le jour ou la carte
// saura emettre plusieurs notes, c'est ici qu'on regardera.
bool MappingEngine::executeMidiNote(const char* script,
                                    uint8_t noteIn, uint8_t veloIn, uint8_t canalIn,
                                    bool estNoteOff, SortieNote& sortie) {
    sortie.emise = sortie.traite = false;
    Evenement e;
    e.type  = estNoteOff ? Evenement::NoteOff : Evenement::NoteOn;
    e.canal = canalIn; e.a = noteIn; e.b = veloIn;

    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite);
    sortie.traite = traite;
    for (int i = 0; i < n; i++) {
        if (liste[i].type != Sortie::Note && liste[i].type != Sortie::NoteOff) continue;
        sortie.note  = liste[i].a;
        sortie.velo  = (liste[i].type == Sortie::NoteOff) ? 0 : liste[i].b;
        sortie.canal = canalValide(liste[i].canal);
        sortie.emise = true;
        break;
    }
    return sortie.emise;
}

bool MappingEngine::executeMidiCc(const char* script,
                                  uint8_t ccIn, uint8_t valeurIn, uint8_t canalIn,
                                  SortieCc& sortie) {
    sortie.emise = sortie.traite = false;
    Evenement e;
    e.type = Evenement::Cc;
    e.canal = canalIn; e.a = ccIn; e.b = valeurIn;

    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite);
    sortie.traite = traite;
    for (int i = 0; i < n; i++) {
        if (liste[i].type != Sortie::Cc) continue;
        sortie.cc     = liste[i].a;
        sortie.valeur = liste[i].b;
        sortie.canal  = canalValide(liste[i].canal);
        sortie.emise  = true;
        break;
    }
    return sortie.emise;
}

// ── Capteurs ───────────────────────────────────────────────────────────────
// Remplace l'interpreteur d'origine (10 verbes, un flottant, une chaine de ':').
// Un capteur passe maintenant par LE MEME moteur que le MIDI entrant, donc par
// le meme vocabulaire : scale, clamp, curve, sel/map, hysteresis, lp, counter…
// Un script de capteur et un script de mapping disent desormais la meme chose.
#ifndef NMS_BANC_HOTE
/* Emission d'une liste de sorties. Extrait d'executerCapteur pour que le
 * battement d'horloge emette EXACTEMENT de la meme facon — deux copies auraient
 * fini par diverger sur un type de message. */
static void emettreVers(MidiSender* sender, const MappingEngine::Sortie* liste, int n) {
    if (!sender) return;
    for (int i = 0; i < n; i++) {
        const MappingEngine::Sortie& s = liste[i];
        const uint8_t ch = (uint8_t)constrain((int)s.canal, 1, 16);
        switch (s.type) {
            case MappingEngine::Sortie::Note:      sender->sendNoteOn(ch, s.a, s.b);        break;
            case MappingEngine::Sortie::NoteOff:   sender->sendNoteOff(ch, s.a, s.b);       break;
            case MappingEngine::Sortie::Cc:        sender->sendControlChange(ch, s.a, s.b); break;
            case MappingEngine::Sortie::Bend:      sender->sendPitchBend(ch, s.valeur14);   break;
            case MappingEngine::Sortie::Touch:     sender->sendAftertouch(ch, s.a);         break;
            case MappingEngine::Sortie::PolyTouch: sender->sendKeyPressure(ch, s.a, s.b);   break;
            case MappingEngine::Sortie::Pgm:       sender->sendProgramChange(ch, s.a);      break;
            case MappingEngine::Sortie::Print:     break;                                   // deja au journal
        }
    }
}

void MappingEngine::executerCapteur(const char* script, float valeur,
                                    MidiSender* sender, Etat* etats, int nEtats,
                                    float brut) {
    if (!script || script[0] == '\0') return;

    // « in » : la poignee conventionnelle sur la valeur qui vient de declencher
    // le script. Le composant publie deja sous SON nom quand il en a un ; ceci
    // garantit qu'un composant sans nom reste scriptable. Ce n'est pas une
    // extension de la langue — r() existe deja.
    FluxRegistry::update("in", valeur);
    /* Et la lecture native sous « raw », pour raw.in(). Quand l'appelant n'en
     * fournit pas de distincte, les deux coincident — c'est le cas d'un contact,
     * qui n'a rien de plus fin que 0/1. */
    FluxRegistry::update("raw", isnan(brut) ? valeur : brut);

    Evenement e;                       // sans famille : seules r/f/i/litteral tirent
    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
    emettreVers(sender, liste, n);
}

/* Le battement d'horloge : fait tourner les pipelines qui n'attendent aucun
 * MIDI — metro() sur Tick, loadbang() sur Init. Meme emission que pour un
 * capteur : un script generaliste (DMX, OSC, MIDI) n'a pas a savoir d'ou vient
 * le declencheur. */
void MappingEngine::battre(const char* script, Evenement::Type type, uint32_t instant,
                           MidiSender* sender, Etat* etats, int nEtats) {
    if (!script || script[0] == '\0') return;
    Evenement e;
    e.type = type;
    e.instant = instant;
    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
    if (n > 0) emettreVers(sender, liste, n);
}
#endif  // NMS_BANC_HOTE
