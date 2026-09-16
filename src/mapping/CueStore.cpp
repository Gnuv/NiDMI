#include "CueStore.h"
#include "ScriptStore.h"
#include "../midi/MidiRouter.h"
#include "../Globals.h"
#include "../audio/AudioEngine.h"
#include "../server/ServerCore.h"     // nidmi_ws_pousser : la carte ANNONCE son etat
#include <LittleFS.h>

namespace Cues {
namespace {

constexpr const char* PARTITION = "mapfs";
constexpr const char* BASE      = "/mapfs";
constexpr const char* FICHIER   = "/cues.txt";

bool  _monte    = false;
bool  _lecture  = false;
int   _index    = 0;
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
   * l'equivalent embarque de trig-wav. Le fichier voyage dans les params, sous
   * `sample=<nom>` : pas de champ nouveau dans la ligne de cue, et le nom est
   * celui de mapfs (le panier de la carte), pas le chemin du poste.
   *
   * C'ETAIT LE MAILLON MANQUANT. Le lecteur existait et marchait ; rien ne lui
   * disait quoi jouer depuis une composition. Un bloc trig-wav laissait donc la
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
     * avec sa derniere valeur : ecrire « loop=0 » une fois vaut pour tous. */
    String noms, boucles, gains;
    bool surCue = true;      // defaut : le comportement d'origine de trig-wav
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
        else if (cle == "oncue")  surCue = (kv.substring(eq + 1).toFloat() >= 0.5f);
        else if (cle == "volume") AudioEngine::setVolume(kv.substring(eq + 1).toFloat());
      }
      debut = fin + 1;
    }

    /* ON COUPE D'ABORD ce que la cue precedente tenait. Sans cela une boucle
     * survivrait a la cue qui l'a lancee — le comportement fantome corrige
     * partout ailleurs. Les sons de CETTE cue repartent juste apres. */
    AudioEngine::arreterEchantillon();
    AudioEngine::fixerDeclenchementSurCue(surCue);

    if (!noms.length()) {
      Serial.println("[cues] aucun echantillon nomme");
    } else {
      /* Armer le lecteur une fois — il ne charge rien, tout est deja en PSRAM.
       * Le premier nom sert d'echantillon par defaut au clavier. */
      String premier = noms.substring(0, (noms.indexOf(',') < 0) ? noms.length()
                                                                 : noms.indexOf(','));
      premier.trim();
      String raison;
      if (!AudioEngine::setSampler(premier.c_str(), raison, /*persister=*/false))
        Serial.printf("[cues] lecteur non arme : %s\n", raison.c_str());

      int dn = 0, db = 0, dg = 0, rang = 0;
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

        /* EN MODE CLAVIER on ne declenche pas : arriver sur la cue ARME et
         * attend la premiere touche — sinon le bloc partirait tout seul. */
        if (nom.length() && surCue) {
          if (!AudioEngine::declencherEchantillon(nom.c_str(), derniereBoucle, dernierGain))
            Serial.printf("[cues] echantillon « %s » absent de mapfs\n", nom.c_str());
        }
        dn = fn + 1; rang++;
      }
      Serial.printf("[cues] %d echantillon(s) %s\n", rang,
                    surCue ? "lances" : "armes pour le clavier");
    }
    /* PAS de `return` : la ligne de journal en fin de fonction vaut pour toutes
     * les cues, et la brancher ici la ferait disparaitre pour celles-ci. Le
     * bloc suivant ne peut pas se declencher — -2 n'est pas >= 0. */
  }

  /* Et une cue qui ne parle plus d'echantillon du tout (engine != -2) arrete
   * celui qui tournait : quitter la cue coupe le son, comme le `dispose()` du
   * BufferSource cote navigateur. */
  if (c.engine != -2) AudioEngine::arreterEchantillon();

  // 2. L'audio, s'il y en a. Une carte sans moteur audio ecrit engine = -1 et
  //    ne paye rien de tout ceci.
  if (c.engine >= 0) {
    AudioEngine::setEngine(c.engine, false);      // false : une cue n'ecrit pas la NVS
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

bool monter() {
  if (_monte) return true;
  if (!LittleFS.begin(true, BASE, 10, PARTITION)) return false;
  _monte = true;
  return true;
}

int nombre() {
  if (!monter() || !LittleFS.exists(FICHIER)) return 0;
  File f = LittleFS.open(FICHIER, FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available()) if (_ligneUtile(f.readStringUntil('\n'))) n++;
  f.close();
  return n;
}

bool lire(int index, Cue& sortie) {
  if (index < 0 || !monter() || !LittleFS.exists(FICHIER)) return false;
  File f = LittleFS.open(FICHIER, FILE_READ);
  if (!f) return false;
  int n = 0;
  bool trouve = false;
  while (f.available()) {
    const String l = f.readStringUntil('\n');
    if (!_ligneUtile(l)) continue;
    if (n++ != index) continue;
    sortie.nom    = _champ(l, 0);
    sortie.duree  = _champ(l, 1).toFloat();
    sortie.script = _champ(l, 2);
    const String e = _champ(l, 3);
    sortie.engine = e.length() ? e.toInt() : -1;
    sortie.params = _champ(l, 4);
    sortie.paramsScript = _champ(l, 5);
    sortie.env          = _champ(l, 6);   // absent sur une cue sans automation
    trouve = true;
    break;
  }
  f.close();
  return trouve;
}

bool ecrireTout(const String& contenuTexte) {
  if (!monter()) return false;
  File f = LittleFS.open(FICHIER, FILE_WRITE);
  if (!f) return false;
  const size_t n = f.print(contenuTexte);
  f.close();
  /* UNE LISTE PLUS COURTE NE LAISSE PAS LA TETE DEHORS. L'index memorise
   * pouvait depasser la nouvelle fin — installer une composition plus courte
   * laissait alors le transport bloque : `demarrer()` ne trouvait plus sa cue et
   * abandonnait, porte de silence fermee, pendant qu'un `goto` declenchait des
   * sons dans le vide. Trouve par le banc trig-wav (MESURES §136). */
  const int total = nombre();
  if (_index >= total) _index = (total > 0) ? total - 1 : 0;
  Serial.printf("[cues] liste ecrite : %u o, %d cues\n", (unsigned)n, total);
  return n == contenuTexte.length();
}

String contenu() {
  if (!monter() || !LittleFS.exists(FICHIER)) return String("");
  File f = LittleFS.open(FICHIER, FILE_READ);
  if (!f) return String("");
  String out;
  out.reserve(f.size() + 1);
  while (f.available()) out += (char)f.read();
  f.close();
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
 * Appele depuis loopTask (aller/demarrer/arreter/suivant y sont tous), donc
 * `nidmi_ws_pousser` est sur : il ne fait qu'empiler dans une file statique. */
static void _annoncer() {
  if (!nidmi_ws_quelqu_un_ecoute()) return;   // personne n'ecoute : rien a dire
  char trame[80];
  /* CINQ CHAMPS : le dernier dit GELEE. Ajoute en queue — l'app destructure,
   * une trame a quatre champs reste donc lisible par une app a jour. */
  snprintf(trame, sizeof trame, "NIDMI_CUE:%d\x1f%s\x1f%.2f\x1f%d\x1f%s",
           _index, _lecture ? "1" : "0", restantSec(), nombre(),
           _enPause ? "1" : "0");
  nidmi_ws_pousser(trame);
}

bool aller(int index) {
  Cue c;
  if (!lire(index, c)) return false;
  _index = index;
  _dureeCourante = c.duree;
  _debutMs = millis();
  _enPause = false;          // changer de cue annule une pause en cours
  _appliquer(c);
  _annoncer();
  return true;
}

void demarrer() {
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
  _enPause = false;          // un arret franc oublie la position gelee
  _lecture = false;
  AudioEngine::couperSon();
  g_midiRouter.fixerTransport(false);   // idem : l'horloge des scripts s'arrete
  _annoncer();
  Serial.println("[cues] arret");
}

void suivant() {
  const int n = nombre();
  if (n <= 0) return;
  const int suiv = _index + 1;
  if (suiv >= n) { Serial.println("[cues] fin de liste"); arreter(); return; }
  aller(suiv);
  if (_lecture) AudioEngine::ouvrirSon();
}

void boucle() {
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

  if ((maintenant - _debutMs) >= (uint32_t)(_dureeCourante * 1000.0f)) suivant();
}

bool  enLecture()   { return _lecture; }
/* GELEE, ET PAS ARRETEE. Trois etats de transport, pas deux : sans ce drapeau
 * l'app deduisait la pause d'un decompte non nul — inference fausse pour une
 * cue infinie, qui ne decompte rien et paraissait donc arretee alors qu'elle
 * sonnait. La carte le DIT au lieu de le laisser deviner. */
bool  enPause()     { return _enPause; }
int   indexCourant(){ return _index; }
float restantSec() {
  if (_dureeCourante <= 0.0f) return 0.0f;         // cue infinie : rien a decompter
  /* EN PAUSE, LE DECOMPTE EXISTE ENCORE — il est gele. Rendre 0 faisait croire
   * a l'app que la cue etait finie, et sa barre de progression se vidait au
   * moment precis ou l'usager voulait la voir tenir. */
  if (!_lecture && !_enPause) return 0.0f;
  const uint32_t ecoule = _enPause ? _ecouleMs : (millis() - _debutMs);
  const float reste = _dureeCourante - (ecoule / 1000.0f);
  return reste > 0 ? reste : 0.0f;
}

}  // namespace Cues
