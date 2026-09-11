#include "CueStore.h"
#include "ScriptStore.h"
#include "../midi/MidiRouter.h"
#include "../Globals.h"
#include "../audio/AudioEngine.h"
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
  // 1. Le script .nms d'abord : il transforme le MIDI, donc il doit etre en
  //    place avant que la moindre note n'arrive.
  g_midiRouter.chargerScriptNomme(c.script.c_str(), false);
  if (c.paramsScript.length()) g_midiRouter.setParamsScript(c.paramsScript);

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
  Serial.printf("[cues] liste ecrite : %u o, %d cues\n", (unsigned)n, nombre());
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

bool aller(int index) {
  Cue c;
  if (!lire(index, c)) return false;
  _index = index;
  _dureeCourante = c.duree;
  _debutMs = millis();
  _appliquer(c);
  return true;
}

void demarrer() {
  if (!aller(_index)) { Serial.println("[cues] aucune cue a jouer"); return; }
  _lecture = true;
  AudioEngine::ouvrirSon();     // PLAY ouvre la porte : c'est le transport qui decide
  Serial.println("[cues] lecture");
}

void arreter() {
  _lecture = false;
  AudioEngine::couperSon();
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
int   indexCourant(){ return _index; }
float restantSec() {
  if (!_lecture || _dureeCourante <= 0.0f) return 0.0f;
  const uint32_t ecoule = millis() - _debutMs;
  const float reste = _dureeCourante - (ecoule / 1000.0f);
  return reste > 0 ? reste : 0.0f;
}

}  // namespace Cues
