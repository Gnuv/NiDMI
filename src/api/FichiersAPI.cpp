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
// prennent `volume=` (`storage` par defaut).
//
// LA SD ET L'EDITEUR SFZ (MESURES §248). Sur `volume=sd`, ces routes ne voient que
// /sfz : les kits et leurs samples. GET lit un `.sfz` ou un `.wav` (refuse en
// concert : le bus de la SD est aux flux) ; POST ecrit un `.sfz` ou un `.wav` —
// `nom` est alors le CHEMIN sous la racine (/sfz/kit/kit.sfz) — par la tache
// `sd-card`, qui recharge ensuite les kits en memoire qui le lisent. Les deux rendent
// la main (202) : la tache `sd-card` lit ou ecrit — jamais async_tcp, une lecture
// multi-secteurs d'ici faisait tomber la carte —, GET /api/sd/job dit ou elle en
// est, GET /api/sd/read?job= sert ce qu'elle a lu (une ecriture est annoncee aussi :
// NIDMI_SD_WRITE). Pas de suppression sur la SD.
#include "APICommon.h"
#include "../config/StorageIndex.h"
#include "../config/Stockage.h"
#include "../config/SdCard.h"
#include "../audio/SfzStore.h"  // lister() : le diagnostic des dossiers de la SD (§208)
#include "../config/Concert.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "../audio/AudioEngine.h"
#include "../audio/SampleStore.h"
#include "../audio/SdStream.h"
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
  if (chemin.endsWith(".wav") || chemin.endsWith(".WAV")) return "audio/wav";
  if (chemin.endsWith(".json")) return "application/json";
  return "text/plain; charset=utf-8";
}

/* The medium the request aims at; storage if none is said. The SD has its own branch in each
 * route (volumeSd: the kits under /sfz, §248); any other medium is refused (501). */
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
// Ce que l'index de storage porte (StorageIndex, §241), a toute profondeur, corrige
// de ce qui attend le silence pour s'ecrire (Differe) : la liste dit ce que la carte
// PORTE, pas ce que la flash a deja recu — comme ScriptStore::listerJson.

template <typename F> void parcourir(F voir) {
  String vus;
  auto un = [&](const String& chemin, size_t octetsFlash) {
    if (!cheminValide(chemin)) return;
    vus += "|" + chemin + "|";
    std::shared_ptr<char> t; size_t na = 0; bool sup = false;
    if (Differe::attente(chemin.c_str(), t, na, sup)) { if (!sup) voir(chemin, na, true); return; }
    voir(chemin, octetsFlash, false);
  };
  /* From storage's index, never the flash (§241): this walk took 9 s in the web server. */
  struct CtxIndex { decltype(un)* un; };
  CtxIndex ci{ &un };
  StorageIndex::visit("/", true, [](const char* path, size_t size, bool dir, void* c) {
    if (!dir) (*((CtxIndex*)c)->un)(String(path), size);
  }, &ci);
  struct Ctx { String* vus; F* voir; };
  Ctx ctx{ &vus, &voir };
  Differe::visiterAttente("/", [](const char* chemin, size_t n, bool supprime, void* c) {
    Ctx& x = *(Ctx*)c;
    const String p = chemin;
    if (supprime || !cheminValide(p) || x.vus->indexOf("|" + p + "|") >= 0) return;
    (*x.voir)(p, n, true);
  }, &ctx);
}

/* THE SD CARD'S SOUNDS: /samples/*.wav, the same layout as storage. Nothing else is
 * listed — for this firmware it only holds sounds (SdCard.h). */
template <typename F> void walkSdCard(F voir) {
  File d = SdCard::openForWeb(SdCard::FOLDER);        // the web server's walk: never on a card in doubt
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    const String nom = baseDe(f.path());
    if (nom.startsWith(".") || genreTeleverse(nom) != Genre::Son) continue;   // macOS "._x.wav": not a sound
    voir(String(SampleStore::DOSSIER) + "/" + nom, (size_t)f.size());
  }
}

/* ── LA SD : les kits SFZ (§248) ─────────────────────────────────────────── */
constexpr size_t SD_SFZ_MAX = 64 * 1024;            // SfzStore's own ceiling for a .sfz text
constexpr size_t SD_WAV_MAX = 3 * 1024 * 1024;      // a sample; the kits share a 4 MB budget
constexpr size_t SD_PSRAM_MARGIN = 1024 * 1024;     // PSRAM left free, all blocks together, while a body waits

bool volumeSd(AsyncWebServerRequest* r) {
  return r->hasParam("volume") && r->getParam("volume")->value() == SdCard::VOLUME_ID;
}

/* Under /sfz, a `.sfz` or a `.wav`, a name FAT takes and a cue can say. */
bool cheminKit(const String& c, String& raison) {
  String bas = c; bas.toLowerCase();
  const String dossier = String(SfzStore::DOSSIER) + "/";
  if (!c.startsWith(dossier) || c.length() <= dossier.length() || c.length() >= 180) {
    raison = "un fichier de kit se range sous " + dossier; return false;
  }
  if (c.indexOf("..") >= 0 || c.indexOf("//") >= 0 || c.indexOf('\\') >= 0) { raison = "chemin invalide"; return false; }
  for (size_t i = 0; i < c.length(); i++) {
    const char k = c[i];
    if ((uint8_t)k < 0x20 || strchr(":*?\"<>|,;=", k)) { raison = String("caractere refuse dans le nom : ") + k; return false; }
  }
  if (baseDe(c).startsWith(".")) { raison = "nom cache refuse"; return false; }
  if (!bas.endsWith(".sfz") && !bas.endsWith(".wav")) { raison = "la SD prend des .sfz et des .wav"; return false; }
  return true;
}

/* A WAV the SFZ store can read: RIFF/WAVE, PCM 16 bits, mono or stereo. */
bool wavJouable(const uint8_t* d, size_t n, String& raison) {
  if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { raison = "pas un fichier WAV"; return false; }
  for (size_t p = 12; p + 8 <= n;) {
    const uint32_t taille = d[p + 4] | (d[p + 5] << 8) | (d[p + 6] << 16) | ((uint32_t)d[p + 7] << 24);
    if (!memcmp(d + p, "fmt ", 4) && p + 24 <= n) {
      const uint16_t format = d[p + 8] | (d[p + 9] << 8), canaux = d[p + 10] | (d[p + 11] << 8);
      const uint16_t bits = d[p + 22] | (d[p + 23] << 8);
      if ((format != 1 && format != 0xFFFE) || bits != 16) {
        raison = "WAV " + String(bits) + " bits : la carte lit le PCM 16 bits"; return false;
      }
      if (canaux < 1 || canaux > 2) { raison = "WAV a " + String(canaux) + " canaux : mono ou stereo"; return false; }
      return true;
    }
    p += 8 + taille + (taille & 1);
  }
  raison = "WAV sans bloc fmt"; return false;
}

}  // namespace

void setupFichiersAPI(AsyncWebServer& server) {
  /* WHERE THE LAST SD JOB IS (§248) — asked after a GET or a POST volume=sd, until it is done. */
  server.on("/api/sd/job", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", SdCard::jobStatus());
  });

  /* WHAT A READ JOB READ (§248), once: from PSRAM, the SD is not touched here. */
  server.on("/api/sd/read", HTTP_GET, [](AsyncWebServerRequest* request) {
    const uint32_t job = request->hasParam("job") ? (uint32_t)request->getParam("job")->value().toInt() : 0;
    uint8_t* data = nullptr; size_t n = 0; String chemin, raison;
    const int code = SdCard::takeRead(job, data, n, chemin, raison);
    if (code != 200) { repondre(request, code, raison); return; }
    std::shared_ptr<char> t((char*)data, [](char* p) { heap_caps_free(p); });
    request->send(nidmi_reponse_tampon(request, typeDe(chemin), t, n));
  });


  /* THE SD CARD DIAGNOSTICS — the board has no serial port: this is where it says why
   * the SD does not mount (CMD0/CMD8 probed by hand) and what its reading is worth.
   * `?retry=1`: a new attempt right now. `?wiring=1`: probes the wiring of a card that
   * will not mount. `?measure=<name.wav>[&hz=...]`: reads that sound end to end, in the
   * card's task (never here), and times every chunk; read again afterwards until
   * `measure.state == "finished"`. `?simulate_loss=1[&ms=1500]` (bench): every read fails
   * for that long, as a card pulled out and put back. A gesture, on demand: nothing polls it. */
  server.on("/api/diag/sd", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("reset_first_data")) SdStream::resetFirstData();
    if (request->hasParam("retry")) SdCard::tryMount();
    if (request->hasParam("wiring")) SdCard::probeWiring();
    /* LISTER un dossier de la SD TEL QUE LE FatFs DE LA CARTE LE VOIT (§208) :
     * lecture seule — le panneau Fichiers ne parcourt que /samples, et un
     * instrument /sfz refusé doit pouvoir se DIAGNOSTIQUER depuis la carte. */
    if (request->hasParam("lister")) {
      request->send(200, "application/json",
                    SfzStore::lister(request->getParam("lister")->value().c_str()));
      return;
    }
    /* The bench hooks BREAK things on purpose: never during a concert (the lock answers 423). */
    if ((request->hasParam("simulate_noise") || request->hasParam("simulate_loss") || request->hasParam("simulate_stale"))
        && Concert::refuse(request, "simulation de panne de la carte SD refusee : la carte est verrouillee pour le concert")) return;
    if ((request->hasParam("measure") || request->hasParam("verify_map"))
        && Concert::refuse(request, "mesure ou verification de la carte SD refusee : la carte est verrouillee pour le concert")) return;
    if (request->hasParam("verify_map")) SdCard::verifyMap(request->getParam("verify_map")->value().c_str());
    if (request->hasParam("simulate_stale")) SdCard::simulateStale();  // bench: every cluster map becomes stale
    if (request->hasParam("simulate_noise"))                           // bench: a noisy bus (blocks read with a bad CRC)
      SdCard::simulateNoise((uint32_t)request->getParam("simulate_noise")->value().toInt());
    if (request->hasParam("simulate_loss")) {                          // bench: what a pulled card looks like
      uint32_t ms = request->hasParam("ms") ? (uint32_t)request->getParam("ms")->value().toInt() : 1500;
      if (ms > 60000) ms = 60000;
      SdCard::simulateLoss(ms);
    }
    if (request->hasParam("measure")) {
      uint32_t hz = request->hasParam("hz") ? (uint32_t)request->getParam("hz")->value().toInt() : 0;
      if (hz && (hz < 400000 || hz > 40000000)) hz = 0;
      SdCard::measure(request->getParam("measure")->value().c_str(), hz);
    }
    request->send(200, "application/json", SdCard::diagnostics());
  });

  /* LES KITS DE LA SD (§208) : tout `.sfz` sous /sfz, avec le poids des WAV a cote (un majorant de
   * ce qu'il prend en PSRAM) — ce que l'inspecteur de l'app propose au choix. Un GESTE (ouvrir
   * le choix), jamais un sondage ; la SD se parcourt « pour le web » (jamais une carte douteuse). */
  server.on("/api/sfz/kits", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", SfzStore::listerKits());
  });

  /* LE MAGASIN SFZ (§208) : les instruments tenus en PSRAM, leur poids, la duree du
   * dernier chargement. `?decharger=1` (banc) : plus aucune cue ne nomme rien — la
   * carte se tait et rend tout ce qu'elle tenait, au silence ; refuse sous le verrou
   * de concert (il decharge ce que la scene attend). Un GESTE, jamais un sondage. */
  server.on("/api/diag/sfz", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("decharger")) {
      if (Concert::refuse(request, "dechargement des instruments SFZ refuse : la carte est verrouillee pour le concert")) return;
      String raison;
      for (uint8_t m = 0; m < SfzStore::MOTEURS; m++) SfzStore::proposer(m, "", raison);   // every engine (§212, §234)
      SfzStore::entretenir("");
    }
    if (request->hasParam("reset_cost")) SfzStore::resetCost();                  // counters back to zero: a trial starts
    if (request->hasParam("cost")) { request->send(200, "application/json", SfzStore::cost()); return; }
    request->send(200, "application/json", SfzStore::diagnostic());
  });

  /* LA LISTE. Un GESTE (ouvrir la colonne, apres un televersement), jamais un sondage —
   * la regle du §161. Storage se lit dans son index (§241), son occupation est celle que
   * l'index remesure au silence ; la SD, elle, se parcourt a chaque appel. */
  server.on("/api/fichiers", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!SampleStore::monter()) { repondre(request, 503, "storage non monte"); return; }
    if (!StorageIndex::waitReady(3000)) {
      repondre(request, 503, "la carte range encore la liste de ses fichiers : quelques secondes apres le demarrage");
      return;
    }
    Ecrit e;
    e.cap = LISTE_MAX;
    e.t = nidmi_tampon_reponse(e.cap);
    Usage* u = (Usage*)heap_caps_malloc(sizeof(Usage) * USAGES_MAX, MALLOC_CAP_SPIRAM);
    if (!e.t || !u) { if (u) heap_caps_free(u); repondre(request, 507, "memoire"); return; }
    const int nu = releverUsages(u);

    const StorageIndex::Stats si = StorageIndex::stats();
    const size_t utilises = si.used, total = si.total;
    const bool charges = SampleStore::charge();
    /* THE SD CARD: declared by the `sd_spi` component (I/O), mounted in its own task.
     * Plugged in afterwards, it is recognised when this list is opened (a new attempt,
     * without waiting here). `utilise` = the size of the sounds listed: a FAT's real
     * occupancy would require reading its whole table. */
    if (SdCard::declared() && !SdCard::mounted()) SdCard::retryIfDue();
    const bool sdMounted = SdCard::mounted();
    uint64_t sdUsed = 0;
    if (sdMounted) walkSdCard([&](const String&, size_t octets) { sdUsed += octets; });
    e.formater("{\"volumes\":[{\"id\":\"%s\",\"nom\":\"Mémoire interne\",\"type\":\"littlefs\","
               "\"prise_en_charge\":true,\"presente\":true,\"total\":%u,\"utilise\":%u},"
               "{\"id\":\"%s\",\"nom\":\"Carte SD\",\"type\":\"sd\","
               "\"prise_en_charge\":true,\"declared\":%s,\"presente\":%s,\"total\":%llu,\"utilise\":%llu}],",
               VOLUME_INTERNE, (unsigned)total, (unsigned)utilises,
               SdCard::VOLUME_ID, SdCard::declared() ? "true" : "false", sdMounted ? "true" : "false",
               (unsigned long long)SdCard::capacityBytes(), (unsigned long long)sdUsed);
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
    if (sdMounted) walkSdCard([&](const String& chemin, size_t octets) {
      const String nom = baseDe(chemin);
      if (!premier) e.ajouter(",");
      premier = false;
      n++;
      e.ajouter("{\"chemin\":"); e.chaine(chemin.c_str());
      e.formater(",\"volume\":\"%s\",\"octets\":%u,\"genre\":\"son\",\"supprimable\":false,\"en_attente\":false",
                 SdCard::VOLUME_ID, (unsigned)octets);
      if (charges)
        e.formater(",\"lisible\":%s", SampleStore::indexDe(nom.c_str()) >= 0 ? "true" : "false");
      { const int fi = SampleStore::indexDe(nom.c_str());
        if (fi >= 0 && SampleStore::isStreamed((uint8_t)fi)) e.ajouter(",\"streamed\":true"); }   // read as a stream, never in PSRAM
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
    if (volumeSd(request)) {
      /* A kit's file, for the SFZ editor (§248): the sd-card task reads it into PSRAM, then
       * /api/sd/read serves it — 202 here, /api/sd/job says when. The bus is the streams' in a
       * concert: refused then. */
      String raison;
      if (!cheminKit(chemin, raison)) { repondre(request, 400, raison); return; }
      if (Concert::refuse(request, "lecture de la SD refusee : la carte est verrouillee pour le concert")) return;
      uint32_t job = 0;
      if (!SdCard::requestRead(chemin.c_str(), job, raison)) { repondre(request, 409, raison); return; }
      request->send(202, "application/json", "{\"status\":\"pending\",\"job\":" + String((unsigned long)job)
                    + ",\"chemin\":\"" + chemin + "\"}");
      return;
    }
    if (!volumeInterne(request)) { repondre(request, 501, "volume inconnu"); return; }
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
      if (volumeSd(request)) {
        /* A kit's file (§248): the body is in PSRAM, the sd-card task writes it. */
        uint8_t* corps = (uint8_t*)request->_tempObject;
        const size_t n = request->contentLength();
        String bas = nom; bas.toLowerCase();
        const size_t max = bas.endsWith(".wav") ? SD_WAV_MAX : SD_SFZ_MAX;
        if (!cheminKit(nom, raison)) { repondre(request, 400, raison); return; }
        if (!n || n > max) { repondre(request, 413, "fichier vide ou trop gros (" + String((unsigned)(max / 1024)) + " Ko au plus)"); return; }
        if (!corps) { repondre(request, 507, "memoire : la carte n'a pas pu garder le fichier"); return; }
        if (bas.endsWith(".wav") && !wavJouable(corps, n, raison)) { repondre(request, 422, raison); return; }
        request->_tempObject = nullptr;           // the card takes it (freed after the write)
        uint32_t job = 0;
        if (!SdCard::requestWrite(nom.c_str(), corps, n, job, raison)) { repondre(request, 409, raison); return; }
        request->send(202, "application/json", "{\"status\":\"pending\",\"job\":" + String((unsigned long)job)
                      + ",\"chemin\":\"" + nom + "\"}");
        return;
      }
      if (!volumeInterne(request)) { repondre(request, 501, "volume inconnu"); return; }
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
      if (volumeSd(request)) {
        String bas = nom; bas.toLowerCase();
        const size_t max = bas.endsWith(".wav") ? SD_WAV_MAX : SD_SFZ_MAX;
        if (index == 0 && total && total <= max
            && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > total
            && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > total + SD_PSRAM_MARGIN)
          request->_tempObject = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (request->_tempObject && index + len <= total)
          memcpy((uint8_t*)request->_tempObject + index, data, len);
        return;                                    // checked at the end, with the reason
      }
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
    if (!volumeInterne(request)) { repondre(request, 501, "rien ne se supprime sur la SD par la carte"); return; }
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
        size_t taille;
        const bool existe = attend ? !sup : StorageIndex::ready() ? StorageIndex::fileSize(chemin.c_str(), taille)
                                                                  : LittleFS.exists(chemin);
        ok = existe && Differe::supprimerFichier(chemin.c_str());
        break;
      }
    }
    if (!ok) { repondre(request, 404, "introuvable"); return; }
    request->send(200, "application/json", "{\"status\":\"ok\",\"chemin\":\"" + chemin + "\"}");
  });
}
