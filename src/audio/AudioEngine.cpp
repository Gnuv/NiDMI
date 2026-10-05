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
#include "FluxSD.h"
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
struct Evenement { uint8_t note; uint8_t velo; uint8_t sorte; uint8_t clip; uint32_t bloc; };   // velo 0 = extinction

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

struct VoixEch {
  bool     actif  = false;
  uint8_t  iEch   = 0;        // index dans SampleStore
  double   pos    = 0.0;
  double   pas    = 1.0;
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
  /* UN CLIP LU DEPUIS LA CARTE SD, EN FLUX (FluxSD.h) : `tete` (>= 0) dit qu'il vient
   * de sa tete — [teteD, teteD + teteN), en PSRAM —, puis du tampon du flux `flux`
   * (-1 : la tete suffit, le clip est plus court). Une voix a PSRAM (tete < 0) lit le
   * magasin comme avant. */
  int8_t          flux   = -1;
  int8_t          tete   = -1;
  const int16_t*  tetePcm = nullptr;
  uint32_t        teteD  = 0, teteN = 0;
};
/* LE GAIN GLISSE (MESURES §165) : un pole d'environ 10 ms a 48 kHz. Un saut de
 * gain en plein son s'entend comme un claquement ; 63 % en 10 ms, le reste en
 * quelques dizaines, c'est sous l'oreille pour un geste de potentiometre. */
constexpr float LISSAGE_GAIN = 1.0f / 480.0f;
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
  int8_t   tete;                         // un son en flux : sa tete (FluxSD), -1 sinon
  uint32_t debut, fin;                   // resolus, en trames du fichier (fin 0 : au bout)
};
struct Liste {
  uint32_t bloc;                         // le bloc de la composition : l'etiquette des voix
  uint8_t  note;                         // la note du clip 1
  uint8_t  suite;                        // 0 s'arreter, 1 enchainer, 2 enchainer et reboucler
  uint8_t  n;                            // nombre de clips
  uint8_t  courant;                      // le clip qui joue (0 : aucun), tenu par la tache audio
  uint8_t  prec;                         // 1 : « Precharger » — sur la cue d'avant, ses tetes se lisent d'avance
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
  /* Un son lu en flux ne part que de sa TETE, deja en PSRAM : sans elle (pas encore lue
   * par la tache de la carte), le clip ne se joue pas — du silence, pas une attente. */
  if (SampleStore::estFlux((uint8_t)c.iEch) && !FluxSD::tetePrete(c.tete)) return false;
  const size_t n = SampleStore::trames((uint8_t)c.iEch);
  const size_t fin = (c.fin > c.debut && c.fin < n) ? c.fin : n;
  return (size_t)c.debut + 2 <= fin;
}

/* Pose sur la voix le son et la selection du clip k — pas son gain. Faux, et
 * la voix intacte, si le clip ne se joue pas.
 *
 * UN SON EN FLUX (FluxSD.h) : la voix part de la TETE du clip, deja en PSRAM, et un flux
 * se place derriere — a la fin de la tete — pour remplir le tampon. La voix qui enchaine
 * d'un clip en flux a un autre garde son flux et le replace ; vers un son en PSRAM, elle
 * le rend. Plus de flux disponible : le clip ne part pas (faux). */
bool _poserClip(VoixEch& vo, const Liste& L, uint8_t k) {
  if (!_clipJouable(L, k)) return false;
  const Clip& c = L.clips[k - 1];
  const size_t n = SampleStore::trames((uint8_t)c.iEch);
  const uint8_t idx = (uint8_t)(&vo - voixEch);
  int8_t flux = FluxSD::possede(vo.flux, idx) ? vo.flux : (int8_t)-1;
  FluxSD::TeteVue tv{nullptr, 0, 0};
  if (SampleStore::estFlux((uint8_t)c.iEch)) {
    FluxSD::teteVue(c.tete, tv);                       // prete : _clipJouable l'a verifiee
    const uint32_t finClip = (c.fin > c.debut && c.fin < n) ? c.fin : (uint32_t)n;
    if (tv.debut + tv.trames < finClip) {              // le clip depasse sa tete : il lui faut un flux
      if (flux < 0) flux = FluxSD::acquerir(idx);
      if (flux < 0) {
        NIDMI_WEB_LOG("[audio] %s : plus de flux SD disponible (%u au plus) — clip non joue",
                      c.son, (unsigned)FluxSD::FLUX_MAX);
        return false;
      }
      FluxSD::demarrer(flux, c.son, tv.debut + tv.trames, finClip,
                       SampleStore::stereo((uint8_t)c.iEch) ? 2 : 1,
                       SampleStore::offsetDonnees((uint8_t)c.iEch));
    } else if (flux >= 0) {                            // la tete suffit : le flux est rendu
      FluxSD::liberer(flux); flux = -1;
    }
  } else if (flux >= 0) {                              // un son en PSRAM : plus de flux
    FluxSD::liberer(flux); flux = -1;
  }
  vo.flux = flux;
  vo.tete = SampleStore::estFlux((uint8_t)c.iEch) ? c.tete : (int8_t)-1;
  vo.tetePcm = tv.pcm; vo.teteD = tv.debut; vo.teteN = tv.trames;
  vo.iEch   = (uint8_t)c.iEch;
  vo.pas    = double(SampleStore::frequence(vo.iEch)) / double(srReel);
  vo.pos    = double(c.debut);
  vo.debutT = c.debut;
  vo.finT   = (c.fin > c.debut && c.fin < n) ? c.fin : 0;
  vo.boucle = c.boucle;
  vo.clip   = k;
  return true;
}

/* LA NOTE QUI ATTEND SA TETE. Un clip lu en flux ne part que de sa tete, lue par la tache de
 * la carte SD (~0,1 a 0,16 s). Une note qui arrive avant qu'elle soit prete n'est PAS perdue :
 * elle est gardee, et le clip part des que sa tete l'est. La derniere note d'une liste l'emporte
 * (un clip a la fois), la note qui arrete l'efface, et on y renonce au bout de ATTENTE_TETE_MS
 * (tete en echec, carte absente) — un clip qui partirait des secondes apres sa note est pire
 * qu'un silence. La tete du clip attendu passe devant les autres a la lecture (FluxSD).
 * Tout ceci ne vit que dans la tache audio : pas de verrou, et jamais un journal. */
struct Attente { uint32_t bloc; uint32_t t0; uint8_t clip; };
Attente           attentes[LISTES_MAX] = {};
volatile bool     attentesAEffacer = false;
constexpr uint32_t ATTENTE_TETE_MS = 2000;

// Le clip k ne se joue-t-il pas PARCE QUE sa tete se charge encore ? (et nulle autre raison)
bool _teteEnCours(const Liste& L, uint8_t k) {
  if (k == 0 || k > L.n) return false;
  const Clip& c = L.clips[k - 1];
  if (c.iEch < 0 || !SampleStore::lisible((uint8_t)c.iEch) || !SampleStore::estFlux((uint8_t)c.iEch)) return false;
  const size_t n = SampleStore::trames((uint8_t)c.iEch);
  const size_t fin = (c.fin > c.debut && c.fin < n) ? c.fin : n;
  if ((size_t)c.debut + 2 > fin) return false;
  return FluxSD::teteEnCours(c.tete);
}

void _attendre(uint32_t bloc, uint8_t k, int8_t tete) {
  Attente* a = nullptr;
  for (auto& e : attentes) if (e.bloc == bloc) { a = &e; break; }
  if (!a) for (auto& e : attentes) if (!e.bloc) { a = &e; break; }
  if (!a) return;
  a->bloc = bloc; a->clip = k; a->t0 = millis();
  FluxSD::prioriser(tete);
}

void _annulerAttente(uint32_t bloc) {
  for (auto& e : attentes) if (e.bloc == bloc) e.bloc = 0;
}

/* Lancer le clip k de la liste li — 0 : seulement l'arreter. Ce que la liste
 * jouait se tait en 5 ms : un clip a la fois. Un clip qui ne se joue pas (sans
 * son) ne fait RIEN, pas meme arreter le precedent. */
void _lancerClip(const Banque& b, uint8_t li, uint8_t k) {
  const Liste& L = b.listes[li];
  if (k > 0 && !_clipJouable(L, k)) {
    /* Sa tete se charge encore : la note attend. Ce qui jouait continue jusqu'au depart du
     * nouveau clip. Pour toute autre raison (son absent, selection vide) : rien, comme avant. */
    if (_teteEnCours(L, k)) _attendre(L.bloc, k, L.clips[k - 1].tete);
    return;
  }
  _annulerAttente(L.bloc);                // un depart, ou l'arret, remplace la note qui attendait
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (voixEch[v].actif && voixEch[v].liste == (int8_t)li) _eteindreEnDouceur(voixEch[v]);
  if (k == 0) return;
  VoixEch* vo = _voixLibre();
  vo->actif = false;                   // voir declencherEchantillon
  __sync_synchronize();
  if (!_poserClip(*vo, L, k)) return;  // plus de flux : la voix reste eteinte, pas reprise a moitie
  vo->liste = (int8_t)li;
  vo->bloc  = L.bloc;
  vo->velo  = 1.0f;                    // la velocite ne compte pas
  vo->cible = _borneGain(L.gain);
  vo->gain  = vo->cible;
  vo->fondu = 0.0f;
  vo->age   = ++voixHorloge;
  vo->picG  = 0; vo->picD = 0;
  __sync_synchronize();
  vo->actif = true;
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
void _notePourLesListes(uint8_t note) {
  if (!banques) return;
  const Banque& b = banques[banqueActive];
  for (uint8_t li = 0; li < b.n; li++) {
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

/* LES NOTES QUI ATTENDENT, a chaque bloc : leur tete est-elle prete ? Le clip part. Abandon : la
 * liste n'existe plus, la tete a echoue, ou l'attente depasse ATTENTE_TETE_MS. */
void _servirAttentes() {
  if (attentesAEffacer) { attentesAEffacer = false; for (auto& e : attentes) e.bloc = 0; }
  if (!banques) return;
  const uint32_t now = millis();
  const Banque& b = banques[banqueActive];
  for (auto& e : attentes) {
    if (!e.bloc) continue;
    int li = -1;
    for (uint8_t i = 0; i < b.n; i++) if (b.listes[i].bloc == e.bloc) { li = i; break; }
    if (li < 0 || e.clip > b.listes[li].n) { e.bloc = 0; continue; }       // la liste est partie
    const Liste& L = b.listes[li];
    if (_clipJouable(L, e.clip)) {
      const uint8_t k = e.clip;
      FluxSD::noterAttente(now - e.t0);
      e.bloc = 0;
      _lancerClip(b, (uint8_t)li, k);
      continue;
    }
    if (!_teteEnCours(L, e.clip) || now - e.t0 > ATTENTE_TETE_MS) { FluxSD::noterAbandon(); e.bloc = 0; }
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
 * navigateur, et c'est ce qu'on porte. */
volatile bool  sampleSurCue = true;
/* Quel echantillon le clavier joue : celui que la derniere cue a designe. */
char echantillonClavier[48] = {0};
/* ET A QUEL VOLUME (MESURES §165) : celui du bloc qui l'a designe, par son
 * identifiant. Une note jouait a sa seule velocite — le volume du bloc n'y
 * entrait ni a l'arrivee sur la cue ni ensuite. */
volatile uint32_t blocClavier = 0;
volatile float    gainClavier = 1.0f;

/* Une trame d'une voix en flux : sa tete d'abord ([teteD, teteD + teteN), en PSRAM), puis
 * le tampon du flux. Faux : pas encore (ou plus) la — l'appelant rend du silence. */
static inline bool _lireTete(const VoixEch& vo, uint32_t t, int16_t& g, int16_t& d) {
  if (t >= vo.teteD && t - vo.teteD < vo.teteN) {
    const uint32_t j = t - vo.teteD;
    if (SampleStore::stereo(vo.iEch)) { g = vo.tetePcm[j * 2]; d = vo.tetePcm[j * 2 + 1]; }
    else                              { g = d = vo.tetePcm[j]; }
    return true;
  }
  return vo.flux >= 0 && FluxSD::lire(vo.flux, t, g, d);
}

/* LES FLUX, A CHAQUE BLOC (avant de rendre) : un flux dont la voix n'est plus — finie,
 * coupee, volee, ou le moteur change — est rendu, et le lecteur ferme son fichier ; et
 * chaque voix en flux dit ou elle en est, ce qui regle jusqu'ou le lecteur remplit. */
void _balayerFlux() {
  for (int8_t f = 0; f < (int8_t)FluxSD::FLUX_MAX; f++) {
    if (!FluxSD::actif(f)) continue;
    const VoixEch& vo = voixEch[FluxSD::voixDe(f)];
    if (!(vo.actif && vo.flux == f)) FluxSD::liberer(f);
  }
  for (uint8_t v = 0; v < VOIX_MAX; v++)
    if (voixEch[v].actif && voixEch[v].flux >= 0) FluxSD::position(voixEch[v].flux, (uint32_t)voixEch[v].pos);
}

void rendreSample() {
  uint8_t manques = 0;                  // les voix en flux qui ont manque de donnees ce bloc
  /* MELANGE. Chaque voix lit son propre echantillon a son propre pas, et on
   * somme en 32 bits avant de borner : additionner en int16 replierait au lieu
   * de saturer, ce qui s'entend comme un craquement franc. */
  const bool mesurer = mesureVu && !gSilence;
  for (size_t i = 0; i < FRAMES; i++) {
    int32_t g = 0, d = 0;
    for (uint8_t v = 0; v < VOIX_MAX; v++) {
      VoixEch& vo = voixEch[v];
      if (!vo.actif) continue;
      /* UNE VOIX EN FLUX n'a pas de PCM en memoire : elle lit sa tete, puis le tampon du
       * flux (_lireTete). Les autres lisent le magasin, comme toujours. */
      const int16_t* pcm = (vo.tete >= 0) ? nullptr : SampleStore::donnees(vo.iEch);
      size_t         n   = SampleStore::trames(vo.iEch);
      if ((vo.tete < 0 && !pcm) || n < 2) { vo.actif = false; continue; }
      /* Fin atteinte : on reboucle, ou la voix s'eteint. Le test precede la
       * lecture pour que le reenroulement ne rejoue pas deux fois la derniere
       * trame a chaque tour. LA FIN est celle du fichier, ou celle de la
       * selection d'un clip de play list (§196), qui reboucle sur son debut —
       * et a la fin d'un clip, la liste peut enchainer sur la meme voix. */
      size_t fin = (vo.finT > 1 && vo.finT < n) ? vo.finT : n;
      if (vo.pos >= double(fin - 1)) {
        if (vo.boucle) {
          vo.pos = double(vo.debutT) + (vo.pos - double(fin - 1));
          if (vo.pos >= double(fin - 1)) vo.pos = double(vo.debutT);
          if (vo.flux >= 0) FluxSD::redemarrer(vo.flux);   // la voix repart de la tete : le flux se replace
        } else if (vo.liste >= 0 && _clipSuivant(vo)) {
          pcm = (vo.tete >= 0) ? nullptr : SampleStore::donnees(vo.iEch);
          n   = SampleStore::trames(vo.iEch);
          if ((vo.tete < 0 && !pcm) || n < 2) { vo.actif = false; continue; }
        } else { vo.actif = false; continue; }
      }
      if (vo.fondu > 0.0f) {
        /* SE TAIRE EN 5 MS (§196) : un clip coupe par un autre. */
        vo.gain -= vo.fondu;
        if (vo.gain <= 0.0f) { vo.actif = false; vo.fondu = 0.0f; continue; }
      } else {
        /* Le gain rejoint sa cible — `fixerGainBloc` la deplace pendant que la
         * voix joue. Arrive a moins de 1e-4, on pose la cible : pas de traine
         * sans fin vers zero, donc pas de nombres denormaux dans la boucle. */
        const float ecart = vo.cible - vo.gain;
        if (ecart != 0.0f)
          vo.gain = (fabsf(ecart) < 1e-4f) ? vo.cible : vo.gain + ecart * LISSAGE_GAIN;
      }
      const size_t k = (size_t)vo.pos;
      const float  f = float(vo.pos - double(k));
      const bool  st = SampleStore::stereo(vo.iEch);
      int32_t eg, ed;
      if (vo.tete >= 0) {
        /* La tete, puis le tampon ; ce qui manque est du SILENCE — jamais une attente. */
        int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
        const bool ok0 = _lireTete(vo, (uint32_t)k, l0, r0);
        const bool ok1 = ok0 && _lireTete(vo, (uint32_t)k + 1, l1, r1);
        if (!ok0)      manques |= (uint8_t)(1u << v);
        else if (!ok1) { l1 = l0; r1 = r0; }
        eg = (int32_t)(l0 + f * (l1 - l0));
        ed = (int32_t)(r0 + f * (r1 - r0));
      } else if (st) {
        eg = (int32_t)(pcm[k*2]     + f * (pcm[(k+1)*2]     - pcm[k*2]));
        ed = (int32_t)(pcm[k*2 + 1] + f * (pcm[(k+1)*2 + 1] - pcm[k*2 + 1]));
      } else {
        eg = ed = (int32_t)(pcm[k] + f * (pcm[k+1] - pcm[k]));
      }
      const int32_t sg = (int32_t)(eg * vo.gain), sd = (int32_t)(ed * vo.gain);
      g += sg; d += sd;
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
    if (g >  32767) g =  32767; else if (g < -32768) g = -32768;
    if (d >  32767) d =  32767; else if (d < -32768) d = -32768;
    entrelace[i * 2] = (int16_t)g; entrelace[i * 2 + 1] = (int16_t)d;
  }
  if (manques)
    for (uint8_t v = 0; v < VOIX_MAX; v++)
      if (manques & (1u << v)) FluxSD::manque(voixEch[v].flux);
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
  if (e.sorte == SORTE_CLIP) { _clipDemande(e.bloc, e.clip); return; }   // §196
  if (moteurCourant == -2 && SampleStore::nombreCharges() > 0) {
    /* LES PLAY LIST D'ABORD (§196) : une note choisit le clip de chaque liste
     * armee. Le clavier d'un play-sf la recoit aussi, s'il est en mode MIDI :
     * deux blocs, deux instruments. */
    if (e.velo != 0) _notePourLesListes(e.note);
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
                          gainClavier, float(e.note) - 60.0f,
                          blocClavier, float(e.velo) / 127.0f);
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
    /* LES PLAY LIST PROPOSEES s'adoptent AVANT les notes de ce bloc (§196) :
     * une liste posee puis jouee (l'ecoute de l'inspecteur) trouve la sienne. */
    if (banqueProposee >= 0 && banques) _adopter(banqueProposee);
    Evenement e;
    while (xQueueReceive(evenements, &e, 0) == pdTRUE) appliquer(e);
    _servirAttentes();                  // les notes dont la tete etait attendue partent des qu'elle est la
    /* UN SON RETIRE (remplace, supprime — §177) se tait ici, au debut du bloc :
     * sa memoire ne se rend que deux blocs plus tard (rendreApresLecture). Sans
     * ce balayage, une voix restee sur un emplacement retire — magasin vide,
     * rendreSample() n'etait plus appele — rejouait le son suivant qui s'y
     * installe. */
    for (uint8_t v = 0; v < VOIX_MAX; v++)
      if (voixEch[v].actif && !SampleStore::lisible(voixEch[v].iEch)) voixEch[v].actif = false;

    _balayerFlux();
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
    strncpy(echantillonClavier, son ? son : "", sizeof(echantillonClavier) - 1);
    Serial.printf("[audio] lecteur d'echantillons arme — %u en PSRAM, clavier sur %s\n",
                  (unsigned)n, echantillonClavier);
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
/* PERDRE UNE NOTE EST UNE FAUTE, une note-off surtout (regle du 24/09). File
 * pleine, on ATTEND qu'un bloc la vide — 2,5 ms — plutot que de jeter : un
 * retard d'un bloc s'entend a peine, une voix bloquee s'entend toujours. Au
 * plus 10 ms : au-dela, l'audio est deja arrete, et le MIDI ne doit pas
 * rester pendu a lui. Ce qui se perd alors se COMPTE (notesPerdues). */
static void deposerEvenement(const Evenement& e) {
  if (xQueueSend(evenements, &e, pdMS_TO_TICKS(10)) != pdTRUE) notesPerduesCompte = notesPerduesCompte + 1;
}

void noteOn(uint8_t note, uint8_t velocity) {
  if (!demarre) return;
  deposerEvenement(Evenement{note, velocity});
}

void noteOff(uint8_t note) {
  if (!demarre) return;
  deposerEvenement(Evenement{note, 0});
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
                           uint32_t bloc, float velo) {
  if (moteurCourant != -2) return false;
  const int i = SampleStore::indexDe(nom);
  if (i < 0) return false;
  if (SampleStore::estFlux((uint8_t)i)) {
    /* Un son lu en flux n'a pas de PCM en memoire : il ne se transpose pas, ne se joue pas
     * sur une note — seule une play list le sait lire (FluxSD.h). Dit, pas muet. */
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
  vo->flux = -1; vo->tete = -1;        // une voix volee a un clip en flux ne garde ni son flux ni sa tete
  vo->iEch   = (uint8_t)i;
  vo->pas    = (double(SampleStore::frequence((uint8_t)i)) / double(srReel))
             * ((demiTons == 0.0f) ? 1.0 : pow(2.0, double(demiTons) / 12.0));
  vo->pos    = 0.0;
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
  if (bloc == blocClavier) gainClavier = g;
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
  return n;
}

/* Le bloc que le CLAVIER joue, et son volume : le premier son d'une cue, ou
 * celui d'un apercu. */
void fixerClavier(uint32_t bloc, float gain) {
  blocClavier = bloc;
  gainClavier = (gain < 0.f) ? 1.0f : _borneGain(gain);
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
  if (listesComprises) attentesAEffacer = true;   // STOP : une note qui attendait ne repart pas apres
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

struct Champs { String liste, bloc, note, suite, gain, debut, fin, boucle, prec; };
Champs _champs(const String& params) {
  return Champs{ _valeurParam(params, "liste"),  _valeurParam(params, "lbloc"),
                 _valeurParam(params, "lnote"),  _valeurParam(params, "lsuite"),
                 _valeurParam(params, "lgain"),  _valeurParam(params, "ldebut"),
                 _valeurParam(params, "lfin"),   _valeurParam(params, "lboucle"),
                 _valeurParam(params, "lprec") };
}

/* Le son d'un clip, cherche dans le magasin ; ses secondes, en trames DU
 * FICHIER. Refait quand un son arrive ou part (_reresoudre). */
/* La tete d'un clip en flux — demandee a la tache de la carte, qui la lit (FluxSD.h).
 * La table est pleine : on eleve d'abord les tetes que plus rien ne nomme (aucune banque,
 * aucune voix) et qui ne sont plus demandees depuis 5 s, puis on redemande. Sous
 * verrouListes, comme tous les appelants de _resoudre. */
bool _teteReferencee(int8_t t) {
  for (uint8_t v = 0; v < VOIX_MAX; v++) if (voixEch[v].actif && voixEch[v].tete == t) return true;
  if (banques)
    for (int b = 0; b < 2; b++)
      for (uint8_t li = 0; li < banques[b].n; li++)
        for (uint8_t k = 0; k < banques[b].listes[li].n; k++)
          if (banques[b].listes[li].clips[k].tete == t) return true;
  return false;
}

/* La tete a evincer pour faire de la place a une demande — jamais une tete qu'une banque ou une voix
 * nomme, jamais une tete en cours de lecture par la tache de la carte (elle ecrirait dans une entree
 * rendue).
 *   Demande COURANTE : d'abord une tete ANTICIPEE (la cue suivante : sans valeur tant qu'on n'y est
 *   pas — et une demande courante qui la nomme l'aurait deja promue), la plus ancienne ; puis la plus
 *   ancienne des autres non demandee depuis 5 s (la banque en construction n'est pas encore nommee :
 *   ses tetes toutes fraiches sont protegees par ce delai).
 *   Demande ANTICIPEE : la meme chose, mais le delai de 5 s vaut pour TOUTES — on n'evince pas ce que la
 *   meme passe vient de demander (les premiers clips de la cue suivante pour charger les derniers) —,
 *   et `seulementAnticipees` quand c'est le demi-budget qui manque : evincer des tetes courantes n'y
 *   changerait rien. -1 : rien a evincer. */
int8_t _teteAEvincer(bool pourAnticipee, bool seulementAnticipees) {
  int8_t choix = -1; bool choixAnticipee = false; uint32_t choixAge = 0;
  for (int8_t i = 0; i < (int8_t)FluxSD::TETES_MAX; i++) {
    if (!FluxSD::teteEvincable(i) || _teteReferencee(i)) continue;
    const bool ant = FluxSD::teteAnticipee(i);
    if (seulementAnticipees && !ant) continue;
    const uint32_t age = FluxSD::teteAgeMs(i);
    if ((!ant || pourAnticipee) && age <= 5000) continue;
    if (choix < 0 || (ant && !choixAnticipee) || (ant == choixAnticipee && age > choixAge)) {
      choix = i; choixAnticipee = ant; choixAge = age;
    }
  }
  return choix;
}

/* La tete d'un clip en flux — demandee a la tache de la carte, qui la lit (FluxSD.h). Le budget est
 * plein : on eleve, une a une, ce qui peut l'etre (_teteAEvincer), puis on redemande ; au bout, un
 * REFUS, compte et dit — pour une demande COURANTE ce clip ne se jouera pas, et l'usager doit pouvoir le
 * savoir (etatTetes) ; pour une ANTICIPEE ce n'est qu'un rattrapage manque (« la note attend »). Sous
 * verrouListes, comme tous les appelants de _resoudre. */
int8_t _teteDe(const char* son, uint32_t debut, bool anticipee = false) {
  int8_t t = FluxSD::demanderTete(son, debut, anticipee);
  while (t == FluxSD::TETE_BUDGET_PLEIN || t == FluxSD::TETE_ANTICIPE_PLEIN) {
    const int8_t v = _teteAEvincer(anticipee, t == FluxSD::TETE_ANTICIPE_PLEIN);
    if (v < 0) break;
    FluxSD::libererTete(v);
    t = FluxSD::demanderTete(son, debut, anticipee);
  }
  if (t == FluxSD::TETE_BUDGET_PLEIN || t == FluxSD::TETE_ANTICIPE_PLEIN) {
    if (anticipee) FluxSD::noterRefusAnticipe();
    else {
      FluxSD::noterRefus();
      NIDMI_WEB_LOG("[SD] %s @%lu : budget des tetes plein (%u Ko) — ce clip ne se jouera pas",
                    son, (unsigned long)debut, (unsigned)(FluxSD::TETES_BUDGET_OCTETS / 1024));
    }
  }
  return t < 0 ? (int8_t)-1 : t;
}

void _resoudre(Clip& c, bool anticipe = false) {
  c.iEch = -1; c.debut = 0; c.fin = 0; c.tete = -1;
  if (!c.son[0]) return;
  const int i = SampleStore::indexDe(c.son);
  if (i < 0) return;
  const float f = (float)SampleStore::frequence((uint8_t)i);
  c.iEch  = (int8_t)i;
  c.debut = (c.debutS > 0.0f) ? (uint32_t)(c.debutS * f + 0.5f) : 0;
  c.fin   = (c.finS   > 0.0f) ? (uint32_t)(c.finS   * f + 0.5f) : 0;
  if (SampleStore::estFlux((uint8_t)i)) c.tete = _teteDe(c.son, c.debut, anticipe);
}

/* La liste i des champs, dans L — tous ses champs poses. Les sons que la
 * carte n'a pas vont dans `absents` (une fois chacun). Rend vrai si au moins
 * un clip porte un son (une liste sans aucun son n'en est pas une). */
bool _remplirListe(Liste& L, const Champs& ch, int i, String* absents, bool anticipe = false) {
  memset(&L, 0, sizeof(Liste));
  L.bloc = (uint32_t)_partie(ch.bloc, ',', i).toInt();
  const String n = _partieCompletee(ch.note,  ',', i);
  const String s = _partieCompletee(ch.suite, ',', i);
  const String g = _partieCompletee(ch.gain,  ',', i);
  L.note  = n.length() ? (uint8_t)constrain(n.toInt(), 0, 127) : 60;
  L.suite = s.length() ? (uint8_t)constrain(s.toInt(), 0, 2)   : 0;
  L.gain  = g.length() ? _borneGain(g.toFloat()) : 1.0f;
  L.prec  = (_partie(ch.prec, ',', i).toInt() != 0) ? 1 : 0;      // absent : non
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
    _resoudre(c, anticipe);
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
void _reresoudre() {
  VerrouListes v;
  if (!banques || !banques[banqueActive].n) return;
  Banque* b = _banqueLibre();
  if (!b) return;
  memcpy(b, &banques[banqueActive], sizeof(Banque));
  for (uint8_t li = 0; li < b->n; li++)
    for (uint8_t k = 0; k < b->listes[li].n; k++) _resoudre(b->listes[li].clips[k]);
  _proposer(b);
}
/* Les clips d'une liste lus en flux, ceux qui ont leur tete, ceux qui n'en auront jamais. */
void _comptesTetes(const Liste& L, uint8_t& flux, uint8_t& pretes, uint8_t& sans) {
  flux = pretes = sans = 0;
  for (uint8_t k = 0; k < L.n; k++) {
    const Clip& c = L.clips[k];
    if (c.iEch < 0 || !SampleStore::estFlux((uint8_t)c.iEch)) continue;
    flux++;
    if (c.tete < 0)                              sans++;       // le budget etait plein : jamais de tete
    else if (FluxSD::tetePrete(c.tete))          pretes++;
    else if (!FluxSD::teteUtilisee(c.tete))      sans++;       // rendue : plus de tete
    else if (FluxSD::teteEvincable(c.tete))      sans++;       // lecture en echec
    // sinon : demandee, la tache de la carte la lit — ce clip attend, il n'est pas perdu
  }
}

}  // namespace

void poserListes(const String& params) {
  VerrouListes v;
  const Champs ch = _champs(params);
  const int nl = _nbParties(ch.liste, ',');
  /* Rien avant, rien apres : pas de banque a proposer (chaque cue sans liste
   * passe par ici). */
  if (!nl && (!banques || !banques[banqueActive].n)) return;
  Banque* b = _banqueLibre();
  if (!b) { Serial.println("[audio] play list : banque non adoptee, listes non posees"); return; }
  memset(b, 0, sizeof(Banque));
  for (int i = 0; i < nl; i++) {
    if (b->n >= LISTES_MAX) {
      Serial.printf("[audio] %d play lists, %u gardees\n", nl, (unsigned)LISTES_MAX);
      break;
    }
    if (_remplirListe(b->listes[b->n], ch, i, nullptr)) b->n++;
  }
  _proposer(b);
}

int poserListe(const String& params, String& absents, uint32_t& bloc, uint8_t* enFlux, uint8_t* sansTete) {
  VerrouListes v;
  absents = "";
  if (enFlux) *enFlux = 0;
  if (sansTete) *sansTete = 0;
  const Champs ch = _champs(params);
  bloc = (uint32_t)_partie(ch.bloc, ',', 0).toInt();
  if (!bloc) return -1;
  Banque* b = _banqueLibre();
  if (!b) return -1;
  memcpy(b, &banques[banqueActive], sizeof(Banque));
  int li = -1;
  for (uint8_t k = 0; k < b->n; k++) if (b->listes[k].bloc == bloc) { li = k; break; }
  if (li < 0) {
    if (b->n >= LISTES_MAX) return -1;               // plus de place
    li = b->n++;
  }
  const bool unSon = _remplirListe(b->listes[li], ch, 0, &absents);
  const int n = b->listes[li].n;
  { uint8_t fl, pr, sa; _comptesTetes(b->listes[li], fl, pr, sa);       // la liste qu'on VIENT de poser
    if (enFlux) *enFlux = fl;
    if (sansTete) *sansTete = sa; }
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
    /* Un clip dont la tete se charge encore s'accepte : la note attendra (_lancerClip). */
    if (k > 0 && !_clipJouable(b.listes[li], k) && !_teteEnCours(b.listes[li], k)) return false;
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

/* « PRECHARGER » (option d'un BLOC play-list, cle `lprec` de la cue) : appelee a l'arrivee sur une
 * cue avec les params de la cue SUIVANTE. Pour chacune de ses listes marquees, les tetes des clips en
 * flux sont demandees d'avance — classe ANTICIPEE, sous son demi-budget, derriere celles de la cue
 * qui joue, evincees en premier — SANS armer la liste : ni banque, ni voix. Si l'usager saute ailleurs,
 * elles s'evincent d'elles-memes. Ce qui ne rentre pas retombe sur « la note attend ». */
void prechargerListes(const String& params) {
  VerrouListes v;
  const Champs ch = _champs(params);
  const int nl = _nbParties(ch.liste, ',');
  if (!nl || !ch.prec.length()) return;
  Liste* tmp = (Liste*)heap_caps_malloc(sizeof(Liste), MALLOC_CAP_SPIRAM);
  if (!tmp) return;
  for (int i = 0; i < nl && i < (int)LISTES_MAX; i++)
    if (_partie(ch.prec, ',', i).toInt() != 0) _remplirListe(*tmp, ch, i, nullptr, /*anticipe=*/true);
  heap_caps_free(tmp);
}

/* L'ETAT DES TETES d'une liste armee : combien de ses clips sont lus en flux, combien ont leur tete
 * (`pretes`), combien n'en auront JAMAIS (`sans` : le budget etait plein, ou la lecture a echoue —
 * ces clips-la ne jouent pas). Le reste attend sa lecture. Faux : la liste n'est pas armee, ou le
 * verrou est pris (on ne l'attend pas : c'est une lecture d'etat). */
bool etatTetes(uint32_t bloc, uint8_t& flux, uint8_t& pretes, uint8_t& sans) {
  flux = pretes = sans = 0;
  if (!banques || xSemaphoreTake(verrouListes, 0) != pdTRUE) return false;
  const Banque& b = banques[banqueActive];
  bool trouvee = false;
  for (uint8_t li = 0; li < b.n && !trouvee; li++)
    if (b.listes[li].bloc == bloc) { trouvee = true; _comptesTetes(b.listes[li], flux, pretes, sans); }
  xSemaphoreGive(verrouListes);
  return trouvee;
}

uint32_t generationListes() { return genListes; }

/* Qui declenche : la cue (defaut, comportement d'origine) ou le clavier. */
void fixerDeclenchementSurCue(bool surCue) { sampleSurCue = surCue; }
bool declenchementSurCue() { return sampleSurCue; }

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
  if (nom) strncpy(echantillonClavier, nom, sizeof(echantillonClavier) - 1);
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

bool samplerActif() { return moteurCourant == -2 && SampleStore::nombreCharges() > 0; }
const char* samplerNom() { return echantillonClavier; }

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
 * les siennes, pas celles de l'audio ni du MIDI. `carteSd` : le son vient de la
 * carte SD, et c'est la tache de CarteSd qui l'appelle — jamais le serveur web :
 * la lecture de plusieurs Mo y prendrait des secondes. */
bool echantillonArrive(const char* nom, String& raison, bool carteSd) {
  int ancien = -1;
  if (!SampleStore::installer(nom, raison, ancien, carteSd)) return false;
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
