#include "SampleStore.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>

namespace SampleStore {
namespace {

constexpr const char* PARTITION = "mapfs";        // 1 Mo, table nidmi_s3_ota_dual_littlefs
constexpr const char* BASE      = "/mapfs";
constexpr const char* DOSSIER   = "/samples";

bool   _monte = false;
File   _enCours;

/* N ECHANTILLONS, tous en PSRAM, tous permanents. Les descripteurs vivent en
 * RAM interne — 24 x ~64 o, soit ~1,5 ko de .bss, et pas un octet du bloc
 * contigu qui decide du service web. Le PCM, lui, est entierement en PSRAM. */
struct Echantillon {
  int16_t* pcm    = nullptr;
  size_t   trames = 0;
  bool     stereo = false;
  uint32_t freq   = 48000;
  char     nom[48] = {0};
};
Echantillon _ech[SAMPLES_MAX];
uint8_t     _n      = 0;
size_t      _octets = 0;

String _chemin(const char* nom) {
  String p = String(DOSSIER) + "/";
  // Pas de traversée : on ne garde que le nom de base.
  const char* base = strrchr(nom, '/');
  p += (base ? base + 1 : nom);
  return p;
}

uint32_t _le32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
uint16_t _le16(const uint8_t* p) { return p[0] | (p[1]<<8); }

}  // namespace

bool estMonte() { return _monte; }

bool monter() {
  if (_monte) return true;
  // formatOnFail : la partition n'a jamais servi, elle est vierge.
  if (!LittleFS.begin(true, BASE, 10, PARTITION)) {
    Serial.println("[samples] montage de mapfs impossible");
    return false;
  }
  if (!LittleFS.exists(DOSSIER)) LittleFS.mkdir(DOSSIER);
  _monte = true;
  Serial.printf("[samples] mapfs monte — %u o utilises sur %u\n",
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
        const char* n = strrchr(f.path(), '/');
        out += "{\"name\":\"" + String(n ? n + 1 : f.name()) + "\",\"bytes\":" + String(f.size()) + "}";
      }
      f = d.openNextFile();
    }
  }
  out += "]";
  return out;
}

bool ecrireDebut(const char* nom) {
  if (!monter()) return false;
  if (_enCours) _enCours.close();
  _enCours = LittleFS.open(_chemin(nom), FILE_WRITE);
  return (bool)_enCours;
}
bool ecrireMorceau(const uint8_t* d, size_t n) {
  if (!_enCours) return false;
  return _enCours.write(d, n) == n;
}
bool ecrireFin() {
  if (!_enCours) return false;
  _enCours.close();
  return true;
}
void ecrireAbandon() { if (_enCours) _enCours.close(); }

bool supprimer(const char* nom) {
  if (!monter()) return false;
  return LittleFS.remove(_chemin(nom));
}



/* Charge UN fichier dans l'emplacement `dest`. Le parseur d'en-tete WAV est
 * inchange — c'est le stockage qui devient multiple. */
static bool _chargerDans(const char* nom, Echantillon& dest, String& raison) {
  if (!monter()) { raison = "mapfs non monte"; return false; }
  File f = LittleFS.open(_chemin(nom), FILE_READ);
  if (!f) { raison = "fichier introuvable"; return false; }

  uint8_t e[12];
  if (f.read(e, 12) != 12 || memcmp(e, "RIFF", 4) || memcmp(e + 8, "WAVE", 4)) {
    raison = "pas un WAV"; f.close(); return false;
  }

  uint16_t canaux = 0, bits = 0;
  uint32_t freq = 0, tailleData = 0;
  // Parcours des chunks jusqu'à « data ».
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
    f.close(); return false;
  }

  // PSRAM : 8,25 Mo libres pendant que le tas interne se bat pour 14 ko.
  dest.pcm = (int16_t*)heap_caps_malloc(tailleData, MALLOC_CAP_SPIRAM);
  if (!dest.pcm) {
    raison = "PSRAM insuffisante pour " + String(tailleData) + " o";
    f.close(); return false;
  }
  const size_t lus = f.read((uint8_t*)dest.pcm, tailleData);
  f.close();
  if (lus != tailleData) {
    heap_caps_free(dest.pcm); dest.pcm = nullptr;
    raison = "lecture incomplete"; return false;
  }

  dest.stereo = (canaux == 2);
  dest.freq   = freq ? freq : 48000;
  dest.trames = tailleData / (2 * canaux);
  strncpy(dest.nom, nom, sizeof(dest.nom) - 1);
  _octets += tailleData;
  Serial.printf("[samples] %s charge : %u trames, %u Hz, %s, %u o en PSRAM\n",
                dest.nom, (unsigned)dest.trames, (unsigned)dest.freq,
                dest.stereo ? "stereo" : "mono", (unsigned)tailleData);
  return true;
}

/* TOUT CHARGER, UNE FOIS. Appele au demarrage et apres chaque televersement ou
 * suppression. Un fichier refuse (mauvais format, PSRAM pleine) est DIT et
 * saute : un magasin qui echoue en silence est pire qu'un magasin vide. */
uint8_t chargerTout() {
  oublierTout();
  if (!monter()) return 0;
  File d = LittleFS.open(DOSSIER);
  if (!d || !d.isDirectory()) return 0;
  File f = d.openNextFile();
  while (f && _n < SAMPLES_MAX) {
    if (!f.isDirectory()) {
      const char* c = strrchr(f.path(), '/');
      String nom = String(c ? c + 1 : f.name());
      f.close();
      String raison;
      if (_chargerDans(nom.c_str(), _ech[_n], raison)) _n++;
      else Serial.printf("[samples] %s ignore : %s\n", nom.c_str(), raison.c_str());
    } else f.close();
    f = d.openNextFile();
  }
  if (f) { Serial.printf("[samples] au-dela de %u echantillons, le reste est ignore\n",
                         (unsigned)SAMPLES_MAX); f.close(); }
  Serial.printf("[samples] %u echantillons prets, %u o en PSRAM\n",
                (unsigned)_n, (unsigned)_octets);
  return _n;
}

void oublierTout() {
  for (uint8_t i = 0; i < _n; i++) {
    if (_ech[i].pcm) heap_caps_free(_ech[i].pcm);
    _ech[i] = Echantillon{};
  }
  _n = 0; _octets = 0;
}

uint8_t        nombreCharges()      { return _n; }
const int16_t* donnees(uint8_t i)   { return (i < _n) ? _ech[i].pcm    : nullptr; }
size_t         trames(uint8_t i)    { return (i < _n) ? _ech[i].trames : 0; }
bool           stereo(uint8_t i)    { return (i < _n) ? _ech[i].stereo : false; }
uint32_t       frequence(uint8_t i) { return (i < _n) ? _ech[i].freq   : 48000; }
const char*    nom(uint8_t i)       { return (i < _n) ? _ech[i].nom    : ""; }

/* Retrouver un echantillon par son NOM — c'est ce que porte une ligne de cue.
 * On compare sur le nom de base : mapfs est un panier plat. */
int indexDe(const char* n) {
  if (!n || !*n) return -1;
  const char* base = strrchr(n, '/');
  if (base) n = base + 1;
  for (uint8_t i = 0; i < _n; i++) if (!strcmp(_ech[i].nom, n)) return i;
  return -1;
}
size_t         octetsPsram() { return _octets; }

}  // namespace SampleStore
