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

// Applique a la carte ce que la cue decrit. C'est ICI que « changer de cue »
// prend un sens materiel — et nulle part dans le navigateur.
void _appliquer(const Cue& c) {
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
  if ((millis() - _debutMs) >= (uint32_t)(_dureeCourante * 1000.0f)) suivant();
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
