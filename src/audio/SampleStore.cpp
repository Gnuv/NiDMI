#include "SampleStore.h"
#include "../config/Stockage.h"
#include "../config/EcrituresDifferees.h"
#include "../config/SdCard.h"
#include "SdStream.h"
#include "SdClusters.h"
#include "../server/WebDebugConsole.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace SampleStore {
namespace {

constexpr const char* PARTITION = Stockage::PARTITION;
constexpr const char* BASE      = Stockage::BASE;
/* Le televersement s'ecrit A COTE, et ne prend son nom qu'une fois verifie
 * (§177) : ouvrir le fichier definitif le tronquait d'emblee — un remplacement
 * interrompu ou refuse emportait l'ancien son, et un fichier a moitie ecrit
 * etait liste pendant tout l'envoi. Hors de /samples : ni liste, ni charge. */
constexpr const char* PROVISOIRE = "/televersement.part";
/* Un televersement sans nouvelle depuis ce delai est mort sans prevenir : le
 * suivant le remplace plutot que d'etre refuse pour toujours. */
constexpr uint32_t    ABANDON_MS = 30000;

bool   _monte = false;

/* LE TELEVERSEMENT EN COURS, et la requete qui le tient (SampleStore.h). Tout
 * ceci n'est touche que par le serveur web (async_tcp) : une seule tache. */
File        _enCours;
String      _nomEnCours;
const void* _proprietaire = nullptr;
uint32_t    _dernierMorceauMs = 0;
bool        _echecEcriture = false;
const void* _refuse = nullptr;          // la derniere requete refusee a l'ouverture...
String      _raisonRefus;               // ... et pourquoi : sa reponse le dira

/* N ECHANTILLONS, tous en PSRAM, tous permanents. Les descripteurs vivent en
 * RAM interne — 24 x ~72 o, soit ~1,7 ko de .bss, et pas un octet du bloc
 * contigu qui decide du service web. Le PCM, lui, est entierement en PSRAM.
 *
 * TROIS ETATS (§177) : vide (pcm nul) ; LISIBLE (`pret`) ; RETIRE (plus
 * `pret`, mais le PCM encore la : la tache audio a pu le lire au bloc en
 * cours). Seul un emplacement vide se remplit. */
struct Echantillon {
  int16_t* pcm    = nullptr;
  size_t   trames = 0;
  size_t   octets = 0;
  bool     stereo = false;
  uint32_t freq   = 48000;
  char     nom[NOM_MAX] = {0};
  bool     sd     = false;      // read from the SD card (SdCard), not from storage
  bool     streamed = false;    // read as a STREAM (SdStream): no data in PSRAM, pcm is null
  uint32_t dataOffset = 0;      // streamed: the byte where the data begins in the file
  volatile bool pret = false;
};
Echantillon      _ech[SAMPLES_MAX];
volatile uint8_t _prets  = 0;
size_t           _octets = 0;
bool             _charge = false;      // chargerTout() est passe

/* LES ECRIVAINS du magasin — chargement, televersement, suppression — passent
 * l'un apres l'autre. Les LECTEURS (la tache audio, un declenchement) ne
 * prennent rien : un emplacement ne devient lisible qu'une fois rempli
 * (`pret` pose en dernier), et ne se vide que deux blocs audio apres avoir
 * cesse de l'etre. Cree a l'initialisation statique, comme le verrou des
 * scripts : creer un mutex ne demande pas l'ordonnanceur. */
StaticSemaphore_t _tamponVerrou;
SemaphoreHandle_t _verrou = xSemaphoreCreateRecursiveMutexStatic(&_tamponVerrou);
struct Verrou {
  Verrou()  { if (_verrou) xSemaphoreTakeRecursive(_verrou, portMAX_DELAY); }
  ~Verrou() { if (_verrou) xSemaphoreGiveRecursive(_verrou); }
};

// Pas de traversée : on ne garde que le nom de base. storage est un panier plat.
const char* _base(const char* nom) {
  const char* b = strrchr(nom, '/');
  return b ? b + 1 : nom;
}
String _chemin(const char* nom) { return String(DOSSIER) + "/" + _base(nom); }
/* A .wav to read: not a hidden file. macOS puts next to every file copied to a FAT
 * card a "._name.wav" of metadata (4 KB) — never a sound. */
bool _isWav(const char* nom) {
  const size_t n = strlen(nom);
  return n > 4 && nom[0] != '.' && !strcasecmp(nom + n - 4, ".wav");
}

uint32_t _le32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
uint16_t _le16(const uint8_t* p) { return p[0] | (p[1]<<8); }

int _index(const char* base) {
  for (int i = 0; i < SAMPLES_MAX; i++)
    if (_ech[i].pret && !strcmp(_ech[i].nom, base)) return i;
  return -1;
}
int _vide() {
  for (int i = 0; i < SAMPLES_MAX; i++) if (!_ech[i].pret && !_ech[i].pcm) return i;
  return -1;
}
// Sous le verrou. Tous les champs d'abord, `pret` en dernier : qui le voit voit le reste.
void _publier(int i, const Echantillon& e) {
  Echantillon& d = _ech[i];
  d.pcm = e.pcm; d.trames = e.trames; d.octets = e.octets;
  d.stereo = e.stereo; d.freq = e.freq; d.sd = e.sd; d.streamed = e.streamed; d.dataOffset = e.dataOffset;
  memcpy(d.nom, e.nom, sizeof(d.nom));
  __sync_synchronize();
  d.pret = true;
  _prets = _prets + 1;
  _octets += e.octets;
}
// Sous le verrou. Plus trouve par son nom, plus lu ; le PCM reste jusqu'a liberer().
void _retirer(int i) {
  _ech[i].pret = false;
  __sync_synchronize();
  _prets = _prets - 1;
}

/* L'EN-TETE : parcours des chunks jusqu'a « data » ; `f` reste au debut des
 * donnees. Le lecteur ne sait jouer que du PCM 16 bits, mono ou stereo. */
bool _lireEntete(File& f, uint16_t& canaux, uint32_t& freq, uint32_t& tailleData,
                 String& raison) {
  uint8_t e[12];
  if (f.read(e, 12) != 12 || memcmp(e, "RIFF", 4) || memcmp(e + 8, "WAVE", 4)) {
    raison = "pas un WAV"; return false;
  }
  uint16_t bits = 0;
  canaux = 0; freq = 0; tailleData = 0;
  while (f.available() >= 8) {
    uint8_t en[8];
    if (f.read(en, 8) != 8) break;
    const uint32_t taille = _le32(en + 4);
    if (!memcmp(en, "fmt ", 4)) {
      uint8_t fmt[16];
      if (f.read(fmt, 16) != 16) break;
      canaux = _le16(fmt + 2); freq = _le32(fmt + 4); bits = _le16(fmt + 14);
      if (taille > 16) f.seek(f.position() + (taille - 16));
    } else if (!memcmp(en, "data", 4)) {
      tailleData = taille;
      break;
    } else {
      f.seek(f.position() + taille + (taille & 1));
    }
  }
  if (bits != 16 || (canaux != 1 && canaux != 2) || !tailleData) {
    raison = "PCM 16 bits mono ou stereo requis (recu " + String(bits) + " bits, "
             + String(canaux) + " canaux)";
    return false;
  }
  if ((size_t)f.available() < tailleData) {
    raison = "fichier tronque : l'en-tete annonce " + String(tailleData)
             + " o de donnees, il en porte " + String((unsigned)f.available());
    return false;
  }
  return true;
}

/* Reads ONE file into `dest`, which nobody can see yet. `fromSdCard`: the sound is
 * on the SD card (SdCard) and not in storage — it is then read in chunks, yielding
 * the CPU between two: a multi-MB sound takes seconds over SPI, and the task that
 * reads it must not starve the IDLE task (watchdog) nor anything else. */
bool _lire(const char* nom, Echantillon& dest, String& raison, bool fromSdCard = false) {
  if (strlen(nom) >= NOM_MAX) {
    raison = "nom trop long (" + String(NOM_MAX - 1) + " caracteres au plus)"; return false;
  }
  File f = fromSdCard ? SdCard::open((String(SdCard::FOLDER) + "/" + _base(nom)).c_str())
                      : LittleFS.open(_chemin(nom), FILE_READ);
  if (!f) { raison = "fichier introuvable"; return false; }
  uint16_t canaux; uint32_t freq, tailleData;
  if (!_lireEntete(f, canaux, freq, tailleData, raison)) { f.close(); return false; }

  /* AN SD SOUND THAT DOES NOT FIT IN PSRAM is played as a STREAM: we only keep its
   * header (its length, its frequency, where the data begins), no data. PSRAM is
   * shared — web server buffers, playlist banks, console —: the SD must not empty
   * it. Bigger than PRELOAD_SOUND_MAX, or beyond PRELOAD_SD_MAX in total. Only a
   * playlist can play it (SdStream.h). */
  if (fromSdCard) {
    size_t preloaded = 0;
    for (int j = 0; j < SAMPLES_MAX; j++) if (_ech[j].pret && _ech[j].sd && !_ech[j].streamed) preloaded += _ech[j].octets;
    if (tailleData > PRELOAD_SOUND_MAX || preloaded + tailleData > PRELOAD_SD_MAX) {
      dest.pcm = nullptr; dest.octets = 0; dest.streamed = true; dest.sd = true;
      dest.dataOffset = (uint32_t)f.position();
      dest.stereo = (canaux == 2);
      dest.freq   = freq ? freq : 48000;
      dest.trames = tailleData / (2 * canaux);
      strlcpy(dest.nom, nom, sizeof(dest.nom));
      f.close();
      SdClusters::build(dest.nom);          // its cluster chain, walked once: the streams then skip FatFs's seek
      NIDMI_WEB_LOG("[samples] %s : %u Ko, %u trames a %u Hz, %s — lu EN FLUX depuis la carte SD",
                    dest.nom, (unsigned)(tailleData / 1024), (unsigned)dest.trames, (unsigned)dest.freq,
                    dest.stereo ? "stereo" : "mono");
      return true;
    }
  }

  // PSRAM : 8,25 Mo libres pendant que le tas interne se bat pour 14 ko.
  int16_t* pcm = (int16_t*)heap_caps_malloc(tailleData, MALLOC_CAP_SPIRAM);
  if (!pcm) {
    raison = "PSRAM insuffisante pour " + String(tailleData) + " o";
    f.close(); return false;
  }
  size_t lus = 0;
  if (!fromSdCard) {
    lus = f.read((uint8_t*)pcm, tailleData);
  } else {
    constexpr size_t CHUNK = 16384;
    while (lus < tailleData) {
      const size_t step = (tailleData - lus < CHUNK) ? tailleData - lus : CHUNK;
      const size_t n = f.read((uint8_t*)pcm + lus, step);
      if (!n) break;
      lus += n;
      vTaskDelay(1);                   // yield: core 0's IDLE task must run
    }
  }
  f.close();
  if (lus != tailleData) { heap_caps_free(pcm); raison = "lecture incomplete"; return false; }

  dest.pcm    = pcm;
  dest.octets = tailleData;
  dest.sd     = fromSdCard;
  dest.stereo = (canaux == 2);
  dest.freq   = freq ? freq : 48000;
  dest.trames = tailleData / (2 * canaux);
  strlcpy(dest.nom, nom, sizeof(dest.nom));
  NIDMI_WEB_LOG("[samples] %s charge%s : %u trames, %u Hz, %s, %u o en PSRAM",
                dest.nom, fromSdCard ? " (carte SD)" : "", (unsigned)dest.trames, (unsigned)dest.freq,
                dest.stereo ? "stereo" : "mono", (unsigned)tailleData);
  return true;
}

}  // namespace

bool estMonte() { return _monte; }

bool wavHeader(File& f, uint16_t& canaux, uint32_t& freq, uint32_t& octetsData, String& raison) {
  return _lireEntete(f, canaux, freq, octetsData, raison);
}

bool monter() {
  if (_monte) return true;
  // formatOnFail : la partition n'a jamais servi, elle est vierge.
  if (!LittleFS.begin(true, BASE, 10, PARTITION)) {
    Serial.println("[samples] montage de storage impossible");
    return false;
  }
  if (!LittleFS.exists(DOSSIER)) LittleFS.mkdir(DOSSIER);
  // Un televersement coupe par un redemarrage : il n'a jamais ete un son.
  if (LittleFS.exists(PROVISOIRE)) LittleFS.remove(PROVISOIRE);
  _monte = true;
  Serial.printf("[samples] storage monte — %u o utilises sur %u\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  return true;
}

size_t espaceTotal()   { return _monte ? LittleFS.totalBytes() : 0; }
size_t espaceUtilise() { return _monte ? LittleFS.usedBytes()  : 0; }

String listerJson() {
  if (!monter()) return "[]";
  String out = "[";
  File d = LittleFS.open(DOSSIER);
  if (d && d.isDirectory()) {
    File f = d.openNextFile();
    bool premier = true;
    while (f) {
      if (!f.isDirectory()) {
        if (!premier) out += ",";
        premier = false;
        const char* base = _base(f.path());
        out += "{\"name\":\"" + String(base) + "\",\"bytes\":" + String(f.size());
        /* SA DUREE, quand il est charge (MESURES §196) : l'editeur de clips en
         * fait l'invite de la fin d'une selection. */
        const int i = _index(base);
        if (i >= 0 && _ech[i].freq)
          out += ",\"ms\":" + String((uint32_t)((uint64_t)_ech[i].trames * 1000ULL / _ech[i].freq));
        out += "}";
      }
      f = d.openNextFile();
    }
  }
  /* The SD card's sounds, after them — "volume" says where they come from. A name
   * that storage already holds wins, and is not listed twice. */
  File s = SdCard::openForWeb(SdCard::FOLDER);        // the web server's walk: never on a card in doubt
  if (s && s.isDirectory()) {
    bool isFirst = out.length() == 1;
    for (File f = s.openNextFile(); f; f = s.openNextFile()) {
      if (f.isDirectory()) continue;
      const char* base = _base(f.path());
      if (!_isWav(base) || LittleFS.exists(_chemin(base))) continue;
      if (!isFirst) out += ",";
      isFirst = false;
      out += "{\"name\":\"" + String(base) + "\",\"bytes\":" + String(f.size())
           + ",\"volume\":\"" + SdCard::VOLUME_ID + "\"";
      { const int fi = _index(base);
        if (fi >= 0 && _ech[fi].streamed) {
          /* What this sound's head weighs in the heads budget: the app adds them up per cue. */
          const uint32_t frames = (_ech[fi].trames < SdStream::HEAD_FRAMES) ? (uint32_t)_ech[fi].trames : SdStream::HEAD_FRAMES;
          out += ",\"streamed\":true,\"head_bytes\":" + String((unsigned)(frames * (_ech[fi].stereo ? 2u : 1u) * 2u));
        } else if (fi < 0) {
          const char* why = refusalOf(base);                 // not loaded: say why, when we know
          if (why && *why) {
            String esc;
            for (const char* c = why; *c; c++) { if (*c == '"' || *c == '\\') esc += '\\'; if ((uint8_t)*c >= 0x20) esc += *c; }
            out += ",\"refused\":\"" + esc + "\"";
          }
        }
      }
      const int i = _index(base);
      if (i >= 0 && _ech[i].freq)
        out += ",\"ms\":" + String((uint32_t)((uint64_t)_ech[i].trames * 1000ULL / _ech[i].freq));
      out += "}";
    }
  }
  out += "]";
  return out;
}

/* THE REFUSALS, in PSRAM (a few dozen bytes each, but internal RAM is the scarce resource):
 * taken at the first refusal, never given back. A small ring: the most recent ones. One writer
 * (the SD-card task); a reader may see a half-written entry once — it only costs a garbled
 * line of a diagnostic list, never a crash (fixed-size, always terminated). */
namespace {
struct Refusal { char nom[NOM_MAX]; char raison[72]; };
constexpr uint8_t REFUSALS_MAX = 8;
Refusal* _refusals = nullptr;
uint8_t  _refusalNext = 0;
}

void noteRefused(const char* nom, const char* raison) {
  if (!nom || !*nom) return;
  if (!_refusals) _refusals = (Refusal*)heap_caps_calloc(REFUSALS_MAX, sizeof(Refusal), MALLOC_CAP_SPIRAM);
  if (!_refusals) return;
  Refusal* r = nullptr;
  for (uint8_t i = 0; i < REFUSALS_MAX; i++) if (!strcmp(_refusals[i].nom, nom)) { r = &_refusals[i]; break; }
  if (!r) { r = &_refusals[_refusalNext]; _refusalNext = (uint8_t)((_refusalNext + 1) % REFUSALS_MAX); }
  strlcpy(r->nom, nom, sizeof(r->nom));
  strlcpy(r->raison, raison ? raison : "", sizeof(r->raison));
}

const char* refusalOf(const char* nom) {
  if (!_refusals || !nom) return nullptr;
  for (uint8_t i = 0; i < REFUSALS_MAX; i++) if (_refusals[i].nom[0] && !strcmp(_refusals[i].nom, nom)) return _refusals[i].raison;
  return nullptr;
}

bool nomValide(const char* nom, String& raison) {
  const size_t n = nom ? strlen(nom) : 0;
  if (!n) { raison = "nom vide"; return false; }
  if (n >= NOM_MAX) {
    raison = "nom trop long (" + String(NOM_MAX - 1) + " caracteres au plus)"; return false;
  }
  for (const char* c = nom; *c; c++) {
    if ((uint8_t)*c < 0x20 || strchr("|;,=/\\\"", *c)) {
      raison = String("caractere interdit dans un nom : « ") + *c
               + " » (il separe les champs d'une cue, un chemin ou du JSON)";
      return false;
    }
  }
  return true;
}

bool ecrireDebut(const void* qui, const char* nom) {
  String raison;
  if (_proprietaire && _proprietaire != qui && millis() - _dernierMorceauMs < ABANDON_MS)
    raison = "un autre televersement est en cours";
  else if (!nomValide(nom, raison)) {}
  else if (!monter())
    raison = "storage non monte";
  if (!raison.length()) {
    if (_enCours) _enCours.close();
    _enCours = LittleFS.open(PROVISOIRE, FILE_WRITE);
    if (_enCours) {
      _proprietaire = qui; _nomEnCours = _base(nom);
      _dernierMorceauMs = millis(); _echecEcriture = false;
      return true;
    }
    raison = "ouverture impossible dans storage";
  }
  _refuse = qui; _raisonRefus = raison;
  return false;
}

bool ecrireMorceau(const void* qui, const uint8_t* d, size_t n) {
  if (qui != _proprietaire || !_enCours) return false;
  _dernierMorceauMs = millis();
  if (_enCours.write(d, n) != n) { _echecEcriture = true; return false; }
  return true;
}

bool ecrireFin(const void* qui, String& raison) {
  if (qui != _proprietaire) {
    raison = (qui == _refuse && _raisonRefus.length()) ? _raisonRefus
                                                       : String("aucun televersement ouvert");
    return false;
  }
  _enCours.close();
  _proprietaire = nullptr;
  Differe::noterFichiersModifies();
  bool bon = false;
  if (_echecEcriture) {
    raison = "ecriture incomplete (storage pleine ?)";
  } else {
    File f = LittleFS.open(PROVISOIRE, FILE_READ);
    uint16_t c; uint32_t fr, t;
    if (!f) raison = "fichier illisible";
    else { bon = _lireEntete(f, c, fr, t, raison); f.close(); }
  }
  if (bon) {
    // L'ancien, s'il y en a un, cede la place au dernier moment.
    const String dest = _chemin(_nomEnCours.c_str());
    if (!LittleFS.rename(PROVISOIRE, dest)) {
      LittleFS.remove(dest);
      bon = LittleFS.rename(PROVISOIRE, dest);
      if (!bon) raison = "renommage impossible dans storage";
    }
  }
  if (!bon) {
    LittleFS.remove(PROVISOIRE);
    Serial.printf("[samples] %s refuse : %s\n", _nomEnCours.c_str(), raison.c_str());
  }
  return bon;
}

void ecrireAbandon(const void* qui) {
  if (qui != _proprietaire) return;
  _enCours.close();
  _proprietaire = nullptr;
  LittleFS.remove(PROVISOIRE);
  Differe::noterFichiersModifies();
  Serial.printf("[samples] %s : televersement interrompu, rien n'est garde\n",
                _nomEnCours.c_str());
}

bool supprimer(const char* nom) {
  if (!monter()) return false;
  Differe::noterFichiersModifies();
  return LittleFS.remove(_chemin(nom));
}

/* TOUT CHARGER, UNE FOIS : au demarrage (restaurerAuBoot), ou au premier son
 * demande. Un fichier refuse (mauvais format, PSRAM pleine) est DIT et saute :
 * un magasin qui echoue en silence est pire qu'un magasin vide.
 *
 * Storage first, HERE; the SD card next, in ITS task (SdCard): MBs read over SPI
 * are read neither in the loop, nor in the web server, nor under the store's lock. */
namespace {
// Under the lock.
void _loadStorage() {
  if (!monter()) return;
  File d = LittleFS.open(DOSSIER);
  if (!d || !d.isDirectory()) return;
  File f = d.openNextFile();
  int i;
  while (f && (i = _vide()) >= 0) {
    if (!f.isDirectory()) {
      const String nom = _base(f.path());
      f.close();
      Echantillon e;
      String raison;
      if (_lire(nom.c_str(), e, raison)) _publier(i, e);
      else Serial.printf("[samples] %s ignore : %s\n", nom.c_str(), raison.c_str());
    } else f.close();
    f = d.openNextFile();
  }
  if (f) { Serial.printf("[samples] au-dela de %u echantillons, le reste est ignore\n",
                         (unsigned)SAMPLES_MAX); f.close(); }
  Serial.printf("[samples] %u echantillons prets, %u o en PSRAM\n",
                (unsigned)_prets, (unsigned)_octets);
}
}  // namespace

uint8_t chargerTout() {
  {
    Verrou verrou;
    if (_charge) return _prets;
    _charge = true;
    _loadStorage();
  }
  SdCard::loadSounds();                // no effect if the card is not mounted
  Verrou verrou;
  return _prets;
}

/* The SD card sounds still to INSTALL: its `.wav` files in /samples whose name is
 * not already in the store (storage wins). Each is installed through
 * _lire(..., fromSdCard), which decides: preloaded in PSRAM if small, otherwise AS A
 * STREAM (no data kept — see _lire). Names that `nomValide` rejects are reported and
 * skipped: a cue could not name them. */
uint8_t sdSoundNames(char noms[][NOM_MAX], uint8_t max) {
  uint8_t n = 0;
  File d = SdCard::open(SdCard::FOLDER);
  if (!d || !d.isDirectory()) {
    NIDMI_WEB_LOG("[SD] pas de dossier %s sur la carte : rien a lire", SdCard::FOLDER);
    return 0;
  }
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    const char* base = _base(f.path());
    if (!_isWav(base) || indexDe(base) >= 0) continue;
    String raison;
    if (!nomValide(base, raison)) { NIDMI_WEB_LOG("[SD] %s ignore : %s", base, raison.c_str()); continue; }
    if (n >= max) { NIDMI_WEB_LOG("[SD] au-dela de %u sons, le reste est ignore", (unsigned)max); break; }
    strlcpy(noms[n++], base, NOM_MAX);
  }
  return n;
}

bool installer(const char* nom, String& raison, int& retire, bool fromSdCard) {
  retire = -1;
  const char* base = _base(nom);
  {
    Verrou verrou;
    if (!_charge) return true;         // chargerTout() le lira avec les autres
  }
  /* Lu HORS du verrou : 30 a 70 ms de flash, pendant lesquelles une cue qui
   * arme le lecteur (setSampler → chargerTout) ne doit pas attendre. */
  Echantillon neuf;
  if (!monter() || !_lire(base, neuf, raison, fromSdCard)) {
    if (!raison.length()) raison = "storage non monte";
    return false;
  }
  Verrou verrou;
  const int i = _vide();
  if (i < 0) {
    heap_caps_free(neuf.pcm);
    raison = "plus de place en PSRAM avant le redemarrage ("
             + String(SAMPLES_MAX) + " echantillons au plus)";
    return false;
  }
  const int ancien = _index(base);
  _publier(i, neuf);                   // le nouveau d'abord : jamais un instant sans son
  if (ancien >= 0) _retirer(ancien);
  retire = ancien;
  return true;
}

int retirer(const char* nom) {
  Verrou verrou;
  const int i = _index(_base(nom));
  if (i >= 0) _retirer(i);
  return i;
}

void liberer(int i) {
  if (i < 0 || i >= SAMPLES_MAX) return;
  Verrou verrou;
  Echantillon& e = _ech[i];
  if (e.pret || !e.pcm) return;
  int16_t* pcm = e.pcm;
  _octets -= e.octets;
  e.pcm = nullptr; e.trames = 0; e.octets = 0; e.stereo = false; e.freq = 48000;
  e.nom[0] = 0; e.sd = false; e.streamed = false; e.dataOffset = 0;
  __sync_synchronize();
  heap_caps_free(pcm);
}

bool           charge()             { Verrou verrou; return _charge; }
uint8_t        nombreCharges()      { return _prets; }
bool           lisible(uint8_t i)   { return i < SAMPLES_MAX && _ech[i].pret; }
const int16_t* donnees(uint8_t i)   { return lisible(i) ? _ech[i].pcm    : nullptr; }
size_t         trames(uint8_t i)    { return lisible(i) ? _ech[i].trames : 0; }
bool           stereo(uint8_t i)    { return (i < SAMPLES_MAX) ? _ech[i].stereo : false; }
uint32_t       frequence(uint8_t i) { return (i < SAMPLES_MAX) ? _ech[i].freq   : 48000; }
bool           isStreamed(uint8_t i)     { return i < SAMPLES_MAX && _ech[i].pret && _ech[i].streamed; }
uint32_t       dataOffset(uint8_t i)     { return (i < SAMPLES_MAX) ? _ech[i].dataOffset : 0; }

/* Retrouver un echantillon par son NOM — c'est ce que porte une ligne de cue.
 * Seuls les lisibles : un son retire ne se declenche plus. */
int indexDe(const char* n) {
  if (!n || !*n) return -1;
  return _index(_base(n));
}
size_t         octetsPsram() { return _octets; }

}  // namespace SampleStore
