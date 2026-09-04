#include "CcMap.h"

#include <Preferences.h>

#include "../audio/AudioEngine.h"
#include "../mapping/MappingEngine.h"   // FluxRegistry

namespace {

constexpr const char* NVS_ESPACE = "nidmi-midi";   // partage avec le nom de script
constexpr const char* NVS_CLE    = "ccmap";

CcMap::Entree s_table[CcMap::MAX];
int           s_nb = 0;

// Armement du CC learn : une cible en attente du prochain CC entrant.
char  s_cibleArmee[16] = {0};
float s_minArme = 0.0f, s_maxArme = 1.0f;

void ecrireNvs(const String& t) {
    Preferences p;
    if (!p.begin(NVS_ESPACE, false)) return;
    if (t.length()) p.putString(NVS_CLE, t);
    else            p.remove(NVS_CLE);
    p.end();
}

// Etale 0..127 dans [min, max]. 127 (et non 128) pour que la butee haute du
// potentiometre atteigne EXACTEMENT max — sinon un CC ne permet jamais
// d'atteindre la valeur maximale, ce qui se remarque tout de suite sur
// « engine » ou sur un nombre de demi-tons.
float etaler(uint8_t valeur, float mn, float mx) {
    return mn + (float(valeur) / 127.0f) * (mx - mn);
}

}  // namespace

namespace CcMap {

bool estParamAudio(const char* nom) {
    if (!nom) return false;
    return !strcmp(nom, "engine")    || !strcmp(nom, "harmonics") ||
           !strcmp(nom, "timbre")    || !strcmp(nom, "morph")     ||
           !strcmp(nom, "decay")     || !strcmp(nom, "lpg_colour");
}

// Applique UNE cible. Separe d'appliquer() pour que l'apprentissage puisse
// s'en servir : la cible bouge des le CC qui l'a apprise, sans qu'il faille
// toucher le potentiometre une seconde fois.
static void poser(const Entree& e, uint8_t valeur) {
    const float v = etaler(valeur, e.min, e.max);
    if (!estParamAudio(e.cible)) {
        // Parametre de script : meme chemin que /api/midi/script?params=.
        FluxRegistry::update(e.cible, v);
        return;
    }
    if (!strcmp(e.cible, "engine")) {
        // Un CC qui balaie les moteurs demande une allocation a chaque cran :
        // setEngine s'en garde lui-meme (il refuse sous le seuil de tas) et ne
        // persiste pas — un geste de jeu ne redefinit pas le defaut du boitier.
        AudioEngine::setEngine((int)lroundf(v), /*persister=*/false);
        return;
    }
    AudioEngine::Params p = AudioEngine::params();
    if      (!strcmp(e.cible, "harmonics"))  p.harmonics = v;
    else if (!strcmp(e.cible, "timbre"))     p.timbre    = v;
    else if (!strcmp(e.cible, "morph"))      p.morph     = v;
    else if (!strcmp(e.cible, "decay"))      p.decay     = v;
    else if (!strcmp(e.cible, "lpg_colour")) p.lpgColour = v;
    AudioEngine::setParams(p);
}

int appliquer(uint8_t canal, uint8_t cc, uint8_t valeur) {
    int touches = 0;
    for (int i = 0; i < s_nb; i++) {
        const Entree& e = s_table[i];
        if (e.cc != cc) continue;
        if (e.canal != 0 && e.canal != canal) continue;   // 0 = tous canaux
        poser(e, valeur);
        touches++;
    }
    return touches;
}

// ── Apprentissage ────────────────────────────────────────────────────────────

void armer(const char* cible, float mn, float mx) {
    if (!cible || !*cible) { desarmer(); return; }
    strlcpy(s_cibleArmee, cible, sizeof(s_cibleArmee));
    s_minArme = mn; s_maxArme = mx;
    Serial.printf("[CcMap] apprentissage arme sur '%s' [%.3f..%.3f]\n", s_cibleArmee, mn, mx);
}

void desarmer()          { s_cibleArmee[0] = '\0'; }
bool arme()              { return s_cibleArmee[0] != '\0'; }
const char* cibleArmee() { return s_cibleArmee; }

bool apprendre(uint8_t canal, uint8_t cc) {
    if (!arme()) return false;

    // Reapprendre la MEME cible remplace son affectation au lieu d'en empiler
    // une seconde : sans ca, corriger un CC laissait l'ancien actif et deux
    // potentiometres se disputaient le parametre.
    int place = -1;
    for (int i = 0; i < s_nb; i++)
        if (!strcmp(s_table[i].cible, s_cibleArmee)) { place = i; break; }
    if (place < 0) {
        if (s_nb >= MAX) {
            Serial.println("[CcMap] table pleine — apprentissage abandonne");
            desarmer();
            return false;
        }
        place = s_nb++;
    }
    Entree& e = s_table[place];
    e.canal = canal; e.cc = cc;
    strlcpy(e.cible, s_cibleArmee, sizeof(e.cible));
    e.min = s_minArme; e.max = s_maxArme;
    desarmer();
    ecrireNvs(texte());     // une affectation apprise survit au redemarrage
    Serial.printf("[CcMap] appris : canal %u CC %u -> %s\n",
                  (unsigned)canal, (unsigned)cc, e.cible);
    return true;
}

// ── Serialisation ────────────────────────────────────────────────────────────

String texte() {
    String out;
    for (int i = 0; i < s_nb; i++) {
        const Entree& e = s_table[i];
        if (out.length()) out += ';';
        out += String(e.canal); out += ':';
        out += String(e.cc);    out += ':';
        out += e.cible;         out += ':';
        out += String(e.min, 3); out += ':';
        out += String(e.max, 3);
    }
    return out;
}

bool setTexte(const String& t, bool persister) {
    s_nb = 0;
    int debut = 0;
    while (debut < (int)t.length() && s_nb < MAX) {
        int fin = t.indexOf(';', debut);
        if (fin < 0) fin = t.length();
        String ligne = t.substring(debut, fin);
        debut = fin + 1;
        ligne.trim();
        if (!ligne.length()) continue;

        // canal:cc:cible:min:max — cinq champs, aucun optionnel. Une ligne
        // malformee est IGNOREE plutot que de faire echouer toute la table :
        // une affectation abimee ne doit pas emporter les onze autres.
        int c[4];
        int cherche = 0, depuis = 0;
        for (int i = 0; i < 4; i++) {
            cherche = ligne.indexOf(':', depuis);
            if (cherche < 0) { cherche = -1; break; }
            c[i] = cherche; depuis = cherche + 1;
        }
        if (cherche < 0) continue;

        Entree e{};
        e.canal = (uint8_t)ligne.substring(0, c[0]).toInt();
        e.cc    = (uint8_t)ligne.substring(c[0] + 1, c[1]).toInt();
        String cible = ligne.substring(c[1] + 1, c[2]); cible.trim();
        if (!cible.length() || e.cc > 127 || e.canal > 16) continue;
        strlcpy(e.cible, cible.c_str(), sizeof(e.cible));
        e.min = ligne.substring(c[2] + 1, c[3]).toFloat();
        e.max = ligne.substring(c[3] + 1).toFloat();
        if (e.min == e.max) continue;                 // etalement impossible
        s_table[s_nb++] = e;
    }
    if (persister) ecrireNvs(texte());
    Serial.printf("[CcMap] table : %d affectation(s)%s\n",
                  s_nb, persister ? " memorisee(s)" : "");
    return true;
}

void vider(bool persister) {
    s_nb = 0;
    desarmer();
    if (persister) ecrireNvs("");
}

void monter() {
    Preferences p;
    if (!p.begin(NVS_ESPACE, true)) return;
    const String t = p.getString(NVS_CLE, "");
    p.end();
    if (!t.length()) return;
    setTexte(t, /*persister=*/false);
}

}  // namespace CcMap
