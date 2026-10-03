#include "Activite.h"

#include <atomic>
#include <stdio.h>

#include "../server/ServerCore.h"   // nidmi_ws_quelqu_un_ecoute, nidmi_ws_pousser
#include "../audio/AudioEngine.h"   // les cretes des voix, pour les vu-metres (§169)

/* Voir Activite.h. Trois mots de 32 bits : les OU 32 bits sont sans verrou
 * sur ce processeur (S32C1I), un mot de 64 bits ne l'aurait pas ete. */
namespace {
  std::atomic<uint32_t> g_sources{0};
  std::atomic<uint32_t> g_brochesBas{0};
  std::atomic<uint32_t> g_brochesHaut{0};
  uint32_t g_dernierePublication = 0;
  uint32_t g_genListesPubliee = 0;        // l'etat des play list deja annonce (§196)
  constexpr uint32_t PERIODE_MS = 100;    // 10 Hz : l'oeil suit un vu-metre
  constexpr uint8_t  BLOCS_VU   = 8;      // autant que de voix d'echantillon
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
  /* Les voix ne mesurent leur crete que si quelqu'un regarde (§169). */
  const bool ecoute = nidmi_ws_quelqu_un_ecoute();
  AudioEngine::fixerMesureVu(ecoute);
  /* On VIDE a chaque periode, qu'on publie ou non : sans onglet, un bit pose
   * il y a une heure ne doit pas s'allumer a la prochaine ouverture. */
  const uint32_t s = g_sources.exchange(0, std::memory_order_relaxed);
  const uint32_t b = g_brochesBas.exchange(0, std::memory_order_relaxed);
  const uint32_t h = g_brochesHaut.exchange(0, std::memory_order_relaxed);
  uint32_t blocs[BLOCS_VU]; uint16_t cg[BLOCS_VU], cd[BLOCS_VU];
  const uint8_t n = AudioEngine::releverCretes(blocs, cg, cd, BLOCS_VU);
  /* LES PLAY LIST (§196) : le clip que chacune joue, a chaque changement —
   * un depart, une fin, un arret, une liste qui arrive ou part. L'etat ENTIER,
   * « NIDMI_LISTE:<bloc>:<clip>,… » (vide : aucune liste) : une trame perdue
   * se rattrape a la suivante. Jamais d'attente : si un ecrivain tient les
   * listes, on reessaie a la prochaine periode. */
  const uint32_t gl = AudioEngine::generationListes();
  if (ecoute && gl != g_genListesPubliee) {
    uint32_t lb[AudioEngine::LISTES_MAX]; uint8_t lc[AudioEngine::LISTES_MAX], ln[AudioEngine::LISTES_MAX];
    const int nl = AudioEngine::etatListes(lb, lc, ln, AudioEngine::LISTES_MAX, /*attendre=*/false);
    if (nl >= 0) {
      char liste[16 + AudioEngine::LISTES_MAX * 16];
      int k = snprintf(liste, sizeof liste, "NIDMI_LISTE:");
      for (int i = 0; i < nl && k > 0 && k < (int)sizeof liste - 16; i++)
        k += snprintf(liste + k, sizeof liste - k, "%s%lu:%u", i ? "," : "",
                      (unsigned long)lb[i], (unsigned)lc[i]);
      if (nidmi_ws_pousser(liste)) g_genListesPubliee = gl;
    }
  }
  if ((!s && !n) || !ecoute) return;
  /* UNE trame pour l'activite ET les vu-metres : autant de paquets de moins.
   * Les cretes en millemes de la pleine echelle (32 767), bornees a 1000 :
   * au-dela, la voix sature, la barre est pleine. */
  char trame[216];
  int l = snprintf(trame, sizeof trame, "NIDMI_ACT:%lx,%lx,%lx",
                   (unsigned long)s, (unsigned long)b, (unsigned long)h);
  for (uint8_t k = 0; k < n && l > 0 && l < (int)sizeof trame - 24; k++) {
    const unsigned g = (unsigned)((cg[k] >= 32767) ? 1000 : (cg[k] * 1000u) / 32767u);
    const unsigned d = (unsigned)((cd[k] >= 32767) ? 1000 : (cd[k] * 1000u) / 32767u);
    l += snprintf(trame + l, sizeof trame - l, "%c%lu:%u:%u",
                  k ? ',' : ';', (unsigned long)blocs[k], g, d);
  }
  nidmi_ws_pousser(trame);   // file pleine : la trame est jetee, sans attendre
}

}  // namespace Activite
