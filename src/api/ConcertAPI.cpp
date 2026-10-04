#include "APICommon.h"
#include "../config/Concert.h"
#include "../server/ServerCore.h"
#include "../mapping/CompoStore.h"

/*
 * API du CONCERT et des CLIENTS — le verrou, et qui est connecte
 * (MESURES §197, §199).
 *
 *   GET  /api/verrou              l'etat : verrouillee, par, depuis_s
 *   POST /api/verrou  etat=on|off [par=…]
 *   GET  /api/clients             les onglets connectes, et ce qu'ils disent
 *
 * Le verrou est un geste de jeu pour la garde (Concert.cpp) : sans quoi on ne le
 * leverait jamais. Le lever n'est pas protege — personne n'est authentifie —,
 * il est ECRIT : la carte retient qui l'a demande, et l'annonce a tous.
 *
 * La liste des clients n'est jamais poussee : la carte dit seulement « elle a
 * change » (NIDMI_CLIENTS), et l'app la relit quand elle l'affiche.
 */
void setupConcertAPI(AsyncWebServer& server) {

    server.on("/api/verrou", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", Concert::etatJson());
    });

    server.on("/api/verrou", HTTP_POST, [](AsyncWebServerRequest *request){
        const String etat = request->hasParam("etat", true)
                          ? request->getParam("etat", true)->value() : String("");
        if (etat != "on" && etat != "off") {
            request->send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"etat=on ou etat=off\"}");
            return;
        }
        /* Qui : ce que le client dit de lui (24 caracteres au plus), et d'ou il
         * vient — l'adresse que la carte voit, pas celle qu'on lui annonce. */
        String qui = request->hasParam("par", true) ? request->getParam("par", true)->value() : String("app");
        qui = qui.substring(0, 24);
        qui += " · ";
        qui += request->client()->remoteIP().toString();
        Concert::fixer(etat == "on", qui);
        request->send(200, "application/json", Concert::etatJson());
    });

    server.on("/api/clients", HTTP_GET, [](AsyncWebServerRequest *request){
        char rev[9];
        snprintf(rev, sizeof rev, "%08x", (unsigned)Compo::revision());
        String j = "{\"rev\":\"";
        j += rev;
        j += "\",\"verrouillee\":";
        j += Concert::verrouille() ? "true" : "false";
        j += ",";
        nidmi_ws_clients_ecrire(j);
        j += "}";
        request->send(200, "application/json", j);
    });
}
