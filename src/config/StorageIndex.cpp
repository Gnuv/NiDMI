#include "StorageIndex.h"
#include "Stockage.h"
#include "../audio/AudioEngine.h"
#include "../server/WebDebugConsole.h"
#include <dirent.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace StorageIndex {
namespace {

constexpr int    kMax     = 512;          // storage held 181 entries on 29/10
constexpr size_t kPathMax = 160;          // a full path, as ScriptStore keeps them
struct Entry { char path[kPathMax]; uint32_t size; bool dir; };

Entry*   g_entries = nullptr;             // kMax entries, in PSRAM
int      g_n = 0;
bool     g_full = false;                  // an entry was refused: said once
StaticSemaphore_t g_lockBuf;
SemaphoreHandle_t g_lock = xSemaphoreCreateMutexStatic(&g_lockBuf);
struct Lock {
  Lock()  { xSemaphoreTake(g_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_lock); }
};

volatile bool g_ready = false;
TaskHandle_t  g_task = nullptr;
size_t        g_used = 0, g_total = 0;
volatile bool g_usedStale = false;        // a write since the last block count

int find(const char* path) {
  for (int i = 0; i < g_n; i++) if (!strcmp(g_entries[i].path, path)) return i;
  return -1;
}
bool under(const char* path, const char* folder, size_t lf) {
  return !strncmp(path, folder, lf) && (path[lf] == '\0' || path[lf] == '/');
}

// Under the lock.
void put(const char* path, size_t size, bool dir) {
  if (!g_entries || !path || path[0] != '/' || strlen(path) >= kPathMax) return;
  const int i = find(path);
  if (i >= 0) { g_entries[i].size = (uint32_t)size; g_entries[i].dir = dir; return; }
  if (g_n >= kMax) {
    if (!g_full) NIDMI_WEB_LOG("[storage] index plein (%d entrees) : %s n'y est pas", kMax, path);
    g_full = true;
    return;
  }
  Entry& e = g_entries[g_n++];
  strlcpy(e.path, path, kPathMax);
  e.size = (uint32_t)size;
  e.dir = dir;
}
// Its folders, from the root down. Under the lock.
void putParents(const char* path) {
  char p[kPathMax];
  strlcpy(p, path, sizeof p);
  for (char* s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
    *s = '\0';
    if (find(p) < 0) put(p, 0, true);
    *s = '/';
  }
}

void wake() {
  g_usedStale = true;
  if (g_task) xTaskNotifyGive(g_task);
}

/* THE WALK, by the POSIX calls: one stat per file, none per folder (Arduino's
 * File reopened every entry by its full path). The entries go straight into the
 * index, each under the lock: a write made meanwhile is never lost (put() updates). */
void walk(const String& rel, int depth) {
  const String full = String(Stockage::BASE) + rel;
  DIR* d = opendir(full.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const String p = (rel == "/" ? String() : rel) + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      { Lock l; put(p.c_str(), 0, true); }
      if (depth < 6) walk(p, depth + 1);
      continue;
    }
    struct stat st;
    const size_t size = stat((String(Stockage::BASE) + p).c_str(), &st) == 0 ? (size_t)st.st_size : 0;
    Lock l;
    put(p.c_str(), size, false);
  }
  closedir(d);
}

/* LittleFS's real occupancy: one traversal of every block, 1.3 s with the sound on.
 * At boot, and at silence after a write — like the writes themselves (§157). */
void countBlocks() {
  size_t total = 0, used = 0;
  if (esp_littlefs_info(Stockage::PARTITION, &total, &used) == ESP_OK) {
    Lock l;
    g_total = total;
    g_used = used;
  }
}

void task(void*) {
  const uint32_t t0 = millis();
  walk("/", 0);
  g_usedStale = false;
  countBlocks();
  g_ready = true;
  int n;
  { Lock l; n = g_n; }
  NIDMI_WEB_LOG("[storage] index : %d entrees, %u o utilises sur %u, en %lu ms",
                n, (unsigned)g_used, (unsigned)g_total, (unsigned long)(millis() - t0));
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (g_usedStale) {
      if (!AudioEngine::silencePourLaFlash()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
      g_usedStale = false;
      countBlocks();
    }
  }
}

}  // namespace

void begin() {
  if (g_entries) return;
  g_entries = (Entry*)heap_caps_calloc(kMax, sizeof(Entry), MALLOC_CAP_SPIRAM);
  if (!g_entries) { NIDMI_WEB_LOG("[storage] index impossible : pas de PSRAM"); return; }
  if (const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                         Stockage::PARTITION))
    g_total = p->size;
  // Core 1, under everything that plays (audio 11, loop 10, web 9, guard 5).
  xTaskCreatePinnedToCore(task, "storage-index", 4096, nullptr, 1, &g_task, 1);
}

bool ready() { return g_ready; }

bool waitReady(uint32_t ms) {
  const uint32_t t0 = millis();
  while (!g_ready && g_entries && millis() - t0 < ms) vTaskDelay(pdMS_TO_TICKS(10));
  return g_ready;
}

void fileWritten(const char* path, size_t size) {
  { Lock l; putParents(path); put(path, size, false); }
  wake();
}

void dirMade(const char* path) {
  { Lock l; putParents(path); put(path, 0, true); }
}

void removed(const char* path) {
  if (!path) return;
  {
    Lock l;
    const size_t lf = strlen(path);
    for (int i = 0; i < g_n;) {
      if (under(g_entries[i].path, path, lf)) { g_entries[i] = g_entries[--g_n]; continue; }
      i++;
    }
  }
  wake();
}

void renamed(const char* from, const char* to) {
  if (!from || !to) return;
  {
    Lock l;
    const size_t lf = strlen(from);
    for (int i = 0; i < g_n; i++) {
      Entry& e = g_entries[i];
      if (!under(e.path, from, lf)) continue;
      char p[kPathMax];
      snprintf(p, sizeof p, "%s%s", to, e.path + lf);
      strlcpy(e.path, p, kPathMax);
    }
    putParents(to);
  }
  wake();
}

void visit(const char* folder, bool recursive,
           void (*fn)(const char* path, size_t size, bool dir, void* ctx), void* ctx) {
  const bool root = !folder || !strcmp(folder, "/") || !*folder;
  const size_t lf = root ? 0 : strlen(folder);
  Lock l;
  for (int i = 0; i < g_n; i++) {
    const Entry& e = g_entries[i];
    if (!root && !(under(e.path, folder, lf) && e.path[lf] == '/')) continue;
    if (!recursive && strchr(e.path + lf + 1, '/')) continue;
    fn(e.path, e.size, e.dir, ctx);
  }
}

bool fileSize(const char* path, size_t& size) {
  Lock l;
  const int i = path ? find(path) : -1;
  if (i < 0 || g_entries[i].dir) return false;
  size = g_entries[i].size;
  return true;
}

Stats stats() {
  Stats s;
  Lock l;
  for (int i = 0; i < g_n; i++) {
    const Entry& e = g_entries[i];
    if (e.dir) continue;
    s.files++;
    s.content += e.size;
    const size_t n = strlen(e.path);
    if (n > 4 && !strcmp(e.path + n - 4, ".nms")) s.scripts++;
  }
  s.used = g_used;
  s.total = g_total;
  s.measured = g_ready;
  return s;
}

}  // namespace StorageIndex
