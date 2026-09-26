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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

// Ferme l'I2S et arrete la tache audio : les broches BCK/LRCK/DIN sont rendues.
// A appeler quand le DAC cesse d'etre declare — setEngine(-1) ne suffit pas, il
// ne fait que deselectionner le moteur (MESURES.md §51).
bool arreter();

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
// (« un échec ici ne doit pas pouvoir coûter l'OTA »). Il compte les PLANTAGES
// CONSECUTIFS (panique, chien de garde) après une restauration, en mémoire RTC :
// un allumage, un redémarrage voulu, un OTA le remettent à zéro, et il n'écrit
// jamais la flash (MESURES §157). Au bout de TENTATIVES_MAX plantages de suite,
// la restauration se coupe : la carte démarre nue, joignable, flashable. Une
// action humaine explicite (choix d'un moteur dans l'UI) la réarme.
constexpr uint8_t TENTATIVES_MAX = 3;

// Appelée quand l'interface est servie (page, ou page de secours) : preuve de
// vie, qui remet le compteur à zéro. Mémoire RTC : appelable d'async_tcp.
void validerConfigBoot();

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

/* ── SYNTHÈSE LOURDE : DÉSACTIVÉE SUR UNE CARTE SEULE ──────────────────────
 *
 * Décision prise sur mesure (MESURES.md §122 à §126) : faire cohabiter le
 * serveur web et Plaits sur ce 8 Mo n'est PAS robuste. Plaits résident laisse
 * 7 668 o de bloc contigu ; une page déjà ouverte tient très bien (101 allers-
 * retours, zéro échec), mais tenter de la RECHARGER coince la carte une à deux
 * minutes — « elle répond, elle ne sert plus » (§15).
 *
 * La carte seule fait donc : capteurs, MIDI, OSC, séquenceur, scripts .nms et
 * ÉCHANTILLONS. L'échantillonneur, lui, cohabite sans problème — son PCM vit en
 * PSRAM et il ne coûte que 1 536 o de bloc contigu (§123).
 *
 * LA SYNTHÈSE COMMENCE À DEUX CARTES. Le build d'un worker de ferme définira
 * NIDMI_SYNTH_LOURDE ; il n'a pas de serveur web à nourrir, donc pas ce conflit.
 *
 * Ce n'est pas une restriction « au cas où » : c'est ce que la mesure impose. */
#ifndef NIDMI_SYNTH_LOURDE
#define NIDMI_SYNTH_LOURDE 0
#endif
// true si cette image accepte les moteurs de synthèse (0..23).
bool syntheseLourdeDisponible();

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
/* `drone` : 0 = GACHETTE (defaut), 1 = CONTINU.
 *
 * Sur Plaits, « trigger patched » veut dire qu'une gachette est branchee : toute
 * note devient une attaque suivie d'une decroissance de LPG, et RIEN NE TIENT
 * JAMAIS — ni pendant une pause, ni ailleurs. Le drapeau etait pose en dur a
 * l'allocation, avec pour seul commentaire « sans ca les moteurs jouent en
 * continu ». C'est precisement ce qu'on veut pouvoir choisir : debranchee, la
 * gachette laisse le moteur sonner en continu, ce qui est un bourdon — un mode
 * musical a part entiere, et accessoirement le seul son qui permette d'entendre
 * qu'une pause ne coupe pas le son (MESURES.md §121.3).
 *
 * Flottant comme les autres pour que Params reste uniforme et voyage par les
 * memes chemins (cue, /api/audio/params) ; lu comme un booleen au seuil 0,5. */
struct Params { float harmonics, timbre, morph, decay, lpgColour, drone; };

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
/* `trig-wav` : demarre a l'ARRIVEE SUR UNE CUE, a la hauteur du fichier, et
 * boucle si la cue le demande. Ni clavier ni transposition — c'est son
 * comportement d'origine cote navigateur (un BufferSource avec `loop`). */
/* Declenche UN echantillon NOMME sur une voix libre (la plus ancienne sinon).
 * `demiTons` = 0 : a la hauteur du fichier — le cas de trig-wav sur cue.
 * HUIT VOIX : mesure a ~380 cycles/echantillon la voix, sur 5 000 de budget,
 * soit 62 % a huit. Deux pistes instrument avec deux sons en meme temps sont
 * donc possibles — elles ne l'etaient pas, le lecteur etant monophonique et le
 * magasin ne tenant qu'un echantillon. */
bool declencherEchantillon(const char* nom, bool boucle = false,
                           float gain = 1.0f, float demiTons = 0.0f);
void arreterEchantillonNomme(const char* nom);   // ce que CETTE cue avait lance
void arreterEchantillon();                       // toutes les voix
/* QUI DECLENCHE. `true` (defaut) : la cue, a la hauteur du fichier — l'original.
 * `false` : le clavier, transpose par la note. Les deux marchent ; c'est le bloc
 * qui tranche, par son parametre `oncue`. */
void fixerDeclenchementSurCue(bool surCue);
bool declenchementSurCue();
const char* samplerNom();
void setParams(const Params& p);
Params params();

/* ── VOLUME DE SORTIE, 0..1 ────────────────────────────────────────────────
 * La carte n'en avait aucun : `Params` ne portait que les cinq continus de
 * Plaits. Le volume de piste vivait donc uniquement dans le navigateur, et une
 * composition qui baisse un fader sonnait a plein sur la carte — autrement dit
 * le fader n'etait qu'un reglage de repetition (MESURES §96.4).
 *
 * Applique sur l'entrelace juste avant la porte de silence, donc AVANT la
 * mesure de crete : « niveauCrete » continue de dire ce qui part reellement
 * vers le DAC, ce qui est tout l'objet de cette mesure.
 * A 1.0 exactement, la boucle de multiplication est sautee — le cas courant ne
 * paie rien. */
void  setVolume(float v);       // borne a [0,1]
float volume();

/* DEPUIS COMBIEN DE TEMPS LA SORTIE EST MUETTE (MESURES §155), en ms :
 * depuis le dernier bloc dont la crete depassait ≈ −60 dBFS. UINT32_MAX quand
 * le moteur ne tourne pas. Leger — un millis() et une soustraction : se lit a
 * chaque tour de boucle. Sert a ne faire, pendant le spectacle, ce qui arrete
 * les coeurs (une ecriture en flash qui efface une page : 43 ms) que la ou
 * rien ne s'entend. */
uint32_t silenceDepuisMs();

/* LE MOMENT D'ECRIRE EN FLASH (MESURES §155, §156). Une ecriture qui efface une
 * page arrete les deux coeurs ~45 ms, plus que les 30 ms d'avance du DMA : ce
 * qui peut attendre attend donc que la sortie soit muette depuis 0,5 s. */
constexpr uint32_t SILENCE_POUR_LA_FLASH_MS = 500;
bool silencePourLaFlash();

/* Le pire aller-retour d'un bloc audio depuis le dernier appel, en µs (et
 * remise a zero). Un bloc dure 2,5 ms ; le DMA en garde 30 d'avance. Sert a
 * mesurer ce qu'un geste coute a l'audio sans attendre qu'il decroche.
 * `renduUs` : la part de ce bloc passee a le rendre — le reste est l'attente
 * du DMA dans i2s.write(). */
uint32_t pireBlocEtRaz(uint32_t* renduUs = nullptr);

/* Les deux sondes du decrochage, depuis le dernier appel (et remise a zero) :
 * le plus grand ecart entre deux fins de tampon DMA, leur nombre, et le plus
 * grand ecart entre deux tics du coeur 1. Tics reguliers mais fins de tampon
 * absentes : le DMA s'est arrete. Les deux absents : le coeur 1 ne recevait
 * plus ses interruptions (MESURES §161). */
void sondesEtRaz(uint32_t& pireEcartEofUs, uint32_t& finsDeTampon, uint32_t& pireEcartTicUs);

/* La plus longue suspension de l'ordonnanceur du coeur 1 depuis le dernier
 * appel (et remise a zero), l'appel de la boucle en cours quand elle a
 * commence, et la tache en cours quand elle a fini. */
void sondeSuspensionEtRaz(uint32_t& pireUs, const char*& section, TaskHandle_t& tache);

/* Le pire retard de la garde d'election depuis le dernier appel (et remise a
 * zero), en µs. En retard elle aussi pendant un decrochage : c'est tout le
 * coeur 1 qui n'elisait plus ; a l'heure : c'est le reveil de l'audio. */
uint32_t gardeRetardEtRaz();

/* L'ecrivain ANNONCE son ecriture — faite en silence, par la regle ci-dessus.
 * Les blocs rendus en retard pendant qu'elle a lieu sont comptes deux fois :
 * dans `sousAlimentations` (ils ont eu lieu) et dans `retardsEcritures` (rien
 * ne s'est entendu). Le voyant « decrochages » ne compte que la difference. */
void ecritureFlashDebut();
void ecritureFlashFin();

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
  uint32_t retardsEcritures;   // dont : pendant une ecriture flash volontaire, en silence
  int      moteur;             // -1 sinus, 0..15 Plaits
  bool     plaitsPret;
  uint32_t plaitsOctets;       // ce que Plaits a réellement pris sur le tas
  uint32_t cyclesParEch;       // coût mesuré du rendu, en cycles/échantillon
  uint32_t heapMiniJamais;     // plancher du tas depuis le boot — LE chiffre qui
                               // dit si l'on est mort d'épuisement mémoire
  int      causeReset;         // esp_reset_reason() : panique ? chien de garde ?
  const char* causeResetTexte;
  uint32_t seuilBascule;       // plus gros bloc requis pour basculer à chaud
  uint8_t  bootEssais;         // plantages consécutifs après restauration (RTC)
  bool     bootCoupe;          // restauration coupée : garde-fou atteint
  bool     silence;            // porte de silence fermée (STOP)
  uint16_t niveau;             // crête réellement envoyée au DAC (0 = muet)
  uint8_t  derniereNote;       // dernière note jouée (255 = aucune)
};
Metriques metriques();

/* Pourquoi ce demarrage — le texte de /api/audio/status « reset_reason ». */
const char* causeResetTexte();

/* Les trois compteurs du son, et rien d'autre : metriques() parcourt le tas.
 * Pour le journal qui survit au redemarrage, a chaque quart de seconde (§162). */
void compteursSon(uint32_t& blocs, uint32_t& retards, uint32_t& retardsEcritures);

}  // namespace AudioEngine
