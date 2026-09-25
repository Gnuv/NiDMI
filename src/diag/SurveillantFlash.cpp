#include "SurveillantFlash.h"
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_flash.h>
#include <esp_private/esp_clk.h>

namespace SurveillantFlash {
namespace {

// Tout ce que touchent les crochets est en RAM interne : la fin d'une
// operation s'execute cache coupe.
DRAM_ATTR esp_flash_os_functions_t g_crochets;
DRAM_ATTR esp_err_t (*g_debutOrig)(void*) = nullptr;
DRAM_ATTR esp_err_t (*g_finOrig)(void*) = nullptr;
DRAM_ATTR uint32_t g_cyclesParUs = 240;
DRAM_ATTR volatile uint32_t g_debutCycles = 0;
DRAM_ATTR volatile uint32_t g_n = 0;
DRAM_ATTR volatile uint32_t g_totalCycles = 0;   // cycles bruts : pas de division
DRAM_ATTR volatile uint32_t g_pireCycles = 0;    // cache coupe (voir releverEtRaz)
DRAM_ATTR TaskHandle_t g_tacheEnCours = nullptr;
DRAM_ATTR TaskHandle_t g_pireTache = nullptr;
// L'ATTENTE avant l'operation : le temps que l'autre coeur se gare (ipc).
// La tache qui attend s'est elevee a la priorite maximale et tourne en rond :
// rien d'autre ne passe sur son coeur, l'audio compris.
DRAM_ATTR volatile uint32_t g_pireAttenteCycles = 0;
DRAM_ATTR TaskHandle_t g_pireAttenteTache = nullptr;

/* Le temps au compteur de cycles : une lecture de registre, sure cache coupe
 * (esp_timer n'est pas garanti en IRAM). Debut et fin d'une operation
 * s'executent sur le meme coeur, interruptions retenues : pas de migration. */
IRAM_ATTR esp_err_t debut(void* arg) {
  // La tache se lit AVANT : cache encore la. Le temps, APRES : c'est alors
  // qu'on tient la flash (une autre operation a pu nous faire attendre).
  const TaskHandle_t t = xTaskGetCurrentTaskHandle();
  const uint32_t avant = (uint32_t)esp_cpu_get_cycle_count();
  const esp_err_t r = g_debutOrig(arg);
  g_tacheEnCours = t;
  g_debutCycles = (uint32_t)esp_cpu_get_cycle_count();
  const uint32_t attente = g_debutCycles - avant;
  if (attente > g_pireAttenteCycles) { g_pireAttenteCycles = attente; g_pireAttenteTache = t; }
  return r;
}

IRAM_ATTR esp_err_t fin(void* arg) {
  const uint32_t d = (uint32_t)esp_cpu_get_cycle_count() - g_debutCycles;
  g_n = g_n + 1;
  g_totalCycles = g_totalCycles + d;
  if (d > g_pireCycles) { g_pireCycles = d; g_pireTache = g_tacheEnCours; }
  return g_finOrig(arg);
}

}  // namespace

void installer() {
  esp_flash_t* c = esp_flash_default_chip;
  if (!c || !c->os_func || g_debutOrig) return;
  if (!c->os_func->start || !c->os_func->end) return;
  const int mhz = esp_clk_cpu_freq() / 1000000;
  if (mhz > 0) g_cyclesParUs = (uint32_t)mhz;
  g_crochets = *c->os_func;
  g_debutOrig = c->os_func->start;
  g_finOrig = c->os_func->end;
  g_crochets.start = debut;
  g_crochets.end = fin;
  c->os_func = &g_crochets;
}

Releve releverEtRaz() {
  Releve r;
  r.operations = g_n;
  r.totalUs = g_totalCycles / g_cyclesParUs;
  r.pireUs = g_pireCycles / g_cyclesParUs;
  r.pireTache = g_pireTache;
  r.pireAttenteUs = g_pireAttenteCycles / g_cyclesParUs;
  r.pireAttenteTache = g_pireAttenteTache;
  g_pireAttenteCycles = 0;
  g_pireAttenteTache = nullptr;
  g_n = 0;
  g_totalCycles = 0;
  g_pireCycles = 0;
  g_pireTache = nullptr;
  return r;
}

}  // namespace SurveillantFlash
