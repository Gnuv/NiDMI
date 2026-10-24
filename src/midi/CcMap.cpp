#include "CcMap.h"
#include "../config/EcrituresDifferees.h"

#include <Preferences.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../audio/AudioEngine.h"
#include "../audio/Effects.h"           // an effect's parameter as a target (§228)
#include "../mapping/MappingEngine.h"   // FluxRegistry
#include "../mapping/Repertoire.h"      // cc.txt de la composition ouverte

namespace {

/* L'ancienne place de la table, commune a toute la carte : la cle ne sert plus
 * qu'a etre effacee, une fois, au demarrage (§187). */
constexpr const char* NVS_ESPACE_RETIRE = "nidmi-midi";
constexpr const char* NVS_CLE_RETIREE   = "ccmap";
constexpr const char* ENTETE = "# canal:cc:cible:min:max:groupe (canal 0 = tous, groupe 0 = tous) — les CC appris de cette composition\n";

CcMap::Entree s_table[CcMap::MAX];
int           s_nb = 0;

// Armement du CC learn : une cible en attente du prochain CC entrant.
char  s_cibleArmee[16] = {0};
float s_minArme = 0.0f, s_maxArme = 1.0f;
uint8_t s_groupeArme = 0;   // the group whose CC is learnt (§222); 0: any

/* LE VERROU DE LA TABLE (MESURES §178). Trois taches y touchent : MidiTask (CC
 * USB et broches, coeur 0) et la boucle (CC RTP, coeur 1) l'appliquent et
 * apprennent ; le serveur web la lit, la remplace, arme l'apprentissage.
 * setTexte() remettait le compte a zero puis remplissait sous les yeux d'un CC
 * entrant — une cible a moitie copiee partait dans le FluxRegistry, et une
 * lecture de l'app (texte(), qu'elle modifie et renvoie) pouvait MEMORISER une
 * affectation abimee. Tenu le temps d'une copie, jamais d'un calcul : le CC
 * n'attend pas le serveur web. Cree a l'initialisation statique. */
StaticSemaphore_t s_tamponVerrou;
SemaphoreHandle_t s_verrou = xSemaphoreCreateRecursiveMutexStatic(&s_tamponVerrou);
struct Verrou {
    Verrou()  { if (s_verrou) xSemaphoreTakeRecursive(s_verrou, portMAX_DELAY); }
    ~Verrou() { if (s_verrou) xSemaphoreGiveRecursive(s_verrou); }
};

/* Dans la composition ouverte, au premier silence (§157) : apprendre un CC en
 * jouant ne doit pas couter le son. Une table vide n'a pas de fichier. */
void memoriser(const String& t) {
    if (!t.length()) {
        const String chemin = Repertoire::chemin(Repertoire::CC);
        if (chemin.length()) Differe::supprimerFichier(chemin.c_str());
        return;
    }
    String raison;
    if (!Repertoire::assurerOuverte("", raison)) {
        Serial.printf("[CcMap] table NON memorisee : %s\n", raison.c_str());
        return;
    }
    String corps = t;
    corps.replace(';', '\n');
    const String f = String(ENTETE) + corps + "\n";
    Differe::poserFichierCopie(Repertoire::chemin(Repertoire::CC).c_str(), f.c_str(), f.length());
}

// Etale 0..127 dans [min, max]. 127 (et non 128) pour que la butee haute du
// potentiometre atteigne EXACTEMENT max — sinon un CC ne permet jamais
// d'atteindre la valeur maximale, ce qui se remarque tout de suite sur
// « engine » ou sur un nombre de demi-tons.
float etaler(uint8_t valeur, float mn, float mx) {
    return mn + (float(valeur) / 127.0f) * (mx - mn);
}

/* By octaves (§228), as the app's `log` slider (js/ui/widgets.js): min · (max/min)^(v/127).
 * Linear when a bound is not positive. */
float spreadOctaves(uint8_t valeur, float mn, float mx) {
    if (mn <= 0.0f || mx <= 0.0f) return etaler(valeur, mn, mx);
    return mn * powf(mx / mn, float(valeur) / 127.0f);
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
static void poser(const Entree& e, uint8_t valeur, AudioEngine::Pour pour = AudioEngine::Pour::TOUS) {
    /* An effect's parameter (§228): the effects of the cue the CC is for, as the synth voice
     * below. The group was settled by appliquer(): the FX block's. */
    if (Effects::isParam(e.cible)) {
        const float v = Effects::inOctaves(e.cible) ? spreadOctaves(valeur, e.min, e.max)
                                                    : etaler(valeur, e.min, e.max);
        Effects::setParam(e.cible, v, AudioEngine::masqueSlots(pour));
        return;
    }
    const float v = etaler(valeur, e.min, e.max);
    if (!estParamAudio(e.cible)) {
        // Parametre de script : meme chemin que /api/midi/script?params=.
        FluxRegistry::update(e.cible, v);
        return;
    }
    if (!strcmp(e.cible, "engine")) {
        // Un CC qui balaie les moteurs demande une allocation a chaque cran :
        // setEngine s'en garde lui-meme (il refuse sous le seuil de tas).
        AudioEngine::setEngine((int)lroundf(v));
        return;
    }
    /* The synth voice of the cue the CC is for (§212). */
    AudioEngine::fixerParamSynth(e.cible, v, pour);
}

int appliquer(uint8_t canal, uint8_t cc, uint8_t valeur, AudioEngine::Pour pour, uint8_t groupe) {
    // Copier sous le verrou, poser hors du verrou : poser() peut changer de moteur.
    Entree vises[MAX];
    int touches = 0;
    {
        Verrou verrou;
        for (int i = 0; i < s_nb; i++) {
            const Entree& e = s_table[i];
            if (e.cc != cc) continue;
            if (e.canal != 0 && e.canal != canal) continue;   // 0 = tous canaux
            /* Another group's: it takes the CC as ITS scripts leave it, in its own walk (§222). */
            if (groupe != AudioEngine::GROUP_ALL && e.groupe != 0 && e.groupe != groupe) continue;
            vises[touches++] = e;
        }
    }
    for (int i = 0; i < touches; i++) poser(vises[i], valeur, pour);
    return touches;
}

uint32_t groupes() {
    Verrou verrou;
    uint32_t m = 0;
    for (int i = 0; i < s_nb; i++) if (s_table[i].groupe) m |= 1u << s_table[i].groupe;
    if (s_cibleArmee[0] && s_groupeArme) m |= 1u << s_groupeArme;   // its walk learns it
    return m;
}

// ── Apprentissage ────────────────────────────────────────────────────────────

void armer(const char* cible, float mn, float mx, uint8_t groupe) {
    if (!cible || !*cible) { desarmer(); return; }
    {
        Verrou verrou;
        strlcpy(s_cibleArmee, cible, sizeof(s_cibleArmee));
        s_minArme = mn; s_maxArme = mx;
        s_groupeArme = (groupe > AudioEngine::GROUP_MAX) ? 0 : groupe;
    }
    Serial.printf("[CcMap] apprentissage arme sur '%s' [%.3f..%.3f], groupe %u\n", cible, mn, mx, (unsigned)groupe);
}

void desarmer()          { Verrou verrou; s_cibleArmee[0] = '\0'; }
bool arme()              { return s_cibleArmee[0] != '\0'; }
const char* cibleArmee() { return s_cibleArmee; }

bool apprendre(uint8_t canal, uint8_t cc, uint8_t groupe) {
    if (!arme()) return false;
    Entree e{};
    {
        Verrou verrou;
        if (!arme()) return false;       // desarme entre-temps (l'app, ou un autre CC)
        /* The CC as the armed block's group leaves it, in that group's walk (§222). */
        if (s_groupeArme && groupe != AudioEngine::GROUP_ALL && groupe != s_groupeArme) return false;

        // Reapprendre la MEME cible remplace son affectation au lieu d'en empiler
        // une seconde : sans ca, corriger un CC laissait l'ancien actif et deux
        // potentiometres se disputaient le parametre. La meme cible du meme groupe
        // (§222) : deux groupes peuvent viser deux blocs du meme nom.
        int place = -1;
        for (int i = 0; i < s_nb; i++)
            if (!strcmp(s_table[i].cible, s_cibleArmee) && s_table[i].groupe == s_groupeArme) { place = i; break; }
        if (place < 0) {
            if (s_nb >= MAX) {
                s_cibleArmee[0] = '\0';
                Serial.println("[CcMap] table pleine — apprentissage abandonne");
                return false;
            }
            place = s_nb;
        }
        e.canal = canal; e.cc = cc;
        strlcpy(e.cible, s_cibleArmee, sizeof(e.cible));
        e.min = s_minArme; e.max = s_maxArme;
        e.groupe = s_groupeArme;
        s_table[place] = e;
        if (place == s_nb) s_nb++;       // l'entree complete, PUIS le compte
        s_cibleArmee[0] = '\0';
    }
    memoriser(texte());     // une affectation apprise survit au redemarrage
    Serial.printf("[CcMap] appris : canal %u CC %u -> %s\n",
                  (unsigned)canal, (unsigned)cc, e.cible);
    return true;
}

// ── Serialisation ────────────────────────────────────────────────────────────

String texte() {
    Entree copie[MAX];
    int n;
    {
        Verrou verrou;
        n = s_nb;
        memcpy(copie, s_table, sizeof(Entree) * n);
    }
    String out;
    for (int i = 0; i < n; i++) {
        const Entree& e = copie[i];
        if (out.length()) out += ';';
        out += String(e.canal); out += ':';
        out += String(e.cc);    out += ':';
        out += e.cible;         out += ':';
        out += String(e.min, 3); out += ':';
        out += String(e.max, 3); out += ':';
        out += String(e.groupe);
    }
    return out;
}

/* Lue a cote, posee d'un coup : un CC entrant voit l'ancienne table ou la
 * nouvelle. Rend le nombre d'affectations. */
static int poserTable(const String& t) {
    Entree table[MAX];
    int nb = 0;
    int debut = 0;
    while (debut < (int)t.length() && nb < MAX) {
        int fin = debut;                                // ';' (l'API) ou une ligne (cc.txt)
        while (fin < (int)t.length() && t[fin] != ';' && t[fin] != '\n') fin++;
        String ligne = t.substring(debut, fin);
        debut = fin + 1;
        ligne.trim();
        if (!ligne.length() || ligne[0] == '#') continue;

        // canal:cc:cible:min:max[:groupe] — cinq champs au moins ; le groupe
        // absent vaut 0, tous (§222). Une ligne malformee est IGNOREE plutot que
        // de faire echouer toute la table : une affectation abimee ne doit pas
        // emporter les onze autres.
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
        const int c4 = ligne.indexOf(':', c[3] + 1);
        e.max = ligne.substring(c[3] + 1, c4 < 0 ? ligne.length() : c4).toFloat();
        const long g = (c4 < 0) ? 0 : ligne.substring(c4 + 1).toInt();
        e.groupe = (uint8_t)((g < 0 || g > AudioEngine::GROUP_MAX) ? 0 : g);
        if (e.min == e.max) continue;                 // etalement impossible
        table[nb++] = e;
    }
    {
        Verrou verrou;
        memcpy(s_table, table, sizeof(Entree) * nb);
        s_nb = nb;
    }
    return nb;
}

bool setTexte(const String& t) {
    const int nb = poserTable(t);
    memoriser(texte());
    Serial.printf("[CcMap] table : %d affectation(s), dans la composition ouverte\n", nb);
    return true;
}

void vider() {
    {
        Verrou verrou;
        s_nb = 0;
        s_cibleArmee[0] = '\0';
    }
    memoriser("");
}

/* La table de la composition ouverte : ce qui attend le silence d'abord, la
 * flash sinon. Pas de fichier : pas d'affectation. */
static void lireLaComposition() {
    const String chemin = Repertoire::chemin(Repertoire::CC);
    std::shared_ptr<char> brut; size_t n = 0;
    String t;
    if (chemin.length() && Differe::lire(chemin.c_str(), brut, n)) t.concat(brut.get(), (unsigned)n);
    const int nb = poserTable(t);
    if (nb) Serial.printf("[CcMap] %d affectation(s) de la composition ouverte\n", nb);
}

void monter() {
    {
        Preferences p;
        bool ancienne = false;
        if (p.begin(NVS_ESPACE_RETIRE, true)) { ancienne = p.isKey(NVS_CLE_RETIREE); p.end(); }
        if (ancienne) Differe::nvsRetirer(NVS_ESPACE_RETIRE, NVS_CLE_RETIREE);   // au silence
    }
    lireLaComposition();
}

void recharger() {
    desarmer();
    lireLaComposition();
}

}  // namespace CcMap
