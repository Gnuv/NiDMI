#include "CarteSd.h"

#include <SD.h>
#include <SPI.h>
#include <driver/gpio.h>
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
volatile bool     _aCablage  = false;
volatile uint32_t _actif     = 0;          // la tache vit : une seule a la fois
volatile uint32_t _dernierEssaiMs = 0;
volatile uint32_t _essais    = 0;

/* LE DIAGNOSTIC, ecrit par la tache, lu par le serveur web (diagnostic()). Des
 * champs simples : une lecture un peu perimee ne casse rien. */
char          _raison[112] = "";           // pourquoi le dernier montage a echoue
volatile int  _cmd0 = -1;                  // R1 de CMD0 (0x01 : la carte repond), -1 : pas sonde
volatile int  _cmd8 = -1;                  // R1 de CMD8
volatile uint32_t _cmd8Echo = 0;           // les 4 octets suivants (0x000001AA : SD v2)
volatile int  _misoBas = -1, _misoHaut = -1;   // niveau de MISO au repos, tire vers le bas / le haut
volatile int  _invCmd0 = -1;               // R1 de CMD0 avec MISO et MOSI echanges
volatile int  _bbCmd0 = -1, _bbCmd8 = -1, _bbInvCmd0 = -1;   // les memes, sans peripherique SPI
volatile bool _cablageFait = false;
/* LA TRACE BRUTE de l'initialisation a la main : les 12 octets lus apres chaque
 * commande, en hexadecimal. `[0]` CMD0, `[1]` CMD8, `[2]` CMD58, `[3]` CMD55,
 * `[4]` ACMD41 (le premier tour), `[5]` ACMD41 (le dernier). */
char          _brut[6][40];
volatile int  _acmd41Tours = -1;
volatile int  _acmd41R1 = -1;
/* Les quatre broches de la carte, mesurees seules (CS, SCK, MISO, MOSI) :
 * lu avec un tirage bas, avec un tirage haut, puis lu en retour apres l'avoir
 * PILOTEE a 0 et a 1. Une broche qui ne suit pas ce qu'elle pilote est en
 * court-circuit (vers GND ou 3V3) ou fortement chargee. -1 : pas mesure. */
volatile int8_t _niv[4][4] = {{-1,-1,-1,-1},{-1,-1,-1,-1},{-1,-1,-1,-1},{-1,-1,-1,-1}};

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

/* UNE COMMANDE SD A LA MAIN : 0xFF d'amorce, six octets, puis jusqu'a dix octets
 * d'attente de la reponse R1 (le bit 7 retombe). `tr` echange un octet — le SPI
 * materiel, ou la sonde « a la main » (bit-bang). */
template <typename T> uint8_t _commandeT(T tr, uint8_t cmd, uint32_t arg, uint8_t crc) {
  tr(0xFF);
  tr(0x40 | cmd);
  tr((uint8_t)(arg >> 24)); tr((uint8_t)(arg >> 16));
  tr((uint8_t)(arg >> 8));  tr((uint8_t)arg);
  tr(crc);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && (r & 0x80); i++) r = tr(0xFF);
  return r;
}

/* SONDER, a 400 kHz ou moins, comme le fait l'initialisation d'une carte : 80
 * impulsions CS haut, CMD0 (retour au repos), CMD8 (SD v2). Ce que la
 * bibliotheque SD sait mais ne dit pas : jusqu'ou la conversation va. */
template <typename T> void _sondeT(T tr, int& r0, int& r8, uint32_t& echo) {
  r0 = r8 = -1; echo = 0;
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  for (int i = 0; i < 10; i++) tr(0xFF);
  digitalWrite(_cs, LOW);
  r0 = _commandeT(tr, 0, 0, 0x95);
  digitalWrite(_cs, HIGH); tr(0xFF);
  digitalWrite(_cs, LOW);
  const uint8_t v8 = _commandeT(tr, 8, 0x1AA, 0x87);
  if (!(v8 & 0x80) && !(v8 & 0x04)) for (int i = 0; i < 4; i++) echo = (echo << 8) | tr(0xFF);
  digitalWrite(_cs, HIGH); tr(0xFF);
  r8 = v8;
}

void _sonderSur(SPIClass& b, int& r0, int& r8, uint32_t& echo) {
  b.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  _sondeT([&](uint8_t o) { return b.transfer(o); }, r0, r8, echo);
  b.endTransaction();
}

/* LE MEME ECHANGE SANS PERIPHERIQUE SPI : les broches battent a la main, mode 0
 * (donnee posee, front montant de l'horloge, lecture de MISO). Si la carte
 * repond ainsi et pas par le SPI materiel, le defaut est dans notre usage du SPI ;
 * si elle ne repond pas non plus, il est dans le cablage. `sckEn`/`misoEn`/
 * `mosiEn` : les broches reelles de chaque role (permutables). */
void _sonderALaMain(uint8_t sckEn, uint8_t misoEn, uint8_t mosiEn, int& r0, int& r8, uint32_t& echo) {
  pinMode(sckEn, OUTPUT);  digitalWrite(sckEn, LOW);
  pinMode(mosiEn, OUTPUT); digitalWrite(mosiEn, HIGH);
  pinMode(misoEn, INPUT_PULLUP);
  auto tr = [&](uint8_t o) -> uint8_t {
    uint8_t r = 0;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(mosiEn, (o >> i) & 1);
      delayMicroseconds(3);
      digitalWrite(sckEn, HIGH);
      delayMicroseconds(3);
      r = (uint8_t)((r << 1) | (digitalRead(misoEn) ? 1 : 0));
      digitalWrite(sckEn, LOW);
    }
    return r;
  };
  _sondeT(tr, r0, r8, echo);
  pinMode(sckEn, INPUT); pinMode(mosiEn, INPUT); pinMode(misoEn, INPUT);
}

/* Une commande, puis les 12 octets qui suivent, gardes bruts. Rend le premier R1
 * valide (bit 7 a 0), 0xFF si aucun. */
int _brutCmd(uint8_t cmd, uint32_t arg, uint8_t crc, char* hex) {
  SPI.transfer(0xFF);
  SPI.transfer(0x40 | cmd);
  SPI.transfer((uint8_t)(arg >> 24)); SPI.transfer((uint8_t)(arg >> 16));
  SPI.transfer((uint8_t)(arg >> 8));  SPI.transfer((uint8_t)arg);
  SPI.transfer(crc);
  int r1 = 0xFF;
  for (int i = 0; i < 12; i++) {
    const uint8_t o = SPI.transfer(0xFF);
    if (hex) snprintf(hex + i * 3, 4, "%02X ", (unsigned)o);
    if (r1 == 0xFF && !(o & 0x80)) r1 = o;
  }
  return r1;
}

/* L'INITIALISATION A LA MAIN, jusqu'ou la carte va : CMD0, CMD8, CMD58 (OCR),
 * puis CMD55 + ACMD41 en boucle (HCS, 1 s au plus) — ce que fait la bibliotheque,
 * en gardant ce que la carte dit au lieu d'un simple echec. Carte non montee. */
void _sonderBrut() {
  for (auto& h : _brut) h[0] = 0;
  _acmd41Tours = 0; _acmd41R1 = -1;
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 20; i++) SPI.transfer(0xFF);
  digitalWrite(_cs, LOW);
  _brutCmd(0, 0, 0x95, _brut[0]);
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
  _brutCmd(8, 0x1AA, 0x87, _brut[1]);
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
  _brutCmd(58, 0, 0x01, _brut[2]);
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
  const uint32_t t0 = millis();
  int r = 0xFF;
  do {
    digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
    _brutCmd(55, 0, 0x01, _acmd41Tours == 0 ? _brut[3] : nullptr);
    digitalWrite(_cs, HIGH); SPI.transfer(0xFF); digitalWrite(_cs, LOW);
    r = _brutCmd(41, 0x40000000, 0x01, _acmd41Tours == 0 ? _brut[4] : _brut[5]);
    _acmd41Tours = _acmd41Tours + 1;
    delay(10);
  } while (r == 0x01 && millis() - t0 < 1000);
  _acmd41R1 = r;
  digitalWrite(_cs, HIGH); SPI.transfer(0xFF);
  SPI.endTransaction();
  NIDMI_WEB_LOG("[SD] init a la main : ACMD41 -> 0x%02X apres %d tour(s) ; CMD8 brut : %s",
                (unsigned)_acmd41R1, (int)_acmd41Tours, _brut[1]);
}

void _sonder() {
  int r0, r8; uint32_t echo;
  _sonderSur(SPI, r0, r8, echo);
  _cmd0 = r0; _cmd8 = r8; _cmd8Echo = echo;
  NIDMI_WEB_LOG("[SD] sonde du bus : CMD0 -> 0x%02X, CMD8 -> 0x%02X (0x%08lX)",
                (unsigned)_cmd0, (unsigned)_cmd8, (unsigned long)echo);
}

/* LA SONDE DE CABLAGE, sur demande, carte non montee. Trois questions que CMD0 a
 * 0xFF ne tranche pas :
 *   - MISO : au repos, avec un tirage vers le bas puis vers le haut. S'il suit le
 *     tirage, rien ne le tient — fil non branche, ou module hors tension sans
 *     tirage propre ; s'il reste haut malgre le tirage vers le bas, quelque chose
 *     le tient (module sous tension).
 *   - MISO et MOSI INVERSES : un second bus (HSPI), les deux fils de donnees
 *     echanges — la confusion classique entre DI/DO et MOSI/MISO. Une carte qui
 *     repond ainsi est branchee a l'envers.
 * Le bus principal est ferme le temps de la sonde, puis rouvert. */
void _mesurerBroche(uint8_t pin, volatile int8_t* r) {
  pinMode(pin, INPUT_PULLDOWN); delay(3); r[0] = digitalRead(pin);
  pinMode(pin, INPUT_PULLUP);   delay(3); r[1] = digitalRead(pin);
  pinMode(pin, OUTPUT);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);   // se relire en sortie
  digitalWrite(pin, LOW);  delay(3); r[2] = digitalRead(pin);
  digitalWrite(pin, HIGH); delay(3); r[3] = digitalRead(pin);
  pinMode(pin, INPUT);
}

void _sonderCablage() {
  SPI.end();
  pinMode(_miso, INPUT_PULLDOWN); delay(3); _misoBas  = digitalRead(_miso);
  pinMode(_miso, INPUT_PULLUP);   delay(3); _misoHaut = digitalRead(_miso);
  pinMode(_miso, INPUT);
  _mesurerBroche(_cs, _niv[0]);
  pinMode(_cs, OUTPUT); digitalWrite(_cs, HIGH);     // carte deselectionnee pendant les autres mesures
  _mesurerBroche(_sck, _niv[1]);
  _mesurerBroche(_miso, _niv[2]); _mesurerBroche(_mosi, _niv[3]);
  {
    SPIClass inv(HSPI);
    inv.begin(_sck, _mosi, _miso, -1);            // la broche MOSI lit, la broche MISO ecrit
    int r0, r8; uint32_t echo;
    _sonderSur(inv, r0, r8, echo);
    _invCmd0 = r0;
    inv.end();
  }
  {
    int r0, r8; uint32_t echo;
    _sonderALaMain(_sck, _miso, _mosi, r0, r8, echo);      // a la main, sans SPI
    _bbCmd0 = r0; _bbCmd8 = r8;
    _sonderALaMain(_sck, _mosi, _miso, r0, r8, echo);      // idem, MISO/MOSI echanges
    _bbInvCmd0 = r0;
  }
  SPI.begin(_sck, _miso, _mosi, -1);
  NIDMI_WEB_LOG("[SD] cablage : MISO bas -> %d, haut -> %d ; SPI MISO/MOSI inverses CMD0 -> 0x%02X ; a la main CMD0 -> 0x%02X, inverses -> 0x%02X",
                _misoBas, _misoHaut, (unsigned)_invCmd0, (unsigned)_bbCmd0, (unsigned)_bbInvCmd0);
  _cablageFait = true;
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
    if (_cmd0 == 0x01) _sonderBrut();                    // elle repond : jusqu'ou va-t-elle ?
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
    if (_aCablage)  { _aCablage  = false; if (_declaree && !_monte) _sonderCablage(); continue; }
    if (_aCharger)  { _aCharger  = false; _chargerSons(); continue; }
    break;
  }
  __sync_lock_release(&_actif);
  // Un drapeau leve entre le dernier tour et la liberation : on repart.
  if (_aDemonter || _aMonter || _aCharger || _aMesurer || _aCablage) _lancer();
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

void sonderCablage() {
  if (!_declaree || _monte) return;
  _aCablage = true;
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
  if (_acmd41Tours >= 0) {
    j += ",\"init_a_la_main\":{\"cmd0\":\"" + String(_brut[0]) + "\",\"cmd8\":\"" + String(_brut[1])
       + "\",\"cmd58\":\"" + String(_brut[2]) + "\",\"cmd55\":\"" + String(_brut[3])
       + "\",\"acmd41_premier\":\"" + String(_brut[4]) + "\",\"acmd41_dernier\":\"" + String(_brut[5])
       + "\",\"acmd41_tours\":" + String((int)_acmd41Tours) + ",\"acmd41_r1\":" + String((int)_acmd41R1) + "}";
  }
  if (_cablageFait) {
    // Une broche qui ne suit pas ce qu'elle pilote : court-circuit ou charge.
    const char* nomBroche[4] = {"CS", "SCK", "MISO", "MOSI"};
    String suspecte;
    for (int b = 0; b < 4; b++)
      if (_niv[b][2] == 1 || _niv[b][3] == 0) suspecte += String(suspecte.length() ? ", " : "") + nomBroche[b];
    static char tampon[160];
    const char* verdict =
        suspecte.length()
            ? (snprintf(tampon, sizeof(tampon), "%s ne suit pas ce que la carte pilote : court-circuit vers GND/3V3, ou fil sur une mauvaise broche", suspecte.c_str()), tampon)
        :
        _bbCmd0 >= 0 && _bbCmd0 != 0xFF && (_cmd0 < 0 || _cmd0 == 0xFF)
            ? "la carte repond A LA MAIN mais pas par le SPI materiel : le defaut est dans notre usage du SPI"
        : _bbInvCmd0 >= 0 && _bbInvCmd0 != 0xFF
            ? "la carte repond MISO et MOSI echanges : les deux fils de donnees sont inverses"
        : _invCmd0 >= 0 && _invCmd0 != 0xFF
            ? "la carte repond en SPI MISO et MOSI echanges : les deux fils de donnees sont inverses"
        : _misoBas == 1
            ? "silence meme a la main : MISO est tenu haut (module sous tension) mais la carte n'entend rien — SCK, MOSI ou CS n'arrivent pas, ou carte non alimentee/mal enfoncee"
            : "silence : MISO flotte (suit le tirage) — fil MISO non branche, ou module hors tension";
    j += ",\"cablage\":{\"miso_tire_bas\":" + String(_misoBas) + ",\"miso_tire_haut\":" + String(_misoHaut)
       + ",\"cmd0_miso_mosi_inverses\":" + String(_invCmd0)
       + ",\"a_la_main_cmd0\":" + String(_bbCmd0) + ",\"a_la_main_cmd8\":" + String(_bbCmd8)
       + ",\"a_la_main_inverses_cmd0\":" + String(_bbInvCmd0)
       + ",\"broches\":{" + [&]() {
           String t;
           for (int b = 0; b < 4; b++)
             t += String(b ? "," : "") + "\"" + nomBroche[b] + "\":{\"tirage_bas\":" + String(_niv[b][0])
                + ",\"tirage_haut\":" + String(_niv[b][1]) + ",\"pilote_0_lu\":" + String(_niv[b][2])
                + ",\"pilote_1_lu\":" + String(_niv[b][3]) + "}";
           return t; }() + "}"
       + ",\"verdict\":\"" + String(verdict) + "\"}";
  }
  j += ",\"mesure\":";
  if (_mesEtat == 0)      j += "null";
  else if (_mesEtat == 1) j += "{\"etat\":\"en cours\"}";
  else                    j += String(_mesJson);
  j += "}";
  return j;
}

}  // namespace CarteSd
