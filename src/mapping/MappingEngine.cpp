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

/* Les reprises en attente. Reservoir BORNE et global : une carte n'a pas de tas
 * a gaspiller, et un script qui differe sans fin doit buter sur une limite
 * visible plutot que sur une allocation qui echoue. */
struct Reprise {
    const char* script = nullptr;
    MappingEngine::Etat* etats = nullptr;
    int      nEtats = 0;
    uint8_t  pipe = 0, seg = 0;
    float    valeur = 0;
    uint32_t echeance = 0;
    MappingEngine::Evenement evt;
    bool     actif = false;
    bool     horsLigne = false;   // simulee : la boucle MIDI ne la vide pas

    /* DEUX ESPECES de reprise.
     *   Differee — del(), makenote() : une valeur mise de cote, rejouee UNE
     *              fois quand `echeance` est passee, puis oubliee.
     *   Continue — lag(), ramp() : une interpolation qui rend une valeur A
     *              CHAQUE battement jusqu'a son terme. Pas d'echeance : elle
     *              tire tant que t < 1.
     * Une tache continue est UNIQUE par (script, pipeline, segment) : un
     * nouvel evenement remplace le glissando en cours au lieu de s'y ajouter,
     * comme la cle `lag_${pi}:${si}` du moteur de reference. */
    uint8_t  genre = 0;
    float    de = 0, vers = 0;    // bornes de l'interpolation
    uint32_t debut = 0, duree = 0;
};
enum : uint8_t { RepDifferee = 0, RepLag = 1, RepRampe = 2 };
constexpr int MAX_REPRISES = 16;
Reprise g_reprises[MAX_REPRISES];

/* L'HORLOGE DU MOTEUR, tenue par le battement.
 * del() s'en sert plutot que de millis() : c'est ce que fait la reference
 * (this._now, mis a jour par tick()), et c'est ce qui rend le verbe
 * EPROUVABLE — un banc peut avancer cette horloge, pas le temps mural. */
uint32_t g_maintenant = 0;

/* L'horloge de la SIMULATION, tenue a part. Sur la carte, l'horloge vivante
 * vaut millis() — des millions — pendant que le banc compte a partir de zero :
 * une echeance posee dans un temps et relue dans l'autre n'arrive jamais. Deux
 * lignes de temps, deux horloges, chacune ecrite par une seule tache. */
uint32_t g_horsLigneMaintenant = 0;


MappingEngine::Impression g_impression = nullptr;
MappingEngine::EmetteurOsc g_emetteurOsc = nullptr;

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
void MappingEngine::surOsc(EmetteurOsc fn) { g_emetteurOsc = fn; }

void MappingEngine::reinitialiser() {
    for (int i = 0; i < MAX_PIPELINES; i++) g_etats[i].reinitialiser();
    /* Et les reprises en attente : elles portent un POINTEUR vers le texte du
     * script. Changer de script libere ce texte — rejouer ensuite lirait de la
     * memoire rendue. */
    for (int i = 0; i < MAX_REPRISES; i++) g_reprises[i].actif = false;
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
    /* in([n]) — L'ENTREE de ce composant. `inlet([n])` en est l'alias exact.
     *
     * Ce que faisait r("in"), mais en le DISANT. r() lit le bus PARTAGE ; rien
     * dans son texte ne distinguait une lecture locale d'une lecture de bus, et
     * un s("in") ecrit ailleurs serait entre en collision avec l'entree du
     * composant. Le vocabulaire vient de Pd et de Max : un objet recoit par ses
     * inlets, numerotes a partir de 0, de gauche a droite.
     *
     * Un composant a plusieurs donnees les expose dans cet ordre — pour un
     * joystick, in(0)=X, in(1)=Y, in(2)=Z. Sans inlet a ce rang on rend 0
     * PLUTOT QUE DE NE PAS TIRER : un pipeline qui lit un axe absent doit
     * donner une valeur neutre, pas disparaitre en silence. */
    if (verbe(seg, "in", args) || verbe(seg, "inlet", args)) {
        const int n = args.length() ? (int)args.toInt() : 0;
        return { (n >= 0 && n < MappingEngine::MAX_INLETS) ? e.inlets[n] : 0.0f, true };
    }
    /* raw.in([n]) — la meme lecture dans la RESOLUTION NATIVE du capteur :
     * 0..4095 pour un ADC 12 bits, 0/1 pour un contact, la ou in() rend un
     * flottant NORMALISE de 0 a 1.
     *
     * Une entree ne porte AUCUNE echelle de protocole : la mise a l'echelle
     * MIDI — comme celle du DMX ou de l'OSC — appartient au script, qui ecrit
     * « in() : *(127) : ctl.out(1,7) ». Le conditionnement, lui (filtre, course
     * utile, hysteresis), s'applique dans les deux cas : il releve de la lecture
     * du capteur, pas de l'intention musicale. */
    if (verbe(seg, "raw.in", args)) {
        const int n = args.length() ? (int)args.toInt() : 0;
        return { (n >= 0 && n < MappingEngine::MAX_INLETS) ? e.raws[n] : 0.0f, true };
    }

    /* osc.in("/adresse") — la source qui repond a un message OSC entrant.
     * Correspondance EXACTE ou par PREFIXE DE SEGMENT : "/x" repond a "/x" et
     * a "/x/y", mais pas a "/xy". La valeur qui entre dans le pipeline est le
     * premier argument du message. */
    if (verbe(seg, "osc.in", args)) {
        String adr = args; adr.trim();
        if (adr.length() >= 2 && (adr[0] == '"' || adr[0] == '\''))
            adr = adr.substring(1, adr.length() - 1);
        if (e.type != Evt::Osc || !adr.length()) return non;
        const String recue = String(e.adresse);
        const String prefixe = adr + "/";
        if (recue == adr || recue.startsWith(prefixe.c_str())) return { e.reel, true };
        return non;
    }
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

/* ARRONDI DU MOTEUR DE REFERENCE : la moitie va vers le HAUT, pas a l'oppose
 * de zero. Math.round(-0.5) rend 0 en JavaScript, la ou lroundf(-0.5) rend -1.
 * Sans egard, c'est invisible tant que les valeurs restent positives — et le
 * banc l'a trouve sur « in() : *(16383) : -(8192) : bend.out(1) » avec une
 * entree a mi-course : 8191,5 - 8192 = -0,5, donc 0 d'un cote et -1 de l'autre.
 * Un demi-pas de pitch bend, mais un ECART de langage. */
long arrondi(float v) { return (long)floorf(v + 0.5f); }

uint8_t sept(float v) { return (uint8_t)constrain((int)arrondi(v), 0, 127); }

// Evalue UN segment. Renvoie false si le segment bloque le pipeline — c'est le
// `null` du moteur web (sel qui ne trouve pas, block, spigot ferme, change sans
// changement) : tout ce qui suit dans ce pipeline est abandonne.
/* OU se trouve le segment qu'on evalue.
 *
 * Les verbes de TEMPS — del, makenote, lag, ramp — ne rendent pas leur valeur
 * tout de suite : ils la mettent de cote et rejouent l'AVAL du pipeline plus
 * tard. Pour ca il faut savoir quel script, quel pipeline, quel segment. Le
 * moteur web garde ces informations dans des cles de son etat ; ici on les
 * passe, ce qui evite un dictionnaire sur une carte qui n'en a pas les moyens. */
struct Contexte {
    const char* script = nullptr;
    Etat*   etats  = nullptr;
    int     nEtats = 0;
    uint8_t pipe   = 0;
    uint8_t seg    = 0;
    bool    horsLigne = false;   // cf. executer(..., horsLigne)
};

/* L'horloge de CE contexte. Une simulation et la carte vivante ne comptent pas
 * dans le meme temps ; lire la mauvaise, c'est poser une echeance qui n'arrive
 * jamais. */
uint32_t horlogeDe(const Contexte* c) {
    return (c && c->horsLigne) ? g_horsLigneMaintenant : g_maintenant;
}

/* Le tableau d'etats REELLEMENT employe : nullptr veut dire « celui du moteur ». */
MappingEngine::Etat& etatDe(MappingEngine::Etat* etats, int nEtats, uint8_t pipe) {
    if (!etats || nEtats <= 0) { etats = g_etats; nEtats = MAX_PIPELINES; }
    return etats[(pipe < nEtats) ? pipe : nEtats - 1];
}

/* Arme (ou REARME) une tache continue. Deux passes : un glissando deja en cours
 * sur ce meme segment est repris, sinon on prend un emplacement libre. */
bool continuer(const Contexte* c, uint8_t genre, float de, float vers,
               uint32_t debut, uint32_t duree, const Evt& e) {
    if (!c || !c->script) return false;
    Reprise* cible = nullptr;
    for (int i = 0; i < MAX_REPRISES && !cible; i++) {
        Reprise& r = g_reprises[i];
        if (r.actif && r.genre == genre && r.script == c->script &&
            r.pipe == c->pipe && r.seg == c->seg) cible = &r;
    }
    for (int i = 0; i < MAX_REPRISES && !cible; i++)
        if (!g_reprises[i].actif) cible = &g_reprises[i];
    if (!cible) {
        Serial.println("[nms] file des differes pleine — une interpolation est perdue");
        return false;
    }
    cible->script = c->script; cible->etats = c->etats; cible->nEtats = c->nEtats;
    cible->pipe = c->pipe;     cible->seg = c->seg;
    cible->genre = genre;      cible->horsLigne = c->horsLigne;
    cible->de = de;            cible->vers = vers;
    cible->debut = debut;      cible->duree = duree;
    cible->valeur = de;        cible->echeance = debut;
    cible->evt = e;            cible->actif = true;
    return true;
}

bool differer(const Contexte* c, float valeur, const Evt& e, uint32_t echeance) {
    if (!c || !c->script) return false;
    for (int i = 0; i < MAX_REPRISES; i++) {
        if (g_reprises[i].actif) continue;
        Reprise& r = g_reprises[i];
        r.script = c->script; r.etats = c->etats; r.nEtats = c->nEtats;
        r.pipe = c->pipe;     r.seg = c->seg;
        r.valeur = valeur;    r.echeance = echeance;
        r.evt = e;            r.actif = true;
        r.horsLigne = c->horsLigne;   // la reprise reste dans SA ligne de temps
        r.genre = RepDifferee;
        return true;
    }
    /* Reservoir plein : on le DIT. Un differe qui disparait en silence est le
     * genre de panne qu'on passe cette session a supprimer. */
    Serial.println("[nms] file des differes pleine — une valeur est perdue");
    return false;
}

/* `e` n'est PAS const : makenote() reecrit l'evenement lui-meme — sa nature,
 * sa note, sa velocite. C'est ainsi que le moteur de reference procede, et
 * c'est ce qui permet a un note.out() en aval d'emettre la note fabriquee
 * plutot que celle qui est entree. */
bool evaluerSegment(const String& seg, float& courant, Evt& e,
                    Sortie* sorties, int max, int& n,
                    Etat& st, bool srcEstCcNum, const Contexte* ctx) {
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
    /* debounce(ms) — laisse passer, puis se tait pendant `ms`.
     *
     * Le moteur web lit Date.now() ; ici c'est millis(). Meme semantique — un
     * temps mural, pas le tick — donc un pipeline d'evenements peut s'en servir
     * sans qu'une horloge tourne. */
    /* del(ms) — met la valeur de cote et rend la main ; l'aval du pipeline sera
     * rejoue quand l'echeance sera passee. Le pipeline s'arrete ICI pour cet
     * evenement : c'est ce que rend le moteur web en renvoyant null. */
    if (verbe(seg, "del", a)) {
        const long ms = (long)valeurArg(a);
        if (ms <= 0) return true;                 // sans delai, on continue tout droit
        differer(ctx, courant, e, horlogeDe(ctx) + (uint32_t)ms);
        return false;
    }

    /* makenote([vel[, duree]]) — fabrique une NOTE a partir de la valeur qui
     * passe. Ce verbe ne sort rien de lui-meme : il reecrit l'evenement, et
     * c'est un note.out() en aval qui emet. Deux formes :
     *   makenote(vel, duree) — note-on maintenant, note-off apres `duree` ms,
     *                          l'aval etant rejoue pour le off.
     *   makenote([vel])      — BASCULE : un evenement ouvre la note, le
     *                          suivant la ferme. De quoi tenir une note avec
     *                          un bouton qui n'envoie qu'une impulsion. */
    if (verbe(seg, "makenote", a)) {
        // Rien au chargement : loadbang parcourt les pipelines, et une bascule
        // qui s'inverse a l'allumage serait une note fantome.
        if (e.type == Evt::Init) return true;
        String m[2];
        const int k = decouperArgs(a, m, 2);
        const uint8_t vel  = (k >= 1 && m[0].length()) ? sept(valeurArg(m[0])) : 127;
        const uint8_t note = sept(courant);
        const uint8_t ch   = e.canal ? e.canal : 1;
        if (k >= 2) {
            const long ms = (long)valeurArg(m[1]);
            e.type = Evt::NoteOn; e.a = note; e.b = vel; e.canal = ch;
            if (ms > 0) {
                Evt off = e;
                off.type = Evt::NoteOff; off.a = note; off.b = 0; off.canal = ch;
                differer(ctx, courant, off, horlogeDe(ctx) + (uint32_t)ms);
            }
            return true;
        }
        if (st.mnNote == (int8_t)note) {          // deja ouverte : on la ferme
            e.type = Evt::NoteOff; e.a = note; e.b = 0; e.canal = ch;
            st.mnNote = -1;
        } else {
            e.type = Evt::NoteOn;  e.a = note; e.b = vel; e.canal = ch;
            st.mnNote = (int8_t)note;
        }
        return true;
    }

    /* lag(monteeMs[, descenteMs]) — glisse vers la valeur recue au lieu d'y
     * sauter. Rend TOUT DE SUITE la position courante ; c'est le battement qui
     * la fait avancer. Avec deux arguments, la montee et la descente ont des
     * durees differentes — ce qu'un potentiometre ou un filtre demandent
     * souvent (attaque vive, retour lent). */
    if (verbe(seg, "lag", a)) {
        String m[2];
        const int k = decouperArgs(a, m, 2);
        if (k < 1) return true;
        const float monte   = valeurArg(m[0]);
        const float descend = (k >= 2) ? valeurArg(m[1]) : monte;
        /* La position S'INSTALLE a la premiere lecture. C'est la semantique du
         * moteur de reference, dont le lecteur d'etat ECRIT son defaut : le
         * premier evenement ne glisse donc pas — il pose le point de depart —
         * et c'est le deuxieme qui part de la. Sans ca, lag() ne glissait
         * jamais : la position valait toujours la valeur recue. */
        if (isnan(st.lagCur)) st.lagCur = courant;
        const float actuel = st.lagCur;
        if (actuel != courant) {
            const float ms = (courant >= actuel) ? monte : descend;
            // Duree nulle : pas de glissando, on se pose sur la valeur.
            if (ms <= 0) { st.lagCur = courant; return true; }
            continuer(ctx, RepLag, actuel, courant, horlogeDe(ctx), (uint32_t)ms, e);
        }
        courant = st.lagCur;                  // la position, pas la consigne
        return true;
    }

    /* ramp(depart, arrivee, ms) — une rampe qui IGNORE la valeur amont : elle
     * fabrique la sienne. Chaque evenement la relance depuis le depart. */
    if (verbe(seg, "ramp", a)) {
        String m[3];
        if (decouperArgs(a, m, 3) == 3) {
            const float de = valeurArg(m[0]), vers = valeurArg(m[1]);
            const float ms = valeurArg(m[2]);
            if (ms <= 0) { courant = vers; return true; }
            continuer(ctx, RepRampe, de, vers, horlogeDe(ctx), (uint32_t)ms, e);
            courant = de;                     // la premiere valeur sort tout de suite
        }
        return true;
    }

    if (verbe(seg, "debounce", a)) {
        const long ms = (long)valeurArg(a);
        const uint32_t maintenant = millis();
        if (ms > 0 && st.debounceDernier != 0 &&
            (uint32_t)(maintenant - st.debounceDernier) < (uint32_t)ms) return false;
        st.debounceDernier = maintenant;
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
            g_impression(e.origine, etiquette.length() ? etiquette.c_str() : "out", courant);
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
        const int v = (int)arrondi(courant);
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
    /* osc.out("/adresse"[, a, b, ...]) — emet un message OSC. La charge est
     * [valeur courante, ...arguments froids]. Passage TRANSPARENT : le verbe
     * rend la valeur, il peut donc se poser au milieu d'un pipeline.
     *
     * osc.out(N, "/adresse"[, ...]) — un NOMBRE NU en premier n'est pas une
     * adresse : c'est un octet d'hote. Sur un poste de travail le moteur de
     * reference y lit 127.0.0.N ; sur la carte, c'est le dernier octet a
     * substituer dans la cible configuree. Meme valeur transportee, lecture
     * adaptee au lieu. */
    if (verbe(seg, "osc.out", a)) {
        String m[2 + MappingEngine::MAX_ARGS_OSC_SUP];
        const int k = decouperArgs(a, m, 2 + MappingEngine::MAX_ARGS_OSC_SUP);
        if (k < 1) return true;
        int i0 = 0;
        uint8_t hote = 0;
        if (estNombre(m[0])) {
            const int oct = (int)arrondi(valeurArg(m[0]));
            if (oct >= 0 && oct <= 255) { hote = (uint8_t)oct; i0 = 1; }
        }
        if (i0 >= k) return true;
        String adr = m[i0];
        if (adr.length() >= 2 && (adr[0] == '"' || adr[0] == '\''))
            adr = adr.substring(1, adr.length() - 1);
        if (!adr.length()) return true;
        // REFUSER plutot que tronquer : une adresse coupee designe autre chose.
        if ((int)adr.length() >= MappingEngine::MAX_ADRESSE_OSC) {
            Serial.print("[nms] adresse OSC trop longue, message abandonne : ");
            Serial.println(adr);
            return true;
        }
        Sortie s;
        s.type = Sortie::Osc;
        s.reel = courant;
        s.hote = hote;
        strncpy(s.adresse, adr.c_str(), MappingEngine::MAX_ADRESSE_OSC - 1);
        s.adresse[MappingEngine::MAX_ADRESSE_OSC - 1] = '\0';
        for (int j = i0 + 1;
             j < k && s.nArgsSup < MappingEngine::MAX_ARGS_OSC_SUP; j++)
            s.argsSup[s.nArgsSup++] = valeurArg(m[j]);
        emettre(sorties, max, n, s);
        return true;
    }
    if (verbe(seg, "bend.out", a)) {
        int ch; if (!canalSeul(a, 1, ch)) return true;
        Sortie s; s.type = Sortie::Bend;
        s.canal = (uint8_t)ch;
        s.valeur14 = (int16_t)constrain((int)arrondi(courant), -8192, 8191);
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
                            Etat* etats, int nEtats, bool horsLigne) {
    if (!etats || nEtats <= 0) { etats = g_etats; nEtats = MAX_PIPELINES; }
    traite = false;
    int n = 0;
    if (!script || script[0] == '\0' || !sorties || max <= 0) return 0;

    /* L'HORLOGE AVANCE AVANT LES PIPELINES. Un del() ou un makenote()
     * rencontre pendant ce battement doit compter a partir de maintenant, pas
     * du battement precedent : sinon son echeance est deja passee et il tire
     * dans le meme souffle. C'est ici, dans le point de passage commun, plutot
     * que chez chaque appelant — le banc, la route d'essai et battre() posaient
     * l'horloge chacun a leur maniere, et deux d'entre eux la posaient trop
     * tard. */
    if (evt.type == Evenement::Tick)
        (horsLigne ? g_horsLigneMaintenant : g_maintenant) = evt.instant;

    const String s = nettoyer(script);
    const Famille fEvt = familleEvenement(evt);

    /* L'evenement est MUTABLE et PARTAGE par tous les pipelines : makenote()
     * le reecrit, et un pipeline suivant voit la reecriture — c'est ce que fait
     * le moteur de reference. On en garde l'empreinte d'origine : si un verbe
     * l'a reecrit, l'evenement fabrique ne doit surtout pas ressortir AUSSI par
     * le passage transparent, comme s'il etait entre par le MIDI. */
    Evenement e = evt;
    const Evenement::Type type0 = evt.type;
    const uint8_t a0 = evt.a, b0 = evt.b;

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
            const Source d = evaluerSource(src, e, stSrc);
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

                Contexte ctx;
                ctx.script = script; ctx.etats = etats; ctx.nEtats = nEtats;
                ctx.pipe = (uint8_t)pi;
                ctx.horsLigne = horsLigne;

                int debut = (finSrc == -1) ? (int)pipe.length() + 1 : finSrc + 1;
                uint8_t si = 0;
                while (debut <= (int)pipe.length()) {
                    const int fin = prochainSep(pipe, debut, ':');
                    String seg = pipe.substring(debut, (fin == -1) ? pipe.length() : fin);
                    seg.trim();
                    ctx.seg = si;
                    if (seg.length() &&
                        !evaluerSegment(seg, courant, e, sorties, max, n, st, srcCcNum, &ctx))
                        break;                      // segment bloquant
                    if (fin == -1) break;
                    debut = fin + 1;
                    si++;
                }
            }
            pi++;
        }

        if (finPipe == -1) break;
        debutPipe = finPipe + 1;
    }
    /* Evenement REECRIT (makenote) : on le declare pris en charge. Sans ca, la
     * note d'origine ressortirait par le passage transparent A COTE de la note
     * fabriquee — deux notes la ou le script n'en demandait qu'une. */
    if (e.type != type0 || e.a != a0 || e.b != b0) traite = true;
    return n;
}

/* REJOUER L'AVAL d'un pipeline, a partir du segment qui suit `seg`.
 *
 * C'est ce que fait le tick() du moteur web quand il vide une file de del ou de
 * makenote : il reprend les segments suivants avec la valeur mise de cote. On
 * repart du texte du script — pipeline `pipe`, segments apres `seg` — plutot
 * que de garder des offsets, qui ne survivraient pas a une reecriture du
 * script. */
static int rejouerDepuis(const char* script, uint8_t pipeIdx, uint8_t segIdx,
                         float valeur, const Evt& evt, Etat* etats, int nEtats,
                         Sortie* sorties, int max, bool horsLigne) {
    int n = 0;
    if (!script || !script[0] || !sorties || max <= 0) return 0;
    if (!etats || nEtats <= 0) { etats = g_etats; nEtats = MAX_PIPELINES; }

    Evt e = evt;                      // copie MUTABLE : cf. evaluerSegment
    const String s = nettoyer(script);
    int pi = 0, debutPipe = 0;
    while (debutPipe <= (int)s.length()) {
        const int finPipe = prochainSep(s, debutPipe, ';');
        const String pipe = s.substring(debutPipe, (finPipe == -1) ? s.length() : finPipe);
        if (pi == pipeIdx) {
            Etat& st = etats[(pi < nEtats) ? pi : nEtats - 1];
            const int finSrc = prochainSep(pipe, 0, ':');
            int debut = (finSrc == -1) ? (int)pipe.length() + 1 : finSrc + 1;
            uint8_t si = 0;
            float courant = valeur;
            Contexte ctx;
            ctx.script = script; ctx.etats = etats; ctx.nEtats = nEtats; ctx.pipe = pipeIdx;
            ctx.horsLigne = horsLigne;   // un del() qui en rejoue un autre y reste
            while (debut <= (int)pipe.length()) {
                const int fin = prochainSep(pipe, debut, ':');
                String seg = pipe.substring(debut, (fin == -1) ? pipe.length() : fin);
                seg.trim();
                if (si > segIdx) {                 // on ne rejoue que l'AVAL
                    ctx.seg = si;
                    if (seg.length() &&
                        !evaluerSegment(seg, courant, e, sorties, max, n, st, false, &ctx))
                        break;
                }
                if (fin == -1) break;
                debut = fin + 1;
                si++;
            }
            return n;
        }
        if (finPipe == -1) break;
        debutPipe = finPipe + 1;
        pi++;
    }
    return n;
}

/* Vide la file des DIFFERES en REMPLISSANT une liste. Disponible des deux
 * cotes : c'est cette variante que le banc de conformite emploie, et c'est donc
 * elle qui prouve del(), makenote(), lag() et ramp(). */
int MappingEngine::battreDifferes(uint32_t maintenant, Sortie* sorties, int max,
                                  bool horsLigne) {
    (horsLigne ? g_horsLigneMaintenant : g_maintenant) = maintenant;
    int total = 0;
    for (int i = 0; i < MAX_REPRISES; i++) {
        Reprise& r = g_reprises[i];
        if (!r.actif) continue;
        if (r.horsLigne != horsLigne) continue;     // chacun sa ligne de temps

        /* Une tache CONTINUE tire a chaque battement : on calcule ou en est
         * l'interpolation, on rejoue l'aval avec cette valeur, et on ne la
         * retire qu'une fois arrivee. */
        if (r.genre != RepDifferee) {
            const int32_t ecoule = (int32_t)(maintenant - r.debut);
            const float t = (r.duree == 0 || ecoule >= (int32_t)r.duree) ? 1.f
                          : (ecoule <= 0) ? 0.f : (float)ecoule / (float)r.duree;
            const float v = r.de + (r.vers - r.de) * t;
            // lag() garde sa position ; ramp() ne laisse rien derriere elle.
            if (r.genre == RepLag) etatDe(r.etats, r.nEtats, r.pipe).lagCur = v;
            if (total < max)
                total += rejouerDepuis(r.script, r.pipe, r.seg, v, r.evt,
                                       r.etats, r.nEtats, sorties + total,
                                       max - total, horsLigne);
            if (t >= 1.f) r.actif = false;
            continue;
        }

        if ((int32_t)(maintenant - r.echeance) < 0) continue;
        r.actif = false;                            // libere AVANT de rejouer :
                                                    // le rejeu peut re-differer
        if (total < max)
            total += rejouerDepuis(r.script, r.pipe, r.seg, r.valeur, r.evt,
                                   r.etats, r.nEtats, sorties + total, max - total,
                                   horsLigne);
    }
    return total;
}

void MappingEngine::viderDifferes(const char* script) {
    for (int i = 0; i < MAX_REPRISES; i++)
        if (!script || g_reprises[i].script == script) g_reprises[i].actif = false;
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
                                    bool estNoteOff, SortieNote& sortie,
                                    Etat* etats, int nEtats) {
    sortie.emise = sortie.traite = false;
    Evenement e;
    e.type  = estNoteOff ? Evenement::NoteOff : Evenement::NoteOn;
    e.canal = canalIn; e.a = noteIn; e.b = veloIn;

    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
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
                                  SortieCc& sortie, Etat* etats, int nEtats) {
    sortie.emise = sortie.traite = false;
    Evenement e;
    e.type = Evenement::Cc;
    e.canal = canalIn; e.a = ccIn; e.b = valeurIn;

    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
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
            case MappingEngine::Sortie::Osc: {
                float args[1 + MappingEngine::MAX_ARGS_OSC_SUP];
                int na = 0;
                args[na++] = s.reel;                       // la valeur courante d'abord
                for (int k = 0; k < s.nArgsSup; k++) args[na++] = s.argsSup[k];
                if (g_emetteurOsc) g_emetteurOsc(s.adresse, args, na, s.hote);
                break;
            }
        }
    }
}

/* La variante a EMETTEUR emploie emettreVers, qui n'existe que dans la carte —
 * elle vit donc sous la garde, avec executerCapteur. */
void MappingEngine::battreDifferes(uint32_t maintenant, MidiSender* sender) {
    Sortie liste[MAX_SORTIES];
    const int n = battreDifferes(maintenant, liste, MAX_SORTIES);
    if (n > 0) emettreVers(sender, liste, n);
}

/* Un COMPOSANT donne ses valeurs a son script. Elles ne passent PLUS par le
 * registre : celui-ci est un bus PARTAGE, et il ne doit contenir que ce qu'on y
 * a mis expressement — « in() : s("bouton1") ». Y publier d'office l'entree de
 * chaque composant remplissait un espace commun de valeurs que personne n'avait
 * demande a partager, sous des noms reserves (« in », « raw ») qu'un s("in")
 * aurait pietines. C'est in(n) et raw.in(n) qui les lisent, maintenant. */
void MappingEngine::executerCapteur(const char* script, const float* valeurs,
                                    int nValeurs, MidiSender* sender,
                                    Etat* etats, int nEtats, const float* bruts,
                                    const char* origine) {
    if (!script || script[0] == '\0' || !valeurs || nValeurs <= 0) return;

    Evenement e;
    e.type = Evenement::Capteur;       // sans famille : aucun passage transparent
    if (origine) snprintf(e.origine, sizeof e.origine, "%s", origine);
    e.nInlets = (uint8_t)((nValeurs > MAX_INLETS) ? MAX_INLETS : nValeurs);
    for (int i = 0; i < e.nInlets; i++) {
        e.inlets[i] = valeurs[i];
        /* Sans lecture native distincte, les deux coincident : c'est le cas d'un
         * contact, qui n'a rien de plus fin que 0/1. */
        e.raws[i] = (bruts && !isnan(bruts[i])) ? bruts[i] : valeurs[i];
    }
    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
    emettreVers(sender, liste, n);
}

/* Commodite pour les composants a UNE valeur — la plupart. */
void MappingEngine::executerCapteur(const char* script, float valeur,
                                    MidiSender* sender, Etat* etats, int nEtats,
                                    float brut, const char* origine) {
    const float v = valeur, b = brut;
    executerCapteur(script, &v, 1, sender, etats, nEtats,
                    isnan(brut) ? nullptr : &b, origine);
}

/* Le battement d'horloge : fait tourner les pipelines qui n'attendent aucun
 * MIDI — metro() sur Tick, loadbang() sur Init. Meme emission que pour un
 * capteur : un script generaliste (DMX, OSC, MIDI) n'a pas a savoir d'ou vient
 * le declencheur. */
void MappingEngine::battre(const char* script, Evenement::Type type, uint32_t instant,
                           MidiSender* sender, Etat* etats, int nEtats,
                           const char* origine) {
    if (!script || script[0] == '\0') return;
    Evenement e;
    e.type = type;
    if (origine) snprintf(e.origine, sizeof e.origine, "%s", origine);
    e.instant = instant;                  // l'horloge est posee par executer()
    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
    if (n > 0) emettreVers(sender, liste, n);
}

void MappingEngine::battreOsc(const char* script, const char* adresse, float valeur,
                              MidiSender* sender, Etat* etats, int nEtats,
                              const char* origine) {
    if (!script || script[0] == '\0' || !adresse) return;
    Evenement e;
    e.type = Evenement::Osc;
    if (origine) snprintf(e.origine, sizeof e.origine, "%s", origine);
    e.reel = valeur;
    snprintf(e.adresse, sizeof e.adresse, "%s", adresse);
    Sortie liste[MAX_SORTIES];
    bool traite = false;
    const int n = executer(script, e, liste, MAX_SORTIES, traite, etats, nEtats);
    if (n > 0) emettreVers(sender, liste, n);
}
#endif  // NMS_BANC_HOTE
