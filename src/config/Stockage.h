#pragma once
// src/config/Stockage.h — LA PARTITION DE FICHIERS, NOMMEE UNE FOIS
// (CONVERGENCE_NIDMI.md §9.2, MESURES §185).
//
// Tout ce qui est un fichier sur la carte vit dans UNE partition LittleFS :
// les compositions, l'interface (ses scripts de broche, ses sauvegardes), les
// sons. Elle s'appelait « mapfs » (« scripts de mapping ») et partageait la
// flash avec « seqfs » (jamais lue) et « coredump » (plus ecrite depuis le
// §163) : les trois n'en font plus qu'une, `storage`, 1,56 Mo — le nom d'usage
// d'une partition de fichiers dans les exemples d'ESP-IDF.
//
// Le nom etait recopie dans six fichiers : une copie finit par diverger. Il
// est ici, et nulle part ailleurs ; la table (tools/nidmi_s3_ota.csv) dit le
// meme.
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

namespace Stockage {
constexpr const char* PARTITION = "storage";    // le nom dans la table de partitions
constexpr const char* BASE      = "/storage";   // son point de montage ; les chemins montres commencent apres

/* THE NEXT ENTRY OF A WALK OF storage — then as long a sleep as it took (MESURES §240).
 * Every flash read parks core 0 (cache off on both cores), and an entry costs
 * hundreds of 128-byte reads: Arduino reopens it by its full path, and LittleFS
 * reads every directory again from the root — 25 ms per entry, 60 in a crowded
 * folder with the sound on. Measured: core 0 parked 36 % of the time with the sound
 * off; deeper in the tree it reached all of it, core 0's idle task stopped running,
 * and the task watchdog restarted the board — the board panel did it each time it
 * opened. Sleeping 1 ms between entries changed nothing. Sleeping as long as the
 * entry took keeps a walk under half of core 0's time, from any task; a walk lasts
 * twice as long. Every walk of storage takes its entries here. */
inline File nextEntry(File& dir) {
  const int64_t t0 = esp_timer_get_time();
  File f = dir.openNextFile();
  uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  if (ms < 1) ms = 1;
  if (ms > 100) ms = 100;
  vTaskDelay(pdMS_TO_TICKS(ms));
  return f;
}
}  // namespace Stockage
