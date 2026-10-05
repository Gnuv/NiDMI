#include "CueStore.h"
#include "../config/Stockage.h"
#include "ScriptStore.h"
#include "../midi/MidiRouter.h"
#include "../Globals.h"
#include "../audio/AudioEngine.h"
#include "../server/ServerCore.h"     // nidmi_ws_pousser : la carte ANNONCE son etat
#include "../config/EcrituresDifferees.h"
#include "Repertoire.h"
#include <LittleFS.h>
#include <memory>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../diag/Chronos.h"     // le retard des cues minutees, et ce que coute une cue (§172)
#include <Preferences.h>          // les options du transport (§181)

namespace Cues {
namespace {

/* ── LE VERROU DU SEQUENCEUR (MESURES §172) ────────────────────────────────
 * Deux taches le pilotent : la boucle, qui fait avancer les cues minutees, et
 * le serveur web, par qui arrivent PLAY, STOP, GO, goto. Rien ne les separait,
 * et appliquer une cue prend 30 a 50 ms (scripts lus en flash, echantillons
 * armes) : une commande pouvait s'intercaler au milieu d'une autre. Toute
 * fonction publique le prend ; recursif, car elles s'appellent entre elles
 * (demarrer -> aller). Cree a l'initialisation statique, avant toute tache. */
StaticSemaphore_t _tamponVerrouSeq;
SemaphoreHandle_t _verrouSeq = xSemaphoreCreateRecursiveMutexStatic(&_tamponVerrouSeq);
struct VerrouSeq {
  VerrouSeq()  { if (_verrouSeq) xSemaphoreTakeRecursive(_verrouSeq, portMAX_DELAY); }
  ~VerrouSeq() { if (_verrouSeq) xSemaphoreGiveRecursive(_verrouSeq); }
};

constexpr const char* PARTITION = Stockage::PARTITION;
constexpr const char* BASE      = Stockage::BASE;

bool  _monte    = false;
bool  _lecture  = false;
int   _index    = 0;
/* Les options du transport (§181) et qui veut connaitre son etat. */
bool  _boucle       = false;
bool  _auDemarrage  = false;
void (*_rappelEtat)(const Etat&) = nullptr;
constexpr const char* NVS_OPTIONS = "nidmi-cues";
uint32_t _debutMs = 0;      // instant d'activation de la cue courante
/* ── LA PAUSE APPARTIENT A LA CARTE, ELLE AUSSI ─────────────────────────────
 * L'app avait la sienne, purement locale : appuyer sur STOP en lecture la
 * mettait en pause pendant que la carte CONTINUAIT. L'ecran disait « arrete »,
 * le son continuait — un mensonge d'etat, exactement ce que le sequenceur
 * unique doit supprimer.
 * Une pause n'est pas un arret : elle GELE le decompte la ou il en est. On
 * retient donc l'ecoule, et la reprise recule l'origine d'autant au lieu de
 * repartir de zero. */
bool     _enPause  = false;
uint32_t _ecouleMs = 0;     // ce qui s'etait ecoule au moment de la pause
float _dureeCourante = 0.0f;

// Decoupe "a|b|c" sans allouer de tableau : on avance de separateur en
// separateur. Retourne le champ n (vide si absent).
String _champ(const String& ligne, int n) {
  int debut = 0;
  for (int i = 0; i < n; i++) {
    const int p = ligne.indexOf('|', debut);
    if (p < 0) return String("");
    debut = p + 1;
  }
  const int fin = ligne.indexOf('|', debut);
  String v = (fin < 0) ? ligne.substring(debut) : ligne.substring(debut, fin);
  v.trim();
  return v;
}

bool _ligneUtile(const String& l) {
  String t = l; t.trim();
  return t.length() && !t.startsWith("#");
}

/* Le premier son d'une liste « a.wav,b.wav » : celui que le clavier jouera —
 * a l'arrivee sur la cue (_appliquer) comme au demarrage (moteurEmploye). */
String _premierSon(const String& noms) {
  const int v = noms.indexOf(',');
  String p = (v < 0) ? noms : noms.substring(0, v);
  p.trim();
  return p;
}

/* La valeur de `cle` dans « cle=valeur;cle=valeur » ; "" si elle n'y est pas. */
String _valeurDe(const String& params, const char* cle) {
  int debut = 0;
  while (debut < (int)params.length()) {
    int fin = params.indexOf(';', debut);
    if (fin < 0) fin = params.length();
    const String kv = params.substring(debut, fin);
    debut = fin + 1;
    const int eq = kv.indexOf('=');
    if (eq <= 0) continue;
    String k = kv.substring(0, eq); k.trim();
    if (k != cle) continue;
    String v = kv.substring(eq + 1); v.trim();
    return v;
  }
  return String("");
}

/* ── AUTOMATION : LES COURBES DE LA CUE COURANTE ───────────────────────────
 * Reechantillonnees par l'app (voir CueStore.h), donc ici : un tableau de
 * nombres par parametre, et une interpolation lineaire sur la duree de la cue.
 * STATIQUE et borne : 6 parametres x 32 points = 768 octets en .bss. Le tas de
 * cette carte est la ressource rare ; une automation ne doit pas y toucher. */
namespace {
  constexpr uint8_t kMaxCourbes = 6;     // les cinq continus de Plaits + volume
  constexpr uint8_t kMaxPoints  = 32;
  struct Courbe {
    char    param[12] = {0};
    uint8_t n = 0;
    float   v[kMaxPoints] = {0};
  };
  Courbe  _courbes[kMaxCourbes];
  uint8_t _nCourbes = 0;
  uint32_t _dernierAppliqueMs = 0;

  void _oublierCourbes() { _nCourbes = 0; }

  /* "harmonics:0.1,0.2;volume:1.0,0.9" — analyse a l'ACTIVATION de la cue,
   * jamais dans la boucle : un changement de cue est rare, la boucle ne l'est
   * pas. */
  void _analyserCourbes(const String& spec) {
    _oublierCourbes();
    int debut = 0;
    while (debut < (int)spec.length() && _nCourbes < kMaxCourbes) {
      int fin = spec.indexOf(';', debut);
      if (fin < 0) fin = spec.length();
      String bloc = spec.substring(debut, fin);
      debut = fin + 1;
      const int deuxPoints = bloc.indexOf(':');
      if (deuxPoints <= 0) continue;
      String nom = bloc.substring(0, deuxPoints); nom.trim();
      if (!nom.length() || nom.length() >= (int)sizeof(Courbe::param)) continue;
      Courbe& co = _courbes[_nCourbes];
      co = Courbe{};
      strlcpy(co.param, nom.c_str(), sizeof co.param);
      String liste = bloc.substring(deuxPoints + 1);
      int d2 = 0;
      while (d2 < (int)liste.length() && co.n < kMaxPoints) {
        int f2 = liste.indexOf(',', d2);
        if (f2 < 0) f2 = liste.length();
        co.v[co.n++] = liste.substring(d2, f2).toFloat();
        d2 = f2 + 1;
      }
      if (co.n) _nCourbes++;
    }
  }

  /* Valeur de la courbe a l'avancement t (0..1), par interpolation lineaire.
   * Un seul point = une constante ; hors bornes = les extremites. */
  float _valeurA(const Courbe& co, float t) {
    if (co.n == 0) return 0.f;
    if (co.n == 1) return co.v[0];
    if (t <= 0.f)  return co.v[0];
    if (t >= 1.f)  return co.v[co.n - 1];
    const float x = t * (co.n - 1);
    const uint8_t i = (uint8_t)x;
    const float f = x - (float)i;
    return co.v[i] + (co.v[i + 1] - co.v[i]) * f;
  }

  void _appliquerAutomation(float t) {
    if (!_nCourbes) return;
    AudioEngine::Params p = AudioEngine::params();
    bool toucheParams = false;
    for (uint8_t k = 0; k < _nCourbes; k++) {
      const float v = _valeurA(_courbes[k], t);
      const char* n = _courbes[k].param;
      if      (!strcmp(n, "harmonics"))  { p.harmonics = v; toucheParams = true; }
      else if (!strcmp(n, "timbre"))     { p.timbre    = v; toucheParams = true; }
      else if (!strcmp(n, "morph"))      { p.morph     = v; toucheParams = true; }
      else if (!strcmp(n, "decay"))      { p.decay     = v; toucheParams = true; }
      else if (!strcmp(n, "lpg_colour")) { p.lpgColour = v; toucheParams = true; }
      else if (!strcmp(n, "drone"))      { p.drone     = v; toucheParams = true; }
      else if (!strcmp(n, "volume"))     { AudioEngine::setVolume(v); }
    }
    if (toucheParams) AudioEngine::setParams(p);
  }
}

// Applique a la carte ce que la cue decrit. C'est ICI que « changer de cue »
// prend un sens materiel — et nulle part dans le navigateur.
void _appliquer(const Cue& c) {
  /* Les courbes d'abord : analysees ICI, a l'activation, jamais dans la boucle.
   * Une cue sans automation en vide la table — sinon la courbe de la cue
   * precedente continuerait de tirer les parametres, exactement le comportement
   * fantome corrige ailleurs (§89.1). */
  _analyserCourbes(c.env);
  /* 1. LES SCRIPTS .nms d'abord : ils transforment le MIDI, donc ils doivent
   *    etre en place avant que la moindre note n'arrive.
   *
   *    UNE CUE EN PORTE PLUSIEURS, un par maillon de la chaine, separes par des
   *    virgules — la position DIT le maillon, et une position vide le libere.
   *    Elle n'en portait qu'UN, charge dans l'emplacement 0 par le defaut de
   *    `chargerScriptNomme` : une composition a plusieurs pistes map jouait donc
   *    dans le navigateur et pas sur la carte, et la cue ecrasait au passage le
   *    premier maillon de la chaine — celui qui est cense tourner tout le temps.
   *
   *    Les maillons qu'aucun script n'occupe sont VIDES, pas laisses tels
   *    quels : sinon le script de la cue precedente continuerait de transformer
   *    les evenements, exactement le comportement fantome corrige ailleurs. */
  {
    const uint8_t n = g_midiRouter.nEmplacements();
    /* LES MAILLONS DE LA ZONE MAIN SONT HORS DE PORTEE D'UNE CUE. Ils tournent
     * tout le temps, quelle que soit la cue — donc ni chargement ni vidage ici.
     * Les positions de la liste restent ABSOLUES : ce qu'une cue ecrit pour un
     * maillon permanent est simplement ignore, et le format ne depend pas de la
     * frontiere. */
    const uint8_t perm = g_midiRouter.nMaillonsPermanents();
    int debut = 0;
    for (uint8_t e = 0; e < n; e++) {
      String nom;
      if (debut >= 0 && debut <= (int)c.script.length()) {
        const int virgule = c.script.indexOf(',', debut);
        nom   = (virgule < 0) ? c.script.substring(debut) : c.script.substring(debut, virgule);
        debut = (virgule < 0) ? -1 : virgule + 1;
      }
      nom.trim();
      if (e < perm) continue;          // zone MAIN : la cue passe son chemin
      g_midiRouter.chargerScriptNomme(nom.c_str(), false, e);
    }
  }
  if (c.paramsScript.length()) g_midiRouter.setParamsScript(c.paramsScript);

  /* 2 bis. L'ECHANTILLON. `engine = -2` veut dire « lecteur d'echantillons » —
   * l'equivalent embarque de play-sf. Le fichier voyage dans les params, sous
   * `sample=<nom>` : pas de champ nouveau dans la ligne de cue, et le nom est
   * celui de storage (le panier de la carte), pas le chemin du poste.
   *
   * C'ETAIT LE MAILLON MANQUANT. Le lecteur existait et marchait ; rien ne lui
   * disait quoi jouer depuis une composition. Un bloc play-sf laissait donc la
   * carte sur son moteur precedent, et une note y sonnait en SINUS — constate
   * par l'utilisateur, « j'entends un sinus quand je joue trig wav ». */
  if (c.engine == -2) {
    /* PLUSIEURS SONS A LA FOIS. Une cue peut porter DEUX pistes instrument avec
     * deux sons — c'etait impossible : le magasin ne tenait qu'un echantillon et
     * le lecteur n'avait qu'une voix. Les deux sont leves (SampleStore.h,
     * AudioEngine.h).
     *
     * FORMAT : les valeurs multiples sont separees par des VIRGULES, et la
     * POSITION fait la correspondance — exactement comme la chaine de scripts
     * d'une cue. « sample=a.wav,b.wav ; loop=1,0 » lance a.wav en boucle et
     * b.wav en coup unique. Une liste `loop` plus courte que `sample` complete
     * avec sa derniere valeur : ecrire « loop=0 » une fois vaut pour tous.
     * `bloc=` porte l'identifiant du bloc de chaque son (MESURES §165), sans
     * completion : c'est par lui que le volume retrouve une voix qui joue. */
    String noms, boucles, gains, blocs;
    bool surCue = true;      // defaut : le comportement d'origine de play-sf
    int debut = 0;
    while (debut < (int)c.params.length()) {
      int fin = c.params.indexOf(';', debut);
      if (fin < 0) fin = c.params.length();
      String kv = c.params.substring(debut, fin);
      const int eq = kv.indexOf('=');
      if (eq > 0) {
        String cle = kv.substring(0, eq); cle.trim();
        if      (cle == "sample") { noms = kv.substring(eq + 1); noms.trim(); }
        else if (cle == "loop")   { boucles = kv.substring(eq + 1); boucles.trim(); }
        else if (cle == "gain")   { gains   = kv.substring(eq + 1); gains.trim(); }
        else if (cle == "bloc")   { blocs   = kv.substring(eq + 1); blocs.trim(); }
        else if (cle == "oncue")  surCue = (kv.substring(eq + 1).toFloat() >= 0.5f);
        else if (cle == "volume") AudioEngine::setVolume(kv.substring(eq + 1).toFloat());
      }
      debut = fin + 1;
    }

    /* ON COUPE D'ABORD ce que la cue precedente tenait. Sans cela une boucle
     * survivrait a la cue qui l'a lancee — le comportement fantome corrige
     * partout ailleurs. Les sons de CETTE cue repartent juste apres.
     * SAUF les clips des play list (§196) : poserListes, plus bas, fait taire
     * ceux d'une liste qui part, et laisse jouer celle qui CONTINUE — une
     * chaine de cellules porte le meme bloc d'une cue a la suivante. */
    AudioEngine::arreterEchantillon(/*listesComprises=*/false);
    AudioEngine::fixerDeclenchementSurCue(surCue);
    /* LES PLAY LIST de la cue : `liste=…` et ses champs (AudioEngine.h). */
    const bool listes = _valeurDe(c.params, "liste").length() > 0;

    if (!noms.length() && !listes) {
      Serial.println("[cues] aucun echantillon nomme");
    } else {
      /* Armer le lecteur une fois — il ne charge rien, tout est deja en PSRAM.
       * Le premier nom sert d'echantillon par defaut au clavier ; une cue qui
       * n'a que des play list arme le lecteur sans clavier. */
      const String premier = _premierSon(noms);
      String raison;
      if (!AudioEngine::setSampler(premier.c_str(), raison))
        Serial.printf("[cues] lecteur non arme : %s\n", raison.c_str());

      int dn = 0, db = 0, dg = 0, dk = 0, rang = 0;
      bool  derniereBoucle = false;
      float dernierGain    = 1.0f;
      while (dn < (int)noms.length()) {
        int fn = noms.indexOf(',', dn); if (fn < 0) fn = noms.length();
        String nom = noms.substring(dn, fn); nom.trim();

        /* La boucle de MEME RANG, ou la derniere lue si la liste est plus
         * courte. Une seule valeur vaut donc pour tous les sons. */
        if (db < (int)boucles.length()) {
          int fb = boucles.indexOf(',', db); if (fb < 0) fb = boucles.length();
          derniereBoucle = (boucles.substring(db, fb).toFloat() >= 0.5f);
          db = fb + 1;
        }
        /* LE GAIN DE MEME RANG. Sans lui, deux sons a plein volume SATURENT :
         * mesure, nappe seule 22 503 et balayage seul 24 575, mais les deux
         * ensemble 32 768 — le plafond. Chaque voix porte donc le volume de SON
         * bloc, comme chaque piste a le sien dans le navigateur. Meme regle de
         * completion : une seule valeur vaut pour tous. */
        if (dg < (int)gains.length()) {
          int fg = gains.indexOf(',', dg); if (fg < 0) fg = gains.length();
          dernierGain = gains.substring(dg, fg).toFloat();
          dg = fg + 1;
        }

        /* LE BLOC DE MEME RANG : l'etiquette de la voix, par laquelle le volume
         * la retrouve pendant qu'elle joue (MESURES §165). Pas de completion :
         * un bloc par son, ou aucun (0). */
        uint32_t bloc = 0;
        if (dk < (int)blocs.length()) {
          int fk = blocs.indexOf(',', dk); if (fk < 0) fk = blocs.length();
          bloc = (uint32_t)blocs.substring(dk, fk).toInt();
          dk = fk + 1;
        }
        /* Le clavier joue le PREMIER son (setSampler ci-dessus) : il prend
         * aussi son bloc et son volume. */
        if (rang == 0) AudioEngine::fixerClavier(bloc, dernierGain);

        /* EN MODE CLAVIER on ne declenche pas : arriver sur la cue ARME et
         * attend la premiere touche — sinon le bloc partirait tout seul. */
        if (nom.length() && surCue) {
          if (!AudioEngine::declencherEchantillon(nom.c_str(), derniereBoucle, dernierGain,
                                                  0.0f, bloc))
            Serial.printf("[cues] echantillon « %s » absent de storage\n", nom.c_str());
        }
        dn = fn + 1; rang++;
      }
      if (rang) Serial.printf("[cues] %d echantillon(s) %s\n", rang,
                              surCue ? "lances" : "armes pour le clavier");
    }
    /* Apres le lecteur : ses clips se cherchent dans le magasin qu'il a
     * charge. Une cue sans liste les retire (une banque vide). */
    AudioEngine::poserListes(c.params);
    /* PAS de `return` : la ligne de journal en fin de fonction vaut pour toutes
     * les cues, et la brancher ici la ferait disparaitre pour celles-ci. Le
     * bloc suivant ne peut pas se declencher — -2 n'est pas >= 0. */
  }

  /* Et une cue qui ne parle plus d'echantillon du tout (engine != -2) arrete
   * celui qui tournait : quitter la cue coupe le son, comme le `dispose()` du
   * BufferSource cote navigateur. */
  if (c.engine != -2) { AudioEngine::arreterEchantillon(); AudioEngine::poserListes(String()); }

  /* "PRELOAD" (a playlist block's option), on EVERY cue — a silent cue prepares the next
   * one too. The NEXT cue — the one GO would play, the first after the last if the list
   * loops — gets the heads of its marked lists read ahead. We do not know where the user
   * will go (jumps are free): it is an OPTION, per block, for linear compositions. The
   * budget bounds it (SdStream.h); if we jump elsewhere, these heads evict themselves. */
  {
    const int n = nombre();
    const int nextIndex = (n <= 0) ? -1 : ((_index + 1 < n) ? _index + 1 : (_boucle ? 0 : -1));
    /* ALWAYS called: a next cue that wants none CLEARS the previous preload. */
    Cue next;
    if (nextIndex >= 0 && nextIndex != _index && lire(nextIndex, next) && next.engine == -2)
      AudioEngine::preloadLists(next.params);
    else
      AudioEngine::preloadLists(String());
  }

  // 2. L'audio, s'il y en a. Une carte sans moteur audio ecrit engine = -1 et
  //    ne paye rien de tout ceci.
  if (c.engine >= 0) {
    AudioEngine::setEngine(c.engine);
    if (c.params.length()) {
      AudioEngine::Params p = AudioEngine::params();
      /* Le VOLUME voyage dans la meme chaine de parametres mais ne vit pas dans
       * `Params` : c'est un gain de SORTIE, pas un reglage de Plaits. On le
       * retient a part, et on ne l'applique que s'il etait present — une cue
       * qui n'en parle pas ne doit pas remettre le gain a une valeur par
       * defaut, elle doit laisser celui d'avant. */
      float volumeCue = -1.f;
      int debut = 0;
      while (debut < (int)c.params.length()) {
        int fin = c.params.indexOf(';', debut);
        if (fin < 0) fin = c.params.length();
        String kv = c.params.substring(debut, fin);
        const int eq = kv.indexOf('=');
        if (eq > 0) {
          String cle = kv.substring(0, eq);       cle.trim();
          const float v = kv.substring(eq + 1).toFloat();
          if      (cle == "harmonics")  p.harmonics = v;
          else if (cle == "timbre")     p.timbre    = v;
          else if (cle == "morph")      p.morph     = v;
          else if (cle == "decay")      p.decay     = v;
          else if (cle == "lpg_colour") p.lpgColour = v;
          else if (cle == "volume")     volumeCue   = v;
        }
        debut = fin + 1;
      }
      AudioEngine::setParams(p);
      if (volumeCue >= 0.f) AudioEngine::setVolume(volumeCue);
    }
  }
  Serial.printf("[cues] %d « %s » duree=%.1fs script=%s engine=%d\n",
                _index, c.nom.c_str(), c.duree,
                c.script.length() ? c.script.c_str() : "(aucun)", c.engine);
}

}  // namespace

namespace {

/* ── LA LISTE EN PSRAM (MESURES §157) ──────────────────────────────────────
 * Elle ne vivait que dans storage, relue a chaque changement de cue — par budget :
 * le tas INTERNE ne pouvait pas la porter. La PSRAM, si (8 Mo, §152). Lue de la
 * memoire a chaque GO, ecrite en flash au premier silence (Differe) : cinq
 * reecritures de cues.txt pendant le jeu avaient coute un bloc audio de 26 ms. */
SemaphoreHandle_t _verrouTexte = nullptr;
std::shared_ptr<char> _texte;
size_t _octets = 0;
/* LE COMPTE DES CUES, GARDE (§181). nombre() parcourait toute la liste — une
 * String par ligne — et il est appele a chaque GO, et desormais a chaque
 * annonce de l'etat du transport. Garde par generation du texte : une liste
 * adoptee pendant un comptage ne laisse jamais un compte perime. */
uint32_t _generationTexte = 0;
uint32_t _generationCompte = 0;
int      _compte = -1;

std::shared_ptr<char> _texteCourant(size_t& n, uint32_t* generation = nullptr) {
  if (!_verrouTexte) { n = 0; return nullptr; }
  xSemaphoreTake(_verrouTexte, portMAX_DELAY);
  auto t = _texte;
  n = _octets;
  if (generation) *generation = _generationTexte;
  xSemaphoreGive(_verrouTexte);
  return t;
}
void _adopterTexte(std::shared_ptr<char> t, size_t n) {
  xSemaphoreTake(_verrouTexte, portMAX_DELAY);
  _texte = t;
  _octets = n;
  _generationTexte++;
  xSemaphoreGive(_verrouTexte);
}
std::shared_ptr<char> _tamponPsram(size_t n) {
  char* p = (char*)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) return nullptr;
  return std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); });
}

// Chaque ligne du texte ; `f` rend false pour s'arreter.
template <typename F>
void _pourChaqueLigne(const char* d, size_t n, F f) {
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && d[j] != '\n') j++;
    String l;
    l.concat(d + i, (unsigned)(j - i));
    if (!f(l)) return;
    i = j + 1;
  }
}

}  // namespace

/* « la liste boucle », lue dans options.txt de la composition ouverte
 * (« boucle=1 ») — absent : elle s'arrete a la derniere cue. */
static void _annoncer();          // plus bas : l'etat du transport, a qui l'ecoute

static bool _boucleDeLaComposition() {
  const String fichier = Repertoire::chemin(Repertoire::OPTIONS);
  std::shared_ptr<char> t; size_t n = 0;
  if (!fichier.length() || !Differe::lire(fichier.c_str(), t, n)) return false;
  String texte;
  texte.concat(t.get(), (unsigned)n);
  return texte.indexOf("boucle=1") >= 0;
}

/* La liste de la composition ouverte, en PSRAM : ce qui attend le silence
 * d'abord, la flash sinon. Aucune composition, ou pas de liste : vide. */
static void _lireLaListe() {
  std::shared_ptr<char> t; size_t n = 0;
  const String fichier = Repertoire::chemin(Repertoire::CUES);
  if (!fichier.length() || !Differe::lire(fichier.c_str(), t, n)) { t = _tamponPsram(0); n = 0; }
  if (t) _adopterTexte(t, n);
}

bool monter() {
  if (_monte) return true;
  if (!LittleFS.begin(true, BASE, 10, PARTITION)) return false;
  if (!_verrouTexte) _verrouTexte = xSemaphoreCreateMutex();
  _lireLaListe();                 // une fois, en PSRAM
  _monte = true;
  return true;
}

void recharger() {
  if (!monter()) return;
  _lireLaListe();
  const bool b = _boucleDeLaComposition();
  VerrouSeq verrou;
  _boucle = b;
  _index = 0;
  _enPause = false;
  _lecture = false;
  _dureeCourante = 0.0f;
  _oublierCourbes();
  _annoncer();
}

int nombre() {
  if (!monter()) return 0;
  size_t n = 0;
  uint32_t g = 0;
  auto t = _texteCourant(n, &g);
  if (!t) return 0;
  xSemaphoreTake(_verrouTexte, portMAX_DELAY);
  const int garde = (_compte >= 0 && _generationCompte == g) ? _compte : -1;
  xSemaphoreGive(_verrouTexte);
  if (garde >= 0) return garde;
  int k = 0;
  _pourChaqueLigne(t.get(), n, [&](const String& l) { if (_ligneUtile(l)) k++; return true; });
  xSemaphoreTake(_verrouTexte, portMAX_DELAY);
  if (g == _generationTexte) { _compte = k; _generationCompte = g; }
  xSemaphoreGive(_verrouTexte);
  return k;
}

bool lire(int index, Cue& sortie) {
  if (index < 0 || !monter()) return false;
  size_t n = 0;
  auto t = _texteCourant(n);
  if (!t) return false;
  int k = 0;
  bool trouve = false;
  _pourChaqueLigne(t.get(), n, [&](const String& l) {
    if (!_ligneUtile(l)) return true;
    if (k++ != index) return true;
    sortie.nom    = _champ(l, 0);
    sortie.duree  = _champ(l, 1).toFloat();
    sortie.script = _champ(l, 2);
    const String e = _champ(l, 3);
    sortie.engine = e.length() ? e.toInt() : -1;
    sortie.params = _champ(l, 4);
    sortie.paramsScript = _champ(l, 5);
    sortie.env          = _champ(l, 6);   // absent sur une cue sans automation
    trouve = true;
    return false;
  });
  return trouve;
}

int moteurEmploye(String& son) {
  son = "";
  if (!monter()) return -1;
  size_t n = 0;
  auto t = _texteCourant(n);
  if (!t) return -1;
  int moteur = -1;
  _pourChaqueLigne(t.get(), n, [&](const String& l) {
    if (!_ligneUtile(l)) return true;
    const String e = _champ(l, 3);
    const int m = e.length() ? e.toInt() : -1;
    if (m >= 0) { moteur = m; return false; }
    if (m != -2) return true;
    const String params = _champ(l, 4);
    const String premier = _premierSon(_valeurDe(params, "sample"));
    /* Un son de play-sf, ou une play list (§196) — sans clavier : son vide. */
    if (!premier.length() && !_valeurDe(params, "liste").length())
      return true;                               // un bloc sans son : la suivante
    son = premier;
    moteur = -2;
    return false;
  });
  return moteur;
}

bool ecrireTout(const String& contenuTexte) {
  if (!monter()) return false;
  const size_t n = contenuTexte.length();
  auto t = _tamponPsram(n);
  if (!t) return false;
  if (n) memcpy(t.get(), contenuTexte.c_str(), n);
  /* Une liste va dans une composition : sur un repertoire vide, la premiere
   * se cree (MESURES §186). */
  String raison;
  if (!Repertoire::assurerOuverte("", raison)) {
    Serial.printf("[cues] liste refusee : %s\n", raison.c_str());
    return false;
  }
  // Rendue tout de suite ; en flash au premier silence.
  if (!Differe::poserFichier(Repertoire::chemin(Repertoire::CUES).c_str(), t, n)) return false;
  _adopterTexte(t, n);
  /* UNE LISTE PLUS COURTE NE LAISSE PAS LA TETE DEHORS. L'index memorise
   * pouvait depasser la nouvelle fin — installer une composition plus courte
   * laissait alors le transport bloque : `demarrer()` ne trouvait plus sa cue et
   * abandonnait, porte de silence fermee, pendant qu'un `goto` declenchait des
   * sons dans le vide. Trouve par le banc play-sf (MESURES §136). */
  const int total = nombre();
  {
    VerrouSeq verrou;
    if (_index >= total) _index = (total > 0) ? total - 1 : 0;
  }
  Serial.printf("[cues] liste recue : %u o, %d cues (flash au premier silence)\n", (unsigned)n, total);
  annoncerEtat();                 // le compte et la cue suivante ont pu changer (§181)
  return true;
}

String contenu() {
  if (!monter()) return String("");
  size_t n = 0;
  auto t = _texteCourant(n);
  String out;
  if (t && n) out.concat(t.get(), (unsigned)n);
  return out;
}

/* ── LA CARTE ANNONCE SON TRANSPORT ────────────────────────────────────────
 *
 * UN SEUL SEQUENCEUR, et il est ici. L'app n'en tient plus : elle envoie des
 * instructions (play, stop, go, goto) et REFLETE ce que la carte lui dit. C'est
 * la regle du projet — « tout s'execute sur la carte ; le navigateur ne fait que
 * preparer » — appliquee au transport, ou elle ne l'etait pas : le navigateur
 * avait son propre tick, decidait des changements de cue, et poussait ensuite le
 * contenu. Deux sequenceurs pour une composition.
 *
 * ON POUSSE UN EVENEMENT, ON NE SE FAIT PAS SONDER. Sonder est precisement ce
 * qui fait giguer la carte (MESURES §82 : ce qui coute, c'est le nombre de
 * paquets recus). Un changement de cue est rare ; entre deux, l'app interpole
 * sa barre de progression — elle connait la duree — et se resynchronise a
 * chaque annonce.
 *
 * Appele sous le verrou du sequenceur, depuis la boucle ou le serveur web :
 * `nidmi_ws_pousser` ne fait qu'empiler dans une file, loopTask envoie. */
static void _annoncer() {
  const int n = nombre();
  /* L'ETAT POUR LE BUS (§181), TOUJOURS : un bouton lit r("sys.cue") qu'un
   * onglet ecoute ou non. */
  if (_rappelEtat) {
    Etat e;
    e.lecture = _lecture; e.pause = _enPause; e.boucle = _boucle; e.auDemarrage = _auDemarrage;
    e.index = _index; e.nombre = n;
    e.suivante = (n <= 0) ? -1 : ((_index + 1 < n) ? _index + 1 : (_boucle ? 0 : -1));
    _rappelEtat(e);
  }
  if (!nidmi_ws_quelqu_un_ecoute()) return;   // personne n'ecoute : rien a dire
  char trame[80];
  /* CINQ CHAMPS : le dernier dit GELEE. Ajoute en queue — l'app destructure,
   * une trame a quatre champs reste donc lisible par une app a jour. */
  snprintf(trame, sizeof trame, "NIDMI_CUE:%d\x1f%s\x1f%.2f\x1f%d\x1f%s",
           _index, _lecture ? "1" : "0", restantSec(), n,
           _enPause ? "1" : "0");
  nidmi_ws_pousser(trame);
}

bool aller(int index) {
  VerrouSeq verrou;
  Cue c;
  if (!lire(index, c)) return false;
  _index = index;
  _dureeCourante = c.duree;
  _debutMs = millis();
  _enPause = false;          // changer de cue annule une pause en cours
  const uint32_t t0 = micros();
  _appliquer(c);
  Chronos::applicationCue.noter(micros() - t0);
  _annoncer();
  return true;
}

void demarrer() {
  VerrouSeq verrou;
  /* REPRISE : on ne recharge pas la cue, on rend son temps. La recharger
   * relancerait son script (loadbang, etats remis a plat) et son moteur — une
   * reprise qui recommence n'est pas une reprise. */
  if (_enPause) {
    _enPause = false;
    _debutMs = millis() - _ecouleMs;
    _lecture = true;
    /* La porte est deja ouverte — la pause n'y touche plus. On la rouvre
     * quand meme : /api/audio/stop peut l'avoir fermee pendant la pause, et
     * une reprise muette serait le meme mensonge d'etat a l'envers. */
    AudioEngine::ouvrirSon();
    g_midiRouter.fixerTransport(true);
    _annoncer();
    Serial.println("[cues] reprise");
    return;
  }
  /* ON RAMENE LA TETE DANS LA LISTE plutot que d'abandonner. L'index vient de la
   * session precedente ou d'une composition plus longue : s'il depasse, la bonne
   * reponse est de jouer la premiere cue, pas de refuser de demarrer en silence. */
  const int total = nombre();
  if (total <= 0) { Serial.println("[cues] aucune cue a jouer"); return; }
  if (_index < 0 || _index >= total) {
    Serial.printf("[cues] index %d hors de la liste (%d cues) — ramene a 0\n", _index, total);
    _index = 0;
  }
  if (!aller(_index)) { Serial.println("[cues] cue illisible"); return; }
  _lecture = true;
  AudioEngine::ouvrirSon();     // PLAY ouvre la porte : c'est le transport qui decide
  /* ET L'HORLOGE DES SCRIPTS. Le sequenceur embarque et le transport de l'app
   * menent au meme drapeau : une carte headless ne doit pas avoir sa propre
   * idee de « en lecture ». */
  g_midiRouter.fixerTransport(true);
  _annoncer();
  Serial.println("[cues] lecture");
}

/* PAUSE : on gele le DEROULE TEMPOREL de la cue — son decompte, son automation
 * d'enveloppe, son enchainement, l'horloge de ses scripts — et RIEN D'AUTRE.
 *
 * LA PORTE DE SILENCE NE BOUGE PAS. C'est toute la difference entre pause et
 * arret, et la carte n'en faisait pas : elle appelait couperSon() ici, si bien
 * que « pause » etait un arret qui se souvenait de l'heure. Ce qui sonne
 * continue donc de sonner — une note tenue, un echantillon en cours, la sortie
 * continue de Plaits. La regle est celle du navigateur, ecrite depuis
 * longtemps : « la pause suspend le deroule temporel d'une case, pas le son ».
 *
 * L'HORLOGE DES SCRIPTS S'ARRETE, elle : un metro() est du temps qui passe.
 * Les DIFFERES, en revanche, continuent d'etre pompes par loopTask — ils ne
 * sont pas gates par le transport. C'est ce qu'il faut : le note-off d'un
 * makenote() parti avant la pause arrive quand meme, au lieu de laisser une
 * note coincee jusqu'a la reprise. Une pause ne fabrique pas de notes
 * fantomes.
 *
 * LE MIDI ENTRANT continue aussi d'etre traite — c'est du jeu live, il n'a
 * jamais dependu du transport. */
void pauser() {
  VerrouSeq verrou;
  if (!_lecture) return;
  _ecouleMs = millis() - _debutMs;
  _enPause = true;
  _lecture = false;
  g_midiRouter.fixerTransport(false);
  _annoncer();
  Serial.printf("[cues] pause a %.2f s — le son continue\n", _ecouleMs / 1000.0f);
}

/* ARRET : lui ferme la porte. Depuis qu'il est le SEUL a la fermer, c'est ce
 * qui le distingue de la pause — pas une nuance de decompte. */
void arreter() {
  VerrouSeq verrou;
  _enPause = false;          // un arret franc oublie la position gelee
  _lecture = false;
  AudioEngine::couperSon();
  g_midiRouter.fixerTransport(false);   // idem : l'horloge des scripts s'arrete
  _annoncer();
  Serial.println("[cues] arret");
}

/* LA CUE SUIVANTE. Rend vrai si une cue a ete (re)appliquee. En fin de liste
 * (§181) : la premiere si la liste boucle ; sinon, un GO a la main ne fait
 * RIEN — il n'y a pas de suivante, et l'arret a son propre bouton —, et la fin
 * d'une derniere cue minutee ARRETE, comme toujours : la liste est jouee. */
static bool _avancer(bool aLaMain) {
  const int n = nombre();
  if (n <= 0) return false;
  int suiv = _index + 1;
  if (suiv >= n) {
    if (!_boucle) {
      Serial.println(aLaMain ? "[cues] fin de liste : pas de suivante" : "[cues] fin de liste");
      if (!aLaMain) arreter();
      return false;
    }
    suiv = 0;
    Serial.println("[cues] fin de liste : retour a la premiere (la liste boucle)");
  }
  aller(suiv);
  if (_lecture) AudioEngine::ouvrirSon();
  return true;
}

void suivant() {
  VerrouSeq verrou;
  _avancer(/*aLaMain=*/true);
}

/* La cue precedente. A la premiere : la derniere si la liste boucle, rien sinon
 * — revenir « avant le debut » n'a pas de sens. */
void precedent() {
  VerrouSeq verrou;
  const int n = nombre();
  if (n <= 0) return;
  int prec = _index - 1;
  if (prec < 0) {
    if (!_boucle) return;
    prec = n - 1;
  }
  aller(prec);
  if (_lecture) AudioEngine::ouvrirSon();
}

void boucle() {
  VerrouSeq verrou;
  if (!_lecture || _dureeCourante <= 0.0f) return;   // 0 = infinie, on attend un GO

  /* L'AUTOMATION, bridee a 50 Hz. `boucle()` tourne a chaque tour de
   * nidmi_loop — soit des milliers de fois par seconde. Appliquer a ce
   * rythme-la reecrirait le patch de Plaits pour rien : 20 ms suffisent
   * largement a l'oreille, et c'est deja plus fin que le tick du sequenceur du
   * navigateur. */
  const uint32_t maintenant = millis();
  if (_nCourbes && (maintenant - _dernierAppliqueMs) >= 20) {
    _dernierAppliqueMs = maintenant;
    const float t = (float)(maintenant - _debutMs) / (_dureeCourante * 1000.0f);
    _appliquerAutomation(t < 0.f ? 0.f : (t > 1.f ? 1.f : t));
  }

  const uint32_t duree = (uint32_t)(_dureeCourante * 1000.0f);
  if ((maintenant - _debutMs) >= duree) {
    /* LE RETARD SUR L'ECHEANCE (§172) : ce que la boucle a mis a voir que la
     * cue etait finie. Il atteignait 0,5 s quand le serveur web l'affamait. */
    const uint32_t echeance = _debutMs + duree;
    const uint32_t retard = maintenant - echeance;
    Chronos::retardCue.noter(retard * 1000u);
    const bool avance = _avancer(/*aLaMain=*/false);
    /* SANS DERIVE : la cue suivante part de l'ECHEANCE, pas de l'instant ou la
     * boucle l'a vue — sinon chaque retard s'ajoutait aux suivants et une
     * liste minutee derivait. Au-dela d'un quart de seconde (la carte etait
     * arretee), on repart de maintenant : rattraper enchainerait des cues.
     * `avance` et non « l'index a change » : une liste d'UNE cue qui boucle
     * se rejoue sur elle-meme (§181). */
    if (_lecture && avance && retard < 250) _debutMs = echeance;
  }
}

bool  enLecture()   { VerrouSeq verrou; return _lecture; }
/* GELEE, ET PAS ARRETEE. Trois etats de transport, pas deux : sans ce drapeau
 * l'app deduisait la pause d'un decompte non nul — inference fausse pour une
 * cue infinie, qui ne decompte rien et paraissait donc arretee alors qu'elle
 * sonnait. La carte le DIT au lieu de le laisser deviner. */
bool  enPause()     { VerrouSeq verrou; return _enPause; }
int   indexCourant(){ VerrouSeq verrou; return _index; }
float restantSec() {
  VerrouSeq verrou;
  if (_dureeCourante <= 0.0f) return 0.0f;         // cue infinie : rien a decompter
  /* EN PAUSE, LE DECOMPTE EXISTE ENCORE — il est gele. Rendre 0 faisait croire
   * a l'app que la cue etait finie, et sa barre de progression se vidait au
   * moment precis ou l'usager voulait la voir tenir. */
  if (!_lecture && !_enPause) return 0.0f;
  const uint32_t ecoule = _enPause ? _ecouleMs : (millis() - _debutMs);
  const float reste = _dureeCourante - (ecoule / 1000.0f);
  return reste > 0 ? reste : 0.0f;
}

// ── Options du transport (MESURES §181, §186) ────────────────────────────────
// Elles valent tout de suite ; la flash suit au silence (Differe, §157).
void fixerBoucle(bool oui) {
  {
    VerrouSeq verrou;
    if (_boucle == oui) return;
    _boucle = oui;
    _annoncer();
  }
  // La forme de la piece : dans SA composition (options.txt), pas dans la carte.
  String raison;
  if (Repertoire::assurerOuverte("", raison)) {
    const char* texte = oui ? "boucle=1\n" : "boucle=0\n";
    Differe::poserFichierCopie(Repertoire::chemin(Repertoire::OPTIONS).c_str(), texte, strlen(texte));
  }
  Serial.printf("[cues] la liste %s\n", oui ? "boucle" : "s'arrete a la derniere cue");
}
bool boucleActive() { VerrouSeq verrou; return _boucle; }

void fixerLectureAuDemarrage(bool oui) {
  {
    VerrouSeq verrou;
    if (_auDemarrage == oui) return;
    _auDemarrage = oui;
    _annoncer();
  }
  Differe::nvsOctet(NVS_OPTIONS, "auto", oui ? 1 : 0);
  Serial.printf("[cues] lecture au demarrage : %s\n", oui ? "oui" : "non");
}
bool lectureAuDemarrage() { VerrouSeq verrou; return _auDemarrage; }

void restaurerOptions() {
  bool a = false, ancienneBoucle = false;
  {
    Preferences p;
    if (p.begin(NVS_OPTIONS, true)) {           // jamais ecrites : les defauts
      a = p.getUChar("auto", 0) != 0;
      ancienneBoucle = p.isKey("boucle");
      p.end();
    }
  }
  /* « boucle » vivait ici jusqu'au §186 : la cle orpheline s'efface, au silence. */
  if (ancienneBoucle) Differe::nvsRetirer(NVS_OPTIONS, "boucle");
  monter();
  const bool b = _boucleDeLaComposition();
  VerrouSeq verrou;
  _boucle = b; _auDemarrage = a;
}

void surChangement(void (*rappel)(const Etat&)) { VerrouSeq verrou; _rappelEtat = rappel; }
void annoncerEtat() { VerrouSeq verrou; _annoncer(); }

}  // namespace Cues
