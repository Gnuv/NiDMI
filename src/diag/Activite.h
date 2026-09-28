#pragma once
#include <stdint.h>

/* ── L'ACTIVITE DES ENTREES (MESURES §167) ────────────────────────────────────
 * La LED du bouton I/O de l'app s'allume quand la CARTE recoit des donnees :
 * MIDI par l'USB ou le reseau (RTP), OSC, broches. Les entrees NOTENT un bit —
 * un OU atomique, sans verrou ni allocation, depuis n'importe quelle tache —
 * et loopTask PUBLIE un resume au plus toutes les 100 ms, seulement si un
 * onglet ecoute :
 *
 *     NIDMI_ACT:<sources>,<gpio 0-31>,<gpio 32-63>      (hexadecimal)
 *               [;<bloc>:<crete G>:<crete D>,…]          (decimal, millemes)
 *
 * La seconde partie nourrit les VU-METRES des pistes (MESURES §169) : la crete
 * de chaque bloc qui sonne depuis la trame precedente — les voix ne la
 * mesurent que si un onglet ecoute.
 * En headless, noter coute un OU, et rien n'est mesure ni publie. Avec un
 * onglet ouvert : au plus 10 petites trames par seconde, et seulement pendant
 * qu'il y a du trafic ou du son. Un resume, pas un flux.
 * Rien ici n'est sur le chemin d'un message MIDI au-dela du OU : la trame part
 * de loopTask, la tache la moins prioritaire, par la file de la WebSocket. */
namespace Activite {
  enum : uint32_t {
    MIDI_USB = 1u << 0,
    MIDI_RTP = 1u << 1,
    MIDI_BLE = 1u << 2,
    OSC      = 1u << 3,
    BROCHE   = 1u << 4,
  };
  void noter(uint32_t sources);
  void noterBroche(uint8_t gpio);        // pose aussi BROCHE
  void publier(uint32_t maintenantMs);   // loopTask seulement
}
