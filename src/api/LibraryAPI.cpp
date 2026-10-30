#include "APICommon.h"
#include "../config/Library.h"
#include <esp_heap_caps.h>

/*
 * THE LIBRARY: script models and presets, kept by the card (src/config/Library, MESURES §249).
 *
 *   GET    /api/library/list                                  {"scripts":[…],"presets":[…]}
 *   GET    /api/library/file?kind=script&name=x.nms           the text
 *   GET    /api/library/file?kind=preset&engine=e&name=n      the JSON
 *   POST   /api/library/file?kind=…&name=…[&engine=…]         raw body: taken at once, written at silence
 *   DELETE /api/library/file?kind=…&name=…[&engine=…]
 *
 * The body is raw and lands in PSRAM: a form-encoded body is split into parameters in internal
 * RAM (§248). Preparation, not play: refused under the concert lock (not in GESTES).
 */
namespace {
void refuse(AsyncWebServerRequest* r, int code, const String& why) {
    r->send(code, "application/json", "{\"status\":\"error\",\"message\":" + nidmi_json_chaine(why) + "}");
}
String query(AsyncWebServerRequest* r, const char* name) {
    return r->hasParam(name) ? r->getParam(name)->value() : String();
}
/* The entry the request names; "" and the reason when it names none. */
String pathOf(AsyncWebServerRequest* r, String& why) {
    const String kind = query(r, "kind");
    if (kind == "script") return Library::scriptPath(query(r, "name"), why);
    if (kind == "preset") return Library::presetPath(query(r, "engine"), query(r, "name"), why);
    why = "kind : script ou preset";
    return String();
}
}  // namespace

void setupLibraryAPI(AsyncWebServer& server) {
    server.on("/api/library/list", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", Library::listJson());
    });

    server.on("/api/library/file", HTTP_GET, [](AsyncWebServerRequest* request) {
        String why;
        const String path = pathOf(request, why);
        if (!path.length()) { refuse(request, 400, why); return; }
        String text;
        if (!Library::read(path, text)) { refuse(request, 404, "absent de la bibliotheque de la carte"); return; }
        request->send(200, path.endsWith(".json") ? "application/json" : "text/plain; charset=utf-8", text);
    });

    server.on("/api/library/file", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            String why;
            const String path = pathOf(request, why);
            if (!path.length()) { refuse(request, 400, why); return; }
            const size_t n = request->contentLength();
            if (n > Library::FILE_MAX) { refuse(request, 413, "trop gros (8 Ko au plus)"); return; }
            const char* body = (const char*)request->_tempObject;
            if (n && !body) { refuse(request, 507, "memoire"); return; }
            if (!Library::write(path, body ? body : "", n, why)) { refuse(request, 507, why); return; }
            request->send(200, "application/json", "{\"status\":\"ok\",\"path\":" + nidmi_json_chaine(path) + "}");
        },
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0 && total && total <= Library::FILE_MAX)
                request->_tempObject = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (request->_tempObject && index + len <= total)
                memcpy((char*)request->_tempObject + index, data, len);
        });

    server.on("/api/library/file", HTTP_DELETE, [](AsyncWebServerRequest* request) {
        String why;
        const String path = pathOf(request, why);
        if (!path.length()) { refuse(request, 400, why); return; }
        if (!Library::remove(path)) { refuse(request, 404, "absent de la bibliotheque de la carte"); return; }
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
}
