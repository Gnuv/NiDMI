#include "../server/ServerCore.h"
#include "ui_index.h"
#include "ui_bundle.h"
#include "app_archive.h"
#include "../utils/PinMapper.h"
#include "../api/APICommon.h"
#include "../api/NetworkAPI.h"
#include "../api/RTPAPI.h"
#include "../api/UsbMidiAPI.h"
#include "../server/WebDebugConsole.h"
#include "../Globals.h"
#include "../server/ServerCallbacks.h"
#include "../components/ComponentRegistry.h"
#include "../processors/TouchProcessor.h"
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <pgmspace.h>

// Forward declarations pour les APIs
void setupPinAPI(AsyncWebServer& server);
void setupOSC_API(AsyncWebServer& server);
void setupCacheAPI(AsyncWebServer& server);
void setupComponentsAPI(AsyncWebServer& server);
void setupSystemAPI(AsyncWebServer& server);
void setupOtaAPI(AsyncWebServer& server);
void setupAudioAPI(AsyncWebServer& server);

Preferences preferences;

volatile bool g_pinMonitoringEnabled = false;

// Fonction pour obtenir la configuration par défaut d'une pin
String getDefaultConfig(String pin) {
    // Pins analogiques (A0, A1, A2, ... A10) - dynamique selon le MCU
    if (pin.startsWith("A")) {
        PinMapper::detectMcu();
        uint8_t gpio = PinMapper::labelToGpio(pin.c_str());
        if (gpio != 255 && PinMapper::hasAdc(gpio)) {
            // Sur ESP32-S3, si la pin supporte le touch, proposer par défaut un Touch avec note incrémentée
            #if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S3)
            if (PinMapper::hasTouch(gpio)) {
                int pinNum = pin.substring(1).toInt();
                int noteVal = 60 + pinNum; // A0 -> 60, A1 -> 61, etc.
                if (noteVal > 127) noteVal = 127;
                String config = "{\"role\":\"touch\",\"rtpMidiEnabled\":true,";
                config += "\"midiMessageType\":\"Note + Key Pressure\",";
                config += "\"midiNote\":" + String(noteVal) + ",\"midiChannel\":1,";
                // OSC en mode raw par défaut pour faciliter le calibrage
                config += "\"oscEnabled\":true,\"oscAddress\":\"/touch\",\"oscFormat\":\"raw\",";
                config += "\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
                return config;
            }
            #endif

            // Sinon, comportement historique : potentiomètre CC avec CC basé sur l’index de pin
            int pinNum = pin.substring(1).toInt();
            int default_cc = pinNum + 1; // A0 -> CC1, A1 -> CC2, etc.
            if (default_cc > 127) default_cc = 127;

            String config = "{\"role\":\"potentiometer\",\"rtpMidiEnabled\":true,\"midiMessageType\":\"Control Change\",";
            config += "\"midiCc\":" + String(default_cc) + ",\"midiChannel\":1,";
            config += "\"filterIntensity\":5,\"oscEnabled\":true,\"oscAddress\":\"/ctl\",";
            config += "\"oscFormat\":\"float\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
            return config;
        }
    }
    
    // Pins MUX (M0_0 à M1_15) - traiter comme potentiomètres
    if (pin.startsWith("M")) {
        // Extraire le numéro de mux et canal : M0_0 -> mux=0, ch=0
        int underscore_pos = pin.indexOf('_');
        if (underscore_pos > 0 && underscore_pos < pin.length() - 1) {
            int mux_id = pin.substring(1, underscore_pos).toInt();
            int channel = pin.substring(underscore_pos + 1).toInt();
            // CC par défaut : 1 + (mux_id * 16) + channel
            int default_cc = 1 + (mux_id * 16) + channel;
            if (default_cc > 127) default_cc = 127; // Limiter à 127
            
            String config = "{\"role\":\"potentiometer\",\"rtpMidiEnabled\":true,\"midiMessageType\":\"Control Change\",";
            config += "\"midiCc\":" + String(default_cc) + ",\"midiChannel\":1,";
            config += "\"filterIntensity\":5,\"oscEnabled\":true,\"oscAddress\":\"/ctl\",";
            config += "\"oscFormat\":\"float\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
            return config;
        }
    }
    
    // Exemples dynamiques basés sur les composants disponibles
    // Chercher les composants pour générer des exemples
    const ComponentDefinition* buttonDef = ComponentRegistry::findById("button");
    const ComponentDefinition* ledDef = ComponentRegistry::findById("led");
    
    // Générer des exemples pour D0-D3 avec button
    if (buttonDef && (pin == "D0" || pin == "D1" || pin == "D2" || pin == "D3")) {
        int pinNum = pin.charAt(1) - '0';
        int noteVal = 60 + pinNum;
        String example = "{\"role\":\"button\",\"rtpMidiEnabled\":true,\"midiMessageType\":\"Note\",\"midiNote\":" + String(noteVal) + ",\"midiChannel\":1";
        if (buttonDef->formFieldCount > 0) {
            for (uint8_t i = 0; i < buttonDef->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                const FormFieldDef& field = buttonDef->formFields[i];
                if (field.id && field.defaultValue) {
                    example += ",\"" + String(field.id) + "\":\"" + String(field.defaultValue) + "\"";
                }
            }
        }
        example += ",\"oscEnabled\":true,\"oscAddress\":\"/note\",\"oscFormat\":\"float\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
        return example;
    }
    
    // Générer des exemples pour D7-D10 avec LED
    if (ledDef && (pin == "D7" || pin == "D8" || pin == "D9" || pin == "D10")) {
        int noteVal = 36;
        if (pin == "D8") noteVal = 37;
        else if (pin == "D9") noteVal = 38;
        
        String rtpType = "Note";
        String rtpParam = "\"midiNote\":" + String(noteVal);
        
        // Pour D10, utiliser CC
        if (pin == "D10") {
            rtpType = "Control Change";
            rtpParam = "\"midiCc\":10";
            // Chercher le message CC
            for (uint8_t i = 0; i < ledDef->midiMessageCount && i < MAX_MIDI_MESSAGES; i++) {
                if (ledDef->midiMessages[i].id && strcmp(ledDef->midiMessages[i].id, "cc") == 0) {
                    rtpType = ledDef->midiMessages[i].displayName ? ledDef->midiMessages[i].displayName : "Control Change";
                    break;
                }
            }
        }
        
        String example = "{\"role\":\"led\",\"rtpMidiEnabled\":true,\"midiMessageType\":\"" + rtpType + "\"," + rtpParam + ",\"midiChannel\":1";
        
        // Ajouter les formFields par défaut
        if (ledDef->formFieldCount > 0) {
            for (uint8_t i = 0; i < ledDef->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                const FormFieldDef& field = ledDef->formFields[i];
                if (field.id) {
                    if (pin == "D10" && strcmp(field.id, "ledMode") == 0 && field.options) {
                        // Pour D10, utiliser "pwm" si disponible
                        String options = field.options;
                        if (options.indexOf("pwm") >= 0) {
                            example += ",\"ledMode\":\"pwm\"";
                        } else if (field.defaultValue) {
                            example += ",\"ledMode\":\"" + String(field.defaultValue) + "\"";
                        }
                    } else if (field.defaultValue) {
                        example += ",\"" + String(field.id) + "\":\"" + String(field.defaultValue) + "\"";
                    }
                }
            }
        }
        
        String oscAddr = (pin == "D10") ? "/ctl" : "/note";
        example += ",\"oscEnabled\":true,\"oscAddress\":\"" + oscAddr + "\",\"oscFormat\":\"float\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
        return example;
    }
    
    // Bus
    if (pin == "SDA" || pin == "SCL" || pin == "I2C") return "{\"role\":\"I2C\",\"rtpMidiEnabled\":false,\"oscEnabled\":true,\"oscAddress\":\"/ctl\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
    if (pin == "MOSI" || pin == "MISO" || pin == "SCK" || pin == "SPI") return "{\"role\":\"SPI\",\"rtpMidiEnabled\":false,\"oscEnabled\":true,\"oscAddress\":\"/ctl\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
    if (pin == "TX" || pin == "RX") return "{\"role\":\"UART\",\"rtpMidiEnabled\":false,\"oscEnabled\":true,\"oscAddress\":\"/ctl\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
    
    // Défaut
    return "{\"role\":\"button\",\"rtpMidiEnabled\":true,\"midiMessageType\":\"Note\",\"midiNote\":60,\"midiChannel\":1,\"btnMode\":\"pulse\",\"btnPulseTiming\":\"release\",\"oscEnabled\":true,\"oscAddress\":\"/note\",\"dbgEnabled\":false,\"dbgHeader\":\"\"}";
}

#include "../audio/AudioEngine.h"

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.println("WebSocket client connected");
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.println("WebSocket client disconnected");
        g_pinMonitoringEnabled = false;
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (!info || info->opcode != WS_TEXT) return;
        // Frame texte complète (1er fragment) — ne pas exiger info->len==len (certaines stacks divergent)
        if (!(info->final && info->index == 0)) return;
        if (!data || len == 0) return;

        String message;
        message.reserve(len);
        for (size_t i = 0; i < len; i++) {
            message += (char)data[i];
        }

#if NIDMI_WEB_DEBUG_CONSOLE
        if (message.startsWith("DEBUG_CONSOLE:")) {
            nidmi_web_debug_handle_ws_text(client, message);
            return;
        }
#endif

        /* Notes jouées depuis l'interface — clavier à l'écran ou MIDI entrant.
         *
         * Pourquoi le WebSocket et pas une route HTTP : une requête par note,
         * c'est exactement la charge concurrente qui creuse le tas de la carte
         * (plancher mesuré à 1 348 o, hardware/bench/MESURES.md §10), et la
         * latence relevée allait de 0,27 à 2,4 s sous charge — injouable. Ici,
         * une seule connexion, aucune allocation par note.
         *
         * Format, aligné sur les préfixes déjà en place :
         *   NOTE_ON:<note>,<velocite>     NOTE_OFF:<note>
         * Volontairement sans accusé de réception : une note perdue vaut mieux
         * qu'un aller-retour sur le chemin temps réel. */
        if (message.startsWith("NOTE_ON:")) {
            const String corps = message.substring(8);
            const int v = corps.indexOf(',');
            const int note = (v > 0 ? corps.substring(0, v) : corps).toInt();
            const int velo = (v > 0 ? corps.substring(v + 1).toInt() : 100);
            if (note >= 0 && note <= 127) {
                AudioEngine::noteOn((uint8_t)note, (uint8_t)constrain(velo, 1, 127));
            }
            return;
        }
        if (message.startsWith("NOTE_OFF:")) {
            const int note = message.substring(9).toInt();
            if (note >= 0 && note <= 127) AudioEngine::noteOff((uint8_t)note);
            return;
        }

        // Activation/désactivation runtime-only du monitoring SVG
        if (message.startsWith("PIN_MONITORING:")) {
            String val = message.substring(15);
            g_pinMonitoringEnabled = (val == "1");
            if (client) client->text(String("PIN_MONITORING_STATE:") + (g_pinMonitoringEnabled ? "1" : "0"));
            return;
        }


        // Commande globale de calibration touch (toutes les baselines)
        if (message == "TOUCH_CALIBRATE_ALL") {
            TouchProcessor::resetAllBaselines();
            client->text("TOUCH_CALIBRATE_DONE");
            return;
        }
        
        if (message.startsWith("PIN_CLICKED:")) {
            String pin = message.substring(12);
            
            // Vérifier NVS (compatible avec système existant)
            preferences.begin("nidmi", true);
            String key = "pin_" + pin;
            String config = preferences.getString(key.c_str(), "");
            
            // Pour les pins de bus, chercher aussi sous le label du bus
            if (config.length() == 0) {
                if (pin == "MOSI" || pin == "MISO" || pin == "SCK") {
                    config = preferences.getString("pin_SPI", "");
                    if (config.length() > 0) pin = "SPI";
                } else if (pin == "SDA" || pin == "SCL") {
                    config = preferences.getString("pin_I2C", "");
                    if (config.length() > 0) pin = "I2C";
                }
            }
            preferences.end();
            
            if (config.length() > 0) {
                // Config trouvée → Envoyer config NVS
                String msg = "PIN_CONFIG:" + pin + ":" + config;
                client->text(msg);
            } else {
                // Pas de config → Envoyer valeurs par défaut complètes
                String defaultConfig = getDefaultConfig(pin);
                String msg = "PIN_CONFIG:" + pin + ":" + defaultConfig;
                client->text(msg);
            }
        }
    }
}

// Fonction pour envoyer le statut RTP-MIDI via WebSocket
void sendRtpStatus(AsyncWebSocket& ws) {
    preferences.begin("nidmi", false);
    bool enabled = preferences.getBool("rtp_enabled", false);
    String name = preferences.getString("rtp_name", "ESP32-Studio");
    String target = preferences.getString("rtp_target", "sta");
    preferences.end();
    
    bool connected = serverCore.rtpMidi().isConnected();
    
    String json = "{";
    json += "\"type\":\"rtp_status\",";
    json += "\"enabled\":" + String(enabled ? "true" : "false") + ",";
    json += "\"name\":\"" + name + "\",";
    json += "\"target\":\"" + target + "\",";
    json += "\"connected\":" + String(connected ? "true" : "false");
    json += "}";
    
    ws.textAll(json);
}

/* Sert un fichier de l'application embarquée (app_archive.h), en streaming
   par chunks depuis PROGMEM — le même patron que l'index historique. Un
   ETag faible (index+taille) épargne le re-téléchargement complet à chaque
   rechargement : ~400 ko de WiFi économisés tant que l'archive ne change pas. */
/* validerAuBout : poser la preuve de vie du garde-fou de boot QUAND LE DERNIER
   OCTET DU CORPS a été remis à la pile TCP — pas à l'arrivée de la requête.
   La distinction n'est pas théorique : mesuré le 2026-09-03, une carte avec
   Plaits résident ACCEPTE la connexion et parse la requête, puis n'arrive
   jamais à écouler les 17 933 o (60 s, zéro octet reçu). Valider à l'entrée du
   gestionnaire déclarait donc saine une config qui ne sert rien — le garde-fou
   ne protégeait de rien. */
static void _sertArchiveApp(AsyncWebServerRequest *request, const String& chemin,
                            bool validerAuBout = false){
    const AppFile* f = nullptr;
    size_t idx = 0;
    for (; idx < APP_FILES_COUNT; idx++) {
        if (chemin.equals(APP_FILES[idx].chemin)) { f = &APP_FILES[idx]; break; }
    }
    if (!f) {
        request->send(404, "text/plain; charset=utf-8", "Not found: " + chemin);
        return;
    }
    String etag = "\"" + String(idx) + "-" + String(f->taille) + "\"";
    if (request->header("If-None-Match") == etag) {
        AsyncWebServerResponse *rep = request->beginResponse(304);
        rep->addHeader("ETag", etag);
        rep->addHeader("Cache-Control", "no-cache");
        request->send(rep);
        // Un 304 est une réponse servie : le navigateur a l'app en cache et la
        // carte a tenu son bout. Ça compte comme preuve de vie.
        if (validerAuBout) AudioEngine::validerConfigBoot();
        return;
    }
    const uint8_t* donnees = f->donnees;
    const size_t taille = f->taille;
    AsyncWebServerResponse *rep = request->beginResponse(f->type, taille,
        [donnees, taille, validerAuBout](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
            size_t aEcrire = (taille - index < maxLen) ? (taille - index) : maxLen;
            if (aEcrire > 0) memcpy_P(buffer, donnees + index, aEcrire);
            // Dernier morceau du corps : la réponse est entièrement écoulée.
            // validerConfigBoot() ne fait que lever un drapeau — on est dans
            // async_tcp, on n'y écrit pas la flash (l'écriture a lieu dans
            // nidmi_loop(), via entretienBoot()).
            if (validerAuBout && aEcrire > 0 && index + aEcrire >= taille)
                AudioEngine::validerConfigBoot();
            return aEcrire;
        });
    if (f->gz) rep->addHeader("Content-Encoding", "gzip");
    rep->addHeader("Cache-Control", "no-cache");
    rep->addHeader("ETag", etag);
    request->send(rep);
}

void setupWebAPI(AsyncWebServer& server, AsyncWebSocket& ws) {
    /* ── L'application NiDMI (nidmi.html + css/ + js/), embarquée ────────────
       GÉNÉRÉE par scripts/cartes/embarquer-app.py (dépôt nidmi) : fichiers
       gzippés en PROGMEM, servis tels quels (Content-Encoding: gzip). C'est
       l'étape 1c de CONVERGENCE_NIDMI.md §10 — une seule origine sert l'app
       ET l'API, donc plus de question CORS. Le streaming par chunks évite
       toute copie heap : le plus gros fichier ne coûte que son tampon. */
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        /* Preuve de vie du garde-fou de boot : servir l'interface est
           exactement ce que le chargement d'un process peut empêcher (il prend
           le dernier gros bloc contigu et AsyncTCP n'a plus de tampons). Y
           arriver déclare donc la config du boot saine. Ne fait que poser un
           drapeau — l'écriture NVS a lieu dans nidmi_loop(). */
        _sertArchiveApp(request, "/nidmi.html", /*validerAuBout=*/true);
    });

    /* Tout chemin de l'app qui n'a pas sa route explicite passe par le
       not-found : l'archive tranche. /css/theme.css, /js/…, et un 404 propre
       pour le reste. /api/* garde son 404 JSON. */
    server.onNotFound([](AsyncWebServerRequest *request){
        if (request->url().startsWith("/api/")) {
            request->send(404, "application/json", "{\"error\":\"route inconnue\"}");
            return;
        }
        _sertArchiveApp(request, request->url());
    });

    // L'UI historique de Patrice reste joignable — filet et comparaison.
    server.on("/patrice", HTTP_GET, [](AsyncWebServerRequest *request){
        size_t htmlLen = strlen_P(INDEX_HTML);
        AsyncWebServerResponse *response = request->beginResponse("text/html; charset=utf-8", htmlLen,
            [htmlLen](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                size_t toWrite = (htmlLen - index < maxLen) ? (htmlLen - index) : maxLen;
                if (toWrite > 0) {
                    memcpy_P(buffer, INDEX_HTML + index, toWrite);
                }
                return toWrite;
            });
        response->addHeader("Connection", "close");
        request->send(response);
    });
    
    // Bundle JavaScript compressé en gzip (route /bundle)
    server.on("/bundle", HTTP_GET, [](AsyncWebServerRequest *request){
        const char *type = "text/javascript";
        AsyncWebServerResponse *response = request->beginResponse_P(200, type, BUNDLE, BUNDLE_LEN);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    // WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
#if NIDMI_WEB_DEBUG_CONSOLE
    nidmi_web_debug_init(&ws);
#endif
    
    // Initialiser le registre des composants
    ComponentRegistry::init();
    
    // Configurer les autres APIs
    setupNetworkAPI(server);
    setupRTPAPI(server);
    setupUsbMidiAPI(server);
    setupPinAPI(server);
    setupOSC_API(server);
    setupCacheAPI(server);
    setupComponentsAPI(server);
    setupSystemAPI(server);
    setupOtaAPI(server);
    setupAudioAPI(server);
}
