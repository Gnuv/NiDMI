// src/api/InterfaceAPI.cpp — L'INTERFACE, EN UN DOCUMENT (CONVERGENCE §9.7,
// MESURES §190).
//
//   GET  /api/interface[?secrets=1]   le document : tous les reglages, leur type
//                                     — le mot de passe WiFi seulement par le
//                                     cable, et sur demande (§159)
//   POST /api/interface?nom=…         le REMPLACER (corps : « espace|cle|type|
//                                     valeur » par ligne) ; la carte redemarre
//   GET/POST /api/interface/nom       son nom (nom=…), pour s'y retrouver
//
// C'est la CARTE qui connait ses reglages : elle les parcourt (src/config/
// Interface), l'app n'en tient aucune liste. Recharger une interface est un
// geste de PREPARATION : la NVS s'ecrit tout de suite, annoncee au moteur, et la
// carte redemarre pour tout prendre — broches, reseau, transports.
#include "APICommon.h"
#include "../config/Interface.h"
#include "../mapping/Repertoire.h"          // jsonDe
#include "../server/ServerCallbacks.h"      // nidmi_requestReboot
#include "../network/UsbNetBootstrap.h"     // parLeCable
#include "../utils/PinMapper.h"

namespace {

constexpr size_t DOCUMENT_MAX = 64 * 1024;   // la NVS entiere n'en fait que 20

void refuser(AsyncWebServerRequest* r, int code, String raison) {
  raison.replace("\"", "'");
  r->send(code, "application/json", "{\"status\":\"error\",\"message\":\"" + raison + "\"}");
}

struct Visite { Ecrit* e; bool premier; };

}  // namespace

void setupInterfaceAPI(AsyncWebServer& server) {

  // AVANT « /api/interface » : le serveur fait correspondre par prefixe.
  server.on("/api/interface/nom", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"nom\":" + Repertoire::jsonDe(Interface::nom()) + "}");
  });
  server.on("/api/interface/nom", HTTP_POST, [](AsyncWebServerRequest* request) {
    const String nom = request->hasParam("nom", true) ? request->getParam("nom", true)->value() : String();
    String raison;
    if (!Interface::renommer(nom, raison)) { refuser(request, 409, raison); return; }
    request->send(200, "application/json",
                  "{\"status\":\"ok\",\"nom\":" + Repertoire::jsonDe(Interface::nom()) + "}");
  });

  server.on("/api/interface", HTTP_GET, [](AsyncWebServerRequest* request) {
    const bool demande = request->hasParam("secrets") && request->getParam("secrets")->value() == "1";
    const bool cable = nidmi_usbnet::parLeCable(request->client()->localIP(), request->client()->remoteIP());
    Ecrit e;
    e.cap = DOCUMENT_MAX;
    e.t = nidmi_tampon_reponse(e.cap);
    if (!e.t) { refuser(request, 507, "memoire"); return; }
    PinMapper::detectMcu();
    String mcu = PinMapper::getMcuName();
    mcu.toLowerCase();
    e.ajouter("{\"format\":\"nidmi-interface\",\"version\":1,\"nom\":");
    e.ajouter(Repertoire::jsonDe(Interface::nom()).c_str());
    e.ajouter(",\"carte\":"); e.chaine(mcu.c_str());
    e.ajouter(",\"reglages\":[");
    Visite v{ &e, true };
    String tus;
    const bool lu = Interface::visiter(demande && cable, [](const char* espace, const char* cle, const char* type,
                                                            const char* valeur, void* ctx) {
      Visite& x = *(Visite*)ctx;
      Ecrit& o = *x.e;
      if (!x.premier) o.ajouter(",");
      x.premier = false;
      o.ajouter("{\"espace\":"); o.chaine(espace);
      o.ajouter(",\"cle\":");    o.chaine(cle);
      o.ajouter(",\"type\":");   o.chaine(type);
      o.ajouter(",\"valeur\":");
      /* Un nombre en nombre — sauf sur 64 bits, qu'un nombre JSON ne tient pas
       * exactement : en texte. Un texte en texte, des octets en hexadecimal. */
      const bool nombre = strcmp(type, "str") && strcmp(type, "blob") && strcmp(type, "u64") && strcmp(type, "i64");
      if (nombre) o.ajouter(valeur); else o.chaine(valeur);
      o.ajouter("}");
    }, &v, tus);
    e.ajouter("],\"tus\":[");
    if (tus.length()) { e.ajouter("\""); e.ajouter(tus.c_str()); e.ajouter("\""); }
    e.ajouter("]}");
    if (!lu) { refuser(request, 500, "la NVS ne se lit pas entiere"); return; }
    if (e.deborde) { refuser(request, 507, "interface trop grande pour sa reponse"); return; }
    request->send(nidmi_reponse_tampon(request, "application/json", e.t, e.n));
  });

  server.on("/api/interface", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      const size_t n = request->contentLength();
      const char* t = (const char*)request->_tempObject;
      if (!n || n > DOCUMENT_MAX || !t) {
        refuser(request, n > DOCUMENT_MAX ? 413 : 400, n ? "document trop grand" : "document vide");
        return;
      }
      const String nom = request->hasParam("nom") ? request->getParam("nom")->value() : String();
      String raison;
      int poses = 0, retires = 0;
      if (!Interface::appliquer(t, n, nom, raison, poses, retires)) { refuser(request, 409, raison); return; }
      request->send(200, "application/json",
        "{\"status\":\"ok\",\"poses\":" + String(poses) + ",\"retires\":" + String(retires)
        + ",\"redemarrage\":true,\"message\":\"la carte redemarre pour prendre son interface\"}");
      nidmi_requestReboot(("interface · " + request->client()->remoteIP().toString()).c_str());
    },
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        if (!total || total > DOCUMENT_MAX) return;       // refuse a la fin
        request->_tempObject = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
      if (request->_tempObject && index + len <= total)
        memcpy((char*)request->_tempObject + index, data, len);
    });
}
