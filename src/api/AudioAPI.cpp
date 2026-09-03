#include "APICommon.h"
#include "../audio/AudioEngine.h"
#include "../midi/MidiRouter.h"
#include "../mapping/ScriptStore.h"
#include "../audio/SampleStore.h"

/*
 * API audio — pilotage et MÉTROLOGIE.
 *
 * /api/audio/status est autant un outil de diagnostic qu'un état : il expose le
 * tas interne libre et surtout LE PLUS GROS BLOC CONTIGU. C'est ce dernier qui
 * décide si Plaits est portable ici — il lui faut 16 ko d'un seul tenant
 * (shared_buffer[16384]). Question restée ouverte au §12.7 de
 * CONVERGENCE_NIDMI.md faute d'une mesure sur S3 avec WiFi et serveur debout.
 */
void setupAudioAPI(AsyncWebServer& server) {

    server.on("/api/audio/status", HTTP_GET, [](AsyncWebServerRequest *request){
        const AudioEngine::Metriques m = AudioEngine::metriques();
        String json = "{";
        json += "\"started\":"            + String(m.demarre ? "true" : "false") + ",";
        json += "\"heap_free\":"          + String(m.heapLibre) + ",";
        json += "\"heap_largest_block\":" + String(m.heapPlusGrosBloc) + ",";
        json += "\"heap_before_init\":"   + String(m.heapAvantInit) + ",";
        json += "\"heap_after_init\":"    + String(m.heapApresInit) + ",";
        json += "\"psram_free\":"         + String(m.psramLibre) + ",";
        json += "\"sample_rate\":"        + String(m.sampleRateReel) + ",";
        json += "\"blocks\":"             + String(m.blocsRendus) + ",";
        json += "\"underruns\":"          + String(m.sousAlimentations) + ",";
        // Plaits demande 16 ko contigus, plus sa marge de manœuvre.
        json += "\"engine\":"             + String(m.moteur) + ",";
        json += "\"plaits_ready\":"       + String(m.plaitsPret ? "true" : "false") + ",";
        json += "\"plaits_bytes\":"       + String(m.plaitsOctets) + ",";
        json += "\"cycles_per_sample\":"  + String(m.cyclesParEch) + ",";
        // 5000 cycles/echantillon disponibles a 240 MHz et 48 kHz (MESURES.md).
        json += "\"load_percent\":" + String(m.cyclesParEch * 100.0f / 5000.0f, 1) + ",";
        json += "\"heap_min_ever\":"      + String(m.heapMiniJamais) + ",";
        json += "\"reset_reason\":\""      + String(m.causeResetTexte) + "\",";
        // Garde-fou du chargement au boot : combien de démarrages consécutifs
        // sans que l'interface ait pu être servie, et si le chargement est coupé.
        json += "\"boot_attempts\":"     + String(m.bootEssais) + ",";
        json += "\"boot_disabled\":"     + String(m.bootCoupe ? "true" : "false") + ",";
        json += "\"gated\":"             + String(m.silence ? "true" : "false") + ",";
        json += "\"niveau\":"            + String(m.niveau) + ",";
        json += "\"derniere_note\":"     + String(m.derniereNote) + ",";
        json += "\"engines_substitues\":\"" + String(AudioEngine::moteursSubstitues()) + "\",";
        // Le firmware expose SON seuil : l'UI ne doit pas en coder un en dur,
        // sinon le bouton promet ce que la carte refuse (le seuil dépend de la
        // taille du pool, donc de l'image — allégée ou complète).
        json += "\"switch_threshold\":" + String(m.seuilBascule) + ",";
        json += "\"plaits_fits\":" + String(m.heapPlusGrosBloc >= m.seuilBascule ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });

    /* Bip de test — la preuve la plus courte que la chaîne I2S marche.
     * POST /api/audio/test  (freq=440&ms=500)  ou  (note=60&ms=500) */
    server.on("/api/audio/test", HTTP_POST, [](AsyncWebServerRequest *request){
        uint32_t ms = 500;
        if (request->hasParam("ms", true)) ms = request->getParam("ms", true)->value().toInt();
        if (ms == 0 || ms > 5000) ms = 500;

        if (request->hasParam("note", true)) {
            const int n = request->getParam("note", true)->value().toInt();
            AudioEngine::noteOn((uint8_t)constrain(n, 0, 127), 100);
            request->send(200, "application/json",
                "{\"status\":\"ok\",\"note\":" + String(n) + "}");
            return;
        }
        float hz = 440.0f;
        if (request->hasParam("freq", true)) hz = request->getParam("freq", true)->value().toFloat();
        AudioEngine::testTone(hz, ms);
        request->send(200, "application/json",
            "{\"status\":\"ok\",\"freq\":" + String(hz, 1) + ",\"ms\":" + String(ms) + "}");
    });

    /* Choix du moteur : -1 = sinus interne, 0..15 = moteur Plaits.
     * L'allocation de Plaits (~24 ko de tas interne) est faite ici, pas au
     * boot : si elle echoue on reste au sinus et la carte ne bronche pas. */
    server.on("/api/audio/engine", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("engine", true)) {
            request->send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"engine parameter required (-1..23)\"}");
            return;
        }
        const int n = request->getParam("engine", true)->value().toInt();
        if (AudioEngine::setEngine(n, /*persister=*/true)) {
            request->send(200, "application/json",
                "{\"status\":\"ok\",\"engine\":" + String(AudioEngine::engine()) + "}");
        } else if (AudioEngine::derniereBascule() == AudioEngine::Bascule::Armee) {
            // 202 : la demande est acceptée mais pas appliquée maintenant. Le
            // choix est en NVS ; il sera chargé au prochain démarrage, sur un
            // tas vierge — le seul ordre d'allocation mesuré comme sûr.
            request->send(202, "application/json",
                "{\"status\":\"armed\",\"engine\":" + String(n) + ",\"message\":"
                "\"tas trop fragmente pour basculer a chaud — choix memorise, "
                "actif au prochain redemarrage\"}");
        } else {
            request->send(507, "application/json",
                "{\"status\":\"error\",\"message\":\"Plaits indisponible (tas insuffisant ?) — sinus conserve\"}");
        }
    });

    /* Réglages Plaits — la charge d'une CUE. Occasionnelle et courte, donc dans
     * la classe de requêtes qui ne creuse pas le tas (MESURES.md §10). Tous les
     * paramètres sont optionnels : on ne change que ce qui est envoyé.
     * Mêmes noms et mêmes plages que engines/core/plaits/web/index.js. */
    server.on("/api/audio/params", HTTP_POST, [](AsyncWebServerRequest *request){
        // ORDRE IMPORTANT : le moteur D'ABORD. setEngine() peut déclencher
        // l'allocation de Plaits, dont l'initialisation repose le patch sur ses
        // valeurs par défaut — appliquer les continus avant, c'est les perdre.
        if (request->hasParam("engine", true)) {
            const int n = request->getParam("engine", true)->value().toInt();
            // Chemin des CUES (js/device/audio-board.js) : on ne persiste pas.
            // Une cue change le son, elle ne redéfinit pas le défaut du boîtier.
            // Un refus de bascule n'est PAS un échec de la cue : les continus
            // qui suivent s'appliquent au process résident, et le spectacle
            // continue. On ne renvoie une erreur que si l'engine a vraiment
            // cassé. Le cas « armé » n'existe pas ici : une cue ne persiste rien.
            if (!AudioEngine::setEngine(n, /*persister=*/false)
                && AudioEngine::derniereBascule() != AudioEngine::Bascule::Armee) {
                request->send(507, "application/json",
                    "{\"status\":\"error\",\"message\":\"moteur indisponible (tas insuffisant ?)\"}");
                return;
            }
        }

        AudioEngine::Params p = AudioEngine::params();
        auto lire = [&](const char* nom, float& dest){
            if (request->hasParam(nom, true))
                dest = request->getParam(nom, true)->value().toFloat();
        };
        lire("harmonics",  p.harmonics);
        lire("timbre",     p.timbre);
        lire("morph",      p.morph);
        lire("decay",      p.decay);
        lire("lpg_colour", p.lpgColour);
        AudioEngine::setParams(p);
        const AudioEngine::Params a = AudioEngine::params();
        String json = "{\"status\":\"ok\",\"engine\":" + String(AudioEngine::engine());
        json += ",\"harmonics\":"  + String(a.harmonics, 3);
        json += ",\"timbre\":"     + String(a.timbre, 3);
        json += ",\"morph\":"      + String(a.morph, 3);
        json += ",\"decay\":"      + String(a.decay, 3);
        json += ",\"lpg_colour\":" + String(a.lpgColour, 3) + "}";
        request->send(200, "application/json", json);
    });

    /* ── Échantillons ────────────────────────────────────────────────────
     * mapfs (1 Mo) était partitionnée mais jamais montée — §12.5 de
     * CONVERGENCE_NIDMI.md. Elle sert enfin. Le fichier persiste en flash ; la
     * lecture se fait depuis la PSRAM, inutilisée jusqu'ici (8,37 Mo) pendant
     * que le tas interne se bat pour 13 ko. */

    server.on("/api/audio/samples", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"mounted\":" + String(SampleStore::monter() ? "true" : "false");
        json += ",\"total\":"  + String(SampleStore::espaceTotal());
        json += ",\"used\":"   + String(SampleStore::espaceUtilise());
        json += ",\"loaded\":\"" + String(AudioEngine::samplerNom()) + "\"";
        json += ",\"psram_bytes\":" + String(SampleStore::octetsPsram());
        json += ",\"files\":" + SampleStore::listerJson() + "}";
        request->send(200, "application/json", json);
    });

    /* Téléversement d'un WAV : corps BRUT, comme /api/ota — le nom passe en
     * paramètre d'URL. PCM 16 bits mono ou stéréo. */
    server.on("/api/audio/sample", HTTP_POST,
        [](AsyncWebServerRequest *request){
            SampleStore::ecrireFin();
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        },
        nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            if (index == 0) {
                String nom = request->hasParam("name")
                           ? request->getParam("name")->value() : String("sample.wav");
                if (!SampleStore::ecrireDebut(nom.c_str())) return;
            }
            SampleStore::ecrireMorceau(data, len);
        });

    /* Suppression d'un échantillon. Si c'est celui qui est chargé, on arrête
     * d'abord le lecteur : sinon la PSRAM garderait des données orphelines et la
     * NVS pointerait sur un fichier absent. */
    server.on("/api/audio/sample", HTTP_DELETE, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name")) {
            request->send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"parametre name requis\"}");
            return;
        }
        const String nom = request->getParam("name")->value();
        if (nom == String(AudioEngine::samplerNom())) {
            AudioEngine::arreterSampler(/*persister=*/true);
        }
        if (SampleStore::supprimer(nom.c_str())) {
            request->send(200, "application/json",
                "{\"status\":\"ok\",\"deleted\":\"" + nom + "\"}");
        } else {
            request->send(404, "application/json",
                "{\"status\":\"error\",\"message\":\"fichier introuvable\"}");
        }
    });

    /* Choix de l'échantillon à jouer. name vide = on arrête le lecteur. */
    server.on("/api/audio/sampler", HTTP_POST, [](AsyncWebServerRequest *request){
        const String nom = request->hasParam("name", true)
                         ? request->getParam("name", true)->value() : String("");
        if (!nom.length()) {
            AudioEngine::arreterSampler(/*persister=*/true);   // action humaine
            request->send(200, "application/json", "{\"status\":\"ok\",\"sampler\":\"\"}");
            return;
        }
        String raison;
        if (AudioEngine::setSampler(nom.c_str(), raison, /*persister=*/true)) {
            request->send(200, "application/json",
                "{\"status\":\"ok\",\"sampler\":\"" + nom + "\"}");
        } else {
            request->send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"" + raison + "\"}");
        }
    });

    /* Extinction — utile quand un noteOn de test reste accroché. */
    /* PLAY : ouvre la porte de silence. Symetrique de /api/audio/stop, et
     * volontairement SANS reallocation — le moteur reste charge, pour que le
     * play suivant reparte instantanement. */
    /* Script .nms applique au MIDI ENTRANT, par la CARTE. Le navigateur ne fait
     * que POUSSER le script — il ne l'execute jamais (regle du headless : tout
     * est fait dans la carte). Corps = le script, vide = passage direct. */
    /* Les scripts .nms vivent dans mapfs — la partition prevue pour eux
     * (« scripts de mapping », table de partitions). Le moteur de script est le
     * COEUR du boitier : une carte peut n'avoir que des .nms et des cues, sans
     * aucun audio. Ces routes sont donc de l'image de base, pas un accessoire. */
    server.on("/api/midi/scripts", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "application/json",
                      "{\"actif\":\"" + g_midiRouter.nomScript() + "\",\"fichiers\":"
                      + ScriptStore::listerJson() + "}");
    });

    /* Depose un script dans mapfs. name = nom du fichier, script = contenu. */
    server.on("/api/midi/scripts", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name", true)) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"message\":\"parametre name requis\"}");
            return;
        }
        const String nom = request->getParam("name", true)->value();
        String contenu;
        if (request->hasParam("script", true)) contenu = request->getParam("script", true)->value();
        if (!ScriptStore::ecrire(nom.c_str(), contenu)) {
            request->send(507, "application/json",
                          "{\"status\":\"error\",\"message\":\"ecriture impossible\"}");
            return;
        }
        // Si c'est le script ACTIF qu'on vient de reecrire, on le recharge.
        if (g_midiRouter.nomScript() == nom) g_midiRouter.chargerScriptNomme(nom.c_str(), false);
        request->send(200, "application/json",
                      "{\"status\":\"ok\",\"name\":\"" + nom + "\"}");
    });

    server.on("/api/midi/scripts", HTTP_DELETE, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name")) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"message\":\"parametre name requis\"}");
            return;
        }
        const String nom = request->getParam("name")->value();
        request->send(ScriptStore::supprimer(nom.c_str()) ? 200 : 404, "application/json",
                      "{\"status\":\"ok\"}");
    });

    /* Choisit le script ACTIF, par nom. persist=1 le memorise en NVS : la carte
     * le rechargera seule au demarrage, sans navigateur. Seul le NOM est
     * persiste — le contenu reste dans LittleFS. */
    server.on("/api/midi/script/select", HTTP_POST, [](AsyncWebServerRequest *request){
        const String nom = request->hasParam("name", true)
                         ? request->getParam("name", true)->value() : String("");
        const bool persister = request->hasParam("persist", true)
                            && request->getParam("persist", true)->value() != "0";
        const bool ok = g_midiRouter.chargerScriptNomme(nom.c_str(), persister);
        request->send(ok ? 200 : 404, "application/json",
                      String("{\"status\":\"") + (ok ? "ok" : "introuvable")
                      + "\",\"actif\":\"" + g_midiRouter.nomScript() + "\"}");
    });

    server.on("/api/midi/script", HTTP_POST, [](AsyncWebServerRequest *request){
        String sc;
        if (request->hasParam("script", true)) sc = request->getParam("script", true)->value();
        g_midiRouter.setScriptMidi(sc);
        request->send(200, "application/json",
                      String("{\"status\":\"ok\",\"len\":") + sc.length() + "}");
    });

    server.on("/api/audio/resume", HTTP_POST, [](AsyncWebServerRequest *request){
        AudioEngine::ouvrirSon();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    server.on("/api/audio/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        // couperSon() ferme la porte de silence : Plaits rend en continu et
        // ignore le note-off (son LPG gere l'extinction), donc les note-off
        // ci-dessous ne le taisent pas — la porte, si. Elle rouvre a la note
        // suivante (jeu live apres un STOP).
        AudioEngine::couperSon();
        for (int n = 0; n < 128; n++) AudioEngine::noteOff((uint8_t)n);
        AudioEngine::testTone(0.0f, 0);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
}
