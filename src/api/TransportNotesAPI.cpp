#include "APICommon.h"
#include "../midi/TransportNotes.h"

/*
 * THE TRANSPORT NOTES — a pedal or a pad that drives the cues (MESURES §215). Settings →
 * Transport in the app; the card holds them (src/midi/TransportNotes).
 *
 *   GET  /api/transport/notes                        {"playstop":{"ch":10,"note":35}|null, …,
 *                                                     "learning":"next"|null}
 *   POST /api/transport/notes  action=…&ch=…&note=…  bind (ch 0 = any channel)
 *   POST /api/transport/notes  action=…&clear=1      unbind
 *   POST /api/transport/learn  action=…              the next note-on the card receives
 *   POST /api/transport/learn  cancel=1              stop learning
 *
 * Actions: playstop, next, prev, first. Each change is announced: NIDMI_TRANSPORT:<json>.
 * Preparation, not play: refused under the concert lock (they are not in GESTES).
 */
namespace {
void refuse(AsyncWebServerRequest* r, int code, const String& why) {
    r->send(code, "application/json", "{\"status\":\"error\",\"message\":\"" + nidmi_json_chaine(why) + "\"}");
}
String field(AsyncWebServerRequest* r, const char* name) {
    return r->hasParam(name, true) ? r->getParam(name, true)->value() : String();
}
}  // namespace

void setupTransportNotesAPI(AsyncWebServer& server) {
    server.on("/api/transport/notes", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", TransportNotes::json());
    });

    server.on("/api/transport/notes", HTTP_POST, [](AsyncWebServerRequest* request) {
        const TransportNotes::Action a = TransportNotes::fromName(field(request, "action").c_str());
        if (a >= TransportNotes::COUNT) { refuse(request, 400, "action : playstop, next, prev ou first"); return; }
        if (field(request, "clear") == "1") {
            TransportNotes::clear(a);
        } else {
            const String ch = field(request, "ch"), note = field(request, "note");
            if (!note.length()) { refuse(request, 400, "note manquante (ou clear=1)"); return; }
            String why;
            if (!TransportNotes::set(a, ch.length() ? ch.toInt() : 0, note.toInt(), why)) {
                refuse(request, 400, why);
                return;
            }
        }
        request->send(200, "application/json", TransportNotes::json());
    });

    server.on("/api/transport/learn", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (field(request, "cancel") == "1") {
            TransportNotes::cancelLearn();
        } else {
            const TransportNotes::Action a = TransportNotes::fromName(field(request, "action").c_str());
            if (a >= TransportNotes::COUNT) { refuse(request, 400, "action : playstop, next, prev ou first"); return; }
            TransportNotes::learn(a);
        }
        request->send(200, "application/json", TransportNotes::json());
    });
}
