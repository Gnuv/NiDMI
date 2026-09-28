#pragma once
#include <stdint.h>
#include <atomic>

/* ── OU PASSE LE TEMPS DE LA TACHE MIDI (MESURES §170) ─────────────────────
 * Un message MIDI entrant attend que la tache MIDI ait fini ce qu'elle fait :
 * son tour de 10 ms (l'horloge des scripts, les differes, les multiplexeurs,
 * les composants), ou le message d'avant. En lecture sur une cue a deux
 * scripts, elle occupait 16 a 18 % du coeur 0 sans aucun MIDI entrant, et des
 * notes attendaient jusqu'a 8 ms — sans qu'on sache ou.
 *
 * Chaque etape se chronometre ici : pire duree, duree moyenne, nombre de
 * passages. Un chrono coute deux lectures d'horloge et trois operations
 * atomiques ; les etapes durent des centaines de microsecondes. Publie par
 * /api/diag/gigue (« chronos »), remis a zero avec elle. */
namespace Chronos {

struct Chrono {
  std::atomic<uint32_t> max{0}, cumul{0}, n{0};
  void noter(uint32_t us) {
    n.fetch_add(1, std::memory_order_relaxed);
    cumul.fetch_add(us, std::memory_order_relaxed);
    if (us > max.load(std::memory_order_relaxed)) max.store(us, std::memory_order_relaxed);
  }
  void raz() { max = 0; cumul = 0; n = 0; }
};

// Le tour de la tache MIDI, etape par etape.
extern Chrono horloge;      // g_midiRouter.battreHorloge : metro(), loadbang()
extern Chrono differes;     // MappingEngine::battreDifferes : del(), lag()...
extern Chrono mux;          // les multiplexeurs
extern Chrono composants;   // les broches (lecture + script)
// Dans un script qui bat (battre) : ce qu'il calcule, et ce qu'il fait sortir.
extern Chrono execution;    // l'interpreteur
extern Chrono emission;     // ses sorties : MIDI (USB, RTP), OSC
extern Chrono impression;   // print(), graph() : journal, port serie, WebSocket
// Un message MIDI USB entrant : sa chaine de scripts, puis le son et les LEDs.
extern Chrono traitementUsb;
// Le sequenceur (§172) : le retard d'une cue minutee sur son echeance, et ce
// que coute l'application d'une cue (scripts, echantillons, moteur).
extern Chrono retardCue;
extern Chrono applicationCue;

void razTout();

}  // namespace Chronos
