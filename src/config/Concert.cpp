#include "Concert.h"
#include "EcrituresDifferees.h"
#include "../server/ServerCore.h"        // nidmi_ws_pousser : les onglets apprennent le verrou
#include "../server/WebDebugConsole.h"   // NIDMI_WEB_LOG
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Concert {
namespace {

constexpr const char* NVS_ESPACE = "nidmi-etat";   // hors de l'interface (voir Concert.h)
constexpr const char* NVS_CLE    = "verrou";

volatile bool s_verrouille = false;
uint32_t      s_depuis     = 0;     // millis() de la derniere bascule ; 0 : relu de la memoire
char          s_par[48]    = "";    // qui l'a pose ; vide : relu de la memoire

/* Le texte de « qui » est lu par le serveur web et ecrit par la tache qui pose
 * le verrou : un verrou court, comme les autres etats partages. */
StaticSemaphore_t s_tampon;
SemaphoreHandle_t s_verrouTexte = xSemaphoreCreateMutexStatic(&s_tampon);
struct VerrouTexte {
  VerrouTexte()  { xSemaphoreTake(s_verrouTexte, portMAX_DELAY); }
  ~VerrouTexte() { xSemaphoreGive(s_verrouTexte); }
};

/* LES GESTES DE JEU : ce qui reste permis verrouille. Chemin EXACT, methode POST.
 * Une route qui n'est pas ici est refusee — c'est la liste des permis qui
 * decide, jamais celle des interdits. */
const char* const GESTES[] = {
  // le transport
  "/api/cues/play", "/api/cues/pause", "/api/cues/stop",
  "/api/cues/go", "/api/cues/goto", "/api/cues/prev",
  // le son : un bip, les paramètres continus (le moteur est refuse dans la
  // route), le silence, la porte, un echantillon, un gain, un clip
  "/api/audio/test", "/api/audio/params", "/api/audio/stop", "/api/audio/resume",
  "/api/audio/sampler/jouer", "/api/audio/sampler/gain", "/api/audio/liste/jouer",
  // les reglages d'un script (son code est refuse dans la route), et un CC
  // injecte dans la chaine, comme une entree
  "/api/midi/script", "/api/midi/chaine",
  // changer de piece : un geste de scene, comme le bouton sys.compo
  "/api/compositions/ouvrir",
  // et le verrou lui-meme : sans lui, on ne le leverait jamais
  "/api/verrou",
};

bool estUnGeste(const String& url) {
  for (const char* g : GESTES) if (url == g) return true;
  return false;
}

void refuser(AsyncWebServerRequest* r, const char* pourquoi) {
  r->send(423, "application/json",
          String("{\"status\":\"error\",\"message\":\"") + pourquoi + "\",\"verrouillee\":true}");
}

/* La garde. `canHandle` est interroge a l'arrivee de la requete — avant son
 * corps, avant son televersement : un POST refuse n'atteint jamais le
 * gestionnaire qui aurait ecrit en flash au fil des octets. */
class Garde : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest* r) const override {
    if (!s_verrouille) return false;
    const uint8_t m = r->method();
    if (m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS) return false;   // lire ne change rien
    if (m == HTTP_POST && estUnGeste(r->url())) return false;
    return true;
  }
  void handleRequest(AsyncWebServerRequest* r) override {
    refuser(r, "la carte est verrouillee (concert) : deverrouiller avant de modifier");
  }
  /* handleBody et handleUpload : ceux de la classe de base, qui ne font rien — le
   * corps est lu et jete, rien ne s'ecrit. */
};

void annoncer() {
  char trame[24];
  snprintf(trame, sizeof trame, "NIDMI_VERROU:%d", s_verrouille ? 1 : 0);
  nidmi_ws_pousser(trame);
}

}  // namespace

void demarrer() {
  bool v = false;
  Preferences p;
  if (p.begin(NVS_ESPACE, true)) {            // jamais ecrite : deverrouillee
    v = p.getUChar(NVS_CLE, 0) != 0;
    p.end();
  }
  s_verrouille = v;
  if (v) NIDMI_WEB_LOG("[concert] VERROUILLEE (relue de la memoire) : la structure ne bouge pas");
}

bool verrouille() { return s_verrouille; }

void fixer(bool oui, const String& par) {
  if (s_verrouille == oui) return;            // deja ainsi : « depuis » et « par » restent ceux du geste qui l'a fait
  {
    VerrouTexte v;
    size_t k = 0;
    for (size_t i = 0; i < par.length() && k < sizeof s_par - 1; i++) {
      const char c = par[i];
      s_par[k++] = (c < 0x20 || c == '"' || c == '\\') ? ' ' : c;   // du JSON, sans echappement a faire
    }
    s_par[k] = 0;
    s_depuis = millis();
  }
  s_verrouille = oui;
  /* Memorise au silence, comme tout ce qui peut attendre (§157) : la barre de
   * titre de l'app dit « en attente du silence » tant que ce n'est pas ecrit. */
  Differe::nvsOctet(NVS_ESPACE, NVS_CLE, oui ? 1 : 0);
  NIDMI_WEB_LOG("[concert] %s par %s", oui ? "VERROUILLEE" : "deverrouillee", par.c_str());
  annoncer();
}

String etatJson() {
  char par[sizeof s_par];
  uint32_t depuis;
  { VerrouTexte v; strlcpy(par, s_par, sizeof par); depuis = s_depuis; }
  String j = "{\"verrouillee\":";
  j += s_verrouille ? "true" : "false";
  j += ",\"par\":\"";
  j += par;
  j += "\",\"depuis_s\":";
  j += String(depuis ? (unsigned)((millis() - depuis) / 1000) : 0u);
  j += "}";
  return j;
}

void installerGarde(AsyncWebServer& server) {
  server.addHandler(new Garde());
}

bool refuse(AsyncWebServerRequest* requete, const char* pourquoi) {
  if (!s_verrouille) return false;
  refuser(requete, pourquoi);
  return true;
}

}  // namespace Concert
