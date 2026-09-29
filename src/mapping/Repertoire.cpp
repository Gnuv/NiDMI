#include "Repertoire.h"
#include "../config/Stockage.h"
#include "../config/EcrituresDifferees.h"
#include "../audio/AudioEngine.h"
#include "../server/WebDebugConsole.h"
#include "../server/ServerCore.h"     // nidmi_ws_pousser : les onglets apprennent le changement
#include "ScriptStore.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Repertoire {
namespace {

constexpr const char* NVS_ESPACE = "nidmi";
constexpr const char* NVS_CLE    = "compo";

/* L'etat — le numero et le nom de la composition ouverte — est lu par le
 * serveur web, la boucle et les taches des scripts : il se prend sous un
 * verrou court, et se rend en copie. */
StaticSemaphore_t _tamponEtat;
SemaphoreHandle_t _verrouEtat = xSemaphoreCreateMutexStatic(&_tamponEtat);
struct VerrouEtat {
  VerrouEtat()  { xSemaphoreTake(_verrouEtat, portMAX_DELAY); }
  ~VerrouEtat() { xSemaphoreGive(_verrouEtat); }
};
/* La CREATION, elle, tient son propre verrou, le temps d'ecrire les dossiers :
 * deux envois qui arrivent ensemble sur un repertoire vide n'en creent qu'une. */
StaticSemaphore_t _tamponCreation;
SemaphoreHandle_t _verrouCreation = xSemaphoreCreateMutexStatic(&_tamponCreation);

uint8_t _numero = 0;
String  _nom;

/* UN GESTE A LA FOIS : deux onglets, ou un onglet et un bouton, qui ouvrent ou
 * copient ensemble ne doivent pas se croiser dans la flash. Recursif : un
 * geste en appelle un autre (nouvelle -> ouvrir). */
StaticSemaphore_t _tamponGeste;
SemaphoreHandle_t _verrouGeste = xSemaphoreCreateRecursiveMutexStatic(&_tamponGeste);
struct VerrouGeste {
  VerrouGeste()  { xSemaphoreTakeRecursive(_verrouGeste, portMAX_DELAY); }
  ~VerrouGeste() { xSemaphoreGiveRecursive(_verrouGeste); }
};

void (*_recharger)() = nullptr;
void (*_prevenir)()  = nullptr;

bool monter() { return LittleFS.begin(true, Stockage::BASE, 10, Stockage::PARTITION); }

String dossierIndex(uint8_t n) {
  char b[8];
  snprintf(b, sizeof b, "/%02u", (unsigned)n);
  return String(DOSSIER) + b;
}

String baseDe(const String& p) { return p.substring(p.lastIndexOf('/') + 1); }

/* Le nom range sous un dossier d'index : son UNIQUE sous-dossier. Aucun, ou
 * plusieurs — une copie faite a la main, par l'USB — : la carte le dit et ne
 * devine pas. */
bool nomSous(uint8_t n, String& nom, String* raison = nullptr) {
  File d = LittleFS.open(dossierIndex(n));
  if (!d || !d.isDirectory()) { if (raison) *raison = "absente"; return false; }
  int k = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) continue;
    if (++k == 1) nom = baseDe(String(f.name()));
  }
  if (k == 1) return true;
  if (raison) *raison = k ? "plusieurs dossiers sous ce numero : la carte ne choisit pas"
                          : "aucun dossier nomme sous ce numero";
  return false;
}

/* Les numeros presents, dans l'ordre : les sous-dossiers de DOSSIER nommes de
 * deux chiffres. Le reste n'est pas une composition, et n'est pas touche. */
template <typename F> void chaqueNumero(F f) {
  bool present[NUMERO_MAX + 1] = {false};
  File d = LittleFS.open(DOSSIER);
  if (d && d.isDirectory()) {
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (!e.isDirectory()) continue;
      const String n = baseDe(String(e.name()));
      if (n.length() != 2 || !isDigit(n[0]) || !isDigit(n[1])) continue;
      const int v = n.toInt();
      if (v >= 1 && v <= NUMERO_MAX) present[v] = true;
    }
  }
  for (int v = 1; v <= NUMERO_MAX; v++) if (present[v]) f((uint8_t)v);
}

// Chaque niveau manquant, du haut vers le bas : LittleFS ne cree qu'un niveau a la fois.
bool mkdirs(const String& chemin) {
  for (int i = 1; i <= (int)chemin.length(); i++) {
    if (i < (int)chemin.length() && chemin[i] != '/') continue;
    const String p = chemin.substring(0, i);
    if (!LittleFS.exists(p) && !LittleFS.mkdir(p)) return false;
  }
  return true;
}

/* Ecrire un fichier TOUT DE SUITE (a cote, puis renomme) — un geste de
 * preparation, annonce au moteur (§177). */
bool ecrireMaintenant(const String& chemin, const char* data, size_t n) {
  const String tmp = chemin + ".tmp";
  AudioEngine::ecritureFlashDebut();
  bool ok = false;
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (f) {
    ok = f.write((const uint8_t*)data, n) == n;
    f.close();
    ok = ok && LittleFS.rename(tmp, chemin);
    if (!ok) LittleFS.remove(tmp);
  }
  AudioEngine::ecritureFlashFin();
  return ok;
}

/* Les fichiers d'un dossier nomme (a plat : une composition n'a pas de
 * sous-dossier). */
template <typename F> void chaqueFichier(const String& dossier, F f) {
  File d = LittleFS.open(dossier);
  if (!d || !d.isDirectory()) return;
  for (File e = d.openNextFile(); e; e = d.openNextFile())
    if (!e.isDirectory()) f(baseDe(String(e.name())), (size_t)e.size());
}

/* Effacer un dossier et ce qu'il porte, a toute profondeur (bornee). */
bool effacerDossier(const String& dossier, int profondeur = 0) {
  File d = LittleFS.open(dossier);
  if (!d || !d.isDirectory()) return !LittleFS.exists(dossier);
  String enfants[16];
  bool estDossier[16];
  int k = 0;
  bool reste = false;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    if (k < 16) { enfants[k] = String(e.path()); estDossier[k] = e.isDirectory(); k++; }
    else reste = true;
  }
  d.close();
  bool ok = true;
  for (int i = 0; i < k; i++) {
    if (estDossier[i]) ok = (profondeur < 4 && effacerDossier(enfants[i], profondeur + 1)) && ok;
    else ok = LittleFS.remove(enfants[i]) && ok;
  }
  if (reste) ok = effacerDossier(dossier, profondeur) && ok;   // plus de seize : on repasse
  return LittleFS.rmdir(dossier) && ok;
}

void annoncer() {
  char trame[32];
  snprintf(trame, sizeof trame, "NIDMI_REPERTOIRE:%u", (unsigned)_numero);
  nidmi_ws_pousser(trame);
  if (_prevenir) _prevenir();
}

uint8_t premierLibre() {
  bool pris[NUMERO_MAX + 1] = {false};
  chaqueNumero([&](uint8_t n) { pris[n] = true; });
  for (uint8_t k = 1; k <= NUMERO_MAX; k++) if (!pris[k]) return k;
  return 0;
}

/* Le numero voulu (libre), ou le premier libre si 0. 0 : aucun. */
uint8_t numeroPour(uint8_t voulu, String& raison) {
  if (voulu) {
    if (voulu > NUMERO_MAX) { raison = "numero hors bornes (1 a " + String((unsigned)NUMERO_MAX) + ")"; return 0; }
    if (LittleFS.exists(dossierIndex(voulu))) { raison = "le n°" + String((unsigned)voulu) + " est pris"; return 0; }
    return voulu;
  }
  const uint8_t n = premierLibre();
  if (!n) raison = "repertoire plein (" + String((unsigned)NUMERO_MAX) + " compositions)";
  return n;
}

void ecrireJson(String& j, const String& s) {
  j += '"';
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') { j += '\\'; j += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", (unsigned)(uint8_t)c); j += b; }
    else j += c;
  }
  j += '"';
}

}  // namespace

bool nomValide(const String& nom, String& raison) {
  if (!nom.length()) { raison = "nom vide"; return false; }
  if (nom.length() > NOM_MAX) {
    raison = "nom trop long : " + String((unsigned)NOM_MAX) + " octets au plus (un accent en vaut deux)";
    return false;
  }
  const char dernier = nom[nom.length() - 1];
  if (nom[0] == ' ' || dernier == ' ' || dernier == '.') {
    raison = "ni espace au debut ou a la fin, ni point final";
    return false;
  }
  for (size_t i = 0; i < nom.length(); i++) {
    const uint8_t c = (uint8_t)nom[i];
    if (c < 0x20 || c == 0x7F) { raison = "caractere de controle interdit"; return false; }
    if (strchr("/\\:*?\"<>|", c)) {
      raison = String("caractere interdit : « ") + (char)c + " » (la carte SD et Windows le refusent)";
      return false;
    }
  }
  return true;
}

void demarrer() {
  if (!monter()) { NIDMI_WEB_LOG("[repertoire] storage non monte : aucune composition"); return; }
  uint8_t voulu = 0;
  {
    Preferences p;
    if (p.begin(NVS_ESPACE, true)) { voulu = p.getUChar(NVS_CLE, 0); p.end(); }
  }
  String nom, raison;
  uint8_t ouvert = 0;
  if (voulu && nomSous(voulu, nom, &raison)) {
    ouvert = voulu;
  } else {
    if (voulu) NIDMI_WEB_LOG("[repertoire] la n°%02u ne s'ouvre pas : %s", (unsigned)voulu, raison.c_str());
    // La premiere lisible du repertoire, sinon aucune.
    chaqueNumero([&](uint8_t n) {
      String k;
      if (!ouvert && nomSous(n, k)) { ouvert = n; nom = k; }
    });
    if (ouvert) Differe::nvsOctet(NVS_ESPACE, NVS_CLE, ouvert);   // au silence (§157)
  }
  { VerrouEtat v; _numero = ouvert; _nom = ouvert ? nom : String(); }
  if (ouvert) NIDMI_WEB_LOG("[repertoire] composition ouverte : n°%02u « %s »", (unsigned)ouvert, nom.c_str());
  else        NIDMI_WEB_LOG("[repertoire] aucune composition : la premiere viendra de l'app");
}

uint8_t numeroOuvert() { VerrouEtat v; return _numero; }
String  nomOuvert()    { VerrouEtat v; return _nom; }

String dossierOuvert() {
  uint8_t n; String nom;
  { VerrouEtat v; n = _numero; nom = _nom; }
  return n ? dossierIndex(n) + "/" + nom : String();
}

String chemin(const char* fichier) {
  const String d = dossierOuvert();
  return d.length() ? d + "/" + fichier : String();
}

bool assurerOuverte(const String& nomPropose, String& raison) {
  if (numeroOuvert()) return true;
  xSemaphoreTake(_verrouCreation, portMAX_DELAY);
  struct Rendre { ~Rendre() { xSemaphoreGive(_verrouCreation); } } rendre;
  if (numeroOuvert()) return true;                     // l'autre envoi l'a creee
  if (!monter()) { raison = "storage non monte"; return false; }
  String nom = nomPropose;
  nom.trim();
  String pourquoi;
  if (nom == "untitled" || !nomValide(nom, pourquoi)) nom = SANS_TITRE;
  bool pris[NUMERO_MAX + 1] = {false};
  chaqueNumero([&](uint8_t n) { pris[n] = true; });
  uint8_t n = 0;
  for (uint8_t k = 1; k <= NUMERO_MAX && !n; k++) if (!pris[k]) n = k;
  if (!n) { raison = "repertoire plein (" + String((unsigned)NUMERO_MAX) + " compositions)"; return false; }
  const String dossier = dossierIndex(n) + "/" + nom;
  AudioEngine::ecritureFlashDebut();
  const bool ok = mkdirs(dossier);
  AudioEngine::ecritureFlashFin();
  if (!ok) { raison = "creation impossible : " + dossier; return false; }
  Differe::noterFichiersModifies();
  { VerrouEtat v; _numero = n; _nom = nom; }
  Differe::nvsOctet(NVS_ESPACE, NVS_CLE, n);                // au silence (§157)
  NIDMI_WEB_LOG("[repertoire] composition creee et ouverte : n°%02u « %s »", (unsigned)n, nom.c_str());
  return true;
}

bool decouper(const String& chemin, uint8_t& numero, String& nom, String& fichier) {
  const String prefixe = String(DOSSIER) + "/";
  if (!chemin.startsWith(prefixe)) return false;
  const int a = prefixe.length();                       // debut du numero
  if ((int)chemin.length() < a + 4 || chemin[a + 2] != '/') return false;
  if (!isDigit(chemin[a]) || !isDigit(chemin[a + 1])) return false;
  const int b = chemin.indexOf('/', a + 3);             // fin du nom
  if (b < 0) return false;
  numero  = (uint8_t)chemin.substring(a, a + 2).toInt();
  nom     = chemin.substring(a + 3, b);
  fichier = chemin.substring(b + 1);
  return numero >= 1 && nom.length() && fichier.length();
}

String listerJson() {
  String j = "{\"ouverte\":" + String((unsigned)numeroOuvert()) + ",\"compositions\":[";
  bool premier = true;
  if (monter()) {
    chaqueNumero([&](uint8_t n) {
      String nom, raison;
      const bool ok = nomSous(n, nom, &raison);
      if (!premier) j += ",";
      premier = false;
      j += "{\"numero\":" + String((unsigned)n) + ",\"nom\":";
      if (ok) ecrireJson(j, nom);
      else { j += "null,\"probleme\":"; ecrireJson(j, raison); }
      j += "}";
    });
  }
  j += "]}";
  return j;
}

void surOuverture(void (*recharger)()) { _recharger = recharger; }
void surChangement(void (*prevenir)())  { _prevenir = prevenir; }

bool ouvrir(uint8_t numero, String& raison) {
  VerrouGeste geste;
  if (!monter()) { raison = "storage non monte"; return false; }
  String nom;
  if (!numero || !nomSous(numero, nom, &raison)) {
    raison = "n°" + String((unsigned)numero) + " : " + (raison.length() ? raison : String("absente"));
    return false;
  }
  { VerrouEtat v; _numero = numero; _nom = nom; }
  Differe::nvsOctet(NVS_ESPACE, NVS_CLE, numero);           // au silence (§157)
  NIDMI_WEB_LOG("[repertoire] ouverte : n°%02u « %s »", (unsigned)numero, nom.c_str());
  if (_recharger) _recharger();
  annoncer();
  return true;
}

bool nouvelle(const String& nom, uint8_t numero, String& raison, uint8_t& obtenu) {
  VerrouGeste geste;
  obtenu = 0;
  if (!nomValide(nom, raison) || !monter()) { if (!raison.length()) raison = "storage non monte"; return false; }
  const uint8_t n = numeroPour(numero, raison);
  if (!n) return false;
  AudioEngine::ecritureFlashDebut();
  const bool ok = mkdirs(dossierIndex(n) + "/" + nom);
  AudioEngine::ecritureFlashFin();
  if (!ok) { raison = "creation impossible"; return false; }
  Differe::noterFichiersModifies();
  obtenu = n;
  return ouvrir(n, raison);
}

bool enregistrerSous(const String& nom, uint8_t numero, String& raison, uint8_t& obtenu) {
  VerrouGeste geste;
  obtenu = 0;
  if (!nomValide(nom, raison)) return false;
  const String source = dossierOuvert();
  if (!source.length()) { raison = "aucune composition ouverte a copier"; return false; }
  if (!monter()) { raison = "storage non monte"; return false; }
  const uint8_t n = numeroPour(numero, raison);
  if (!n) return false;
  const String cible = dossierIndex(n) + "/" + nom;
  /* Tout ce qui attend d'abord : la copie part de la flash, entiere. */
  Differe::ecrireSousMaintenant((source + "/").c_str());
  AudioEngine::ecritureFlashDebut();
  bool ok = mkdirs(cible);
  AudioEngine::ecritureFlashFin();
  int copies = 0;
  if (ok) chaqueFichier(source, [&](const String& fichier, size_t) {
    if (!ok || fichier.endsWith(".tmp")) return;
    std::shared_ptr<char> t; size_t k = 0;
    ok = Differe::lire((source + "/" + fichier).c_str(), t, k) && ecrireMaintenant(cible + "/" + fichier, t.get(), k);
    if (ok) copies++;
  });
  Differe::noterFichiersModifies();
  if (!ok) {
    effacerDossier(dossierIndex(n));                        // ne rien laisser de moitie
    raison = "copie impossible (storage pleine ?)";
    return false;
  }
  /* ON CONTINUE SUR LA COPIE : elle est identique a ce qui est en memoire —
   * la source, la liste, la chaine. Rien ne se relit, rien ne s'arrete. */
  { VerrouEtat v; _numero = n; _nom = nom; }
  Differe::nvsOctet(NVS_ESPACE, NVS_CLE, n);
  obtenu = n;
  NIDMI_WEB_LOG("[repertoire] enregistree sous : n°%02u « %s » (%d fichier(s)) — on continue sur la copie",
                (unsigned)n, nom.c_str(), copies);
  annoncer();
  return true;
}

bool renommer(uint8_t numero, const String& nom, String& raison) {
  VerrouGeste geste;
  if (!nomValide(nom, raison) || !monter()) { if (!raison.length()) raison = "storage non monte"; return false; }
  String ancien;
  if (!nomSous(numero, ancien, &raison)) { raison = "n°" + String((unsigned)numero) + " : " + raison; return false; }
  if (ancien == nom) return true;
  const String de = dossierIndex(numero) + "/" + ancien, vers = dossierIndex(numero) + "/" + nom;
  bool ok;
  {
    /* PERSONNE NE CALCULE UN CHEMIN PENDANT QU'IL CHANGE : un magasin qui en
     * demande un attend la fin du renommage — sinon une ecriture posee sous
     * l'ancien nom recreerait l'ancien dossier au silence. */
    VerrouEtat v;
    Differe::ecrireSousMaintenant((de + "/").c_str());
    ScriptStore::oublierSous((de + "/").c_str());
    AudioEngine::ecritureFlashDebut();
    ok = LittleFS.rename(de, vers);
    AudioEngine::ecritureFlashFin();
    if (ok && _numero == numero) _nom = nom;
  }
  Differe::noterFichiersModifies();
  if (!ok) { raison = "renommage impossible"; return false; }
  NIDMI_WEB_LOG("[repertoire] n°%02u renommee « %s »", (unsigned)numero, nom.c_str());
  annoncer();
  return true;
}

bool changerNumero(uint8_t de, uint8_t vers, String& raison) {
  VerrouGeste geste;
  if (!monter()) { raison = "storage non monte"; return false; }
  String nom;
  if (!nomSous(de, nom, &raison)) { raison = "n°" + String((unsigned)de) + " : " + raison; return false; }
  if (!vers || vers > NUMERO_MAX) { raison = "numero hors bornes (1 a " + String((unsigned)NUMERO_MAX) + ")"; return false; }
  if (vers == de) return true;
  if (LittleFS.exists(dossierIndex(vers))) { raison = "le n°" + String((unsigned)vers) + " est pris"; return false; }
  bool ok;
  {
    VerrouEtat v;                                           // voir renommer
    Differe::ecrireSousMaintenant((dossierIndex(de) + "/").c_str());
    ScriptStore::oublierSous((dossierIndex(de) + "/").c_str());
    AudioEngine::ecritureFlashDebut();
    ok = LittleFS.rename(dossierIndex(de), dossierIndex(vers));
    AudioEngine::ecritureFlashFin();
    if (ok && _numero == de) _numero = vers;
  }
  Differe::noterFichiersModifies();
  if (!ok) { raison = "deplacement impossible"; return false; }
  if (numeroOuvert() == vers) Differe::nvsOctet(NVS_ESPACE, NVS_CLE, vers);
  NIDMI_WEB_LOG("[repertoire] « %s » : n°%02u -> n°%02u", nom.c_str(), (unsigned)de, (unsigned)vers);
  annoncer();
  return true;
}

bool supprimer(uint8_t numero, String& raison) {
  VerrouGeste geste;
  if (!monter()) { raison = "storage non monte"; return false; }
  if (numero == numeroOuvert()) { raison = "c'est la composition ouverte : en ouvrir une autre d'abord"; return false; }
  const String dossier = dossierIndex(numero);
  if (!LittleFS.exists(dossier)) { raison = "n°" + String((unsigned)numero) + " : absente"; return false; }
  /* Ce qui attendait d'y etre ecrit ne l'est plus : sinon, au silence, le
   * dossier renaitrait. */
  Differe::oublierSous((dossier + "/").c_str());
  ScriptStore::oublierSous((dossier + "/").c_str());
  AudioEngine::ecritureFlashDebut();
  const bool ok = effacerDossier(dossier);
  AudioEngine::ecritureFlashFin();
  Differe::noterFichiersModifies();
  if (!ok) { raison = "effacement incomplet"; return false; }
  NIDMI_WEB_LOG("[repertoire] n°%02u supprimee", (unsigned)numero);
  annoncer();
  return true;
}

uint8_t voisine(int sens) {
  if (!monter()) return 0;
  uint8_t presents[NUMERO_MAX];
  int k = 0;
  chaqueNumero([&](uint8_t n) { String nom; if (nomSous(n, nom)) presents[k++] = n; });
  if (!k) return 0;
  const uint8_t ouvert = numeroOuvert();
  int i = -1;
  for (int j = 0; j < k; j++) if (presents[j] == ouvert) i = j;
  if (i < 0) return presents[sens >= 0 ? 0 : k - 1];
  const int suivant = ((i + (sens >= 0 ? 1 : -1)) % k + k) % k;
  return presents[suivant] == ouvert ? 0 : presents[suivant];
}

}  // namespace Repertoire
