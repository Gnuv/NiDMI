#include "APICommon.h"
#include "../components/basic/ButtonDef.h"
#include "../utils/ComponentInitializer.h"
#include "../server/WebDebugConsole.h"   // ButtonConfig, pour retablir le pull
#include "../utils/PinMapper.h"
#include "../utils/JSONParser.h"
#include "../managers/ComponentManager.h"
#include "../server/ServerCallbacks.h" /* Pour nidmi_requestReloadPins */
#include "../hardware/MuxConstants.h"
#include "../Globals.h"
#include "../config/ConfigCache.h"
#include "../components/ComponentRegistry.h"
#include "../components/ValidationRegistry.h"
#include "../managers/complex/ComplexHandlerRegistry.h"
#include "../network/UsbMidiManager.h"

/* Forward declarations */
String getDefaultConfig(String pin);

/* Limite NVS : une valeur ne doit pas dépasser ~1984 octets (ESP-IDF). Garder marge pour éviter corruption. */
static const size_t NVS_MAX_PIN_CONFIG_SIZE = 1900U;

/* Sentinelle pour indiquer au request handler que le body était trop gros (413) */
static const void* PINAPI_PAYLOAD_TOO_LARGE = (const void*)1;

/** Extrait la valeur d'une clé JSON "\"key\":\"value\"" depuis un buffer (évite String complète = moins de pile) */
static bool extractJsonQuoted(const char* json, size_t jsonLen, const char* key, char* out, size_t outLen) {
    if (!json || !key || !out || outLen == 0) return false;
    size_t keyLen = strlen(key);
    /* Chercher "\"key\":\"" */
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    size_t patternLen = strlen(pattern);
    const char* p = strstr(json, pattern);
    if (!p || (size_t)(p - json) + patternLen > jsonLen) return false;
    p += patternLen;
    const char* end = (const char*)memchr(p, '"', jsonLen - (size_t)(p - json));
    if (!end) return false;
    size_t valLen = (size_t)(end - p);
    if (valLen >= outLen) valLen = outLen - 1;
    memcpy(out, p, valLen);
    out[valLen] = '\0';
    return true;
}

/* ── Fusion d'une configuration de broche ────────────────────────────────────
 *
 * REGLE : une ecriture PARTIELLE ne doit jamais detruire le reste.
 *
 * /api/pins/set remplacait la configuration entiere par ce que portait la
 * requete. Or l'interface envoie souvent quelques champs : choisir le role
 * d'une broche expedie « {gpio, label, role} » et rien d'autre — le script de
 * mapping, le nom du composant et le mode MIDI etaient donc EFFACES sur la
 * carte, en silence, par un simple choix dans une liste. C'est ce qui rendait
 * un bouton muet des qu'on retouchait a sa configuration.
 *
 * On conserve donc les champs que la requete ne mentionne pas. Ce que
 * l'utilisateur change, change ; le reste continue — la regle du headless,
 * appliquee a la configuration.
 *
 * Pour EFFACER une broche il y a /api/pins/delete : c'est explicite, et ca
 * doit le rester.                                                            */
static String fusionnerConfigBroche(const String& neuf, const String& ancien) {
    if (ancien.length() < 2 || neuf.length() < 2) return neuf;

    String sortie = neuf;
    int i = ancien.indexOf('{');
    if (i < 0) return neuf;
    i++;

    while (i < (int)ancien.length()) {
        while (i < (int)ancien.length() && (ancien[i] == ' ' || ancien[i] == ',')) i++;
        if (i >= (int)ancien.length() || ancien[i] == '}') break;
        if (ancien[i] != '"') break;                       // pas un objet plat : on renonce

        const int debutCle = ++i;
        while (i < (int)ancien.length() && ancien[i] != '"') { if (ancien[i] == '\\') i++; i++; }
        const String cle = ancien.substring(debutCle, i);
        i++;                                                // guillemet fermant
        while (i < (int)ancien.length() && (ancien[i] == ' ' || ancien[i] == ':')) i++;

        const int debutVal = i;
        if (ancien[i] == '"') {                             // chaine
            i++;
            while (i < (int)ancien.length() && ancien[i] != '"') { if (ancien[i] == '\\') i++; i++; }
            i++;
        } else if (ancien[i] == '{' || ancien[i] == '[') {   // objet ou tableau
            int prof = 0; bool chaine = false;
            for (; i < (int)ancien.length(); i++) {
                const char c = ancien[i];
                if (chaine) { if (c == '\\') i++; else if (c == '"') chaine = false; continue; }
                if (c == '"') { chaine = true; continue; }
                if (c == '{' || c == '[') prof++;
                else if (c == '}' || c == ']') { if (--prof == 0) { i++; break; } }
            }
        } else {                                            // nombre, true, false, null
            while (i < (int)ancien.length() && ancien[i] != ',' && ancien[i] != '}') i++;
        }
        String valeur = ancien.substring(debutVal, i);
        valeur.trim();

        if (!cle.length() || !valeur.length()) continue;
        // Deja porte par la requete ? Alors c'est elle qui fait foi.
        if (sortie.indexOf(String("\"") + cle + "\":") >= 0) continue;

        const int accolade = sortie.lastIndexOf('}');
        if (accolade < 0) break;
        sortie = sortie.substring(0, accolade) + ",\"" + cle + "\":" + valeur + "}";
    }
    return sortie;
}

void setupPinAPI(AsyncWebServer& server) {
    /* API - Capacités des pins (dynamique selon MCU) */
    server.on("/api/pins/caps", HTTP_GET, [](AsyncWebServerRequest *request){
        /* Détecter le MCU */
        PinMapper::detectMcu();
        
        /* Construire JSON dynamique */
        String json = "{";
        String mcuName = PinMapper::getMcuName();
        mcuName.toLowerCase();
        json += "\"board\":\"" + mcuName + "\",";
        json += "\"pins\":[";
        
        const PinMapping* mappings = PinMapper::getAllMappings();
        size_t count = PinMapper::getMappingCount();

        /* Pin(s) "sensible(s)" : occupée(s) par un périphérique interne au firmware, à ne pas
         * réutiliser pour un composant. Cas actuel : sur S3 avec USB-MIDI actif au compile-time,
         * l'USB natif est dédié au MIDI (TinyUSB) donc Serial retombe sur l'UART0 matériel
         * (GPIO43/44 = D6/D7) -> ces pins sont occupées en continu par la console debug. */
        bool serialOnUart = nidmi_usb_midi_enabled_at_compile_time();
        uint8_t uartTxGpio = serialOnUart ? PinMapper::labelToGpio("TX") : 255;
        uint8_t uartRxGpio = serialOnUart ? PinMapper::labelToGpio("RX") : 255;

        for (size_t i = 0; i < count; i++) {
            if (i > 0) json += ",";
            bool isSensitive = serialOnUart &&
                (mappings[i].gpio == uartTxGpio || mappings[i].gpio == uartRxGpio);
            json += "{";
            json += "\"gpio\":" + String(mappings[i].gpio) + ",";
            json += "\"label\":\"" + String(mappings[i].label) + "\",";
            json += "\"caps\":{";
            json += "\"in\":true,";
            json += "\"out\":true,";
            json += "\"adc\":" + String(mappings[i].has_adc ? "true" : "false") + ",";
            json += "\"pwm\":" + String(mappings[i].has_pwm ? "true" : "false") + ",";
            json += "\"touch\":" + String(mappings[i].has_touch ? "true" : "false");
            json += "},";
            json += "\"sensitive\":" + String(isSensitive ? "true" : "false");
            if (isSensitive) {
                json += ",\"sensitiveReason\":\"Console série (Serial) - USB natif dédié au MIDI\"";
            }
            json += "}";
        }
        
        json += "],";
        json += "\"bus\":{";
        
        /* Bus I2C - Utiliser PinMapper pour obtenir les GPIO dynamiquement */
        json += "\"i2c\":{";
        uint8_t sda_gpio = PinMapper::labelToGpio("SDA");
        uint8_t scl_gpio = PinMapper::labelToGpio("SCL");
        json += "\"sda\":" + String(sda_gpio) + ",\"scl\":" + String(scl_gpio);
        json += "},";
        
        /* Bus SPI - Utiliser PinMapper pour obtenir les GPIO dynamiquement */
        json += "\"spi\":{";
        uint8_t mosi_gpio = PinMapper::labelToGpio("MOSI");
        uint8_t miso_gpio = PinMapper::labelToGpio("MISO");
        uint8_t sck_gpio = PinMapper::labelToGpio("SCK");
        json += "\"mosi\":" + String(mosi_gpio) + ",\"miso\":" + String(miso_gpio) + ",\"sck\":" + String(sck_gpio);
        json += "},";
        
        /* Bus UART - Utiliser PinMapper pour obtenir les GPIO dynamiquement */
        json += "\"uart\":{";
        uint8_t tx_gpio = PinMapper::labelToGpio("TX");
        uint8_t rx_gpio = PinMapper::labelToGpio("RX");
        json += "\"tx\":" + String(tx_gpio) + ",\"rx\":" + String(rx_gpio);
        json += "}";
        
        json += "}";
        json += "}";
        
        request->send(200, "application/json", json);
    });

    /* API - Liste des pins configurées (format pour saveAll avec pinLabel) */
    /* Inclut aussi les composants complexes depuis MuxManager */
    /* API - Lecture ADC brute d'une pin.
     * Outil de diagnostic câblage : permet de voir la valeur réellement lue par l'ADC
     * sans dépendre de la télémétrie (qui n'est émise que lors d'un événement MIDI).
     * Usage : /api/pins/read?gpio=1  ou  /api/pins/read?gpio=D0
     * Retourne un échantillonnage court (min/max/moyenne) pour rendre le bruit visible. */
    /* ── CE QUE LA CARTE EXECUTE ──────────────────────────────────────────
     * /api/pins/list dit ce qu'il y a en memoire morte. Celle-ci dit ce que la
     * carte a CHARGE et execute a cet instant. Les deux doivent coincider ;
     * quand elles divergent, une ecriture n'a pas pris effet.
     * C'est la regle — « la page montre l'etat exact de ce qui tourne » —
     * rendue verifiable en une requete. Elle reste, la ou l'echafaudage de
     * debogage a ete retire.                                                 */
    server.on("/api/pins/actif", HTTP_GET, [](AsyncWebServerRequest *request){
        auto ech = [](const String& v){ String o; for (unsigned i=0;i<v.length();i++){ char c=v[i];
            if (c=='"') o+="\\\""; else if (c=='\\') o+="\\\\";
            else if (c=='\n') o+="\\n"; else if ((unsigned char)c<0x20) o+=' '; else o+=c; } return o; };
        /* `rechargement_en_cours` : vrai si un rechargement a commence et n'est
         * pas revenu. Avec une liste VIDE, il distingue les deux pannes qui se
         * ressemblent — « le rechargement s'est bloque apres avoir vide » et
         * « il a fini mais n'a rien trouve ». Deux entiers, pas un
         * echafaudage : c'est la meme question que la route pose deja. */
        String j = String("{\"rechargement_en_cours\":")
                 + (g_componentManager.isNvsWriteInProgress() ? "true" : "false")
                 + ",\"phase\":\"" + g_componentManager.phaseRechargement() + "\""
                 + ",\"phase_i\":" + String(g_componentManager.phaseIndice())
                 + ",\"composants\":[";
        for (uint8_t i = 0; i < g_componentManager.getComponentCount(); i++) {
            const ComponentConfig* c = g_componentManager.getConfig(i);
            if (!c) continue;
            if (j[j.length()-1] != '[') j += ",";
            j += String("{\"gpio\":") + c->gpio
               + ",\"nom\":\"" + ech(String(c->name)) + "\""
               + ",\"mode\":\"" + (c->midiMode == MidiMode::SCRIPT ? "script" : "midi") + "\""
               + ",\"script\":\"" + ech(String(c->mappingScript)) + "\"}";
        }
        j += "]}";
        request->send(200, "application/json", j);
    });

    server.on("/api/pins/read", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("gpio")) {
            request->send(400, "application/json",
                          "{\"error\":\"parametre 'gpio' manquant (ex: ?gpio=1 ou ?gpio=D0)\"}");
            return;
        }

        String param = request->getParam("gpio")->value();
        PinMapper::detectMcu();

        /* Accepter soit un numéro de GPIO, soit un label de pad (D0, D1, ...) */
        uint8_t gpio;
        if (param.length() > 0 && isDigit(param.charAt(0))) {
            gpio = (uint8_t)param.toInt();
        } else {
            gpio = PinMapper::labelToGpio(param);
        }

        if (gpio == 255 || gpio > 48) {
            request->send(404, "application/json", "{\"error\":\"pin inconnue\"}");
            return;
        }

        if (!PinMapper::hasAdc(gpio)) {
            String err = "{\"error\":\"GPIO" + String(gpio) + " (" + PinMapper::gpioToLabel(gpio) +
                         ") n'a pas d'ADC\"}";
            request->send(400, "application/json", err);
            return;
        }

        /* 16 échantillons rapprochés : assez pour révéler le bruit, assez court pour ne pas
         * retarder la tâche réseau. */
        const uint8_t SAMPLES = 16;
        uint32_t sum = 0;
        uint16_t vmin = 4095;
        uint16_t vmax = 0;
        uint16_t last = 0;
        for (uint8_t i = 0; i < SAMPLES; i++) {
            last = (uint16_t)analogRead(gpio);
            sum += last;
            if (last < vmin) vmin = last;
            if (last > vmax) vmax = last;
        }
        uint16_t avg = (uint16_t)(sum / SAMPLES);

        /* RETABLIR LE MODE DE LA BROCHE.
         *
         * analogRead() reconfigure le pad en entree ADC et SUPPRIME le pull
         * interne. Sur une broche portant un composant configure — un bouton en
         * INPUT_PULLUP, typiquement — lire son etat depuis le moniteur la
         * laissait FLOTTANTE jusqu'au redemarrage : le bouton cessait de
         * fonctionner, et la lecture suivante montrait un bruit qu'on prenait
         * pour un defaut de cablage. Piege vecu deux fois, la seconde apres un
         * premier correctif qui ne marchait pas.
         *
         * On appelle donc la primitive OFFICIELLE, celle du demarrage, plutot
         * que de reconstituer sa logique a cote : elle connait les modes de
         * pull, le cas tactile (ou il ne faut PAS appeler pinMode), et tout ce
         * qu'on aurait oublie. Reconstituer, c'est se condamner a diverger. */
        {
            bool retabli = false;
            for (uint8_t i = 0; i < g_componentManager.getComponentCount(); i++) {
                ComponentConfig* cfg = g_componentManager.getConfigMutable(i);
                if (!cfg || cfg->gpio != gpio) continue;
                ComponentInitializer::setupGpio(gpio, cfg->type, cfg);
                retabli = true;
                break;
            }
            NIDMI_WEB_LOG("[pins/read] GPIO%d lu ; mode %s", (int)gpio,
                          retabli ? "retabli (composant configure)" : "laisse en ADC (aucun composant)");
        }

        String json = "{";
        json += "\"gpio\":" + String(gpio) + ",";
        json += "\"label\":\"" + PinMapper::gpioToLabel(gpio) + "\",";
        json += "\"raw\":" + String(last) + ",";
        json += "\"avg\":" + String(avg) + ",";
        json += "\"min\":" + String(vmin) + ",";
        json += "\"max\":" + String(vmax) + ",";
        /* Tension indicative : ADC 12 bits, atténuation par défaut (~3.3 V pleine échelle) */
        json += "\"volts\":" + String(avg * 3.3f / 4095.0f, 2);
        json += "}";
        request->send(200, "application/json", json);
    });

    server.on("/api/pins/list", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"pins\":[";
        
        Preferences preferences;
        preferences.begin("nidmi", true);
        
        bool first = true;
        
        /* Obtenir toutes les pins disponibles pour ce MCU */
        PinMapper::detectMcu();
        const PinMapping* mappings = PinMapper::getAllMappings();
        size_t mapping_count = PinMapper::getMappingCount();
        
        /* Charger les pins simples depuis NVS */
        for (size_t i = 0; i < mapping_count; i++) {
            String pinLabel = String(mappings[i].label);
            String key = "pin_" + pinLabel;
            String configStr = preferences.getString(key.c_str(), "");
            if (!configStr.isEmpty()) {
                /* Ignorer les MUX qui ont additionalPins (seront ajoutés depuis MuxManager) */
                /* Les autres composants avec additionalPins (joystick, etc.) sont inclus normalement */
                if(configStr.indexOf("\"additionalPins\"") >= 0) {
                    /* Vérifier si c'est un MUX (rôle commence par "hc4" pour hc4067/hc4051) */
                    bool isMux = (configStr.indexOf("\"role\":\"hc4") >= 0);
                    if(isMux) {
                        continue;
                    }
                }
                if (!first) json += ",";
                json += configStr;  // Le config est déjà un JSON complet avec pinLabel
                first = false;
            }
        }
        
        /* Charger les pins de bus (I2C, SPI, UART) depuis NVS */
        const char* busLabels[] = {"I2C", "SPI", "TX", "RX"};
        for (int i = 0; i < 4; i++) {
            const char* busLabel = busLabels[i];
            String key = "pin_" + String(busLabel);
            String configStr = preferences.getString(key.c_str(), "");
            if (!configStr.isEmpty()) {
                if (!first) json += ",";
                json += configStr;  // Le config est déjà un JSON complet avec pinLabel
                first = false;
            }
        }
        
        /* Ajouter les composants complexes depuis MuxManager */
        for (uint8_t i = 0; i < MAX_MUXES; i++) {
            const MuxConfig* cfg = g_componentManager.getMuxConfig(i);
            if (cfg && cfg->enabled) {
                /* Chercher d'abord le pinLabel depuis NVS (pinLabel original sauvegardé) */
                String sigPinLabel = "";
                /* Essayer d'abord la clé générique, puis la clé spécifique MUX pour compatibilité */
                String keyNVS = "pinLabel_complex_" + String(i);
                String savedPinLabel = preferences.getString(keyNVS.c_str(), "");
                if(savedPinLabel.isEmpty()) {
                    /* Fallback pour compatibilité avec l'ancien format */
                    keyNVS = "pinLabel_mux_" + String(i);
                    savedPinLabel = preferences.getString(keyNVS.c_str(), "");
                }
                if(!savedPinLabel.isEmpty()) {
                    sigPinLabel = savedPinLabel;
                } else {
                    /* Si pas trouvé dans NVS, trouver le pinLabel correspondant au GPIO SIG */
                    /* Préférer les labels analogiques (commencent par "A") si disponibles */
                    String analogPinLabel = "";
                    for (size_t j = 0; j < mapping_count; j++) {
                        if(mappings[j].gpio == cfg->sig_pin) {
                            String label = String(mappings[j].label);
                            if(label.startsWith("A")) {
                                analogPinLabel = label;
                                break;
                            } else if(sigPinLabel.isEmpty()) {
                                sigPinLabel = label;
                            }
                        }
                    }
                    if(!analogPinLabel.isEmpty()) {
                        sigPinLabel = analogPinLabel;
                    }
                }
                
                if(!sigPinLabel.isEmpty()) {
                    /* Essayer d'abord de lire depuis NVS (contient tous les paramètres sauvegardés) */
                    String key = "pin_" + sigPinLabel;
                    String configStr = preferences.getString(key.c_str(), "");
                    
                    if(!configStr.isEmpty()) {
                        /* Si la config existe dans NVS, l'utiliser directement (contient tous les paramètres MIDI, formFields, etc.) */
                        if (!first) json += ",";
                        json += configStr;  /* Le config est déjà un JSON complet avec pinLabel */
                        first = false;
                    } else {
                        /* Fallback : construire depuis MuxConfig si pas trouvé dans NVS (compatibilité) */
                        if (!first) json += ",";
                        
                        /* Obtenir le rôle depuis NVS (stocké lors de la sauvegarde) */
                        String roleKey = "role_complex_" + String(i);
                        String role = preferences.getString(roleKey.c_str(), "");
                        if(role.isEmpty()) {
                            /* Fallback pour compatibilité avec l'ancien format */
                            roleKey = "role_mux_" + String(i);
                            role = preferences.getString(roleKey.c_str(), "");
                        }
                        /* Fallback : si pas trouvé, utiliser "hc4067" par défaut pour compatibilité */
                        if(role.isEmpty()) {
                            role = "hc4067";
                        }
                        
                        /* Construire le JSON unifié avec additionalPins en utilisant le handler générique */
                        ComplexHandler* handler = ComplexHandlerRegistry::getHandler(role.c_str());
                        if(handler) {
                            /* Utiliser le handler générique pour obtenir les infos */
                            json += "{";
                            json += "\"pinLabel\":\"" + sigPinLabel + "\",";
                            json += "\"role\":\"" + role + "\",";
                            String infoJson = "";
                            if(handler->getComponentInfo(sigPinLabel.c_str(), cfg->sig_pin, infoJson)) {
                                json += infoJson;  /* Ajoute additionalPins, formFields, midiParams, etc. */
                            } else {
                                /* Fallback si getComponentInfo échoue */
                                json += "\"additionalPins\":{},";
                                json += "\"min\":0,\"max\":4095,";
                                json += "\"midiCc\":1,\"midiChannel\":1,\"rtpMidiEnabled\":true";
                            }
                            json += "}";
                        } else {
                            /* Fallback si handler non trouvé (ne devrait pas arriver) */
                            json += "{";
                            json += "\"pinLabel\":\"" + sigPinLabel + "\",";
                            json += "\"role\":\"" + role + "\",";
                            json += "\"additionalPins\":{},";
                            json += "\"min\":0,\"max\":4095,";
                            json += "\"rtpCc\":1,\"rtpChan\":1,\"rtpEnabled\":true";
                            json += "}";
                        }
                        first = false;
                    }
                }
            }
        }
        
        preferences.end();
        json += "]}";
        
        request->send(200, "application/json", json);
    });

    /* API - Configuration d'une pin (format paramètres URL ou JSON direct) */
    /* Body handler pour recevoir le JSON brut envoyé par le LIS3DH */
    server.on("/api/pins/set", HTTP_POST,
    /* Request handler (fin de requête) */
    [](AsyncWebServerRequest *request){
        /* Body trop gros (refusé par le body handler pour ne pas corrompre la NVS) */
        if (request->_tempObject == PINAPI_PAYLOAD_TOO_LARGE) {
            request->_tempObject = nullptr;
            request->send(413, "application/json", "{\"status\":\"error\",\"message\":\"Config trop grande pour NVS (max 1900 octets)\"}");
            return;
        }
        /* Si le body JSON a déjà été traité par le body handler, ne rien faire */
        if (request->_tempObject) {
            request->send(200, "application/json", "{\"status\":\"ok\"}");
            free(request->_tempObject);
            request->_tempObject = nullptr;
            return;
        }
        
        if(!request->hasParam("pinLabel", true) || !request->hasParam("role", true)){
            request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"pinLabel and role required\"}");
            return;
        }
        
        String pinLabel = request->getParam("pinLabel", true)->value();
        String role = request->getParam("role", true)->value();
        
        /* Obtenir la définition du composant pour lecture dynamique des paramètres */
        const ComponentDefinition* def = ComponentRegistry::findById(role.c_str());
        
        /* Trouver le GPIO correspondant au pinLabel */
        uint8_t sigGpio = 255;
        PinMapper::detectMcu();
        const PinMapping* mappings = PinMapper::getAllMappings();
        size_t mapping_count = PinMapper::getMappingCount();
        for (size_t i = 0; i < mapping_count; i++) {
            if(String(mappings[i].label) == pinLabel) {
                sigGpio = mappings[i].gpio;
                break;
            }
        }
        
        /* Echappement JSON COMPLET.
         *
         * L'ancien ne traitait que « \ » et « " ». Un script de mapping tient
         * desormais sur plusieurs lignes — les pipelines sont separes par « ; »
         * et on les ecrit l'un sous l'autre — et un saut de ligne BRUT dans une
         * chaine JSON est invalide. La config partait telle quelle en NVS, puis
         * /api/pins/list la recrachait : JSON.parse echouait cote app et TOUTE
         * la zone d'inventaire I/O restait vide. Une ligne de trop dans un
         * script, et plus aucune broche ne s'affichait.
         *
         * On echappe donc aussi les caracteres de controle. */
        auto jsonChaine = [](const String& v) -> String {
            String out;
            out.reserve(v.length() + 8);
            for (unsigned i = 0; i < v.length(); i++) {
                const char c = v[i];
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '"':  out += "\\\""; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    default:
                        if ((unsigned char)c < 0x20) {
                            char u[8];
                            snprintf(u, sizeof u, "\\u%04x", (unsigned)(unsigned char)c);
                            out += u;
                        } else out += c;
                }
            }
            return out;
        };

        /* Construire le JSON à partir des paramètres */
        String json = "{";
        json += "\"pinLabel\":\"" + pinLabel + "\",";
        json += "\"role\":\"" + role + "\"";
        
        /* forcerChaine : le champ est du TEXTE, quoi qu'il ressemble. Sans ca,
         * un script commencant par un chiffre — « 60 : note.out(1) ; » est un
         * .nms parfaitement valide — passait par la branche « nombre » et
         * sortait sans guillemets, donc en JSON invalide. L'heuristique ne peut
         * pas deviner : c'est au champ de dire ce qu'il est. */
        auto addParamEx = [&](const char* name, bool forcerChaine) {
            String keyCheck = String("\"") + name + "\":";
            if(json.indexOf(keyCheck) >= 0) return;
            if(!request->hasParam(name, true)) return;
            String val = request->getParam(name, true)->value();
            const bool nombre = !forcerChaine && val.indexOf(',') < 0 &&
                ((val.length() > 0 && val[0] >= '0' && val[0] <= '9') ||
                 (val.length() > 1 && val[0] == '-' && val[1] >= '0' && val[1] <= '9'));
            if(!forcerChaine && (val == "true" || val == "false"))
                json += ",\"" + String(name) + "\":" + val;
            else if(nombre)
                json += ",\"" + String(name) + "\":" + val;
            else
                json += ",\"" + String(name) + "\":\"" + jsonChaine(val) + "\"";
        };
        auto addParam = [&](const char* name) { addParamEx(name, false); };
        
        /* Paramètres MIDI communs (toujours présents) */
        addParam("rtpMidiEnabled"); // Compatibilité: accepter aussi rtpEnabled
        addParam("rtpEnabled"); // Ancien format pour compatibilité
        addParam("midiMessageType"); // Nouveau format
        addParam("rtpType"); // Ancien format pour compatibilité
        /* Champs personnalisés de mapping script */
        addParamEx("mappingScript", /*forcerChaine=*/true);
        /* Mode MIDI: RTP vs Mapping Script */
        addParam("midiMode");
        /* Pour composants avec axes (joystick, IMU), sauvegarder les types MIDI par axe */
        addParam("midiMessageTypeX");
        addParam("midiMessageTypeY");
        addParam("midiMessageTypeZ");
        
        /* Lire dynamiquement les paramètres MIDI depuis def->midiMessages[].params[] */
        if(def && def->midiMessageCount > 0 && def->midiMessages) {
            for(uint8_t i = 0; i < def->midiMessageCount; i++) {
                const MidiMessageDef& msg = def->midiMessages[i];
                if(msg.params && msg.paramCount > 0) {
                    for(uint8_t j = 0; j < msg.paramCount && j < msg.paramsCapacity; j++) {
                        const MidiParamDef& param = msg.params[j];
                        if(param.id && strlen(param.id) > 0) {
                            /* Traiter les paramètres RANGE comme Min/Max */
                            if(param.type == FieldType::RANGE) {
                                String minId = String(param.id) + "Min";
                                String maxId = String(param.id) + "Max";
                                // Toujours sauvegarder les valeurs (même si par défaut)
                                if(request->hasParam(minId.c_str(), true)) {
                                    addParam(minId.c_str());
                                } else if(param.defaultMin) {
                                    // Sauvegarder la valeur par défaut si absente
                                    json += ",\"" + minId + "\":" + String(param.defaultMin);
                                }
                                if(request->hasParam(maxId.c_str(), true)) {
                                    addParam(maxId.c_str());
                                } else if(param.defaultMax) {
                                    // Sauvegarder la valeur par défaut si absente
                                    json += ",\"" + maxId + "\":" + String(param.defaultMax);
                                }
                            } else {
                                /* Pour autres types (NUMBER, INFO, etc.) */
                                addParam(param.id);
                            }
                            
                            /* Si le message a un axe, sauvegarder aussi les params préfixés (X_midiCc, Y_midiCc, etc.) */
                            if(msg.axis && strlen(msg.axis) > 0) {
                                char axisUpper = (msg.axis[0] >= 'a' && msg.axis[0] <= 'z') ? (msg.axis[0] - 32) : msg.axis[0];
                                if(param.type == FieldType::RANGE) {
                                    String prefixedMinId = String(axisUpper) + "_" + String(param.id) + "Min";
                                    String prefixedMaxId = String(axisUpper) + "_" + String(param.id) + "Max";
                                    addParam(prefixedMinId.c_str());
                                    addParam(prefixedMaxId.c_str());
                                } else {
                                    String prefixedId = String(axisUpper) + "_" + String(param.id);
                                    addParam(prefixedId.c_str());
                                }
                            }
                        }
                    }
                }
            }
        }
        
        
        /* Joystick : garantir X_midiCc et Y_midiCc dans le JSON (fallback sur midiCc si absents de la requête) */
        if(role == "joystick") {
            int xCc = 7, yCc = 7;
            if(request->hasParam("X_midiCc", true)) {
                xCc = request->getParam("X_midiCc", true)->value().toInt();
            } else if(request->hasParam("midiCc", true)) {
                xCc = request->getParam("midiCc", true)->value().toInt();
            }
            if(request->hasParam("Y_midiCc", true)) {
                yCc = request->getParam("Y_midiCc", true)->value().toInt();
            } else if(request->hasParam("midiCc", true)) {
                yCc = request->getParam("midiCc", true)->value().toInt();
            }
            if(!request->hasParam("X_midiCc", true)) {
                json += ",\"X_midiCc\":" + String(xCc);
            }
            if(!request->hasParam("Y_midiCc", true)) {
                json += ",\"Y_midiCc\":" + String(yCc);
            }
        }
        
        /* Lire dynamiquement les formFields depuis def->formFields[] */
        if(def && def->formFieldCount > 0 && def->formFields) {
            for(uint8_t i = 0; i < def->formFieldCount && i < def->formFieldsCapacity; i++) {
                const FormFieldDef& field = def->formFields[i];
                if(field.id && strlen(field.id) > 0 && field.id[0] != '_') {
                    if(field.type == FieldType::CHECKBOX) {
                        /* Pour checkbox, vérifier que la valeur est "true" */
                        if(request->hasParam(field.id, true)) {
                            String val = request->getParam(field.id, true)->value();
                            if(val == "true") {
                                addParam(field.id);
                            }
                        }
                    } else if(field.type == FieldType::RANGE) {
                        /* Pour RANGE, lire Min et Max */
                        String minId = String(field.id) + "Min";
                        String maxId = String(field.id) + "Max";
                        if(request->hasParam(minId.c_str(), true)) {
                            addParam(minId.c_str());
                        }
                        if(request->hasParam(maxId.c_str(), true)) {
                            addParam(maxId.c_str());
                        }
                    } else {
                        /* Pour autres types (TEXT, NUMBER, SELECT, INFO) */
                        if(strcmp(field.id, "csGpio") == 0) {
                            Serial.printf("[PinAPI] csGpio trouvé dans formFields, hasParam: %d\n", request->hasParam(field.id, true));
                            if(request->hasParam(field.id, true)) {
                                String val = request->getParam(field.id, true)->value();
                                Serial.printf("[PinAPI] csGpio valeur: %s\n", val.c_str());
                            }
                        }
                        addParam(field.id);
                    }
                }
            }
        }
        
        /* Paramètres communs (OSC, Debug) */
        addParam("oscEnabled");
        addParam("oscAddress");
        addParam("oscFormat");
        addParam("dbgEnabled");
        addParam("dbgHeader");
        
        /* Nom personnalisé du composant */
        addParam("name");
        
        /* Vérifier si le composant a des additionalPins */
        bool hasAdditionalPins = false;
        
        if(def && def->additionalPinCount > 0 && def->additionalPins) {
            /* Vérifier que tous les paramètres required sont présents */
            hasAdditionalPins = true;
            for(uint8_t i = 0; i < def->additionalPinCount && i < def->additionalPinsCapacity; i++) {
                if(!def->additionalPins[i].optional) {
                    String pinId = String(def->additionalPins[i].id);
                    bool hasParam = request->hasParam(pinId.c_str(), true);
                    if(!hasParam) {
                        hasAdditionalPins = false;
                        break;
                    }
                }
            }
            
            if(hasAdditionalPins) {
                json += ",\"additionalPins\":{";
                bool first = true;
                for(uint8_t i = 0; i < def->additionalPinCount && i < def->additionalPinsCapacity; i++) {
                    const AdditionalPinDef& pin = def->additionalPins[i];
                    if(request->hasParam(pin.id, true)) {
                        if(!first) json += ",";
                        json += "\"" + String(pin.id) + "\":" + request->getParam(pin.id, true)->value();
                        first = false;
                    } else if(!pin.optional) {
                        // Pin requise absente (ne devrait pas arriver après la vérification ci-dessus)
                        hasAdditionalPins = false;
                        break;
                    } else {
                        // Pin optionnelle absente, utiliser la valeur par défaut
                        if(!first) json += ",";
                        json += "\"" + String(pin.id) + "\":" + String(pin.defaultValue);
                        first = false;
                    }
                }
                json += "}";
                
                /* Note: complexId supprimé - plus besoin d'ID explicite */
            }
        }
        
        json += "}";
        
        /* FUSION : on conserve ce que la requete ne mentionne pas. */
        {
            Preferences lecture;
            if (lecture.begin("nidmi", true)) {
                const String cleF = "pin_" + pinLabel;
                const String ancien = lecture.getString(cleF.c_str(), "");
                lecture.end();
                if (ancien.length()) {
                    const String fusionne = fusionnerConfigBroche(json, ancien);
                    if (fusionne.length() != json.length())
                        Serial.printf("[PinAPI] %s : fusion %u -> %u octets\n",
                                      pinLabel.c_str(), (unsigned)json.length(),
                                      (unsigned)fusionne.length());
                    json = fusionne;
                }
            }
        }

        if (json.length() > NVS_MAX_PIN_CONFIG_SIZE) {
            Serial.printf("[PinAPI] JSON trop gros pour NVS: %u > %u (pin=%s)\n",
                (unsigned)json.length(), (unsigned)NVS_MAX_PIN_CONFIG_SIZE, pinLabel.c_str());
            request->send(413, "application/json", "{\"status\":\"error\",\"message\":\"Config trop grande pour NVS (max 1900 octets)\"}");
            return;
        }
        
        Preferences preferences;
        preferences.begin("nidmi", false);
        String key = "pin_" + pinLabel;
        size_t written = preferences.putString(key.c_str(), json);
        preferences.end();

        if (written == 0) {
            Serial.printf("[PinAPI] ERREUR NVS pour %s\n", pinLabel.c_str());
        }

        /* Pas de nidmi_requestReloadPins() ICI : il y en a deja un a la fin de
         * ce meme gestionnaire, present depuis l'origine (juste avant le
         * request->send). J'en avais ajoute un second en croyant qu'il
         * manquait — ma recherche s'etait arretee 200 lignes trop tot, avant
         * le bloc des composants a broches multiples. Une recherche BORNEE ne
         * prouve pas une absence : deuxieme fois dans cette session. */
        
        /* Si additionalPins présent, utiliser le handler générique pour ce type de composant */
        if(hasAdditionalPins && def) {
            /* Obtenir le handler pour ce type de composant */
            ComplexHandler* handler = ComplexHandlerRegistry::getHandler(role.c_str());
            
            if(handler) {
                /* Construire ComplexComponentData depuis la requête HTTP */
                ComplexComponentData data;
                data.def = def;
                data.pinLabel = pinLabel.c_str();
                data.mainPinGpio = sigGpio;
                
                /* Allouer et remplir additionalPins */
                data.additionalPinCount = def->additionalPinCount;
                data.additionalPins = new ComplexComponentData::AdditionalPinValue[data.additionalPinCount];
                for(uint8_t i = 0; i < def->additionalPinCount && i < def->additionalPinsCapacity; i++) {
                    const AdditionalPinDef& pinDef = def->additionalPins[i];
                    data.additionalPins[i].id = pinDef.id;
                    
                    if(request->hasParam(pinDef.id, true)) {
                        data.additionalPins[i].gpio = request->getParam(pinDef.id, true)->value().toInt();
                    } else if(!pinDef.optional && pinDef.defaultValue != 255) {
                        data.additionalPins[i].gpio = pinDef.defaultValue;
                    } else {
                        data.additionalPins[i].gpio = pinDef.defaultValue;  /* 255 pour non connecté */
                    }
                }
                
                /* Allouer et remplir formFields */
                data.formFieldCount = def->formFieldCount;
                if(data.formFieldCount > 0) {
                    data.formFields = new ComplexComponentData::FormFieldValue[data.formFieldCount];
                    uint8_t fieldIndex = 0;
                    for(uint8_t i = 0; i < def->formFieldCount && i < def->formFieldsCapacity && fieldIndex < data.formFieldCount; i++) {
                        const FormFieldDef& field = def->formFields[i];
                        if(field.id && strlen(field.id) > 0 && field.id[0] != '_') {
                            data.formFields[fieldIndex].id = field.id;
                            if(request->hasParam(field.id, true)) {
                                data.formFields[fieldIndex].value = request->getParam(field.id, true)->value();
                            } else if(field.defaultValue && strlen(field.defaultValue) > 0) {
                                data.formFields[fieldIndex].value = String(field.defaultValue);
                            } else {
                                data.formFields[fieldIndex].value = "";
                            }
                            fieldIndex++;
                        }
                    }
                    data.formFieldCount = fieldIndex;  /* Ajuster le count réel */
                } else {
                    data.formFields = nullptr;
                }
                
                /* Allouer et remplir midiParams */
                data.midiParamCount = 0;
                data.midiParams = nullptr;
                if(def && def->midiMessageCount > 0 && def->midiMessages) {
                    /* Compter les paramètres MIDI */
                    for(uint8_t i = 0; i < def->midiMessageCount; i++) {
                        const MidiMessageDef& msg = def->midiMessages[i];
                        if(msg.params && msg.paramCount > 0) {
                            data.midiParamCount += msg.paramCount;
                        }
                    }
                    
                    if(data.midiParamCount > 0) {
                        data.midiParams = new ComplexComponentData::MidiParamValue[data.midiParamCount];
                        uint8_t paramIndex = 0;
                        for(uint8_t i = 0; i < def->midiMessageCount; i++) {
                            const MidiMessageDef& msg = def->midiMessages[i];
                            if(msg.params && msg.paramCount > 0) {
                                for(uint8_t j = 0; j < msg.paramCount && j < msg.paramsCapacity && paramIndex < data.midiParamCount; j++) {
                                    const MidiParamDef& param = msg.params[j];
                                    if(param.id && strlen(param.id) > 0) {
                                        data.midiParams[paramIndex].id = param.id;
                                        if(request->hasParam(param.id, true)) {
                                            data.midiParams[paramIndex].value = request->getParam(param.id, true)->value();
                                        } else if(param.defaultValue && strlen(param.defaultValue) > 0) {
                                            data.midiParams[paramIndex].value = String(param.defaultValue);
                                        } else {
                                            data.midiParams[paramIndex].value = "";
                                        }
                                        paramIndex++;
                                    }
                                }
                            }
                        }
                        data.midiParamCount = paramIndex;  /* Ajuster le count réel */
                    }
                }
                
                /* Lire paramètres OSC/Debug */
                data.oscEnabled = request->hasParam("oscEnabled", true) && request->getParam("oscEnabled", true)->value() == "true";
                data.oscAddress = request->hasParam("oscAddress", true) ? request->getParam("oscAddress", true)->value() : "";
                data.oscFormat = request->hasParam("oscFormat", true) ? request->getParam("oscFormat", true)->value() : "float";
                data.dbgEnabled = request->hasParam("dbgEnabled", true) && request->getParam("dbgEnabled", true)->value() == "true";
                data.dbgHeader = request->hasParam("dbgHeader", true) ? request->getParam("dbgHeader", true)->value() : "";
                
                /* VALIDATION AVANT d'appeler addComponent() */
                auto validation = ValidationRegistry::validateComplex(role.c_str(), data);
                if(!validation.valid) {
                    /* Libérer la mémoire allouée */
                    if(data.additionalPins) delete[] data.additionalPins;
                    if(data.formFields) delete[] data.formFields;
                    if(data.midiParams) delete[] data.midiParams;
                    
                    request->send(400, "application/json", 
                        "{\"status\":\"error\",\"message\":\"" + validation.error_message + "\"}");
                    return;
                }
                
                /* Appeler le handler générique */
                if(handler->addComponent(data)) {
                    /* Composant complexe ajouté avec succès */
                } else {
                    /* Libérer la mémoire allouée */
                    delete[] data.additionalPins;
                    if(data.formFields) delete[] data.formFields;
                    if(data.midiParams) delete[] data.midiParams;
                    request->send(500, "application/json", "{\"status\":\"error\",\"error\":\"Failed to add complex component\"}");
                    return;
                }
                
                /* Libérer la mémoire allouée */
                delete[] data.additionalPins;
                if(data.formFields) delete[] data.formFields;
                if(data.midiParams) delete[] data.midiParams;
            }
        }
        
        /* Mettre à jour ConfigCache */
        g_configCache.setConfigClean(pinLabel, json);
        nidmi_requestReloadPins();
        
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    },
    /* Upload handler (non utilisé) */
    NULL,
    /* Body handler : reçoit le JSON brut (LIS3DH, MPR121). Réduire la pile : pas de String(json) complète. */
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (total == 0 || total > 4096) return; /* Taille invalide ou trop grosse pour NVS */
        if (index == 0) {
            request->_tempObject = malloc(total + 1);
            if (!request->_tempObject) return;
        }
        if (!request->_tempObject) return;
        memcpy((uint8_t*)request->_tempObject + index, data, len);
        if (index + len != total) return;
        ((char*)request->_tempObject)[total] = '\0';
        const char* buf = (const char*)request->_tempObject;
        char pinLabelBuf[16];
        char roleBuf[32];
        if (!extractJsonQuoted(buf, total, "pinLabel", pinLabelBuf, sizeof(pinLabelBuf)) ||
            !extractJsonQuoted(buf, total, "role", roleBuf, sizeof(roleBuf)) ||
            pinLabelBuf[0] == '\0' || roleBuf[0] == '\0') {
            Serial.printf("[PinAPI] JSON body invalide (pinLabel ou role manquant, len=%u)\n", (unsigned)total);
            free(request->_tempObject);
            request->_tempObject = nullptr;
            return;
        }
        if (total > NVS_MAX_PIN_CONFIG_SIZE) {
            Serial.printf("[PinAPI] JSON body trop gros pour NVS: %u > %u (pin=%s)\n",
                (unsigned)total, (unsigned)NVS_MAX_PIN_CONFIG_SIZE, pinLabelBuf);
            free(request->_tempObject);
            request->_tempObject = (void*)PINAPI_PAYLOAD_TOO_LARGE;
            return;
        }
        String pinLabel = String(pinLabelBuf);
        String key = String("pin_") + pinLabel;
        Preferences preferences;
        preferences.begin("nidmi", false);
        preferences.putString(key.c_str(), buf); /* Écrire directement depuis le buffer, pas de String(json) */
        preferences.end();
        g_configCache.setConfigClean(pinLabel, buf, total);
        nidmi_requestReloadPins();
        Serial.printf("[PinAPI] JSON body %s role=%s len=%u\n", pinLabelBuf, roleBuf, (unsigned)total);
    });

    /* API - Suppression d'une pin (unifié pour simples et complexes) */
    server.on("/api/pins/delete", HTTP_POST, [](AsyncWebServerRequest *request){
        if(request->hasParam("pin", true)){
            String pinLabel = request->getParam("pin", true)->value();
            
            /* Chercher le complexId depuis MuxManager en premier (plus fiable) */
            PinMapper::detectMcu();
            const PinMapping* mappings = PinMapper::getAllMappings();
            size_t mapping_count = PinMapper::getMappingCount();
            
            /* Trouver le GPIO SIG correspondant au pinLabel */
            uint8_t sigGpio = 255;
            for (size_t i = 0; i < mapping_count; i++) {
                if(String(mappings[i].label) == pinLabel) {
                    sigGpio = mappings[i].gpio;
                    break;
                }
            }
            
            /* Chercher si c'est un composant avec additionalPins en lisant la config NVS */
            Preferences preferences;
            preferences.begin("nidmi", true);
            String key = "pin_" + pinLabel;
            String configStr = preferences.getString(key.c_str(), "");
            preferences.end();
            
            /* Extraire le role depuis la config NVS si disponible */
            String role = "";
            if(configStr.length() > 0) {
                int roleStart = configStr.indexOf("\"role\":\"");
                if(roleStart >= 0) {
                    roleStart += 8;  /* Longueur de "\"role\":\"" */
                    int roleEnd = configStr.indexOf("\"", roleStart);
                    if(roleEnd > roleStart) {
                        role = configStr.substring(roleStart, roleEnd);
                    }
                }
            }
            
            /* Si role trouvé et handler disponible, utiliser le handler générique */
            if(role.length() > 0) {
                ComplexHandler* handler = ComplexHandlerRegistry::getHandler(role.c_str());
                if(handler && sigGpio != 255) {
                    /* Utiliser le handler générique pour supprimer le composant */
                    handler->removeComponent(pinLabel.c_str(), sigGpio);
                }
            } else {
                /* Pour les composants simples, supprimer via ComponentManager */
                if(sigGpio != 255) {
                    g_componentManager.removeComponent(sigGpio);
                }
            }
            
            /* Supprimer dans NVS et ConfigCache (pour tous les types) */
            g_configCache.removeConfig(pinLabel);
            
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"pin required\"}");
        }
    });
}
