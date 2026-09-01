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

int16_t* _pcm     = nullptr;      // en PSRAM
size_t   _trames  = 0;
bool     _stereo  = false;
uint32_t _freq    = 48000;
size_t   _octets  = 0;
char     _nom[48] = {0};

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

void decharger() {
  if (_pcm) { heap_caps_free(_pcm); _pcm = nullptr; }
  _trames = 0; _octets = 0; _nom[0] = '\0';
}

bool charger(const char* nom, String& raison) {
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

  decharger();
  // PSRAM : 8,37 Mo inutilisés pendant que le tas interne se bat pour 13 ko.
  _pcm = (int16_t*)heap_caps_malloc(tailleData, MALLOC_CAP_SPIRAM);
  if (!_pcm) {
    raison = "PSRAM insuffisante pour " + String(tailleData) + " o";
    f.close(); return false;
  }
  const size_t lus = f.read((uint8_t*)_pcm, tailleData);
  f.close();
  if (lus != tailleData) { decharger(); raison = "lecture incomplete"; return false; }

  _stereo = (canaux == 2);
  _freq   = freq ? freq : 48000;
  _octets = tailleData;
  _trames = tailleData / (2 * canaux);
  strncpy(_nom, nom, sizeof(_nom) - 1);
  Serial.printf("[samples] %s charge : %u trames, %u Hz, %s, %u o en PSRAM\n",
                _nom, (unsigned)_trames, (unsigned)_freq,
                _stereo ? "stereo" : "mono", (unsigned)_octets);
  return true;
}

bool           estCharge()   { return _pcm != nullptr && _trames > 0; }
const int16_t* donnees()     { return _pcm; }
size_t         trames()      { return _trames; }
bool           stereo()      { return _stereo; }
uint32_t       frequence()   { return _freq; }
const char*    nomCharge()   { return _nom; }
size_t         octetsPsram() { return _octets; }

}  // namespace SampleStore
