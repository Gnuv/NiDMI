#include "SampleStore.h"
#include "../config/Stockage.h"
#include "../config/EcrituresDifferees.h"
#include "../config/CarteSd.h"
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
  bool     sd     = false;      // lu depuis la carte SD (CarteSd), pas depuis storage
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
bool _estWav(const char* nom) {
  const size_t n = strlen(nom);
  return n > 4 && !strcasecmp(nom + n - 4, ".wav");
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
  d.stereo = e.stereo; d.freq = e.freq; d.sd = e.sd;
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

/* Lit UN fichier dans `dest`, qui n'est encore visible de personne. `carteSd` :
 * le son est sur la carte SD (CarteSd) et non dans storage — il se lit alors par
 * morceaux, en cedant le processeur entre deux : un son de plusieurs Mo prend des
 * secondes en SPI, et la tache qui le lit ne doit pas affamer l'IDLE (chien de
 * garde) ni rien d'autre. */
bool _lire(const char* nom, Echantillon& dest, String& raison, bool carteSd = false) {
  if (strlen(nom) >= NOM_MAX) {
    raison = "nom trop long (" + String(NOM_MAX - 1) + " caracteres au plus)"; return false;
  }
  File f = carteSd ? CarteSd::ouvrir((String(CarteSd::DOSSIER) + "/" + _base(nom)).c_str())
                   : LittleFS.open(_chemin(nom), FILE_READ);
  if (!f) { raison = "fichier introuvable"; return false; }
  uint16_t canaux; uint32_t freq, tailleData;
  if (!_lireEntete(f, canaux, freq, tailleData, raison)) { f.close(); return false; }

  // PSRAM : 8,25 Mo libres pendant que le tas interne se bat pour 14 ko.
  int16_t* pcm = (int16_t*)heap_caps_malloc(tailleData, MALLOC_CAP_SPIRAM);
  if (!pcm) {
    raison = "PSRAM insuffisante pour " + String(tailleData) + " o";
    f.close(); return false;
  }
  size_t lus = 0;
  if (!carteSd) {
    lus = f.read((uint8_t*)pcm, tailleData);
  } else {
    constexpr size_t MORCEAU = 16384;
    while (lus < tailleData) {
      const size_t pas = (tailleData - lus < MORCEAU) ? tailleData - lus : MORCEAU;
      const size_t n = f.read((uint8_t*)pcm + lus, pas);
      if (!n) break;
      lus += n;
      vTaskDelay(1);                   // cede : l'IDLE du coeur 0 doit tourner
    }
  }
  f.close();
  if (lus != tailleData) { heap_caps_free(pcm); raison = "lecture incomplete"; return false; }

  dest.pcm    = pcm;
  dest.octets = tailleData;
  dest.sd     = carteSd;
  dest.stereo = (canaux == 2);
  dest.freq   = freq ? freq : 48000;
  dest.trames = tailleData / (2 * canaux);
  strlcpy(dest.nom, nom, sizeof(dest.nom));
  NIDMI_WEB_LOG("[samples] %s charge%s : %u trames, %u Hz, %s, %u o en PSRAM",
                dest.nom, carteSd ? " (carte SD)" : "", (unsigned)dest.trames, (unsigned)dest.freq,
                dest.stereo ? "stereo" : "mono", (unsigned)tailleData);
  return true;
}

}  // namespace

bool estMonte() { return _monte; }

bool enteteWav(File& f, uint16_t& canaux, uint32_t& freq, uint32_t& octetsData, String& raison) {
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
  /* Les sons de la carte SD, a la suite — « volume » dit d'ou ils viennent. Un nom
   * que storage porte deja l'emporte, et ne se liste pas deux fois. */
  File s = CarteSd::ouvrir(CarteSd::DOSSIER);
  if (s && s.isDirectory()) {
    bool premier = out.length() == 1;
    for (File f = s.openNextFile(); f; f = s.openNextFile()) {
      if (f.isDirectory()) continue;
      const char* base = _base(f.path());
      if (!_estWav(base) || LittleFS.exists(_chemin(base))) continue;
      if (!premier) out += ",";
      premier = false;
      out += "{\"name\":\"" + String(base) + "\",\"bytes\":" + String(f.size())
           + ",\"volume\":\"" + CarteSd::VOLUME + "\"";
      const int i = _index(base);
      if (i >= 0 && _ech[i].freq)
        out += ",\"ms\":" + String((uint32_t)((uint64_t)_ech[i].trames * 1000ULL / _ech[i].freq));
      out += "}";
    }
  }
  out += "]";
  return out;
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
 * storage d'abord, ICI ; la carte SD ensuite, dans SA tache (CarteSd) : des Mo
 * lus en SPI ne se lisent ni dans la boucle, ni dans le serveur web, ni sous le
 * verrou du magasin. */
namespace {
// Sous le verrou.
void _chargerStorage() {
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
    _chargerStorage();
  }
  CarteSd::chargerSons();              // sans effet si la carte n'est pas montee
  Verrou verrou;
  return _prets;
}

/* Les sons de la carte SD qu'il reste a PRECHARGER : ses `.wav` de /samples dont le
 * nom n'est pas deja dans le magasin (storage l'emporte).
 *
 * UN PLAFOND, parce que la PSRAM est une ressource commune : les tampons de reponse
 * du serveur web, les banques de play list et la console y vivent aussi. Une carte
 * SD pleine de sons ne doit pas la vider. Un son plus gros que PRECHARGE_SON_MAX, ou
 * qui depasserait PRECHARGE_SD_MAX en tout, n'est PAS precharge — il est dit, et il
 * attend la lecture en flux (CarteSd.h), qui ne le garde pas en memoire.
 * Les noms que `nomValide` refuse sont dits et sautes : une cue ne saurait pas les
 * nommer. */
uint8_t sonsDeLaCarteSd(char noms[][NOM_MAX], uint8_t max) {
  uint8_t n = 0;
  File d = CarteSd::ouvrir(CarteSd::DOSSIER);
  if (!d || !d.isDirectory()) {
    NIDMI_WEB_LOG("[SD] pas de dossier %s sur la carte : rien a lire", CarteSd::DOSSIER);
    return 0;
  }
  size_t cumul = 0;
  for (int i = 0; i < SAMPLES_MAX; i++) if (_ech[i].pret && _ech[i].sd) cumul += _ech[i].octets;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    const char* base = _base(f.path());
    if (!_estWav(base) || indexDe(base) >= 0) continue;
    String raison;
    if (!nomValide(base, raison)) { NIDMI_WEB_LOG("[SD] %s ignore : %s", base, raison.c_str()); continue; }
    const size_t taille = f.size();
    if (taille > PRECHARGE_SON_MAX || cumul + taille > PRECHARGE_SD_MAX) {
      NIDMI_WEB_LOG("[SD] %s : %u Ko, trop pour la PSRAM (un son : %u Ko au plus, la SD : %u Ko en tout) — pas precharge",
                    base, (unsigned)(taille / 1024), (unsigned)(PRECHARGE_SON_MAX / 1024),
                    (unsigned)(PRECHARGE_SD_MAX / 1024));
      continue;
    }
    if (n >= max) { NIDMI_WEB_LOG("[SD] au-dela de %u sons, le reste est ignore", (unsigned)max); break; }
    cumul += taille;
    strlcpy(noms[n++], base, NOM_MAX);
  }
  return n;
}

bool installer(const char* nom, String& raison, int& retire, bool carteSd) {
  retire = -1;
  const char* base = _base(nom);
  {
    Verrou verrou;
    if (!_charge) return true;         // chargerTout() le lira avec les autres
  }
  /* Lu HORS du verrou : 30 a 70 ms de flash, pendant lesquelles une cue qui
   * arme le lecteur (setSampler → chargerTout) ne doit pas attendre. */
  Echantillon neuf;
  if (!monter() || !_lire(base, neuf, raison, carteSd)) {
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
  e.nom[0] = 0; e.sd = false;
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

/* Retrouver un echantillon par son NOM — c'est ce que porte une ligne de cue.
 * Seuls les lisibles : un son retire ne se declenche plus. */
int indexDe(const char* n) {
  if (!n || !*n) return -1;
  return _index(_base(n));
}
size_t         octetsPsram() { return _octets; }

}  // namespace SampleStore
