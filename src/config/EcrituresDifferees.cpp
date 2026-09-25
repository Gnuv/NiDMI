#include "EcrituresDifferees.h"
#include <atomic>
#include "../audio/AudioEngine.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Differe {
namespace {

constexpr const char* PARTITION = "mapfs";
constexpr const char* BASE      = "/mapfs";
constexpr int MAX_FICHIERS = 24;
constexpr int MAX_NVS      = 24;
constexpr unsigned long STABLE_MS  = 3000;    // une rafale de poses n'ecrit qu'une fois
constexpr unsigned long REESSAI_MS = 30000;   // apres un echec d'ecriture

struct Fichier {
  bool actif = false;
  char chemin[64] = {0};
  std::shared_ptr<char> tampon;              // nullptr + supprime : effacer
  size_t n = 0;
  bool supprime = false;
  unsigned long poseA = 0, echecA = 0;
};

enum class TypeNvs : uint8_t { Chaine, Octet, Retirer };
struct Nvs {
  bool actif = false;
  char espace[16] = {0};
  char cle[16] = {0};
  TypeNvs type = TypeNvs::Chaine;
  String chaine;
  uint8_t octet = 0;
  unsigned long poseA = 0, echecA = 0;
};

Fichier fichiers[MAX_FICHIERS];
Nvs     nvs[MAX_NVS];
uint32_t ecritures = 0, identiques = 0, echecs = 0, refus = 0, pireUs = 0;

/* Le verrou ne garde que des echanges — jamais une ecriture en flash, qui
 * travaille sur sa propre copie : un poseur (async_tcp) n'attend jamais la
 * flash. */
SemaphoreHandle_t leVerrou() {
  static SemaphoreHandle_t v = xSemaphoreCreateMutex();
  return v;
}
struct Garde {
  Garde()  { xSemaphoreTake(leVerrou(), portMAX_DELAY); }
  ~Garde() { xSemaphoreGive(leVerrou()); }
};
/* Et un second verrou pour l'ECRITURE elle-meme : la boucle et la tache de
 * redemarrage peuvent vider la file en meme temps — deux ecritures du meme
 * fichier temporaire ne doivent pas se croiser. */
SemaphoreHandle_t leVerrouEcriture() {
  static SemaphoreHandle_t v = xSemaphoreCreateMutex();
  return v;
}
struct GardeEcriture {
  GardeEcriture()  { xSemaphoreTake(leVerrouEcriture(), portMAX_DELAY); }
  ~GardeEcriture() { xSemaphoreGive(leVerrouEcriture()); }
};

bool monter() {
  // mapfs est partagee : deja montee, LittleFS.begin le voit et n'y touche pas.
  return LittleFS.begin(true, BASE, 10, PARTITION);
}

std::atomic<uint32_t> g_generationFichiers{0};

// A cote, puis renomme : une coupure au milieu laisse l'ancien intact.
bool ecrireFichier(const char* chemin, const char* data, size_t n) {
  if (!monter()) return false;
  const String tmp = String(chemin) + ".tmp";
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) return false;
  const size_t e = f.write((const uint8_t*)data, n);
  f.close();
  g_generationFichiers++;
  if (e != n) { LittleFS.remove(tmp); return false; }
  return LittleFS.rename(tmp, chemin);
}

bool effacerFichier(const char* chemin) {
  if (!monter()) return false;
  g_generationFichiers++;
  return !LittleFS.exists(chemin) || LittleFS.remove(chemin);
}

bool ecrireNvs(const Nvs& e, bool* identique = nullptr) {
  Preferences p;
  if (!p.begin(e.espace, false)) return false;
  bool ok = true;
  if (identique) *identique = false;
  // Une valeur identique ne s'ecrit pas : relire coute une lecture, reecrire
  // peut couter un effacement (l'app redimensionne la chaine a chaque suivi).
  switch (e.type) {
    case TypeNvs::Chaine:
      if (!p.isKey(e.cle) || p.getString(e.cle, "") != e.chaine)
        ok = p.putString(e.cle, e.chaine) == e.chaine.length();
      else if (identique) *identique = true;
      break;
    case TypeNvs::Octet:
      if (!p.isKey(e.cle) || p.getUChar(e.cle, 0) != e.octet)
        ok = p.putUChar(e.cle, e.octet) == 1;
      else if (identique) *identique = true;
      break;
    case TypeNvs::Retirer:
      if (p.isKey(e.cle)) ok = p.remove(e.cle);
      else if (identique) *identique = true;
      break;
  }
  p.end();
  return ok;
}

void noter(uint32_t dt, bool ok) {
  if (dt > pireUs) pireUs = dt;
  if (ok) ecritures++; else echecs++;
}

// Ecrire un fichier pris dans la file ; sans silence exige si `force`.
bool traiterFichier(int i, unsigned long now, bool force) {
  std::shared_ptr<char> t;
  size_t n = 0;
  bool supprime = false;
  char chemin[64];
  {
    Garde g;
    Fichier& f = fichiers[i];
    if (!f.actif) return false;
    if (!force && (now - f.poseA < STABLE_MS || (f.echecA && now - f.echecA < REESSAI_MS)))
      return false;
    t = f.tampon; n = f.n; supprime = f.supprime;
    strlcpy(chemin, f.chemin, sizeof chemin);
  }
  bool ok;
  uint32_t dt;
  {
    GardeEcriture e;
    AudioEngine::ecritureFlashDebut();
    const uint32_t t0 = micros();
    ok = supprime ? effacerFichier(chemin) : ecrireFichier(chemin, t.get(), n);
    dt = micros() - t0;
    AudioEngine::ecritureFlashFin();
  }
  noter(dt, ok);
  Garde g;
  Fichier& f = fichiers[i];
  // Une pose plus recente a pu arriver pendant l'ecriture : elle garde sa place.
  if (f.actif && f.tampon == t && f.supprime == supprime && !strcmp(f.chemin, chemin)) {
    if (ok) { f.actif = false; f.tampon.reset(); f.n = 0; }
    else    f.echecA = now ? now : 1;
  }
  return true;
}

bool traiterNvs(int i, unsigned long now, bool force) {
  Nvs e;
  {
    Garde g;
    Nvs& v = nvs[i];
    if (!v.actif) return false;
    if (!force && (now - v.poseA < STABLE_MS || (v.echecA && now - v.echecA < REESSAI_MS)))
      return false;
    e = v;
  }
  bool ok, identique = false;
  uint32_t dt;
  {
    GardeEcriture ge;
    AudioEngine::ecritureFlashDebut();
    const uint32_t t0 = micros();
    ok = ecrireNvs(e, &identique);
    dt = micros() - t0;
    AudioEngine::ecritureFlashFin();
  }
  if (identique) identiques++;       // rien d'ecrit : pas une ecriture
  else noter(dt, ok);
  Garde g;
  Nvs& v = nvs[i];
  if (v.actif && v.poseA == e.poseA && !strcmp(v.espace, e.espace) && !strcmp(v.cle, e.cle)) {
    if (ok) { v.actif = false; v.chaine = String(); }
    else    v.echecA = now ? now : 1;
  }
  return true;
}

void poserNvs(const char* espace, const char* cle, TypeNvs type, const String* chaine, uint8_t octet) {
  if (!espace || !cle || strlen(espace) >= 16 || strlen(cle) >= 16) return;
  Nvs* cible = nullptr;
  {
    Garde g;
    Nvs* libre = nullptr;
    for (auto& v : nvs) {
      if (v.actif && !strcmp(v.espace, espace) && !strcmp(v.cle, cle)) { cible = &v; break; }
      if (!v.actif && !libre) libre = &v;
    }
    if (!cible) cible = libre;
    if (cible) {
      cible->actif = true;
      strlcpy(cible->espace, espace, sizeof cible->espace);
      strlcpy(cible->cle, cle, sizeof cible->cle);
      cible->type = type;
      cible->chaine = chaine ? *chaine : String();
      cible->octet = octet;
      cible->poseA = millis();
      cible->echecA = 0;
      return;
    }
    refus++;
  }
  // File pleine : ne rien perdre — ecrire tout de suite, comme avant.
  Nvs e;
  strlcpy(e.espace, espace, sizeof e.espace);
  strlcpy(e.cle, cle, sizeof e.cle);
  e.type = type; e.chaine = chaine ? *chaine : String(); e.octet = octet;
  GardeEcriture ge;
  AudioEngine::ecritureFlashDebut();
  noter(0, ecrireNvs(e));
  AudioEngine::ecritureFlashFin();
}

}  // namespace

bool poserFichier(const char* chemin, std::shared_ptr<char> tampon, size_t n) {
  if (!chemin || strlen(chemin) >= sizeof(Fichier::chemin) || !tampon) return false;
  Garde g;
  Fichier* libre = nullptr;
  for (auto& f : fichiers) {
    if (f.actif && !strcmp(f.chemin, chemin)) { libre = &f; break; }
    if (!f.actif && !libre) libre = &f;
  }
  if (!libre) { refus++; return false; }
  libre->actif = true;
  strlcpy(libre->chemin, chemin, sizeof libre->chemin);
  libre->tampon = tampon;
  libre->n = n;
  libre->supprime = false;
  libre->poseA = millis();
  libre->echecA = 0;
  return true;
}

bool poserFichierCopie(const char* chemin, const char* data, size_t n) {
  char* p = (char*)heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) return false;
  if (n) memcpy(p, data, n);
  return poserFichier(chemin, std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); }), n);
}

bool supprimerFichier(const char* chemin) {
  if (!chemin || strlen(chemin) >= sizeof(Fichier::chemin)) return false;
  Garde g;
  Fichier* libre = nullptr;
  for (auto& f : fichiers) {
    if (f.actif && !strcmp(f.chemin, chemin)) { libre = &f; break; }
    if (!f.actif && !libre) libre = &f;
  }
  if (!libre) { refus++; return false; }
  libre->actif = true;
  strlcpy(libre->chemin, chemin, sizeof libre->chemin);
  libre->tampon.reset();
  libre->n = 0;
  libre->supprime = true;
  libre->poseA = millis();
  libre->echecA = 0;
  return true;
}

bool attente(const char* chemin, std::shared_ptr<char>& tampon, size_t& n, bool& supprime) {
  if (!chemin) return false;
  Garde g;
  for (auto& f : fichiers) {
    if (f.actif && !strcmp(f.chemin, chemin)) {
      tampon = f.tampon; n = f.n; supprime = f.supprime;
      return true;
    }
  }
  return false;
}

void visiterAttente(const char* prefixe,
                    void (*visiter)(const char* chemin, size_t n, bool supprime, void* ctx),
                    void* ctx) {
  struct Vu { char chemin[64]; size_t n; bool supprime; };
  Vu vus[MAX_FICHIERS];
  int k = 0;
  const size_t lp = prefixe ? strlen(prefixe) : 0;
  {
    Garde g;
    for (auto& f : fichiers) {
      if (!f.actif || (lp && strncmp(f.chemin, prefixe, lp))) continue;
      strlcpy(vus[k].chemin, f.chemin, sizeof vus[k].chemin);
      vus[k].n = f.n; vus[k].supprime = f.supprime;
      k++;
    }
  }
  for (int i = 0; i < k; i++) visiter(vus[i].chemin, vus[i].n, vus[i].supprime, ctx);
}

void nvsChaine(const char* espace, const char* cle, const String& valeur) {
  poserNvs(espace, cle, TypeNvs::Chaine, &valeur, 0);
}
void nvsOctet(const char* espace, const char* cle, uint8_t valeur) {
  poserNvs(espace, cle, TypeNvs::Octet, nullptr, valeur);
}
void nvsRetirer(const char* espace, const char* cle) {
  poserNvs(espace, cle, TypeNvs::Retirer, nullptr, 0);
}

void boucle() {
  if (!enAttente()) return;
  if (!AudioEngine::silencePourLaFlash()) return;
  const unsigned long now = millis();
  // Une ecriture par tour : le silence se reverifie avant la suivante — une
  // note peut arriver entre deux.
  for (int i = 0; i < MAX_FICHIERS; i++) if (traiterFichier(i, now, false)) return;
  for (int i = 0; i < MAX_NVS; i++)      if (traiterNvs(i, now, false)) return;
}

void toutEcrireMaintenant() {
  const unsigned long now = millis();
  for (int i = 0; i < MAX_FICHIERS; i++) traiterFichier(i, now, true);
  for (int i = 0; i < MAX_NVS; i++)      traiterNvs(i, now, true);
}

void noterFichiersModifies() { g_generationFichiers++; }
uint32_t generationFichiers() { return g_generationFichiers.load(); }

bool enAttente() {
  for (auto& f : fichiers) if (f.actif) return true;
  for (auto& v : nvs)      if (v.actif) return true;
  return false;
}

String etatJson() {
  int nf = 0, nn = 0;
  {
    Garde g;
    for (auto& f : fichiers) if (f.actif) nf++;
    for (auto& v : nvs)      if (v.actif) nn++;
  }
  String j = "{\"fichiers_en_attente\":" + String(nf);
  j += ",\"nvs_en_attente\":" + String(nn);
  j += ",\"ecritures\":" + String(ecritures) + ",\"identiques\":" + String(identiques);
  j += ",\"echecs\":" + String(echecs);
  j += ",\"refus\":" + String(refus) + ",\"pire_us\":" + String(pireUs) + "}";
  return j;
}

}  // namespace Differe
