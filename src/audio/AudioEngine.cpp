#include "AudioEngine.h"

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
#include <Preferences.h>

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

uint32_t heapAvant = 0, heapApres = 0, srReel = SAMPLE_RATE;
volatile uint32_t nBlocs = 0, nRetards = 0;

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
plaits::Patch        plaitsPatch;
plaits::Modulations  plaitsMod;
plaits::Voice::Frame plaitsTrames[FRAMES];
int                  moteurCourant = -1;          // -1 = sinus
volatile uint32_t    plaitsOctets  = 0;
volatile uint32_t    cyclesEch     = 0;
volatile bool        plaitsTrigger = false;

bool plaitsAlloue() {
  if (plaitsVoix) return true;
  const uint32_t avant = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  plaitsMem = (char*)heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!plaitsMem) { Serial.println("[audio] Plaits : 16 ko contigus indisponibles"); return false; }

  void* brut = heap_caps_malloc(sizeof(plaits::Voice), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!brut) {
    Serial.printf("[audio] Plaits : %u o pour Voice indisponibles\n", (unsigned)sizeof(plaits::Voice));
    heap_caps_free(plaitsMem); plaitsMem = nullptr;
    return false;
  }
  plaitsVoix = new (brut) plaits::Voice();

  stmlib::BufferAllocator allocateur(plaitsMem, 16384);
  plaitsVoix->Init(&allocateur);

  plaitsPatch.note = 48.0f;
  plaitsPatch.harmonics = 0.5f; plaitsPatch.timbre = 0.5f; plaitsPatch.morph = 0.5f;
  plaitsPatch.frequency_modulation_amount = 0.0f;
  plaitsPatch.timbre_modulation_amount    = 0.0f;
  plaitsPatch.morph_modulation_amount     = 0.0f;
  plaitsPatch.decay = 0.5f; plaitsPatch.lpg_colour = 0.5f; plaitsPatch.engine = 0;

  plaitsMod = plaits::Modulations{};
  plaitsMod.trigger_patched = true;   // sans ça les moteurs jouent en continu

  plaitsOctets = avant - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  Serial.printf("[audio] Plaits alloue : %lu o (Voice %u + 16384)\n",
                (unsigned long)plaitsOctets, (unsigned)sizeof(plaits::Voice));
  return true;
}

// ---- lecteur d'échantillons ----------------------------------------------
// Monophonique, à la manière de trig-wav : un déclenchement repart de zéro.
// La hauteur suit la note (do central = hauteur d'origine), par lecture à pas
// fractionnaire avec interpolation linéaire — quelques opérations par
// échantillon, sans commune mesure avec un moteur de synthèse.
volatile bool  sampleActif = false;
double         samplePos   = 0.0;
double         samplePas   = 1.0;
float          sampleGain  = 0.0f;

void rendreSample() {
  const int16_t* pcm = SampleStore::donnees();
  const size_t   n   = SampleStore::trames();
  const bool     st  = SampleStore::stereo();
  for (size_t i = 0; i < FRAMES; i++) {
    int16_t g = 0, d = 0;
    if (sampleActif && pcm && samplePos < double(n - 1)) {
      const size_t k = (size_t)samplePos;
      const float  f = float(samplePos - double(k));
      if (st) {
        g = (int16_t)(pcm[k*2]     + f * (pcm[(k+1)*2]     - pcm[k*2]));
        d = (int16_t)(pcm[k*2 + 1] + f * (pcm[(k+1)*2 + 1] - pcm[k*2 + 1]));
      } else {
        g = d = (int16_t)(pcm[k] + f * (pcm[k+1] - pcm[k]));
      }
      g = (int16_t)(g * sampleGain);
      d = (int16_t)(d * sampleGain);
      samplePos += samplePas;
    } else {
      sampleActif = false;
    }
    entrelace[i * 2] = g; entrelace[i * 2 + 1] = d;
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
  if (SampleStore::estCharge() && moteurCourant == -2) {
    if (e.velo == 0) return;                 // l'échantillon va au bout
    // do central (60) = hauteur d'origine ; on compense aussi l'écart entre la
    // fréquence du fichier et celle réellement obtenue par l'I2S.
    samplePas   = (double(SampleStore::frequence()) / double(srReel))
                * pow(2.0, (double(e.note) - 60.0) / 12.0);
    samplePos   = 0.0;
    sampleGain  = float(e.velo) / 127.0f;
    sampleActif = true;
    return;
  }
  if (moteurCourant >= 0 && plaitsVoix) {
    if (e.velo == 0) return;                 // le LPG de Plaits gère l'extinction
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
    Evenement e;
    while (xQueueReceive(evenements, &e, 0) == pdTRUE) appliquer(e);

    const uint32_t t0 = millis();
    const uint32_t c0 = ESP.getCycleCount();
    if (moteurCourant == -2 && SampleStore::estCharge()) {
      rendreSample();
    } else if (moteurCourant >= 0 && plaitsVoix) {
      if (plaitsTrigger) { plaitsMod.trigger = 1.0f; plaitsTrigger = false; }
      rendrePlaits();
    } else {
      rendre();
    }
    cyclesEch = (ESP.getCycleCount() - c0) / FRAMES;
    i2s.write((const uint8_t*)entrelace, sizeof(entrelace));
    // Un bloc dure 2,67 ms ; si l'aller-retour dépasse largement, c'est que la
    // tâche a été préemptée au point de vider le DMA.
    if (millis() - t0 > 20) nRetards++;
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

void memoriser(const String& valeur) {
  Preferences p;
  if (!p.begin(NVS_ESPACE, false)) return;
  p.putString(NVS_CLE, valeur);
  p.end();
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
    String raison;
    const String nom = v.substring(2);
    if (SampleStore::charger(nom.c_str(), raison)) {
      moteurCourant = -2;
      Serial.printf("[audio] echantillon restaure : %s\n", nom.c_str());
    } else {
      Serial.printf("[audio] restauration de %s impossible : %s\n",
                    nom.c_str(), raison.c_str());
    }
  } else if (v.startsWith("p:")) {
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

bool ensureStarted() {
  if (demarre) return true;

  heapAvant = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  evenements = xQueueCreate(32, sizeof(Evenement));
  if (!evenements) { Serial.println("[audio] file impossible"); return false; }

  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[audio] i2s.begin a echoue — le reste du boitier continue");
    vQueueDelete(evenements); evenements = nullptr;
    return false;
  }
  srReel = i2s.txSampleRate();
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

  heapApres = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  demarre = true;
  restaurer();          // le choix memorise, pas au boot : ici, a la 1re note
  Serial.printf("[audio] demarre — %lu Hz reels, heap interne %lu -> %lu (cout %ld o)\n",
                (unsigned long)srReel, (unsigned long)heapAvant,
                (unsigned long)heapApres, (long)heapAvant - (long)heapApres);
  return true;
}

void noteOn(uint8_t note, uint8_t velocity) {
  if (!ensureStarted()) return;
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

bool setSampler(const char* nom, String& raison, bool persister) {
  if (!ensureStarted()) { raison = "audio indisponible"; return false; }
  libererPlaits();                       // on ne tient jamais les deux à la fois
  if (!SampleStore::charger(nom, raison)) return false;
  moteurCourant = -2;
  if (persister) memoriser(String("s:") + nom);
  return true;
}

void arreterSampler(bool persister) {
  const bool etaitCharge = (moteurCourant == -2);
  if (etaitCharge) { moteurCourant = -1; if (persister) memoriser("-1"); }
  sampleActif = false;
  // Même précaution que libererPlaits(), qui manquait ici : rendreSample() lit
  // le tampon PSRAM à chaque bloc. Le libérer sans attendre, c'est un accès
  // après libération — audio en charpie ou plantage. On laisse huit blocs
  // (2,5 ms chacun) à la tâche audio pour repasser au sinus. L'appelant est le
  // gestionnaire HTTP, de priorité inférieure : le vTaskDelay lui rend la main.
  if (etaitCharge) vTaskDelay(pdMS_TO_TICKS(20));
  SampleStore::decharger();
}

bool samplerActif() { return moteurCourant == -2 && SampleStore::estCharge(); }
const char* samplerNom() { return SampleStore::nomCharge(); }

bool setEngine(int moteur, bool persister) {
  if (moteur == -2) return false;        // passer par setSampler
  if (moteur < 0) {
    sampleActif = false; libererPlaits();
    if (persister) memoriser("-1");
    return true;
  }
  if (moteurCourant == -2) arreterSampler(persister);   // propage la décision
  if (moteur > 23) return false;   // 24 moteurs (engine2 + classiques)
  if (!ensureStarted()) return false;
  if (!plaitsAlloue()) return false;
  plaitsPatch.engine = moteur;
  moteurCourant = moteur;
  if (persister) memoriser(String("p:") + moteur);
  return true;
}

int engine() { return moteurCourant; }

void setParams(const Params& p) {
  // Écriture directe : ce sont des float alignés, lus par la tâche audio au
  // bloc suivant. Un verrou coûterait plus cher que le pire cas — un bloc rendu
  // avec un mélange de l'ancien et du nouveau réglage, soit 2,5 ms.
  plaitsPatch.harmonics  = constrain(p.harmonics, 0.0f, 1.0f);
  plaitsPatch.timbre     = constrain(p.timbre,    0.0f, 1.0f);
  plaitsPatch.morph      = constrain(p.morph,     0.0f, 1.0f);
  plaitsPatch.decay      = constrain(p.decay,     0.0f, 1.0f);
  plaitsPatch.lpg_colour = constrain(p.lpgColour, 0.0f, 1.0f);
}

Params params() {
  return Params{ plaitsPatch.harmonics, plaitsPatch.timbre, plaitsPatch.morph,
                 plaitsPatch.decay, plaitsPatch.lpg_colour };
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
  // Plancher historique : si ce chiffre frôle zéro, le crash est un épuisement
  // du tas, pas un chien de garde. C'est la mesure qui départage.
  m.heapMiniJamais    = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  m.causeReset        = (int)esp_reset_reason();
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  m.causeResetTexte = "poweron";   break;
    case ESP_RST_SW:       m.causeResetTexte = "logiciel";  break;
    case ESP_RST_PANIC:    m.causeResetTexte = "PANIQUE";   break;
    case ESP_RST_INT_WDT:  m.causeResetTexte = "wdt_int";   break;
    case ESP_RST_TASK_WDT: m.causeResetTexte = "WDT_TACHE"; break;
    case ESP_RST_WDT:      m.causeResetTexte = "wdt_autre"; break;
    case ESP_RST_BROWNOUT: m.causeResetTexte = "BROWNOUT";  break;
    case ESP_RST_DEEPSLEEP: m.causeResetTexte = "deepsleep"; break;
    default:               m.causeResetTexte = "inconnu";   break;
  }
  return m;
}

}  // namespace AudioEngine
