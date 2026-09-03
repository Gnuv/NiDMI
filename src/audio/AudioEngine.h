/*
 * AudioEngine — sortie audio I2S du boîtier NiDMI.
 *
 * Preuve de concept : une XIAO ESP32-S3 tient l'UI web, le MIDI ET le son.
 *
 * Choix structurants (voir hardware/bench/MESURES.md du dépôt NiDMI) :
 *
 *  - INITIALISATION PARESSEUSE. Rien au boot : le moteur démarre à la première
 *    note. Un plantage audio ne doit jamais coûter l'OTA, qui est le seul
 *    chemin de flash prouvé sur cette carte (pas de CDC en variante usbmidi-on).
 *    Bénéfice second : le diagnostic touch du boot (NiDMI.cpp, touchRead(1) =
 *    GPIO1 = BCK) est passé depuis longtemps quand l'I2S s'installe.
 *
 *  - TÂCHE SUR LE CŒUR 1, PRIORITÉ 11. Le cœur 0 porte la pile WiFi (prio ~23)
 *    et la pile TCP/IP (prio 18) — vérifié dans le sdkconfig du core Arduino :
 *    ESP_WIFI_TASK_PINNED_TO_CORE_0, LWIP_TCPIP_TASK_AFFINITY_CPU0,
 *    LWIP_TCPIP_TASK_PRIO=18. Une tâche audio sur le cœur 0 serait préemptée à
 *    chaque paquet — et c'est cette carte qui sert l'app en HTTP. Le cœur 1 ne
 *    porte que loop() (prio 1) et async_tcp (prio 10) : à 11, l'audio y passe
 *    devant. Le téléchargement de l'UI ralentit un peu pendant que le son
 *    joue ; c'est le bon arbitrage, l'audio est temps réel, le HTTP non.
 *
 *  - FILE FreeRTOS pour les notes. noteOn/noteOff sont appelés depuis MidiTask
 *    (cœur 0) et depuis les rappels RTP : on ne touche jamais l'état des voix
 *    hors de la tâche audio.
 */
#pragma once
#include <Arduino.h>   // String, pour les messages d'erreur du sampler
#include <stdint.h>
#include <stddef.h>

namespace AudioEngine {

// Câblage : MAQUETTE_V1.md §2.0 (partie DAC).
constexpr int PIN_BCLK = 1;   // D0
constexpr int PIN_LRCK = 2;   // D1
constexpr int PIN_DIN  = 3;   // D2
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr int      VOIX        = 4;

// Démarre l'I2S et la tâche si ce n'est pas déjà fait. Sûr à appeler souvent.
// Retourne false si l'I2S a refusé de s'ouvrir (le reste du boîtier continue).
bool ensureStarted();

bool isStarted();

// ── Restauration au boot ───────────────────────────────────────────────────
// MESURES.md §11 (corrigé le 2026-09-03) : c'est l'ORDRE d'allocation qui
// décide, pas la quantité. Allouer un engine sur un tas VIERGE laisse un bloc
// contigu suffisant à AsyncTCP — mesuré, la page sort en 0,854 s. Le faire
// APRÈS un chargement de page (32 requêtes en 6 connexions, plancher du tas à
// 1 348 o) prend le dernier gros bloc et le serveur ne se relève plus : la
// carte répond au ping et ne sert plus rien.
//
// Conséquence structurante : les engines se chargent AU BOOT, une fois, et ne
// changent plus de la session. Une cue fait varier les PARAMÈTRES d'un process
// résident (y compris les 24 moteurs de Plaits, qui ne réallouent rien), jamais
// le process lui-même. Changer de process demande un redémarrage.
//
// À appeler UNE FOIS à la fin de nidmi_setup() : après serverCore.begin(), donc
// WiFi et serveur déjà installés, et avant qu'aucune page n'ait été servie.
void restaurerAuBoot();

// Garde-fou, qui remplace la protection qu'offrait l'initialisation paresseuse
// (« un échec ici ne doit pas pouvoir coûter l'OTA »). Le compteur de tentatives
// est écrit en NVS AVANT d'allouer, et remis à zéro quand l'interface a
// réellement été servie. Au bout de TENTATIVES_MAX boots sans cette preuve de
// vie, la restauration se coupe : la carte démarre nue, joignable, flashable.
// Une action humaine explicite (choix d'un moteur dans l'UI) la réarme.
constexpr uint8_t TENTATIVES_MAX = 3;

// Appelée par la route "/" : ne fait que lever un drapeau (contexte async_tcp,
// on n'y écrit pas la flash).
void validerConfigBoot();

// Appelée par nidmi_loop() : c'est elle qui écrit la NVS, hors du contexte async.
void entretienBoot();

// Thread-safe. velocity 0 sur noteOn = noteOff (convention MIDI).
void noteOn(uint8_t note, uint8_t velocity);
void noteOff(uint8_t note);

// Bip de test : fréquence en Hz, durée en ms. 0 Hz = silence immédiat.
void testTone(float hz, uint32_t ms);

// Coupe le son IMMÉDIATEMENT : porte de silence sur la sortie I2S. Plaits rend
// en continu et ignore le note-off (son LPG gère l'extinction) — donc un
// simple note-off ne le fait pas taire, et le moteur « dronait » sans jamais
// s'arrêter au STOP du transport. La porte zère la sortie jusqu'à la prochaine
// note, qui la rouvre (jeu live au clavier après un STOP). Voir le .cpp.
void couperSon();

// PLAY : ouvre la porte. Le moteur reste charge — « silence, mais moteur prêt ».
void ouvrirSon();

// Indices des moteurs Plaits SUBSTITUÉS dans cette image (liste "2,3,4,..."),
// chaîne vide si l'image est complète. Sous -DPLAITS_LEGER, sept emplacements
// jouent virtual-analog à la place de leur moteur d'origine : les indices sont
// préservés (compatibilité navigateur ↔ carte) mais l'étiquette ment. L'UI a
// besoin de le savoir pour ne pas proposer un son qu'elle n'obtiendra pas.
const char* moteursSubstitues();

// Moteur : -1 = sinus interne (toujours disponible), 0..23 = moteur Plaits.
// 24 moteurs : les 8 de engine2/ puis les 16 classiques — même plage que le
// moteur WASM du navigateur (engines/core/plaits/web/index.js), pour qu'un même
// bloc de composition sonne pareil des deux côtés.
// Le passage à Plaits alloue paresseusement ses ~24 ko sur le TAS INTERNE — si
// l'allocation échoue, on reste au sinus et on le dit. Le son ne doit jamais
// pouvoir emporter le reste du boîtier.
// persister : écrire le choix en NVS. FAUX PAR DÉFAUT, et ce n'est pas un
// détail — une écriture NVS est une écriture FLASH, et une opération flash
// bloque le cache d'instructions, donc la tâche audio (mesuré : 1,8 % de blocs
// en retard, MESURES.md §13). Une cue qui change de moteur en performance ne
// doit donc RIEN écrire. Seule une action humaine explicite persiste.
// GARDE D'ALLOCATION À CHAUD. Prendre 16 ko d'un seul tenant sur un tas déjà
// haché par un chargement de page prend le dernier gros bloc, et AsyncTCP ne
// s'en relève pas : la carte répond au ping sans plus servir de HTTP (constaté
// le 2026-09-03). setEngine() refuse donc d'allouer sous le seuil — mais
// mémorise le choix, qui sera chargé au prochain boot sur un tas vierge.
// Deux cas ne sont JAMAIS refusés, parce qu'ils n'allouent rien :
//   - Plaits déjà résident : changer parmi ses 24 moteurs écrit un entier ;
//   - moteur = -1 : libère.
// C'est ce qui permet à une cue de piloter le son sans jamais risquer la carte.
enum class Bascule : uint8_t { Appliquee, Armee, Echec };
Bascule derniereBascule();

bool setEngine(int moteur, bool persister = false);
// engine = -1 LIBÈRE Plaits (et ne fait pas que le désélectionner) : sans ça la
// carte ne peut plus servir sa propre interface. Voir le .cpp.
void libererPlaits();
int  engine();

// Les cinq continus de Plaits, 0..1 — mêmes identifiants et mêmes plages que le
// moteur web. C'est ce qui permet à une cue de piloter indifféremment le WASM
// du navigateur ou le DSP de la carte.
struct Params { float harmonics, timbre, morph, decay, lpgColour; };

// Lecteur d'échantillons — l'équivalent embarqué de trig-wav. Bien moins cher
// que Plaits en RAM interne (quelques centaines d'octets contre 26 632), parce
// que l'échantillon vit en PSRAM : il cohabite donc avec le service de
// l'interface sans le dégrader. Voir SampleStore.
bool setSampler(const char* nom, String& raison, bool persister = false);
// Même règle que setEngine : on n'écrit en NVS que sur action explicite.
// setEngine() appelle cette fonction en interne quand on quitte le mode
// échantillon — y compris depuis le chemin des cues. Sans le paramètre, une cue
// effaçait le choix mémorisé (constaté au test de redémarrage).
void arreterSampler(bool persister = false);
bool samplerActif();
const char* samplerNom();
void setParams(const Params& p);
Params params();

// Métrologie — répond à la question §12.7 de CONVERGENCE_NIDMI.md : combien de
// tas reste-t-il réellement une fois WiFi + serveur async + app embarquée en
// place ? Le plus gros bloc contigu compte autant que le total : Plaits demande
// 16 ko d'un seul tenant.
struct Metriques {
  bool     demarre;
  uint32_t heapAvantInit;      // 0 tant que le moteur n'a jamais démarré
  uint32_t heapApresInit;
  uint32_t heapLibre;          // interne, à l'instant de l'appel
  uint32_t heapPlusGrosBloc;   // interne contigu — le chiffre qui décide de Plaits
  uint32_t psramLibre;
  uint32_t sampleRateReel;     // ce que l'I2S a vraiment obtenu (pas d'APLL sur S3)
  uint32_t blocsRendus;
  uint32_t sousAlimentations;  // blocs rendus en retard (indicateur de craquement)
  int      moteur;             // -1 sinus, 0..15 Plaits
  bool     plaitsPret;
  uint32_t plaitsOctets;       // ce que Plaits a réellement pris sur le tas
  uint32_t cyclesParEch;       // coût mesuré du rendu, en cycles/échantillon
  uint32_t heapMiniJamais;     // plancher du tas depuis le boot — LE chiffre qui
                               // dit si l'on est mort d'épuisement mémoire
  int      causeReset;         // esp_reset_reason() : panique ? chien de garde ?
  const char* causeResetTexte;
  uint32_t seuilBascule;       // plus gros bloc requis pour basculer à chaud
  uint8_t  bootEssais;         // boots consécutifs sans interface servie
  bool     bootCoupe;          // restauration coupée : garde-fou atteint
  bool     silence;            // porte de silence fermée (STOP)
  uint16_t niveau;             // crête réellement envoyée au DAC (0 = muet)
  uint8_t  derniereNote;       // dernière note jouée (255 = aucune)
};
Metriques metriques();

}  // namespace AudioEngine
