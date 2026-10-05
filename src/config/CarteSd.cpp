#include "CarteSd.h"

#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../audio/AudioEngine.h"
#include "../audio/SampleStore.h"
#include "../server/WebDebugConsole.h"

namespace CarteSd {
namespace {

uint8_t _cs = 255, _sck = 255, _miso = 255, _mosi = 255;
volatile bool     _declaree = false;
volatile bool     _monte    = false;
volatile uint64_t _total    = 0;
uint32_t          _hz       = FREQUENCE_DEFAUT_HZ;

/* CE QU'IL RESTE A FAIRE, dit par ceux qui n'ont pas le droit d'attendre. La
 * tache lit ces drapeaux ; qui les leve (serveur web, restauration au demarrage)
 * repart aussitot. */
volatile bool     _aDemonter = false;
volatile bool     _aMonter   = false;
volatile bool     _aCharger  = false;
volatile bool     _aMesurer  = false;
volatile uint32_t _actif     = 0;          // la tache vit : une seule a la fois
volatile uint32_t _dernierEssaiMs = 0;
volatile uint32_t _essais    = 0;

/* LE DIAGNOSTIC, ecrit par la tache, lu par le serveur web (diagnostic()). Des
 * champs simples : une lecture un peu perimee ne casse rien. */
char          _raison[112] = "";           // pourquoi le dernier montage a echoue
volatile int  _cmd0 = -1;                  // R1 de CMD0 (0x01 : la carte repond), -1 : pas sonde
volatile int  _cmd8 = -1;                  // R1 de CMD8
volatile uint32_t _cmd8Echo = 0;           // les 4 octets suivants (0x000001AA : SD v2)

/* LA MESURE DE LECTURE. Etat : 0 jamais, 1 en cours, 2 finie. Le JSON se pose
 * d'un coup, l'etat en dernier. */
volatile uint8_t _mesEtat = 0;
char     _mesJson[1024] = "";
char     _mesNom[SampleStore::NOM_MAX] = "";
uint32_t _mesHz = 0;

/* Pile de la tache : le montage FAT, un tableau de 24 noms (1,1 ko), les String de
 * la lecture et une mesure. Transitoire — rendue a la mort de la tache. */
constexpr uint32_t PILE = 8192;

void _lancer();

void _demonter() {
  if (!_monte) return;
  _monte = false;
  _total = 0;
  SD.end();
  NIDMI_WEB_LOG("[SD] demontee");
}

/* UN OCTET SUR LE BUS. */
uint8_t _echanger(uint8_t o) { return SPI.transfer(o); }

/* Une commande SD a la main : 0xFF d'amorce, six octets, puis jusqu'a dix octets
 * d'attente de la reponse R1 (le bit 7 retombe). */
uint8_t _commande(uint8_t cmd, uint32_t arg, uint8_t crc) {
  _echanger(0xFF);
  _echanger(0x40 | cmd);
  _echanger((uint8_t)(arg >> 24)); _echanger((uint8_t)(arg >> 16));
  _echanger((uint8_t)(arg >> 8));  _echanger((uint8_t)arg);
  _echanger(crc);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && (r & 0x80); i++) r = _echanger(0xFF);
  return r;
}

/* SONDER LE BUS, a 400 kHz, comme le fait l'initialisation d'une carte : 80
 * impulsions CS haut, CMD0 (retour au repos), CMD8 (SD v2). Ce que la
 * bibliotheque SD sait mais ne dit pas : jusqu'ou la conversation va. */
void _sonder() {
  _cmd0 = _cmd8 = -1; _cmd8Echo = 0;
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) _echanger(0xFF);
  digitalWrite(_cs, LOW);
  _cmd0 = _commande(0, 0, 0x95);
  digitalWrite(_cs, HIGH); _echanger(0xFF);
  digitalWrite(_cs, LOW);
  const uint8_t r8 = _commande(8, 0x1AA, 0x87);
  uint32_t echo = 0;
  if (!(r8 & 0x80) && !(r8 & 0x04)) for (int i = 0; i < 4; i++) echo = (echo << 8) | _echanger(0xFF);
  digitalWrite(_cs, HIGH); _echanger(0xFF);
  SPI.endTransaction();
  _cmd8 = r8; _cmd8Echo = echo;
  NIDMI_WEB_LOG("[SD] sonde du bus : CMD0 -> 0x%02X, CMD8 -> 0x%02X (0x%08lX)",
                (unsigned)_cmd0, (unsigned)_cmd8, (unsigned long)echo);
}

void _monter() {
  if (!_declaree || _monte) return;
  _dernierEssaiMs = millis();
  _essais++;
  _raison[0] = 0;
  /* Le bus d'abord, avec ses broches, SANS CS materiel (-1) : c'est la
   * bibliotheque SD qui le tient (digitalWrite). Le LIS3DH en SPI partage ce bus
   * sur son propre CS — begin() est sans effet s'il a deja ete ouvert. */
  SPI.begin(_sck, _miso, _mosi, -1);
  if (!SD.begin(_cs, SPI, _hz)) {
    _sonder();
    const char* cause = _cmd0 == 0xFF || _cmd0 < 0
        ? "aucune reponse a CMD0 : carte absente, ou MISO/MOSI/SCK/CS/alimentation mal cables"
        : "la carte repond a CMD0 mais ne s'initialise pas : format FAT32 ? carte SDXC ? alimentation ?";
    snprintf(_raison, sizeof(_raison), "%s", cause);
    NIDMI_WEB_LOG("[SD] pas de carte (CS=%u SCK=%u MISO=%u MOSI=%u, %lu MHz) : %s",
                  (unsigned)_cs, (unsigned)_sck, (unsigned)_miso, (unsigned)_mosi,
                  (unsigned long)(_hz / 1000000UL), cause);
    return;
  }
  const sdcard_type_t type = SD.cardType();
  if (type == CARD_NONE) {
    SD.end();
    snprintf(_raison, sizeof(_raison), "aucune carte reconnue apres l'initialisation");
    NIDMI_WEB_LOG("[SD] aucune carte reconnue");
    return;
  }
  _total = SD.cardSize();
  _monte = true;
  NIDMI_WEB_LOG("[SD] montee : %s, %lu Mo, a %lu MHz",
                type == CARD_MMC ? "MMC" : type == CARD_SD ? "SD" : type == CARD_SDHC ? "SDHC" : "?",
                (unsigned long)(_total / (1024ULL * 1024ULL)),
                (unsigned long)(_hz / 1000000UL));
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
    else NIDMI_WEB_LOG("[SD] %s ignore : %s", noms[i], raison.c_str());
  }
  NIDMI_WEB_LOG("[SD] %u son(s) precharge(s) sur %u dans %s", (unsigned)bons, (unsigned)n, DOSSIER);
}

/* LA MESURE : lire un .wav de bout en bout, comme le ferait la lecture en flux —
 * par morceaux de 16 ko, dans un tampon en PSRAM, avec la meme cession du
 * processeur entre deux. On chronometre chaque lecture : la MOYENNE dit le debit,
 * le PIRE dit de quelle avance un tampon a besoin. Bornee (12 Mo ou 12 s). */
void _mesurer() {
  constexpr size_t   PAS = 16384;
  constexpr size_t   OCTETS_MAX = 12u * 1024u * 1024u;
  constexpr uint32_t DUREE_MAX_MS = 12000;
  auto fin = [](const char* erreur) {
    snprintf(_mesJson, sizeof(_mesJson), "{\"etat\":\"finie\",\"erreur\":\"%s\"}", erreur);
    __sync_synchronize();
    _mesEtat = 2;
  };
  if (_mesHz && _mesHz != _hz) {                    // une autre frequence : on remonte
    _demonter();
    _hz = _mesHz;
    _monter();
  }
  if (!_monte) return fin("carte non montee (voir le diagnostic)");
  const String chemin = String(DOSSIER) + "/" + _mesNom;
  File f = SD.open(chemin.c_str(), FILE_READ);
  if (!f || f.isDirectory()) return fin("fichier introuvable dans /samples");
  const size_t taille = f.size();
  uint16_t canaux = 0; uint32_t freq = 0, donnees = 0; String raison;
  const bool wav = SampleStore::enteteWav(f, canaux, freq, donnees, raison);
  if (!wav) { f.seek(0); donnees = taille; canaux = 0; freq = 0; }
  const size_t debut = f.position();
  uint8_t* tampon = (uint8_t*)heap_caps_malloc(PAS, MALLOC_CAP_SPIRAM);
  if (!tampon) { f.close(); return fin("PSRAM"); }

  uint32_t n = 0, sup10 = 0, sup30 = 0, sup100 = 0, max_us = 0;
  uint64_t somme_us = 0;
  size_t total = 0;
  const uint32_t t0 = millis();
  while (total < donnees && total < OCTETS_MAX && millis() - t0 < DUREE_MAX_MS) {
    const uint32_t a = micros();
    const size_t k = f.read(tampon, PAS);
    const uint32_t d = micros() - a;
    if (!k) break;
    total += k; n++; somme_us += d;
    if (d > max_us) max_us = d;
    if (d > 10000) sup10++;
    if (d > 30000) sup30++;
    if (d > 100000) sup100++;
    vTaskDelay(1);                                   // la cession de la vraie lecture
  }
  const uint32_t mur_ms = millis() - t0;
  uint32_t saut_us = 0;
  if (donnees > 2 * PAS) {                           // ouvrir au milieu : le depart d'un clip
    const uint32_t a = micros();
    f.seek(debut + donnees / 2);
    f.read(tampon, PAS);
    saut_us = micros() - a;
  }
  f.close();
  heap_caps_free(tampon);

  const double pur  = somme_us ? (double)total * 1e6 / (double)somme_us / 1024.0 : 0.0;
  const double reel = mur_ms ? (double)total * 1000.0 / (double)mur_ms / 1024.0 : 0.0;
  const double besoin = (wav && freq) ? (double)freq * canaux * 2 / 1024.0 : 0.0;
  snprintf(_mesJson, sizeof(_mesJson),
           "{\"etat\":\"finie\",\"fichier\":\"%s\",\"octets\":%u,\"wav\":%s,\"hz_bus\":%lu,\"freq\":%lu,\"canaux\":%u,"
           "\"lu_octets\":%u,\"morceau\":%u,\"lectures\":%lu,\"moy_us\":%lu,\"max_us\":%lu,"
           "\"sup_10ms\":%lu,\"sup_30ms\":%lu,\"sup_100ms\":%lu,\"saut_milieu_us\":%lu,"
           "\"debit_pur_ko_s\":%.0f,\"debit_avec_cession_ko_s\":%.0f,\"besoin_ko_s\":%.0f,"
           "\"flux_possibles\":%.1f}",
           _mesNom, (unsigned)taille, wav ? "true" : "false", (unsigned long)_hz, (unsigned long)freq,
           (unsigned)canaux, (unsigned)total, (unsigned)PAS, (unsigned long)n,
           (unsigned long)(n ? somme_us / n : 0), (unsigned long)max_us,
           (unsigned long)sup10, (unsigned long)sup30, (unsigned long)sup100, (unsigned long)saut_us,
           pur, reel, besoin, besoin > 0 ? reel / besoin : 0.0);
  __sync_synchronize();
  _mesEtat = 2;
  NIDMI_WEB_LOG("[SD] mesure %s : %.0f Ko/s (%.0f avec cession), pire lecture %lu us, saut %lu us",
                _mesNom, pur, reel, (unsigned long)max_us, (unsigned long)saut_us);
}

void _tache(void*) {
  for (;;) {
    if (_aDemonter) { _aDemonter = false; _demonter();  continue; }
    if (_aMonter)   { _aMonter   = false; _monter();    continue; }
    if (_aMesurer)  { _aMesurer  = false; _mesurer();   continue; }
    if (_aCharger)  { _aCharger  = false; _chargerSons(); continue; }
    break;
  }
  __sync_lock_release(&_actif);
  // Un drapeau leve entre le dernier tour et la liberation : on repart.
  if (_aDemonter || _aMonter || _aCharger || _aMesurer) _lancer();
  vTaskDelete(nullptr);
}

void _lancer() {
  if (__sync_lock_test_and_set(&_actif, 1)) return;      // elle tourne : elle verra les drapeaux
  /* Coeur 0, priorite 1 : sous le WiFi, la pile TCP/IP, l'audio (coeur 1) et la
   * boucle — elle ne tourne que quand rien d'autre n'a besoin du processeur. */
  if (xTaskCreatePinnedToCore(_tache, "carte-sd", PILE, nullptr, 1, nullptr, 0) != pdPASS) {
    __sync_lock_release(&_actif);
    NIDMI_WEB_LOG("[SD] tache impossible (memoire) — la carte ne sera pas montee");
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

void essayer() {
  if (!_declaree || _monte) return;
  _aMonter = true;
  _lancer();
}

void chargerSons() {
  if (!_monte) return;
  _aCharger = true;
  _lancer();
}

bool mesurer(const char* nom, uint32_t hz) {
  if (!_declaree || !nom || !*nom || strlen(nom) >= sizeof(_mesNom) || _mesEtat == 1) return false;
  strlcpy(_mesNom, nom, sizeof(_mesNom));
  _mesHz = hz;
  _mesEtat = 1;
  _aMesurer = true;
  _lancer();
  return true;
}

File ouvrir(const char* chemin) {
  if (!_monte) return File();
  return SD.open(chemin, FILE_READ);
}

String diagnostic() {
  String j = "{\"declaree\":" + String(_declaree ? "true" : "false")
           + ",\"monte\":" + String(_monte ? "true" : "false")
           + ",\"broches\":{\"cs\":" + String(_cs) + ",\"sck\":" + String(_sck)
           + ",\"miso\":" + String(_miso) + ",\"mosi\":" + String(_mosi) + "}"
           + ",\"hz\":" + String((unsigned long)_hz)
           + ",\"essais\":" + String((unsigned long)_essais)
           + ",\"tache\":" + String(_actif ? "true" : "false")
           + ",\"total\":" + String((unsigned long)(_total / 1024ULL)) + "";   // en Ko
  if (_essais) j += ",\"dernier_essai_il_y_a_ms\":" + String((unsigned long)(millis() - _dernierEssaiMs));
  if (_raison[0]) j += ",\"raison\":\"" + String(_raison) + "\"";
  if (_cmd0 >= 0)
    j += ",\"sonde\":{\"cmd0\":" + String(_cmd0) + ",\"cmd8\":" + String(_cmd8)
       + ",\"cmd8_echo\":" + String((unsigned long)_cmd8Echo) + "}";
  j += ",\"mesure\":";
  if (_mesEtat == 0)      j += "null";
  else if (_mesEtat == 1) j += "{\"etat\":\"en cours\"}";
  else                    j += String(_mesJson);
  j += "}";
  return j;
}

}  // namespace CarteSd
