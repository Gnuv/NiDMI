#include "CompoStore.h"
#include "../config/Stockage.h"
#include "../config/EcrituresDifferees.h"
#include "Repertoire.h"
#include "../server/ServerCore.h"     // nidmi_ws_pousser : les onglets apprennent qu'une source est arrivee
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Compo {
namespace {

constexpr const char* PARTITION = Stockage::PARTITION;
constexpr const char* BASE      = Stockage::BASE;

/* La composition la plus recente, rendue par GET. L'ecriture en flash, elle,
 * passe par Differe (au silence) — qui partage ce meme tampon, sans copie. */
SemaphoreHandle_t verrou = nullptr;
std::shared_ptr<char> actuelle;
size_t octetsActuels = 0;
uint32_t revisionActuelle = 0;     // le CRC32 de `actuelle` ; 0 : pas de source (sous `verrou`)

/* LE CRC32 de la source. Jamais 0 pour un contenu : 0 dit « pas de source ». */
uint32_t crc(const char* d, size_t n) {
  if (!d || !n) return 0;
  const uint32_t r = esp_rom_crc32_le(0, (const uint8_t*)d, (uint32_t)n);
  return r ? r : 1;
}

std::shared_ptr<char> tamponPsram(size_t n) {
  char* p = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) return nullptr;
  return std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); });
}

// Un motif dans un tampon qui n'est pas termine par un nul : son indice, ou -1.
long chercher(const char* d, size_t n, size_t depuis, const char* motif) {
  const size_t m = strlen(motif);
  for (size_t i = depuis; i + m <= n; i++) if (!memcmp(d + i, motif, m)) return (long)i;
  return -1;
}

/* LE TITRE DE LA COMPOSITION, pris dans « "meta":{… "name":"…" …} ». La carte ne
 * lit pas la source — pas de bibliotheque JSON, et elle n'en a pas besoin — :
 * elle y cherche ce seul champ, pour nommer le dossier d'une composition qui
 * arrive sur un repertoire vide. Les echappements se decodent : l'app envoie
 * ses accents tels quels, mais un autre outil les ecrit \u00e9 — le banc du
 * §186 l'a fait, et la composition s'appelait « ru00e9pertoire ». Les paires de
 * substitution (un emoji) sont omises ; Repertoire::nomValide tranche ensuite. */
String titreDe(const char* d, size_t n) {
  const long meta = chercher(d, n, 0, "\"meta\"");
  if (meta < 0) return String();
  const long nom = chercher(d, n, (size_t)meta, "\"name\"");
  if (nom < 0) return String();
  size_t i = (size_t)nom + 6;
  while (i < n && (d[i] == ' ' || d[i] == ':')) i++;
  if (i >= n || d[i] != '"') return String();
  String t;
  for (i++; i < n && d[i] != '"' && t.length() < 96; i++) {
    if (d[i] != '\\') { t += d[i]; continue; }
    if (++i >= n) break;
    const char e = d[i];
    if (e == 'u' && i + 4 < n) {
      const char hex[5] = { d[i + 1], d[i + 2], d[i + 3], d[i + 4], 0 };
      const uint32_t c = strtoul(hex, nullptr, 16);
      i += 4;
      if (c < 0x80) t += (char)c;
      else if (c < 0x800) { t += (char)(0xC0 | (c >> 6)); t += (char)(0x80 | (c & 0x3F)); }
      else if (c < 0xD800 || c > 0xDFFF) {
        t += (char)(0xE0 | (c >> 12)); t += (char)(0x80 | ((c >> 6) & 0x3F)); t += (char)(0x80 | (c & 0x3F));
      }
    } else if (e == '"' || e == '\\' || e == '/') {
      t += e;
    }                                        // \n, \t… : omis
  }
  return t;
}

}  // namespace

void demarrer() {
  if (!verrou) verrou = xSemaphoreCreateMutex();
  recharger();
}

void recharger() {
  if (!verrou) return;
  std::shared_ptr<char> t;
  size_t n = 0;
  const String fichier = Repertoire::chemin(Repertoire::SOURCE);
  if (!fichier.length() || !Differe::lire(fichier.c_str(), t, n) || !n || n > MAX_OCTETS) {
    t.reset(); n = 0;
  }
  const uint32_t rev = crc(t.get(), n);       // hors du verrou : c'est un parcours du tampon
  xSemaphoreTake(verrou, portMAX_DELAY);
  actuelle = t;
  octetsActuels = n;
  revisionActuelle = rev;
  xSemaphoreGive(verrou);
}

std::shared_ptr<char> courante(size_t& octets, uint32_t* revision) {
  if (!verrou) { octets = 0; if (revision) *revision = 0; return nullptr; }
  xSemaphoreTake(verrou, portMAX_DELAY);
  auto t = actuelle;
  octets = octetsActuels;
  if (revision) *revision = revisionActuelle;
  xSemaphoreGive(verrou);
  return t;
}

uint32_t revision() {
  if (!verrou) return 0;
  xSemaphoreTake(verrou, portMAX_DELAY);
  const uint32_t r = revisionActuelle;
  xSemaphoreGive(verrou);
  return r;
}

bool adopter(std::shared_ptr<char> tampon, size_t octets, String& raison,
             bool aBase, uint32_t base, bool* obsolete, uint32_t* revision) {
  if (obsolete) *obsolete = false;
  if (!verrou) { raison = "magasin non demarre"; return false; }
  const String titre = titreDe(tampon.get(), octets);
  if (!Repertoire::assurerOuverte(titre, raison)) return false;
  /* UNE COMPOSITION « SANS TITRE » ET SANS SOURCE PREND LE TITRE DE LA PREMIERE
   * QUI ARRIVE (§186) : elle a ete creee avant son contenu — « Nouveau », la
   * fin d'un banc — et n'etait pas encore nommee. Un titre invalide, ou deja
   * pris : elle garde son nom, et la source arrive quand meme. */
  size_t deja = 0;
  if (!courante(deja) && Repertoire::nomOuvert() == Repertoire::SANS_TITRE
      && titre.length() && titre != "untitled" && titre != Repertoire::SANS_TITRE) {
    String pourquoi;
    if (Repertoire::nomValide(titre, pourquoi)) Repertoire::renommer(Repertoire::numeroOuvert(), titre, pourquoi);
  }
  const uint32_t nouvelle = crc(tampon.get(), octets);
  /* COMPARER, PUIS POSER, SOUS LE MEME VERROU (MESURES §198). Posee avant la
   * comparaison, une source refusee partirait quand meme en flash au silence ;
   * deux envois qui se croisent laissaient aussi la flash a l'un et la memoire
   * a l'autre. Differe ne rappelle jamais ce magasin : l'ordre des verrous est
   * toujours celui-ci, puis le sien. */
  xSemaphoreTake(verrou, portMAX_DELAY);
  if (aBase && revisionActuelle && base != revisionActuelle) {
    if (obsolete) *obsolete = true;
    if (revision) *revision = revisionActuelle;
    xSemaphoreGive(verrou);
    raison = "la composition a change ailleurs (un autre onglet, un autre appareil)";
    return false;
  }
  if (!Differe::poserFichier(Repertoire::chemin(Repertoire::SOURCE).c_str(), tampon, octets)) {
    xSemaphoreGive(verrou);
    raison = "file des ecritures differees pleine : reessayer";
    return false;
  }
  actuelle = tampon;          // l'ancienne est rendue quand son dernier lecteur la lache
  octetsActuels = octets;
  revisionActuelle = nouvelle;
  xSemaphoreGive(verrou);
  if (revision) *revision = nouvelle;
  /* LES AUTRES ONGLETS L'APPRENNENT — un evenement, jamais un sondage. Ils ne
   * reprennent rien d'eux-memes : ils disent qu'ils sont en retard, et c'est
   * l'operateur qui decide quand (une scene ne doit pas changer d'un coup). */
  if (nidmi_ws_quelqu_un_ecoute()) {
    char trame[40];
    snprintf(trame, sizeof trame, "NIDMI_COMPO:%u:%08x",
             (unsigned)Repertoire::numeroOuvert(), (unsigned)nouvelle);
    nidmi_ws_pousser(trame);
  }
  return true;
}

String etatJson() {
  size_t n = 0;
  courante(n);
  std::shared_ptr<char> t;
  size_t na = 0;
  bool supprime = false;
  const bool attend = Differe::attente(Repertoire::chemin(Repertoire::SOURCE).c_str(), t, na, supprime);
  char rev[9];
  snprintf(rev, sizeof rev, "%08x", (unsigned)revision());
  String j = "{\"octets\":" + String((unsigned)n);
  j += ",\"revision\":\"";
  j += rev;
  j += "\",\"en_attente\":";
  j += attend ? "true" : "false";
  j += "}";
  return j;
}

}  // namespace Compo
