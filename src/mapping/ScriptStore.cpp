#include "ScriptStore.h"
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

String listerJson() {
  if (!monter()) return "[]";
  String out = "[";
  File d = LittleFS.open(DOSSIER);
  if (d && d.isDirectory()) {
    bool premier = true;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      if (f.isDirectory()) continue;
      String n = String(f.name());
      const int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      if (!premier) out += ",";
      premier = false;
      out += "{\"name\":\"" + n + "\",\"bytes\":" + String((unsigned)f.size()) + "}";
    }
  }
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
  return LittleFS.exists(_chemin(nom));
}

bool ecrire(const char* nom, const String& contenu) {
  if (!nom || !*nom || !monter()) return false;
  if (contenu.length() > TAILLE_MAX) {
    Serial.printf("[scripts] %s refuse : %u o > %u\n",
                  nom, (unsigned)contenu.length(), (unsigned)TAILLE_MAX);
    return false;
  }
  File f = LittleFS.open(_chemin(nom), FILE_WRITE);
  if (!f) return false;
  const size_t ecrit = f.print(contenu);
  f.close();
  Serial.printf("[scripts] %s ecrit (%u o)\n", nom, (unsigned)ecrit);
  return ecrit == contenu.length();
}

bool supprimer(const char* nom) {
  if (!nom || !*nom || !monter()) return false;
  return LittleFS.remove(_chemin(nom));
}

bool lire(const char* nom, String& contenu) {
  contenu = "";
  if (!nom || !*nom || !monter()) return false;
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
