#include "JournalAvant.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <cstdarg>
#include <cstring>
#include "../audio/AudioEngine.h"

extern "C" const char* nidmi_redemarrageDemandePar();

namespace JournalAvant {
namespace {

constexpr uint32_t MAGIE   = 0x4A415654;   // « JAVT »
constexpr uint8_t  N_AUDIO = 10;           // deux rapports du surveillant (quatre a cinq lignes chacun)
constexpr uint8_t  N_TOUT  = 6;            // le contexte : les dernieres lignes d'autre chose
constexpr uint16_t LARGEUR = 200;          // une ligne de console

struct Ligne {
  uint32_t n;                              // numero d'arrivee : l'ordre, meme dans la meme milliseconde
  uint32_t t;                              // ms depuis le demarrage
  char texte[LARGEUR];
};

struct Journal {
  uint32_t magie;
  uint32_t taille;                         // sizeof(Journal) : une autre image, une autre forme
  uint32_t dureeMs, blocs, retards, retardsEcritures;
  uint32_t suivant;                        // numero de la prochaine ligne
  uint8_t  audioProchain, audioNb, toutProchain, toutNb;
  Ligne    audio[N_AUDIO];
  Ligne    tout[N_TOUT];
};

RTC_NOINIT_ATTR Journal g_rtc;             // ~3,3 Ko sur les 8 de la RTC lente
portMUX_TYPE g_verrou = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_pret = false;              // rien ne s'ecrit avant la capture : ce serait sur la vie precedente

Journal* g_avant = nullptr;                // la vie precedente, a l'abri en PSRAM
const Ligne* g_ordre[N_AUDIO + N_TOUT];    // ses lignes, dans l'ordre d'arrivee
uint8_t g_nb = 0;
constexpr size_t LIGNE_REJEU = LARGEUR + 24;
char* g_rejeu = nullptr;                   // resume + lignes datees, bout a bout
uint16_t g_debutRejeu[1 + N_AUDIO + N_TOUT];
uint8_t g_nbRejeu = 0;

void vider(Journal& j) {
  j.magie = MAGIE;
  j.taille = sizeof(Journal);
  j.dureeMs = j.blocs = j.retards = j.retardsEcritures = 0;
  j.suivant = 0;
  j.audioProchain = j.audioNb = j.toutProchain = j.toutNb = 0;
}

// Une memoire RTC n'est jamais tout a fait sure : bornes et textes remis d'aplomb.
void assainir(Ligne* l, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    l[i].texte[LARGEUR - 1] = '\0';
    for (char* c = l[i].texte; *c; c++)
      if ((uint8_t)*c < 0x20) *c = ' ';
  }
}

void ranger(Journal& j) {
  if (j.audioNb > N_AUDIO) j.audioNb = N_AUDIO;
  if (j.toutNb > N_TOUT) j.toutNb = N_TOUT;
  assainir(j.audio, N_AUDIO);
  assainir(j.tout, N_TOUT);
  // Les deux anneaux se fusionnent : un rapport se lit dans son contexte.
  g_nb = 0;
  for (uint8_t i = 0; i < j.audioNb; i++) g_ordre[g_nb++] = &j.audio[i];
  for (uint8_t i = 0; i < j.toutNb; i++) g_ordre[g_nb++] = &j.tout[i];
  for (uint8_t i = 1; i < g_nb; i++)
    for (uint8_t k = i; k > 0 && g_ordre[k - 1]->n > g_ordre[k]->n; k--) {
      const Ligne* x = g_ordre[k]; g_ordre[k] = g_ordre[k - 1]; g_ordre[k - 1] = x;
    }
}

// « 12:03 » depuis le demarrage, « 1:02:03 » au-dela de l'heure.
void heure(char* dst, size_t n, uint32_t ms) {
  const uint32_t s = ms / 1000;
  if (s >= 3600) snprintf(dst, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                          (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  else snprintf(dst, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// Une ligne de rejeu de plus ; snprintf rend ce qu'il AURAIT ecrit, d'ou la borne.
void poser(size_t& pos, const char* fmt, ...) {
  g_debutRejeu[g_nbRejeu++] = (uint16_t)pos;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(g_rejeu + pos, LIGNE_REJEU, fmt, ap);
  va_end(ap);
  if (n < 0) { g_rejeu[pos] = '\0'; n = 0; }
  if ((size_t)n > LIGNE_REJEU - 1) n = LIGNE_REJEU - 1;
  pos += (size_t)n + 1;
}

void fabriquerRejeu(const Journal& j) {
  g_rejeu = (char*)heap_caps_malloc(LIGNE_REJEU * (1 + g_nb), MALLOC_CAP_SPIRAM);
  if (!g_rejeu) return;
  char duree[16];
  heure(duree, sizeof(duree), j.dureeMs);
  const char* par = nidmi_redemarrageDemandePar();
  size_t pos = 0;
  poser(pos, "[avant] la vie precedente : %s de marche, %lu decrochage(s) ; fin : %s%s%s",
        duree, (unsigned long)resume().entendus(), AudioEngine::causeResetTexte(),
        (par && *par) ? ", demandee par " : "", (par && *par) ? par : "");
  for (uint8_t i = 0; i < g_nb; i++) {
    char h[16];
    heure(h, sizeof(h), g_ordre[i]->t);
    poser(pos, "[avant %s] %s", h, g_ordre[i]->texte);
  }
}

}  // namespace

void capturer() {
  if (g_rtc.magie == MAGIE && g_rtc.taille == sizeof(Journal)) {
    g_avant = (Journal*)heap_caps_malloc(sizeof(Journal), MALLOC_CAP_SPIRAM);
    if (g_avant) {
      memcpy(g_avant, &g_rtc, sizeof(Journal));
      ranger(*g_avant);
      fabriquerRejeu(*g_avant);
    }
  }
  vider(g_rtc);
  g_pret = true;
}

void noter(const char* texte) {
  if (!g_pret || !texte || !*texte) return;
  const bool audio = strncmp(texte, "[audio]", 7) == 0;
  const uint32_t t = millis();
  // Le verrou ne fait que RESERVER la case : la copie, en RTC lente, se fait
  // hors section critique. Deux ecrivains sur la meme case supposeraient dix
  // lignes simultanees.
  Ligne* l;
  uint32_t n;
  portENTER_CRITICAL(&g_verrou);
  n = g_rtc.suivant++;
  if (audio) {
    l = &g_rtc.audio[g_rtc.audioProchain];
    g_rtc.audioProchain = (g_rtc.audioProchain + 1) % N_AUDIO;
    if (g_rtc.audioNb < N_AUDIO) g_rtc.audioNb++;
  } else {
    l = &g_rtc.tout[g_rtc.toutProchain];
    g_rtc.toutProchain = (g_rtc.toutProchain + 1) % N_TOUT;
    if (g_rtc.toutNb < N_TOUT) g_rtc.toutNb++;
  }
  portEXIT_CRITICAL(&g_verrou);
  l->n = n;
  l->t = t;
  strlcpy(l->texte, texte, LARGEUR);
}

void compteurs(uint32_t blocs, uint32_t retards, uint32_t retardsEcritures) {
  if (!g_pret) return;
  g_rtc.blocs = blocs;
  g_rtc.retards = retards;
  g_rtc.retardsEcritures = retardsEcritures;
  g_rtc.dureeMs = millis();
}

bool disponible() { return g_avant != nullptr; }

Resume resume() {
  Resume r;
  if (!g_avant) return r;
  r.dureeMs = g_avant->dureeMs;
  r.blocs = g_avant->blocs;
  r.retards = g_avant->retards;
  r.retardsEcritures = g_avant->retardsEcritures;
  return r;
}

uint8_t nbLignes() { return g_avant ? g_nb : 0; }

bool ligne(uint8_t i, uint32_t& t, const char*& texte) {
  if (!g_avant || i >= g_nb) return false;
  t = g_ordre[i]->t;
  texte = g_ordre[i]->texte;
  return true;
}

uint8_t nbLignesAudio() { return g_avant ? g_avant->audioNb : 0; }

uint8_t nbLignesRejeu() { return g_rejeu ? g_nbRejeu : 0; }

const char* ligneRejeu(uint8_t i) {
  return (g_rejeu && i < g_nbRejeu) ? g_rejeu + g_debutRejeu[i] : nullptr;
}

}  // namespace JournalAvant
