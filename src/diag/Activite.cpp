#include "Activite.h"

#include <atomic>
#include <stdio.h>

#include "../server/ServerCore.h"   // nidmi_ws_quelqu_un_ecoute, nidmi_ws_pousser

/* Voir Activite.h. Trois mots de 32 bits : les OU 32 bits sont sans verrou
 * sur ce processeur (S32C1I), un mot de 64 bits ne l'aurait pas ete. */
namespace {
  std::atomic<uint32_t> g_sources{0};
  std::atomic<uint32_t> g_brochesBas{0};
  std::atomic<uint32_t> g_brochesHaut{0};
  uint32_t g_dernierePublication = 0;
  constexpr uint32_t PERIODE_MS = 150;
}

namespace Activite {

void noter(uint32_t sources) {
  g_sources.fetch_or(sources, std::memory_order_relaxed);
}

void noterBroche(uint8_t gpio) {
  if (gpio < 32)      g_brochesBas.fetch_or(1u << gpio, std::memory_order_relaxed);
  else if (gpio < 64) g_brochesHaut.fetch_or(1u << (gpio - 32), std::memory_order_relaxed);
  g_sources.fetch_or(BROCHE, std::memory_order_relaxed);
}

void publier(uint32_t maintenant) {
  if ((uint32_t)(maintenant - g_dernierePublication) < PERIODE_MS) return;
  g_dernierePublication = maintenant;
  /* On VIDE a chaque periode, qu'on publie ou non : sans onglet, un bit pose
   * il y a une heure ne doit pas s'allumer a la prochaine ouverture. */
  const uint32_t s = g_sources.exchange(0, std::memory_order_relaxed);
  const uint32_t b = g_brochesBas.exchange(0, std::memory_order_relaxed);
  const uint32_t h = g_brochesHaut.exchange(0, std::memory_order_relaxed);
  if (!s || !nidmi_ws_quelqu_un_ecoute()) return;
  char trame[48];
  snprintf(trame, sizeof trame, "NIDMI_ACT:%lx,%lx,%lx",
           (unsigned long)s, (unsigned long)b, (unsigned long)h);
  nidmi_ws_pousser(trame);   // file pleine : la trame est jetee, sans attendre
}

}  // namespace Activite
