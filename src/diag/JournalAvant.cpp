#include "JournalAvant.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <cstdarg>
#include <cstring>
#include <esp_cpu.h>
#include <esp_debug_helpers.h>
#include <esp_memory_utils.h>
#include "esp_private/panic_internal.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include <xtensa_context.h>
#include <esp_cpu_utils.h>        // esp_cpu_process_stack_pc
#endif
#include "../audio/AudioEngine.h"

extern "C" const char* nidmi_redemarrageDemandePar();

namespace JournalAvant {
namespace {

constexpr uint32_t MAGIE   = 0x4A415654;   // « JAVT »
constexpr uint8_t  N_RAPPORT = 10;         // les surveillants : deux rapports de bloc lent, ou un lien mort
constexpr uint8_t  N_TOUT  = 6;            // le contexte : les dernieres lignes d'autre chose
constexpr uint16_t LARGEUR = 200;          // une ligne de console

struct Ligne {
  uint32_t n;                              // numero d'arrivee : l'ordre, meme dans la meme milliseconde
  uint32_t t;                              // ms depuis le demarrage
  char texte[LARGEUR];
};

/* LA PANIQUE QUI A MIS FIN A LA VIE (MESURES §163). Le 26/09, la carte
 * plantait toutes les deux minutes sur le chien de garde des interruptions,
 * et ESP-IDF n'en ecrivait pas le core dump : rien ne disait ou. Arduino
 * enveloppe son gestionnaire de panique et appelle un crochet
 * (set_arduino_panic_handler) : on y note, avant qu'il passe la main, le coeur
 * fautif, la raison, les registres utiles et la pile d'appels des DEUX coeurs
 * — une adresse se retrouve dans l'ELF de l'image (addr2line). */
constexpr uint32_t MAGIE_PANIQUE = 0x50414E51;   // « PANQ »
constexpr uint8_t  PROFONDEUR = 12;
struct Panique {
  uint32_t magie;                          // MAGIE_PANIQUE : la vie precedente a fini en panique
  int32_t  coeur;
  char     raison[48];                     // vide si la chaine n'etait pas en RAM
  uint32_t raisonAdr;                      // son adresse, pour la retrouver dans l'ELF
  uint32_t adresse, pc, exccause, excvaddr;
  uint32_t pile[2][PROFONDEUR];            // les PC successifs, coeur 0 et coeur 1
  uint8_t  profondeur[2];
};

/* Une panique DANS la panique : le 26/09, le chien de garde des interruptions
 * tirait pendant l'ecriture du core dump de la premiere, et le releve de
 * celle-ci, la vraie cause, etait ecrase. On garde la premiere ; celle-ci se
 * note a part, en bref. */
struct PaniqueImbriquee {
  uint32_t nb;                             // combien, dans cette vie
  int32_t  coeur;
  uint32_t raisonAdr, pc, exccause;
  uint32_t pile[6];
  uint8_t  profondeur;
};

struct Journal {
  uint32_t magie;
  uint32_t taille;                         // sizeof(Journal) : une autre image, une autre forme
  uint32_t dureeMs, blocs, retards, retardsEcritures;
  uint32_t suivant;                        // numero de la prochaine ligne
  uint8_t  rapportProchain, rapportNb, toutProchain, toutNb;
  Ligne    rapport[N_RAPPORT];
  Ligne    tout[N_TOUT];
  Panique  panique;
  PaniqueImbriquee imbriquee;
};

RTC_NOINIT_ATTR Journal g_rtc;             // ~3,3 Ko sur les 8 de la RTC lente
portMUX_TYPE g_verrou = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_pret = false;              // rien ne s'ecrit avant la capture : ce serait sur la vie precedente

Journal* g_avant = nullptr;                // la vie precedente, a l'abri en PSRAM
const Ligne* g_ordre[N_RAPPORT + N_TOUT];    // ses lignes, dans l'ordre d'arrivee
uint8_t g_nb = 0;
constexpr size_t LIGNE_REJEU = LARGEUR + 24;
char* g_rejeu = nullptr;                   // resume, panique, lignes datees, bout a bout
constexpr uint8_t N_ENTETE = 5;            // le resume, la panique, ses deux piles, l'imbriquee
uint16_t g_debutRejeu[N_ENTETE + N_RAPPORT + N_TOUT];
uint8_t g_nbRejeu = 0;

void vider(Journal& j) {
  j.magie = MAGIE;
  j.taille = sizeof(Journal);
  j.dureeMs = j.blocs = j.retards = j.retardsEcritures = 0;
  j.suivant = 0;
  j.rapportProchain = j.rapportNb = j.toutProchain = j.toutNb = 0;
  j.panique.magie = 0;
  j.imbriquee.nb = 0;
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
  if (j.rapportNb > N_RAPPORT) j.rapportNb = N_RAPPORT;
  if (j.toutNb > N_TOUT) j.toutNb = N_TOUT;
  assainir(j.rapport, N_RAPPORT);
  assainir(j.tout, N_TOUT);
  // Les deux anneaux se fusionnent : un rapport se lit dans son contexte.
  g_nb = 0;
  for (uint8_t i = 0; i < j.rapportNb; i++) g_ordre[g_nb++] = &j.rapport[i];
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

// « 4037a1b2 42001234 … » : une pile d'appels, a passer telle quelle a addr2line.
void pileTexte(char* dst, size_t n, const uint32_t* pile, uint8_t prof) {
  size_t pos = 0;
  dst[0] = '\0';
  for (uint8_t i = 0; i < prof && pos + 10 < n; i++)
    pos += snprintf(dst + pos, n - pos, i ? " %08lx" : "%08lx", (unsigned long)pile[i]);
}

void fabriquerRejeu(const Journal& j) {
  g_rejeu = (char*)heap_caps_malloc(LIGNE_REJEU * (N_ENTETE + g_nb), MALLOC_CAP_SPIRAM);
  if (!g_rejeu) return;
  char duree[16];
  heure(duree, sizeof(duree), j.dureeMs);
  const char* par = nidmi_redemarrageDemandePar();
  size_t pos = 0;
  poser(pos, "[avant] la vie precedente : %s de marche, %lu decrochage(s) ; fin : %s%s%s",
        duree, (unsigned long)resume().entendus(), AudioEngine::causeResetTexte(),
        (par && *par) ? ", demandee par " : "", (par && *par) ? par : "");
  if (j.panique.magie == MAGIE_PANIQUE) {
    const Panique& p = j.panique;
    poser(pos, "[avant] panique coeur %ld : %s (raison a %08lx) ; pc %08lx, cause %lu, adresse %08lx",
          (long)p.coeur, p.raison[0] ? p.raison : "?", (unsigned long)p.raisonAdr,
          (unsigned long)p.pc, (unsigned long)p.exccause, (unsigned long)p.excvaddr);
    for (uint8_t c = 0; c < 2; c++) {
      char pile[PROFONDEUR * 9 + 1];
      pileTexte(pile, sizeof(pile), p.pile[c], p.profondeur[c]);
      poser(pos, "[avant] pile coeur %u : %s", (unsigned)c, p.profondeur[c] ? pile : "-");
    }
    if (j.imbriquee.nb) {
      char pile[6 * 9 + 1];
      pileTexte(pile, sizeof(pile), j.imbriquee.pile, j.imbriquee.profondeur);
      poser(pos, "[avant] puis %lu panique(s) pendant son traitement : coeur %ld, raison a %08lx, pc %08lx ; pile %s",
            (unsigned long)j.imbriquee.nb, (long)j.imbriquee.coeur, (unsigned long)j.imbriquee.raisonAdr,
            (unsigned long)j.imbriquee.pc, pile);
    }
  }
  for (uint8_t i = 0; i < g_nb; i++) {
    char h[16];
    heure(h, sizeof(h), g_ordre[i]->t);
    poser(pos, "[avant %s] %s", h, g_ordre[i]->texte);
  }
}

/* Tout ce qui suit s'execute DANS la panique : IRAM, RAM interne et RTC
 * seulement, rien qu'on doive lire en flash (le cache peut etre coupe). */
void IRAM_ATTR pileDe(const void* cadre, uint32_t* pile, uint8_t& n) {
  n = 0;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  if (!cadre) return;
  const XtExcFrame* f = (const XtExcFrame*)cadre;
  esp_backtrace_frame_t fr = { (uint32_t)f->pc, (uint32_t)f->a1, (uint32_t)f->a0, cadre };
  pile[n++] = esp_cpu_process_stack_pc(fr.pc);
  // Comme la pile d'ESP-IDF : une remontee par cadre, arretee a un pointeur fou.
  while (n < PROFONDEUR && fr.next_pc && esp_stack_ptr_is_sane(fr.sp)) {
    if (!esp_backtrace_get_next_frame(&fr)) break;
    pile[n++] = esp_cpu_process_stack_pc(fr.pc);
  }
#else
  (void)cadre; (void)pile;
#endif
}

/* Le crochet de panique d'Arduino : son enveloppe de esp_panic_handler
 * (platform.txt : -Wl,--wrap=esp_panic_handler) l'appelle avec la pile
 * d'appels du coeur fautif, deja remontee ; l'autre coeur, on le remonte
 * ici, depuis le cadre qu'ESP-IDF a garde de lui. */
void IRAM_ATTR paniqueArduino(arduino_panic_info_t* info, void*) {
  if (g_rtc.panique.magie == MAGIE_PANIQUE) {
    // Deja une panique dans cette vie : celle-ci arrive PENDANT son traitement.
    PaniqueImbriquee& q = g_rtc.imbriquee;
    q.nb++;
    q.coeur = info->core;
    q.raisonAdr = (uint32_t)info->reason;
    q.pc = (uint32_t)info->pc;
    q.exccause = 0;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    const void* c = (info->core >= 0 && info->core < SOC_CPU_CORES_NUM) ? g_exc_frames[info->core] : nullptr;
    if (c) { q.pc = ((const XtExcFrame*)c)->pc; q.exccause = ((const XtExcFrame*)c)->exccause; }
#endif
    uint8_t k = 0;
    for (; k < 6 && k < info->backtrace_len; k++) q.pile[k] = info->backtrace[k];
    q.profondeur = k;
    return;
  }
  Panique& p = g_rtc.panique;
  p.magie = 0;                                 // incomplete tant qu'on l'ecrit
  p.coeur = info->core;
  p.raisonAdr = (uint32_t)info->reason;
  size_t i = 0;
  if (info->reason && esp_ptr_in_dram(info->reason))
    for (; i < sizeof(p.raison) - 1 && info->reason[i]; i++) p.raison[i] = info->reason[i];
  p.raison[i] = '\0';
  p.adresse = (uint32_t)info->pc;
  p.pc = p.exccause = p.excvaddr = 0;
  const int autre = info->core ? 0 : 1;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  const void* cadreFautif = (info->core >= 0 && info->core < SOC_CPU_CORES_NUM) ? g_exc_frames[info->core] : nullptr;
  if (cadreFautif) {
    const XtExcFrame* f = (const XtExcFrame*)cadreFautif;
    p.pc = f->pc;
    p.exccause = f->exccause;
    p.excvaddr = f->excvaddr;
  }
#endif
  uint8_t n = 0;
  for (; n < PROFONDEUR && n < info->backtrace_len; n++) p.pile[info->core & 1][n] = info->backtrace[n];
  p.profondeur[info->core & 1] = n;
  pileDe(autre < SOC_CPU_CORES_NUM ? g_exc_frames[autre] : nullptr, p.pile[autre], p.profondeur[autre]);
  p.magie = MAGIE_PANIQUE;
}

}  // namespace

void capturer() {
  if (g_rtc.magie == MAGIE && g_rtc.taille == sizeof(Journal)) {
    g_avant = (Journal*)heap_caps_malloc(sizeof(Journal), MALLOC_CAP_SPIRAM);
    if (g_avant) {
      memcpy(g_avant, &g_rtc, sizeof(Journal));
      Panique& p = g_avant->panique;
      p.raison[sizeof(p.raison) - 1] = '\0';
      for (char* c = p.raison; *c; c++) if ((uint8_t)*c < 0x20 || (uint8_t)*c > 0x7E) *c = '?';
      if (p.profondeur[0] > PROFONDEUR) p.profondeur[0] = PROFONDEUR;
      if (p.profondeur[1] > PROFONDEUR) p.profondeur[1] = PROFONDEUR;
      if (g_avant->imbriquee.profondeur > 6) g_avant->imbriquee.profondeur = 6;
      ranger(*g_avant);
      fabriquerRejeu(*g_avant);
    }
  }
  vider(g_rtc);
  g_pret = true;
  set_arduino_panic_handler(paniqueArduino, nullptr);
}

void noter(const char* texte) {
  if (!g_pret || !texte || !*texte) return;
  // Les surveillants — l'audio (bloc lent), le lien du cable (lien mort) :
  // leur rapport ne doit pas etre chasse par le bavardage qui suit.
  const bool rapport = strncmp(texte, "[audio]", 7) == 0 || strncmp(texte, "[usbnet]", 8) == 0;
  const uint32_t t = millis();
  // Le verrou ne fait que RESERVER la case : la copie, en RTC lente, se fait
  // hors section critique. Deux ecrivains sur la meme case supposeraient dix
  // lignes simultanees.
  Ligne* l;
  uint32_t n;
  portENTER_CRITICAL(&g_verrou);
  n = g_rtc.suivant++;
  if (rapport) {
    l = &g_rtc.rapport[g_rtc.rapportProchain];
    g_rtc.rapportProchain = (g_rtc.rapportProchain + 1) % N_RAPPORT;
    if (g_rtc.rapportNb < N_RAPPORT) g_rtc.rapportNb++;
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

uint8_t nbLignesRapport() { return g_avant ? g_avant->rapportNb : 0; }

uint8_t nbLignesRejeu() { return g_rejeu ? g_nbRejeu : 0; }

const char* ligneRejeu(uint8_t i) {
  return (g_rejeu && i < g_nbRejeu) ? g_rejeu + g_debutRejeu[i] : nullptr;
}

bool panique(PaniqueAvant& q) {
  if (!g_avant || g_avant->panique.magie != MAGIE_PANIQUE) return false;
  const Panique& p = g_avant->panique;
  q.coeur = p.coeur;
  q.raison = p.raison;
  q.raisonAdr = p.raisonAdr;
  q.adresse = p.adresse;
  q.pc = p.pc;
  q.exccause = p.exccause;
  q.excvaddr = p.excvaddr;
  for (uint8_t c = 0; c < 2; c++) { q.pile[c] = p.pile[c]; q.profondeur[c] = p.profondeur[c]; }
  const PaniqueImbriquee& m = g_avant->imbriquee;
  q.imbriquees = m.nb;
  q.imbriqueeCoeur = m.coeur;
  q.imbriqueeRaisonAdr = m.raisonAdr;
  q.imbriqueePc = m.pc;
  q.imbriqueePile = m.pile;
  q.imbriqueeProfondeur = m.nb ? m.profondeur : 0;
  return true;
}

}  // namespace JournalAvant

/* LE CORE DUMP N'EST PLUS ECRIT (MESURES §163). Le 26/09, son ecriture
 * n'aboutissait jamais : le chien de garde des interruptions la coupait
 * (pile d'appels : esp_core_dump_write → esp_core_dump_setup_stack), la
 * carte perdait une seconde ou davantage avant de redemarrer — et le releve
 * de la panique qui l'avait demandee etait ecrase. Le releve ci-dessus, en
 * memoire RTC, le remplace : la carte redemarre tout de suite. Edition de
 * liens : -Wl,--wrap=esp_core_dump_write (scripts/nidmi.sh). */
extern "C" void __wrap_esp_core_dump_write(panic_info_t* info) { (void)info; }
