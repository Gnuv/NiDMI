#include "Instrument.h"
#include "EcrituresDifferees.h"
#include "../mapping/Repertoire.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Instrument {
namespace {

constexpr const char* NVS_ESPACE = "nidmi";
constexpr const char* NVS_CLE    = "instrument";

/* Lu par le serveur web, ecrit par lui : un verrou court quand meme, le nom se
 * rend en copie. */
StaticSemaphore_t _tampon;
SemaphoreHandle_t _verrou = xSemaphoreCreateMutexStatic(&_tampon);
struct Verrou {
  Verrou()  { xSemaphoreTake(_verrou, portMAX_DELAY); }
  ~Verrou() { xSemaphoreGive(_verrou); }
};
String _nom;

}  // namespace

void demarrer() {
  Preferences p;
  String n;
  if (p.begin(NVS_ESPACE, true)) { n = p.getString(NVS_CLE, ""); p.end(); }
  Verrou v;
  _nom = n;
}

String nom() { Verrou v; return _nom; }

bool renommer(const String& n, String& raison) {
  if (n.length() && !Repertoire::nomValide(n, raison)) return false;
  {
    Verrou v;
    if (_nom == n) return true;
    _nom = n;
  }
  if (n.length()) Differe::nvsChaine(NVS_ESPACE, NVS_CLE, n);   // au silence (§157)
  else            Differe::nvsRetirer(NVS_ESPACE, NVS_CLE);
  Repertoire::annoncer();          // les barres de titre des onglets se relisent
  return true;
}

}  // namespace Instrument
