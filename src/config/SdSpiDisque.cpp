#include "SdSpiDisque.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vfs_api.h>

extern "C" {
#include "ff.h"
#include "diskio.h"
#include "diskio_impl.h"
#include "esp_vfs_fat.h"
}

#include "../server/WebDebugConsole.h"

namespace SdSpiDisque {
namespace {

SPIClass* _spi = nullptr;
uint8_t   _cs = 255;
uint32_t  _hz = 10000000;
bool      _hc = false;                 // adressage par blocs (SDHC/SDXC) ; sinon par octets
bool      _v2 = false;
bool      _pret = false;
uint32_t  _secteurs = 0;
uint32_t  _echo8 = 0;
uint8_t   _pdrv = 0xFF;
char      _point[16] = "";
volatile uint32_t _lectures = 0, _erreursCrc = 0, _relectures = 0;

/* Le systeme de fichiers vu par l'API `File` d'Arduino : une implementation VFS
 * avec son point de montage, comme SDFS dans la bibliotheque. Cree au montage, pas a
 * l'initialisation statique. */
class Disque : public fs::FS {
public:
  Disque() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}
  void point(const char* p) { _impl->mountpoint(p); }
};
Disque* _fs = nullptr;

constexpr uint8_t DEBUT_BLOC = 0xFE;

inline void _select()   { digitalWrite(_cs, LOW); }
inline void _deselect() { digitalWrite(_cs, HIGH); _spi->transfer(0xFF); }   // la carte lache MISO

/* La carte est prete quand elle rend 0xFF (elle tient MISO a 0 tant qu'elle est occupee). */
bool _attendrePret(uint32_t ms) {
  const uint32_t t0 = millis();
  uint8_t r;
  do { r = _spi->transfer(0xFF); } while (r != 0xFF && millis() - t0 < ms);
  return r == 0xFF;
}

/* Une commande : six octets, puis R1 (le bit 7 retombe, dix octets au plus) et, si
 * `r32`, les quatre octets d'un R3/R7. CRC : seuls CMD0 et CMD8 sont verifies. */
uint8_t _cmd(uint8_t cmd, uint32_t arg, uint8_t crc, uint32_t* r32 = nullptr) {
  uint8_t p[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                   (uint8_t)(arg >> 8), (uint8_t)arg, crc };
  _spi->writeBytes(p, 6);
  if (cmd == 12) _spi->transfer(0xFF);                   // STOP_TRANSMISSION : un octet de bourrage
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 10; i++) { r1 = _spi->transfer(0xFF); if (!(r1 & 0x80)) break; }
  if (r32) {
    uint32_t v = 0;
    if (r1 != 0xFF) for (int i = 0; i < 4; i++) v = (v << 8) | _spi->transfer(0xFF);
    *r32 = v;
  }
  return r1;
}

/* CRC16-CCITT (poly 0x1021, init 0) des donnees d'un bloc. */
uint16_t _crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

/* Un bloc de donnees apres une commande de lecture : le jeton 0xFE, `n` octets, le CRC16.
 * Le CRC mal recu est COMPTE, pas rejete (voir l'en-tete). */
bool _bloc(uint8_t* buf, size_t n) {
  const uint32_t t0 = millis();
  uint8_t tok;
  do {
    tok = _spi->transfer(0xFF);
    if (tok == 0xFF && millis() - t0 > 4) vTaskDelay(1);   // une carte lente : on cede
  } while (tok == 0xFF && millis() - t0 < 300);
  if (tok != DEBUT_BLOC) return false;
  _spi->transferBytes(nullptr, buf, n);
  const uint16_t recu = _spi->transfer16(0xFFFF);
  if (recu != _crc16(buf, n)) _erreursCrc = _erreursCrc + 1;
  return true;
}

/* L'INITIALISATION, a 400 kHz. Chaque echec dit son etape. */
bool _initialiser(String& raison) {
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  _spi->beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  bool ok = false;
  auto fin = [&]() { _deselect(); _spi->endTransaction(); return ok; };

  for (int i = 0; i < 20; i++) _spi->transfer(0xFF);      // 160 impulsions, CS haut
  uint8_t r = 0xFF;
  for (int essai = 0; essai < 5 && r != 0x01; essai++) {
    _select();
    _attendrePret(50);                                    // une carte occupee : on ignore l'attente
    r = _cmd(0, 0, 0x95);
    _deselect();
    if (r != 0x01) for (int i = 0; i < 10; i++) _spi->transfer(0xFF);
  }
  if (r != 0x01) {
    raison = r == 0xFF ? "CMD0 : aucune reponse (carte absente, fils, alimentation)"
                       : "CMD0 : reponse inattendue 0x" + String(r, HEX);
    return fin();
  }

  /* CMD8 : un RENSEIGNEMENT. R1 = 0x01 : une carte v2 ; 0x05 : « commande illegale »,
   * une carte v1. L'echo (0x1AA) est note, jamais exige. */
  _select();
  uint32_t r7 = 0;
  const uint8_t r8 = _cmd(8, 0x1AA, 0x87, &r7);
  _deselect();
  _v2 = (r8 == 0x01);
  _echo8 = _v2 ? r7 : 0;
  if (_v2 && (r7 & 0xFFF) != 0x1AA)
    NIDMI_WEB_LOG("[SD] CMD8 : echo 0x%08lX au lieu de 0x1AA — carte non conforme, on continue", (unsigned long)r7);

  /* ACMD41 : sortir du repos. HCS (bit 30) si v2. Jusqu'a 1,5 s. */
  uint32_t t0 = millis();
  do {
    _select(); _cmd(55, 0, 0x01); _deselect();
    _select(); r = _cmd(41, _v2 ? 0x40000000UL : 0, 0x01); _deselect();
    if (r == 0x01) vTaskDelay(pdMS_TO_TICKS(10));
  } while (r == 0x01 && millis() - t0 < 1500);
  if (r != 0x00) {
    raison = r == 0x01 ? "ACMD41 : la carte reste au repos apres 1,5 s (alimentation trop faible ?)"
                       : "ACMD41 : reponse 0x" + String(r, HEX) + " (carte MMC ou non supportee)";
    return fin();
  }

  /* L'adressage : CCS (bit 30 de l'OCR). Sinon par octets, blocs de 512. */
  uint32_t ocr = 0;
  _select(); r = _cmd(58, 0, 0x01, &ocr); _deselect();
  _hc = (r == 0x00) && (ocr & (1UL << 30));
  if (!_hc) {
    _select(); r = _cmd(16, 512, 0x01); _deselect();
    if (r != 0x00) { raison = "CMD16 : longueur de bloc refusee (0x" + String(r, HEX) + ")"; return fin(); }
  }

  /* La capacite : le CSD (CMD9), un bloc de 16 octets. */
  uint8_t csd[16];
  _select();
  r = _cmd(9, 0, 0x01);
  const bool lu = (r == 0x00) && _bloc(csd, 16);
  _deselect();
  if (!lu) { raison = "CMD9 : CSD illisible"; return fin(); }
  if ((csd[0] >> 6) == 1) {                               // CSD v2 (SDHC/SDXC)
    const uint32_t c = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
    _secteurs = (c + 1) * 1024;
  } else {                                                // CSD v1
    const uint32_t lectBl = csd[5] & 0x0F;
    const uint32_t c = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    const uint32_t m = ((csd[9] & 3) << 1) | (csd[10] >> 7);
    _secteurs = (uint32_t)(((uint64_t)(c + 1) << (m + 2) << lectBl) >> 9);
  }
  ok = true;
  return fin();
}

/* Lire `n` secteurs : un seul (CMD17) ou d'un seul tenant (CMD18). Trois essais. */
bool _lireSecteurs(uint8_t* buf, uint32_t secteur, uint32_t n) {
  _spi->beginTransaction(SPISettings(_hz, MSBFIRST, SPI_MODE0));
  bool ok = false;
  for (int essai = 0; essai < 3 && !ok; essai++) {
    if (essai) _relectures = _relectures + 1;
    _select();
    if (!_attendrePret(300)) { _deselect(); continue; }
    const uint32_t adr = _hc ? secteur : (secteur << 9);
    if (n == 1) {
      ok = (_cmd(17, adr, 0x01) == 0x00) && _bloc(buf, 512);
    } else if (_cmd(18, adr, 0x01) == 0x00) {
      uint32_t i = 0;
      for (; i < n; i++) if (!_bloc(buf + 512 * i, 512)) break;
      _cmd(12, 0, 0x01);
      _attendrePret(300);
      ok = (i == n);
    }
    _deselect();
  }
  _spi->endTransaction();
  if (ok) _lectures = _lectures + n;
  return ok;
}

// ── FatFs : un lecteur en lecture seule ─────────────────────────────────────
DSTATUS _ffInit(BYTE)   { return _pret ? STA_PROTECT : STA_NOINIT; }
DSTATUS _ffStatus(BYTE) { return _pret ? STA_PROTECT : STA_NOINIT; }
DRESULT _ffRead(BYTE, BYTE* b, DWORD sec, UINT n) {
  if (!_pret) return RES_NOTRDY;
  return _lireSecteurs(b, sec, n) ? RES_OK : RES_ERROR;
}
DRESULT _ffWrite(BYTE, const BYTE*, DWORD, UINT) { return RES_WRPRT; }
DRESULT _ffIoctl(BYTE, BYTE cmd, void* buff) {
  switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *((DWORD*)buff) = _secteurs; return RES_OK;
    case GET_SECTOR_SIZE:  *((WORD*)buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *((DWORD*)buff) = 1; return RES_OK;
  }
  return RES_PARERR;
}

}  // namespace

bool monter(SPIClass& spi, uint8_t cs, uint32_t hz, const char* point, String& raison) {
  if (_pret) return true;
  _spi = &spi; _cs = cs; _hz = hz;
  _lectures = _erreursCrc = _relectures = 0;
  if (!_initialiser(raison)) return false;

  if (ff_diskio_get_drive(&_pdrv) != ESP_OK || _pdrv == 0xFF) {
    raison = "FatFs : plus de lecteur libre"; return false;
  }
  static ff_diskio_impl_t impl;
  impl.init = &_ffInit; impl.status = &_ffStatus; impl.read = &_ffRead;
  impl.write = &_ffWrite; impl.ioctl = &_ffIoctl;
  ff_diskio_register(_pdrv, &impl);
  _pret = true;
  strlcpy(_point, point, sizeof(_point));

  char drv[3] = { (char)('0' + _pdrv), ':', 0 };
  FATFS* fatfs = nullptr;
  if (esp_vfs_fat_register(point, drv, 5, &fatfs) != ESP_OK) {
    _pret = false; ff_diskio_register(_pdrv, nullptr); _pdrv = 0xFF;
    raison = "esp_vfs_fat_register a echoue (point de montage deja pris ?)";
    return false;
  }
  const FRESULT res = f_mount(fatfs, drv, 1);
  if (res != FR_OK) {
    esp_vfs_fat_unregister_path(point);
    _pret = false; ff_diskio_register(_pdrv, nullptr); _pdrv = 0xFF;
    raison = res == FR_NO_FILESYSTEM
        ? String("aucun systeme de fichiers FAT lisible : la carte est-elle formatee en FAT32 ?")
        : "FatFs : f_mount a echoue (code " + String((int)res) + ")";
    return false;
  }
  if (!_fs) _fs = new Disque();
  _fs->point(point);
  return true;
}

void demonter() {
  if (!_pret) return;
  char drv[3] = { (char)('0' + _pdrv), ':', 0 };
  f_mount(nullptr, drv, 0);
  esp_vfs_fat_unregister_path(_point);
  ff_diskio_register(_pdrv, nullptr);
  _pret = false;
  _pdrv = 0xFF;
  _secteurs = 0;
}

bool        montee()    { return _pret; }
uint64_t    capacite()  { return (uint64_t)_secteurs * 512ULL; }
const char* type()      { return _hc ? "SDHC/SDXC" : "SD"; }
uint32_t    echoCmd8()  { return _echo8; }
uint32_t    lectures()    { return _lectures; }
uint32_t    erreursCrc()  { return _erreursCrc; }
uint32_t    relectures()  { return _relectures; }

File ouvrir(const char* chemin) {
  if (!_pret || !_fs) return File();
  return _fs->open(chemin, FILE_READ);
}

}  // namespace SdSpiDisque
