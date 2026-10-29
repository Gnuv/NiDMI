#include "../config/Occupations.h"
#include "../managers/ComponentManager.h"
#include "../Globals.h"   // g_componentManager : les broches audio peuvent porter un composant
#include "AudioEngine.h"
#include "../server/ServerCallbacks.h"   // demarrage a vide, une fois

#include <Arduino.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>
#include <new>
#include <PlaitsDSP.h>
#include "SampleStore.h"
#include "SdStream.h"
#include "SfzStore.h"
#include "DspBench.h"
#include "SynthVoice.h"
#include "Effects.h"
#include "ProcessCosts.h"
#include "BlockLoads.h"
#include "../server/WebDebugConsole.h"
#include "../config/EcrituresDifferees.h"
#include "../midi/MidiRouter.h"   // le silence MIDI, pour le moment d'ecrire en flash (§170)
#include <Preferences.h>
#include <driver/i2s_std.h>
#include <esp_attr.h>
#include <esp_freertos_hooks.h>
#include <esp_timer.h>

namespace AudioEngine {
namespace {

constexpr size_t FRAMES     = 120;          // 2,5 ms = 10 x plaits::kBlockSize (12).
                                            // La classe I2S garde en
                                            // plus 6 × 240 trames de DMA (~30 ms),
                                            // c'est la marge contre la gigue WiFi.
constexpr float  AMPLITUDE  = 0.22f;        // ≈ −13 dBFS : audible, jamais brutal
constexpr float  RAMPE_MS   = 8.0f;         // anti-clic à l'attaque et à l'extinction

I2SClass    i2s;
TaskHandle_t tache      = nullptr;
QueueHandle_t evenements = nullptr;
/* Le stockage de la file des notes : pris une fois, garde d'un demarrage du
 * moteur a l'autre (voir « LA FILE DES NOTES VERS LE SON »). */
constexpr UBaseType_t CAPACITE_EVENEMENTS = 256;
uint8_t*      stockageEvenements = nullptr;
StaticQueue_t structureEvenements;
volatile uint32_t notesPerduesCompte = 0;
volatile bool demarre    = false;
/* Arret propre de la tache audio : elle rend la main d'elle-meme plutot que
 * d'etre tuee. La tuer pendant un i2s.write() laisserait le DMA a moitie servi
 * et le peripherique dans un etat qu'aucun i2s.end() ne rattrape. */
volatile bool arretDemande = false;
volatile bool tacheArretee = false;

uint32_t heapAvant = 0, heapApres = 0, srReel = SAMPLE_RATE;
volatile uint32_t nBlocs = 0, nRetards = 0;
// Les ecritures en flash VOLONTAIRES, faites en silence (§155, §156) : un compteur
// de sequence — impair pendant l'ecriture —, et les blocs en retard qui l'ont
// chevauchee. Ils sont dans nRetards (ils ont eu lieu) ET ici (rien ne s'est
// entendu) : le voyant ne compte que la difference.
volatile uint32_t ecrituresFlash = 0, nRetardsEcritures = 0;
// Le pire aller-retour d'un bloc (rendu + remise au DMA), en µs, depuis la
// derniere lecture : ce qu'un geste coute a l'audio, bien avant le seuil de
// retard (20 ms) — en silence comme en jeu.
volatile uint32_t pireBlocUs = 0;
// … et la part de ce pire bloc passee a le RENDRE (calcul, echantillons lus en
// PSRAM) ; le reste, c'est i2s.write() qui attend qu'un tampon DMA se libere.
// Un decrochage dans le rendu et un decrochage dans l'attente du DMA n'ont pas
// la meme cause (MESURES §161).
volatile uint32_t pireBlocRenduUs = 0;

/* LES DEUX SONDES DU DECROCHAGE (MESURES §161). Quand un bloc attend le DMA,
 * deux causes : le DMA a cesse d'envoyer (plus de fin de tampon), ou le coeur
 * de l'audio a cesse de recevoir ses interruptions (plus de tic non plus).
 * La fin de tampon vient de l'interruption du DMA (IRAM, CONFIG_I2S_ISR_
 * IRAM_SAFE) ; le tic, du crochet d'horloge du coeur 1. Chacune note son plus
 * grand ecart depuis le dernier releve. Tout en RAM interne. */
}  // namespace
}  // namespace AudioEngine
extern const char* volatile g_sectionEnCours;   // NiDMI.cpp : l'appel de la boucle en cours
namespace AudioEngine {
namespace {
DRAM_ATTR volatile uint32_t sondeDernierEof = 0, sondePireEof = 0, sondeNbEof = 0;
DRAM_ATTR volatile uint32_t sondeDernierTic = 0, sondePireTic = 0;

bool IRAM_ATTR sondeFinDeTampon(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  const uint32_t t = (uint32_t)esp_timer_get_time();
  if (sondeDernierEof) { const uint32_t e = t - sondeDernierEof; if (e > sondePireEof) sondePireEof = e; }
  sondeDernierEof = t;
  sondeNbEof = sondeNbEof + 1;
  return false;
}

/* LA TROISIEME SONDE : l'ordonnanceur du coeur 1 est-il SUSPENDU ? Une tache
 * reveillee y reste alors « prete » sans etre elue, et la tache en cours
 * continue seule — ce que l'autopsie a montre (audio « R » pendant 350 ms,
 * loopTask seule en marche). Le tic est appele meme ordonnanceur suspendu :
 * il note la plus longue suspension, et l'appel de la boucle en cours quand
 * elle a commence (un pointeur copie, jamais lu ici). */
DRAM_ATTR volatile uint32_t sondeSuspDebut = 0, sondePireSusp = 0;
DRAM_ATTR const char* volatile sondeSuspSection = nullptr;
DRAM_ATTR const char* volatile sondePireSuspSection = nullptr;
DRAM_ATTR volatile TaskHandle_t sondePireSuspTache = nullptr;

void IRAM_ATTR sondeTic() {
  const uint32_t t = (uint32_t)esp_timer_get_time();
  if (sondeDernierTic) { const uint32_t e = t - sondeDernierTic; if (e > sondePireTic) sondePireTic = e; }
  sondeDernierTic = t;
  if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED) {
    if (!sondeSuspDebut) { sondeSuspDebut = t; sondeSuspSection = g_sectionEnCours; }
  } else if (sondeSuspDebut) {
    const uint32_t d = t - sondeSuspDebut;
    if (d > sondePireSusp) {
      sondePireSusp = d;
      sondePireSuspSection = sondeSuspSection;
      sondePireSuspTache = xTaskGetCurrentTaskHandle();
    }
    sondeSuspDebut = 0;
  }
}

// Une note en attente, déposée par MidiTask ou par un rappel RTP.
// `sorte` CLIP (§196) : jouer le clip `clip` de la play list du bloc `bloc`,
// demande hors du clavier (l'ecoute de l'inspecteur) — par la meme file, pour
// que seule la tache audio touche aux voix des listes.
constexpr uint8_t SORTE_NOTE = 0, SORTE_CLIP = 1;
/* `canal`: the MIDI channel the note came on, 1..16 (0: none said — the test tone,
 * the app's socket). Each instrument listens to ITS track's channel (§210). */
/* `slots`: the cue slots it is for, a bit each (§212) — resolved when it is deposited. */
struct Evenement { uint8_t note; uint8_t velo; uint8_t sorte; uint8_t clip; uint32_t bloc; uint8_t canal;
                   uint8_t slots; uint8_t group; };   // velo 0 = extinction; group: §221
static inline bool _ecoute(uint8_t veut, uint8_t vient) { return !veut || !vient || veut == vient; }

struct Voix {
  bool  active   = false;
  uint8_t note   = 0;
  float phase    = 0.0f;
  float increment = 0.0f;
  float gain     = 0.0f;      // enveloppe courante
  float cible    = 0.0f;      // 0 = en extinction
} voix[VOIX];

// Bip de test : une voix hors clavier, pilotée par /api/audio/test.
volatile float    bipHz = 0.0f;
volatile uint32_t bipBlocsRestants = 0;
float             bipPhase = 0.0f;

int16_t entrelace[FRAMES * 2];
/* THE MIX, IN FLOAT (§210, §230): `mix` the slot's, `bus` the sum of one group whose FX are its
 * own, the groups in turn (Effects.h). Full scale = 1.0. */
float mixL[FRAMES], mixR[FRAMES], busL[FRAMES], busR[FRAMES], synthMono[FRAMES];
/* THE CAPTURE OF WHAT GOES TO THE DAC (§210, diagnostics): a PSRAM buffer the web
 * arms, the audio task fills block by block just before i2s.write — the samples
 * the DAC receives, after the volume and the gate. Read back as WAV. */
int16_t*          captureBuf = nullptr;
volatile uint32_t captureFrames = 0, captureFilled = 0;
volatile bool     captureArmed = false;

// ---- Plaits ---------------------------------------------------------------
// Tout est alloué à la demande sur le TAS INTERNE (jamais la PSRAM : trop lente
// pour de l'audio, cf. MESURES.md §4). Mesuré sur cette carte : 31,7 ko d'un
// seul tenant disponibles, contre ~24 ko nécessaires.
plaits::Voice*       plaitsVoix   = nullptr;
char*                plaitsMem    = nullptr;      // shared_buffer[16384] de plaits.cc
volatile float       gVolume      = 1.0f;         // 0..1, cf. AudioEngine.h
plaits::Patch        plaitsPatch;
plaits::Modulations  plaitsMod;
plaits::Voice::Frame plaitsTrames[FRAMES];
int                  moteurCourant = -1;          // -1 = sinus
volatile uint32_t    plaitsOctets  = 0;
volatile uint32_t    cyclesEch     = 0;
volatile bool        plaitsTrigger = false;
volatile bool    gSilence      = true;    // MUET au demarrage : rien ne sort tant que PLAY n'a pas ouvert
volatile uint16_t niveauCrete  = 0;      // crete du DERNIER bloc reellement envoye
volatile uint32_t dernierSonMs = 0;      // debut du dernier bloc AUDIBLE envoye (> SEUIL_AUDIBLE)
constexpr uint16_t SEUIL_AUDIBLE = 32;   // ≈ −60 dBFS : en dessous, un bloc est du silence
volatile uint8_t  derniereNote  = 255;   // 255 = aucune ; temoin du MIDI reellement joue

// Taille du scratch stmlib. Plaits remet l'allocateur a zero avant CHAQUE
// moteur (« All engines will share the same RAM space », voice.cpp) : le pool
// doit donc valoir la taille du moteur LE PLUS GOURMAND du registre, pas leur
// somme. Mesure par moteur, MESURES.md §17 :
//
//   registre complet : Particle 16 384 · String 15 520 · Speech 14 656  -> 16 384
//   image allegee    : les sept lourds sont substitues, le max devient
//                      Swarm a 512 o                                    ->  1 024
//                      (2x de marge sur une mesure exacte, pas estimee)
#ifdef PLAITS_LEGER
constexpr size_t POOL_PLAITS = 1024;
#else
constexpr size_t POOL_PLAITS = 16384;
#endif

/* Ce que l'utilisateur a demande, independamment de l'existence de Plaits : le
 * reglage arrive parfois AVANT l'allocation (une cue pose ses params sur un
 * moteur pas encore resident). */
bool droneVoulu = false;

bool plaitsAlloue() {
  if (plaitsVoix) return true;
  const uint32_t avant = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  plaitsMem = (char*)heap_caps_malloc(POOL_PLAITS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!plaitsMem) {
    Serial.printf("[audio] Plaits : %u o contigus indisponibles pour le scratch\n",
                  (unsigned)POOL_PLAITS);
    return false;
  }

  void* brut = heap_caps_malloc(sizeof(plaits::Voice), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!brut) {
    Serial.printf("[audio] Plaits : %u o pour Voice indisponibles\n", (unsigned)sizeof(plaits::Voice));
    heap_caps_free(plaitsMem); plaitsMem = nullptr;
    return false;
  }
  plaitsVoix = new (brut) plaits::Voice();

  stmlib::BufferAllocator allocateur(plaitsMem, POOL_PLAITS);
  plaitsVoix->Init(&allocateur);

  plaitsPatch.note = 48.0f;
  plaitsPatch.harmonics = 0.5f; plaitsPatch.timbre = 0.5f; plaitsPatch.morph = 0.5f;
  plaitsPatch.frequency_modulation_amount = 0.0f;
  plaitsPatch.timbre_modulation_amount    = 0.0f;
  plaitsPatch.morph_modulation_amount     = 0.0f;
  plaitsPatch.decay = 0.5f; plaitsPatch.lpg_colour = 0.5f; plaitsPatch.engine = 0;

  plaitsMod = plaits::Modulations{};
  /* On REPOSE le choix courant, on ne le force pas a « gachette ».
   * Cette ligne valait `= true` en dur : une reallocation de Plaits — un
   * changement de moteur, une cue — ramenait un bloc en bourdon a la gachette,
   * en silence. Un reglage qui se perd a l'allocation n'est pas un reglage. */
  plaitsMod.trigger_patched = !droneVoulu;

  plaitsOctets = avant - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  Serial.printf("[audio] Plaits alloue : %lu o (Voice %u + pool %u)%s\n",
                (unsigned long)plaitsOctets, (unsigned)sizeof(plaits::Voice),
                (unsigned)POOL_PLAITS,
#ifdef PLAITS_LEGER
                " — IMAGE ALLEGEE, 7 emplacements substitues");
#else
                "");
#endif
  return true;
}

// ---- lecteur d'échantillons ----------------------------------------------
// Monophonique, à la manière de play-sf : un déclenchement repart de zéro.
// La hauteur suit la note (do central = hauteur d'origine), par lecture à pas
// fractionnaire avec interpolation linéaire — quelques opérations par
// échantillon, sans commune mesure avec un moteur de synthèse.
/* ── POLYPHONIE ────────────────────────────────────────────────────────────
 * Le lecteur etait MONOPHONIQUE, et le magasin ne tenait qu'un echantillon :
 * deux pistes instrument avec deux sons en meme temps etaient donc impossibles
 * de deux facons a la fois. Signale par l'usager.
 *
 * COMBIEN DE VOIX ? Mesure, pas estimation (MESURES §131) :
 *     echantillon charge mais silencieux ......  36 cycles/echantillon
 *     une voix qui joue .......................  420
 *     donc UNE VOIX COUTE ~380 cycles, sur un budget de 5 000 a 240 MHz/48 kHz.
 *
 *     6 voix  -> 2 320 cycles, 46 %
 *     8 voix  -> 3 080 cycles, 62 %   <- retenu
 *    10 voix  -> 3 840 cycles, 77 %
 *
 * On s'arrete a 8 : il reste un tiers du budget pour le reste, et la regle du
 * projet est que l'audio ne cede jamais. Le vol de voix prend la PLUS ANCIENNE,
 * comme partout ailleurs. */
#ifndef VOIX_MAX
#define VOIX_MAX 8
#endif

/* THE CASE'S ENVELOPE (MESURES §211-212) — Delay, Attack to Sustain, Release: what a cue
 * applies to everything it plays, as the app's DASDR of a case — on its slot's OUTPUT. A
 * linear ramp toward `target`, after `wait` samples; `stop`: a release — the slot empties at 0. */
struct EnvCue { float now = 1.0f, target = 1.0f, step = 0.0f; uint32_t wait = 0; bool stop = false; };

struct VoixEch {
  bool     actif  = false;
  uint8_t  iEch   = 0;        // index dans SampleStore
  /* WHERE IT READS, AND HOW FAST: fixed point 32.32 — the frame in the high word, the fraction
   * in the low one (MESURES §236). They were doubles: the S3 computes only floats in hardware,
   * and the seven soft-float routines a sample took made a voice cost 505 cycles per sample, 10 %
   * of core 1. A 32-bit fraction is as fine as the double's over a sound's length. */
  uint64_t pos    = 0;
  uint64_t pas    = 1ull << 32;
  float    gain   = 0.0f;     // gain COURANT, qui glisse vers `cible`
  float    cible  = 0.0f;     // velo x gain du bloc (MESURES §165)
  float    velo   = 1.0f;     // part de la velocite : 1 sur cue
  uint32_t bloc   = 0;        // bloc de la composition qui l'a lancee, 0 = aucun
  bool     boucle = false;
  uint32_t age    = 0;        // ordre de declenchement, pour le vol de voix
  uint16_t picG   = 0;        // cretes depuis la derniere lecture (vu-metres, §169)
  uint16_t picD   = 0;
  /* UN CLIP D'UNE PLAY LIST (§196) : sa liste dans la banque active (-1 :
   * aucune), son rang, sa selection en trames (finT 0 : jusqu'au bout du
   * fichier) ; `fondu` > 0 : la voix se tait, de tant par echantillon. */
  int8_t   liste  = -1;
  uint8_t  clip   = 0;
  uint32_t debutT = 0;
  uint32_t finT   = 0;
  float    fondu  = 0.0f;
  /* A CLIP READ FROM THE SD CARD, AS A STREAM (SdStream.h): `head` (>= 0) says it comes
   * from its head — [headStart, headStart + headFrames), in PSRAM —, then from the buffer
   * of stream `stream` (-1: the head is enough, the clip is shorter). A PSRAM voice
   * (head < 0) reads the store as before. */
  int8_t          stream = -1;
  int8_t          head   = -1;
  const int16_t*  headPcm = nullptr;
  uint32_t        headStart = 0, headFrames = 0;
  /* THE CUE SLOT IT PLAYS IN (§212): its mix, its effects, its envelope. */
  uint8_t         slot = 0;
};
/* LE GAIN GLISSE (MESURES §165) : un pole d'environ 10 ms a 48 kHz. Un saut de
 * gain en plein son s'entend comme un claquement ; 63 % en 10 ms, le reste en
 * quelques dizaines, c'est sous l'oreille pour un geste de potentiometre. */
constexpr float LISSAGE_GAIN = 1.0f / 480.0f;
/* A rate (frames of the sound per output sample) in 32.32 fixed point — at a voice's start only. */
static inline uint64_t _fixedStep(double rate) { return (uint64_t)(rate * 4294967296.0 + 0.5); }
static inline uint64_t _fixedFrame(uint32_t frame) { return (uint64_t)frame << 32; }
/* LE PLAFOND D'UNE VOIX : +12 dB, celui des faders de l'app (VOL_GAIN_MAX,
 * 3,981). On bornait a 1 : un fader pousse au-dessus de 0 dB ne changeait rien
 * sur la carte (MESURES §166). Au-dela de 1, la somme peut saturer — comme
 * dans toute table de mixage numerique ; c'est le choix de qui pousse le
 * fader. `!(g >= 0)` attrape aussi NaN (« gain=nan » dans une requete). */
constexpr float GAIN_VOIX_MAX = 3.981f;
static inline float _borneGain(float g) {
  if (!(g >= 0.0f)) return 0.0f;
  return (g > GAIN_VOIX_MAX) ? GAIN_VOIX_MAX : g;
}
VoixEch  voixEch[VOIX_MAX];
uint32_t voixHorloge = 0;

/* ── THE TWO CUE SLOTS (MESURES §212) ──────────────────────────────────────
 * Gael: "the departure case and the arrival case must be active at the same time during the
 * fade; only two; a jump while a release is not over cuts the sound with a very brief
 * fade-out". A cue plays in a SLOT: its play-sf keyboard, its play lists (tagged with the
 * slot), its SFZ engine, its synth voice, its effects (Effects.h), its envelope — on the
 * slot's OUTPUT, after its effects, as the browser puts a case's envelope on its mix (§13):
 * a reverb tail falls with the Release.
 *
 *   FREE      nothing armed; its envelope neutral (what the inspector plays sounds in it);
 *   CURRENT   the cue that plays: Delay, Attack, Sustain;
 *   LEAVING   the cue that left, in its Release — still played by its MIDI;
 *   CUTTING   10 ms to silence (a Release of 0, a third jump, STOP);
 *   DRAINED   silent: the audio task stopped its voices and cleared its effects; the loop
 *             disarms the rest (synth voice, kit, scripts) and frees it.
 *
 * The cue (loopTask) posts a request per slot; the audio task applies it at the start of its
 * next block — before it adopts the cue's lists and before any of its sounds is rendered (the
 * audio task outranks the loop on core 1: what the loop posts lands between two blocks). The
 * loop configures a slot's instruments only while it is FREE, or after posting its arrival. */
enum : uint8_t { SLOT_FREE = 0, SLOT_CURRENT, SLOT_LEAVING, SLOT_CUTTING, SLOT_DRAINED };
enum : uint8_t { REQ_NONE = 0, REQ_ARRIVE, REQ_LEAVE, REQ_CUT };
constexpr uint8_t SLOT_AUCUN = 0xFF;          // a list whose slot was drained: it answers nothing
constexpr float SLOT_CUT_S = 0.010f;          // the brief fade-out of a cut
struct Clavier { char nom[48] = ""; uint32_t bloc = 0; float gain = 1.0f; uint8_t canal = 0; bool surCue = true;
                 uint8_t poly = KEYBOARD_POLY_DEFAULT; };   // its polyphony (§219)
struct CueSlot {
  volatile uint8_t  state = SLOT_FREE;
  EnvCue            env;                    // audio task
  float             cut = 1.0f;             // audio task: the brief fade-out, 1 then down to 0
  /* What the cue posts (loopTask), applied at the next block. */
  volatile uint8_t  req = REQ_NONE;
  volatile float    reqDelayS = 0.0f, reqAttackS = 0.0f, reqSustain = 1.0f, reqReleaseS = 0.0f;
  volatile uint32_t gen = 0;                // bumps when a cue arrives in it (STOP spares a newer one)
  /* Its instruments, written by the cue while the slot is free or just arrived. */
  Clavier           clavier;                // the play-sf keyboard (nom "": none)
  volatile int8_t   kit[KITS_PER_SLOT] = {-1, -1};   // its SfzStore engines, -1 none (§234: two kits)
  volatile uint8_t  kitCanal[KITS_PER_SLOT] = {};    // each kit's channel, its track's (0: all) — §210
  volatile int8_t   synth = -1;             // its SynthVoice voice, -1 none
  volatile int8_t   synth2 = -1;            // its second voice, the cue in two voices (§214), -1 none
  uint8_t           prochaine = 0;          // audio task: which of the two takes the next note
  volatile uint8_t  voixVoulues = 1;        // what its cue asks: 1, or 2 voices (§214)
  /* The group of each of its instrument blocks (§221), written by the cue like its channels:
   * the count dropped first, the entries, then the count. */
  struct BlockGroup { uint32_t bloc; uint8_t group; } groups[GROUPS_PER_SLOT];
  volatile uint8_t  nGroups = 0;
};
CueSlot           slots[CUE_SLOTS];
/* THE SWITCH'S SECTION (§238): the loop holds the audio task off while what goes on changes slot
 * — they share core 1 (§172), and with its interrupts off the audio task cannot start a block. */
static portMUX_TYPE slotsMux = portMUX_INITIALIZER_UNLOCKED;
static_assert(KITS_PER_SLOT == 2, "CueSlot::kit's initializer is written out for two");
static_assert(SfzStore::MOTEURS >= CUE_SLOTS * KITS_PER_SLOT, "an SFZ engine for every kit of both slots");

/* The group of an instrument block in a slot (§221): 0 when its cue does not place it. */
static uint8_t _groupOf(const CueSlot& c, uint32_t bloc) {
  const uint8_t n = c.nGroups;
  for (uint8_t i = 0; i < n && i < GROUPS_PER_SLOT; i++) if (c.groups[i].bloc == bloc) return c.groups[i].group;
  return 0;
}
static inline bool _forGroup(uint8_t group, const CueSlot& c, uint32_t bloc) {
  return group == GROUP_ALL || _groupOf(c, bloc) == group;
}
/* THE METERS OF THE OTHER INSTRUMENTS (MESURES §220): the SFZ kits (one meter per engine: one slot
 * holds it at a time, §234) and the synth voice of each slot,
 * their peak since the last reading, after their gain — as a sample voice's (picG/picD), in int16
 * units. Written by the audio task, read and reset by releverCretes, without a lock: a peak can be
 * lost there, a meter does not notice. */
struct PeakBlock { uint32_t bloc = 0; uint16_t g = 0, d = 0; };
PeakBlock kitPeak[SfzStore::MOTEURS], synthPeak[CUE_SLOTS];
volatile int8_t   slotCurrent = -1;         // the cue that plays (loopTask writes, anyone reads)
volatile int8_t   slotLeaving = -1;         // the one in its Release, -1 none

/* THE GATE CLOSES IN A FADE (STOP, §211): closed from one block to the next, with every voice cut
 * at the same instant, it was a click. It now falls to silence in 10 ms; then the slots that were
 * playing are drained — only those no cue arrived in since the STOP (`stopGen`): a PLAY right
 * after keeps its own. */
constexpr float   GATE_FADE_S = 0.010f;
float             gateGain = 0.0f;          // audio task: 1 open, 0 closed
volatile uint32_t stopGen[CUE_SLOTS] = {0, 0};   // each slot's generation at the STOP
volatile bool     stopVoices = false;       // posted by couperSon, served by the audio task

static inline float envTick(EnvCue& e) {
  if (e.wait) { e.wait--; return e.now; }
  if (e.step != 0.0f) {
    e.now += e.step;
    if ((e.step > 0.0f) ? (e.now >= e.target) : (e.now <= e.target)) { e.now = e.target; e.step = 0.0f; }
  } else if (e.now != e.target) {
    e.now = e.target;                       // after a Delay without Attack: it starts at once
  }
  return e.now;
}
static inline void envAdvance(EnvCue& e, uint32_t n) {
  if (e.wait >= n) { e.wait -= n; return; }
  n -= e.wait; e.wait = 0;
  if (e.step != 0.0f) {
    e.now += e.step * float(n);
    if ((e.step > 0.0f) ? (e.now >= e.target) : (e.now <= e.target)) { e.now = e.target; e.step = 0.0f; }
  } else {
    e.now = e.target;
  }
}
/* From `from`, after `delayS`, to `sustain` in `attackS`. */
static inline void envFrom(EnvCue& e, float from, float delayS, float attackS, float sustain) {
  const float sr = float(srReel);
  e.now = from; e.target = sustain; e.stop = false;
  e.wait = (delayS > 0.0f) ? (uint32_t)(delayS * sr) : 0;
  const float n = attackS * sr;
  e.step = (n >= 1.0f) ? (sustain - from) / n : 0.0f;
  if (e.step == 0.0f && e.wait == 0) e.now = sustain;
}
/* To silence in `releaseS`, then the slot empties. */
static inline void envLeave(EnvCue& e, float releaseS) {
  const float n = releaseS * float(srReel);
  e.target = 0.0f; e.wait = 0; e.stop = true;
  e.step = (n >= 1.0f && e.now > 0.0f) ? -e.now / n : 0.0f;
  if (e.step == 0.0f) e.now = 0.0f;
}

/* The slot the keyboard setters, the inspector and the boot speak to: the cue that plays, or
 * slot 0 before any (§212). */
static inline uint8_t _slotIci() {
  const int8_t s = slotCurrent;
  return (s >= 0 && s < (int8_t)CUE_SLOTS) ? (uint8_t)s : 0;
}

/* THE SLOT DRAINED (audio task, §212): its output is already silent — its voices stop, its
 * kit's voices too, its effects forget their tail. The loop disarms the rest and frees it. */
static void _desarmerListes(uint8_t s);   // with the play lists, below
static void _drainSlot(uint8_t s) {
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (voixEch[v].actif && voixEch[v].slot == s) voixEch[v].actif = false;
  _desarmerListes(s);
  for (const int8_t k : slots[s].kit) if (k >= 0) SfzStore::toutEteindre((uint8_t)k);
  Effects::clear(s);
  slots[s].env = EnvCue{};
  slots[s].cut = 1.0f;
  __sync_synchronize();
  slots[s].state = SLOT_DRAINED;
}

/* What the cue posted, at the start of a block (audio task) — before the lists of the cue
 * are adopted, before any of its sounds is rendered. */
static void _appliquerDemandes() {
  for (uint8_t s = 0; s < CUE_SLOTS; s++) {
    CueSlot& c = slots[s];
    const uint8_t r = c.req;
    if (r == REQ_NONE) continue;
    __sync_synchronize();
    if (r == REQ_ARRIVE) {
      const float del = c.reqDelayS, att = c.reqAttackS, sus = c.reqSustain;
      envFrom(c.env, (del > 0.0f || att > 0.0f) ? 0.0f : sus, del, att, sus);
      c.cut = 1.0f;
      c.state = SLOT_CURRENT;
    } else if (r == REQ_LEAVE) {
      envLeave(c.env, c.reqReleaseS);
      c.state = SLOT_LEAVING;
    } else if (r == REQ_CUT) {
      c.state = SLOT_CUTTING;
    }
    c.req = REQ_NONE;
  }
}

/* ── LES PLAY LIST (MESURES §196) ─────────────────────────────────────────
 * Voir AudioEngine.h. Une banque = les listes armees ; DEUX banques, en
 * PSRAM (~15 Ko, prises une fois) : un ecrivain remplit celle que la tache
 * audio ne lit pas et la PROPOSE ; elle l'adopte au debut d'un bloc et ne
 * relit plus l'autre — c'est seulement alors qu'un ecrivain peut la reprendre.
 * Les ecrivains (la cue, le chemin vivant, un son qui arrive) passent l'un
 * apres l'autre, sous verrouListes ; la tache audio ne prend aucun verrou.
 * Les noms sont resolus, et les secondes converties en trames, par
 * l'ecrivain : la tache audio ne lit que des index et des nombres. */
/* Des structures A PLAT, sans constructeur : une banque se met a zero et se
 * recopie d'un bloc (memset, memcpy), et la memoire prise a zero est deja deux
 * banques vides. Tout champ se pose explicitement (_remplirListe). */
struct Clip {
  char     son[SampleStore::NOM_MAX];
  float    debutS, finS;                 // ce que dit la cue, en secondes
  bool     boucle;
  int8_t   iEch;                         // resolu : -1, la carte n'a pas ce son
  int8_t   head;                         // a streamed sound: its head (SdStream), -1 otherwise
  uint32_t debut, fin;                   // resolus, en trames du fichier (fin 0 : au bout)
};
struct Liste {
  uint32_t bloc;                         // le bloc de la composition : l'etiquette des voix
  uint8_t  note;                         // la note du clip 1
  uint8_t  suite;                        // 0 s'arreter, 1 enchainer, 2 enchainer et reboucler
  uint8_t  n;                            // nombre de clips
  uint8_t  courant;                      // le clip qui joue (0 : aucun), tenu par la tache audio
  uint8_t  preload;                      // 1: "Preload" — on the cue before, its heads are read ahead
  uint8_t  canal;                        // the MIDI channel its notes come on, 1..16 (0: all) — §210
  uint8_t  slot;                         // the cue slot it plays in (§212)
  float    gain;
  Clip     clips[CLIPS_MAX];
};
struct Banque {
  uint8_t n;
  Liste   listes[LISTES_MAX];
};
Banque*           banques        = nullptr;   // [2], en PSRAM
volatile int8_t   banqueActive   = 0;          // celle que lit la tache audio
volatile int8_t   banqueProposee = -1;         // a adopter au debut du prochain bloc
volatile uint32_t genListes      = 0;          // change a chaque changement d'etat
StaticSemaphore_t tamponVerrouListes;
SemaphoreHandle_t verrouListes = xSemaphoreCreateMutexStatic(&tamponVerrouListes);
/* Un clip qui en coupe un autre fait taire celui-ci en 5 ms, lineairement :
 * un arret net en plein son claque. */
constexpr float FONDU_COUPE_S = 0.005f;
/* LES VU-METRES DE L'APP (MESURES §169) : chaque voix retient sa crete, que
 * loopTask releve (releverCretes). SEULEMENT quand un onglet ecoute — sans lui,
 * rien n'est mesure, pas seulement rien n'est envoye — et porte ouverte : une
 * voix qui tourne derriere la porte fermee ne s'entend pas, elle ne doit pas
 * s'afficher. */
volatile bool mesureVu = false;

/* Reste vrai tant qu'au moins une voix sonne — c'est ce que lit la porte de
 * silence et ce que « niveau » reflete. */
static inline bool _uneVoixSonne() {
  for (uint8_t v = 0; v < VOIX_MAX; v++) if (voixEch[v].actif) return true;
  return false;
}

/* A drained slot's lists leave with it: still in the bank until the next cue posts one, they no
 * longer answer a note (the audio task writes its own bank, as it does `courant`) — §212. */
static void _desarmerListes(uint8_t s) {
  if (!banques) return;
  for (uint8_t li = 0; li < banques[banqueActive].n; li++)
    if (banques[banqueActive].listes[li].slot == s) banques[banqueActive].listes[li].slot = SLOT_AUCUN;
}

/* La voix libre, sinon la PLUS ANCIENNE. */
VoixEch* _voixLibre() {
  VoixEch* plusVieille = &voixEch[0];
  for (uint8_t v = 0; v < VOIX_MAX; v++) {
    if (!voixEch[v].actif) return &voixEch[v];
    if (voixEch[v].age < plusVieille->age) plusVieille = &voixEch[v];
  }
  return plusVieille;
}

/* ── LES PLAY LIST, COTE TACHE AUDIO (§196) ────────────────────────────────
 * Tout ce qui suit tourne dans la tache audio, et seulement elle : une note,
 * un clip demande, la fin d'un clip, l'adoption d'une banque. Elle ne lit que
 * la banque active, des index et des nombres. */

/* Faire taire une voix en 5 ms : elle quitte sa liste — elle n'enchainera
 * plus — et descend lineairement jusqu'a zero, ou elle s'eteint. */
void _eteindreEnDouceur(VoixEch& vo) {
  vo.liste = -1;
  const float g = (vo.gain > 1e-4f) ? vo.gain : 1e-4f;
  vo.fondu = g / (FONDU_COUPE_S * float(srReel));
}

/* Le clip k (1..n) de L se joue-t-il ? Un son que la carte a, une selection
 * qui contient au moins deux trames. */
bool _clipJouable(const Liste& L, uint8_t k) {
  if (k == 0 || k > L.n) return false;
  const Clip& c = L.clips[k - 1];
  if (c.iEch < 0 || !SampleStore::lisible((uint8_t)c.iEch)) return false;
  /* A streamed sound only starts from its HEAD, already in PSRAM: without it (not read
   * yet by the SD-card task) the clip does not play — silence, not a wait. */
  if (SampleStore::isStreamed((uint8_t)c.iEch) && !SdStream::headReady(c.head)) return false;
  const size_t n = SampleStore::trames((uint8_t)c.iEch);
  const size_t fin = (c.fin > c.debut && c.fin < n) ? c.fin : n;
  return (size_t)c.debut + 2 <= fin;
}

/* Pose sur la voix le son et la selection du clip k — pas son gain. Faux, et
 * la voix intacte, si le clip ne se joue pas.
 *
 * A STREAMED SOUND (SdStream.h): the voice starts from the clip's HEAD, already in PSRAM,
 * and a stream positions itself behind it — at the end of the head — to fill the buffer.
 * A voice that chains from one streamed clip to another keeps its stream and repositions
 * it; towards a PSRAM sound, it gives it back. No stream available: the clip does not
 * start (false). */
static_assert(SdStream::MAX_STREAMS >= LISTES_MAX + 1, "a streamed retrigger needs a stream beyond one per list (SdStream.h)");
bool _poserClip(VoixEch& vo, const Liste& L, uint8_t k) {
  if (!_clipJouable(L, k)) return false;
  const Clip& c = L.clips[k - 1];
  const size_t n = SampleStore::trames((uint8_t)c.iEch);
  const uint8_t voiceIndex = (uint8_t)(&vo - voixEch);
  int8_t stream = SdStream::owns(vo.stream, voiceIndex) ? vo.stream : (int8_t)-1;
  SdStream::HeadView hv{nullptr, 0, 0};
  if (SampleStore::isStreamed((uint8_t)c.iEch)) {
    SdStream::headView(c.head, hv);                    // ready: _clipJouable checked it
    const uint32_t clipEnd = (c.fin > c.debut && c.fin < n) ? c.fin : (uint32_t)n;
    if (hv.start + hv.frames < clipEnd) {              // the clip outlasts its head: it needs a stream
      if (stream < 0) stream = SdStream::acquire(voiceIndex);
      if (stream < 0) {                                // counted, never logged from the audio task
        SdStream::recordNoStream();
        return false;
      }
      SdStream::start(stream, c.son, hv.start + hv.frames, clipEnd,
                      SampleStore::stereo((uint8_t)c.iEch) ? 2 : 1,
                      SampleStore::dataOffset((uint8_t)c.iEch));
    } else if (stream >= 0) {                          // the head is enough: the stream is given back
      SdStream::release(stream); stream = -1;
    }
  } else if (stream >= 0) {                            // a PSRAM sound: no stream any more
    SdStream::release(stream); stream = -1;
  }
  vo.stream = stream;
  vo.head = SampleStore::isStreamed((uint8_t)c.iEch) ? c.head : (int8_t)-1;
  vo.headPcm = hv.pcm; vo.headStart = hv.start; vo.headFrames = hv.frames;
  vo.iEch   = (uint8_t)c.iEch;
  vo.pas    = _fixedStep(double(SampleStore::frequence(vo.iEch)) / double(srReel));
  vo.pos    = _fixedFrame(c.debut);
  vo.debutT = c.debut;
  vo.finT   = (c.fin > c.debut && c.fin < n) ? c.fin : 0;
  vo.boucle = c.boucle;
  vo.clip   = k;
  return true;
}

/* THE NOTE THAT WAITS FOR ITS HEAD. A streamed clip only starts from its head, read by the
 * SD-card task (~0.1 to 0.16 s). A note that arrives before it is ready is NOT lost: it is
 * kept, and the clip starts as soon as its head is. The last note of a list wins (one clip
 * at a time), the stop note clears it, and it is given up after HEAD_WAIT_MS (head failed,
 * card absent) — a clip that would start seconds after its note is worse than silence. The
 * awaited clip's head goes ahead of the others in the read order (SdStream). All of this
 * lives in the audio task only: no lock, and never a log. */
struct PendingNote { uint32_t bloc; uint32_t t0; uint8_t clip; };
PendingNote       pendingNotes[LISTES_MAX] = {};
volatile bool     clearPendingNotes = false;
constexpr uint32_t HEAD_WAIT_MS = 2000;

// Is clip k unplayable ONLY because its head is still loading? (and for no other reason)
bool _headLoading(const Liste& L, uint8_t k) {
  if (k == 0 || k > L.n) return false;
  const Clip& c = L.clips[k - 1];
  if (c.iEch < 0 || !SampleStore::lisible((uint8_t)c.iEch) || !SampleStore::isStreamed((uint8_t)c.iEch)) return false;
  const size_t n = SampleStore::trames((uint8_t)c.iEch);
  const size_t fin = (c.fin > c.debut && c.fin < n) ? c.fin : n;
  if ((size_t)c.debut + 2 > fin) return false;
  return SdStream::headLoading(c.head);
}

void _waitForHead(uint32_t bloc, uint8_t k, int8_t head) {
  PendingNote* a = nullptr;
  for (auto& e : pendingNotes) if (e.bloc == bloc) { a = &e; break; }
  if (!a) for (auto& e : pendingNotes) if (!e.bloc) { a = &e; break; }
  if (!a) return;
  a->bloc = bloc; a->clip = k; a->t0 = millis();
  SdStream::prioritize(head);
}

void _cancelWait(uint32_t bloc) {
  for (auto& e : pendingNotes) if (e.bloc == bloc) e.bloc = 0;
}

/* Lancer le clip k de la liste li — 0 : seulement l'arreter. Ce que la liste
 * jouait se tait en 5 ms : un clip a la fois. Un clip qui ne se joue pas (sans
 * son) ne fait RIEN, pas meme arreter le precedent. */
void _lancerClip(const Banque& b, uint8_t li, uint8_t k) {
  const Liste& L = b.listes[li];
  if (k > 0 && !_clipJouable(L, k)) {
    /* Its head is still loading: the note waits. What was playing continues until the new
     * clip starts. For any other reason (sound absent, empty selection): nothing, as before. */
    if (_headLoading(L, k)) _waitForHead(L.bloc, k, L.clips[k - 1].head);
    else {
      /* A streamed clip that is not loading and not ready: its head failed or was refused —
       * counted (a note lost is something to know), never logged from here. */
      const Clip& c = L.clips[k - 1];
      if (c.iEch >= 0 && SampleStore::isStreamed((uint8_t)c.iEch)) SdStream::recordNoHead();
    }
    return;
  }
  _cancelWait(L.bloc);                    // a start, or the stop note, replaces the note that was waiting
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (voixEch[v].actif && voixEch[v].liste == (int8_t)li) _eteindreEnDouceur(voixEch[v]);
  if (k == 0) return;
  VoixEch* vo = _voixLibre();
  vo->actif = false;                   // voir declencherEchantillon
  __sync_synchronize();
  if (!_poserClip(*vo, L, k)) return;  // no stream left: the voice stays off, not half re-posed
  vo->liste = (int8_t)li;
  vo->bloc  = L.bloc;
  vo->velo  = 1.0f;                    // la velocite ne compte pas
  vo->cible = _borneGain(L.gain);
  vo->gain  = vo->cible;
  vo->fondu = 0.0f;
  vo->age   = ++voixHorloge;
  vo->picG  = 0; vo->picD = 0;
  vo->slot  = (L.slot < CUE_SLOTS) ? L.slot : _slotIci();   // its list's cue slot (§212)
  __sync_synchronize();
  vo->actif = true;
}

/* THE KEYBOARD'S POLYPHONY (§219), audio task, before a note: the keyboard's voices that still
 * sound (not fading out) are brought under its `poly` — the oldest fall silent in 5 ms, as a clip
 * cut by another. Its voices: in its slot, outside any list, of its block (of its sound when it
 * has no block, a preview). A fading voice still holds one of the card's eight for those 5 ms. */
void _limitKeyboardVoices(const CueSlot& c, uint8_t s) {
  const int i = c.clavier.bloc ? -1 : SampleStore::indexDe(c.clavier.nom);
  for (;;) {
    VoixEch* oldest = nullptr;
    uint8_t n = 0;
    for (uint8_t v = 0; v < VOIX_MAX; v++) {
      VoixEch& vo = voixEch[v];
      if (!vo.actif || vo.slot != s || vo.liste >= 0 || vo.fondu > 0.0f) continue;
      if (c.clavier.bloc ? vo.bloc != c.clavier.bloc : (i < 0 || vo.iEch != (uint8_t)i)) continue;
      n++;
      if (!oldest || vo.age < oldest->age) oldest = &vo;
    }
    if (n < c.clavier.poly || !oldest) return;
    _eteindreEnDouceur(*oldest);
  }
}

/* LA FIN D'UN CLIP — sa selection lue, sans boucle : la liste decide (`suite`).
 * Vrai si la voix enchaine, l'autre clip deja pose : la meme voix lit un autre
 * son, sans un echantillon de trou. Les clips qui ne se jouent pas sont
 * sautes ; au plus un tour de liste. */
bool _clipSuivant(VoixEch& vo) {
  if (vo.liste < 0 || !banques) return false;
  const Banque& b = banques[banqueActive];
  if (vo.liste >= (int8_t)b.n) return false;
  const Liste& L = b.listes[vo.liste];
  if (L.suite == 0 || !L.n) return false;
  for (uint8_t essai = 1; essai <= L.n; essai++) {
    uint16_t k = (uint16_t)vo.clip + essai;
    if (k > L.n) {
      if (L.suite != 2) return false;
      k = (uint16_t)((k - 1) % L.n) + 1;
    }
    if (_poserClip(vo, L, (uint8_t)k)) return true;
  }
  return false;
}

/* UNE NOTE CHOISIT LE CLIP, dans chaque liste armee : la note du clip 1 joue
 * le clip 1, la suivante le clip 2… ; celle juste en dessous arrete. */
void _notePourLesListes(uint8_t note, uint8_t canal, uint8_t slot, uint8_t group) {
  if (!banques) return;
  const Banque& b = banques[banqueActive];
  for (uint8_t li = 0; li < b.n; li++) {
    if (b.listes[li].slot != slot) continue;             // another cue's list (§212)
    if (!_ecoute(b.listes[li].canal, canal)) continue;   // another track's channel (§210)
    if (!_forGroup(group, slots[slot], b.listes[li].bloc)) continue;   // another group's (§221)
    const int k = int(note) - int(b.listes[li].note) + 1;   // 0 : la note d'arret
    if (k >= 0 && k <= b.listes[li].n) _lancerClip(b, li, (uint8_t)k);
  }
}

/* Un clip demande hors du clavier : l'ecoute de l'inspecteur (jouerClip). */
void _clipDemande(uint32_t bloc, uint8_t k) {
  if (!banques || !bloc) return;
  const Banque& b = banques[banqueActive];
  for (uint8_t li = 0; li < b.n; li++)
    if (b.listes[li].bloc == bloc) { _lancerClip(b, li, k); return; }
}

/* THE WAITING NOTES, every block: is their head ready? The clip starts. Given up: the list
 * no longer exists, the head failed, or the wait exceeds HEAD_WAIT_MS. */
void _servePendingNotes() {
  if (clearPendingNotes) { clearPendingNotes = false; for (auto& e : pendingNotes) e.bloc = 0; }
  if (!banques) return;
  const uint32_t now = millis();
  const Banque& b = banques[banqueActive];
  for (auto& e : pendingNotes) {
    if (!e.bloc) continue;
    int li = -1;
    for (uint8_t i = 0; i < b.n; i++) if (b.listes[i].bloc == e.bloc) { li = i; break; }
    if (li < 0 || e.clip > b.listes[li].n) { e.bloc = 0; continue; }       // the list is gone
    const Liste& L = b.listes[li];
    if (_clipJouable(L, e.clip)) {
      const uint8_t k = e.clip;
      SdStream::recordWait(now - e.t0);
      e.bloc = 0;
      _lancerClip(b, (uint8_t)li, k);
      continue;
    }
    if (!_headLoading(L, e.clip) || now - e.t0 > HEAD_WAIT_MS) { SdStream::recordDrop(); e.bloc = 0; }
  }
}

/* L'ADOPTION d'une banque proposee, au debut d'un bloc — ou par l'ecrivain
 * quand la tache audio ne tourne pas. Une voix dont la liste continue (meme
 * bloc : une chaine de cellules) la suit, et glisse vers son nouveau gain ;
 * celle d'une liste qui part se tait en 5 ms. */
void _adopter(int8_t prop) {
  const Banque& an = banques[banqueActive];
  Banque& nv = banques[prop];
  for (uint8_t v = 0; v < VOIX_MAX; v++) {
    VoixEch& vo = voixEch[v];
    if (!vo.actif || vo.liste < 0) continue;
    const uint32_t bloc = (vo.liste < (int8_t)an.n) ? an.listes[vo.liste].bloc : 0;
    int8_t li = -1;
    for (uint8_t k = 0; k < nv.n && bloc; k++) if (nv.listes[k].bloc == bloc) { li = (int8_t)k; break; }
    if (li < 0) { _eteindreEnDouceur(vo); continue; }
    vo.liste = li;
    vo.cible = vo.velo * _borneGain(nv.listes[li].gain);
    /* A list that goes on (the same block in the arriving cue) moves to its slot — its
     * output, its effects, its envelope, as a chained case in the browser (§13.7, §212); a
     * list of the cue in its Release keeps its own. */
    vo.slot = nv.listes[li].slot;
  }
  for (uint8_t k = 0; k < nv.n; k++) nv.listes[k].courant = 0;   // recompte au bloc
  banqueActive = prop;
  __sync_synchronize();
  banqueProposee = -1;
  genListes = genListes + 1;
}

/* LE CLIP QUI JOUE, recompte a chaque bloc DEPUIS LES VOIX : rien d'autre ne
 * peut mentir — une fin, un STOP, un vol de voix, une coupure de cue. */
void _suivreLesListes() {
  if (!banques) return;
  Banque& b = banques[banqueActive];
  for (uint8_t li = 0; li < b.n; li++) {
    uint8_t c = 0;
    for (uint8_t v = 0; v < VOIX_MAX; v++)
      if (voixEch[v].actif && voixEch[v].liste == (int8_t)li) { c = voixEch[v].clip; break; }
    if (b.listes[li].courant != c) { b.listes[li].courant = c; genListes = genListes + 1; }
  }
}
/* DEUX FACONS DE DECLENCHER, et c'est le bloc qui choisit.
 *   surCue = true  — comportement d'ORIGINE de play-sf : le son part a
 *                    l'arrivee sur la cue, a la hauteur du fichier, et le
 *                    clavier ne le touche pas.
 *   surCue = false — le clavier le declenche, transpose par la note (do central
 *                    = hauteur d'origine). C'est ce que la carte faisait, et ca
 *                    marche : on le garde plutot que de le jeter.
 * Le defaut est `true` : c'est le comportement du moteur tel qu'il existe cote
 * navigateur, et c'est ce qu'on porte.
 * Quel echantillon le clavier joue, celui que la cue a designe, ET A QUEL VOLUME
 * (MESURES §165) : celui du bloc qui l'a designe. Tout cela appartient a SON
 * EMPLACEMENT de cue depuis le §212 (CueSlot::clavier) : la cue qui part garde le
 * sien pendant son Release. */

/* A frame of a streamed voice: its head first ([headStart, headStart + headFrames), in PSRAM),
 * then the stream's buffer. False: not (or no longer) there — the caller yields silence. */
static inline bool _readHead(const VoixEch& vo, bool st, uint32_t t, int16_t& g, int16_t& d) {
  if (t >= vo.headStart && t - vo.headStart < vo.headFrames) {
    const uint32_t j = t - vo.headStart;
    if (st) { g = vo.headPcm[j * 2]; d = vo.headPcm[j * 2 + 1]; }
    else    { g = d = vo.headPcm[j]; }
    return true;
  }
  return vo.stream >= 0 && SdStream::read(vo.stream, t, g, d);
}

/* THE STREAMS, EVERY BLOCK (before rendering): a stream whose voice is gone — finished,
 * cut, stolen, or the engine changed — is given back, and the reader closes its file; and
 * every streamed voice says where it is, which sets how far the reader fills. */
void _sweepStreams() {
  for (int8_t f = 0; f < (int8_t)SdStream::MAX_STREAMS; f++) {
    if (!SdStream::active(f)) continue;
    const VoixEch& vo = voixEch[SdStream::voiceOf(f)];
    if (!(vo.actif && vo.stream == f)) SdStream::release(f);
  }
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (voixEch[v].actif && voixEch[v].stream >= 0) SdStream::position(voixEch[v].stream, (uint32_t)(voixEch[v].pos >> 32));
}

/* THE MASTER (§212): the two cue slots, each through its effects and at its envelope, add up
 * here; saturated once, at the end (_finirLeMelange). */
float masterL[FRAMES], masterR[FRAMES];

/* A block's peak over one audio block, after its gain `k` (§220): the meter of a kit or a synth
 * voice. Float in, full scale 1.0; kept in int16 units, as a sample voice's, up to 65 535. */
void _notePeak(PeakBlock& p, uint32_t bloc, const float* l, const float* r, float k) {
  float mg = 0.0f, md = 0.0f;
  for (size_t i = 0; i < FRAMES; i++) {
    const float a = fabsf(l[i]), b = fabsf(r[i]);
    if (a > mg) mg = a;
    if (b > md) md = b;
  }
  const float ag = fminf(65535.0f, mg * fabsf(k) * 32767.0f), ad = fminf(65535.0f, md * fabsf(k) * 32767.0f);
  p.bloc = bloc;
  if (ag > p.g) p.g = (uint16_t)ag;
  if (ad > p.d) p.d = (uint16_t)ad;
}

/* ONE VOICE OF A SLOT, its whole block, added into `l`/`r` — the sum it belongs to (§230: a
 * voice renders once, into its group's sum or straight into the slot's mix). What it read past
 * its data marks `underruns`. */
static void _mixVoice(uint8_t v, float* l, float* r, uint8_t& underruns, bool mesurer) {
  constexpr float IN = 1.0f / 32768.0f;
  constexpr float FRACTION = 1.0f / 4294967296.0f;
  VoixEch& vo = voixEch[v];
  /* WHAT IT READS, asked once a block — and again when a list chains to another clip. A
   * STREAMED VOICE has no PCM in memory: it reads its head, then the stream's buffer
   * (_readHead). The others read the store, as always. A sound retired meanwhile: only the
   * block under way may still read it, its memory is given back two blocks later
   * (rendreApresLecture) — the next block finds it gone. */
  const int16_t* pcm = (vo.head >= 0) ? nullptr : SampleStore::donnees(vo.iEch);
  size_t         n   = SampleStore::trames(vo.iEch);
  bool           st  = SampleStore::stereo(vo.iEch);
  /* MELANGE. Chaque voix lit son propre echantillon a son propre pas, et on
   * somme en float avant de borner : additionner en int16 replierait au lieu
   * de saturer, ce qui s'entend comme un craquement franc. */
  for (size_t i = 0; i < FRAMES; i++) {
    if ((vo.head < 0 && !pcm) || n < 2) { vo.actif = false; return; }
    /* Fin atteinte : on reboucle, ou la voix s'eteint. Le test precede la
     * lecture pour que le reenroulement ne rejoue pas deux fois la derniere
     * trame a chaque tour. LA FIN est celle du fichier, ou celle de la
     * selection d'un clip de play list (§196), qui reboucle sur son debut —
     * et a la fin d'un clip, la liste peut enchainer sur la meme voix. */
    const size_t   fin  = (vo.finT > 1 && vo.finT < n) ? vo.finT : n;
    const uint64_t last = _fixedFrame((uint32_t)(fin - 1));
    if (vo.pos >= last) {
      if (vo.boucle) {
        vo.pos = _fixedFrame(vo.debutT) + (vo.pos - last);
        if (vo.pos >= last) vo.pos = _fixedFrame(vo.debutT);
        if (vo.stream >= 0) SdStream::restart(vo.stream);   // the voice restarts from the head: the stream repositions
      } else if (vo.liste >= 0 && _clipSuivant(vo)) {
        pcm = (vo.head >= 0) ? nullptr : SampleStore::donnees(vo.iEch);
        n   = SampleStore::trames(vo.iEch);
        st  = SampleStore::stereo(vo.iEch);
        if ((vo.head < 0 && !pcm) || n < 2) { vo.actif = false; return; }
      } else { vo.actif = false; return; }
    }
    if (vo.fondu > 0.0f) {
      /* SE TAIRE EN 5 MS (§196) : un clip coupe par un autre. */
      vo.gain -= vo.fondu;
      if (vo.gain <= 0.0f) { vo.actif = false; vo.fondu = 0.0f; return; }
    } else {
      /* Le gain rejoint sa cible — `fixerGainBloc` la deplace pendant que la
       * voix joue. Arrive a moins de 1e-4, on pose la cible : pas de traine
       * sans fin vers zero, donc pas de nombres denormaux dans la boucle. */
      const float ecart = vo.cible - vo.gain;
      if (ecart != 0.0f)
        vo.gain = (fabsf(ecart) < 1e-4f) ? vo.cible : vo.gain + ecart * LISSAGE_GAIN;
    }
    const size_t k = (size_t)(vo.pos >> 32);
    const float  f = float((uint32_t)vo.pos) * FRACTION;
    int32_t eg, ed;
    if (vo.head >= 0) {
      /* The head, then the buffer; what is missing is SILENCE — never a wait. */
      int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
      const bool ok0 = _readHead(vo, st, (uint32_t)k, l0, r0);
      const bool ok1 = ok0 && _readHead(vo, st, (uint32_t)k + 1, l1, r1);
      if (!ok0)      underruns |= (uint8_t)(1u << v);
      else if (!ok1) { l1 = l0; r1 = r0; }
      eg = (int32_t)(l0 + f * (l1 - l0));
      ed = (int32_t)(r0 + f * (r1 - r0));
    } else if (st) {
      eg = (int32_t)(pcm[k*2]     + f * (pcm[(k+1)*2]     - pcm[k*2]));
      ed = (int32_t)(pcm[k*2 + 1] + f * (pcm[(k+1)*2 + 1] - pcm[k*2 + 1]));
    } else {
      eg = ed = (int32_t)(pcm[k] + f * (pcm[k+1] - pcm[k]));
    }
    const float   gv = vo.gain;
    const int32_t sg = (int32_t)(eg * gv), sd = (int32_t)(ed * gv);
    l[i] += sg * IN; r[i] += sd * IN;
    if (mesurer) {
      /* La crete de la voix, APRES son gain : ce qu'elle apporte au melange.
       * Au-dela de 32 767 (gain de fader > 1), elle sature : le vu-metre
       * l'affichera plein, en rouge. */
      const int32_t ag = (sg < 0) ? -sg : sg, ad = (sd < 0) ? -sd : sd;
      if (ag > vo.picG) vo.picG = (uint16_t)((ag > 65535) ? 65535 : ag);
      if (ad > vo.picD) vo.picD = (uint16_t)((ad > 65535) ? 65535 : ad);
    }
    vo.pos += vo.pas;
  }
}

/* ONE CUE SLOT'S BLOCK (§212): its voices, its kit, its synth voice, summed as in the web version
 * (§230) — each group whose FX tracks are its own apart, through them, then into the slot's mix
 * with the instruments of the other groups; the MIX zone's FX on that mix — then its envelope
 * (and a cut's fade) into the master. A slot whose envelope fell to silence, or whose cut ended,
 * is drained. */
void rendreSlot(uint8_t s, uint8_t& underruns) {
  CueSlot& c = slots[s];
  if (c.state == SLOT_DRAINED) return;
  bool voix = false;
  for (uint8_t v = 0; v < VOIX_MAX; v++) voix = voix || (voixEch[v].actif && voixEch[v].slot == s);
  /* A free slot has nothing armed: only what the inspector plays in it. */
  if (c.state == SLOT_FREE && !voix) return;
  Effects::beginBlock(s);                     // the chain a writer posted, adopted whole
  /* WHICH SUM EACH INSTRUMENT GOES INTO (§230): its group's, when that group has FX of its own;
   * else straight into the slot's mix. Asked once per block. */
  constexpr uint8_t DIRECT = 0xFF;
  const uint32_t fxGroups = Effects::groupMask(s);
  auto laneOf = [&](uint32_t bloc) -> uint8_t {
    const uint8_t g = _groupOf(c, bloc);
    return (g <= GROUP_MAX && (fxGroups & (1u << g))) ? g : DIRECT;
  };
  uint8_t lane[VOIX_MAX];
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    lane[v] = (voixEch[v].actif && voixEch[v].slot == s) ? laneOf(voixEch[v].bloc) : DIRECT;
  const bool mesurer = mesureVu && !gSilence;
  /* THE SFZ ENGINES of the slot (§207; two kits, §234): each renders float into the scratch
   * buffers inside the sum of its lane — once a block, since every lane is summed once. */
  static float sfzG[FRAMES], sfzD[FRAMES];   // ~1 ko, internal RAM: audio-path state (the kits in turn)
  int8_t kitM[KITS_PER_SLOT]; float kitK[KITS_PER_SLOT]; uint8_t kitLane[KITS_PER_SLOT];
  for (uint8_t k = 0; k < KITS_PER_SLOT; k++) {
    kitM[k] = c.kit[k];
    kitK[k] = (kitM[k] >= 0) ? SfzStore::gainLisse((uint8_t)kitM[k]) : 0.0f;   // the block's fader, glided (§208)
    kitLane[k] = (kitM[k] >= 0) ? laneOf(SfzStore::blocId((uint8_t)kitM[k])) : DIRECT;
  }
  /* THE SLOT'S SYNTH VOICE, or its two (rendered ahead on core 0; the same block, the same
   * routing — §214). */
  const int8_t sv = c.synth, sv2 = c.synth2;
  memset(synthMono, 0, sizeof(synthMono));
  bool synthSonne = false;
  if (sv >= 0)  synthSonne |= SynthVoice::take((uint8_t)sv, synthMono, FRAMES);
  if (sv2 >= 0) synthSonne |= SynthVoice::take((uint8_t)sv2, synthMono, FRAMES);
  uint8_t synthLane = DIRECT;
  if (synthSonne) {
    const uint32_t sb = SynthVoice::bloc((uint8_t)(sv >= 0 ? sv : sv2));
    synthLane = laneOf(sb);
    if (mesurer) _notePeak(synthPeak[s], sb, synthMono, synthMono, 1.0f);   // its meter (§220)
  }
  /* One sum: the voices of that lane, and the kits and the synth voice when they are in it. */
  auto sum = [&](uint8_t want, float* l, float* r) {
    memset(l, 0, FRAMES * sizeof(float));
    memset(r, 0, FRAMES * sizeof(float));
    for (uint8_t v = 0; v < VOIX_MAX; v++) {
      if (!(lane[v] == want && voixEch[v].actif && voixEch[v].slot == s)) continue;
      const uint32_t bloc = voixEch[v].bloc;
      const uint32_t t = ESP.getCycleCount();
      _mixVoice(v, l, r, underruns, mesurer);
      const uint32_t dt = ESP.getCycleCount() - t;
      BlockLoads::add(1, bloc, dt);                  // its block's load, live (§235)
      /* What a voice costs (§231), a whole block only: one that ended during it rendered less. */
      const VoixEch& vo = voixEch[v];
      if (vo.actif)
        ProcessCosts::note(vo.liste < 0 ? ProcessCosts::PLAY_SF
                           : vo.head >= 0 ? ProcessCosts::PLAY_LIST_STREAM : ProcessCosts::PLAY_LIST,
                           dt, FRAMES);
    }
    for (uint8_t k = 0; k < KITS_PER_SLOT; k++) {
      const int8_t m = kitM[k];
      if (m < 0 || kitLane[k] != want || !SfzStore::rendreBloc((uint8_t)m, sfzG, sfzD, FRAMES)) continue;
      const float g = kitK[k];
      if (mesurer) _notePeak(kitPeak[m], SfzStore::blocId((uint8_t)m), sfzG, sfzD, g);   // §220
      for (size_t i = 0; i < FRAMES; i++) { l[i] += sfzG[i] * g; r[i] += sfzD[i] * g; }
    }
    if (synthSonne && synthLane == want)
      for (size_t i = 0; i < FRAMES; i++) { l[i] += synthMono[i]; r[i] += synthMono[i]; }
  };
  sum(DIRECT, mixL, mixR);
  for (uint32_t m = fxGroups; m; m &= m - 1) {
    const uint8_t g = (uint8_t)__builtin_ctz(m);
    sum(g, busL, busR);
    Effects::processGroup(s, g, busL, busR, FRAMES);
    for (size_t i = 0; i < FRAMES; i++) { mixL[i] += busL[i]; mixR[i] += busR[i]; }
  }
  Effects::processMix(s, mixL, mixR, FRAMES);
  /* ITS ENVELOPE, on its output — after the effects: a reverb tail falls with the Release
   * (§211-212); and a cut's 10 ms fade. */
  const float pasCoupe = 1.0f / (SLOT_CUT_S * float(srReel));
  const bool coupe = (c.state == SLOT_CUTTING);
  for (size_t i = 0; i < FRAMES; i++) {
    float k = envTick(c.env);
    if (coupe) { c.cut = (c.cut > pasCoupe) ? c.cut - pasCoupe : 0.0f; k *= c.cut; }
    masterL[i] += mixL[i] * k; masterR[i] += mixR[i] * k;
  }
  if ((coupe && c.cut <= 0.0f) || (c.state == SLOT_LEAVING && c.env.stop && c.env.now <= 0.0f))
    _drainSlot(s);
}

/* THE TWO SLOTS, then the int16 bus of the DAC — saturated here, once. */
void rendreSample() {
  uint8_t underruns = 0;                // the streamed voices that ran short of data this block
  memset(masterL, 0, sizeof(masterL));
  memset(masterR, 0, sizeof(masterR));
  for (uint8_t s = 0; s < CUE_SLOTS; s++) rendreSlot(s, underruns);
  if (underruns)
    for (uint8_t v = 0; v < VOIX_MAX; v++)
      if (underruns & (1u << v)) SdStream::underrun(voixEch[v].stream);
}

void _finirLeMelange() {
  for (size_t i = 0; i < FRAMES; i++) {
    float l = masterL[i] * 32767.0f, r = masterR[i] * 32767.0f;
    l = l > 32767.0f ? 32767.0f : (l < -32768.0f ? -32768.0f : l);
    r = r > 32767.0f ? 32767.0f : (r < -32768.0f ? -32768.0f : r);
    entrelace[i * 2] = (int16_t)l; entrelace[i * 2 + 1] = (int16_t)r;
  }
}

void rendrePlaits() {
  for (size_t i = 0; i < FRAMES; i += plaits::kBlockSize) {
    plaitsVoix->Render(plaitsPatch, plaitsMod, &plaitsTrames[i], plaits::kBlockSize);
    plaitsMod.trigger = 0.0f;   // une impulsion d'un bloc, pas un niveau tenu
  }
  for (size_t i = 0; i < FRAMES; i++) {
    entrelace[i * 2]     = plaitsTrames[i].out;
    entrelace[i * 2 + 1] = plaitsTrames[i].aux;
  }
}

float frequenceDeNote(uint8_t note) {
  // 440 Hz au la3 (note 69). On divise par la fréquence RÉELLE de l'I2S, pas
  // par 48000 : sans APLL, le S3 ne tombe pas juste, et Plaits fait la même
  // correction dans dsp.h (kCorrectedSampleRate).
  return 440.0f * powf(2.0f, (float(note) - 69.0f) / 12.0f);
}

/* THE READER IS READY: engine -2 with something to play — a sound of storage or
 * the SD, an SFZ kit, or the synth voice (§210). A board whose kits all live in /sfz has no sound in
 * storage: the reader used to count as not ready, and the kit rendered nothing.
 * actif(), not charge(): voices still draining after an unload must be rendered
 * too (the frozen-voice defect, through another door). */
bool lecteurPret() {
  return moteurCourant == -2 && (SampleStore::nombreCharges() > 0 || SfzStore::actif()
                                 || SynthVoice::active(0) || SynthVoice::active(1));
}

void appliquer(const Evenement& e) {
  // La porte n'est PAS ouverte par les notes : c'est le TRANSPORT qui decide.
  // Sans play, le clavier ne doit rien produire — regle demandee explicitement.
  // Ouverture par ouvrirSon() (PLAY), fermeture par couperSon() (STOP).
  if (e.sorte == SORTE_CLIP) { _clipDemande(e.bloc, e.clip); return; }   // §196
  if (lecteurPret()) {
    /* EACH INSTRUMENT ON ITS TRACK'S CHANNEL (§210), as in the browser: the
     * lists (a note picks the clip of each armed list), the synth voice, the
     * SFZ kit, the play-sf keyboard — every one whose channel matches plays it.
     * (The SFZ kit used to take the keyboard; channels say it better.)
     * IN EACH CUE SLOT THE NOTE IS FOR (§212): the cue that plays, the one in its Release. */
    for (uint8_t s = 0; s < CUE_SLOTS; s++) {
      if (!(e.slots & (1u << s))) continue;
      CueSlot& c = slots[s];
      if (e.velo != 0) _notePourLesListes(e.note, e.canal, s, e.group);
      /* One voice: it takes every note. Two (§214): they take turns — a voice the guard
       * stopped is skipped, the other takes them all. The voice of another group's track
       * takes none (§221). */
      int8_t sv = c.synth;
      const int8_t sv2 = c.synth2;
      const int8_t svb = (sv >= 0) ? sv : sv2;
      const bool synthIci = svb >= 0 && _forGroup(e.group, c, SynthVoice::bloc((uint8_t)svb));
      if (synthIci && sv2 >= 0 && e.velo != 0) {
        const bool a = sv >= 0 && SynthVoice::active((uint8_t)sv), b = SynthVoice::active((uint8_t)sv2);
        if (a && b) { if (c.prochaine) sv = sv2; c.prochaine ^= 1; }
        else if (b) sv = sv2;
      }
      if (synthIci && sv >= 0) SynthVoice::noteOn((uint8_t)sv, e.canal, e.note, e.velo);
      for (uint8_t k = 0; k < KITS_PER_SLOT; k++) {   // each kit of the slot (§234), on its channel and group
        const int8_t kit = c.kit[k];
        if (!(kit >= 0 && SfzStore::notesArmees((uint8_t)kit) && _ecoute(c.kitCanal[k], e.canal)
              && _forGroup(e.group, c, SfzStore::blocId((uint8_t)kit)))) continue;
        if (e.velo == 0) SfzStore::noteOff((uint8_t)kit, e.note);
        else             SfzStore::noteOn((uint8_t)kit, e.note, e.velo);
      }
      /* EN MODE « SUR CUE », le clavier ne touche pas l'echantillon : c'est la cue
       * qui le declenche. La note est ABSORBEE quand meme — sans le retour plus
       * bas elle tomberait sur le sinus, et on entendrait un bip a chaque touche
       * sur une carte dont le moteur est le lecteur. */
      if (c.clavier.surCue) continue;
      if (e.velo == 0) continue;               // l'échantillon va au bout
      if (!c.clavier.nom[0] || !_ecoute(c.clavier.canal, e.canal)) continue;
      if (!_forGroup(e.group, c, c.clavier.bloc)) continue;   // another group's keyboard (§221)
      /* Au clavier, c'est l'echantillon du CLAVIER qui joue — celui que la cue a
       * designe. Do central (60) = hauteur d'origine. Une voix par note : jouer un
       * accord donne un accord — within the keyboard's polyphony (§219). */
      _limitKeyboardVoices(c, s);
      declencherEchantillon(c.clavier.nom, /*boucle=*/false, c.clavier.gain, float(e.note) - 60.0f,
                            c.clavier.bloc, float(e.velo) / 127.0f, (int8_t)s);
    }
    return;
  }
  if (e.velo != 0) derniereNote = e.note;   // temoin : la note REELLEMENT jouee
  if (moteurCourant >= 0 && plaitsVoix) {
    // Le LPG de Plaits gere l'extinction : son decay EST le relachement, et il
    // doit rester audible. (J'avais tente de fermer la porte au relachement de
    // la derniere touche : ca coupait net et supprimait le decay — regression.)
    if (e.velo == 0) return;
    plaitsPatch.note = float(e.note);
    plaitsTrigger = true;
    return;
  }
  if (e.velo == 0) {
    for (auto& v : voix) if (v.active && v.note == e.note) v.cible = 0.0f;
    return;
  }
  Voix* cible = nullptr;
  for (auto& v : voix) if (v.active && v.note == e.note) { cible = &v; break; }
  if (!cible) for (auto& v : voix) if (!v.active) { cible = &v; break; }
  if (!cible) cible = &voix[0];               // vol de voix : la plus ancienne
  cible->active    = true;
  cible->note      = e.note;
  cible->increment = frequenceDeNote(e.note) / float(srReel);
  cible->cible     = AMPLITUDE * (float(e.velo) / 127.0f);
}

void rendre() {
  const float pas = 1.0f / (RAMPE_MS * 0.001f * float(srReel));
  for (size_t i = 0; i < FRAMES; i++) {
    float s = 0.0f;
    for (auto& v : voix) {
      if (!v.active) continue;
      if (v.gain < v.cible)      v.gain = fminf(v.gain + pas * AMPLITUDE, v.cible);
      else if (v.gain > v.cible) v.gain = fmaxf(v.gain - pas * AMPLITUDE, v.cible);
      if (v.gain <= 0.0f && v.cible == 0.0f) { v.active = false; continue; }
      v.phase += v.increment;
      if (v.phase >= 1.0f) v.phase -= 1.0f;
      s += sinf(v.phase * 2.0f * float(M_PI)) * v.gain;
    }
    if (bipBlocsRestants && bipHz > 0.0f) {
      bipPhase += bipHz / float(srReel);
      if (bipPhase >= 1.0f) bipPhase -= 1.0f;
      s += sinf(bipPhase * 2.0f * float(M_PI)) * AMPLITUDE;
    }
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    const int16_t e = (int16_t)(s * 32000.0f);
    entrelace[i * 2] = e; entrelace[i * 2 + 1] = e;
  }
  if (bipBlocsRestants) bipBlocsRestants--;
}

void boucleAudio(void*) {
  /* 95 % of a block's cycles: 240 MHz, 120 frames at the real rate. */
  const uint32_t budgetBloc = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL * FRAMES
                                         / (srReel ? srReel : SAMPLE_RATE) * 95 / 100);
  for (;;) {
    if (arretDemande) { tacheArretee = true; vTaskDelete(nullptr); }
    /* LES PLAY LIST PROPOSEES s'adoptent AVANT les notes de ce bloc (§196) :
     * une liste posee puis jouee (l'ecoute de l'inspecteur) trouve la sienne. */
    _appliquerDemandes();                // what the cues posted for their slots, before their lists (§212)
    if (banqueProposee >= 0 && banques) _adopter(banqueProposee);
    SfzStore::adopter();                 // the proposed SFZ instrument, same pattern (§207)
    SfzStore::blocRendu();              // the heartbeat: the loop is alive (§208)
    Evenement e;
    while (xQueueReceive(evenements, &e, 0) == pdTRUE) { SfzStore::evenementVu(); appliquer(e); }
    _servePendingNotes();               // notes whose head was awaited start as soon as it is there
    /* UN SON RETIRE (remplace, supprime — §177) se tait ici, au debut du bloc :
     * sa memoire ne se rend que deux blocs plus tard (rendreApresLecture). Sans
     * ce balayage, une voix restee sur un emplacement retire — magasin vide,
     * rendreSample() n'etait plus appele — rejouait le son suivant qui s'y
     * installe. */
    for (uint8_t v = 0; v < VOIX_MAX; v++)
      if (voixEch[v].actif && !SampleStore::lisible(voixEch[v].iEch)) voixEch[v].actif = false;

    _sweepStreams();
    const uint32_t t0 = millis();
    const uint32_t u0 = micros();
    const uint32_t e0 = ecrituresFlash;
    const uint32_t c0 = ESP.getCycleCount();
    ProcessCosts::beginBlock();          // what each process of the block costs (§231)
    if (lecteurPret()) {
      rendreSample();                    // the two cue slots (§212)
      _finirLeMelange();
    } else {
      /* Nothing renders: a slot leaving or being cut has nothing to fade — drained now, or it
       * would hold its place for good (§212). */
      for (uint8_t s = 0; s < CUE_SLOTS; s++)
        if (slots[s].state == SLOT_LEAVING || slots[s].state == SLOT_CUTTING) _drainSlot(s);
      if (moteurCourant >= 0 && plaitsVoix) {
        if (plaitsTrigger) { plaitsMod.trigger = 1.0f; plaitsTrigger = false; }
        rendrePlaits();
      } else {
        rendre();
      }
    }
    const uint32_t renderCycles = ESP.getCycleCount() - c0;
    ProcessCosts::endBlock();
    cyclesEch = renderCycles / FRAMES;
    /* THE GUARD OF CORE 1 (§209-210): a render over 95 % of its block, 20 blocks
     * in a row — the audio task would never yield again, and the watchdog
     * restarts the card. The effects go first (the reverb, then the low-pass),
     * until the next cue; samples and kit never. */
    {
      static uint32_t surCharge = 0;
      if (renderCycles > budgetBloc) { if (++surCharge >= 20) { Effects::shed(); surCharge = 0; } }
      else surCharge = 0;
    }
    SfzStore::costBlock(cyclesEch * FRAMES);          // what the SFZ blocks cost, kernel and whole (§208)
#if NIDMI_DSP_BENCH
    DspBench::process(entrelace, FRAMES, renderCycles);   // measurement images only (DspBench.h)
#endif
    _suivreLesListes();                 // le clip qui joue, depuis les voix (§196)
    /* VOLUME. Avant la porte, donc avant la mesure de crete : ce qu'on mesure
     * reste ce qui part vraiment. A 1.0 on ne touche a rien — le cas courant ne
     * paie pas une multiplication par echantillon. */
    {
      const float g = gVolume;
      if (g < 0.999f) {
        for (size_t i = 0; i < FRAMES * 2; i++) {
          float v = (float)entrelace[i] * g;
          if (v >  32767.f) v =  32767.f;
          if (v < -32768.f) v = -32768.f;
          entrelace[i] = (int16_t)v;
        }
      }
    }
    // Porte de silence (STOP) : Plaits rend en continu et ignore le note-off.
    /* It closes in a 10 ms fade, then the voices of before the STOP stop behind it (§211). Opened
     * again before it closed (a PLAY right after), they stop all the same: STOP stops them. */
    if (gSilence) {
      if (gateGain > 0.0f) {
        const float pas = 1.0f / (GATE_FADE_S * float(srReel));
        for (size_t i = 0; i < FRAMES; i++) {
          gateGain = (gateGain > pas) ? gateGain - pas : 0.0f;
          entrelace[2 * i]     = (int16_t)(entrelace[2 * i] * gateGain);
          entrelace[2 * i + 1] = (int16_t)(entrelace[2 * i + 1] * gateGain);
        }
      } else {
        memset(entrelace, 0, sizeof(entrelace));
      }
    } else {
      gateGain = 1.0f;
    }
    /* The gate closed (or opened again before: STOP stops them all the same): the slots that
     * were playing are drained — not one a cue arrived in since (§212). */
    if (stopVoices && (gateGain <= 0.0f || !gSilence)) {
      stopVoices = false;
      for (uint8_t s = 0; s < CUE_SLOTS; s++)
        if (slots[s].gen == stopGen[s] && slots[s].state != SLOT_DRAINED) _drainSlot(s);
    }
    // Niveau crete de ce qui part REELLEMENT vers le DAC (donc apres la porte).
    // Sans cette mesure, « est-ce que ca sonne ? » ne se repond qu'a l'oreille,
    // et tout diagnostic audio devient une conversation au lieu d'une lecture.
    {
      uint16_t crete = 0;
      for (size_t i = 0; i < FRAMES * 2; i++) {
        int16_t v = entrelace[i];
        uint16_t a = (v < 0) ? (uint16_t)(-(int32_t)v) : (uint16_t)v;
        if (a > crete) crete = a;
      }
      niveauCrete = crete;
      if (crete > SEUIL_AUDIBLE) dernierSonMs = t0;
    }
    if (captureArmed) {
      const uint32_t reste = captureFrames - captureFilled, k = reste < FRAMES ? reste : FRAMES;
      memcpy(captureBuf + captureFilled * 2, entrelace, k * 2 * sizeof(int16_t));
      captureFilled = captureFilled + k;
      if (captureFilled >= captureFrames) captureArmed = false;
    }
    const uint32_t ur = micros();
    i2s.write((const uint8_t*)entrelace, sizeof(entrelace));
    // Un bloc dure 2,67 ms ; si l'aller-retour dépasse largement, c'est que la
    // tâche a été préemptée au point de vider le DMA.
    const uint32_t du = micros() - u0;
    if (du > pireBlocUs) { pireBlocUs = du; pireBlocRenduUs = ur - u0; }
    if (millis() - t0 > 20) {
      nRetards++;
      // Ce bloc a-t-il chevauche une ecriture volontaire ? (commencee avant lui,
      // ou pendant : la sequence a bouge, ou elle etait impaire a son debut)
      if ((e0 & 1u) || ecrituresFlash != e0) nRetardsEcritures++;
    }
    nBlocs++;
  }
}

}  // namespace

/* LE MOTEUR EN COURS, pour le diagnostic (§208) : la note du SFZ mourrait
 * sans que rien le dise — le statut nommera maintenant la porte. */
int moteurActif() { return moteurCourant; }

// ── Le moteur de la composition, et le garde-fou ────────────────────────────
// LE MOTEUR N'EST PLUS MEMORISE : il se DEDUIT de la composition ouverte
// (Cues::moteurEmploye, MESURES §187). Il vivait en NVS, commun a toute la
// carte — ""/"-1" sinus, "p:<n>" Plaits, "s:<nom>" echantillon —, pose en
// passant par chaque son ecoute dans l'inspecteur : la derniere ecoute, dans
// n'importe quelle composition, decidait de ce que la carte chargerait au
// demarrage. Une composition dont les cues jouent des sons pouvait demarrer
// sans eux, une autre charger ce qu'elle n'emploie pas. Ses cues disent ce
// qu'elle emploie ; une copie a cote finit par mentir (CONVERGENCE §9.6).
namespace {
constexpr const char* NVS_ESPACE = "nidmi-audio";
/* Les cles retirees s'effacent au demarrage, pour ne pas laisser d'orphelines
 * dans le reservoir : l'ancien compteur du garde-fou (§157), l'ancien choix du
 * moteur (§187). */
constexpr const char* NVS_CLES_RETIREES[] = { "bootess", "moteur" };

/* LE GARDE-FOU DU BOOT COMPTE LES PLANTAGES (MESURES §157). Il comptait les
 * demarrages sans interface servie, en NVS : un instrument autonome qu'on
 * allume et eteint sans jamais ouvrir l'app coupait son son au 3e allumage —
 * et chaque demarrage ecrivait deux fois la flash, dont une pendant le son.
 * Il compte maintenant les PLANTAGES CONSECUTIFS (panique, chien de garde)
 * apres une restauration : un allumage, un redemarrage voulu, un OTA les
 * remettent a zero. En memoire RTC — elle survit a un plantage, pas a une
 * coupure de courant, qui n'est pas une boucle de plantage : plus aucune
 * ecriture en flash. (Une config qui affame la carte sans la planter sert la
 * page de secours, qui compte comme preuve de vie depuis le §138.) */
RTC_NOINIT_ATTR uint32_t rtcMagie;
RTC_NOINIT_ATTR uint8_t  rtcPlantages;
constexpr uint32_t MAGIE_RTC = 0x4E694433;   // « NiD3 »
// Réserve de bloc contigu à laisser au serveur après l'allocation de Plaits.
// Mesuré (MESURES.md §11, §16, §19) : la carte sert sa page en 0,05 s avec
// 16 372 o de plus gros bloc, et n'y arrive JAMAIS à 7 668. On garde 12 000.
constexpr uint32_t RESERVE_SERVICE = 12000;

// Le seuil de bascule à chaud se DÉDUIT du besoin, il n'est pas une constante :
// une image allégée (POOL_PLAITS = 1 024) doit pouvoir basculer là où l'image
// complète (16 384) ne le doit pas. Un seuil figé se trompait forcément dans
// l'un des deux cas.
static inline uint32_t seuilBasculeChaud() {
  return (uint32_t)POOL_PLAITS + (uint32_t)sizeof(plaits::Voice) + RESERVE_SERVICE;
}
Bascule derniereBasc = Bascule::Appliquee;

uint8_t  essaisAuBoot      = 0;        // plantages consecutifs comptes a ce demarrage
bool     restaurationCoupee = false;

// Ce que la composition emploie, pour le journal.
String decrire(int moteur, const char* son) {
  if (moteur == -2) return (son && *son)
                         ? String("le lecteur d'echantillons (clavier sur ") + son + ")"
                         : String("le lecteur d'echantillons (des play list, sans clavier)");
  if (moteur >= 0)  return String("le moteur de synthese n°") + moteur;
  return String("aucun moteur");
}

// Appele une seule fois, par restaurerAuBoot() (3 s apres le demarrage, sous le
// garde-fou des plantages consecutifs) : un echec ici ne doit pas pouvoir couter
// l'OTA.
void restaurer(int moteur, const char* son) {
  if (moteur == -2) {
    /* ON CHARGE TOUT, pas seulement celui-la. Une composition met des sons
     * differents sur des pistes differentes ; ne charger que le premier,
     * c'est arriver a la premiere cue avec un magasin incomplet. */
    const uint8_t n = SampleStore::chargerTout();
    moteurCourant = -2;
    Clavier& k = slots[_slotIci()].clavier;
    strncpy(k.nom, son ? son : "", sizeof(k.nom) - 1);
    Serial.printf("[audio] lecteur d'echantillons arme — %u en PSRAM, clavier sur %s\n",
                  (unsigned)n, k.nom);
  } else if (moteur >= 0) {
    if (!syntheseLourdeDisponible()) {
      /* Une composition ecrite pour une image qui accepte la synthese, ouverte
       * sur une image qui ne l'accepte pas. On l'ignore, et on le DIT — un
       * reglage qui disparait en silence est un piege. */
      Serial.println("[audio] la composition emploie un moteur de synthese : IGNORE, "
                     "cette image n'en accepte pas (carte seule). Voir MESURES.md §126.");
      return;
    }
    if (moteur <= 23 && plaitsAlloue()) {
      plaitsPatch.engine = moteur;
      moteurCourant = moteur;
      Serial.printf("[audio] moteur Plaits restaure : %d\n", moteur);
    }
  }
}
}  // namespace

/* Un choix humain explicite — un moteur, un son, choisis dans l'app — réarme le
 * garde-fou : c'est le seul chemin de sortie quand la restauration a été coupée
 * (la carte sert alors son UI, donc l'utilisateur peut choisir autre chose — ou
 * le même moteur, en connaissance de cause). */
void rearmerGardeFou() {
  rtcPlantages = 0;
  essaisAuBoot = 0;
  restaurationCoupee = false;
}

bool isStarted() { return demarre; }

// ── Restauration au boot, et son garde-fou ─────────────────────────────────
// Voir l'en-tête pour le pourquoi. Ici, le comment :
//
//   1. un démarrage qui ne suit pas un plantage remet le compteur (RTC) à zéro ;
//   2. le moteur que la composition ouverte emploie (déduit de ses cues par
//      l'appelant) — si elle n'en emploie aucun, on s'arrête là ;
//   3. si le compteur a atteint TENTATIVES_MAX, couper : la carte démarre nue.
//      C'est la protection de l'OTA que l'initialisation paresseuse assurait
//      avant, transposée au nouvel ordre ;
//   4. sinon compter cette tentative (avant le risque), puis démarrer l'audio
//      et restaurer.
//
// Plus aucune écriture en flash : le compteur vivait en NVS, et sa remise à
// zéro coûtait 1,8 % de blocs en retard le temps de l'écriture, une fois par
// boot (MESURES.md §13) — pendant le son. Il vit en RTC depuis le §157.
void restaurerAuBoot(int moteur, const char* son) {
  /* DEMARRAGE A VIDE, DEMANDE POUR CE SEUL DEMARRAGE. Le drapeau est consomme
   * ici : le suivant restaurera le son. La NVS n'est pas touchee — et le
   * compteur de tentatives non plus : ce n'est pas une tentative. */
  if (nidmi_prendreDemarrageAVide()) {
    Serial.println("[audio] boot : demarrage A VIDE demande — le son reviendra au suivant");
    return;
  }
  // Le compteur : un demarrage qui ne suit pas un plantage le remet a zero.
  const esp_reset_reason_t raison = esp_reset_reason();
  const bool apresPlantage = raison == ESP_RST_PANIC || raison == ESP_RST_INT_WDT
                          || raison == ESP_RST_TASK_WDT || raison == ESP_RST_WDT;
  if (rtcMagie != MAGIE_RTC) { rtcMagie = MAGIE_RTC; rtcPlantages = 0; }
  if (!apresPlantage) rtcPlantages = 0;
  essaisAuBoot = rtcPlantages;

  {
    Preferences p;
    bool retirer = false;
    if (p.begin(NVS_ESPACE, true)) {
      for (const char* cle : NVS_CLES_RETIREES) retirer = retirer || p.isKey(cle);
      p.end();
    }
    if (retirer && p.begin(NVS_ESPACE, false)) {  // avant le son : une fois
      for (const char* cle : NVS_CLES_RETIREES) if (p.isKey(cle)) p.remove(cle);
      p.end();
    }
  }
  const String choix = decrire(moteur, son);
  if (moteur == -1) {
    Serial.println("[audio] boot : la composition ouverte n'emploie aucun moteur, la carte demarre nue");
    return;
  }

  if (rtcPlantages >= TENTATIVES_MAX) {
    restaurationCoupee = true;
    Serial.printf("[audio] boot : restauration COUPEE — %u plantages de suite apres restauration.\n"
                  "        La carte demarre nue (la composition emploie %s).\n"
                  "        Choisir un process dans l'UI rearme le chargement.\n",
                  (unsigned)rtcPlantages, choix.c_str());
    return;
  }

  // Cette tentative compte AVANT le risque ; si la carte ne plante pas, le
  // prochain demarrage (qui ne suivra pas un plantage) la remettra a zero.
  // essaisAuBoot garde, lui, les plantages qui ont PRECEDE ce demarrage.
  rtcPlantages++;

  Serial.printf("[audio] boot : chargement de %s (tentative %u/%u), tas %lu o, plus gros bloc %lu o\n",
                choix.c_str(), (unsigned)essaisAuBoot, (unsigned)TENTATIVES_MAX,
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  if (!ensureStarted()) {
    Serial.println("[audio] boot : I2S indisponible — le reste du boitier continue");
    return;
  }
  restaurer(moteur, son);

  Serial.printf("[audio] boot : apres chargement, tas %lu o, plus gros bloc %lu o\n",
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

bool arreter() {
  if (!demarre) return true;

  /* FERMER L'I2S, pas seulement desactiver le moteur.
   *
   * setEngine(-1) ne fait que desélectionner : la tache continue de nourrir le
   * DMA et le peripherique pilote toujours BCK/LRCK/DIN. Mesure au moment de la
   * panne : DAC retire, bus {}, engine -1 — et pourtant started true avec les
   * blocs qui montent (61894 -> 63118 en 3 s). L'utilisateur avait toujours du
   * son, et les trois broches restaient pilotees alors que la carte les
   * declarait libres. « Pas de DAC declare, pas d'audio » exige la fermeture. */
  arretDemande = true;
  tacheArretee = false;
  for (int i = 0; i < 200 && !tacheArretee; i++) vTaskDelay(pdMS_TO_TICKS(5));
  if (!tacheArretee) {
    Serial.println("[audio] arret : la tache n'a pas rendu la main — on n'y touche pas");
    arretDemande = false;
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));      // laisser vTaskDelete s'achever

  i2s.end();
  if (evenements) { vQueueDelete(evenements); evenements = nullptr; }
  tache = nullptr;
  demarre = false;
  arretDemande = false;
  Serial.printf("[audio] arrete — I2S ferme, broches rendues, tas interne %lu o\n",
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

void validerConfigBoot() {
  // Memoire RTC : rien a ecrire en flash, donc appelable d'async_tcp. Une
  // restauration coupee attend un choix humain (rearmerGardeFou), pas une page.
  if (restaurationCoupee) return;
  rtcPlantages = 0;
  essaisAuBoot = 0;
}

/* LA GARDE D'ELECTION (MESURES §161). Sur le coeur 1, le tic de FreeRTOS ne
 * relance l'election que pour partager le temps entre taches de MEME priorite,
 * ou si un changement est reste en attente (xTaskIncrementTickOtherCores,
 * ESP-IDF 5.5) — jamais parce qu'une tache PLUS prioritaire est prete. Mesure :
 * reveillee sans que le coeur 1 soit prevenu, l'audio est restee « prete »
 * jusqu'a 500 ms pendant que loopTask (priorite 1) tournait seule — le DMA
 * rejouait ses anciens tampons. La garde se reveille toutes les 2 ms au-dessus
 * de loopTask : chaque reveil fait refaire l'election, et l'audio prete est
 * elue en 2 ms au plus, loin des 30 ms d'avance du DMA. Elle ne fait rien
 * d'autre que noter son propre retard. */
namespace {
/* Set when loopTask was at 1. Since §172 the loop (10) and the web server (9) are above it:
 * it re-elects only against what runs below 5 (idle). Raised to 11, or waking the audio
 * itself every 2 ms, it did not help (§242): during a burst of flash reads started on core 1,
 * the audio, woken, was not elected for ~83 ms while the guard and IDLE1 ran. What helped is
 * starting those bursts from core 0 (StorageIndex::run). */
constexpr UBaseType_t GARDE_PRIORITE = 5;       // < web (9), < loop (10), < audio (11)
constexpr uint32_t    GARDE_PERIODE_MS = 2;
volatile uint32_t gardePireRetardUs = 0;
TaskHandle_t garde = nullptr;

void gardeElection(void*) {
  TickType_t dernier = xTaskGetTickCount();
  uint32_t attendu = (uint32_t)esp_timer_get_time() + GARDE_PERIODE_MS * 1000;
  for (;;) {
    vTaskDelayUntil(&dernier, pdMS_TO_TICKS(GARDE_PERIODE_MS));
    const uint32_t t = (uint32_t)esp_timer_get_time();
    const int32_t retard = (int32_t)(t - attendu);
    if (retard > 0 && (uint32_t)retard > gardePireRetardUs) gardePireRetardUs = (uint32_t)retard;
    attendu += GARDE_PERIODE_MS * 1000;
    if ((int32_t)(t - attendu) > 0) attendu = t + GARDE_PERIODE_MS * 1000;   // pas de dette
  }
}
}  // namespace

bool ensureStarted() {
  if (demarre) return true;

  /* PAS DE DAC DECLARE, PAS D'AUDIO.
   *
   * Regle posee par l'utilisateur, et c'est la bonne : la carte decrit ce qui
   * est REELLEMENT branche. Tant que le DAC n'est pas declare dans l'inventaire
   * I/O, ses trois broches appartiennent a qui veut — un bouton, un
   * potentiometre — et ouvrir l'I2S dessus casserait ce qui s'y trouve. Dans
   * l'autre sens, un capteur qui sonde le BCK bloque la carte (MESURES.md §45).
   * Declarer le DAC est donc le geste qui autorise le son. */
  if (!Occupations::audioDeclare()) {
    Serial.println("[audio] demarrage refuse : aucun DAC declare. "
                   "Le declarer dans la zone I/O (broche D0) pour activer le son.");
    return false;
  }

  heapAvant = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  /* LA FILE DES NOTES VERS LE SON (MESURES §170). Elle tenait 32 evenements,
   * et un envoi sur une file pleine JETAIT la note — une note-off comprise :
   * une voix tenue pour toujours. 256, en PSRAM (seules des taches y touchent,
   * jamais cache coupe) : l'audio la vide a chaque bloc (2,5 ms), et le MIDI
   * USB n'en apporte pas le dixieme dans ce temps. */
  if (!stockageEvenements) {
    stockageEvenements = (uint8_t*)heap_caps_malloc(CAPACITE_EVENEMENTS * sizeof(Evenement),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stockageEvenements)
      stockageEvenements = (uint8_t*)heap_caps_malloc(CAPACITE_EVENEMENTS * sizeof(Evenement),
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (stockageEvenements)
    evenements = xQueueCreateStatic(CAPACITE_EVENEMENTS, sizeof(Evenement),
                                    stockageEvenements, &structureEvenements);
  if (!evenements) { Serial.println("[audio] file impossible"); return false; }

  // ── Reprendre les broches au domaine RTC avant de configurer l'I2S ───────
  // GPIO1 est le BCK de l'I2S ET la broche T1 du diagnostic tactile du boot
  // (NiDMI.cpp, touchDiag). touchRead() bascule la broche dans le mux RTC, et
  // i2s.begin() ne l'en sort PAS : plus d'horloge de bit, le DMA ne se vide
  // jamais, et la tâche audio se bloque définitivement dans i2s.write() après
  // avoir rempli le tampon.
  //
  // Symptôme mesuré : compteur de blocs figé à ~64 (soit exactement la taille
  // du DMA, 6 × 240 trames), quel que soit le moteur et quel que soit le moment
  // où l'audio démarre. Après ce correctif : 407 blocs/s, soit 48 840
  // échantillons/s — le temps réel, avec zéro sous-alimentation.
  //
  // L'en-tête de ce fichier pariait sur l'ORDRE (« le diagnostic touch est
  // passé depuis longtemps quand l'I2S s'installe »). L'ordre ne suffit pas :
  // ce que touchRead laisse derrière lui est un état de broche, pas une
  // occupation temporaire.
  // NE PAS appeler touch_pad_deinit() ici : l'API tactile LEGACY entre en
  // CONFLIT avec le nouveau driver tactile (IDF 5.5), et le boot part en
  // abort() en boucle — « legacy_touch_driver: CONFLICT! ». Le bon remède est
  // en amont : ne jamais SONDER GPIO1 (le BCK) au boot. Voir NiDMI.cpp,
  // TOUCH_BOOT_DIAG, désormais à 0 par défaut. MESURES.md §19, CORRECTIFS_AMONT §10.

  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] i2s.begin a echoue — le reste du boitier continue");
    vQueueDelete(evenements); evenements = nullptr;
    return false;
  }
  srReel = i2s.txSampleRate();
  // Les sondes du decrochage : poser le rappel exige un canal a l'arret.
  if (i2s_chan_handle_t tx = i2s.txChan()) {
    i2s_event_callbacks_t rappels = {};
    rappels.on_sent = sondeFinDeTampon;
    i2s_channel_disable(tx);
    i2s_channel_register_event_callback(tx, &rappels, nullptr);
    i2s_channel_enable(tx);
  }
  static bool ticPose = false;
  if (!ticPose) ticPose = esp_register_freertos_tick_hook_for_cpu(sondeTic, 1) == ESP_OK;
  if (!srReel) srReel = SAMPLE_RATE;

  // Couper l'économie d'énergie WiFi. MESURÉ sur cette carte, avant/après :
  // l'aller-retour passait de 141 ms de moyenne (min 28, max 261, écart-type 76)
  // à quelque chose d'utilisable. La station ne se réveillant qu'aux balises
  // DTIM, chaque note jouée depuis l'interface attendait son tour — d'où une
  // latence de clavier inacceptable.
  // Le firmware le faisait déjà, mais uniquement dans OSCQueue::begin() : donc
  // seulement si la file OSC démarrait. On le fait ici parce que dès qu'il y a
  // du son, la latence prime sur les milliampères.
  WiFi.setSleep(false);

  // Cœur 1, priorité 11 : au-dessus d'async_tcp (10), et loin de la pile WiFi
  // qui vit sur le cœur 0. Voir l'en-tête.
  ProcessCosts::begin();                 // the meters, and what this firmware measured before (§231)
  BlockLoads::begin();                   // each block's load, live (§235)
  if (xTaskCreatePinnedToCore(boucleAudio, "audio", 4096, nullptr, 11, &tache, 1) != pdPASS) {
    Serial.println("[audio] tache impossible");
    i2s.end(); vQueueDelete(evenements); evenements = nullptr;
    return false;
  }
  // La garde d'election, sur le meme coeur (voir plus haut). Une seule, pour
  // toute la vie de la carte : elle ne depend pas de la tache audio.
  if (!garde && xTaskCreatePinnedToCore(gardeElection, "garde", 1280, nullptr,
                                        GARDE_PRIORITE, &garde, 1) != pdPASS)
    Serial.println("[audio] garde d'election impossible");

  heapApres = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  demarre = true;
  // NE RESTAURE PLUS ICI. Le chemin paresseux (première note) est précisément
  // l'ordre fatal : à ce moment la page est chargée et le tas haché. La
  // restauration est passée au boot, dans restaurerAuBoot().
  Serial.printf("[audio] demarre — %lu Hz reels, heap interne %lu -> %lu (cout %ld o)\n",
                (unsigned long)srReel, (unsigned long)heapAvant,
                (unsigned long)heapApres, (long)heapAvant - (long)heapApres);
  return true;
}

// Doit rester d'accord avec les NIDMI_LOURD de plaits/dsp/voice.cpp
// (hardware/bench/plaits-instrumentation.patch) : six_op x3 (2,3,4),
// string_machine (6), speech (15), particle (18), string (19).
bool syntheseLourdeDisponible() { return NIDMI_SYNTH_LOURDE != 0; }

const char* moteursSubstitues() {
#ifdef PLAITS_LEGER
  return "2,3,4,6,15,18,19";
#else
  return "";
#endif
}

// PLAY : la porte s'ouvre. Le moteur reste charge (aucune reallocation), on ne
// fait que laisser passer le son.
//
// ⚠️ PORTEE DE CE CHOIX — il ne vaut QUE pour la carte unique.
// Ici le process est FIGE : sur 8 Mo avec l'interface embarquee, on ne peut pas
// changer de famille de moteur en cours de session (MESURES.md §15, l'ordre
// d'allocation). Garder Plaits charge au STOP est donc gratuit, et evite une
// reallocation risquee.
// En architecture MULTI-CARTES (V2, ETUDE_V2_FERME.md), la regle S'INVERSE : le
// principe meme est de CHANGER de process selon les cues, donc le STOP doit
// DECHARGER pour liberer le worker. Ne pas transposer ce comportement tel quel.
void ouvrirSon() {
  gSilence = false;
}

void couperSon() {
  bipBlocsRestants = 0;                   // coupe le bip de test
  for (auto& v : voix) v.cible = 0.0f;    // le sinus s'eteint
  clearPendingNotes = true;               // a note that was waiting does not start afterwards
  /* The voices go silent with the gate, in its 10 ms fade; then the slots that were playing are
   * drained (§211-212). Without the audio task, nobody fades: they stop now. */
  if (!demarre) arreterEchantillon();
  else {
    for (uint8_t s = 0; s < CUE_SLOTS; s++) stopGen[s] = slots[s].gen;
    __sync_synchronize();
    stopVoices = true;
  }
  slotLeaving = -1;                        // STOP ends the cue in its Release too
  __sync_synchronize();
  gSilence = true;                        // la porte se ferme
}

/* UNE NOTE NE DEMARRE PLUS LE MOTEUR. Comme noteOff, elle ne joue que si un
 * moteur a ete CHARGE deliberement — par setEngine(n>=0), setSampler ou un bip
 * de test, qui appellent ensureStarted() chacun a leur tour.
 *
 * Elle le demarrait, au titre de l'echo « le boitier s'entend lui-meme ». Le
 * cout, mesure : la PREMIERE note emise par un script de broche demarrait
 * l'I2S, qui garde ~16 ko et ne les rend jamais (/api/audio/stop repond « ok »
 * sans rien liberer). Le plus grand bloc contigu tombait de 30 708 a 11 764
 * octets — c'est lui, et non le tas total, qui decide si AsyncTCP peut
 * constituer ses tampons. Un appui sur un bouton coutait donc, definitivement,
 * la marge reseau de la carte, et l'OTA devenait capricieux dans la foulee
 * (MESURES.md §72.2).
 *
 * Et c'est la volonte de l'usager : pas de son tant qu'un moteur audio n'a pas
 * ete charge. Un boitier qui chante sans qu'on le lui ait demande est une
 * surprise, pas une fonction. */
/* PERDRE UNE NOTE EST UNE FAUTE, une note-off surtout (regle du 24/09). File
 * pleine, on ATTEND qu'un bloc la vide — 2,5 ms — plutot que de jeter : un
 * retard d'un bloc s'entend a peine, une voix bloquee s'entend toujours. Au
 * plus 10 ms : au-dela, l'audio est deja arrete, et le MIDI ne doit pas
 * rester pendu a lui. Ce qui se perd alors se COMPTE (notesPerdues). */
static void deposerEvenement(const Evenement& e) {
  if (xQueueSend(evenements, &e, pdMS_TO_TICKS(10)) != pdTRUE) notesPerduesCompte = notesPerduesCompte + 1;
}

/* The slots a note is for, when it is deposited (§212): the branch of the chain it came out of
 * says which cue; everything else reaches every slot that is not being cut. */
static uint8_t _masque(Pour pour) {
  if (pour == Pour::COURANTE) { const int8_t s = slotCurrent; return (s >= 0) ? (uint8_t)(1u << s) : 0; }
  if (pour == Pour::SORTANTE) { const int8_t s = slotLeaving; return (s >= 0) ? (uint8_t)(1u << s) : 0; }
  uint8_t m = 0;
  for (uint8_t s = 0; s < CUE_SLOTS; s++) {
    const uint8_t st = slots[s].state;
    if (st != SLOT_CUTTING && st != SLOT_DRAINED) m |= (uint8_t)(1u << s);
  }
  return m;
}

uint8_t masqueSlots(Pour pour) { return _masque(pour); }

void controleKits(uint8_t cc, uint8_t valeur, Pour pour, uint8_t group) {
  const int8_t sl = slotLeaving;
  for (uint8_t m = 0; m < SfzStore::MOTEURS; m++) {
    /* The slot that plays this kit, if one does: whose branch it is, and its group (§222). */
    int8_t chez = -1;
    for (uint8_t s = 0; s < CUE_SLOTS && chez < 0; s++)
      for (const int8_t k : slots[s].kit) if (k == (int8_t)m) { chez = (int8_t)s; break; }
    const bool sien = (sl >= 0 && chez == sl);
    if (!(pour == Pour::SORTANTE ? sien : (pour == Pour::COURANTE ? !sien : true))) continue;
    const bool ici = group == GROUP_ALL || chez < 0 || _forGroup(group, slots[chez], SfzStore::blocId(m));
    if (ici) SfzStore::controle(m, cc, valeur);
  }
}

void fixerParamSynth(const char* nom, float v, Pour pour) {
  auto poser = [&](Params& p) {
    if      (!strcmp(nom, "harmonics"))  p.harmonics = v;
    else if (!strcmp(nom, "timbre"))     p.timbre    = v;
    else if (!strcmp(nom, "morph"))      p.morph     = v;
    else if (!strcmp(nom, "decay"))      p.decay     = v;
    else if (!strcmp(nom, "lpg_colour")) p.lpgColour = v;
    else if (!strcmp(nom, "drone"))      p.drone     = v;
  };
  const uint8_t m = _masque(pour);
  const int8_t sc = slotCurrent;
  /* The cue that plays, through setParams: its voice and the engine's own copy (what
   * /api/audio/params reads) — also before any cue. */
  if (pour != Pour::SORTANTE && (sc < 0 || (m & (1u << sc)))) { Params p = params(); poser(p); setParams(p); }
  for (uint8_t s = 0; s < CUE_SLOTS; s++) {
    if (!(m & (1u << s)) || (int8_t)s == sc) continue;
    for (const int8_t sv : {slots[s].synth, slots[s].synth2}) {
      if (sv < 0) continue;
      Params p = SynthVoice::params((uint8_t)sv); poser(p); SynthVoice::setParams((uint8_t)sv, p);
    }
  }
}

void noteOn(uint8_t canal, uint8_t note, uint8_t velocity, Pour pour, uint8_t group) {
  if (!demarre) return;
  deposerEvenement(Evenement{note, velocity, SORTE_NOTE, 0, 0, canal, _masque(pour), group});
}

void noteOff(uint8_t canal, uint8_t note, Pour pour, uint8_t group) {
  if (!demarre) return;
  deposerEvenement(Evenement{note, 0, SORTE_NOTE, 0, 0, canal, _masque(pour), group});
}

void setBlockGroups(uint8_t slot, const uint32_t* blocks, const uint8_t* groups, uint8_t n) {
  if (slot >= CUE_SLOTS) return;
  CueSlot& c = slots[slot];
  c.nGroups = 0;
  __sync_synchronize();
  if (n > GROUPS_PER_SLOT) n = GROUPS_PER_SLOT;
  for (uint8_t i = 0; i < n; i++) c.groups[i] = CueSlot::BlockGroup{blocks[i], groups[i] > GROUP_MAX ? GROUP_MAX : groups[i]};
  __sync_synchronize();
  c.nGroups = n;
}

uint32_t instrumentGroups() {
  uint32_t m = 0;
  const uint8_t vivants = _masque(Pour::TOUS);
  for (uint8_t s = 0; s < CUE_SLOTS; s++) {
    if (!(vivants & (1u << s))) continue;
    const CueSlot& c = slots[s];
    if (c.clavier.nom[0]) m |= 1u << _groupOf(c, c.clavier.bloc);
    const int8_t sv = c.synth;
    for (const int8_t kit : c.kit) if (kit >= 0) m |= 1u << _groupOf(c, SfzStore::blocId((uint8_t)kit));
    if (sv >= 0)  m |= 1u << _groupOf(c, SynthVoice::bloc((uint8_t)sv));
    if (banques) {
      const Banque& b = banques[banqueActive];
      for (uint8_t li = 0; li < b.n; li++) if (b.listes[li].slot == s) m |= 1u << _groupOf(c, b.listes[li].bloc);
    }
  }
  return m;
}

bool armerCapture(uint32_t frames) {
  captureArmed = false;
  vTaskDelay(pdMS_TO_TICKS(5));               // the audio task leaves the buffer
  if (captureBuf && frames > captureFrames) { heap_caps_free(captureBuf); captureBuf = nullptr; }
  if (!captureBuf) captureBuf = (int16_t*)heap_caps_malloc(frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!captureBuf) return false;
  captureFrames = frames;
  captureFilled = 0;
  captureArmed = true;
  return true;
}

bool capture(const int16_t*& data, uint32_t& frames, bool& finie) {
  data = captureBuf;
  frames = captureFilled;
  finie = captureBuf && !captureArmed && captureFilled >= captureFrames;
  return captureBuf != nullptr;
}

/* ── THE CUE SLOTS, the cue sequencer's side (§212) ─────────────────────── */
uint8_t etatSlot(uint8_t slot) {
  if (slot >= CUE_SLOTS) return SLOT_LIBRE;
  /* A request posted and not applied yet counts as done: the sequencer asked for it. */
  switch (slots[slot].req) {
    case REQ_ARRIVE: return SLOT_COURANT;
    case REQ_LEAVE:  return SLOT_SORTANT;
    case REQ_CUT:    return SLOT_COUPE;
    default: break;
  }
  switch (slots[slot].state) {
    case SLOT_CURRENT: return SLOT_COURANT;
    case SLOT_LEAVING: return SLOT_SORTANT;
    case SLOT_CUTTING: return SLOT_COUPE;
    case SLOT_DRAINED: return SLOT_VIDE;
    default:           return SLOT_LIBRE;
  }
}
int8_t slotCourant() { return slotCurrent; }
int8_t slotSortant() { return slotLeaving; }

void arriverSlot(uint8_t slot, float delayS, float attackS, float sustain) {
  if (slot >= CUE_SLOTS) return;
  CueSlot& c = slots[slot];
  c.reqDelayS  = delayS > 0.0f ? delayS : 0.0f;
  c.reqAttackS = attackS > 0.0f ? attackS : 0.0f;
  c.reqSustain = (sustain >= 0.0f) ? (sustain > 1.0f ? 1.0f : sustain) : 1.0f;
  c.gen = c.gen + 1;
  __sync_synchronize();                      // the parameters before the request
  c.req = REQ_ARRIVE;
  if (!demarre) { c.state = SLOT_CURRENT; c.req = REQ_NONE; c.env = EnvCue{}; }   // nobody renders: no envelope to run
  if (slotLeaving == (int8_t)slot) slotLeaving = -1;
  slotCurrent = (int8_t)slot;
}

void quitterSlot(uint8_t slot, float releaseS) {
  if (slot >= CUE_SLOTS) return;
  CueSlot& c = slots[slot];
  c.reqReleaseS = releaseS > 0.0f ? releaseS : 0.0f;
  __sync_synchronize();
  c.req = REQ_LEAVE;
  slotLeaving = (int8_t)slot;
  if (slotCurrent == (int8_t)slot) slotCurrent = -1;
  if (!demarre) { c.req = REQ_NONE; _drainSlot(slot); }
}

void couperSlot(uint8_t slot) {
  if (slot >= CUE_SLOTS) return;
  CueSlot& c = slots[slot];
  if (c.state == SLOT_FREE && c.req == REQ_NONE) {   // nothing plays in it: drained at once, by the loop's hand
    c.state = SLOT_DRAINED;
  } else {
    c.req = REQ_CUT;
    if (!demarre) { c.req = REQ_NONE; _drainSlot(slot); }
  }
  if (slotLeaving == (int8_t)slot) slotLeaving = -1;
}

/* What goes on passes from `from` to `to` (§238): the kits, to the arriving cue's positions; the
 * synth voices; the play lists of those blocks, with their voices (one fading out has left its
 * list: it stays). Plain writes — switchSlots holds the audio task off meanwhile, so its active
 * bank is not rewritten under it. */
static void _handOver(uint8_t from, uint8_t to, const HandOver& h) {
  CueSlot& a = slots[from];
  CueSlot& n = slots[to];
  for (uint8_t i = 0; i < KITS_PER_SLOT; i++) {
    const int8_t m = h.kit[i];
    if (m < 0) continue;
    for (uint8_t j = 0; j < KITS_PER_SLOT; j++) if (a.kit[j] == m) { a.kit[j] = -1; break; }
    n.kit[i] = m;
    n.kitCanal[i] = h.kitChannel[i] > 16 ? 0 : h.kitChannel[i];
  }
  if (h.synth) {
    n.synth = a.synth; n.synth2 = a.synth2; n.voixVoulues = a.voixVoulues;
    a.synth = -1; a.synth2 = -1;
  }
  if (!banques) return;
  Banque& b = banques[banqueActive];
  for (uint8_t li = 0; li < b.n; li++) {
    bool goesOn = false;
    for (const uint32_t bloc : h.lists) goesOn = goesOn || (bloc && bloc == b.listes[li].bloc);
    if (!goesOn || b.listes[li].slot != from) continue;
    b.listes[li].slot = to;
    for (uint8_t v = 0; v < VOIX_MAX; v++)
      if (voixEch[v].actif && voixEch[v].slot == from && voixEch[v].liste == (int8_t)li) voixEch[v].slot = to;
  }
}

void switchSlots(int8_t from, uint8_t to, const HandOver& h, float releaseS,
                 float delayS, float attackS, float sustain) {
  if (to >= CUE_SLOTS) return;
  const bool leaves = from >= 0 && from < (int8_t)CUE_SLOTS && from != (int8_t)to;
  /* Nobody renders: nothing to hold off — and a slot that leaves is drained at once (its kit's
   * voices, its effects), which is no work for a section with interrupts off. */
  const bool hold = demarre;
  if (hold) portENTER_CRITICAL(&slotsMux);
  if (leaves) _handOver((uint8_t)from, to, h);
  arriverSlot(to, delayS, attackS, sustain);
  if (leaves) {
    if (releaseS > 0.0f) quitterSlot((uint8_t)from, releaseS);
    else                 couperSlot((uint8_t)from);
  }
  if (hold) portEXIT_CRITICAL(&slotsMux);
}

void libererSlot(uint8_t slot) {
  if (slot >= CUE_SLOTS) return;
  CueSlot& c = slots[slot];
  c.clavier = Clavier{};
  for (uint8_t k = 0; k < KITS_PER_SLOT; k++) { c.kit[k] = -1; c.kitCanal[k] = 0; }
  c.synth = -1; c.synth2 = -1;
  c.nGroups = 0;
  __sync_synchronize();
  c.state = SLOT_FREE;
  if (slotLeaving == (int8_t)slot) slotLeaving = -1;
}

int8_t kitDuSlot(uint8_t slot, uint8_t i) { return (slot < CUE_SLOTS && i < KITS_PER_SLOT) ? slots[slot].kit[i] : -1; }
int8_t synthDuSlot(uint8_t slot) { return slot < CUE_SLOTS ? slots[slot].synth : -1; }
int8_t synth2DuSlot(uint8_t slot) { return slot < CUE_SLOTS ? slots[slot].synth2 : -1; }
void   fixerSynth2Slot(uint8_t slot, int8_t voix) { if (slot < CUE_SLOTS) slots[slot].synth2 = voix; }
void fixerVoixVoulues(uint8_t slot, uint8_t n) { if (slot < CUE_SLOTS) slots[slot].voixVoulues = (n >= 2) ? 2 : 1; }

bool ajusterVoix(uint8_t slot) {
  if (slot >= CUE_SLOTS) return false;
  CueSlot& c = slots[slot];
  const int8_t v1 = c.synth, v2 = c.synth2;
  if (c.voixVoulues >= 2 && v1 >= 0 && v2 < 0) {
    const int8_t libre = voixSynthLibre();
    if (libre < 0) return false;            // the other cue holds it: when it lets it go
    String r;
    c.synth2 = libre;
    if (!SynthVoice::clone((uint8_t)v1, (uint8_t)libre, r)) { c.synth2 = -1; return false; }
    return true;
  }
  if ((c.voixVoulues < 2 || v1 < 0) && v2 >= 0) {
    c.synth2 = -1;
    String r;
    SynthVoice::configure((uint8_t)v2, -1, 0, 0, 1.0f, r);
    return true;
  }
  return false;
}

int8_t voixSynthLibre() {
  for (uint8_t v = 0; v < SynthVoice::VOICES; v++) {
    bool tenue = false;
    for (const CueSlot& c : slots) tenue = tenue || c.synth == (int8_t)v || c.synth2 == (int8_t)v;
    if (!tenue) return (int8_t)v;
  }
  return -1;
}
void   fixerKitSlot(uint8_t slot, uint8_t i, int8_t moteur) { if (slot < CUE_SLOTS && i < KITS_PER_SLOT) slots[slot].kit[i] = moteur; }
int8_t moteurKitLibre() {
  for (uint8_t m = 0; m < SfzStore::MOTEURS; m++) {
    bool tenu = false;
    for (const CueSlot& c : slots) for (const int8_t k : c.kit) tenu = tenu || k == (int8_t)m;
    if (!tenu) return (int8_t)m;
  }
  return -1;
}
void   fixerSynthSlot(uint8_t slot, int8_t voix) { if (slot < CUE_SLOTS) slots[slot].synth = voix; }

String etatSlots() {
  static const char* const NOMS[] = {"free", "current", "leaving", "cutting", "drained"};
  String j = "[";
  for (uint8_t s = 0; s < CUE_SLOTS; s++) {
    const CueSlot& c = slots[s];
    uint8_t voix = 0;
    for (uint8_t v = 0; v < VOIX_MAX; v++) voix += (voixEch[v].actif && voixEch[v].slot == s) ? 1 : 0;
    uint8_t listes = 0;
    if (banques) for (uint8_t li = 0; li < banques[banqueActive].n; li++) listes += (banques[banqueActive].listes[li].slot == s) ? 1 : 0;
    char t[400];
    snprintf(t, sizeof t, "%s{\"slot\":%u,\"state\":\"%s\",\"level\":%.3f,\"target\":%.3f,\"cut\":%.3f,"
                          "\"voices\":%u,\"lists\":%u,\"keyboard\":\"%s\",\"keyboard_ch\":%u,\"keyboard_poly\":%u,\"kit\":%d,\"kit2\":%d,"
                          "\"synth\":%d,\"synth_on\":%s,\"synth2\":%d,\"gen\":%lu}",
             s ? "," : "", (unsigned)s, NOMS[c.state < 5 ? c.state : 0], c.env.now, c.env.target, c.cut,
             (unsigned)voix, (unsigned)listes, c.clavier.surCue ? "" : c.clavier.nom, (unsigned)c.clavier.canal,
             (unsigned)c.clavier.poly, (int)c.kit[0], (int)c.kit[1], (int)c.synth, (c.synth >= 0 && SynthVoice::active((uint8_t)c.synth)) ? "true" : "false",
             (int)c.synth2, (unsigned long)c.gen);
    j += t;
    /* Its instrument blocks' groups (§221): "bloc:group,…". */
    j.remove(j.length() - 1);
    j += ",\"groups\":\"";
    for (uint8_t i = 0; i < c.nGroups && i < GROUPS_PER_SLOT; i++)
      j += String(i ? "," : "") + String((unsigned long)c.groups[i].bloc) + ":" + String((unsigned)c.groups[i].group);
    j += "\"}";
  }
  return j + String("],\"current\":") + (int)slotCurrent + ",\"leaving\":" + (int)slotLeaving;
}

void fixerCanaux(uint8_t clavier, const uint8_t* kits) {
  CueSlot& c = slots[_slotIci()];
  c.clavier.canal = clavier > 16 ? 0 : clavier;
  for (uint8_t k = 0; k < KITS_PER_SLOT; k++) c.kitCanal[k] = kits[k] > 16 ? 0 : kits[k];
}

uint32_t notesPerdues() { return notesPerduesCompte; }

void testTone(float hz, uint32_t ms) {
  if (!ensureStarted()) return;
  bipHz = hz;
  bipPhase = 0.0f;
  bipBlocsRestants = hz > 0.0f ? (ms * srReel) / (1000UL * FRAMES) : 0;
}

// Rend les ~26,6 ko de Plaits au tas. INDISPENSABLE et pas cosmétique : avec
// Plaits résident il ne reste que 7,7 ko de plus gros bloc, et AsyncTCP n'a
// alors plus de quoi constituer ses tampons — la page ne se sert plus (mesuré :
// au-delà de 120 s pour /). Les deux pics ne doivent pas coïncider : on charge
// l'app moteur libéré, puis on alloue. Voir hardware/bench/MESURES.md §10.
void libererPlaits() {
  if (!plaitsVoix) return;
  // La tâche audio (cœur 1, prio 11) doit cesser de lire l'objet avant qu'on le
  // détruise. Elle repasse au sinus dès son bloc suivant (2,5 ms) ; on lui en
  // laisse huit. L'appelant est le gestionnaire HTTP, de priorité INFÉRIEURE :
  // le vTaskDelay lui rend donc bien la main.
  moteurCourant = -1;
  vTaskDelay(pdMS_TO_TICKS(20));
  plaitsVoix->~Voice();
  heap_caps_free(plaitsVoix); plaitsVoix = nullptr;
  heap_caps_free(plaitsMem);  plaitsMem  = nullptr;
  plaitsOctets = 0;
  Serial.printf("[audio] Plaits libere — tas interne %lu o, plus gros bloc %lu o\n",
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* DECLENCHER SANS NOTE, a la hauteur du fichier. C'est ce que fait `play-sf`
 * a l'arrivee sur une cue : on part de zero, on lit au rythme du fichier (la
 * seule correction est l'ecart entre sa frequence et celle que l'I2S a
 * reellement obtenue), et on boucle si la cue le demande. */

/* Declenche UN echantillon, nomme. `demiTons` = 0 signifie « a la hauteur du
 * fichier » — le cas de play-sf sur cue ; le clavier passe un ecart. `gain`
 * negatif = non precise, donc plein. */
bool declencherEchantillon(const char* nom, bool boucle, float gain, float demiTons,
                           uint32_t bloc, float velo, int8_t slot) {
  if (moteurCourant != -2) return false;
  const int i = SampleStore::indexDe(nom);
  if (i < 0) return false;
  if (SampleStore::isStreamed((uint8_t)i)) {
    /* A streamed sound has no PCM in memory: it cannot be transposed, nor played on a
     * note — only a playlist can read it (SdStream.h). Said, not mute. */
    NIDMI_WEB_LOG("[audio] %s est lu en flux depuis la SD : seulement dans une play list", nom);
    return false;
  }
  VoixEch* vo = _voixLibre();
  /* ETEINTE PENDANT QU'ON LA REECRIT. La tache audio, plus prioritaire, peut
   * lire la voix entre deux ecritures — celles d'une cue ou d'un apercu
   * viennent d'une autre tache. Une voix volee a moitie reecrite lirait le
   * nouvel echantillon a l'ANCIENNE position, au-dela de sa fin. La barriere
   * interdit au compilateur de supprimer ou de deplacer l'extinction. */
  vo->actif  = false;
  __sync_synchronize();
  vo->stream = -1; vo->head = -1;      // a voice stolen from a streamed clip keeps neither its stream nor its head
  vo->iEch   = (uint8_t)i;
  vo->pas    = _fixedStep((double(SampleStore::frequence((uint8_t)i)) / double(srReel))
                          * ((demiTons == 0.0f) ? 1.0 : pow(2.0, double(demiTons) / 12.0)));
  vo->pos    = 0;
  vo->velo   = !(velo >= 0.f) ? 0.0f : ((velo > 1.f) ? 1.0f : velo);
  vo->cible  = vo->velo * ((gain < 0.f) ? 1.0f : _borneGain(gain));
  vo->gain   = vo->cible;              // l'attaque part a son niveau : pas de fondu
  vo->bloc   = bloc;
  vo->boucle = boucle;
  vo->age    = ++voixHorloge;
  vo->picG   = 0; vo->picD = 0;        // une voix volee ne garde pas la crete d'avant
  /* Une voix de play-sf n'est pas un clip (§196) : le fichier entier, aucune
   * liste — une voix volee a une liste ne doit rien en garder. */
  vo->liste  = -1; vo->clip = 0; vo->debutT = 0; vo->finT = 0; vo->fondu = 0.0f;
  vo->slot   = (slot >= 0 && slot < (int8_t)CUE_SLOTS) ? (uint8_t)slot : _slotIci();   // its cue's slot (§212)
  __sync_synchronize();
  vo->actif  = true;
  return true;
}

/* LE VOLUME D'UN BLOC PENDANT QU'IL JOUE (MESURES §165). Le gain d'une voix
 * etait fixe a son declenchement : tourner Volume, ou un fader, ne s'entendait
 * qu'a la cue suivante. On deplace la CIBLE de chaque voix du bloc — la voix y
 * glisse dans rendreSample() — et le gain que le clavier donnera aux notes
 * suivantes, si le bloc est le sien. Un seul flottant par voix : l'ecriture est
 * atomique, la tache audio lit l'ancienne valeur ou la nouvelle. */
uint8_t fixerGainBloc(uint32_t bloc, float gain) {
  /* The SFZ kit is one block's too: its fader is the same living path (§208). The count of voices
   * stays the sampler's own — the kit answers through SfzStore. */
  SfzStore::fixerGainBloc(bloc, gain);
  SynthVoice::setBlockGain(bloc, gain);    // the synth voice is one block's too (§210)
  if (!bloc) return 0;                     // 0 = voix sans bloc : on n'y touche pas
  const float g = _borneGain(gain);
  /* Une play list de ce bloc (§196) : ses clips A VENIR aussi. Un flottant
   * aligne, ecrit d'un coup : la tache audio lit l'ancien ou le nouveau. */
  if (banques) {
    Banque& b = banques[banqueActive];
    for (uint8_t li = 0; li < b.n; li++) if (b.listes[li].bloc == bloc) b.listes[li].gain = g;
  }
  uint8_t n = 0;
  for (uint8_t v = 0; v < VOIX_MAX; v++) {
    VoixEch& vo = voixEch[v];
    if (vo.actif && vo.bloc == bloc) { vo.cible = vo.velo * g; n++; }
  }
  for (CueSlot& c : slots) if (c.clavier.bloc == bloc) c.clavier.gain = g;   // each cue's keyboard (§212)
  return n;
}

/* LES VU-METRES (MESURES §169). `fixerMesureVu` : loopTask dit si un onglet
 * ecoute ; sans lui, les voix ne mesurent rien. `releverCretes` : la plus haute
 * valeur de chaque voix depuis la derniere lecture, rangee par BLOC (plusieurs
 * voix d'un meme bloc : la plus haute), apres le volume de sortie. Remet les
 * cretes a zero. Un bloc muet n'y figure pas. Lecture et remise se croisent
 * sans verrou avec la tache audio : une crete peut s'y perdre, un vu-metre ne
 * s'en apercoit pas. */
void fixerMesureVu(bool oui) { mesureVu = oui; }

uint8_t releverCretes(uint32_t* blocs, uint16_t* cretesG, uint16_t* cretesD, uint8_t max) {
  uint8_t n = 0;
  const float sortie = gVolume;
  for (uint8_t i = 0; i < VOIX_MAX; i++) {
    VoixEch& vo = voixEch[i];
    const uint16_t pg = vo.picG, pd = vo.picD;
    if (!pg && !pd) continue;
    vo.picG = 0; vo.picD = 0;
    if (!vo.bloc) continue;
    const uint16_t g = (uint16_t)fminf(65535.f, pg * sortie);
    const uint16_t d = (uint16_t)fminf(65535.f, pd * sortie);
    uint8_t k = 0;
    while (k < n && blocs[k] != vo.bloc) k++;
    if (k == n) {
      if (n >= max) continue;
      blocs[n] = vo.bloc; cretesG[n] = 0; cretesD[n] = 0; n++;
    }
    if (g > cretesG[k]) cretesG[k] = g;
    if (d > cretesD[k]) cretesD[k] = d;
  }
  /* The kits (one per engine) and the synth voices (one per slot), §220: each its block, beside
   * the sample voices. */
  for (uint8_t t = 0; t < SfzStore::MOTEURS + CUE_SLOTS; t++) {
    PeakBlock& p = (t < SfzStore::MOTEURS) ? kitPeak[t] : synthPeak[t - SfzStore::MOTEURS];
    const uint16_t pg = p.g, pd = p.d;
    if (!pg && !pd) continue;
    p.g = 0; p.d = 0;
    if (!p.bloc) continue;
    const uint16_t g = (uint16_t)fminf(65535.f, pg * sortie);
    const uint16_t d = (uint16_t)fminf(65535.f, pd * sortie);
    uint8_t k = 0;
    while (k < n && blocs[k] != p.bloc) k++;
    if (k == n) {
      if (n >= max) continue;
      blocs[n] = p.bloc; cretesG[n] = 0; cretesD[n] = 0; n++;
    }
    if (g > cretesG[k]) cretesG[k] = g;
    if (d > cretesD[k]) cretesD[k] = d;
  }
  return n;
}

/* Le bloc que le CLAVIER joue, et son volume : le premier son d'une cue, ou
 * celui d'un apercu. */
void fixerClavier(uint32_t bloc, float gain) {
  Clavier& k = slots[_slotIci()].clavier;
  k.bloc = bloc;
  k.gain = (gain < 0.f) ? 1.0f : _borneGain(gain);
}

/* Arrete UNE voix par son echantillon — quitter une cue coupe ce qu'elle avait
 * lance, sans toucher a ce qu'une autre piste tient. */
void arreterEchantillonNomme(const char* nom) {
  const int i = SampleStore::indexDe(nom);
  if (i < 0) return;
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (voixEch[v].actif && voixEch[v].iEch == (uint8_t)i) voixEch[v].actif = false;
}

/* Quitter la cue arrete le son — comme le `dispose()` du BufferSource cote
 * navigateur. Sans ca, une boucle survivrait a la cue qui l'a lancee. */
void arreterEchantillon(bool listesComprises) {
  if (listesComprises) clearPendingNotes = true;   // STOP: a note that was waiting does not start afterwards
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (listesComprises || voixEch[v].liste < 0) voixEch[v].actif = false;
}

/* ── LES PLAY LIST, COTE ECRIVAINS (§196) ─────────────────────────────────
 * La cue (loopTask ou serveur web), le chemin vivant et l'ecoute (serveur
 * web), un son qui arrive ou part. L'un apres l'autre, sous verrouListes. */
namespace {
struct VerrouListes {
  VerrouListes()  { xSemaphoreTake(verrouListes, portMAX_DELAY); }
  ~VerrouListes() { xSemaphoreGive(verrouListes); }
};

bool _assurerBanques() {
  if (banques) return true;
  /* Deux banques a zero : deux banques vides. En PSRAM — ~15 Ko que le tas
   * interne n'a pas a porter ; la tache audio n'y lit qu'aux bornes d'un clip
   * et a chaque note, jamais a chaque echantillon. */
  banques = (Banque*)heap_caps_calloc(2, sizeof(Banque), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!banques) Serial.println("[audio] play list : 2 banques impossibles en PSRAM");
  return banques != nullptr;
}

/* Attendre que la proposition en cours soit adoptee — au plus 200 ms ; la
 * tache audio l'adopte au bloc suivant (2,5 ms). Si elle ne tourne pas,
 * personne d'autre ne lit les banques : on adopte soi-meme. */
bool _attendreAdoption() {
  for (int i = 0; i < 200 && banqueProposee >= 0; i++) {
    if (!demarre) { _adopter(banqueProposee); break; }
    vTaskDelay(1);
  }
  return banqueProposee < 0;
}

/* La banque que la tache audio NE LIT PAS — a remplir puis proposer. Nul si
 * une proposition reste pendante : jamais on ne reecrit ce qu'elle va lire. */
Banque* _banqueLibre() {
  if (!_assurerBanques() || !_attendreAdoption()) return nullptr;
  return &banques[1 - banqueActive];
}

void _proposer(Banque* b) {
  __sync_synchronize();
  banqueProposee = (int8_t)(b - banques);
  /* Adoptee avant de rendre la main : l'appelant peut enchainer (jouer un
   * clip), et le suivant trouvera la banque libre. */
  _attendreAdoption();
}

// La valeur de `cle` dans « cle=valeur;cle=valeur » ; "" si absente.
String _valeurParam(const String& params, const char* cle) {
  int debut = 0;
  while (debut < (int)params.length()) {
    int fin = params.indexOf(';', debut);
    if (fin < 0) fin = params.length();
    const int eq = params.indexOf('=', debut);
    if (eq > debut && eq < fin) {
      String k = params.substring(debut, eq); k.trim();
      if (k == cle) { String v = params.substring(eq + 1, fin); v.trim(); return v; }
    }
    debut = fin + 1;
  }
  return String();
}
// La partie k de `s` coupee par `sep` ; "" au-dela.
String _partie(const String& s, char sep, int k) {
  int debut = 0;
  for (int i = 0; i < k; i++) {
    const int p = s.indexOf(sep, debut);
    if (p < 0) return String();
    debut = p + 1;
  }
  const int fin = s.indexOf(sep, debut);
  String v = (fin < 0) ? s.substring(debut) : s.substring(debut, fin);
  v.trim();
  return v;
}
int _nbParties(const String& s, char sep) {
  if (!s.length()) return 0;
  int n = 1;
  for (size_t i = 0; i < s.length(); i++) if (s[i] == sep) n++;
  return n;
}
// Comme _partie, mais une liste plus courte prolonge sa derniere valeur.
String _partieCompletee(const String& s, char sep, int k) {
  const int n = _nbParties(s, sep);
  return n ? _partie(s, sep, (k < n) ? k : n - 1) : String();
}

struct Champs { String liste, bloc, note, suite, gain, debut, fin, boucle, preload, canal; };
Champs _champs(const String& params) {
  return Champs{ _valeurParam(params, "liste"),  _valeurParam(params, "lbloc"),
                 _valeurParam(params, "lnote"),  _valeurParam(params, "lsuite"),
                 _valeurParam(params, "lgain"),  _valeurParam(params, "ldebut"),
                 _valeurParam(params, "lfin"),   _valeurParam(params, "lboucle"),
                 _valeurParam(params, "lpreload"), _valeurParam(params, "lch") };
}

/* Le son d'un clip, cherche dans le magasin ; ses secondes, en trames DU
 * FICHIER. Refait quand un son arrive ou part (_reresoudre). */
/* THE HEAD OF A STREAMED CLIP — requested from the SD-card task, which reads it (SdStream.h).
 * Under listsLock, like every caller of _resoudre. A head is "referenced" while a bank
 * or a voice names it. */
bool _headReferenced(int8_t h) {
  for (uint8_t v = 0; v < VOIX_MAX; v++) if (voixEch[v].actif && voixEch[v].head == h) return true;
  if (banques)
    for (int b = 0; b < 2; b++)
      for (uint8_t li = 0; li < banques[b].n; li++)
        for (uint8_t k = 0; k < banques[b].listes[li].n; k++)
          if (banques[b].listes[li].clips[k].head == h) return true;
  return false;
}

/* The head to evict to make room for a request — never a head that a bank or a voice
 * names, never a head being read by the SD-card task (it would write into a released
 * entry).
 *   CURRENT request: first an ANTICIPATED head (the next cue: worthless until we get there
 *   — and a current request naming it would already have promoted it), the oldest; then
 *   the oldest of the others not requested for 5 s (the bank under construction is not
 *   named yet: its brand-new heads are protected by that delay).
 *   ANTICIPATED request: the same, but the 5 s delay applies to ALL of them — we do not
 *   evict what the same pass has just requested (the first clips of the next cue to load
 *   the last ones) —, and `onlyAnticipated` when it is the half-budget that is short:
 *   evicting current heads would not change anything. -1: nothing to evict. */
int8_t _headToEvict(bool forAnticipated, bool onlyAnticipated) {
  int8_t pick = -1; bool pickAnticipated = false; uint32_t pickAge = 0;
  for (int8_t i = 0; i < (int8_t)SdStream::MAX_HEADS; i++) {
    if (!SdStream::headEvictable(i) || _headReferenced(i)) continue;
    const bool anticipated = SdStream::headAnticipated(i);
    if (onlyAnticipated && !anticipated) continue;
    const uint32_t age = SdStream::headAgeMs(i);
    if ((!anticipated || forAnticipated) && age <= 5000) continue;
    if (pick < 0 || (anticipated && !pickAnticipated) || (anticipated == pickAnticipated && age > pickAge)) {
      pick = i; pickAnticipated = anticipated; pickAge = age;
    }
  }
  return pick;
}

/* The budget is full: we evict, one by one, what can be (_headToEvict), then ask again; in
 * the end, a REFUSAL, counted and said — for a CURRENT request that clip will not play, and
 * the user must be able to know it (headsState); for an ANTICIPATED one it is only a missed
 * catch-up ("the note waits"). */
int8_t _headFor(const char* name, uint32_t start, bool anticipated = false) {
  int8_t h = SdStream::requestHead(name, start, anticipated);
  while (h == SdStream::HEAD_BUDGET_FULL || h == SdStream::HEAD_ANTICIPATED_FULL) {
    const int8_t victim = _headToEvict(anticipated, h == SdStream::HEAD_ANTICIPATED_FULL);
    if (victim < 0) break;
    SdStream::releaseHead(victim);
    h = SdStream::requestHead(name, start, anticipated);
  }
  if (h == SdStream::HEAD_BUDGET_FULL || h == SdStream::HEAD_ANTICIPATED_FULL) {
    if (anticipated) SdStream::recordAnticipatedRefusal();
    else {
      SdStream::recordRefusal();
      NIDMI_WEB_LOG("[SD] %s @%lu : budget des tetes plein (%u Ko) — ce clip ne se jouera pas",
                    name, (unsigned long)start, (unsigned)(SdStream::HEADS_BUDGET_BYTES / 1024));
    }
  }
  return h < 0 ? (int8_t)-1 : h;
}

void _resoudre(Clip& c, bool anticipated = false) {
  c.iEch = -1; c.debut = 0; c.fin = 0; c.head = -1;
  if (!c.son[0]) return;
  const int i = SampleStore::indexDe(c.son);
  if (i < 0) return;
  const float f = (float)SampleStore::frequence((uint8_t)i);
  c.iEch  = (int8_t)i;
  c.debut = (c.debutS > 0.0f) ? (uint32_t)(c.debutS * f + 0.5f) : 0;
  c.fin   = (c.finS   > 0.0f) ? (uint32_t)(c.finS   * f + 0.5f) : 0;
  if (SampleStore::isStreamed((uint8_t)i)) c.head = _headFor(c.son, c.debut, anticipated);
}

/* La liste i des champs, dans L — tous ses champs poses. Les sons que la
 * carte n'a pas vont dans `absents` (une fois chacun). Rend vrai si au moins
 * un clip porte un son (une liste sans aucun son n'en est pas une). */
bool _remplirListe(Liste& L, const Champs& ch, int i, String* absents, bool anticipated = false) {
  memset(&L, 0, sizeof(Liste));
  L.bloc = (uint32_t)_partie(ch.bloc, ',', i).toInt();
  const String n = _partieCompletee(ch.note,  ',', i);
  const String s = _partieCompletee(ch.suite, ',', i);
  const String g = _partieCompletee(ch.gain,  ',', i);
  L.note  = n.length() ? (uint8_t)constrain(n.toInt(), 0, 127) : 60;
  L.suite = s.length() ? (uint8_t)constrain(s.toInt(), 0, 2)   : 0;
  L.gain  = g.length() ? _borneGain(g.toFloat()) : 1.0f;
  L.preload = (_partie(ch.preload, ',', i).toInt() != 0) ? 1 : 0;      // absent: no
  const String c = _partieCompletee(ch.canal, ',', i);                  // its track's channel (§210)
  L.canal = c.length() ? (uint8_t)constrain(c.toInt(), 0, 16) : 0;
  const String noms = _partie(ch.liste, ',', i), debs = _partie(ch.debut,  ',', i),
               fins = _partie(ch.fin,   ',', i), bcls = _partie(ch.boucle, ',', i);
  const int nc = _nbParties(noms, '/');
  if (nc > CLIPS_MAX)
    Serial.printf("[audio] play list %lu : %d clips, %u gardes\n",
                  (unsigned long)L.bloc, nc, (unsigned)CLIPS_MAX);
  bool unSon = false;
  for (int k = 0; k < nc && k < CLIPS_MAX; k++) {
    Clip& c = L.clips[k];
    const String nom = _partie(noms, '/', k);
    strncpy(c.son, nom.c_str(), sizeof(c.son) - 1);
    c.debutS = _partie(debs, '/', k).toFloat();
    c.finS   = _partie(fins, '/', k).toFloat();
    c.boucle = _partie(bcls, '/', k).toFloat() >= 0.5f;
    _resoudre(c, anticipated);
    if (c.son[0]) unSon = true;
    if (absents && c.son[0] && c.iEch < 0
        && ("," + *absents + ",").indexOf("," + String(c.son) + ",") < 0)
      *absents += (absents->length() ? "," : "") + String(c.son);
    L.n = (uint8_t)(k + 1);
  }
  return unSon;
}

/* Un son est arrive ou parti : les index des clips se recalculent — un clip
 * ne garde jamais l'emplacement d'un son retire. */
/* THE PRELOAD IN PROGRESS: the NEXT cue's params, kept (in PSRAM) to be REPLAYED when a sound
 * arrives. At boot, cue 1 starts on its own and prepares cue 2 BEFORE the SD is mounted and
 * its sounds exist: the request did not find the sound, and nothing replayed it — at every
 * power-up of a composition that plays at start. Written and read under listsLock. */
char* preloadParams = nullptr;

/* The heads of the lists marked `lpreload` in `params` — ANTICIPATED class, without arming
 * the list. Under listsLock. Idempotent: a head already requested is not requested again. */
void _preloadFromParams(const char* params) {
  if (!params || !*params) return;
  const String p(params);
  const Champs ch = _champs(p);
  const int nl = _nbParties(ch.liste, ',');
  if (!nl || !ch.preload.length()) return;
  Liste* tmp = (Liste*)heap_caps_malloc(sizeof(Liste), MALLOC_CAP_SPIRAM);
  if (!tmp) return;
  for (int i = 0; i < nl && i < (int)LISTES_MAX; i++)
    if (_partie(ch.preload, ',', i).toInt() != 0) _remplirListe(*tmp, ch, i, nullptr, /*anticipated=*/true);
  heap_caps_free(tmp);
}

void _reresoudre() {
  VerrouListes v;
  _preloadFromParams(preloadParams);   // a sound has just arrived: the next cue may find it
  if (!banques || !banques[banqueActive].n) return;
  Banque* b = _banqueLibre();
  if (!b) return;
  memcpy(b, &banques[banqueActive], sizeof(Banque));
  for (uint8_t li = 0; li < b->n; li++)
    for (uint8_t k = 0; k < b->listes[li].n; k++) _resoudre(b->listes[li].clips[k]);
  _proposer(b);
}
/* A list's clips read as streams, those that have their head, those that never will. */
void _headCounts(const Liste& L, uint8_t& streamed, uint8_t& ready, uint8_t& without) {
  streamed = ready = without = 0;
  for (uint8_t k = 0; k < L.n; k++) {
    const Clip& c = L.clips[k];
    if (c.iEch < 0 || !SampleStore::isStreamed((uint8_t)c.iEch)) continue;
    streamed++;
    if (c.head < 0)                              without++;       // the budget was full: never a head
    else if (SdStream::headReady(c.head))        ready++;
    else if (!SdStream::headInUse(c.head))       without++;       // released: no head any more
    else if (SdStream::headEvictable(c.head))    without++;       // the read failed
    // otherwise: requested, the SD-card task is reading it — this clip waits, it is not lost
  }
}

}  // namespace

void poserListes(const String& params, int8_t slot, int8_t garder) {
  VerrouListes v;
  const Champs ch = _champs(params);
  const int nl = _nbParties(ch.liste, ',');
  /* Rien avant, rien apres : pas de banque a proposer (chaque cue sans liste
   * passe par ici). */
  if (!nl && (!banques || !banques[banqueActive].n)) return;
  const uint8_t ici = (slot >= 0 && slot < (int8_t)CUE_SLOTS) ? (uint8_t)slot : _slotIci();
  Banque* b = _banqueLibre();
  if (!b) { Serial.println("[audio] play list : banque non adoptee, listes non posees"); return; }
  memset(b, 0, sizeof(Banque));
  for (int i = 0; i < nl; i++) {
    if (b->n >= LISTES_MAX) {
      Serial.printf("[audio] %d play lists, %u gardees\n", nl, (unsigned)LISTES_MAX);
      break;
    }
    if (_remplirListe(b->listes[b->n], ch, i, nullptr)) { b->listes[b->n].slot = ici; b->n++; }
  }
  /* THE LISTS OF THE CUE IN ITS RELEASE, kept playable in their slot (§211-212): those the
   * arriving cue does not continue. */
  if (garder >= 0 && banques) {
    const Banque& an = banques[banqueActive];
    for (uint8_t k = 0; k < an.n; k++) {
      const Liste& L = an.listes[k];
      if (L.slot != (uint8_t)garder || !L.bloc) continue;
      bool continuee = false;
      for (uint8_t j = 0; j < b->n; j++) if (b->listes[j].bloc == L.bloc) { continuee = true; break; }
      if (continuee) continue;
      if (b->n >= LISTES_MAX) {
        Serial.printf("[audio] play list %lu de la cue qui part : plus de place, elle se tait\n", (unsigned long)L.bloc);
        continue;
      }
      b->listes[b->n] = L;
      b->n++;
    }
  }
  _proposer(b);
}

int poserListe(const String& params, String& absents, uint32_t& bloc, uint8_t* streamedClips, uint8_t* clipsWithoutHead) {
  VerrouListes v;
  absents = "";
  if (streamedClips) *streamedClips = 0;
  if (clipsWithoutHead) *clipsWithoutHead = 0;
  const Champs ch = _champs(params);
  bloc = (uint32_t)_partie(ch.bloc, ',', 0).toInt();
  if (!bloc) return -1;
  Banque* b = _banqueLibre();
  if (!b) return -1;
  memcpy(b, &banques[banqueActive], sizeof(Banque));
  int li = -1;
  for (uint8_t k = 0; k < b->n; k++) if (b->listes[k].bloc == bloc) { li = k; break; }
  /* The list keeps its cue slot; a new one is the playing cue's (§212). */
  const uint8_t slotListe = (li >= 0) ? b->listes[li].slot : _slotIci();
  if (li < 0) {
    if (b->n >= LISTES_MAX) return -1;               // plus de place
    li = b->n++;
  }
  const bool unSon = _remplirListe(b->listes[li], ch, 0, &absents);
  b->listes[li].slot = slotListe;
  const int n = b->listes[li].n;
  { uint8_t st, rd, wo; _headCounts(b->listes[li], st, rd, wo);        // the list we JUST posed
    if (streamedClips) *streamedClips = st;
    if (clipsWithoutHead) *clipsWithoutHead = wo; }
  if (!unSon) {                                      // sans aucun son : retiree
    for (int k = li; k + 1 < b->n; k++) memcpy(&b->listes[k], &b->listes[k + 1], sizeof(Liste));
    b->n--;
  }
  _proposer(b);
  return unSon ? n : 0;
}

bool jouerClip(uint32_t bloc, uint8_t k) {
  if (!demarre || !bloc) return false;
  {
    VerrouListes v;
    if (!banques) return false;
    const Banque& b = banques[banqueActive];
    int li = -1;
    for (uint8_t i = 0; i < b.n; i++) if (b.listes[i].bloc == bloc) { li = i; break; }
    if (li < 0) return false;
    /* A clip whose head is still loading is accepted: the note will wait (_lancerClip). */
    if (k > 0 && !_clipJouable(b.listes[li], k) && !_headLoading(b.listes[li], k)) return false;
  }
  Evenement e{};
  e.sorte = SORTE_CLIP; e.bloc = bloc; e.clip = k;
  deposerEvenement(e);
  return true;
}

int etatListes(uint32_t* blocs, uint8_t* clips, uint8_t* nombres, uint8_t max, bool attendre) {
  if (!banques) return 0;
  if (xSemaphoreTake(verrouListes, attendre ? portMAX_DELAY : 0) != pdTRUE) return -1;
  const Banque& b = banques[banqueActive];
  uint8_t n = 0;
  for (uint8_t i = 0; i < b.n && n < max; i++, n++) {
    blocs[n] = b.listes[i].bloc; clips[n] = b.listes[i].courant; nombres[n] = b.listes[i].n;
  }
  xSemaphoreGive(verrouListes);
  return n;
}

/* "PRELOAD" (a playlist BLOCK's option, cue key `lpreload`): called on arrival at a cue with
 * the NEXT cue's params. For each of its marked lists, the heads of the streamed clips are
 * requested ahead of time — ANTICIPATED class, under its half-budget, behind those of the
 * playing cue, evicted first — WITHOUT arming the list: no bank, no voice. If the user jumps
 * elsewhere, they evict themselves. What does not fit falls back on "the note waits". */
void preloadLists(const String& params) {
  VerrouListes v;
  if (preloadParams) { heap_caps_free(preloadParams); preloadParams = nullptr; }
  if (params.indexOf("lpreload=") < 0) return;                 // the next cue wants none: nothing to keep
  preloadParams = (char*)heap_caps_malloc(params.length() + 1, MALLOC_CAP_SPIRAM);
  if (!preloadParams) return;
  memcpy(preloadParams, params.c_str(), params.length() + 1);
  _preloadFromParams(preloadParams);
}

/* THE HEADS STATE of an armed list: how many of its clips are read as streams, how many
 * have their head (`ready`), how many NEVER will (`without`: the budget was full, or the
 * read failed — those clips do not play). The rest waits for its read. False: the list is
 * not armed, or the lock is taken (we do not wait for it: this is a state read). */
bool headsState(uint32_t bloc, uint8_t& streamed, uint8_t& ready, uint8_t& without) {
  streamed = ready = without = 0;
  if (!banques || xSemaphoreTake(verrouListes, 0) != pdTRUE) return false;
  const Banque& b = banques[banqueActive];
  bool found = false;
  for (uint8_t li = 0; li < b.n && !found; li++)
    if (b.listes[li].bloc == bloc) { found = true; _headCounts(b.listes[li], streamed, ready, without); }
  xSemaphoreGive(verrouListes);
  return found;
}

uint32_t generationListes() { return genListes; }

/* Qui declenche : la cue (defaut, comportement d'origine) ou le clavier. */
void fixerDeclenchementSurCue(bool surCue) { slots[_slotIci()].clavier.surCue = surCue; }
bool declenchementSurCue() { return slots[_slotIci()].clavier.surCue; }

/* The keyboard's polyphony (§219): one byte, written whole, read by the audio task at a note. */
void setKeyboardPoly(uint8_t n) {
  slots[_slotIci()].clavier.poly = (n < 1) ? 1 : ((n > VOIX_MAX) ? VOIX_MAX : n);
}
uint8_t keyboardPoly() { return slots[_slotIci()].clavier.poly; }
uint32_t keyboardBlock() { return slots[_slotIci()].clavier.bloc; }

/* ARMER LE LECTEUR — il ne « charge » plus rien : tout est deja en PSRAM.
 * `nom` designe seulement l'echantillon que le CLAVIER jouera ; les cues, elles,
 * nomment le leur a chaque declenchement. */
bool setSampler(const char* nom, String& raison) {
  if (!ensureStarted()) { raison = "audio indisponible"; return false; }
  libererPlaits();                       // on ne tient jamais les deux à la fois
  SampleStore::chargerTout();            // la premiere fois ; ensuite, rien a relire
  if (nom && *nom && SampleStore::indexDe(nom) < 0) {
    raison = "echantillon « " + String(nom) + " » absent de storage";
    return false;
  }
  if (nom) { Clavier& k = slots[_slotIci()].clavier; strncpy(k.nom, nom, sizeof(k.nom) - 1); k.nom[sizeof(k.nom) - 1] = 0; }
  moteurCourant = -2;
  return true;
}

void arreterSampler() {
  if (moteurCourant == -2) moteurCourant = -1;
  arreterEchantillon();
  poserListes(String());              // plus de lecteur, plus de play list (§196)
  /* ON NE LIBERE PAS LA PSRAM. Les echantillons restent charges tant que
   * leur fichier est dans storage (voir SampleStore.h — le pire cas absolu tient
   * dans 12,7 % de la PSRAM) : pas de rechargement de 32 a 72 ms a chaque cue.
   * Seuls un remplacement ou une suppression rendent la memoire d'UN son, et
   * a l'abri de la tache audio (echantillonArrive / echantillonParti). */
}

bool samplerActif() { return lecteurPret(); }
const char* samplerNom() { return slots[_slotIci()].clavier.nom; }

/* RENDRE LA PSRAM D'UN SON RETIRE, quand la tache audio ne peut plus la lire.
 * Retire, il n'est plus trouve par son nom, et le bloc suivant coupe ses voix
 * (boucleAudio) ; seul le bloc EN COURS a l'instant du retrait a pu le lire.
 * Deux blocs finis (5 ms) : il est fini. La tache arretee ne lit rien — et
 * une tache qui ne rend plus de bloc depuis 200 ms n'est pas en train de
 * rendre. */
static void rendreApresLecture(int i) {
  if (i < 0) return;
  if (demarre) {
    const uint32_t b0 = nBlocs, t0 = millis();
    while (nBlocs - b0 < 2 && millis() - t0 < 200) vTaskDelay(1);
  }
  SampleStore::liberer(i);
}

/* UN SON ARRIVE DANS MAPFS : jouable des la reponse au televersement. Il ne
 * l'etait qu'apres un redemarrage (MESURES §177) — liste, marque ● dans
 * l'inspecteur, il repondait « absent de la carte » ; un son REMPLACE gardait
 * l'ancien. Appele par le serveur web : les 30 a 70 ms de lecture en flash sont
 * les siennes, pas celles de l'audio ni du MIDI. `fromSdCard`: the sound comes from the
 * SD card, and it is SdCard's task that calls it — never the web server: reading several
 * MB there would take seconds. */
bool echantillonArrive(const char* nom, String& raison, bool fromSdCard) {
  int ancien = -1;
  if (!SampleStore::installer(nom, raison, ancien, fromSdCard)) return false;
  rendreApresLecture(ancien);
  _reresoudre();                      // un clip qui le nomme le trouve (§196)
  return true;
}

/* UN SON QUITTE MAPFS : il se tait, et sa PSRAM est rendue. */
void echantillonParti(const char* nom) {
  rendreApresLecture(SampleStore::retirer(nom));
  _reresoudre();                      // ses clips ne le jouent plus (§196)
}

bool setEngine(int moteur) {
  if (moteur == -2) return false;        // passer par setSampler
  if (moteur < 0) {
    arreterEchantillon(); libererPlaits();
    derniereBasc = Bascule::Appliquee;
    return true;
  }
  if (moteur > 23) { derniereBasc = Bascule::Echec; return false; }
  /* PAS DE SYNTHÈSE SUR UNE CARTE SEULE. Refus net, pas d'armement : il n'y a
   * rien à charger au prochain démarrage. L'échantillonneur (moteur -2) passe
   * au-dessus de cette garde — c'est lui qui reste. */
  if (!syntheseLourdeDisponible()) { derniereBasc = Bascule::Echec; return false; }
  if (!ensureStarted()) { derniereBasc = Bascule::Echec; return false; }

  // Garde d'allocation à chaud — voir l'en-tête. Si Plaits est déjà résident,
  // plaitsAlloue() sort immédiatement et rien n'est pris au tas : on ne teste
  // donc le seuil que dans le cas où il y a vraiment 16 ko à trouver.
  // ELLE PASSE AVANT arreterSampler() : un refus doit laisser le boîtier
  // EXACTEMENT dans l'état où il était. Couper l'échantillon puis refuser
  // d'allouer Plaits, ce serait rendre la carte muette jusqu'au redémarrage.
  if (!plaitsVoix) {
    const uint32_t bloc = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (bloc < seuilBasculeChaud()) {
      /* ARMEE : le prochain demarrage l'allouera sur un tas vierge — si la
       * composition ouverte l'emploie, puisqu'il s'en deduit (§187). */
      derniereBasc = Bascule::Armee;
      Serial.printf("[audio] bascule REFUSEE a chaud : plus gros bloc %lu o < %lu.\n"
                    "        Le moteur %d viendra au prochain demarrage, si la composition l'emploie.\n",
                    (unsigned long)bloc, (unsigned long)seuilBasculeChaud(), moteur);
      return false;
    }
  }
  if (moteurCourant == -2) arreterSampler();
  if (!plaitsAlloue()) { derniereBasc = Bascule::Echec; return false; }
  derniereBasc = Bascule::Appliquee;
  plaitsPatch.engine = moteur;
  moteurCourant = moteur;
  return true;
}

/* UNE AUTRE COMPOSITION S'OUVRE (§186, §187) : comme au demarrage, le moteur
 * qu'elle emploie se prepare — ses sons en memoire, le clavier sur le premier ;
 * son moteur de synthese, si le tas le permet (sinon il viendra au prochain
 * demarrage, deduit de ses cues). RIEN NE SE DEMOLIT : l'I2S reste ouvert, un
 * moteur resident le reste — liberer puis reallouer a chaque changement, c'est
 * hacher le tas qu'un spectacle entier doit garder. Ce qui jouait se tait, et
 * le clavier revient « sur cue », comme a l'allumage : une cue l'armera. */
void preparer(int moteur, const char* son) {
  arreterEchantillon();
  poserListes(String());              // ses play list viendront avec ses cues (§196)
  fixerDeclenchementSurCue(true);
  if (moteur == -2) {
    String raison;
    if (!setSampler(son, raison))
      Serial.printf("[audio] composition ouverte : lecteur non arme — %s\n", raison.c_str());
  } else if (moteur >= 0) {
    setEngine(moteur);
  }
  Serial.printf("[audio] composition ouverte : elle emploie %s\n", decrire(moteur, son).c_str());
}

int engine() { return moteurCourant; }
Bascule derniereBascule() { return derniereBasc; }

void setParams(const Params& p) {
  /* The synth voice of the cue that plays (§210, §212). */
  const int8_t sv = slots[_slotIci()].synth, sv2 = slots[_slotIci()].synth2;
  if (sv >= 0) SynthVoice::setParams((uint8_t)sv, p);
  if (sv2 >= 0) SynthVoice::setParams((uint8_t)sv2, p);   // its second voice (§214)
  // Écriture directe : ce sont des float alignés, lus par la tâche audio au
  // bloc suivant. Un verrou coûterait plus cher que le pire cas — un bloc rendu
  // avec un mélange de l'ancien et du nouveau réglage, soit 2,5 ms.
  plaitsPatch.harmonics  = constrain(p.harmonics, 0.0f, 1.0f);
  plaitsPatch.timbre     = constrain(p.timbre,    0.0f, 1.0f);
  plaitsPatch.morph      = constrain(p.morph,     0.0f, 1.0f);
  plaitsPatch.decay      = constrain(p.decay,     0.0f, 1.0f);
  plaitsPatch.lpg_colour = constrain(p.lpgColour, 0.0f, 1.0f);
  /* BOURDON. Ecriture directe elle aussi : `trigger_patched` est un bool lu par
   * la tache audio au bloc suivant, meme regle que les continus ci-dessus. Le
   * voulu est garde a part pour survivre a une reallocation. */
  droneVoulu = (p.drone >= 0.5f);
  plaitsMod.trigger_patched = !droneVoulu;
}

Params params() {
  return Params{ plaitsPatch.harmonics, plaitsPatch.timbre, plaitsPatch.morph,
                 plaitsPatch.decay, plaitsPatch.lpg_colour,
                 droneVoulu ? 1.0f : 0.0f };
}

/* Volume de sortie, 0..1 — voir AudioEngine.h. Borne ici et nulle part ailleurs :
 * une valeur hors plage venue du reseau ne doit pas atteindre la boucle audio. */
void setVolume(float v) {
  if (!(v >= 0.f)) v = 0.f;          // attrape aussi NaN
  if (v > 1.f)     v = 1.f;
  gVolume = v;
}
float volume() { return gVolume; }

void sondeSuspensionEtRaz(uint32_t& pireUs, const char*& section, TaskHandle_t& tache) {
  pireUs = sondePireSusp;          sondePireSusp = 0;
  section = sondePireSuspSection;  sondePireSuspSection = nullptr;
  tache = sondePireSuspTache;      sondePireSuspTache = nullptr;
}

uint32_t gardeRetardEtRaz() {
  const uint32_t r = gardePireRetardUs;
  gardePireRetardUs = 0;
  return r;
}

void sondesEtRaz(uint32_t& pireEcartEofUs, uint32_t& finsDeTampon, uint32_t& pireEcartTicUs) {
  pireEcartEofUs = sondePireEof;  sondePireEof = 0;
  finsDeTampon   = sondeNbEof;    sondeNbEof = 0;
  pireEcartTicUs = sondePireTic;  sondePireTic = 0;
}

uint32_t pireBlocEtRaz(uint32_t* renduUs) {
  const uint32_t p = pireBlocUs;
  if (renduUs) *renduUs = pireBlocRenduUs;
  pireBlocUs = 0;
  return p;
}

bool silencePourLaFlash() {
  return silenceDepuisMs() >= SILENCE_POUR_LA_FLASH_MS
      && MidiRouter::silenceMidiDepuisMs() >= SILENCE_POUR_LA_FLASH_MS;   // et le MIDI (§170)
}
void ecritureFlashDebut() { ecrituresFlash = ecrituresFlash + 1; }   // -> impair
void ecritureFlashFin()   { ecrituresFlash = ecrituresFlash + 1; }   // -> pair

uint32_t silenceDepuisMs() {
  if (!demarre) return UINT32_MAX;          // pas de son a proteger
  // Le dernier son D'ABORD, l'heure ensuite : lus dans l'autre ordre, un bloc
  // audible date entre les deux lectures ferait deborder la soustraction — et
  // un son en cours passerait pour un long silence. Dans cet ordre, son <=
  // maintenant : la difference non signee est juste, retour de millis() compris.
  const uint32_t son = dernierSonMs;
  const uint32_t maintenant = millis();
  return maintenant - son;
}

Metriques metriques() {
  Metriques m{};
  m.moteur       = moteurCourant;
  m.plaitsPret   = (plaitsVoix != nullptr);
  m.plaitsOctets = plaitsOctets;
  m.cyclesParEch = cyclesEch;
  m.demarre           = demarre;
  m.heapAvantInit     = heapAvant;
  m.heapApresInit     = heapApres;
  m.heapLibre         = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m.heapPlusGrosBloc  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  m.psramLibre        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  m.sampleRateReel    = srReel;
  m.blocsRendus       = nBlocs;
  m.sousAlimentations = nRetards;
  m.retardsEcritures  = nRetardsEcritures;
  // Plancher historique : si ce chiffre frôle zéro, le crash est un épuisement
  // du tas, pas un chien de garde. C'est la mesure qui départage.
  m.heapMiniJamais    = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  m.seuilBascule      = seuilBasculeChaud();
  m.bootEssais        = essaisAuBoot;
  m.bootCoupe         = restaurationCoupee;
  m.silence           = gSilence;
  m.niveau            = niveauCrete;
  m.derniereNote      = derniereNote;
  m.causeReset        = (int)esp_reset_reason();
  m.causeResetTexte   = causeResetTexte();
  return m;
}

const char* causeResetTexte() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "logiciel";
    case ESP_RST_PANIC:     return "PANIQUE";
    case ESP_RST_INT_WDT:   return "wdt_int";
    case ESP_RST_TASK_WDT:  return "WDT_TACHE";
    case ESP_RST_WDT:       return "wdt_autre";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default:                return "inconnu";
  }
}

void compteursSon(uint32_t& blocs, uint32_t& retards, uint32_t& retardsEcritures) {
  blocs = nBlocs;
  retards = nRetards;
  retardsEcritures = nRetardsEcritures;
}

}  // namespace AudioEngine
