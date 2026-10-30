#include "Library.h"
#include "StorageIndex.h"
#include "EcrituresDifferees.h"
#include <memory>

namespace Library {
namespace {

/* A NAME, NOT A PATH: no folder, nothing hidden, nothing a JSON string or a listing would
 * have to escape. Accents are fine (UTF-8 bytes above 0x7F). */
bool nameOk(const String& n, size_t max) {
  if (!n.length() || n.length() > max || n[0] == '.' || n.indexOf("..") >= 0) return false;
  for (size_t i = 0; i < n.length(); i++) {
    const char c = n[i];
    if ((uint8_t)c < 0x20 || strchr("/\\\":*?<>|", c)) return false;
  }
  return true;
}

/* An engine id as the app names it (`play-sf`, `plaits`), or a script-plugin's: the id of the
 * script it was saved as (SAVE NMS), which may hold spaces. A name, then. */
bool engineOk(const String& e) { return nameOk(e, 64); }

/* Names are checked on the way in (no quote, no backslash, no control character): quoting is enough. */
String quoted(const String& s) { return "\"" + s + "\""; }

struct Lists { String scripts, presets; String seen; bool firstScript = true, firstPreset = true; };

void add(Lists& l, const char* path, size_t bytes) {
  const String p = path;
  if (l.seen.indexOf("|" + p + "|") >= 0) return;
  l.seen += "|" + p + "|";
  const String scripts = String(SCRIPTS) + "/", presets = String(PRESETS) + "/";
  if (p.startsWith(scripts)) {
    const String name = p.substring(scripts.length());
    if (name.indexOf('/') >= 0 || !name.endsWith(".nms")) return;
    if (!l.firstScript) l.scripts += ",";
    l.firstScript = false;
    l.scripts += "{\"name\":" + quoted(name) + ",\"bytes\":" + String((unsigned)bytes) + "}";
  } else if (p.startsWith(presets)) {
    const String rest = p.substring(presets.length());
    const int slash = rest.indexOf('/');
    if (slash <= 0 || rest.indexOf('/', slash + 1) >= 0 || !rest.endsWith(".json")) return;
    if (!l.firstPreset) l.presets += ",";
    l.firstPreset = false;
    l.presets += "{\"engine\":" + quoted(rest.substring(0, slash))
               + ",\"name\":" + quoted(rest.substring(slash + 1, rest.length() - 5))
               + ",\"bytes\":" + String((unsigned)bytes) + "}";
  }
}

}  // namespace

String scriptPath(const String& name, String& why) {
  if (!name.endsWith(".nms") || !nameOk(name, 96)) { why = "nom de script invalide (x.nms, sans / ni \")"; return String(); }
  return String(SCRIPTS) + "/" + name;
}

String presetPath(const String& engine, const String& name, String& why) {
  if (!engineOk(engine)) { why = "moteur invalide"; return String(); }
  if (!nameOk(name, 80)) { why = "nom de preset invalide (sans / ni \")"; return String(); }
  return String(PRESETS) + "/" + engine + "/" + name + ".json";
}

/* What waits for the silence first (it is what the card holds, deleted or not), then the
 * index for the rest. */
String listJson() {
  Lists l;
  struct Ctx { Lists* l; };
  Ctx ctx{ &l };
  Differe::visiterAttente("/library/", [](const char* path, size_t n, bool deleted, void* c) {
    Lists& l = *((Ctx*)c)->l;
    if (deleted) { l.seen += "|" + String(path) + "|"; return; }
    add(l, path, n);
  }, &ctx);
  StorageIndex::visit("/library", true, [](const char* path, size_t size, bool dir, void* c) {
    if (!dir) add(*((Ctx*)c)->l, path, size);
  }, &ctx);
  return "{\"scripts\":[" + l.scripts + "],\"presets\":[" + l.presets + "]}";
}

bool read(const String& path, String& out) {
  out = "";
  std::shared_ptr<char> t;
  size_t n = 0;
  if (!Differe::lire(path.c_str(), t, n)) return false;
  if (t && n) out.concat(t.get(), (unsigned)n);
  return true;
}

bool write(const String& path, const char* data, size_t n, String& why) {
  if (!path.length()) { why = "chemin invalide"; return false; }
  if (n > FILE_MAX) { why = "trop gros (" + String((unsigned)n) + " o, " + String((unsigned)FILE_MAX) + " au plus)"; return false; }
  if (!Differe::poserFichierCopie(path.c_str(), data, n)) { why = "file d'ecriture pleine, reessayer apres le silence"; return false; }
  return true;
}

bool remove(const String& path) {
  std::shared_ptr<char> t;
  size_t n = 0, size = 0;
  bool deleted = false;
  const bool held = Differe::attente(path.c_str(), t, n, deleted) ? !deleted : StorageIndex::fileSize(path.c_str(), size);
  return held && Differe::supprimerFichier(path.c_str());
}

}  // namespace Library
