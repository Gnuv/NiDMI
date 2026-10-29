#include "ScriptStore.h"
#include "../config/Stockage.h"
#include "../config/StorageIndex.h"
#include "../config/EcrituresDifferees.h"
#include "../server/WebDebugConsole.h"
#include "Repertoire.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ScriptStore {
namespace {

/* ── LES SCRIPTS GARDES EN PSRAM (MESURES §172) ─────────────────────────────
 * Une cue relisait ses scripts en flash a chaque arrivee. Ouvrir un fichier
 * LittleFS, c'est plusieurs lectures de metadonnees, et chacune suspend le
 * cache des DEUX coeurs : 28 a 51 ms par cue, et autant de micro-arrets pour
 * l'audio et le MIDI. Un script lu une fois reste ici, en PSRAM ; ecrire() et
 * supprimer() le tiennent a jour — ce sont les seuls chemins d'ecriture des
 * .nms (les ecritures differees ne font qu'y reporter ce qu'ecrire() a pose).
 * Seules des taches y touchent. */
constexpr int    kGardesMax = 32;
constexpr size_t kNomMax    = 160;         // un CHEMIN complet (§186), en PSRAM
struct Garde { char nom[kNomMax]; char* texte; size_t n; };
Garde* _gardes = nullptr;          // kGardesMax entrees, en PSRAM, prises au premier besoin
int    _nGardes = 0;
StaticSemaphore_t _tamponVerrouGardes;
SemaphoreHandle_t _verrouGardes = xSemaphoreCreateMutexStatic(&_tamponVerrouGardes);
struct VerrouGardes {
  VerrouGardes()  { if (_verrouGardes) xSemaphoreTake(_verrouGardes, portMAX_DELAY); }
  ~VerrouGardes() { if (_verrouGardes) xSemaphoreGive(_verrouGardes); }
};

Garde* _trouver(const char* nom) {
  for (int i = 0; i < _nGardes; i++)
    if (!strcmp(_gardes[i].nom, nom)) return &_gardes[i];
  return nullptr;
}

void _oublier(const char* nom) {
  VerrouGardes v;
  Garde* g = _gardes ? _trouver(nom) : nullptr;
  if (!g) return;
  heap_caps_free(g->texte);
  *g = _gardes[--_nGardes];          // la derniere prend sa place
}

void _garder(const char* nom, const char* texte, size_t n) {
  if (strlen(nom) >= kNomMax) return;          // nom trop long : on relira la flash
  char* copie = (char*)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copie) return;
  if (n) memcpy(copie, texte, n);
  VerrouGardes v;
  if (!_gardes) {
    _gardes = (Garde*)heap_caps_calloc(kGardesMax, sizeof(Garde), MALLOC_CAP_SPIRAM);
    if (!_gardes) { heap_caps_free(copie); return; }
  }
  Garde* g = _trouver(nom);
  if (!g) {
    if (_nGardes >= kGardesMax) { heap_caps_free(copie); return; }   // plein : la flash
    g = &_gardes[_nGardes++];
    strlcpy(g->nom, nom, kNomMax);
  } else {
    heap_caps_free(g->texte);
  }
  g->texte = copie;
  g->n = n;
}

bool _relire(const char* nom, String& contenu) {
  VerrouGardes v;
  Garde* g = _gardes ? _trouver(nom) : nullptr;
  if (!g) return false;
  contenu.concat(g->texte, (unsigned)g->n);
  return true;
}

constexpr const char* PARTITION = Stockage::PARTITION;
constexpr const char* BASE      = Stockage::BASE;
constexpr size_t      TAILLE_MAX = 8192;       // un .nms tient tres largement dedans

bool _monte = false;

// Le dossier d'un lieu ; "" si c'est la composition et qu'aucune n'est ouverte.
String _dossier(Lieu lieu) {
  return lieu == Lieu::Interface ? String(DOSSIER_INTERFACE) : Repertoire::dossierOuvert();
}

}  // namespace

// Pas de traversee de chemin : on ne garde que le nom de base.
String chemin(Lieu lieu, const char* nom) {
  const String d = _dossier(lieu);
  if (!d.length() || !nom) return String();
  const char* base = strrchr(nom, '/');
  return d + "/" + (base ? base + 1 : nom);
}

namespace {
String _chemin(Lieu lieu, const char* nom) { return chemin(lieu, nom); }
}  // namespace

bool estMonte() { return _monte; }

/* Les dossiers se creent a la premiere ecriture (Differe, au silence) : monter
 * n'ecrit rien. */
bool monter() {
  if (_monte) return true;
  // storage est PARTAGEE avec les echantillons : si elle est deja montee,
  // LittleFS.begin le voit et n'y touche pas.
  if (!LittleFS.begin(true, BASE, 10, PARTITION)) {
    Serial.println("[scripts] montage de storage impossible");
    return false;
  }
  _monte = true;
  return true;
}

/* LA LISTE DIT CE QUE LA CARTE PORTE, pas ce que la flash a deja recu : un
 * script envoye attend le silence pour s'ecrire (§157) — il existe pourtant,
 * une cue peut l'appeler. Donc : la flash, corrigee de ce qui attend. */
namespace {
struct Liste { String* out; String* vus; bool* premier; };
void _ajouter(Liste& l, const String& nom, size_t octets) {
  if (!*l.premier) *l.out += ",";
  *l.premier = false;
  *l.out += "{\"name\":\"" + nom + "\",\"bytes\":" + String((unsigned)octets) + "}";
  *l.vus += "|" + nom + "|";
}
}  // namespace

String listerJson(Lieu lieu) {
  const String dossier = _dossier(lieu);
  if (!monter() || !dossier.length()) return "[]";
  String out = "[", vus;
  bool premier = true;
  Liste l{ &out, &vus, &premier };
  /* From the index (§241): this walk froze the web server 1.1 s at each connection. */
  struct CtxIndex { Liste* l; String* vus; Lieu lieu; };
  CtxIndex ci{ &l, &vus, lieu };
  StorageIndex::visit(dossier.c_str(), false, [](const char* path, size_t size, bool dir, void* c) {
    if (dir) return;
    CtxIndex& x = *(CtxIndex*)c;
    const char* slash = strrchr(path, '/');
    const String n = slash ? slash + 1 : path;
    if (!n.endsWith(".nms")) return;               // la composition porte aussi ses cues, sa source…
    std::shared_ptr<char> t; size_t na = 0; bool sup = false;
    if (Differe::attente(_chemin(x.lieu, n.c_str()).c_str(), t, na, sup)) {
      if (!sup) _ajouter(*x.l, n, na);
      else *x.vus += "|" + n + "|";
      return;
    }
    _ajouter(*x.l, n, size);
  }, &ci);
  // Ce qui attend et que la flash n'a pas encore : les .nms de CE dossier.
  struct Ctx { Liste* l; size_t lp; };
  Ctx ctx{ &l, dossier.length() + 1 };
  Differe::visiterAttente((dossier + "/").c_str(),
    [](const char* chemin, size_t n, bool supprime, void* c) {
      Ctx& x = *(Ctx*)c;
      const String nom = chemin + x.lp;
      if (supprime || nom.indexOf('/') >= 0 || !nom.endsWith(".nms")
          || x.l->vus->indexOf("|" + nom + "|") >= 0) return;
      _ajouter(*x.l, nom, n);
    }, &ctx);
  out += "]";
  return out;
}

/* THE BENCH WALK (MESURES §240-241): what a walk of storage costs, by Arduino's File, as
 * the measure the board panel made used to — each entry reopened by its full path, 25 to
 * 45 ms. What storage holds and occupies now comes from StorageIndex, walked once. */
namespace {
struct Mesure { size_t fichiers = 0, scripts = 0, contenu = 0, utilises = 0, total = 0; };

/* A TOUTE PROFONDEUR : le repertoire range une composition trois niveaux sous
 * la racine (compositions/<nn>/<nom>/, §186). Borne a six niveaux — l'arbre
 * n'en a pas plus, et une recursion sans borne sur une pile de tache non. */
/* The bench's trace (§240): every step said on the console before it is taken. */
struct Trace { String* out; int n = 0; int max = 0; bool stop = false; };

bool step(Trace* t, const char* what, const String& path) {
  if (!t) return true;
  if (t->stop || ++t->n > t->max) { t->stop = true; return false; }
  NIDMI_WEB_LOG("[walk] %d %s %s", t->n, what, path.c_str());
  return true;
}
void stepDone(Trace* t, const char* what, const String& path, uint32_t t0) {
  if (!t) return;
  if (t->out->length() > 1) *t->out += ",";
  *t->out += "{\"n\":" + String(t->n) + ",\"step\":\"" + what + "\",\"path\":\"" + path
           + "\",\"us\":" + String((unsigned long)(micros() - t0)) + "}";
}

void parcourirPour(Mesure& m, const String& dossier, int profondeur, Trace* t) {
  if (!step(t, "open", dossier)) return;
  uint32_t t0 = micros();
  File d = LittleFS.open(dossier);
  stepDone(t, "open", dossier, t0);
  if (!d || !d.isDirectory()) return;
  for (;;) {
    if (!step(t, "next", dossier)) return;
    t0 = micros();
    File f = d.openNextFile();
    if (!f) return;
    const String p = f.path();
    stepDone(t, "next", p, t0);
    if (f.isDirectory()) { if (profondeur < 6) parcourirPour(m, p, profondeur + 1, t); continue; }
    m.fichiers++; m.contenu += f.size();
    if (p.endsWith(".nms")) m.scripts++;
  }
}

bool mesurer(Mesure& m, Trace* t) {
  m = Mesure();
  if (t && t->max < 0) {               // withInfo: the block count first, as the measure did
    t->max = -t->max;
    if (!step(t, "info", "/")) return false;
    const uint32_t t0 = micros();
    m.total    = LittleFS.totalBytes();
    m.utilises = LittleFS.usedBytes();
    stepDone(t, "info", "/", t0);
  }
  parcourirPour(m, "/", 0, t);
  return !(t && t->stop);
}
}  // namespace

String walkTrace(int maxSteps, bool withInfo) {
  if (!monter()) return "{\"error\":\"storage not mounted\"}";
  String out = "[";
  Trace t;
  t.out = &out;
  t.max = withInfo ? -maxSteps : maxSteps;
  Mesure m;
  const uint32_t t0 = millis();
  const bool complete = mesurer(m, &t);
  out += "]";
  NIDMI_WEB_LOG("[walk] end: %d steps, %s, %lu ms", t.n, complete ? "complete" : "stopped",
                (unsigned long)(millis() - t0));
  return "{\"complete\":" + String(complete ? "true" : "false") + ",\"ms\":"
       + String((unsigned long)(millis() - t0)) + ",\"steps\":" + out + "}";
}

bool existe(Lieu lieu, const char* nom) {
  if (!nom || !*nom || !monter()) return false;
  const String c = _chemin(lieu, nom);
  if (!c.length()) return false;
  std::shared_ptr<char> t; size_t n = 0; bool sup = false;
  if (Differe::attente(c.c_str(), t, n, sup)) return !sup;
  size_t taille;
  return StorageIndex::ready() ? StorageIndex::fileSize(c.c_str(), taille) : LittleFS.exists(c);
}

/* Recu tout de suite, ecrit en flash au premier silence (§157) : l'ecriture
 * qui efface arrete l'audio. lire() rend deja le nouveau contenu. */
bool ecrire(Lieu lieu, const char* nom, const String& contenu) {
  if (!nom || !*nom || !monter()) return false;
  if (contenu.length() > TAILLE_MAX) {
    Serial.printf("[scripts] %s refuse : %u o > %u\n",
                  nom, (unsigned)contenu.length(), (unsigned)TAILLE_MAX);
    return false;
  }
  if (lieu == Lieu::Composition) {
    String raison;
    if (!Repertoire::assurerOuverte("", raison)) {
      Serial.printf("[scripts] %s refuse : %s\n", nom, raison.c_str());
      return false;
    }
  }
  const String c = _chemin(lieu, nom);
  if (!c.length() || !Differe::poserFichierCopie(c.c_str(), contenu.c_str(), contenu.length()))
    return false;
  _garder(c.c_str(), contenu.c_str(), contenu.length());
  Serial.printf("[scripts] %s recu (%u o, flash au premier silence)\n", nom, (unsigned)contenu.length());
  return true;
}

bool supprimer(Lieu lieu, const char* nom) {
  if (!nom || !*nom || !monter() || !existe(lieu, nom)) return false;
  const String c = _chemin(lieu, nom);
  _oublier(c.c_str());
  return Differe::supprimerFichier(c.c_str());
}

void oublierSous(const char* prefixe) {
  const size_t lp = strlen(prefixe);
  VerrouGardes v;
  if (!_gardes) return;
  for (int i = 0; i < _nGardes; ) {
    if (strncmp(_gardes[i].nom, prefixe, lp)) { i++; continue; }
    heap_caps_free(_gardes[i].texte);
    _gardes[i] = _gardes[--_nGardes];          // la derniere prend sa place, et se relit
  }
}

bool supprimerChemin(const String& c) {
  if (!c.endsWith(".nms") || !monter()) return false;
  _oublier(c.c_str());
  return Differe::supprimerFichier(c.c_str());
}

bool lire(Lieu lieu, const char* nom, String& contenu) {
  contenu = "";
  if (!nom || !*nom || !monter()) return false;
  const String chemin = _chemin(lieu, nom);
  if (!chemin.length()) return false;
  {
    std::shared_ptr<char> t; size_t n = 0; bool sup = false;
    if (Differe::attente(chemin.c_str(), t, n, sup)) {
      if (sup) return false;
      if (t && n) contenu.concat(t.get(), (unsigned)n);
      return true;
    }
  }
  if (_relire(chemin.c_str(), contenu)) return true;     // deja lu : la PSRAM (§172)
  File f = LittleFS.open(chemin, FILE_READ);
  if (!f) return false;
  // D'UN BLOC : un .nms est petit (8 Ko au plus).
  const size_t n = f.size();
  char* tampon = (char*)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tampon) { f.close(); return false; }
  const size_t lu = f.read((uint8_t*)tampon, n);
  f.close();
  contenu.concat(tampon, (unsigned)lu);
  if (lu == n) _garder(chemin.c_str(), tampon, n);
  heap_caps_free(tampon);
  return lu == n;
}

}  // namespace ScriptStore
