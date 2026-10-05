#include "SdSpiDisk.h"

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

namespace SdSpiDisk {
namespace {

SPIClass* _spi = nullptr;
uint8_t   _cs = 255;
uint32_t  _hz = 10000000;
bool      _hc = false;                 // block addressing (SDHC/SDXC); otherwise byte addressing
bool      _v2 = false;
bool      _ready = false;
uint32_t  _sectors = 0;
uint32_t  _cmd8Echo = 0;
uint8_t   _pdrv = 0xFF;
char      _mountPoint[16] = "";
volatile uint32_t _blocksRead = 0, _crcErrors = 0, _retries = 0;

/* The file system as seen by Arduino's `File` API: a VFS implementation with its
 * mount point, like SDFS in the library. Created at mount time, not at static
 * initialisation. */
class Disk : public fs::FS {
public:
  Disk() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}
  void mountPoint(const char* p) { _impl->mountpoint(p); }
};
Disk* _fs = nullptr;

constexpr uint8_t DATA_TOKEN = 0xFE;

inline void _select()   { digitalWrite(_cs, LOW); }
inline void _deselect() { digitalWrite(_cs, HIGH); _spi->transfer(0xFF); }   // the card releases MISO

/* The card is ready when it returns 0xFF (it holds MISO at 0 while it is busy). */
bool _waitReady(uint32_t ms) {
  const uint32_t t0 = millis();
  uint8_t r;
  do { r = _spi->transfer(0xFF); } while (r != 0xFF && millis() - t0 < ms);
  return r == 0xFF;
}

/* A command: six bytes, then R1 (bit 7 drops, ten bytes at most) and, if `r32`,
 * the four bytes of an R3/R7. CRC: only CMD0 and CMD8 are checked. */
uint8_t _cmd(uint8_t cmd, uint32_t arg, uint8_t crc, uint32_t* r32 = nullptr) {
  uint8_t p[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                   (uint8_t)(arg >> 8), (uint8_t)arg, crc };
  _spi->writeBytes(p, 6);
  if (cmd == 12) _spi->transfer(0xFF);                   // STOP_TRANSMISSION: one stuff byte
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 10; i++) { r1 = _spi->transfer(0xFF); if (!(r1 & 0x80)) break; }
  if (r32) {
    uint32_t v = 0;
    if (r1 != 0xFF) for (int i = 0; i < 4; i++) v = (v << 8) | _spi->transfer(0xFF);
    *r32 = v;
  }
  return r1;
}

/* CRC16-CCITT (poly 0x1021, init 0) of a block's data. */
uint16_t _crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

/* A data block after a read command: the 0xFE token, `n` bytes, the CRC16.
 * A bad CRC is COUNTED, not rejected (see the header). */
bool _block(uint8_t* buf, size_t n) {
  const uint32_t t0 = millis();
  uint8_t tok;
  do {
    tok = _spi->transfer(0xFF);
    if (tok == 0xFF && millis() - t0 > 4) vTaskDelay(1);   // a slow card: yield
  } while (tok == 0xFF && millis() - t0 < 300);
  if (tok != DATA_TOKEN) return false;
  _spi->transferBytes(nullptr, buf, n);
  const uint16_t received = _spi->transfer16(0xFFFF);
  if (received != _crc16(buf, n)) _crcErrors = _crcErrors + 1;
  return true;
}

/* INITIALISATION, at 400 kHz. Every failure names its step. */
bool _initialise(String& reason) {
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  _spi->beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  bool ok = false;
  auto finish = [&]() { _deselect(); _spi->endTransaction(); return ok; };

  for (int i = 0; i < 20; i++) _spi->transfer(0xFF);      // 160 pulses, CS high
  uint8_t r = 0xFF;
  for (int attempt = 0; attempt < 5 && r != 0x01; attempt++) {
    _select();
    _waitReady(50);                                       // a busy card: the wait is ignored
    r = _cmd(0, 0, 0x95);
    _deselect();
    if (r != 0x01) for (int i = 0; i < 10; i++) _spi->transfer(0xFF);
  }
  if (r != 0x01) {
    reason = r == 0xFF ? "CMD0 : aucune reponse (carte absente, fils, alimentation)"
                       : "CMD0 : reponse inattendue 0x" + String(r, HEX);
    return finish();
  }

  /* CMD8: a piece of INFORMATION. R1 = 0x01: a v2 card; 0x05: "illegal command",
   * a v1 card. The echo (0x1AA) is noted, never demanded. */
  _select();
  uint32_t r7 = 0;
  const uint8_t r8 = _cmd(8, 0x1AA, 0x87, &r7);
  _deselect();
  _v2 = (r8 == 0x01);
  _cmd8Echo = _v2 ? r7 : 0;
  if (_v2 && (r7 & 0xFFF) != 0x1AA)
    NIDMI_WEB_LOG("[SD] CMD8 : echo 0x%08lX au lieu de 0x1AA — carte non conforme, on continue", (unsigned long)r7);

  /* ACMD41: leave idle. HCS (bit 30) if v2. Up to 1.5 s. */
  uint32_t t0 = millis();
  do {
    _select(); _cmd(55, 0, 0x01); _deselect();
    _select(); r = _cmd(41, _v2 ? 0x40000000UL : 0, 0x01); _deselect();
    if (r == 0x01) vTaskDelay(pdMS_TO_TICKS(10));
  } while (r == 0x01 && millis() - t0 < 1500);
  if (r != 0x00) {
    reason = r == 0x01 ? "ACMD41 : la carte reste au repos apres 1,5 s (alimentation trop faible ?)"
                       : "ACMD41 : reponse 0x" + String(r, HEX) + " (carte MMC ou non supportee)";
    return finish();
  }

  /* Addressing: CCS (OCR bit 30). Otherwise by bytes, 512-byte blocks. */
  uint32_t ocr = 0;
  _select(); r = _cmd(58, 0, 0x01, &ocr); _deselect();
  _hc = (r == 0x00) && (ocr & (1UL << 30));
  if (!_hc) {
    _select(); r = _cmd(16, 512, 0x01); _deselect();
    if (r != 0x00) { reason = "CMD16 : longueur de bloc refusee (0x" + String(r, HEX) + ")"; return finish(); }
  }

  /* The capacity: the CSD (CMD9), a 16-byte block. */
  uint8_t csd[16];
  _select();
  r = _cmd(9, 0, 0x01);
  const bool readOk = (r == 0x00) && _block(csd, 16);
  _deselect();
  if (!readOk) { reason = "CMD9 : CSD illisible"; return finish(); }
  if ((csd[0] >> 6) == 1) {                               // CSD v2 (SDHC/SDXC)
    const uint32_t c = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
    _sectors = (c + 1) * 1024;
  } else {                                                // CSD v1
    const uint32_t readBlLen = csd[5] & 0x0F;
    const uint32_t c = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    const uint32_t m = ((csd[9] & 3) << 1) | (csd[10] >> 7);
    _sectors = (uint32_t)(((uint64_t)(c + 1) << (m + 2) << readBlLen) >> 9);
  }
  ok = true;
  return finish();
}

/* Read `n` sectors: a single one (CMD17) or in one go (CMD18). Three attempts. */
bool _readSectors(uint8_t* buf, uint32_t sector, uint32_t n) {
  _spi->beginTransaction(SPISettings(_hz, MSBFIRST, SPI_MODE0));
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    if (attempt) _retries = _retries + 1;
    _select();
    if (!_waitReady(300)) { _deselect(); continue; }
    const uint32_t address = _hc ? sector : (sector << 9);
    if (n == 1) {
      ok = (_cmd(17, address, 0x01) == 0x00) && _block(buf, 512);
    } else if (_cmd(18, address, 0x01) == 0x00) {
      uint32_t i = 0;
      for (; i < n; i++) if (!_block(buf + 512 * i, 512)) break;
      _cmd(12, 0, 0x01);
      _waitReady(300);
      ok = (i == n);
    }
    _deselect();
  }
  _spi->endTransaction();
  if (ok) _blocksRead = _blocksRead + n;
  return ok;
}

// ── FatFs: a read-only drive ────────────────────────────────────────────────
DSTATUS _ffInit(BYTE)   { return _ready ? STA_PROTECT : STA_NOINIT; }
DSTATUS _ffStatus(BYTE) { return _ready ? STA_PROTECT : STA_NOINIT; }
DRESULT _ffRead(BYTE, BYTE* b, DWORD sec, UINT n) {
  if (!_ready) return RES_NOTRDY;
  return _readSectors(b, sec, n) ? RES_OK : RES_ERROR;
}
DRESULT _ffWrite(BYTE, const BYTE*, DWORD, UINT) { return RES_WRPRT; }
DRESULT _ffIoctl(BYTE, BYTE cmd, void* buff) {
  switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *((DWORD*)buff) = _sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *((WORD*)buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *((DWORD*)buff) = 1; return RES_OK;
  }
  return RES_PARERR;
}

}  // namespace

bool mount(SPIClass& spi, uint8_t cs, uint32_t hz, const char* mountPoint, String& reason) {
  if (_ready) return true;
  _spi = &spi; _cs = cs; _hz = hz;
  _blocksRead = _crcErrors = _retries = 0;
  if (!_initialise(reason)) return false;

  if (ff_diskio_get_drive(&_pdrv) != ESP_OK || _pdrv == 0xFF) {
    reason = "FatFs : plus de lecteur libre"; return false;
  }
  static ff_diskio_impl_t impl;
  impl.init = &_ffInit; impl.status = &_ffStatus; impl.read = &_ffRead;
  impl.write = &_ffWrite; impl.ioctl = &_ffIoctl;
  ff_diskio_register(_pdrv, &impl);
  _ready = true;
  strlcpy(_mountPoint, mountPoint, sizeof(_mountPoint));

  char drv[3] = { (char)('0' + _pdrv), ':', 0 };
  FATFS* fatfs = nullptr;
  if (esp_vfs_fat_register(mountPoint, drv, 5, &fatfs) != ESP_OK) {
    _ready = false; ff_diskio_register(_pdrv, nullptr); _pdrv = 0xFF;
    reason = "esp_vfs_fat_register a echoue (point de montage deja pris ?)";
    return false;
  }
  const FRESULT res = f_mount(fatfs, drv, 1);
  if (res != FR_OK) {
    esp_vfs_fat_unregister_path(mountPoint);
    _ready = false; ff_diskio_register(_pdrv, nullptr); _pdrv = 0xFF;
    reason = res == FR_NO_FILESYSTEM
        ? String("aucun systeme de fichiers FAT lisible : la carte est-elle formatee en FAT32 ?")
        : "FatFs : f_mount a echoue (code " + String((int)res) + ")";
    return false;
  }
  if (!_fs) _fs = new Disk();
  _fs->mountPoint(mountPoint);
  return true;
}

void unmount() {
  if (!_ready) return;
  char drv[3] = { (char)('0' + _pdrv), ':', 0 };
  f_mount(nullptr, drv, 0);
  esp_vfs_fat_unregister_path(_mountPoint);
  ff_diskio_register(_pdrv, nullptr);
  _ready = false;
  _pdrv = 0xFF;
  _sectors = 0;
}

bool        mounted()       { return _ready; }
uint64_t    capacityBytes() { return (uint64_t)_sectors * 512ULL; }
const char* type()          { return _hc ? "SDHC/SDXC" : "SD"; }
uint32_t    cmd8Echo()      { return _cmd8Echo; }
uint32_t    blocksRead()    { return _blocksRead; }
uint32_t    crcErrors()     { return _crcErrors; }
uint32_t    retries()       { return _retries; }

File open(const char* path) {
  if (!_ready || !_fs) return File();
  return _fs->open(path, FILE_READ);
}

}  // namespace SdSpiDisk
