#include "APICommon.h"
#include "../config/Concert.h"
#include "../server/ServerCore.h"
#include "../mapping/CompoStore.h"
#include "../server/WebDebugConsole.h"   // NIDMI_WEB_LOG : la ligne de /api/diag/journal (§200)

/*
 * API du CONCERT et des CLIENTS — le verrou, et qui est connecte
 * (MESURES §197, §199).
 *
 *   GET  /api/verrou              l'etat : verrouillee, par, depuis_s
 *   POST /api/verrou  etat=on|off [par=…]
 *   GET  /api/clients             les onglets connectes, et ce qu'ils disent
 *   POST /api/diag/journal  texte=…   une ligne dans la console (MESURES §200)
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

    /* UNE LIGNE DANS LA CONSOLE (MESURES §200). La console ne va plus qu'aux
     * onglets qui s'y abonnent : pour l'eprouver, il faut une ligne qu'on
     * provoque. La seule route qui en ecrivait une, /api/pins/read, lit un ADC —
     * sur une broche du bus audio, c'est l'I2S qui s'arrete (§45). Celle-ci n'a
     * aucun autre effet : la ligne va au journal comme les autres (historique,
     * port serie, memoire RTC). 120 caracteres au plus. */
    server.on("/api/diag/journal", HTTP_POST, [](AsyncWebServerRequest *request){
        String t = request->hasParam("texte", true) ? request->getParam("texte", true)->value() : String("");
        if (!t.length()) {
            request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"texte= manquant\"}");
            return;
        }
        if (t.length() > 120) t = t.substring(0, 120);
        NIDMI_WEB_LOG("[journal] %s", t.c_str());
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
}
