// src/api/FichiersAPI.cpp — CE QUE LA CARTE GARDE, ET CE QU'ON PEUT EN FAIRE
// (MESURES §179).
//
// La colonne « Fichiers » du panneau Carte avait ete dessinee pour une API de
// fichiers generique que le firmware n'a jamais servie : sur une vraie carte,
// elle ne montrait qu'« echec ». Et l'arborescence qu'elle imaginait
// (/compositions, /sequences, /presets) n'etait pas celle de mapfs.
//
// Ici, la carte DIT ce qu'elle porte et ce que chaque fichier permet ; l'app
// n'a pas a connaitre l'arborescence ni les regles :
//   GET    /api/fichiers           la liste : genre, taille, qui s'en sert, droits
//   GET    /api/fichier?chemin=    le contenu (telecharger)
//   POST   /api/fichier?nom=       corps brut ; la CARTE range selon l'extension
//   DELETE /api/fichier?chemin=    si le genre le permet
//
// Chaque genre passe par SON magasin : un son par SampleStore (verifie, jouable
// des la reponse — §177), un script par ScriptStore, une configuration par
// Differe (ecrite au silence). La liste de cues et la composition viennent de
// l'app (« Installer ») : elles se telechargent, elles ne se suppriment pas
// d'ici.
//
// LES SUPPORTS (§180). Tout ce qui precede vit sur la memoire interne — mapfs,
// LittleFS. Une carte SD viendra : chaque fichier dit donc sur QUEL support il
// est (`volume`), la liste declare les supports et leur etat, et les routes
// prennent `volume=` (mapfs par defaut). Ce firmware declare la SD pour dire
// qu'il ne la lit pas encore — l'app n'a pas a l'inventer.
#include "APICommon.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "../audio/AudioEngine.h"
#include "../audio/SampleStore.h"
#include "../mapping/ScriptStore.h"
#include "../mapping/CueStore.h"
#include "../mapping/CompoStore.h"
#include "../midi/MidiRouter.h"
#include "../config/EcrituresDifferees.h"

namespace {

constexpr const char* DOSSIER_CONFIG = "/config";
constexpr const char* VOLUME_INTERNE = "mapfs";     // LittleFS, la memoire interne
constexpr size_t      CONFIG_MAX     = 32 * 1024;   // une configuration exportee pese quelques ko
constexpr size_t      LISTE_MAX      = 32 * 1024;   // la reponse de /api/fichiers, en PSRAM
constexpr int         USAGES_MAX     = 64;

enum class Genre { Son, Script, Cues, Composition, Configuration, Autre };

const char* nomDuGenre(Genre g) {
  switch (g) {
    case Genre::Son:           return "son";
    case Genre::Script:        return "script";
    case Genre::Cues:          return "cues";
    case Genre::Composition:   return "composition";
    case Genre::Configuration: return "configuration";
    default:                   return "autre";
  }
}

// `chemin` est-il un fichier posé directement dans `dossier` ?
bool dans(const String& chemin, const char* dossier) {
  const size_t n = strlen(dossier);
  return chemin.length() > n + 1 && chemin.startsWith(dossier) && chemin[n] == '/'
         && chemin.indexOf('/', n + 1) < 0;
}

Genre genreDe(const String& chemin) {
  if (dans(chemin, SampleStore::DOSSIER)) return Genre::Son;
  if (dans(chemin, ScriptStore::DOSSIER)) return Genre::Script;
  if (chemin == Cues::FICHIER)            return Genre::Cues;
  if (chemin == Compo::FICHIER)           return Genre::Composition;
  if (dans(chemin, DOSSIER_CONFIG))       return Genre::Configuration;
  return Genre::Autre;
}

// Ce qu'on peut TELEVERSER, et ou la carte le range : elle decide par l'extension.
Genre genreTeleverse(const String& nom) {
  String n = nom; n.toLowerCase();
  if (n.endsWith(".wav"))  return Genre::Son;
  if (n.endsWith(".json")) return Genre::Configuration;
  return Genre::Autre;
}

/* La liste de cues et la composition viennent de l'app : les supprimer d'ici
 * laisserait la carte jouer ce que plus rien ne decrit. « Installer » les
 * remplace. */
bool supprimable(Genre g) { return g != Genre::Cues && g != Genre::Composition; }

// Absolu, sans remontee, court, et pas une ecriture en cours (.tmp de Differe,
// .part d'un televersement).
bool cheminValide(const String& c) {
  return c.length() > 1 && c.length() < 64 && c[0] == '/' && c.indexOf("..") < 0
         && !c.endsWith(".tmp") && !c.endsWith(".part");
}

String baseDe(const String& c) { return c.substring(c.lastIndexOf('/') + 1); }

const char* typeDe(const String& chemin) {
  if (chemin.endsWith(".wav"))  return "audio/wav";
  if (chemin.endsWith(".json")) return "application/json";
  return "text/plain; charset=utf-8";
}

/* Le support vise par la requete ; mapfs si rien n'est dit. Un autre : refuse
 * (501) — la SD n'est pas encore lue par ce firmware. */
bool volumeInterne(AsyncWebServerRequest* r) {
  return !r->hasParam("volume") || r->getParam("volume")->value() == VOLUME_INTERNE;
}

void repondre(AsyncWebServerRequest* r, int code, const String& message) {
  String m = message; m.replace("\"", "'");
  r->send(code, "application/json",
          String("{\"status\":\"") + (code == 200 ? "ok" : "error") + "\",\"message\":\"" + m + "\"}");
}

// ── La reponse, ecrite en PSRAM ──────────────────────────────────────────────
// Le tas interne est le reservoir qui decide (§150) : une liste de fichiers qui
// grandit par reallocations y laisserait des trous. Un tampon PSRAM, rempli une
// fois, part tel quel (nidmi_reponse_tampon).
struct Ecrit {
  std::shared_ptr<char> t;
  size_t cap = 0, n = 0;
  bool deborde = false;
  void brut(const char* s, size_t k) {
    if (deborde || n + k >= cap) { deborde = true; return; }
    memcpy(t.get() + n, s, k); n += k;
  }
  void ajouter(const char* s) { brut(s, strlen(s)); }
  void formater(const char* fmt, ...) {
    if (deborde) return;
    va_list a; va_start(a, fmt);
    const int k = vsnprintf(t.get() + n, cap - n, fmt, a);
    va_end(a);
    if (k < 0 || n + (size_t)k >= cap) { deborde = true; return; }
    n += (size_t)k;
  }
  void chaine(const char* s) {                       // une chaine JSON, echappee
    ajouter("\"");
    for (; *s; s++) {
      if (*s == '"' || *s == '\\') { const char e[2] = {'\\', *s}; brut(e, 2); }
      else if ((uint8_t)*s < 0x20) formater("\\u%04x", (unsigned)(uint8_t)*s);
      else brut(s, 1);
    }
    ajouter("\"");
  }
};

// ── Qui s'en sert ────────────────────────────────────────────────────────────
// Les cues qui nomment un son ou un script (la carte a la liste : c'est elle qui
// la joue), et les maillons de la chaine qui en portent un EN CE MOMENT — un
// script pousse en ligne vit dans _emplacementN.nms, que la carte relit au
// demarrage : aucune cue ne le nomme, et le supprimer viderait le maillon.
// Supprimer un son qu'une cue joue, c'est une cue muette : l'app le dit avant.
struct Usage { char nom[SampleStore::NOM_MAX]; char par[72]; };

int noterUsage(Usage* u, int n, String nom, const String& par) {
  nom.trim();
  if (!nom.length() || nom.length() >= sizeof u[0].nom) return n;
  for (int i = 0; i < n; i++) {
    if (nom != u[i].nom) continue;
    if (strstr(u[i].par, par.c_str())) return n;          // deja dit
    static const char* const SEP = " · ";                  // UTF-8 : quatre octets
    static const char* const ETC = " …";
    const size_t lp = strlen(u[i].par);
    if (lp + strlen(SEP) + par.length() < sizeof u[i].par) {
      strcat(u[i].par, SEP); strcat(u[i].par, par.c_str());
    } else if (!strstr(u[i].par, ETC) && lp + strlen(ETC) < sizeof u[i].par) {
      strcat(u[i].par, ETC);
    }
    return n;
  }
  if (n >= USAGES_MAX) return n;
  strlcpy(u[n].nom, nom.c_str(), sizeof u[n].nom);
  strlcpy(u[n].par, par.c_str(), sizeof u[n].par);
  return n + 1;
}

// Chaque element d'une liste a virgules.
template <typename F> void chaqueNom(const String& liste, F f) {
  int d = 0;
  while (d < (int)liste.length()) {
    int e = liste.indexOf(',', d); if (e < 0) e = liste.length();
    f(liste.substring(d, e));
    d = e + 1;
  }
}

// La valeur d'une cle dans « cle=valeur;cle=valeur ».
String valeurDe(const String& params, const char* cle) {
  const String c = String(cle) + "=";
  int d = 0;
  while (d < (int)params.length()) {
    int e = params.indexOf(';', d); if (e < 0) e = params.length();
    String kv = params.substring(d, e); kv.trim();
    if (kv.startsWith(c)) return kv.substring(c.length());
    d = e + 1;
  }
  return String();
}

int releverUsages(Usage* u) {
  int n = 0;
  const int nc = Cues::nombre();
  for (int i = 0; i < nc; i++) {
    Cues::Cue c;
    if (!Cues::lire(i, c)) continue;
    const String qui = c.nom.length() ? c.nom : ("cue " + String(i + 1));
    chaqueNom(c.script, [&](const String& s) { n = noterUsage(u, n, s, qui); });
    if (c.engine == -2)
      chaqueNom(valeurDe(c.params, "sample"), [&](const String& s) { n = noterUsage(u, n, s, qui); });
  }
  for (uint8_t e = 0; e < g_midiRouter.nEmplacements(); e++)
    n = noterUsage(u, n, g_midiRouter.nomEmplacement(e),
                   e < g_midiRouter.nMaillonsPermanents() ? String("MAIN")
                                                          : "maillon " + String(e + 1));
  return n;
}

const char* usageDe(const Usage* u, int n, const String& nom) {
  for (int i = 0; i < n; i++) if (nom == u[i].nom) return u[i].par;
  return nullptr;
}

// ── Parcourir mapfs ──────────────────────────────────────────────────────────
// La racine et un niveau : il n'y en a pas d'autre. Corrige de ce qui attend le
// silence pour s'ecrire (Differe) : la liste dit ce que la carte PORTE, pas ce
// que la flash a deja recu — comme ScriptStore::listerJson.
template <typename F> void parcourir(F voir) {
  String vus;
  auto un = [&](const String& chemin, size_t octetsFlash) {
    if (!cheminValide(chemin)) return;
    vus += "|" + chemin + "|";
    std::shared_ptr<char> t; size_t na = 0; bool sup = false;
    if (Differe::attente(chemin.c_str(), t, na, sup)) { if (!sup) voir(chemin, na, true); return; }
    voir(chemin, octetsFlash, false);
  };
  File racine = LittleFS.open("/");
  if (racine && racine.isDirectory()) {
    for (File f = racine.openNextFile(); f; f = racine.openNextFile()) {
      const String p = f.path();
      if (!f.isDirectory()) { un(p, f.size()); continue; }
      File d = LittleFS.open(p);
      if (!d || !d.isDirectory()) continue;
      for (File g = d.openNextFile(); g; g = d.openNextFile())
        if (!g.isDirectory()) un(String(g.path()), g.size());
    }
  }
  struct Ctx { String* vus; F* voir; };
  Ctx ctx{ &vus, &voir };
  Differe::visiterAttente("/", [](const char* chemin, size_t n, bool supprime, void* c) {
    Ctx& x = *(Ctx*)c;
    const String p = chemin;
    if (supprime || !cheminValide(p) || x.vus->indexOf("|" + p + "|") >= 0) return;
    (*x.voir)(p, n, true);
  }, &ctx);
}

}  // namespace

void setupFichiersAPI(AsyncWebServer& server) {

  /* LA LISTE. Parcourt mapfs a chaque appel : c'est un GESTE (ouvrir la colonne,
   * apres un televersement), jamais un sondage — la regle du §161. L'occupation,
   * elle, est celle que la carte garde et remesure au silence (ScriptStore). */
  server.on("/api/fichiers", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!SampleStore::monter()) { repondre(request, 503, "mapfs non monte"); return; }
    Ecrit e;
    e.cap = LISTE_MAX;
    e.t = nidmi_tampon_reponse(e.cap);
    Usage* u = (Usage*)heap_caps_malloc(sizeof(Usage) * USAGES_MAX, MALLOC_CAP_SPIRAM);
    if (!e.t || !u) { if (u) heap_caps_free(u); repondre(request, 507, "memoire"); return; }
    const int nu = releverUsages(u);

    size_t fichiers, scripts, contenu, utilises, total;
    ScriptStore::infos(fichiers, scripts, contenu, utilises, total);
    const bool charges = SampleStore::charge();
    e.formater("{\"volumes\":[{\"id\":\"%s\",\"nom\":\"Mémoire interne\",\"type\":\"littlefs\","
               "\"prise_en_charge\":true,\"presente\":true,\"total\":%u,\"utilise\":%u},"
               "{\"id\":\"sd\",\"nom\":\"Carte SD\",\"type\":\"sd\","
               "\"prise_en_charge\":false,\"presente\":false}],",
               VOLUME_INTERNE, (unsigned)total, (unsigned)utilises);
    e.formater("\"televersables\":[{\"extension\":\".wav\",\"genre\":\"son\",\"volume\":\"%s\",\"dossier\":\"%s\"},"
               "{\"extension\":\".json\",\"genre\":\"configuration\",\"volume\":\"%s\",\"dossier\":\"%s\"}],",
               VOLUME_INTERNE, SampleStore::DOSSIER, VOLUME_INTERNE, DOSSIER_CONFIG);
    e.ajouter("\"liste\":[");
    bool premier = true;
    int n = 0;
    parcourir([&](const String& chemin, size_t octets, bool enAttente) {
      const Genre g = genreDe(chemin);
      const String nom = baseDe(chemin);
      if (!premier) e.ajouter(",");
      premier = false;
      n++;
      e.ajouter("{\"chemin\":"); e.chaine(chemin.c_str());
      e.formater(",\"volume\":\"%s\",\"octets\":%u,\"genre\":\"%s\",\"supprimable\":%s,\"en_attente\":%s",
                 VOLUME_INTERNE, (unsigned)octets, nomDuGenre(g), supprimable(g) ? "true" : "false",
                 enAttente ? "true" : "false");
      // Lisible : en PSRAM, donc jouable. Tant que le magasin n'a jamais ete
      // charge, on ne sait pas — on se tait plutot que d'annoncer « injouable ».
      if (g == Genre::Son && charges)
        e.formater(",\"lisible\":%s", SampleStore::indexDe(nom.c_str()) >= 0 ? "true" : "false");
      if (g == Genre::Son || g == Genre::Script) {
        const char* par = usageDe(u, nu, nom);
        if (par) { e.ajouter(",\"utilise_par\":"); e.chaine(par); }
      }
      e.ajouter("}");
    });
    heap_caps_free(u);
    e.formater("],\"fichiers\":%d}", n);   // le compte, tous supports
    if (e.deborde) { repondre(request, 507, "liste trop longue pour sa reponse"); return; }
    request->send(nidmi_reponse_tampon(request, "application/json", e.t, e.n));
  });

  /* TELECHARGER. Depuis ce qui attend le silence s'il y en a (c'est le plus
   * recent), sinon depuis la flash — lu d'un coup en PSRAM, puis servi. */
  server.on("/api/fichier", HTTP_GET, [](AsyncWebServerRequest* request) {
    const String chemin = request->hasParam("chemin") ? request->getParam("chemin")->value() : String();
    if (!volumeInterne(request)) { repondre(request, 501, "ce firmware ne lit pas encore de carte SD"); return; }
    if (!cheminValide(chemin)) { repondre(request, 400, "chemin invalide"); return; }
    if (!SampleStore::monter()) { repondre(request, 503, "mapfs non monte"); return; }
    std::shared_ptr<char> t; size_t n = 0; bool sup = false;
    if (Differe::attente(chemin.c_str(), t, n, sup)) {
      if (sup || !t) { repondre(request, 404, "introuvable"); return; }
      request->send(nidmi_reponse_tampon(request, typeDe(chemin), t, n));
      return;
    }
    File f = LittleFS.open(chemin, FILE_READ);
    if (!f || f.isDirectory()) { repondre(request, 404, "introuvable"); return; }
    n = f.size();
    auto b = nidmi_tampon_reponse(n ? n : 1);
    if (!b) { f.close(); repondre(request, 507, "memoire"); return; }
    const size_t lus = n ? f.read((uint8_t*)b.get(), n) : 0;
    f.close();
    if (lus != n) { repondre(request, 500, "lecture incomplete"); return; }
    request->send(nidmi_reponse_tampon(request, typeDe(chemin), b, n));
  });

  /* TELEVERSER. Corps BRUT, le nom en parametre d'URL ; la carte range selon
   * l'extension et DIT pourquoi elle refuse. Un son : verifie, jouable des
   * « ok » (§177) — il s'ecrit tout de suite, c'est l'exception des
   * televersements, chiffree au §177. Une configuration : gardee en PSRAM,
   * ecrite au silence (Differe). */
  server.on("/api/fichier", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      const String nom = request->hasParam("nom") ? request->getParam("nom")->value() : String();
      String raison;
      if (!volumeInterne(request)) { repondre(request, 501, "ce firmware ne lit pas encore de carte SD"); return; }
      switch (genreTeleverse(nom)) {
        case Genre::Son: {
          if (!SampleStore::ecrireFin(request, raison)) { repondre(request, 422, raison); return; }
          if (!AudioEngine::echantillonArrive(nom.c_str(), raison)) { repondre(request, 507, raison); return; }
          request->send(200, "application/json",
            "{\"status\":\"ok\",\"chemin\":\"" + String(SampleStore::DOSSIER) + "/" + nom + "\"}");
          return;
        }
        case Genre::Configuration: {
          const char* t = (const char*)request->_tempObject;
          const size_t n = request->contentLength();
          if (!SampleStore::nomValide(nom.c_str(), raison)) { repondre(request, 400, raison); return; }
          if (!n || n > CONFIG_MAX || !t) {
            repondre(request, 413, "configuration vide ou trop grosse (" + String((unsigned)CONFIG_MAX) + " o au plus)");
            return;
          }
          size_t k = 0;
          while (k < n && isspace((unsigned char)t[k])) k++;
          if (k == n || t[k] != '{') { repondre(request, 400, "une configuration est un objet JSON"); return; }
          const String chemin = String(DOSSIER_CONFIG) + "/" + nom;
          if (!Differe::poserFichierCopie(chemin.c_str(), t, n)) {
            repondre(request, 507, "file des ecritures differees pleine : reessayer au silence");
            return;
          }
          request->send(200, "application/json",
            "{\"status\":\"ok\",\"chemin\":\"" + chemin + "\",\"en_attente\":true}");
          return;
        }
        default:
          repondre(request, 415, "la carte range les sons (.wav) et les configurations (.json)");
      }
    },
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      const String nom = request->hasParam("nom") ? request->getParam("nom")->value() : String();
      if (!volumeInterne(request)) return;          // refuse a la fin, avec la raison
      switch (genreTeleverse(nom)) {
        case Genre::Son:
          if (index == 0) {
            /* La requete part avant la fin : rien n'est garde. Appele aussi apres
             * une fin normale — sans effet, le televersement est clos. */
            request->onDisconnect([request]() { SampleStore::ecrireAbandon(request); });
            SampleStore::ecrireDebut(request, nom.c_str());
          }
          SampleStore::ecrireMorceau(request, data, len);
          return;
        case Genre::Configuration:
          if (index == 0 && total && total <= CONFIG_MAX)
            request->_tempObject = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (request->_tempObject && index + len <= total)
            memcpy((char*)request->_tempObject + index, data, len);
          return;
        default:
          return;                                  // refuse a la fin, avec la raison
      }
    });

  /* SUPPRIMER, si le genre le permet. Un son se tait et rend sa PSRAM (§177) ;
   * un script ou une configuration s'effacent au silence (Differe). */
  server.on("/api/fichier", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    const String chemin = request->hasParam("chemin") ? request->getParam("chemin")->value() : String();
    if (!volumeInterne(request)) { repondre(request, 501, "ce firmware ne lit pas encore de carte SD"); return; }
    if (!cheminValide(chemin)) { repondre(request, 400, "chemin invalide"); return; }
    const Genre g = genreDe(chemin);
    if (!supprimable(g)) {
      repondre(request, 403, "la liste de cues et la composition viennent de l'app : Installer les remplace");
      return;
    }
    if (!SampleStore::monter()) { repondre(request, 503, "mapfs non monte"); return; }
    const String nom = baseDe(chemin);
    bool ok = false;
    switch (g) {
      case Genre::Son:
        AudioEngine::echantillonParti(nom.c_str());
        ok = SampleStore::supprimer(nom.c_str());
        break;
      case Genre::Script:
        ok = ScriptStore::supprimer(nom.c_str());
        break;
      default: {
        std::shared_ptr<char> t; size_t n = 0; bool sup = false;
        const bool attend = Differe::attente(chemin.c_str(), t, n, sup);
        const bool existe = attend ? !sup : LittleFS.exists(chemin);
        ok = existe && Differe::supprimerFichier(chemin.c_str());
        break;
      }
    }
    if (!ok) { repondre(request, 404, "introuvable"); return; }
    request->send(200, "application/json", "{\"status\":\"ok\",\"chemin\":\"" + chemin + "\"}");
  });
}
