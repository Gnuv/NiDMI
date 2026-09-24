#include "ScriptStore.h"
#include "../config/EcrituresDifferees.h"
#include <LittleFS.h>

namespace ScriptStore {
namespace {

constexpr const char* PARTITION = "mapfs";     // meme partition que les echantillons
constexpr const char* BASE      = "/mapfs";
constexpr const char* DOSSIER   = "/scripts";
constexpr size_t      TAILLE_MAX = 8192;       // un .nms tient tres largement dedans

bool _monte = false;

// Pas de traversee de chemin : on ne garde que le nom de base.
String _chemin(const char* nom) {
  String p = String(DOSSIER) + "/";
  const char* base = strrchr(nom, '/');
  p += (base ? base + 1 : nom);
  return p;
}

}  // namespace

bool estMonte() { return _monte; }

bool monter() {
  if (_monte) return true;
  // mapfs est PARTAGEE avec les echantillons : si elle est deja montee,
  // LittleFS.begin le voit et n'y touche pas.
  if (!LittleFS.begin(true, BASE, 10, PARTITION)) {
    Serial.println("[scripts] montage de mapfs impossible");
    return false;
  }
  if (!LittleFS.exists(DOSSIER)) LittleFS.mkdir(DOSSIER);
  _monte = true;
  return true;
}

/* LA LISTE DIT CE QUE LA CARTE PORTE, pas ce que la flash a deja recu : un
 * script envoye attend le silence pour s'ecrire (§157) — il existe pourtant,
 * une cue peut l'appeler. Donc : la flash, corrigee de ce qui attend. */
namespace {
struct Liste { String* out; String* vus; bool* premier; };
void _ajouter(Liste& l, const String& nom, size_t octets) {
  if (!*l.premier) *l.out += ",";
  *l.premier = false;
  *l.out += "{\"name\":\"" + nom + "\",\"bytes\":" + String((unsigned)octets) + "}";
  *l.vus += "|" + nom + "|";
}
}  // namespace

String listerJson() {
  if (!monter()) return "[]";
  String out = "[", vus;
  bool premier = true;
  Liste l{ &out, &vus, &premier };
  File d = LittleFS.open(DOSSIER);
  if (d && d.isDirectory()) {
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (f.isDirectory()) continue;
      String n = String(f.name());
      const int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      if (n.endsWith(".tmp")) continue;            // une ecriture en cours
      std::shared_ptr<char> t; size_t na = 0; bool sup = false;
      if (Differe::attente(_chemin(n.c_str()).c_str(), t, na, sup)) {
        if (!sup) _ajouter(l, n, na);
        else vus += "|" + n + "|";
        continue;
      }
      _ajouter(l, n, f.size());
    }
  }
  // Ce qui attend et que la flash n'a pas encore.
  Differe::visiterAttente((String(DOSSIER) + "/").c_str(),
    [](const char* chemin, size_t n, bool supprime, void* ctx) {
      Liste& l = *(Liste*)ctx;
      const char* base = strrchr(chemin, '/');
      const String nom = base ? base + 1 : chemin;
      if (supprime || l.vus->indexOf("|" + nom + "|") >= 0) return;
      _ajouter(l, nom, n);
    }, &l);
  out += "]";
  return out;
}

void infos(size_t& fichiers, size_t& scripts, size_t& octetsContenu,
           size_t& octetsUtilises, size_t& octetsTotal) {
  fichiers = scripts = octetsContenu = octetsUtilises = octetsTotal = 0;
  if (!monter()) return;
  octetsTotal    = LittleFS.totalBytes();
  octetsUtilises = LittleFS.usedBytes();
  /* Deux niveaux suffisent : la racine porte les echantillons et les cues,
   * /scripts porte les .nms. Pas de recursion generale — il n'y a pas d'autre
   * niveau, et en inventer un serait du code qu'aucun cas n'exerce. */
  File racine = LittleFS.open("/");
  if (!racine || !racine.isDirectory()) return;
  for (File f = racine.openNextFile(); f; f = racine.openNextFile()) {
    if (!f.isDirectory()) { fichiers++; octetsContenu += f.size(); continue; }
    File d = LittleFS.open(f.path());
    if (!d || !d.isDirectory()) continue;
    const bool estScripts = (String(f.path()) == DOSSIER);
    for (File g = d.openNextFile(); g; g = d.openNextFile()) {
      if (g.isDirectory()) continue;
      fichiers++; octetsContenu += g.size();
      if (estScripts) scripts++;
    }
  }
}

bool existe(const char* nom) {
  if (!nom || !*nom || !monter()) return false;
  std::shared_ptr<char> t; size_t n = 0; bool sup = false;
  if (Differe::attente(_chemin(nom).c_str(), t, n, sup)) return !sup;
  return LittleFS.exists(_chemin(nom));
}

/* Recu tout de suite, ecrit en flash au premier silence (§157) : l'ecriture
 * qui efface arrete l'audio. lire() rend deja le nouveau contenu. */
bool ecrire(const char* nom, const String& contenu) {
  if (!nom || !*nom || !monter()) return false;
  if (contenu.length() > TAILLE_MAX) {
    Serial.printf("[scripts] %s refuse : %u o > %u\n",
                  nom, (unsigned)contenu.length(), (unsigned)TAILLE_MAX);
    return false;
  }
  if (!Differe::poserFichierCopie(_chemin(nom).c_str(), contenu.c_str(), contenu.length()))
    return false;
  Serial.printf("[scripts] %s recu (%u o, flash au premier silence)\n", nom, (unsigned)contenu.length());
  return true;
}

bool supprimer(const char* nom) {
  if (!nom || !*nom || !monter() || !existe(nom)) return false;
  return Differe::supprimerFichier(_chemin(nom).c_str());
}

bool lire(const char* nom, String& contenu) {
  contenu = "";
  if (!nom || !*nom || !monter()) return false;
  {
    std::shared_ptr<char> t; size_t n = 0; bool sup = false;
    if (Differe::attente(_chemin(nom).c_str(), t, n, sup)) {
      if (sup) return false;
      if (t && n) contenu.concat(t.get(), (unsigned)n);
      return true;
    }
  }
  File f = LittleFS.open(_chemin(nom), FILE_READ);
  if (!f) return false;
  // On lit d'un bloc : un .nms est petit, et un flux caractere par caractere
  // sur LittleFS coute bien plus cher que la lecture elle-meme.
  contenu.reserve(f.size() + 1);
  while (f.available()) contenu += (char)f.read();
  f.close();
  return true;
}

}  // namespace ScriptStore
