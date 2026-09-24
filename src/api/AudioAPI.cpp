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
#include "../mapping/CompoStore.h"
#include "../audio/SampleStore.h"
#include "../server/ServerCallbacks.h"   // demandeur, a vide, sante
#include "../server/ServerCore.h"        // serverCore.usbMidi() : le banc MIDI USB
#include <nvs.h>
#include <esp_heap_caps.h>   // le bloc contigu : le reservoir qui predit la panne
#include <memory>          // la carte du tas garde son texte jusqu'au dernier envoi
#include <esp_timer.h>

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
        // dont ceux d'une ecriture flash faite en silence : inaudibles (§156)
        json += "\"underruns_ecritures\":" + String(m.retardsEcritures) + ",";
        // Plaits demande 16 ko contigus, plus sa marge de manœuvre.
        json += "\"engine\":"             + String(m.moteur) + ",";
        json += "\"plaits_ready\":"       + String(m.plaitsPret ? "true" : "false") + ",";
        json += "\"plaits_bytes\":"       + String(m.plaitsOctets) + ",";
        json += "\"cycles_per_sample\":"  + String(m.cyclesParEch) + ",";
        // 5000 cycles/echantillon disponibles a 240 MHz et 48 kHz (MESURES.md).
        json += "\"load_percent\":" + String(m.cyclesParEch * 100.0f / 5000.0f, 1) + ",";
        json += "\"heap_min_ever\":"      + String(m.heapMiniJamais) + ",";
        json += "\"reset_reason\":\""      + String(m.causeResetTexte) + "\",";
        /* QUI a demande ce redemarrage — vide s'il n'a ete demande par personne
         * (coupure de courant, panique, chien de garde). Et s'il est a vide. */
        {
            String par = nidmi_redemarrageDemandePar();
            par.replace("\\", "\\\\"); par.replace("\"", "\\\"");
            json += "\"redemarrage_demande_par\":\"" + par + "\",";
            json += "\"demarre_a_vide\":" + String(nidmi_demarreAVide() ? "true" : "false") + ",";
        }
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
        /* CE QUE CETTE CARTE SAIT FAIRE — l'app demande, elle ne suppose pas.
         * `false` ici veut dire : les blocs de synthese ne sonneront pas sur
         * elle ; les echantillons, si. */
        json += "\"synthese\":" + String(AudioEngine::syntheseLourdeDisponible() ? "true" : "false") + ",";
        json += "\"sampler_oncue\":" + String(AudioEngine::declenchementSurCue() ? "true" : "false") + ",";
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
        /* PAS DE SYNTHESE SUR UNE CARTE SEULE — et on dit POURQUOI, avec ce
         * qui reste. Un refus sans alternative envoie chercher une panne qui
         * n'existe pas. */
        /* -2 N'EST PAS UN MOTEUR DE CETTE ROUTE, et le dire vaut mieux que
         * laisser l'echec retomber dans le 507 generique.
         *
         * setEngine() rejette -2 en PREMIERE ligne (« passer par setSampler »).
         * La route repondait alors « Plaits indisponible (tas insuffisant ?) ».
         * Faux deux fois : il n'etait pas question de Plaits, et le tas n'y
         * etait pour rien. Vecu — en voulant rearmer le garde-fou de boot apres
         * une coupure, cette reponse a envoye chercher un probleme de memoire
         * qui n'existait pas, pendant que le rearmement, lui, n'avait PAS eu
         * lieu. Le commentaire juste en dessous le dit deja : « un message faux
         * coute plus cher qu'une absence de message ». Il valait aussi pour
         * celui-la. */
        if (n == -2) {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"message\":"
                "\"L'echantillonneur ne se choisit pas ici : POST /api/audio/sampler "
                "avec name=<fichier.wav> (name vide pour l'arreter). Cette route ne "
                "prend que -1 (aucun moteur) et 0..23 (synthese).\"}");
            return;
        }
        if (n >= 0 && !AudioEngine::syntheseLourdeDisponible()) {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"synthese\":false,\"message\":"
                "\"Cette carte ne fait pas de synthese. Mesure : un moteur lourd "
                "resident ne laisse que 7 668 o de memoire d'un seul tenant, et "
                "recharger l'interface coince alors la carte (MESURES §122-126). "
                "Ce qui marche ici : capteurs, MIDI, OSC, sequenceur, scripts .nms "
                "et ECHANTILLONS (trig-wav). La synthese commence a deux cartes.\"}");
            return;
        }
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
                "{\"status\":\"armed\",\"engine\":" + String(n)
                + ",\"redemarrage_requis\":true,\"message\":"
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
        bool _redemarrageRequis = false;
        // ORDRE IMPORTANT : le moteur D'ABORD. setEngine() peut déclencher
        // l'allocation de Plaits, dont l'initialisation repose le patch sur ses
        // valeurs par défaut — appliquer les continus avant, c'est les perdre.
        if (request->hasParam("engine", true)) {
            const int n = request->getParam("engine", true)->value().toInt();
            /* Une cue qui demande un moteur de synthese sur une carte qui n'en
             * fait pas : on NE casse PAS la cue — les continus qui suivent et
             * le reste du spectacle continuent. On laisse simplement setEngine
             * refuser, et `engine` rendu plus bas dira la verite. */
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
            /* ET SI C'ETAIT ARME, ON LE DIT. Le cas passait ici en SILENCE :
             * 200, « engine » rendu a sa valeur reelle, et personne pour
             * remarquer qu'elle n'etait pas celle demandee. L'app demandait
             * Plaits, recevait un succes, et n'avait pas Plaits — le mensonge
             * d'etat que tout ce chantier supprime. Le champ dit ce qui manque :
             * un redemarrage, et rien d'autre. */
            _redemarrageRequis = (AudioEngine::derniereBascule()
                                  == AudioEngine::Bascule::Armee);
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
        lire("drone",      p.drone);
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
        json += ",\"drone\":"      + String(a.drone >= 0.5f ? 1 : 0);
        json += ",\"volume\":"     + String(AudioEngine::volume(), 3);
        /* Le seul geste qui appliquerait ce qui vient d'etre demande. La carte
         * ne redemarre PAS d'elle-meme : elle dit ce qu'il faudrait. Decider de
         * couper le son revient a qui tient la salle, pas au firmware. */
        json += ",\"redemarrage_requis\":" + String(_redemarrageRequis ? "true" : "false");
        json += "}";
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

    /* DECLENCHER L'ECHANTILLON MAINTENANT — le chemin VIVANT.
     * Cote navigateur, `trig-wav` demarre son BufferSource au chargement de la
     * case ; poser un son dans l'inspecteur doit donc s'entendre tout de suite,
     * sans attendre un changement de cue. `loop` et `oncue` accompagnent le
     * declenchement : c'est le meme etat que la cue installerait. */
    server.on("/api/audio/sampler/jouer", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!AudioEngine::samplerActif()) {
            request->send(409, "application/json",
                "{\"status\":\"error\",\"message\":\"aucun echantillon charge\"}");
            return;
        }
        /* `name` DESIGNE LEQUEL. Le lecteur est polyphonique et le magasin tient
         * tous les echantillons : « celui qui est charge » n'existe plus, il
         * faut nommer. Sans nom, on retombe sur celui du clavier. */
        const String nom = request->hasParam("name", true)
                         ? request->getParam("name", true)->value()
                         : String(AudioEngine::samplerNom());
        const bool boucle = request->hasParam("loop", true)
                         && request->getParam("loop", true)->value() != "0";
        const bool surCue = !request->hasParam("oncue", true)
                         || request->getParam("oncue", true)->value() != "0";
        AudioEngine::fixerDeclenchementSurCue(surCue);
        bool lance = false;
        if (surCue) lance = AudioEngine::declencherEchantillon(nom.c_str(), boucle);
        else        AudioEngine::arreterEchantillonNomme(nom.c_str());  // au clavier de jouer
        if (surCue && !lance) {
            request->send(404, "application/json",
                String("{\"status\":\"error\",\"message\":\"echantillon « ")
                + nom + " » absent de la carte\"}");
            return;
        }
        request->send(200, "application/json",
            String("{\"status\":\"ok\",\"name\":\"") + nom
            + "\",\"loop\":" + (boucle ? "true" : "false")
            + ",\"oncue\":" + (surCue ? "true" : "false") + "}");
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
    /* ── UNE INSTRUCTION REND L'ETAT QU'ELLE PRODUIT ──────────────────────
     * L'app applique CETTE reponse, sans attendre l'annonce WebSocket. Sinon
     * le transport dependrait d'une socket : si elle est tombee — et elle
     * tombe, la carte ferme quand son bloc contigu s'effondre — la carte
     * jouerait pendant que l'ecran resterait fige, et « le bouton play ne
     * fonctionne plus ». L'annonce reste utile pour ce que la carte decide
     * SEULE (l'enchainement minute) ; elle n'est plus le seul chemin. */
    server.on("/api/cues/play", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::demarrer();
        /* « ok » NE DOIT PAS VOULOIR DIRE « j'ai repondu ». La route rendait
         * status ok alors que la lecture n'avait pas demarre — mesure, avec
         * « lecture: false » dans la meme reponse. Un appelant qui lit le statut
         * et pas le detail croyait donc que ca jouait. */
        const bool parti = Cues::enLecture();
        request->send(200, "application/json",
                      String("{\"status\":\"") + (parti ? "ok" : "rien a jouer")
                      + "\",\"index\":" + Cues::indexCourant()
                      + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                      + ",\"restant\":" + String(Cues::restantSec(), 2)
                      + ",\"pause\":" + String(Cues::enPause() ? "true" : "false") + "}");
    });
    /* PAUSE : le TEMPS gele, le SON continue. La porte de silence ne bouge
     * pas — seul l'arret la ferme — donc une note tenue reste tenue.
     * « play » reprend la ou l'on en etait, sans recharger la cue. */
    server.on("/api/cues/pause", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::pauser();
        request->send(200, "application/json",
                      String("{\"status\":\"ok\",\"index\":") + Cues::indexCourant()
                      + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                      + ",\"restant\":" + String(Cues::restantSec(), 2)
                      + ",\"pause\":" + String(Cues::enPause() ? "true" : "false") + "}");
    });
    server.on("/api/cues/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::arreter();
        request->send(200, "application/json", String("{\"status\":\"ok\",\"index\":") + Cues::indexCourant()
                      + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                      + ",\"restant\":" + String(Cues::restantSec(), 2)
                      + ",\"pause\":" + String(Cues::enPause() ? "true" : "false") + "}");
    });
    server.on("/api/cues/go", HTTP_POST, [](AsyncWebServerRequest *request){
        Cues::suivant();
        request->send(200, "application/json", String("{\"status\":\"ok\",\"index\":") + Cues::indexCourant()
                      + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                      + ",\"restant\":" + String(Cues::restantSec(), 2)
                      + ",\"pause\":" + String(Cues::enPause() ? "true" : "false") + "}");
    });
    server.on("/api/cues/goto", HTTP_POST, [](AsyncWebServerRequest *request){
        const int i = request->hasParam("i", true)
                    ? request->getParam("i", true)->value().toInt() : 0;
        const bool ok = Cues::aller(i);
        request->send(ok ? 200 : 404, "application/json",
                      String("{\"status\":\"") + (ok ? "ok" : "hors liste")
                      + "\",\"index\":" + Cues::indexCourant()
                      + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                      + ",\"restant\":" + String(Cues::restantSec(), 2)
                      + ",\"pause\":" + String(Cues::enPause() ? "true" : "false") + "}");
    });
    server.on("/api/cues/texte", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain; charset=utf-8", Cues::contenu());
    });
    server.on("/api/cues", HTTP_GET, [](AsyncWebServerRequest *request){
        String j = "{\"n\":" + String(Cues::nombre())
                 + ",\"index\":" + String(Cues::indexCourant())
                 + ",\"lecture\":" + String(Cues::enLecture() ? "true" : "false")
                 /* DEUX TRANSPORTS, ET ILS NE DISENT PAS LA MEME CHOSE.
                  * `lecture` = le SEQUENCEUR EMBARQUE deroule sa liste (cas
                  * headless). `horloge_scripts` = l'horloge des scripts bat —
                  * elle est ouverte aussi bien par Cues::demarrer que par le
                  * transport de l'app (/api/audio/resume), qui pilote les cues
                  * lui-meme et ne fait donc pas courir celui de la carte.
                  * Les confondre, c'etait annoncer « lecture: false » pendant
                  * qu'un metro() battait : un etat faux, constate. On les
                  * NOMME tous les deux plutot que d'en inventer un seul. */
                 + ",\"horloge_scripts\":" + String(g_midiRouter.enLecture() ? "true" : "false")
                 + ",\"restant\":" + String(Cues::restantSec(), 2)
                 /* TROISIEME etat, et pas une nuance des deux premiers : gelee
                  * n'est pas arretee. La porte de silence reste ouverte en
                  * pause — ce qui sonnait sonne encore — alors qu'un arret la
                  * ferme. Le deduire d'un decompte non nul etait faux pour une
                  * cue infinie, qui ne decompte rien. */
                 + ",\"pause\":" + String(Cues::enPause() ? "true" : "false") + "}";
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

    /* ── LA COMPOSITION, GARDEE PAR LA CARTE (MESURES §156) ─────────────────
     * La source de cues.txt et des .nms — le JSON que l'app serialise —, pour
     * que l'app la recharge en se connectant : la page montre alors ce que la
     * carte porte. La carte ne la lit pas ; elle la garde et la rend.
     * GET : 204 si elle n'en a pas. POST : le corps JSON brut (pas un
     * formulaire : il depasse les parametres), recu en PSRAM morceau par
     * morceau ; rendu aussitot, ecrit en flash au premier silence. */
    server.on("/api/compo", HTTP_GET, [](AsyncWebServerRequest *request){
        size_t n = 0;
        auto t = Compo::courante(n);
        if (!t || !n) { request->send(204); return; }
        request->send(nidmi_reponse_tampon(request, "application/json", t, n));
    });
    server.on("/api/compo", HTTP_POST,
        [](AsyncWebServerRequest *request){
            char* p = (char*)request->_tempObject;
            const size_t n = request->contentLength();
            if (!p || !n) {
                request->send(n > Compo::MAX_OCTETS ? 413 : 400, "application/json",
                    n > Compo::MAX_OCTETS
                    ? String("{\"status\":\"error\",\"message\":\"composition trop grosse : plafond ")
                      + String((unsigned)Compo::MAX_OCTETS) + " octets\"}"
                    : String("{\"status\":\"error\",\"message\":\"corps JSON attendu\"}"));
                return;
            }
            if (p[0] != '{') {
                request->send(400, "application/json",
                    "{\"status\":\"error\",\"message\":\"une composition est un objet JSON\"}");
                return;
            }
            request->_tempObject = nullptr;   // la carte le garde : le serveur ne le liberera pas
            Compo::adopter(std::shared_ptr<char>(p, [](char* q) { heap_caps_free(q); }), n);
            request->send(200, "application/json",
                String("{\"status\":\"ok\",\"octets\":") + String((unsigned)n)
                + ",\"message\":\"rendue tout de suite, ecrite en flash au premier silence\"}");
        },
        nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            if (index == 0) {
                if (!total || total > Compo::MAX_OCTETS) return;   // refusee a la fin
                request->_tempObject = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (request->_tempObject && index + len <= total)
                memcpy((char*)request->_tempObject + index, data, len);
        });

server.on("/api/midi/scripts", HTTP_GET, [](AsyncWebServerRequest *request){
        /* `actif` : le nom de l'emplacement 0 — conserve pour ne pas casser
         * les clients existants. `emplacements` dit ce que TOUS portent, ce
         * qu'un seul nom ne pouvait pas exprimer. */
        String emps = "[";
        for (uint8_t e = 0; e < g_midiRouter.nEmplacements(); e++) {
            if (e) emps += ",";
            emps += "\"" + g_midiRouter.nomEmplacement(e) + "\"";
        }
        emps += "]";
        /* `n` et `plafond` : l'app n'a plus a savoir combien la carte tient, et
         * surtout elle n'a plus a l'ECRIRE EN DUR — c'etait un 4 recopie des
         * deux cotes, donc deux nombres a maintenir d'accord. */
        request->send(200, "application/json",
                      "{\"actif\":\"" + g_midiRouter.nomScript() + "\",\"emplacements\":" + emps
                      + ",\"n\":" + String((unsigned)g_midiRouter.nEmplacements())
                      + ",\"plafond\":" + String((unsigned)MidiRouter::PLAFOND_SCRIPTS_MAP)
                      + ",\"permanents\":" + String((unsigned)g_midiRouter.nMaillonsPermanents())
                      + ",\"fichiers\":" + ScriptStore::listerJson() + "}");
    });

    /* LA LONGUEUR DE LA CHAINE, dictee par la COMPOSITION.
     * L'app compte ses pistes map et le dit ; la carte alloue ce qu'elle peut et
     * REND ce qu'elle a obtenu. Si la memoire a manque, c'est elle qui le dit —
     * l'app n'a pas a le deviner, ni a porter une copie de la limite. */
    server.on("/api/midi/chaine/taille", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("n", true)) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"message\":\"parametre n requis\"}");
            return;
        }
        const int voulu = request->getParam("n", true)->value().toInt();
        if (voulu < 0 || voulu > MidiRouter::PLAFOND_SCRIPTS_MAP) {
            request->send(400, "application/json",
                String("{\"status\":\"error\",\"message\":\"n hors bornes (0..")
                + String((unsigned)MidiRouter::PLAFOND_SCRIPTS_MAP) + ")\"}");
            return;
        }
        const uint8_t obtenu = g_midiRouter.dimensionnerChaine((uint8_t)voulu);
        /* `permanents` : combien de maillons de tete appartiennent a la zone
         * MAIN. Meme requete que la longueur — c'est une seule decision de la
         * composition, et deux requetes laisseraient une fenetre ou la carte
         * aurait la nouvelle longueur avec l'ancienne frontiere. */
        if (request->hasParam("permanents", true)) {
            const int perm = request->getParam("permanents", true)->value().toInt();
            g_midiRouter.fixerMaillonsPermanents((uint8_t)((perm < 0) ? 0 : perm));
        }
        request->send(obtenu == voulu ? 200 : 507, "application/json",
            String("{\"status\":\"") + (obtenu == voulu ? "ok" : "partiel")
            + "\",\"demande\":" + String(voulu)
            + ",\"obtenu\":" + String((unsigned)obtenu)
            + ",\"permanents\":" + String((unsigned)g_midiRouter.nMaillonsPermanents()) + "}");
    });

    /* ── CE QUE LA CARTE EXECUTE VRAIMENT ─────────────────────────────────
     * Le CONTENU d'un maillon, et celui d'un fichier de mapfs. Il n'y avait
     * aucun moyen de les lire : on poussait un script, on observait un
     * comportement, et quand les deux ne s'accordaient pas il ne restait qu'a
     * deviner lequel des deux mentait.
     *
     * Constate a l'usage : un script de 43 caracteres, un fichier de 61 octets
     * sur la carte, et rien pour dire ce qu'il y avait dedans. Trois allers et
     * retours pour ne pas trancher.
     *
     * « La page montre l'etat REEL de la carte » — la regle vaut aussi pour les
     * scripts. En texte brut : c'est du code, on veut le LIRE. */
    server.on("/api/midi/script", HTTP_GET, [](AsyncWebServerRequest *request){
        const uint8_t emp = request->hasParam("slot")
                          ? (uint8_t)request->getParam("slot")->value().toInt() : 0;
        if (emp >= g_midiRouter.nEmplacements()) {
            request->send(404, "text/plain; charset=utf-8",
                          "maillon inexistant (la chaine en tient "
                          + String((unsigned)g_midiRouter.nEmplacements()) + ")");
            return;
        }
        request->send(200, "text/plain; charset=utf-8", g_midiRouter.contenuEmplacement(emp));
    });

    /* Le contenu d'un FICHIER de mapfs. Pendant du precedent : l'un dit ce qui
     * TOURNE, l'autre ce qui est RANGE — et c'est en les comparant qu'on voit
     * qu'une cue n'a pas charge ce qu'on croyait. */
    /* ⚠ PAS « /api/midi/scripts/lire » : le serveur fait correspondre par
     * PREFIXE, si bien que la route etait avalee par le GET de la liste — elle
     * rendait le JSON de l'inventaire au lieu du fichier, sans erreur. Un nom
     * qui ne prefixe rien. */
    server.on("/api/midi/fichier", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name")) {
            request->send(400, "text/plain; charset=utf-8", "parametre name requis");
            return;
        }
        String contenu;
        if (!ScriptStore::lire(request->getParam("name")->value().c_str(), contenu)) {
            request->send(404, "text/plain; charset=utf-8", "introuvable dans mapfs");
            return;
        }
        request->send(200, "text/plain; charset=utf-8", contenu);
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
        /* Les priorites LUES, pas ecrites en dur : elles etaient recopiees ici
         * (5, 4, 11), et ont change au §149 — une valeur recopiee finit par
         * mentir. */
        auto prio = [](const char* nom) -> int {
            TaskHandle_t t = xTaskGetHandle(nom);
            return t ? (int)uxTaskPriorityGet(t) : -1;
        };
        json += "},\"temps_reel\":{\"mux\":" + String(prio("MuxTask"));
        json += ",\"midi\":" + String(prio("MidiTask"));
        json += ",\"audio\":" + String(prio("audio")) + "},";
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

    /* ── OU PASSE LE TEMPS DE CALCUL : TACHE PAR TACHE, COEUR PAR COEUR ─────
     * Compteurs de FreeRTOS (horloge esp_timer, µs, 32 bits : ils repassent par
     * zero toutes les 71 min — ne comparer que des ecarts courts). Le client lit
     * AVANT et APRES une charge et fait la difference : n = nom, p = priorite,
     * c = coeur impose (-1 : aucun), t = temps cumule. IDLE0/IDLE1 donnent la
     * part libre de chaque coeur. Limite : le temps passe en INTERRUPTION est
     * compte a la tache interrompue. MESURES §149.
     * Et pour la memoire (§150) : m = marge de pile jamais entamee depuis le
     * demarrage (octets), b = adresse de la pile, h = celle du TCB — que
     * /api/diag/tas retrouve dans la carte du tas. */
    server.on("/api/diag/taches", HTTP_GET, [](AsyncWebServerRequest *request){
        const UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
        TaskStatus_t* st = (TaskStatus_t*)malloc(n * sizeof(TaskStatus_t));
        if (!st) { request->send(503, "application/json", "{\"erreur\":\"memoire\"}"); return; }
        uint32_t total = 0;
        const UBaseType_t k = uxTaskGetSystemState(st, n, &total);
        String j = "{\"total_us\":" + String((unsigned long)total) + ",\"taches\":[";
        for (UBaseType_t i = 0; i < k; ++i) {
            if (i) j += ',';
            const int coeur = (st[i].xCoreID == 0 || st[i].xCoreID == 1) ? (int)st[i].xCoreID : -1;
            j += "{\"n\":\"" + String(st[i].pcTaskName) + "\",\"p\":" + String((unsigned)st[i].uxCurrentPriority);
            j += ",\"c\":" + String(coeur) + ",\"t\":" + String((unsigned long)st[i].ulRunTimeCounter);
            j += ",\"m\":" + String((unsigned long)st[i].usStackHighWaterMark * sizeof(StackType_t));
            j += ",\"b\":" + String((unsigned long)(uintptr_t)st[i].pxStackBase);
            j += ",\"h\":" + String((unsigned long)(uintptr_t)st[i].xHandle) + "}";
        }
        free(st);
        j += "]}";
        request->send(200, "application/json", j);
    });

    /* ── BANC : UNE RAFALE MIDI SUR L'USB ─────────────────────────────────
     * Un accord de `notes` notes (128 au plus) par canal, sur `canaux` canaux
     * a partir de `canal` : toutes les note-on, puis toutes les note-off,
     * d'un seul coup — ce qu'un script ou un GO de cue peut produire. Envoye
     * DIRECTEMENT a la sortie USB (sans l'echo audio ni les autres transports
     * de MidiRouter) : c'est elle qu'on eprouve. `direct=1` rejoue l'ancien
     * chemin (ecriture dans la file de TinyUSB, retour ignore) pour mesurer
     * ce qu'il perdait, dans le meme demarrage. `nettoyer=1` envoie seulement
     * un CC 123 (toutes notes eteintes) par canal, par le chemin normal —
     * apres un essai `direct`, qui laisse des notes bloquees chez l'hote.
     * Lecteur : hardware/bench/midi/rafale-usb.py. MESURES §151. */
    server.on("/api/diag/midi-rafale", HTTP_POST, [](AsyncWebServerRequest *request){
        auto entier = [request](const char* nom, long defaut, long mini, long maxi) {
            long v = request->hasParam(nom, true) ? request->getParam(nom, true)->value().toInt() : defaut;
            return v < mini ? mini : (v > maxi ? maxi : v);
        };
        const uint16_t notes = (uint16_t)entier("notes", 64, 1, 128);
        const uint8_t canal = (uint8_t)entier("canal", 16, 1, 16);
        const uint8_t canaux = (uint8_t)entier("canaux", 1, 1, (long)(17 - canal));
        const uint8_t velocite = (uint8_t)entier("velocite", 1, 1, 127);
        const bool direct = entier("direct", 0, 0, 1) == 1;
        const bool nettoyer = entier("nettoyer", 0, 0, 1) == 1;
        UsbMidiManager& usb = serverCore.usbMidi();
        if (!usb.isConnected()) {
            request->send(409, "application/json", "{\"erreur\":\"MIDI USB non demarre\"}");
            return;
        }
        const int64_t t0 = esp_timer_get_time();
        for (uint8_t k = 0; k < canaux; ++k) {
            if (nettoyer) {
                usb.sendControlChange((uint8_t)(canal + k), 123, 0);
            } else {
                usb.rafaleBanc(notes, (uint8_t)(canal + k), velocite, direct);
            }
        }
        const uint32_t duree = (uint32_t)(esp_timer_get_time() - t0);
        const unsigned messages = nettoyer ? canaux : 2u * notes * canaux;
        request->send(200, "application/json",
            String("{\"messages\":") + messages + ",\"direct\":" + (direct ? "true" : "false") +
            ",\"duree_us\":" + duree + "}");
    });

    /* ── LA CARTE DU TAS INTERNE ──────────────────────────────────────────
     * Le plus gros bloc contigu est le chiffre qui decide ; mais un chiffre ne
     * dit pas CE QUI le borne. Cette route parcourt le tas interne bloc par
     * bloc (heap_caps_walk) et rend, region par region, chaque bloc : decalage
     * depuis le debut de la region, taille, occupe (1) ou libre (0). Croisee
     * avec /api/diag/taches (adresse de chaque pile et de chaque TCB), elle dit
     * qui est ou. Lecteur : hardware/bench/memoire/tas.py. MESURES §150.
     *
     * Le releve est pris AVANT d'allouer quoi que ce soit pour la reponse, dans
     * des tampons en PSRAM : il ne deplace pas ce qu'il mesure. Le parcours
     * tient le verrou du tas — une section critique, interruptions masquees sur
     * ce coeur — le temps de ranger quelques centaines de blocs : un outil de
     * banc, jamais une sonde. */
    server.on("/api/diag/tas", HTTP_GET, [](AsyncWebServerRequest *request){
        struct Bloc { uint32_t adr; uint32_t taille; uint8_t occupe; uint8_t region; };
        struct Releve {
            enum { MAX_BLOCS = 2048, MAX_REGIONS = 8 };
            uint32_t debut[MAX_REGIONS], fin[MAX_REGIONS];
            int nRegions, nBlocs;
            bool tronque;
            Bloc blocs[MAX_BLOCS];
        };
        constexpr size_t TEXTE_MAX = Releve::MAX_BLOCS * 28 + 1024;
        Releve* r = (Releve*)heap_caps_calloc(1, sizeof(Releve), MALLOC_CAP_SPIRAM);
        char* texte = (char*)heap_caps_malloc(TEXTE_MAX, MALLOC_CAP_SPIRAM);
        if (!r || !texte) {
            heap_caps_free(r);
            heap_caps_free(texte);
            request->send(503, "application/json", "{\"erreur\":\"psram\"}");
            return;
        }
        const size_t gros = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t libre = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t mini = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        heap_caps_walk(MALLOC_CAP_INTERNAL, [](walker_heap_into_t h, walker_block_info_t b, void* u) -> bool {
            Releve* r = static_cast<Releve*>(u);
            int reg = r->nRegions - 1;
            if (reg < 0 || r->debut[reg] != (uint32_t)h.start) {
                if (r->nRegions >= Releve::MAX_REGIONS) { r->tronque = true; return false; }
                reg = r->nRegions++;
                r->debut[reg] = (uint32_t)h.start;
                r->fin[reg] = (uint32_t)h.end;
            }
            if (r->nBlocs >= Releve::MAX_BLOCS) { r->tronque = true; return false; }
            Bloc& x = r->blocs[r->nBlocs++];
            x.adr = (uint32_t)(uintptr_t)b.ptr;
            x.taille = (uint32_t)b.size;
            x.occupe = b.used ? 1 : 0;
            x.region = (uint8_t)reg;
            return true;
        }, r);

        // Borne par construction (2 048 blocs de 18 caracteres au plus) ; la
        // garde tient quand meme : jamais d'ecriture au-dela du tampon.
        size_t n = 0;
        auto ecrire = [&](const char* fmt, auto... a) {
            if (n >= TEXTE_MAX - 1) return;
            const int k = snprintf(texte + n, TEXTE_MAX - n, fmt, a...);
            if (k > 0) n = (n + (size_t)k < TEXTE_MAX - 1) ? n + (size_t)k : TEXTE_MAX - 1;
        };
        ecrire("{\"gros\":%u,\"libre\":%u,\"mini\":%u,\"tronque\":%s,\"regions\":[",
               (unsigned)gros, (unsigned)libre, (unsigned)mini, r->tronque ? "true" : "false");
        for (int g = 0; g < r->nRegions; ++g) {
            ecrire("%s{\"d\":%lu,\"f\":%lu,\"blocs\":[", g ? "," : "",
                   (unsigned long)r->debut[g], (unsigned long)r->fin[g]);
            bool premier = true;
            for (int i = 0; i < r->nBlocs; ++i) {
                const Bloc& x = r->blocs[i];
                if (x.region != g) continue;
                ecrire("%s[%lu,%lu,%u]", premier ? "" : ",",
                       (unsigned long)(x.adr - r->debut[g]), (unsigned long)x.taille, (unsigned)x.occupe);
                premier = false;
            }
            ecrire("%s", "]}");
        }
        ecrire("%s", "]}");
        heap_caps_free(r);

        // Le texte vit en PSRAM jusqu'au dernier morceau envoye.
        std::shared_ptr<char> garde(texte, [](char* p) { heap_caps_free(p); });
        request->send(nidmi_reponse_tampon(request, "application/json", garde, n));
    });

    /* ── LES RESERVOIRS, DITS PAR LA CARTE ────────────────────────────────
     *
     * Trois reservoirs se remplissaient sans que rien ne le dise, chacun d'une
     * espece differente — et c'est pour ca qu'un seul chiffre ne suffisait pas :
     *
     *   NVS      — 20 480 o pour TOUT (WiFi, mDNS, OSC, noms des scripts, table
     *              CC, seuils du mux, et les configurations de broches). La
     *              carte accepte 1 900 o par broche et en tient 32 : trois fois
     *              la partition. La borne par broche est verifiee, le TOTAL ne
     *              l'etait nulle part.
     *   mapfs    — 1 Mo, mais alloue par bloc : un script de 30 octets en occupe
     *              plusieurs milliers. Un pourcentage d'octets rassure a tort ;
     *              c'est le NOMBRE DE FICHIERS qui bute en premier. D'ou les
     *              deux mesures cote a cote — l'ecart EST le surcout.
     *   reprises — le seul qu'on ne puisse pas dimensionner a la configuration
     *              (voir MappingEngine::statsReprises). Seize places pour toute
     *              la carte ; au-dela, un lag() ne partait pas, en silence.
     *
     * `?reset=1` ouvre une fenetre propre sur les reprises, comme pour la
     * gigue : remettre a zero AVANT la charge, lire APRES.
     *
     * Lecture seule et hors du chemin temps reel : cette route ne fait que
     * lire des compteurs et parcourir un repertoire. */
    /* LA SANTE, DECIDEE PAR LA CARTE. Lue UNE fois par l'app a la connexion ;
     * ensuite la carte annonce elle-meme chaque changement (« NIDMI_SANTE: »),
     * sans etre sondee. Causes separees par des virgules ; vide = rien a
     * signaler. La liste et ses seuils : NiDMI.cpp, « LA SANTE DE LA CARTE ». */
    server.on("/api/diag/sante", HTTP_GET, [](AsyncWebServerRequest *request){
        char causes[96];
        nidmi_santeTexte(nidmi_sante(), causes, sizeof causes);
        request->send(200, "application/json", String("{\"causes\":\"") + causes + "\"}");
    });

    server.on("/api/diag/reservoirs", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("reset")) {
            MappingEngine::reinitStatsReprises();
            UsbMidiManager::reinitStatsSortie();
            request->send(200, "application/json", "{\"reset\":true}");
            return;
        }
        String json = "{";

        /* NVS : les chiffres viennent du systeme (nvs_get_stats), pas d'un
         * comptage a nous. Une entree fait 32 octets — c'est la structure de
         * l'ESP-IDF, pas une constante du projet. */
        /* ── LE RESERVOIR LE PLUS CONTRAINT, ET CELUI QUI MANQUAIT ────────
         * Le PLUS GROS BLOC CONTIGU, pas le tas libre. La distinction est toute
         * la panne : mesure le 2026-09-12, une carte a 22 372 o LIBRES mais
         * 7 668 o de plus grand bloc repondait a chaque requete prise une par
         * une — et ne servait plus une page, parce qu'un navigateur en ouvre
         * trente de front. Les octets etaient la ; ils etaient en miettes.
         * Il se fragmente a chaque chargement de page (4 a 11 ko, §81) et ne se
         * defragmente JAMAIS : seul un redemarrage le rend. C'est donc le seul
         * chiffre qui PREDIT la panne, et il n'etait sur aucun ecran.
         * `min_ever` dit de combien on est passe pres sans le voir. */
        json += "\"tas\":{\"bloc_contigu\":"
              + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        json += ",\"libre\":"     + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        json += ",\"min_jamais\":" + String((unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        json += ",\"plancher\":"  + String((unsigned)MidiRouter::PLANCHER_BLOC_CONTIGU);
        /* DEPUIS COMBIEN DE TEMPS LA CARTE TOURNE. Le tas n'est PAS stabilise
         * au demarrage : mesure le 2026-09-12, +1 s apres un redemarrage la
         * carte annonce 13 812 o de bloc contigu, +3 s 26 612. Lire le panneau
         * a ce moment-la fait croire au plancher alors que WiFi et lwIP
         * finissent de s'installer — faux positif constate, et alarmant pour
         * rien. L'app a besoin de savoir que la mesure est encore tiede. */
        json += ",\"uptime_ms\":" + String((unsigned long)millis()) + "},";

        nvs_stats_t st = {};
        if (nvs_get_stats(nullptr, &st) == ESP_OK) {
            json += "\"nvs\":{\"entrees_utilisees\":" + String((unsigned)st.used_entries);
            json += ",\"entrees_libres\":"  + String((unsigned)st.free_entries);
            json += ",\"entrees_total\":"   + String((unsigned)st.total_entries);
            json += ",\"espaces\":"         + String((unsigned)st.namespace_count);
            json += ",\"octets_par_entree\":32}";
        } else {
            json += "\"nvs\":null";          // le dire, plutot que d'inventer des zeros
        }

        size_t nf = 0, ns = 0, oc = 0, ou_ = 0, ot = 0;
        ScriptStore::infos(nf, ns, oc, ou_, ot);
        json += ",\"mapfs\":{\"fichiers\":" + String((unsigned)nf);
        json += ",\"scripts\":"        + String((unsigned)ns);
        json += ",\"octets_contenu\":" + String((unsigned)oc);
        json += ",\"octets_utilises\":"+ String((unsigned)ou_);
        json += ",\"octets_total\":"   + String((unsigned)ot) + "}";
        // La composition gardee pour l'app (§156) : recue, rendue, ecrite au silence.
        json += ",\"compo\":" + Compo::etatJson();

        uint16_t enCours = 0, maxVu = 0, capacite = 0; uint32_t refus = 0;
        MappingEngine::statsReprises(enCours, maxVu, refus, capacite);
        json += ",\"reprises\":{\"en_cours\":" + String((unsigned)enCours);
        json += ",\"max\":"      + String((unsigned)maxVu);
        json += ",\"refus\":"    + String((unsigned)refus);
        json += ",\"capacite\":" + String((unsigned)capacite) + "}";

        /* La sortie MIDI USB (MESURES §151) : notre file devant celle de
         * TinyUSB. `debordes` et `sans_hote` ne sont pas des pertes de
         * note-off — la reconciliation les rattrape (`relaches`). */
        UsbMidiManager::StatsSortie mu;
        UsbMidiManager::statsSortie(mu);
        json += ",\"midi_usb\":{\"envoyes\":" + String((unsigned long)mu.envoyes);
        json += ",\"attentes\":"  + String((unsigned long)mu.attentes);
        json += ",\"debordes\":"  + String((unsigned long)mu.debordes);
        json += ",\"sans_hote\":" + String((unsigned long)mu.sansHote);
        json += ",\"relaches\":"  + String((unsigned long)mu.relaches);
        json += ",\"max\":"       + String((unsigned)mu.fileMax);
        json += ",\"capacite\":"  + String((unsigned)mu.capacite) + "}";

        /* La borne par broche, pour que le client n'ait pas a la recopier :
         * elle est deja publiee par /api/pins/caps, et deux copies d'un meme
         * nombre finissent toujours par diverger. On dit seulement ce qui
         * manquait — la partition qui les accueille TOUTES. */
        json += "}";
        request->send(200, "application/json", json);
    });

    /* ── LE BUS INTER-BLOCS, RENDU VISIBLE ────────────────────────────────
     * s("nom") ecrit ici, r("nom") y lit. C'est le SEUL lien entre un script de
     * broche et un script map — et il etait entierement invisible : quand ca ne
     * marchait pas, rien ne disait si l'ecriture avait eu lieu, sous quel nom,
     * ni avec quelle valeur. On cherchait donc dans les deux scripts a la fois,
     * sans moyen de trancher.
     *
     * Trois choses que la seule lecture du code ne donne pas :
     *   - le NOM tel qu'il est arrive (strlcpy borne a 16 : « potentiometre_1 »
     *     et « potentiometre_2 » deviennent la MEME entree) ;
     *   - la VALEUR, qui dit l'echelle — in() rend 0..1, et un r() suivi d'un
     *     ctl.out() sort alors 0 ou 1, ce qui ressemble a « rien recu » ;
     *   - le NOMBRE d'entrees : 32 places, partagees par toutes les broches et
     *     tous les maillons.
     * Lecture seule, hors du chemin temps reel. */
    server.on("/api/diag/registre", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"n\":" + String(FluxRegistry::count)
                    + ",\"capacite\":32,\"max_nom\":15,\"entrees\":[";
        for (int i = 0; i < FluxRegistry::count && i < 32; i++) {
            if (i) json += ",";
            json += "{\"nom\":\"" + String(FluxRegistry::entries[i].name)
                  + "\",\"valeur\":" + String(FluxRegistry::entries[i].value, 4) + "}";
        }
        json += "]}";
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
        g_midiRouter.fixerTransport(true);   // l'horloge des scripts repart
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    server.on("/api/audio/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        // couperSon() ferme la porte de silence : Plaits rend en continu et
        // ignore le note-off (son LPG gere l'extinction), donc les note-off
        // ci-dessous ne le taisent pas — la porte, si. Elle rouvre a la note
        // suivante (jeu live apres un STOP).
        AudioEngine::couperSon();
        g_midiRouter.fixerTransport(false);  // et l'horloge des scripts s'arrete
        for (int n = 0; n < 128; n++) AudioEngine::noteOff((uint8_t)n);
        AudioEngine::testTone(0.0f, 0);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
}
