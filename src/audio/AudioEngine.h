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

// Thread-safe. velocity 0 sur noteOn = noteOff (convention MIDI).
void noteOn(uint8_t note, uint8_t velocity);
void noteOff(uint8_t note);

// Bip de test : fréquence en Hz, durée en ms. 0 Hz = silence immédiat.
void testTone(float hz, uint32_t ms);

// Moteur : -1 = sinus interne (toujours disponible), 0..23 = moteur Plaits.
// 24 moteurs : les 8 de engine2/ puis les 16 classiques — même plage que le
// moteur WASM du navigateur (engines/core/plaits/web/index.js), pour qu'un même
// bloc de composition sonne pareil des deux côtés.
// Le passage à Plaits alloue paresseusement ses ~24 ko sur le TAS INTERNE — si
// l'allocation échoue, on reste au sinus et on le dit. Le son ne doit jamais
// pouvoir emporter le reste du boîtier.
bool setEngine(int moteur);
int  engine();

// Les cinq continus de Plaits, 0..1 — mêmes identifiants et mêmes plages que le
// moteur web. C'est ce qui permet à une cue de piloter indifféremment le WASM
// du navigateur ou le DSP de la carte.
struct Params { float harmonics, timbre, morph, decay, lpgColour; };
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
};
Metriques metriques();

}  // namespace AudioEngine
