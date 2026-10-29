#include "HeapTrough.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>

namespace HeapTrough {
namespace {

RTC_NOINIT_ATTR uint32_t s_magic;
constexpr uint32_t kMagic = 0x48454150UL;   // "HEAP"

constexpr uint32_t kRunMs = 60000, kWindowMs = 50, kWindows = kRunMs / kWindowMs;
constexpr int kMarks = 32, kBlocks = 1536, kTasks = 48;

struct Block { uint32_t addr, size; uint8_t used; };
struct TaskRec { char name[configMAX_TASK_NAME_LEN]; uint32_t stack, tcb; };
struct Photo {
  uint32_t ms = 0, free = 0, largest = 0;
  int nBlocks = 0, nTasks = 0;
  bool truncated = false;
  Block blocks[kBlocks];
  TaskRec tasks[kTasks];
};
struct Data {
  uint32_t freeMin[kWindows], largestMin[kWindows];
  uint32_t nWindows = 0;
  struct { uint32_t ms; const char* what; } marks[kMarks];
  std::atomic<int> nMarks{0};
  Photo low, end;
  TaskStatus_t status[kTasks];
  bool done = false;
};
Data* g = nullptr;

void photo(Photo& p) {
  p.ms = millis();
  p.free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  p.largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  p.nBlocks = 0;
  p.truncated = false;
  heap_caps_walk(MALLOC_CAP_INTERNAL, [](walker_heap_into_t, walker_block_info_t b, void* u) -> bool {
    Photo* p = static_cast<Photo*>(u);
    if (p->nBlocks >= kBlocks) { p->truncated = true; return false; }
    p->blocks[p->nBlocks++] = { (uint32_t)(uintptr_t)b.ptr, (uint32_t)b.size, (uint8_t)(b.used ? 1 : 0) };
    return true;
  }, &p);
  uint32_t total = 0;
  const UBaseType_t n = uxTaskGetSystemState(g->status, kTasks, &total);
  p.nTasks = (int)n;
  for (UBaseType_t i = 0; i < n; i++) {
    strlcpy(p.tasks[i].name, g->status[i].pcTaskName, sizeof p.tasks[i].name);
    p.tasks[i].stack = (uint32_t)(uintptr_t)g->status[i].pxStackBase;
    p.tasks[i].tcb = (uint32_t)(uintptr_t)g->status[i].xHandle;
  }
}

/* Core 0, above storage (2) and sd-card (1), under everything that plays. The lowest point
 * is photographed when it falls under 16 KB and 256 bytes below the last photo. */
void task(void*) {
  const uint32_t t0 = millis();
  uint32_t lowest = UINT32_MAX;
  while (millis() - t0 < kRunMs) {
    const uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t l = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t w = millis() / kWindowMs;
    if (w < kWindows) {
      while (g->nWindows <= w) { g->freeMin[g->nWindows] = UINT32_MAX; g->largestMin[g->nWindows] = UINT32_MAX; g->nWindows++; }
      if (f < g->freeMin[w]) g->freeMin[w] = f;
      if (l < g->largestMin[w]) g->largestMin[w] = l;
    }
    if (f < 16384 && (lowest == UINT32_MAX || f + 256 <= lowest)) {
      lowest = f;
      photo(g->low);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  photo(g->end);
  g->done = true;
  vTaskDelete(nullptr);
}

}  // namespace

void arm(bool on) { s_magic = on ? kMagic : 0; }
bool armed() { return s_magic == kMagic; }

void begin() {
  if (!armed() || g) return;
  g = (Data*)heap_caps_calloc(1, sizeof(Data), MALLOC_CAP_SPIRAM);
  if (!g) return;
  new (&g->nMarks) std::atomic<int>(0);
  xTaskCreatePinnedToCore(task, "heaptrough", 3072, nullptr, 3, nullptr, 0);
}

void mark(const char* what) {
  if (!g || !what) return;
  for (int i = 0, n = g->nMarks.load(); i < n && i < kMarks; i++) if (g->marks[i].what == what) return;
  const int i = g->nMarks.fetch_add(1);
  if (i >= kMarks) return;
  g->marks[i].ms = millis();
  g->marks[i].what = what;
}

bool json(char*& text, size_t& len) {
  text = nullptr;
  len = 0;
  if (!g) return false;
  const size_t cap = 160 * 1024;
  char* t = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!t) return false;
  size_t n = 0;
  auto put = [&](const char* fmt, auto... a) {
    if (n >= cap - 1) return;
    const int k = snprintf(t + n, cap - n, fmt, a...);
    if (k > 0) n = (n + (size_t)k < cap - 1) ? n + (size_t)k : cap - 1;
  };
  put("{\"armed\":%s,\"done\":%s,\"min_ever\":%u,\"window_ms\":%u,\"free\":[", armed() ? "true" : "false",
      g->done ? "true" : "false", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)kWindowMs);
  for (uint32_t i = 0; i < g->nWindows; i++) put("%s%u", i ? "," : "", (unsigned)(g->freeMin[i] == UINT32_MAX ? 0 : g->freeMin[i]));
  put("],\"largest\":[");
  for (uint32_t i = 0; i < g->nWindows; i++) put("%s%u", i ? "," : "", (unsigned)(g->largestMin[i] == UINT32_MAX ? 0 : g->largestMin[i]));
  put("],\"marks\":[");
  const int nm = g->nMarks.load() < kMarks ? g->nMarks.load() : kMarks;
  for (int i = 0; i < nm; i++) put("%s[%u,\"%s\"]", i ? "," : "", (unsigned)g->marks[i].ms, g->marks[i].what);
  put("]");
  const Photo* ph[2] = { &g->low, &g->end };
  const char* nm2[2] = { "low", "end" };
  for (int k = 0; k < 2; k++) {
    const Photo& p = *ph[k];
    put(",\"%s\":{\"ms\":%u,\"free\":%u,\"largest\":%u,\"truncated\":%s,\"tasks\":[", nm2[k], (unsigned)p.ms,
        (unsigned)p.free, (unsigned)p.largest, p.truncated ? "true" : "false");
    for (int i = 0; i < p.nTasks; i++) put("%s[\"%s\",%u,%u]", i ? "," : "", p.tasks[i].name, (unsigned)p.tasks[i].stack, (unsigned)p.tasks[i].tcb);
    put("],\"blocks\":[");
    for (int i = 0; i < p.nBlocks; i++) put("%s[%u,%u,%u]", i ? "," : "", (unsigned)p.blocks[i].addr, (unsigned)p.blocks[i].size, (unsigned)p.blocks[i].used);
    put("]}");
  }
  put("}");
  text = t;
  len = n;
  return true;
}

}  // namespace HeapTrough
