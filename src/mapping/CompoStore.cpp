#include "CompoStore.h"
#include "../audio/AudioEngine.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Compo {
namespace {

constexpr const char* PARTITION  = "mapfs";   // meme partition que cues.txt et les echantillons
constexpr const char* BASE       = "/mapfs";
constexpr const char* FICHIER    = "/compo.json";
constexpr const char* TEMPORAIRE = "/compo.tmp";
constexpr unsigned long STABLE_MS  = 3000;    // une rafale d'envois n'ecrit qu'une fois
constexpr unsigned long REESSAI_MS = 30000;   // apres un echec d'ecriture

/* Le verrou ne garde que des ECHANGES de pointeurs — jamais l'ecriture en
 * flash, qui travaille sur sa propre copie du shared_ptr : le serveur web
 * (async_tcp) n'attend donc jamais la flash. */
SemaphoreHandle_t verrou = nullptr;
std::shared_ptr<char> actuelle;
size_t octetsActuels = 0;
bool aEcrire = false;
unsigned long recueA = 0, echecA = 0;
uint32_t ecritures = 0, echecs = 0, pireUs = 0, derniereUs = 0;

std::shared_ptr<char> tamponPsram(size_t n) {
  char* p = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) return nullptr;
  return std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); });
}

bool monter() {
  // mapfs est partagee : deja montee, LittleFS.begin le voit et n'y touche pas.
  return LittleFS.begin(true, BASE, 10, PARTITION);
}

// Ecrire a cote, puis renommer : une coupure au milieu laisse l'ancienne intacte.
bool ecrireFichier(const char* data, size_t n) {
  if (!monter()) return false;
  File f = LittleFS.open(TEMPORAIRE, FILE_WRITE);
  if (!f) return false;
  const size_t ecrit = f.write((const uint8_t*)data, n);
  f.close();
  if (ecrit != n) { LittleFS.remove(TEMPORAIRE); return false; }
  return LittleFS.rename(TEMPORAIRE, FICHIER);
}

}  // namespace

void demarrer() {
  if (!verrou) verrou = xSemaphoreCreateMutex();
  if (!monter() || !LittleFS.exists(FICHIER)) return;
  File f = LittleFS.open(FICHIER, FILE_READ);
  if (!f) return;
  const size_t n = f.size();
  if (n > 0 && n <= MAX_OCTETS) {
    auto t = tamponPsram(n);
    if (t && f.read((uint8_t*)t.get(), n) == n) {
      xSemaphoreTake(verrou, portMAX_DELAY);
      actuelle = t;
      octetsActuels = n;
      xSemaphoreGive(verrou);
    }
  }
  f.close();
}

std::shared_ptr<char> courante(size_t& octets) {
  if (!verrou) { octets = 0; return nullptr; }
  xSemaphoreTake(verrou, portMAX_DELAY);
  auto t = actuelle;
  octets = octetsActuels;
  xSemaphoreGive(verrou);
  return t;
}

void adopter(std::shared_ptr<char> tampon, size_t octets) {
  if (!verrou) return;
  xSemaphoreTake(verrou, portMAX_DELAY);
  actuelle = tampon;          // l'ancienne est rendue quand son dernier lecteur la lache
  octetsActuels = octets;
  aEcrire = true;
  recueA = millis();
  xSemaphoreGive(verrou);
}

void boucle() {
  if (!aEcrire) return;
  const unsigned long now = millis();
  if (now - recueA < STABLE_MS) return;
  if (echecA && now - echecA < REESSAI_MS) return;
  if (!AudioEngine::silencePourLaFlash()) return;

  xSemaphoreTake(verrou, portMAX_DELAY);
  auto t = actuelle;
  const size_t n = octetsActuels;
  aEcrire = false;
  xSemaphoreGive(verrou);
  if (!t || !n) return;

  AudioEngine::ecritureFlashDebut();
  const uint32_t t0 = micros();
  const bool ok = ecrireFichier(t.get(), n);
  const uint32_t dt = micros() - t0;
  AudioEngine::ecritureFlashFin();

  derniereUs = dt;
  if (dt > pireUs) pireUs = dt;
  if (ok) {
    ecritures++;
    echecA = 0;
  } else {
    echecs++;
    echecA = now ? now : 1;
    // Une autre a pu arriver pendant l'ecriture : elle garde son drapeau.
    xSemaphoreTake(verrou, portMAX_DELAY);
    aEcrire = true;
    xSemaphoreGive(verrou);
  }
}

bool enAttente() { return aEcrire; }

String etatJson() {
  size_t n = 0;
  courante(n);
  String j = "{\"octets\":" + String((unsigned)n);
  j += ",\"en_attente\":";
  j += aEcrire ? "true" : "false";
  j += ",\"ecritures\":" + String(ecritures) + ",\"echecs\":" + String(echecs);
  j += ",\"pire_us\":" + String(pireUs) + ",\"derniere_us\":" + String(derniereUs) + "}";
  return j;
}

}  // namespace Compo
