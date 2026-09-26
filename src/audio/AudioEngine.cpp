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
#include "../config/EcrituresDifferees.h"
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
struct Evenement { uint8_t note; uint8_t velo; };   // velo 0 = extinction

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
// Monophonique, à la manière de trig-wav : un déclenchement repart de zéro.
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

struct VoixEch {
  bool     actif  = false;
  uint8_t  iEch   = 0;        // index dans SampleStore
  double   pos    = 0.0;
  double   pas    = 1.0;
  float    gain   = 0.0f;
  bool     boucle = false;
  uint32_t age    = 0;        // ordre de declenchement, pour le vol de voix
};
VoixEch  voixEch[VOIX_MAX];
uint32_t voixHorloge = 0;

/* Reste vrai tant qu'au moins une voix sonne — c'est ce que lit la porte de
 * silence et ce que « niveau » reflete. */
static inline bool _uneVoixSonne() {
  for (uint8_t v = 0; v < VOIX_MAX; v++) if (voixEch[v].actif) return true;
  return false;
}
/* DEUX FACONS DE DECLENCHER, et c'est le bloc qui choisit.
 *   surCue = true  — comportement d'ORIGINE de trig-wav : le son part a
 *                    l'arrivee sur la cue, a la hauteur du fichier, et le
 *                    clavier ne le touche pas.
 *   surCue = false — le clavier le declenche, transpose par la note (do central
 *                    = hauteur d'origine). C'est ce que la carte faisait, et ca
 *                    marche : on le garde plutot que de le jeter.
 * Le defaut est `true` : c'est le comportement du moteur tel qu'il existe cote
 * navigateur, et c'est ce qu'on porte. */
volatile bool  sampleSurCue = true;
/* Quel echantillon le clavier joue : celui que la derniere cue a designe. */
char echantillonClavier[48] = {0};

void rendreSample() {
  /* MELANGE. Chaque voix lit son propre echantillon a son propre pas, et on
   * somme en 32 bits avant de borner : additionner en int16 replierait au lieu
   * de saturer, ce qui s'entend comme un craquement franc. */
  for (size_t i = 0; i < FRAMES; i++) {
    int32_t g = 0, d = 0;
    for (uint8_t v = 0; v < VOIX_MAX; v++) {
      VoixEch& vo = voixEch[v];
      if (!vo.actif) continue;
      const int16_t* pcm = SampleStore::donnees(vo.iEch);
      const size_t   n   = SampleStore::trames(vo.iEch);
      if (!pcm || n < 2) { vo.actif = false; continue; }
      /* Fin atteinte : on reboucle, ou la voix s'eteint. Le test precede la
       * lecture pour que le reenroulement ne rejoue pas deux fois la derniere
       * trame a chaque tour. */
      if (vo.pos >= double(n - 1)) {
        if (!vo.boucle) { vo.actif = false; continue; }
        vo.pos -= double(n - 1);
      }
      const size_t k = (size_t)vo.pos;
      const float  f = float(vo.pos - double(k));
      const bool  st = SampleStore::stereo(vo.iEch);
      int32_t eg, ed;
      if (st) {
        eg = (int32_t)(pcm[k*2]     + f * (pcm[(k+1)*2]     - pcm[k*2]));
        ed = (int32_t)(pcm[k*2 + 1] + f * (pcm[(k+1)*2 + 1] - pcm[k*2 + 1]));
      } else {
        eg = ed = (int32_t)(pcm[k] + f * (pcm[k+1] - pcm[k]));
      }
      g += (int32_t)(eg * vo.gain);
      d += (int32_t)(ed * vo.gain);
      vo.pos += vo.pas;
    }
    if (g >  32767) g =  32767; else if (g < -32768) g = -32768;
    if (d >  32767) d =  32767; else if (d < -32768) d = -32768;
    entrelace[i * 2] = (int16_t)g; entrelace[i * 2 + 1] = (int16_t)d;
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

void appliquer(const Evenement& e) {
  // La porte n'est PAS ouverte par les notes : c'est le TRANSPORT qui decide.
  // Sans play, le clavier ne doit rien produire — regle demandee explicitement.
  // Ouverture par ouvrirSon() (PLAY), fermeture par couperSon() (STOP).
  if (moteurCourant == -2 && SampleStore::nombreCharges() > 0) {
    /* EN MODE « SUR CUE », le clavier ne touche pas l'echantillon : c'est la cue
     * qui le declenche. On ABSORBE la note quand meme — sans ce retour elle
     * tomberait sur le sinus plus bas, et on entendrait un bip a chaque touche
     * sur une carte dont le moteur est le lecteur. */
    if (sampleSurCue) return;
    if (e.velo == 0) return;                 // l'échantillon va au bout
    /* Au clavier, c'est l'echantillon du CLAVIER qui joue — celui que la cue a
     * designe en dernier. Do central (60) = hauteur d'origine. Une voix par
     * note : jouer un accord donne un accord, ce que la version monophonique
     * ne pouvait pas. */
    declencherEchantillon(echantillonClavier, /*boucle=*/false,
                          float(e.velo) / 127.0f, float(e.note) - 60.0f);
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
  for (;;) {
    if (arretDemande) { tacheArretee = true; vTaskDelete(nullptr); }
    Evenement e;
    while (xQueueReceive(evenements, &e, 0) == pdTRUE) appliquer(e);

    const uint32_t t0 = millis();
    const uint32_t u0 = micros();
    const uint32_t e0 = ecrituresFlash;
    const uint32_t c0 = ESP.getCycleCount();
    if (moteurCourant == -2 && SampleStore::nombreCharges() > 0) {
      rendreSample();
    } else if (moteurCourant >= 0 && plaitsVoix) {
      if (plaitsTrigger) { plaitsMod.trigger = 1.0f; plaitsTrigger = false; }
      rendrePlaits();
    } else {
      rendre();
    }
    cyclesEch = (ESP.getCycleCount() - c0) / FRAMES;
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
    if (gSilence) memset(entrelace, 0, sizeof(entrelace));
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

// ── Persistance du choix de moteur ─────────────────────────────────────────
// Le fichier d'echantillon survit au redemarrage (il est sur mapfs), mais le
// CHOIX ne survivait pas : apres un reboot on retombait sur le sinus, et
// l'echantillon telebverse semblait avoir disparu. C'est de la configuration de
// carte au sens du §11 de CONVERGENCE_NIDMI.md — « la carte detient la config
// qui tourne » — donc elle a sa place en NVS.
//
// Format d'une seule cle : ""/"-1" = sinus · "p:<n>" = Plaits n · "s:<nom>" =
// echantillon. Une cle plutot que deux : l'etat est exclusif par construction.
namespace {
constexpr const char* NVS_ESPACE = "nidmi-audio";
constexpr const char* NVS_CLE    = "moteur";
// L'ancien compteur du garde-fou, en NVS, retire au §157 : sa cle s'efface au
// premier demarrage, pour ne pas laisser d'orpheline dans le reservoir.
constexpr const char* NVS_CLE_ESSAIS_RETIREE = "bootess";

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

void memoriser(const String& valeur) {
  // Le choix vaut tout de suite ; il s'ecrit en flash au premier silence (§157).
  Differe::nvsChaine(NVS_ESPACE, NVS_CLE, valeur);
  // Un choix humain explicite réarme le garde-fou : c'est le seul chemin de
  // sortie quand la restauration a été coupée (la carte sert alors son UI, donc
  // l'utilisateur peut choisir autre chose — ou le même moteur, en connaissance
  // de cause).
  rtcPlantages = 0;
  essaisAuBoot = 0;
  restaurationCoupee = false;
}

// Appele une seule fois, a la premiere note : on ne charge JAMAIS au boot, pour
// la meme raison que le reste du moteur est paresseux — un echec ici ne doit
// pas pouvoir couter l'OTA.
void restaurer() {
  Preferences p;
  if (!p.begin(NVS_ESPACE, true)) return;
  const String v = p.getString(NVS_CLE, "");
  p.end();
  if (!v.length() || v == "-1") return;

  if (v.startsWith("s:")) {
    /* ON CHARGE TOUT, pas seulement celui-la. Une composition met des sons
     * differents sur des pistes differentes ; ne restaurer que le dernier
     * designe, c'est arriver a la premiere cue avec un magasin incomplet. */
    const uint8_t n = SampleStore::chargerTout();
    moteurCourant = -2;
    const String nom = v.substring(2);
    strncpy(echantillonClavier, nom.c_str(), sizeof(echantillonClavier) - 1);
    Serial.printf("[audio] lecteur d'echantillons arme — %u en PSRAM, clavier sur %s\n",
                  (unsigned)n, nom.c_str());
  } else if (v.startsWith("p:")) {
    if (!syntheseLourdeDisponible()) {
      /* Un choix memorise par une image qui acceptait la synthese ne doit pas
       * ressusciter dans une image qui ne l'accepte plus. On l'ignore, et on le
       * DIT — un reglage qui disparait en silence est un piege. */
      Serial.println("[audio] moteur de synthese memorise IGNORE : cette image "
                     "n'en accepte pas (carte seule). Voir MESURES.md §126.");
      return;
    }
    const int n = v.substring(2).toInt();
    if (n >= 0 && n <= 23 && plaitsAlloue()) {
      plaitsPatch.engine = n;
      moteurCourant = n;
      Serial.printf("[audio] moteur Plaits restaure : %d\n", n);
    }
  }
}
}  // namespace

bool isStarted() { return demarre; }

// ── Restauration au boot, et son garde-fou ─────────────────────────────────
// Voir l'en-tête pour le pourquoi. Ici, le comment :
//
//   1. un démarrage qui ne suit pas un plantage remet le compteur (RTC) à zéro ;
//   2. lire le choix mémorisé — s'il n'y a rien à charger, on s'arrête là ;
//   3. si le compteur a atteint TENTATIVES_MAX, couper : la carte démarre nue.
//      C'est la protection de l'OTA que l'initialisation paresseuse assurait
//      avant, transposée au nouvel ordre ;
//   4. sinon compter cette tentative (avant le risque), puis démarrer l'audio
//      et restaurer.
//
// Plus aucune écriture en flash : le compteur vivait en NVS, et sa remise à
// zéro coûtait 1,8 % de blocs en retard le temps de l'écriture, une fois par
// boot (MESURES.md §13) — pendant le son. Il vit en RTC depuis le §157.
void restaurerAuBoot() {
  /* DEMARRAGE A VIDE, DEMANDE POUR CE SEUL DEMARRAGE. Le drapeau est consomme
   * ici : le suivant restaurera le son. La NVS n'est pas lue, donc pas touchee —
   * et le compteur de tentatives non plus : ce n'est pas une tentative. */
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

  String choix;
  {
    Preferences p;
    if (!p.begin(NVS_ESPACE, false)) return;   // ecriture : effacer l'ancienne cle, une fois
    choix = p.getString(NVS_CLE, "");
    if (p.isKey(NVS_CLE_ESSAIS_RETIREE)) p.remove(NVS_CLE_ESSAIS_RETIREE);   // avant le son
    p.end();
  }
  if (!choix.length() || choix == "-1") {
    Serial.println("[audio] boot : aucun process memorise, la carte demarre nue");
    return;
  }

  if (rtcPlantages >= TENTATIVES_MAX) {
    restaurationCoupee = true;
    Serial.printf("[audio] boot : restauration COUPEE — %u plantages de suite apres restauration.\n"
                  "        La carte demarre nue (choix conserve : %s).\n"
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
  restaurer();

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
  // restauration coupee attend un choix humain (memoriser), pas une page.
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
constexpr UBaseType_t GARDE_PRIORITE = 5;       // > loopTask (1), < async_tcp (10), < audio (11)
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

  evenements = xQueueCreate(32, sizeof(Evenement));
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
  gSilence = true;                       // la porte se ferme
  bipBlocsRestants = 0;                   // coupe le bip de test
  for (auto& v : voix) v.cible = 0.0f;    // le sinus s'eteint
  arreterEchantillon();                   // toutes les voix se taisent net
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
void noteOn(uint8_t note, uint8_t velocity) {
  if (!demarre) return;
  Evenement e{note, velocity};
  xQueueSend(evenements, &e, 0);            // jamais bloquant : on préfère
}                                           // perdre une note qu'un paquet TCP

void noteOff(uint8_t note) {
  if (!demarre) return;
  Evenement e{note, 0};
  xQueueSend(evenements, &e, 0);
}

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

/* DECLENCHER SANS NOTE, a la hauteur du fichier. C'est ce que fait `trig-wav`
 * a l'arrivee sur une cue : on part de zero, on lit au rythme du fichier (la
 * seule correction est l'ecart entre sa frequence et celle que l'I2S a
 * reellement obtenue), et on boucle si la cue le demande. */
/* La voix libre, sinon la PLUS ANCIENNE. */
static VoixEch* _voixLibre() {
  VoixEch* plusVieille = &voixEch[0];
  for (uint8_t v = 0; v < VOIX_MAX; v++) {
    if (!voixEch[v].actif) return &voixEch[v];
    if (voixEch[v].age < plusVieille->age) plusVieille = &voixEch[v];
  }
  return plusVieille;
}

/* Declenche UN echantillon, nomme. `demiTons` = 0 signifie « a la hauteur du
 * fichier » — le cas de trig-wav sur cue ; le clavier passe un ecart. */
bool declencherEchantillon(const char* nom, bool boucle, float gain, float demiTons) {
  if (moteurCourant != -2) return false;
  const int i = SampleStore::indexDe(nom);
  if (i < 0) return false;
  VoixEch* vo = _voixLibre();
  vo->iEch   = (uint8_t)i;
  vo->pas    = (double(SampleStore::frequence((uint8_t)i)) / double(srReel))
             * ((demiTons == 0.0f) ? 1.0 : pow(2.0, double(demiTons) / 12.0));
  vo->pos    = 0.0;
  vo->gain   = (gain < 0.f) ? 1.0f : ((gain > 1.f) ? 1.0f : gain);
  vo->boucle = boucle;
  vo->age    = ++voixHorloge;
  vo->actif  = true;
  return true;
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
void arreterEchantillon() { for (uint8_t v = 0; v < VOIX_MAX; v++) voixEch[v].actif = false; }

/* Qui declenche : la cue (defaut, comportement d'origine) ou le clavier. */
void fixerDeclenchementSurCue(bool surCue) { sampleSurCue = surCue; }
bool declenchementSurCue() { return sampleSurCue; }

/* ARMER LE LECTEUR — il ne « charge » plus rien : tout est deja en PSRAM.
 * `nom` designe seulement l'echantillon que le CLAVIER jouera ; les cues, elles,
 * nomment le leur a chaque declenchement. */
bool setSampler(const char* nom, String& raison, bool persister) {
  if (!ensureStarted()) { raison = "audio indisponible"; return false; }
  libererPlaits();                       // on ne tient jamais les deux à la fois
  if (SampleStore::nombreCharges() == 0) SampleStore::chargerTout();
  if (nom && *nom && SampleStore::indexDe(nom) < 0) {
    raison = "echantillon « " + String(nom) + " » absent de mapfs";
    return false;
  }
  if (nom) strncpy(echantillonClavier, nom, sizeof(echantillonClavier) - 1);
  moteurCourant = -2;
  if (persister) memoriser(String("s:") + nom);
  return true;
}

void arreterSampler(bool persister) {
  const bool etaitArme = (moteurCourant == -2);
  if (etaitArme) { moteurCourant = -1; if (persister) memoriser("-1"); }
  arreterEchantillon();
  /* ON NE LIBERE PLUS LA PSRAM. Il n'y a plus rien a liberer au bon moment :
   * les echantillons restent charges pour la vie de la carte (voir
   * SampleStore.h — le pire cas absolu tient dans 12,7 % de la PSRAM). Avec eux
   * disparait l'acces-apres-liberation que ce vTaskDelay protegeait, et la
   * latence de 32 a 72 ms qu'un rechargement coutait a chaque cue. */
}

bool samplerActif() { return moteurCourant == -2 && SampleStore::nombreCharges() > 0; }
const char* samplerNom() { return echantillonClavier; }

bool setEngine(int moteur, bool persister) {
  if (moteur == -2) return false;        // passer par setSampler
  if (moteur < 0) {
    arreterEchantillon(); libererPlaits();
    if (persister) memoriser("-1");
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
      derniereBasc = Bascule::Armee;
      if (persister) memoriser(String("p:") + moteur);
      Serial.printf("[audio] bascule REFUSEE a chaud : plus gros bloc %lu o < %lu.\n"
                    "        Choix %s pour le prochain demarrage.\n",
                    (unsigned long)bloc, (unsigned long)seuilBasculeChaud(),
                    persister ? "memorise" : "NON memorise (chemin cue)");
      return false;
    }
  }
  if (moteurCourant == -2) arreterSampler(persister);   // propage la décision
  if (!plaitsAlloue()) { derniereBasc = Bascule::Echec; return false; }
  derniereBasc = Bascule::Appliquee;
  plaitsPatch.engine = moteur;
  moteurCourant = moteur;
  if (persister) memoriser(String("p:") + moteur);
  return true;
}

int engine() { return moteurCourant; }
Bascule derniereBascule() { return derniereBasc; }

void setParams(const Params& p) {
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
  return silenceDepuisMs() >= SILENCE_POUR_LA_FLASH_MS;
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
