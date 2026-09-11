#include "../config/Occupations.h"
#include "../managers/ComponentManager.h"
#include "../Globals.h"
#include "APICommon.h"
#include "../audio/AudioEngine.h"
#include "../midi/MidiRouter.h"
#include "../midi/CcMap.h"
#include "../mapping/MappingEngine.h"
#include "../mapping/VocabulaireEmbarque.h"
#include "../mapping/ScriptStore.h"
#include "../mapping/CueStore.h"
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
        /* Marges de PILE, en octets. Celle de la tache MIDI est relevee par
         * elle-meme ; celle-ci est mesuree ici meme, donc c'est celle du
         * serveur web. Les deux portent les tableaux de sortie du moteur. */
        extern uint32_t g_margePileMidi;
        json += "\"stack_midi_libre\":"   + String(g_margePileMidi) + ",";
        json += "\"stack_web_libre\":"    +
                String((uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)) + ",";
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
        /* DIRE LA VRAIE RAISON.
         *
         * ensureStarted() refuse de demarrer quand une broche du bus audio
         * porte un composant, mais setEngine() ne rend qu'un echec generique :
         * l'API repondait « Plaits indisponible (tas insuffisant ?) » pour une
         * broche occupee. Un message faux coute plus cher qu'une absence de
         * message — on verifie donc ici, avant, pour pouvoir nommer la cause. */
        if (n >= 0 && !Occupations::audioDeclare()) {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"message\":\"Aucun DAC declare : le son est desactive. "
                "Declarer « DAC audio (I2S) » sur la broche D0 dans la zone I/O.\"}");
            return;
        }
        /* Plus de garde « une broche audio porte un composant » ici : une fois le
         * DAC declare, /api/pins/set refuse tout autre composant sur ses trois
         * broches, donc le seul composant qui peut s'y trouver est le DAC
         * lui-meme — et la garde se declenchait justement sur lui. La
         * declaration ci-dessus est la condition, et elle suffit. */
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
        /* Le VOLUME ne vit pas dans Params : c'est un gain de sortie, pas un
         * parametre de Plaits. Il voyage sur la meme route parce qu'il change
         * pour les memes raisons — une cue qui rappelle son etat. */
        if (request->hasParam("volume", true))
            AudioEngine::setVolume(request->getParam("volume", true)->value().toFloat());
        const AudioEngine::Params a = AudioEngine::params();
        String json = "{\"status\":\"ok\",\"engine\":" + String(AudioEngine::engine());
        json += ",\"harmonics\":"  + String(a.harmonics, 3);
        json += ",\"timbre\":"     + String(a.timbre, 3);
        json += ",\"morph\":"      + String(a.morph, 3);
        json += ",\"decay\":"      + String(a.decay, 3);
        json += ",\"lpg_colour\":" + String(a.lpgColour, 3);
        json += ",\"volume\":"     + String(AudioEngine::volume(), 3) + "}";
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
        /* ── CUES SUR LA CARTE ──────────────────────────────────────────────
     * Une carte deployee n'a pas de navigateur pour lui dire quelle cue jouer.
     * Elle tient sa liste (mapfs:/cues.txt), la parcourt et applique elle-meme
     * ce que chaque cue decrit. L'app devient un outil d'ECRITURE de cette
     * liste, pas un maillon de son execution.
     *
     * ⚠️ ORDRE D'ENREGISTREMENT CRITIQUE. ESPAsyncWebServer fait du PREFIXE,
     * pas de l'exact : un handler "/api/cues" intercepte aussi
     * "/api/cues/play". Enregistre en premier, il a avale les commandes de
     * transport — un POST /api/cues/play a ete traite comme un envoi de liste
     * et a EFFACE les cues. Les routes SPECIFIQUES passent donc d'abord. */
    server.on("/api/cues/play", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::demarrer();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.on("/api/cues/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::arreter();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.on("/api/cues/go", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::suivant();
        request->send(200, "application/json",
                      String("{\"status\":\"ok\",\"index\":") + Cues::indexCourant() + "}");
    });
    server.on("/api/cues/goto", HTTP_POST, [](AsyncWebServerRequest *request){
        const int i = request->hasParam("i", true)
                    ? request->getParam("i", true)->value().toInt() : 0;
        const bool ok = Cues::aller(i);
        request->send(ok ? 200 : 404, "application/json",
                      String("{\"status\":\"") + (ok ? "ok" : "hors liste")
                      + "\",\"index\":" + Cues::indexCourant() + "}");
    });
    server.on("/api/cues/texte", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain; charset=utf-8", Cues::contenu());
    });
    server.on("/api/cues", HTTP_GET, [](AsyncWebServerRequest *request){
        String j = "{\"n\":" + String(Cues::nombre())
                 + ",\"index\":" + String(Cues::indexCourant())
                 + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                 + ",\"restant\":" + String(Cues::restantSec(), 2) + "}";
        request->send(200, "application/json", j);
    });
    server.on("/api/cues", HTTP_POST, [](AsyncWebServerRequest *request){
        /* `cues` ABSENT = on NE TOUCHE PAS a la liste.
         *
         * L'absence valait chaine vide, donc EFFACAIT tout : une requete mal
         * formee — un nom de parametre errone, par exemple — repondait 200 et
         * rendait une liste vide. Constate en direct, sur les cues de l'usager.
         * Exactement le defaut deja corrige sur /api/midi/script ; il etait
         * reste ici. Pour EFFACER, on envoie donc `cues` explicitement vide. */
        if (!request->hasParam("cues", true)) {
            request->send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"parametre 'cues' absent — "
                "rien n'a ete modifie. Envoyer 'cues' vide pour effacer.\"}");
            return;
        }
        const String texte = request->getParam("cues", true)->value();
        const bool ok = Cues::ecrireTout(texte);
        request->send(ok ? 200 : 507, "application/json",
                      String("{\"status\":\"") + (ok ? "ok" : "error")
                      + "\",\"n\":" + Cues::nombre() + "}");
    });

server.on("/api/midi/scripts", HTTP_GET, [](AsyncWebServerRequest *request){
        /* `actif` : le nom de l'emplacement 0 — conserve pour ne pas casser
         * les clients existants. `emplacements` dit ce que TOUS portent, ce
         * qu'un seul nom ne pouvait pas exprimer. */
        String emps = "[";
        for (uint8_t e = 0; e < MidiRouter::MAX_SCRIPTS_MAP; e++) {
            if (e) emps += ",";
            emps += "\"" + g_midiRouter.nomEmplacement(e) + "\"";
        }
        emps += "]";
        request->send(200, "application/json",
                      "{\"actif\":\"" + g_midiRouter.nomScript() + "\",\"emplacements\":" + emps
                      + ",\"fichiers\":" + ScriptStore::listerJson() + "}");
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
    /* Ce que la CHAINE des emplacements fait d'un CC entrant, sans rien emettre.
     * Le pendant de /api/mapping/essai pour le routage : celle-la eprouve un
     * script isole, celle-ci la chaine complete telle qu'elle tourne. */
    server.on("/api/midi/chaine", HTTP_POST, [](AsyncWebServerRequest *request){
        auto par = [&](const char* n, int d) -> int {
            return request->hasParam(n, true) ? request->getParam(n, true)->value().toInt() : d;
        };
        uint8_t c = (uint8_t)par("ch", 1), n = (uint8_t)par("cc", 0), v = (uint8_t)par("val", 0);
        const bool passe = g_midiRouter.chaineScriptsCc(c, n, v);
        request->send(200, "application/json",
            String("{\"passe\":") + (passe ? "true" : "false")
            + ",\"ch\":" + String((int)c) + ",\"cc\":" + String((int)n)
            + ",\"val\":" + String((int)v) + "}");
    });

    server.on("/api/midi/script/select", HTTP_POST, [](AsyncWebServerRequest *request){
        const String nom = request->hasParam("name", true)
                         ? request->getParam("name", true)->value() : String("");
        const bool persister = request->hasParam("persist", true)
                            && request->getParam("persist", true)->value() != "0";
        const uint8_t emp = request->hasParam("slot", true)
                          ? (uint8_t)request->getParam("slot", true)->value().toInt() : 0;
        const bool ok = g_midiRouter.chargerScriptNomme(nom.c_str(), persister, emp);
        request->send(ok ? 200 : 404, "application/json",
                      String("{\"status\":\"") + (ok ? "ok" : "introuvable")
                      + "\",\"actif\":\"" + g_midiRouter.nomScript() + "\"}");
    });

    server.on("/api/midi/script", HTTP_POST, [](AsyncWebServerRequest *request){
        // `script` ABSENT = on ne touche pas au code. Auparavant l'absence
        // valait chaine vide et EFFACAIT le script : impossible d'envoyer les
        // seuls reglages. Or un tour de potentiometre en produit une douzaine,
        // et les faire porter le .nms entier (jusqu'a 8 ko) noyait une carte
        // dont le plus grand bloc contigu descend sous 15 ko — elle acceptait
        // la requete sans plus servir de reponse. Pour EFFACER le script, on
        // envoie donc `script` explicitement vide.
        const uint8_t emp = request->hasParam("slot", true)
                          ? (uint8_t)request->getParam("slot", true)->value().toInt() : 0;
        if (request->hasParam("script", true))
            g_midiRouter.setScriptMidi(request->getParam("script", true)->value(), emp);
        // Les REGLAGES du script. Sans eux, r("param","nom",min,max,defaut)
        // retombe sur son defaut et le script parait inerte : c'est ce qui
        // rendait un bloc transpose sans effet alors que son code etait bien
        // charge et correctement interprete.
        if (request->hasParam("params", true))
            g_midiRouter.setParamsScript(request->getParam("params", true)->value());
        request->send(200, "application/json",
                      String("{\"status\":\"ok\",\"len\":")
                          + g_midiRouter.scriptMidi().length() + "}");
    });

    /* ── CE QUE CETTE CARTE-CI EXECUTE ───────────────────────────────────
     * L'app colorie les scripts de broche et previent quand un objet « n'est
     * pas execute par la carte ». Elle le SAVAIT EN DUR : une liste de dix
     * noms ecrite a la main, figee au temps ou le moteur n'en executait que
     * dix, si bien qu'elle signalait comme inexistants des objets parfaitement
     * executes ici — `in` et `graph`, mesure faite.
     *
     * Une liste recopiee derive, et une liste copiee DEPUIS UN AUTRE DEPOT
     * decrit de surcroit un firmware de reference, pas LA carte branchee, qui
     * peut tourner une version plus ancienne. Donc la carte le dit elle-meme.
     * C'est la regle du projet — la page montre l'etat exact de ce qui tourne —
     * appliquee au langage : l'app ne suppose plus, elle demande.
     *
     * La reponse est un litteral en flash (VocabulaireEmbarque.h, genere par
     * scripts/generer-vocabulaire.py depuis MappingEngine.cpp) : la route ne
     * construit rien, pas un octet de tas pris a AsyncTCP.                    */
    /* ── GIGUE D'ORDONNANCEMENT ───────────────────────────────────────────
     * « Si quelque chose doit ralentir, c'est l'UI web — jamais le MIDI »
     * (CONVERGENCE §1.5). Or le serveur web tourne a la priorite 10 et les
     * taches temps reel a 5 (ADC) et 4 (MIDI) : il les preempte PAR
     * CONSTRUCTION. L'inversion est certaine ; le prejudice, lui, se mesure.
     *
     * Les deux taches ont une periode fixe (vTaskDelayUntil). On publie donc
     * l'ECART a cette periode : pire cas, moyenne, et nombre de tours ou
     * l'ecart depasse la moitie de la periode. `?reset=1` ouvre une fenetre
     * propre — mesurer au repos, puis sous charge, et comparer.
     *
     * La lecture elle-meme est une requete HTTP, donc une charge : c'est
     * pourquoi on RESET avant la charge et on LIT apres, jamais pendant.   */
    server.on("/api/diag/gigue", HTTP_GET, [](AsyncWebServerRequest *request){
        extern volatile uint32_t g_gigueMidiMaxUs, g_gigueMidiTours,
                                 g_gigueMidiRetards, g_gigueMidiCumulUs;
        extern volatile uint32_t g_gigueMuxMaxUs, g_gigueMuxTours,
                                 g_gigueMuxRetards, g_gigueMuxCumulUs;
        extern void nidmi_gigue_midi_reset();
        extern void nidmi_gigue_mux_reset();
        if (request->hasParam("reset")) {
            nidmi_gigue_midi_reset();
            nidmi_gigue_mux_reset();
            request->send(200, "application/json", "{\"reset\":true}");
            return;
        }
        const uint32_t nMidi = g_gigueMidiTours, nMux = g_gigueMuxTours;
        String json = "{";
        /* CE gestionnaire s'execute DANS la tache async_tcp : il peut donc dire
         * sa propre priorite et son coeur. Un reglage qu'on croit pose et qui ne
         * l'est pas est pire que pas de reglage — ici la carte le prouve
         * elle-meme, au lieu qu'on deduise d'un drapeau de compilation. */
        json += "\"web\":{\"tache\":\"" + String(pcTaskGetName(nullptr)) + "\"";
        json += ",\"priorite\":"  + String((unsigned)uxTaskPriorityGet(nullptr));
        json += ",\"coeur\":"     + String((int)xPortGetCoreID());
        json += "},\"temps_reel\":{\"mux\":5,\"midi\":4,\"audio\":11},";
        json += "\"midi\":{\"periode_us\":10000";
        json += ",\"tours\":"     + String(nMidi);
        json += ",\"max_us\":"    + String(g_gigueMidiMaxUs);
        json += ",\"moy_us\":"    + String(nMidi ? (g_gigueMidiCumulUs / nMidi) : 0);
        json += ",\"retards\":"   + String(g_gigueMidiRetards) + "},";
        json += "\"mux\":{\"periode_us\":5000";
        json += ",\"tours\":"     + String(nMux);
        json += ",\"max_us\":"    + String(g_gigueMuxMaxUs);
        json += ",\"moy_us\":"    + String(nMux ? (g_gigueMuxCumulUs / nMux) : 0);
        json += ",\"retards\":"   + String(g_gigueMuxRetards) + "}}";
        request->send(200, "application/json", json);
    });

    server.on("/api/mapping/vocabulaire", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", VOCABULAIRE_EMBARQUE_JSON);
    });

    /* ── Essai a blanc d'un script .nms ───────────────────────────────────
     * Execute un script sur un evenement DONNE et rend ce qu'il emettrait,
     * sans rien jouer ni envoyer. Deux usages :
     *   - mettre au point un mapping sans cabler de potentiometre ;
     *   - rejouer sur la CARTE la suite de conformite du banc
     *     (hardware/bench/nms/), qui ne tournait que sur le poste. C'est ce qui
     *     transforme « conforme au moteur web » en « conforme SUR LA CIBLE » —
     *     le banc hote compile avec un ersatz d'Arduino, pas avec le vrai.
     * L'etat de pipeline est le SIEN : essayer un script ne derange pas celui
     * qui tourne.                                                            */
    server.on("/api/mapping/essai", HTTP_POST, [](AsyncWebServerRequest *request){
        /* `in` et `raw` : les ENTREES du composant et leurs lectures natives,
         * en virgules. Elles ne passent plus par le registre — celui-ci est un
         * bus PARTAGE, et in(n)/raw.in(n) lisent ce que le composant donne. */
        static MappingEngine::Etat etats[8];
        auto par = [&](const char* n) -> String {
            return request->hasParam(n, true) ? request->getParam(n, true)->value() : String("");
        };
        /* Le texte du script doit SURVIVRE a la requete : un differe garde un
         * pointeur dessus et ne sera rejoue qu'a la requete suivante. On le
         * garde donc ici, et on oublie les reprises de l'ancien AVANT de le
         * remplacer — sinon on rejouerait sur de la memoire rendue. */
        static String scriptEssai;
        const String demande = par("script");
        if (demande != scriptEssai) {
            MappingEngine::viderDifferes(scriptEssai.c_str());
            scriptEssai = demande;
        }
        if (par("reset") == "1") {
            for (auto& e : etats) e.reinitialiser();
            MappingEngine::viderDifferes(scriptEssai.c_str());
        }
        // genre=reset : on remet a zero et on N'EXECUTE PAS. Sans ce
        // court-circuit, la requete de remise a zero jouait aussi un evenement
        // et faisait avancer toggle/seq/counter d'un cran avant meme le premier
        // cas — decalage constate en rejouant la suite sur la carte.
        if (par("genre") == "reset") {
            request->send(200, "application/json", "{\"reset\":true,\"out\":[]}");
            return;
        }

        MappingEngine::Evenement ev;
        const String genre = par("genre");
        const int a = par("a").toInt(), b = par("b").toInt(), c = par("c").toInt();
        if      (genre == "note")    { ev.type = MappingEngine::Evenement::NoteOn;    ev.a = a; ev.b = b; }
        else if (genre == "noteoff") { ev.type = MappingEngine::Evenement::NoteOff;   ev.a = a; ev.b = b; }
        else if (genre == "cc")      { ev.type = MappingEngine::Evenement::Cc;        ev.a = a; ev.b = b; }
        else if (genre == "bend")    { ev.type = MappingEngine::Evenement::Bend;      ev.valeur14 = a; }
        else if (genre == "touch")   { ev.type = MappingEngine::Evenement::Touch;     ev.a = a; }
        else if (genre == "ptouch")  { ev.type = MappingEngine::Evenement::PolyTouch; ev.a = a; ev.b = b; }
        else if (genre == "pgm")     { ev.type = MappingEngine::Evenement::Pgm;       ev.a = a; }
        // Les sources d'horloge : « tick » porte l'instant dans `a`, « init »
        // tire loadbang(). Sans elles, le banc ne pouvait pas prouver metro().
        else if (genre == "tick")    { ev.type = MappingEngine::Evenement::Tick; ev.instant = (uint32_t)a; }
        else if (genre == "init")    { ev.type = MappingEngine::Evenement::Init; }
        /* « osc » : un message OSC entrant, pour eprouver osc.in(). L'adresse
         * arrive en clair ; le premier argument est lu en FLOTTANT, un OSC ne
         * comptant pas en entiers de 0 a 127. */
        else if (genre == "osc") {
            ev.type = MappingEngine::Evenement::Osc;
            ev.reel = par("a").toFloat();
            snprintf(ev.adresse, sizeof ev.adresse, "%s", par("adresse").c_str());
        }
        else if (genre == "capteur") {
            ev.type = MappingEngine::Evenement::Capteur;
            snprintf(ev.origine, sizeof ev.origine, "essai");
            auto listeFlottants = [](const String& t, float* out, int max) -> int {
                int n = 0, i = 0;
                while (i <= (int)t.length() && n < max) {
                    int j = t.indexOf(',', i);
                    if (j < 0) j = t.length();
                    if (j > i) out[n++] = t.substring(i, j).toFloat();
                    i = j + 1;
                }
                return n;
            };
            float vs[MappingEngine::MAX_INLETS], rs[MappingEngine::MAX_INLETS];
            int nv = listeFlottants(par("in"), vs, MappingEngine::MAX_INLETS);
            if (!nv) { vs[0] = par("a").toFloat(); nv = 1; }
            const int nr = listeFlottants(par("raw"), rs, MappingEngine::MAX_INLETS);
            ev.nInlets = (uint8_t)nv;
            for (int i = 0; i < nv; i++) {
                ev.inlets[i] = vs[i];
                ev.raws[i]   = (i < nr) ? rs[i] : vs[i];
            }
        }
        else {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"message\":\"genre inconnu\"}");
            return;
        }
        if (ev.type != MappingEngine::Evenement::Capteur) ev.canal = (uint8_t)c;

        /* Rend une sortie OSC sous la meme forme que le banc hote :
         * les arguments en texte, a trois decimales, pour que deux moteurs qui
         * ne comptent pas dans le meme flottant restent comparables. */
        auto oscEnJson = [](const MappingEngine::Sortie& s) -> String {
            String args = String(s.reel, 3);
            for (int k = 0; k < s.nArgsSup; k++) args += "," + String(s.argsSup[k], 3);
            return String("{\"t\":\"osc\",\"a\":\"") + s.adresse
                 + "\",\"b\":\"" + args + "\",\"c\":" + String((int)s.hote) + "}";
        };
        MappingEngine::Sortie liste[MappingEngine::MAX_SORTIES];
        bool traite = false;
        // Un COMPOSANT n'a qu'UN slot d'etat pour tous ses pipelines (il vit
        // dans ComponentState, et il y a jusqu'a 64 composants). L'essai doit
        // donc en donner un seul lui aussi, sinon il repondrait mieux que la
        // realite et un script a deux « toggle() » paraitrait correct ici tout
        // en se marchant dessus sur la carte.
        const int slots = (genre == "capteur") ? 1 : 8;
        int n = MappingEngine::executer(scriptEssai.c_str(), ev, liste,
                                              MappingEngine::MAX_SORTIES, traite,
                                              etats, slots, /*horsLigne=*/true);
        /* Un battement vide aussi la file des DIFFERES — del(), makenote()...
         * Sans ca ces verbes seraient invisibles a l'essai : leur sortie ne
         * vient pas de l'appel qui les a rencontres, mais d'un battement
         * ulterieur. Le banc hote fait exactement pareil. */
        if (ev.type == MappingEngine::Evenement::Tick && n < MappingEngine::MAX_SORTIES)
            n += MappingEngine::battreDifferes(ev.instant, liste + n,
                                               MappingEngine::MAX_SORTIES - n,
                                               /*horsLigne=*/true);
        // PASSAGE : quand aucun pipeline n'a pris l'evenement en charge, il
        // ressort tel quel — c'est ce que fait MidiRouter a partir de `traite`,
        // et ce que fait le moteur web. La route le montre donc aussi, sans
        // quoi elle repondrait « rien » la ou la carte laisse passer.
        // Quirk du moteur web reproduit : pour une note, le canal est omis (0).
        String passage;
        if (!traite) {
            switch (ev.type) {
                case MappingEngine::Evenement::NoteOn:
                    passage = String("{\"t\":\"note\",\"a\":") + ev.a + ",\"b\":" + ev.b + ",\"c\":0}"; break;
                case MappingEngine::Evenement::NoteOff:
                    passage = String("{\"t\":\"noteoff\",\"a\":") + ev.a + ",\"b\":0,\"c\":0}"; break;
                case MappingEngine::Evenement::Cc:
                    passage = String("{\"t\":\"cc\",\"a\":") + ev.a + ",\"b\":" + ev.b + ",\"c\":" + ev.canal + "}"; break;
                case MappingEngine::Evenement::Bend:
                    passage = String("{\"t\":\"bend\",\"a\":") + ev.valeur14 + ",\"c\":" + ev.canal + "}"; break;
                case MappingEngine::Evenement::Touch:
                    passage = String("{\"t\":\"touch\",\"a\":") + ev.a + ",\"b\":0,\"c\":" + ev.canal + "}"; break;
                case MappingEngine::Evenement::PolyTouch:
                    passage = String("{\"t\":\"ptouch\",\"a\":") + ev.a + ",\"b\":" + ev.b + ",\"c\":" + ev.canal + "}"; break;
                case MappingEngine::Evenement::Pgm:
                    passage = String("{\"t\":\"pgm\",\"a\":") + ev.a + ",\"b\":0,\"c\":" + ev.canal + "}"; break;
                default: break;
            }
        }

        String j = String("{\"traite\":") + (traite ? "true" : "false") + ",\"out\":[";
        j += passage;
        for (int i = 0; i < n; i++) {
            if (i || passage.length()) j += ",";
            const MappingEngine::Sortie& o = liste[i];
            const char* t = "note";
            switch (o.type) {
                case MappingEngine::Sortie::Note:      t = "note";    break;
                case MappingEngine::Sortie::NoteOff:   t = "noteoff"; break;
                case MappingEngine::Sortie::Cc:        t = "cc";      break;
                case MappingEngine::Sortie::Bend:      t = "bend";    break;
                case MappingEngine::Sortie::Touch:     t = "touch";   break;
                case MappingEngine::Sortie::PolyTouch: t = "ptouch";  break;
                case MappingEngine::Sortie::Pgm:       t = "pgm";     break;
                case MappingEngine::Sortie::Print:     t = "print";   break;
                case MappingEngine::Sortie::Osc:       t = "osc";     break;
            }
            if (o.type == MappingEngine::Sortie::Osc)
                j += oscEnJson(o);
            else if (o.type == MappingEngine::Sortie::Bend)
                j += String("{\"t\":\"bend\",\"a\":") + o.valeur14 + ",\"c\":" + o.canal + "}";
            else
                j += String("{\"t\":\"") + t + "\",\"a\":" + o.a + ",\"b\":" + o.b
                   + ",\"c\":" + o.canal + "}";
        }
        j += "]}";
        request->send(200, "application/json", j);
    });

    /* ── Table CC -> parametre ────────────────────────────────────────────
     * Le CC learn vivait entierement dans le navigateur : il cessait des
     * qu'on le debranchait. La table vit maintenant dans la carte, en NVS.
     *
     * ORDRE : /api/midi/cc/learn AVANT /api/midi/cc — ESPAsyncWebServer fait
     * du prefixe, pas de l'exact, et le generique avalerait la commande
     * (meme piege que /api/cues, qui avait efface la liste des cues).       */
    server.on("/api/midi/cc/learn", HTTP_POST, [](AsyncWebServerRequest *request){
        String cible;
        if (request->hasParam("cible", true)) cible = request->getParam("cible", true)->value();
        if (!cible.length()) {
            CcMap::desarmer();
            request->send(200, "application/json", "{\"status\":\"ok\",\"arme\":\"\"}");
            return;
        }
        // Bornes de la cible. Un parametre continu de Plaits vit dans 0..1, un
        // parametre de script dans ce que declare son r("param",…) — c'est
        // l'app qui sait, elle les envoie. Sans elles on etalerait tout dans
        // 0..1 et « semitones » ne bougerait jamais que d'un demi-ton.
        float mn = 0.0f, mx = 1.0f;
        if (request->hasParam("min", true)) mn = request->getParam("min", true)->value().toFloat();
        if (request->hasParam("max", true)) mx = request->getParam("max", true)->value().toFloat();
        CcMap::armer(cible.c_str(), mn, mx);
        request->send(200, "application/json",
                      String("{\"status\":\"ok\",\"arme\":\"") + cible + "\"}");
    });

    server.on("/api/midi/cc", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"table\":\"" + CcMap::texte() + "\"";
        json += ",\"arme\":\"" + String(CcMap::cibleArmee()) + "\"";
        json += ",\"max\":" + String(CcMap::MAX) + "}";
        request->send(200, "application/json", json);
    });

    server.on("/api/midi/cc", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("table", true)) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"message\":\"parametre 'table' manquant\"}");
            return;
        }
        CcMap::setTexte(request->getParam("table", true)->value(), /*persister=*/true);
        request->send(200, "application/json",
                      String("{\"status\":\"ok\",\"table\":\"") + CcMap::texte() + "\"}");
    });

    server.on("/api/midi/cc", HTTP_DELETE, [](AsyncWebServerRequest *request){
        CcMap::vider(/*persister=*/true);
        request->send(200, "application/json", "{\"status\":\"ok\",\"table\":\"\"}");
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
