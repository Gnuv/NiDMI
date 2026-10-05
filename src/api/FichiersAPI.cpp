// src/api/FichiersAPI.cpp — CE QUE LA CARTE GARDE, ET CE QU'ON PEUT EN FAIRE
// (MESURES §179).
//
// La colonne « Fichiers » du panneau Carte avait ete dessinee pour une API de
// fichiers generique que le firmware n'a jamais servie : sur une vraie carte,
// elle ne montrait qu'« echec ». Et l'arborescence qu'elle imaginait
// (/compositions, /sequences, /presets) n'etait pas celle de la carte.
//
// Ici, la carte DIT ce qu'elle porte et ce que chaque fichier permet ; l'app
// n'a pas a connaitre l'arborescence ni les regles :
//   GET    /api/fichiers           la liste : genre, taille, qui s'en sert, droits
//   GET    /api/fichier?chemin=    le contenu (telecharger)
//   POST   /api/fichier?nom=       corps brut ; la CARTE range selon l'extension
//   DELETE /api/fichier?chemin=    si le genre le permet
//
// Chaque genre passe par SON magasin : un son par SampleStore (verifie, jouable
// des la reponse — §177), un script par ScriptStore, une interface enregistree
// par Differe (ecrite au silence, §190). La liste de cues et la composition viennent de
// l'app (« Installer ») : elles se telechargent, elles ne se suppriment pas
// d'ici.
//
// LES SUPPORTS (§180). Tout ce qui precede vit sur la memoire interne —
// `storage`, LittleFS. Une carte SD viendra : chaque fichier dit donc sur QUEL support il
// est (`volume`), la liste declare les supports et leur etat, et les routes
// prennent `volume=` (`storage` par defaut). Ce firmware declare la SD pour dire
// qu'il ne la lit pas encore — l'app n'a pas a l'inventer.
#include "APICommon.h"
#include "../config/Stockage.h"
#include "../config/CarteSd.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "../audio/AudioEngine.h"
#include "../audio/SampleStore.h"
#include "../mapping/ScriptStore.h"
#include "../mapping/CueStore.h"
#include "../mapping/CompoStore.h"
#include "../midi/MidiRouter.h"
#include "../config/EcrituresDifferees.h"
#include "../mapping/Repertoire.h"
#include "../config/Instrument.h"
#include "../config/Interface.h"

namespace {

constexpr const char* VOLUME_INTERNE = Stockage::PARTITION;   // LittleFS, la memoire interne
/* Les interfaces enregistrees sur la carte (§190) : des .interface — des ZIP
 * que l'app construit —, gardes tels quels. Quelques dizaines de ko. */
constexpr size_t      INTERFACE_MAX  = 128 * 1024;
constexpr size_t      LISTE_MAX      = 32 * 1024;   // la reponse de /api/fichiers, en PSRAM
constexpr int         USAGES_MAX     = 64;

enum class Genre { Son, Script, Cues, Composition, Chaine, Options, Cc, Texte, Interface, Autre };

const char* nomDuGenre(Genre g) {
  switch (g) {
    case Genre::Son:           return "son";
    case Genre::Script:        return "script";
    case Genre::Cues:          return "cues";
    case Genre::Composition:   return "composition";
    case Genre::Chaine:        return "chaine";
    case Genre::Options:       return "options";
    case Genre::Cc:            return "cc";
    case Genre::Texte:         return "texte";
    case Genre::Interface:     return "interface";
    default:                   return "autre";
  }
}

// `chemin` est-il un fichier posé directement dans `dossier` ?
bool dans(const String& chemin, const char* dossier) {
  const size_t n = strlen(dossier);
  return chemin.length() > n + 1 && chemin.startsWith(dossier) && chemin[n] == '/'
         && chemin.indexOf('/', n + 1) < 0;
}

/* Le genre se lit au CHEMIN (§186) : les sons dans samples/, les scripts de
 * broche dans interface/scripts/, et dans une composition — compositions/<nn>/
 * <nom>/ — des noms generiques qui disent chacun ce qu'ils sont. */
Genre genreDe(const String& chemin) {
  // Les README au-dessus des compositions : l'instrument, l'interface (§188).
  if (chemin == Instrument::LISEZMOI || chemin == Instrument::LISEZMOI_INTERFACE) return Genre::Texte;
  if (dans(chemin, SampleStore::DOSSIER))            return Genre::Son;
  if (dans(chemin, ScriptStore::DOSSIER_INTERFACE))  return Genre::Script;
  if (dans(chemin, Interface::DOSSIER_ENREGISTREES)) return Genre::Interface;
  uint8_t numero; String nom, fichier;
  if (Repertoire::decouper(chemin, numero, nom, fichier) && fichier.indexOf('/') < 0) {
    if (fichier == Repertoire::SOURCE)   return Genre::Composition;
    if (fichier == Repertoire::CUES)     return Genre::Cues;
    if (fichier == Repertoire::CHAINE)   return Genre::Chaine;
    if (fichier == Repertoire::OPTIONS)  return Genre::Options;
    if (fichier == Repertoire::CC)       return Genre::Cc;
    if (fichier == Repertoire::LISEZMOI) return Genre::Texte;
    if (fichier.endsWith(".nms"))        return Genre::Script;
  }
  return Genre::Autre;
}

// Ce qu'on peut TELEVERSER, et ou la carte le range : elle decide par l'extension.
Genre genreTeleverse(const String& nom) {
  String n = nom; n.toLowerCase();
  if (n.endsWith(".wav"))  return Genre::Son;
  if (n.endsWith(".interface")) return Genre::Interface;
  return Genre::Autre;
}

/* La source, la liste de cues, la chaine, les options et les CC appris d'une
 * composition viennent de l'app : les supprimer d'ici laisserait la carte jouer
 * ce que plus rien ne decrit. « Installer » les remplace. */
bool supprimable(Genre g) {
  return g != Genre::Cues && g != Genre::Composition && g != Genre::Chaine && g != Genre::Options
      && g != Genre::Cc;
}

// Absolu, sans remontee, court, et pas une ecriture en cours (.tmp de Differe,
// .part d'un televersement).
bool cheminValide(const String& c) {
  return c.length() > 1 && c.length() < 160 && c[0] == '/' && c.indexOf("..") < 0
         && !c.endsWith(".tmp") && !c.endsWith(".part");
}

String baseDe(const String& c) { return c.substring(c.lastIndexOf('/') + 1); }

const char* typeDe(const String& chemin) {
  if (chemin.endsWith(".wav"))  return "audio/wav";
  if (chemin.endsWith(".json")) return "application/json";
  return "text/plain; charset=utf-8";
}

/* Le support vise par la requete ; storage si rien n'est dit. Un autre : refuse
 * (501) — la SD ne fait que se LIRE (ses sons, par le magasin) : elle se remplit
 * depuis un ordinateur, rien d'autre n'y passe par la carte. */
bool volumeInterne(AsyncWebServerRequest* r) {
  return !r->hasParam("volume") || r->getParam("volume")->value() == VOLUME_INTERNE;
}

void repondre(AsyncWebServerRequest* r, int code, const String& message) {
  String m = message; m.replace("\"", "'");
  r->send(code, "application/json",
          String("{\"status\":\"") + (code == 200 ? "ok" : "error") + "\",\"message\":\"" + m + "\"}");
}

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

// ── Parcourir storage ──────────────────────────────────────────────────────────
// A toute profondeur — une composition vit trois niveaux sous la racine (§186) —,
// bornee a six niveaux : l'arbre n'en a pas plus. Corrige de ce qui attend le
// silence pour s'ecrire (Differe) : la liste dit ce que la carte PORTE, pas ce
// que la flash a deja recu — comme ScriptStore::listerJson.
template <typename U> void parcourirDossier(const String& dossier, int profondeur, U& un) {
  File d = LittleFS.open(dossier);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    const String p = f.path();
    if (f.isDirectory()) { if (profondeur < 6) parcourirDossier(p, profondeur + 1, un); continue; }
    un(p, f.size());
  }
}

template <typename F> void parcourir(F voir) {
  String vus;
  auto un = [&](const String& chemin, size_t octetsFlash) {
    if (!cheminValide(chemin)) return;
    vus += "|" + chemin + "|";
    std::shared_ptr<char> t; size_t na = 0; bool sup = false;
    if (Differe::attente(chemin.c_str(), t, na, sup)) { if (!sup) voir(chemin, na, true); return; }
    voir(chemin, octetsFlash, false);
  };
  parcourirDossier(String("/"), 0, un);
  struct Ctx { String* vus; F* voir; };
  Ctx ctx{ &vus, &voir };
  Differe::visiterAttente("/", [](const char* chemin, size_t n, bool supprime, void* c) {
    Ctx& x = *(Ctx*)c;
    const String p = chemin;
    if (supprime || !cheminValide(p) || x.vus->indexOf("|" + p + "|") >= 0) return;
    (*x.voir)(p, n, true);
  }, &ctx);
}

/* LES SONS DE LA CARTE SD : /samples/*.wav, la meme forme que storage. Rien d'autre
 * n'y est liste — elle ne porte, pour ce firmware, que des sons (CarteSd.h). */
template <typename F> void parcourirCarteSd(F voir) {
  File d = CarteSd::ouvrir(CarteSd::DOSSIER);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    const String nom = baseDe(f.path());
    if (genreTeleverse(nom) != Genre::Son) continue;
    voir(String(SampleStore::DOSSIER) + "/" + nom, (size_t)f.size());
  }
}

}  // namespace

void setupFichiersAPI(AsyncWebServer& server) {

  /* LE DIAGNOSTIC DE LA CARTE SD — la carte n'a pas de port serie : c'est ici qu'elle
   * dit pourquoi la SD ne se monte pas (CMD0/CMD8 sondes a la main) et ce que vaut sa
   * lecture. `?essai=1` : un nouvel essai tout de suite. `?cablage=1` : sonde le cablage d'une carte qui ne monte pas. `?mesure=<nom.wav>[&hz=…]` :
   * lit ce son de bout en bout, dans la tache de la carte (jamais ici), et chronometre
   * chaque morceau ; on relit ensuite jusqu'a `mesure.etat == "finie"`. Un geste, a la
   * demande : rien ne le sonde. */
  server.on("/api/diag/sd", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("essai")) CarteSd::essayer();
    if (request->hasParam("cablage")) CarteSd::sonderCablage();
    if (request->hasParam("mesure")) {
      uint32_t hz = request->hasParam("hz") ? (uint32_t)request->getParam("hz")->value().toInt() : 0;
      if (hz && (hz < 400000 || hz > 40000000)) hz = 0;
      CarteSd::mesurer(request->getParam("mesure")->value().c_str(), hz);
    }
    request->send(200, "application/json", CarteSd::diagnostic());
  });

  /* LA LISTE. Parcourt storage a chaque appel : c'est un GESTE (ouvrir la colonne,
   * apres un televersement), jamais un sondage — la regle du §161. L'occupation,
   * elle, est celle que la carte garde et remesure au silence (ScriptStore). */
  server.on("/api/fichiers", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!SampleStore::monter()) { repondre(request, 503, "storage non monte"); return; }
    Ecrit e;
    e.cap = LISTE_MAX;
    e.t = nidmi_tampon_reponse(e.cap);
    Usage* u = (Usage*)heap_caps_malloc(sizeof(Usage) * USAGES_MAX, MALLOC_CAP_SPIRAM);
    if (!e.t || !u) { if (u) heap_caps_free(u); repondre(request, 507, "memoire"); return; }
    const int nu = releverUsages(u);

    size_t fichiers, scripts, contenu, utilises, total;
    ScriptStore::infos(fichiers, scripts, contenu, utilises, total);
    const bool charges = SampleStore::charge();
    /* LA CARTE SD : declaree par le composant `sd_spi` (I/O), montee dans sa
     * tache. Branchee apres coup, elle se reconnait a l'ouverture de cette liste
     * (un nouvel essai, sans attendre ici). `utilise` = la taille des sons qu'on
     * en liste : l'occupation reelle d'une FAT demande de relire toute sa table. */
    if (CarteSd::declaree() && !CarteSd::monte()) CarteSd::reessayer();
    const bool sdMontee = CarteSd::monte();
    uint64_t sdUtilise = 0;
    if (sdMontee) parcourirCarteSd([&](const String&, size_t octets) { sdUtilise += octets; });
    e.formater("{\"volumes\":[{\"id\":\"%s\",\"nom\":\"Mémoire interne\",\"type\":\"littlefs\","
               "\"prise_en_charge\":true,\"presente\":true,\"total\":%u,\"utilise\":%u},"
               "{\"id\":\"%s\",\"nom\":\"Carte SD\",\"type\":\"sd\","
               "\"prise_en_charge\":true,\"declaree\":%s,\"presente\":%s,\"total\":%llu,\"utilise\":%llu}],",
               VOLUME_INTERNE, (unsigned)total, (unsigned)utilises,
               CarteSd::VOLUME, CarteSd::declaree() ? "true" : "false", sdMontee ? "true" : "false",
               (unsigned long long)CarteSd::total(), (unsigned long long)sdUtilise);
    e.formater("\"televersables\":[{\"extension\":\".wav\",\"genre\":\"son\",\"volume\":\"%s\",\"dossier\":\"%s\"},"
               "{\"extension\":\".interface\",\"genre\":\"interface\",\"volume\":\"%s\",\"dossier\":\"%s\"}],",
               VOLUME_INTERNE, SampleStore::DOSSIER, VOLUME_INTERNE, Interface::DOSSIER_ENREGISTREES);
    e.ajouter("\"liste\":[");
    bool premier = true;
    int n = 0;
    const String ouvert = Repertoire::dossierOuvert();
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
      /* Qui s'en sert : un son, ou un script de la composition OUVERTE — les
       * cues et la chaine qu'on releve sont les siennes. */
      const bool ouverte = ouvert.length() && chemin.startsWith(ouvert + "/");
      if (g == Genre::Son || (g == Genre::Script && ouverte)) {
        const char* par = usageDe(u, nu, nom);
        if (par) { e.ajouter(",\"utilise_par\":"); e.chaine(par); }
      }
      e.ajouter("}");
    });
    if (sdMontee) parcourirCarteSd([&](const String& chemin, size_t octets) {
      const String nom = baseDe(chemin);
      if (!premier) e.ajouter(",");
      premier = false;
      n++;
      e.ajouter("{\"chemin\":"); e.chaine(chemin.c_str());
      e.formater(",\"volume\":\"%s\",\"octets\":%u,\"genre\":\"son\",\"supprimable\":false,\"en_attente\":false",
                 CarteSd::VOLUME, (unsigned)octets);
      if (charges)
        e.formater(",\"lisible\":%s", SampleStore::indexDe(nom.c_str()) >= 0 ? "true" : "false");
      const char* par = usageDe(u, nu, nom);
      if (par) { e.ajouter(",\"utilise_par\":"); e.chaine(par); }
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
    if (!volumeInterne(request)) { repondre(request, 501, "la carte SD se lit seule pour l'instant : ni telechargement, ni televersement, ni suppression par la carte"); return; }
    if (!cheminValide(chemin)) { repondre(request, 400, "chemin invalide"); return; }
    if (!SampleStore::monter()) { repondre(request, 503, "storage non monte"); return; }
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
   * televersements, chiffree au §177. Une interface : un ZIP, gardee en PSRAM,
   * ecrite au silence (Differe), rangee avec les interfaces enregistrees (§190). */
  server.on("/api/fichier", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      const String nom = request->hasParam("nom") ? request->getParam("nom")->value() : String();
      String raison;
      if (!volumeInterne(request)) { repondre(request, 501, "la carte SD se lit seule pour l'instant : ni telechargement, ni televersement, ni suppression par la carte"); return; }
      switch (genreTeleverse(nom)) {
        case Genre::Son: {
          if (!SampleStore::ecrireFin(request, raison)) { repondre(request, 422, raison); return; }
          if (!AudioEngine::echantillonArrive(nom.c_str(), raison)) { repondre(request, 507, raison); return; }
          request->send(200, "application/json",
            "{\"status\":\"ok\",\"chemin\":\"" + String(SampleStore::DOSSIER) + "/" + nom + "\"}");
          return;
        }
        case Genre::Interface: {
          const char* t = (const char*)request->_tempObject;
          const size_t n = request->contentLength();
          const String base = nom.substring(0, nom.length() - strlen(".interface"));
          if (!Repertoire::nomValide(base, raison)) { repondre(request, 400, raison); return; }
          if (!n || n > INTERFACE_MAX || !t) {
            repondre(request, 413, "interface vide ou trop grosse (" + String((unsigned)INTERFACE_MAX) + " o au plus)");
            return;
          }
          if (n < 4 || memcmp(t, "PK\x03\x04", 4)) { repondre(request, 400, "un .interface est un ZIP"); return; }
          const String chemin = String(Interface::DOSSIER_ENREGISTREES) + "/" + nom;
          if (!Differe::poserFichierCopie(chemin.c_str(), t, n)) {
            repondre(request, 507, "file des ecritures differees pleine : reessayer au silence");
            return;
          }
          request->send(200, "application/json",
            "{\"status\":\"ok\",\"chemin\":\"" + chemin + "\",\"en_attente\":true}");
          return;
        }
        default:
          repondre(request, 415, "la carte range les sons (.wav) et les interfaces (.interface)");
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
        case Genre::Interface:
          if (index == 0 && total && total <= INTERFACE_MAX)
            request->_tempObject = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (request->_tempObject && index + len <= total)
            memcpy((char*)request->_tempObject + index, data, len);
          return;
        default:
          return;                                  // refuse a la fin, avec la raison
      }
    });

  /* SUPPRIMER, si le genre le permet. Un son se tait et rend sa PSRAM (§177) ;
   * un script ou une interface enregistree s'effacent au silence (Differe). */
  server.on("/api/fichier", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    const String chemin = request->hasParam("chemin") ? request->getParam("chemin")->value() : String();
    if (!volumeInterne(request)) { repondre(request, 501, "la carte SD se lit seule pour l'instant : ni telechargement, ni televersement, ni suppression par la carte"); return; }
    if (!cheminValide(chemin)) { repondre(request, 400, "chemin invalide"); return; }
    const Genre g = genreDe(chemin);
    if (!supprimable(g)) {
      repondre(request, 403, "la source, les cues, la chaine et les options d'une composition "
                             "viennent de l'app : Installer les remplace");
      return;
    }
    if (!SampleStore::monter()) { repondre(request, 503, "storage non monte"); return; }
    const String nom = baseDe(chemin);
    bool ok = false;
    switch (g) {
      case Genre::Son:
        AudioEngine::echantillonParti(nom.c_str());
        ok = SampleStore::supprimer(nom.c_str());
        break;
      case Genre::Script:
        ok = ScriptStore::supprimerChemin(chemin);
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
