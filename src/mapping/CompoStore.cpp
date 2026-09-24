#include "CompoStore.h"
#include "../config/EcrituresDifferees.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Compo {
namespace {

constexpr const char* PARTITION = "mapfs";   // meme partition que cues.txt et les echantillons
constexpr const char* BASE      = "/mapfs";
constexpr const char* FICHIER   = "/compo.json";

/* La composition la plus recente, rendue par GET. L'ecriture en flash, elle,
 * passe par Differe (au silence) — qui partage ce meme tampon, sans copie. */
SemaphoreHandle_t verrou = nullptr;
std::shared_ptr<char> actuelle;
size_t octetsActuels = 0;

std::shared_ptr<char> tamponPsram(size_t n) {
  char* p = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) return nullptr;
  return std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); });
}

}  // namespace

void demarrer() {
  if (!verrou) verrou = xSemaphoreCreateMutex();
  // mapfs est partagee : deja montee, LittleFS.begin le voit et n'y touche pas.
  if (!LittleFS.begin(true, BASE, 10, PARTITION) || !LittleFS.exists(FICHIER)) return;
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

bool adopter(std::shared_ptr<char> tampon, size_t octets) {
  if (!verrou) return false;
  if (!Differe::poserFichier(FICHIER, tampon, octets)) return false;
  xSemaphoreTake(verrou, portMAX_DELAY);
  actuelle = tampon;          // l'ancienne est rendue quand son dernier lecteur la lache
  octetsActuels = octets;
  xSemaphoreGive(verrou);
  return true;
}

String etatJson() {
  size_t n = 0;
  courante(n);
  std::shared_ptr<char> t;
  size_t na = 0;
  bool supprime = false;
  const bool attend = Differe::attente(FICHIER, t, na, supprime);
  String j = "{\"octets\":" + String((unsigned)n);
  j += ",\"en_attente\":";
  j += attend ? "true" : "false";
  j += "}";
  return j;
}

}  // namespace Compo
