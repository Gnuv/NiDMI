#include "CarteSd.h"

#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../audio/AudioEngine.h"
#include "../audio/SampleStore.h"

namespace CarteSd {
namespace {

uint8_t _cs = 255, _sck = 255, _miso = 255, _mosi = 255;
volatile bool     _declaree = false;
volatile bool     _monte    = false;
volatile uint64_t _total    = 0;

/* CE QU'IL RESTE A FAIRE, dit par ceux qui n'ont pas le droit d'attendre. La
 * tache lit ces drapeaux ; qui les leve (serveur web, restauration au demarrage)
 * repart aussitot. */
volatile bool     _aDemonter = false;
volatile bool     _aMonter   = false;
volatile bool     _aCharger  = false;
volatile uint32_t _actif     = 0;          // la tache vit : une seule a la fois
volatile uint32_t _dernierEssaiMs = 0;

/* Pile de la tache : le montage FAT, un tableau de 24 noms (1,1 ko) et les
 * String de la lecture. Transitoire — rendue a la mort de la tache. */
constexpr uint32_t PILE = 8192;

void _lancer();

void _demonter() {
  if (!_monte) return;
  _monte = false;
  _total = 0;
  SD.end();
  Serial.println("[SD] demontee");
}

void _monter() {
  if (!_declaree || _monte) return;
  _dernierEssaiMs = millis();
  /* Le bus d'abord, avec ses broches, SANS CS materiel (-1) : c'est la
   * bibliotheque SD qui le tient (digitalWrite). Le LIS3DH en SPI partage ce bus
   * sur son propre CS — begin() est sans effet s'il a deja ete ouvert. */
  SPI.begin(_sck, _miso, _mosi, -1);
  if (!SD.begin(_cs, SPI, FREQUENCE_HZ)) {
    Serial.printf("[SD] pas de carte (CS=%u SCK=%u MISO=%u MOSI=%u) — branchee, a 3V3, "
                  "formatee en FAT32 ? Le panneau FICHIERS retente a chaque ouverture.\n",
                  (unsigned)_cs, (unsigned)_sck, (unsigned)_miso, (unsigned)_mosi);
    return;
  }
  const sdcard_type_t type = SD.cardType();
  if (type == CARD_NONE) { SD.end(); Serial.println("[SD] aucune carte reconnue"); return; }
  _total = SD.cardSize();
  _monte = true;
  Serial.printf("[SD] montee : %s, %lu Mo, a %lu MHz\n",
                type == CARD_MMC ? "MMC" : type == CARD_SD ? "SD" : type == CARD_SDHC ? "SDHC" : "?",
                (unsigned long)(_total / (1024ULL * 1024ULL)),
                (unsigned long)(FREQUENCE_HZ / 1000000UL));
  _aCharger = true;
}

/* Les sons de la carte, un par un : chacun passe par echantillonArrive(), le meme
 * chemin qu'un son televerse — publie dans le magasin, et les clips qui le
 * nomment le retrouvent. Rien si le magasin n'a jamais ete charge : c'est
 * chargerTout() qui nous rappellera, quand un moteur de sons en aura besoin. */
void _chargerSons() {
  if (!_monte || !SampleStore::charge()) return;
  char noms[SAMPLES_MAX][SampleStore::NOM_MAX];
  const uint8_t n = SampleStore::sonsDeLaCarteSd(noms, SAMPLES_MAX);
  uint8_t bons = 0;
  for (uint8_t i = 0; i < n && _monte; i++) {
    String raison;
    if (AudioEngine::echantillonArrive(noms[i], raison, true)) bons++;
    else Serial.printf("[SD] %s ignore : %s\n", noms[i], raison.c_str());
  }
  Serial.printf("[SD] %u son(s) lu(s) sur %u dans %s\n", (unsigned)bons, (unsigned)n, DOSSIER);
}

void _tache(void*) {
  for (;;) {
    if (_aDemonter) { _aDemonter = false; _demonter();  continue; }
    if (_aMonter)   { _aMonter   = false; _monter();    continue; }
    if (_aCharger)  { _aCharger  = false; _chargerSons(); continue; }
    break;
  }
  __sync_lock_release(&_actif);
  // Un drapeau leve entre le dernier tour et la liberation : on repart.
  if (_aDemonter || _aMonter || _aCharger) _lancer();
  vTaskDelete(nullptr);
}

void _lancer() {
  if (__sync_lock_test_and_set(&_actif, 1)) return;      // elle tourne : elle verra les drapeaux
  /* Coeur 0, priorite 1 : sous le WiFi, la pile TCP/IP, l'audio (coeur 1) et la
   * boucle — elle ne tourne que quand rien d'autre n'a besoin du processeur. */
  if (xTaskCreatePinnedToCore(_tache, "carte-sd", PILE, nullptr, 1, nullptr, 0) != pdPASS) {
    __sync_lock_release(&_actif);
    Serial.println("[SD] tache impossible (memoire) — la carte ne sera pas montee");
  }
}

}  // namespace

void declarer(uint8_t cs, uint8_t sck, uint8_t miso, uint8_t mosi) {
  const bool memes = _declaree && cs == _cs && sck == _sck && miso == _miso && mosi == _mosi;
  if (memes) {
    if (!_monte) { _aMonter = true; _lancer(); }
    return;
  }
  const bool changement = _declaree;                     // d'autres broches : on repart de zero
  _cs = cs; _sck = sck; _miso = miso; _mosi = mosi;
  _declaree = true;
  if (changement) _aDemonter = true;
  _aMonter = true;
  _lancer();
}

void retirer() {
  _declaree = false;
  _cs = _sck = _miso = _mosi = 255;
  _aMonter = false;
  _aDemonter = true;
  _lancer();
}

bool     declaree() { return _declaree; }
bool     monte()    { return _monte; }
uint64_t total()    { return _total; }

void reessayer() {
  if (!_declaree || _monte || _actif) return;
  if (millis() - _dernierEssaiMs < 5000) return;
  _aMonter = true;
  _lancer();
}

void chargerSons() {
  if (!_monte) return;
  _aCharger = true;
  _lancer();
}

File ouvrir(const char* chemin) {
  if (!_monte) return File();
  return SD.open(chemin, FILE_READ);
}

}  // namespace CarteSd
