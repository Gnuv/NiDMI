#include "../config/Occupations.h"
#include "complex/ComplexHandlerRegistry.h"
#include "complex/ComplexHandler.h"
#include "../audio/AudioEngine.h"   // PIN_BCLK/LRCK/DIN
#include "ComponentManager.h"
#include "../server/WebDebugConsole.h"
#include <Arduino.h> // For Serial.printf
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include "../server/ServerCore.h"
#include "../osc/OSCQueue.h"
#include "../diag/Activite.h"   // la LED d'activite de l'app (MESURES §167)
#include "../diag/Chronos.h"    // ou passe le temps du tour (MESURES §170)
#include "../midi/MidiMessageType.h"
#include "../config/ConfigCache.h"
#include "../config/ConfigLoader.h"
#include "../utils/JSONParser.h"
#include "../processors/ProcessorRegistry.h"
#include "../processors/Processors.h"  // Centralise tous les processeurs pour l'enregistrement automatique
#include "../processors/ImuProcessor.h"
#include "../processors/JoystickProcessor.h"
#include "../processors/Joystick3Processor.h"
#include "../components/ComponentRegistry.h"  // Pour trouver les définitions de composants
#include "../components/basic/ButtonDef.h"    // Pour ButtonConfig (btnMode)
#include "../components/ValidationRegistry.h"  // Pour la validation centralisée
#include "../utils/PinMapper.h"
#include "../osc/OSCCalibrationHandler.h"
#include "../osc/OSCConfigLoader.h"
#include "../utils/ComponentInitializer.h"
#include "MuxValidator.h"
#include "../mapping/MappingEngine.h"
#include "../midi/MidiRouter.h"
#include "../Globals.h"
#include "../components/motion/Lis3dhDef.h"


ComponentManager::ComponentManager()
    : component_count(0), midi_sender(nullptr), midiTaskHandle(nullptr), midiTaskStarted(false),
      telemetryQueue(nullptr), telemetryDropCount(0) {
    // Initialiser les filtres
    for (int i = 0; i < MAX_COMPONENTS; i++) {
        filters[i].alpha = 0.1f;
        filters[i].initialized = false;
        last_telemetry_sent_ts[i] = 0;
        last_telemetry_sent_ts_aux[i] = 0;
        last_telemetry_sent_ts_aux2[i] = 0;
    }
}

ComponentManager::~ComponentManager() {
    // Arrêter la tâche MIDI avant destruction
    if (midiTaskStarted && midiTaskHandle != nullptr) {
        vTaskDelete(midiTaskHandle);
        midiTaskHandle = nullptr;
        midiTaskStarted = false;
    }
    if (telemetryQueue != nullptr) {
        vQueueDelete(telemetryQueue);
        telemetryQueue = nullptr;
    }
    clearAll();
}

/* ── L'OSC DES SCRIPTS PASSE PAR LA BOUCLE (MESURES §176) ─────────────────
 * osc.out() emettait depuis la tache du script — MidiTask le plus souvent —
 * sur l'objet UDP d'OSCManager, que la boucle lit au meme moment : beginPacket()
 * pose le destinataire dans l'objet, parsePacket() y ecrit l'expediteur du
 * paquet recu — un osc.out() surpris entre les deux partait chez l'expediteur.
 * Et l'octet d'hote changeait la cible le temps d'un envoi. Le rappel DEPOSE
 * (file PSRAM de 32, jamais bloquante) ; la boucle emet juste apres avoir lu
 * l'OSC entrant (emettreOscScripts). Une seule tache touche l'objet. */
namespace {
struct EnvoiOscScript {
    char    adresse[MappingEngine::MAX_ADRESSE_OSC];
    float   args[8];
    uint8_t n, hote;
};
constexpr UBaseType_t kEnvoisOscMax = 32;
StaticQueue_t g_fileOscTcb;
QueueHandle_t g_fileOsc = nullptr;
volatile uint32_t g_oscScriptsDeposes = 0, g_oscScriptsEmis = 0, g_oscScriptsJetes = 0;
}  // namespace

void ComponentManager::statsOscScripts(uint32_t& deposes, uint32_t& emis, uint32_t& jetes) {
    deposes = g_oscScriptsDeposes; emis = g_oscScriptsEmis; jetes = g_oscScriptsJetes;
}

void ComponentManager::emettreOscScripts() {
    if (!g_fileOsc) return;
    EnvoiOscScript e;
    OSCManager& o = osc_manager;
    for (int k = 0; k < 16 && xQueueReceive(g_fileOsc, &e, 0) == pdTRUE; k++) {
        const String adresse(e.adresse);
        g_oscScriptsEmis = g_oscScriptsEmis + 1;
        if (!e.hote) { o.sendMultiFloat(adresse, e.args, e.n); continue; }
        /* Octet d'hote : le meme reseau que la cible configuree, dernier octet
         * substitue. Le moteur de reference y lit 127.0.0.N — un poste de
         * travail ; ici la lecture qui a un sens est « le voisin de la cible ». */
        const String cible = o.getTargetIP();
        const int point = cible.lastIndexOf('.');
        if (point < 0) { o.sendMultiFloat(adresse, e.args, e.n); continue; }
        const uint16_t port = o.getTargetPort();
        o.setTarget(cible.substring(0, point + 1) + String((int)e.hote), port);
        o.sendMultiFloat(adresse, e.args, e.n);
        o.setTarget(cible, port);
    }
}

void ComponentManager::begin(MidiSender* sender) {
    midi_sender = sender;
    telemetryQueue = xQueueCreate(32, sizeof(TelemetryWsMsg));  // 8->32 : moins d'overflow sur capteurs actifs
    /* Charger d'abord les MUX */
    loadMuxConfigFromNVS();
    /* Le TRANSPORT de osc.out(). Le moteur de script ne connait aucun
     * transport : on le lui pose, comme l'impression. Le banc de conformite ne
     * le pose pas — une epreuve n'arrose pas le reseau de l'usager. */
    if (!g_fileOsc) {
        uint8_t* stock = (uint8_t*)heap_caps_malloc(kEnvoisOscMax * sizeof(EnvoiOscScript),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stock) g_fileOsc = xQueueCreateStatic(kEnvoisOscMax, sizeof(EnvoiOscScript), stock, &g_fileOscTcb);
    }
    MappingEngine::surOsc([](const char* adresse, const float* args, int n,
                             uint8_t hote) {
        if (!g_fileOsc) return;
        EnvoiOscScript e;
        strlcpy(e.adresse, adresse ? adresse : "", sizeof e.adresse);
        if (n > (int)(sizeof e.args / sizeof e.args[0])) n = sizeof e.args / sizeof e.args[0];
        if (n < 0) n = 0;
        for (int i = 0; i < n; i++) e.args[i] = args[i];
        e.n = (uint8_t)n;
        e.hote = hote;
        /* File pleine : l'envoi est jete sans attendre (de l'UDP, perte par
         * nature) — la tache MIDI ne se bloque pas pour le reseau. */
        if (xQueueSend(g_fileOsc, &e, 0) != pdTRUE) g_oscScriptsJetes = g_oscScriptsJetes + 1;
        else g_oscScriptsDeposes = g_oscScriptsDeposes + 1;
    });

    /* Puis charger les configs des pins */
    ConfigLoader::loadFromNVS(*this);
    /* ET COMPTER CE QU'ON VIENT DE CHARGER. C'est le chemin du DEMARRAGE —
     * `reloadConfigs()`, lui, ne sert qu'aux modifications ulterieures — et
     * personne n'y rafraichissait les occupations. `s_audio` restait donc a sa
     * valeur initiale, faux, et la carte REFUSAIT le son au motif qu'« aucun
     * DAC n'est declare » alors qu'elle en listait un sur D0.
     * Le son ne revenait qu'une fois l'app ouverte sur la zone I/O, dont la
     * lecture de /api/pins/caps rafraichit au passage : une carte headless qui
     * redemarrait revenait MUETTE et le restait, faute de quelqu'un pour ouvrir
     * une page. Voir MESURES.md §119.2. */
    Occupations::rafraichir();
    
    // Charger et initialiser la configuration OSC depuis NVS
    OSCConfigLoader::OSCConfig oscConfig = OSCConfigLoader::loadFromNVS();
    OSCConfigLoader::initialize(
        oscConfig,
        osc_manager,
        osc_queue,
        [this](const String& address, float value, const String& arg_string) {
            // D'abord, gérer les commandes de calibrage des multiplexeurs
            OSCCalibrationHandler::handleMessage(*this, address, value, arg_string);
            
            // Ensuite, router les messages OSC vers les LEDs
            LedProcessor::handleOscMessage(configs, component_count, address, value, arg_string);

            /* Enfin, les SCRIPTS : osc.in() ecoute ici. Le message traverse les
             * quatre emplacements map, chacun libre d'y repondre ou non.
             * Les broches, elles, sont pilotees par leur broche — comme pour le
             * MIDI entrant, qui va aux emplacements et non aux broches. */
            g_midiRouter.recevoirOsc(address.c_str(), value);
        }
    );
    
    // Démarrer la tâche FreeRTOS pour les multiplexeurs
    mux_manager.begin();
    
    // Tâche MIDI sur Core 0 (avec MuxTask). loop()/serveur = Core 1 → éviter de charger Core 1 pour que le serveur ne plante pas avec 7+ pins touch.
    const BaseType_t midiTaskCore = 0;
    const uint32_t midiTaskStackBytes = 8192;  // Stack pour traitement composants
    BaseType_t result;
#if defined(configSUPPORT_STATIC_ALLOCATION) && (configSUPPORT_STATIC_ALLOCATION == 1)
    // Stack allouée statiquement (.bss) plutôt que sur le heap : sur ESP32-C3 le heap
    // est trop fragmenté après WiFi+serveur pour fournir 8 KB contigus, et l'alloc
    // dynamique échouait (xTaskCreate → "Failed to create FreeRTOS MIDI task").
    // Le buffer existe au link, donc la création ne peut plus échouer par manque de heap.
    static StackType_t midiTaskStack[8192];
    static StaticTask_t midiTaskTCB;
    midiTaskHandle = xTaskCreateStaticPinnedToCore(
        midiTask,
        "MidiTask",
        midiTaskStackBytes,
        this,
        19,                // Juste sous MuxTask (20), AU-DESSUS de la pile reseau
                           // (tiT, 18) : a 4, le MIDI prenait 335 µs de retard moyen
                           // sous charge reseau ; a 19, 4 a 15 µs (MESURES §149).
        midiTaskStack,
        &midiTaskTCB,
        midiTaskCore
    );
    result = (midiTaskHandle != nullptr) ? pdPASS : pdFAIL;
#else
    result = xTaskCreatePinnedToCore(
        midiTask,
        "MidiTask",
        midiTaskStackBytes,
        this,
        19,                // Juste sous MuxTask (20), AU-DESSUS de la pile reseau
                           // (tiT, 18) : a 4, le MIDI prenait 335 µs de retard moyen
                           // sous charge reseau ; a 19, 4 a 15 µs (MESURES §149).
        &midiTaskHandle,
        midiTaskCore
    );
#endif

    if (result == pdPASS) {
        midiTaskStarted = true;
        Serial.printf("[ComponentManager] FreeRTOS MIDI task started on Core %d (free heap: %d)\n", (int)midiTaskCore, (int)ESP.getFreeHeap());
    } else {
        Serial.println("[ComponentManager] ERROR: Failed to create FreeRTOS MIDI task");
    }
    
    // Serial.printf("[ComponentManager] Loaded %d components\n", component_count);
    
    printStats();
}

void ComponentManager::syncOSCConfig() {
    // Récupérer la config de osc_manager (limiter la portée de la String)
    String targetStr = osc_manager.getTargetIP();
    int port = osc_manager.getTargetPort();
    bool broadcast = osc_manager.isBroadcastEnabled();
    
    // Appliquer à osc_queue
    osc_queue.setTarget(targetStr, port);
    osc_queue.setBroadcast(broadcast);
}

void ComponentManager::update() {
    if (!midi_sender) {
        static unsigned long lastLog = 0;
        if (millis() - lastLog > 10000) { // Log toutes les 10s
            Serial.println("[ComponentManager] No MIDI sender configured");
            lastLog = millis();
        }
        // Ne pas return : OSC / queue WS doivent tourner même sans MIDI (USB absent, etc.)
    }

    // Log périodique du nombre de composants
    static unsigned long lastComponentLog = 0;
    // if (millis() - lastComponentLog > 30000) { // Log toutes les 30s
    //     Serial.printf("[ComponentManager] Processing %d components\n", component_count);
    //     for (uint8_t i = 0; i < component_count; i++) {
    //         const ComponentConfig& config = configs[i];
    //         const char* typeName = "Unknown";
    //         switch (config.type) {
    //             case ComponentType::POTENTIOMETER: typeName = "Potentiometer"; break;
    //             case ComponentType::BUTTON: typeName = "Button"; break;
    //             case ComponentType::LED: typeName = "LED"; break;
    //         }
    //         Serial.printf("  [%d] %s on GPIO%d, MIDI ch%d param%d\n", 
    //                      i, typeName, config.gpio, config.midi_channel, config.midi_param);
    //     }
    //     lastComponentLog = millis();
    // }

    syncOSCConfig();
    
    // Traiter les messages OSC entrants (commandes de calibrage)
    osc_manager.update();
    // Puis l'OSC que les scripts ont depose (§176) : la meme tache, le meme objet UDP.
    emettreOscScripts();
    
    // Les multiplexeurs sont maintenant lus par la tâche FreeRTOS sur Core 0
    // Seulement envoyer les batches OSC si la sortie OSC globale est activée (NVS osc_out_all)
    if (osc_output_all_enabled_) {
        mux_manager.sendOscBatches(osc_queue);
    }
    
    // Traiter OSC en priorité (avec queue FreeRTOS)
    osc_queue.update();
    
    // Le traitement des composants directs (non-MUX) est maintenant dans la tâche MIDI sur Core 0
    // On ne garde ici que le traitement réseau/OSC qui doit rester sur Core 1

    /* Drainer la file de telemetrie, remplie par midiTaskLoop sur le coeur 0.
     *
     * ⚠️ Ce commentaire disait : « textAll() est appele ici sur Core 1, ou vit
     * le serveur web — thread-safe ». C'est FAUX : le meme coeur n'est PAS la
     * meme tache — loopTask est preemptee par async_tcp a n'importe quelle
     * instruction. Il a ensuite dit que loopTask etait la seule a modifier la
     * liste de clients : FAUX aussi, async_tcp la modifie a chaque depart
     * d'onglet — la carte en a plante (MESURES §171).
     *
     * La file tient le reseau hors de MidiTask ; l'envoi passe par le registre
     * des onglets, sous son verrou (ServerCore.h, « LES ONGLETS »). On DRAINE
     * dans tous les cas — sinon la file deborde et bloque le producteur. */
    if (telemetryQueue) {
        TelemetryWsMsg tm;
        uint8_t drained = 0;
        while (drained < 32 && xQueueReceive(telemetryQueue, &tm, 0) == pdTRUE) {
            nidmi_ws_envoyer_aux_abonnes(NIDMI_ABO_BROCHES, tm.payload);   // a ceux qui suivent (§200)
            drained++;
        }
    }
}

void ComponentManager::reloadConfigs() {
    {
        Preferences prefs;
        prefs.begin("nidmi", true);
        osc_output_all_enabled_ = prefs.getBool("osc_out_all", true);
        prefs.end();
    }
    marquer("pause");
    bool wdt = pauseRealtimeTasks();
    marquer("clearAll");
    clearAll();
    marquer("mux");
    loadMuxConfigFromNVS();
    marquer("nvs");
    ConfigLoader::loadFromNVS(*this);
    /* APRES le chargement, et c'est tout l'objet du correctif.
     *
     * `rafraichir()` etait appele en TETE de cette fonction — donc avant
     * `clearAll()`, qui vide le registre, et avant `loadFromNVS()`, qui le
     * remplit. Il comptait les composants d'un registre vide et concluait
     * « aucun DAC declare ». Comme il ne compte que la, la conclusion tenait
     * jusqu'au prochain appel, et le seul autre appelant est l'API des broches.
     *
     * CONSEQUENCE MESUREE : apres chaque redemarrage, la carte REFUSAIT le son
     * — « Aucun DAC declare » — alors que le DAC etait bien declare sur D0 dans
     * sa propre liste. Le son ne revenait qu'une fois l'app ouverte sur la zone
     * I/O, dont la lecture de /api/pins/caps rafraichit au passage. Autrement
     * dit : une carte headless qui redemarre revenait MUETTE et le restait,
     * puisqu'il n'y a personne pour ouvrir une page. Exactement ce que la regle
     * premiere interdit — « debranchee, la carte doit continuer a l'identique ».
     *
     * Le comptage doit suivre le chargement, pas le preceder. */
    Occupations::rafraichir();
    marquer("reprise");
    resumeRealtimeTasks(wdt);
    marquer("repos");
}

/* La phase SURVIT au redemarrage.
 *
 * Le chien de garde arme ci-dessous transforme un rechargement bloque en
 * redemarrage — c'est ce qu'on veut d'un instrument sans ecran. Mais le
 * redemarrage effacerait justement la seule trace de l'endroit ou ca a cale, et
 * les deux correctifs se combattraient. La RTC RAM n'est pas remise a zero par
 * un reset ; on y depose la derniere phase atteinte, et /api/pins/actif la
 * publie sous « phase_precedente ». Le blocage se raconte donc lui-meme, apres
 * coup, sans qu'il faille etre au clavier au bon moment. */
RTC_NOINIT_ATTR static char     s_phaseRtc[24];
RTC_NOINIT_ATTR static int32_t  s_phaseRtcI;
RTC_NOINIT_ATTR static uint32_t s_phaseRtcMagie;
static const uint32_t PHASE_MAGIE = 0x4E49444DUL;   // "NIDM"

static char s_phaseAvant[24]  = "";
static int  s_phaseAvantI     = -1;

void ComponentManager::capturerPhasePrecedente() {
    if (s_phaseRtcMagie == PHASE_MAGIE) {
        s_phaseRtc[sizeof(s_phaseRtc) - 1] = '\0';
        strlcpy(s_phaseAvant, s_phaseRtc, sizeof(s_phaseAvant));
        s_phaseAvantI = (int)s_phaseRtcI;
    } else {
        s_phaseAvant[0] = '\0';
        s_phaseAvantI = -1;
    }
    s_phaseRtcMagie = PHASE_MAGIE;
    strlcpy(s_phaseRtc, "demarrage", sizeof(s_phaseRtc));
    s_phaseRtcI = -1;
}

const char* ComponentManager::phaseAvantRedemarrage() { return s_phaseAvant; }
int         ComponentManager::phaseAvantIndice()      { return s_phaseAvantI; }

void ComponentManager::marquer(const char* p, int i) {
    _phase = p;
    _phaseI = i;
    strlcpy(s_phaseRtc, p ? p : "?", sizeof(s_phaseRtc));
    s_phaseRtcI = i;
    if (_chienArme) esp_task_wdt_reset();
}

bool ComponentManager::pauseRealtimeTasks() {
    _nvsWriteInProgress = true;
    /* ARMER le chien de garde sur la tache qui recharge.
     *
     * Constate en direct : un rechargement s'est bloque dans la lecture de la
     * broche D0 et n'en est jamais ressorti — plus de 30 s, zero composant,
     * plus aucun MIDI, et rien pour l'en sortir : cette tache n'etait surveillee
     * par personne. La carte restait morte jusqu'a une intervention humaine, ce
     * qui n'a aucun sens pour un instrument qui tourne sans ecran.
     *
     * Chaque marqueur nourrit le chien (voir marquer()) : un rechargement qui
     * avance n'est jamais interrompu, seul un CALAGE declenche le redemarrage.
     * Ca ne repare pas la cause — ca l'empeche d'etre definitive. */
    _chienArme = (esp_task_wdt_add(NULL) == ESP_OK);
    /* ATTENDRE L'ACQUITTEMENT, au lieu d'esperer que 20 ms suffisent.
     *
     * L'ancienne version posait le drapeau puis dormait 20 ms « le temps que ca
     * se calme ». Ce n'est pas une synchronisation : si la tache temps reel etait
     * dans un analogRead ou dans un envoi MIDI a cet instant, elle y restait
     * pendant que clearAll() liberait ses configurations et que setupGpio()
     * reconfigurait ses broches. Reconfigurer une broche qu'une autre tache est
     * peut-etre en train de lire est faux par construction, que ce soit la cause
     * du calage observe ou non.
     *
     * On attend donc qu'elle DISE qu'elle est garee. Plafond a 200 ms : au-dela
     * on continue quand meme — mieux vaut un rechargement imparfait qu'un
     * rechargement qui ne revient jamais. */
    for (int i = 0; i < 40 && !_tempsReelEnPause; i++) vTaskDelay(pdMS_TO_TICKS(5));
    /* On NE RETIRE PLUS la tache du chien de garde.
     *
     * Elle en etait retiree pour qu'un rechargement un peu long ne le declenche
     * pas. Mais un rechargement qui se BLOQUE devenait alors definitif : la
     * boucle ne revenait jamais, _nvsWriteInProgress restait vrai, les
     * composants restaient VIDES — plus aucun MIDI — et rien ne remettait la
     * carte d'aplomb. Il fallait la debrancher.
     * Le rechargement prend une centaine de millisecondes ; le chien de garde
     * en tolere plusieurs secondes. Le garder arme ne coute rien et transforme
     * un blocage definitif en un redemarrage de deux secondes. */
    vTaskDelay(pdMS_TO_TICKS(20));
    return false;
}

void ComponentManager::resumeRealtimeTasks(bool restoreWdt) {
    _nvsWriteInProgress = false;
    if (_chienArme) { esp_task_wdt_delete(NULL); _chienArme = false; }
    if (restoreWdt) {
        esp_task_wdt_add(xTaskGetCurrentTaskHandle());
        esp_task_wdt_reset();
    }
}




bool ComponentManager::addComponent(uint8_t gpio, ComponentType type, uint8_t midi_param, uint8_t channel, MidiMessageType msg_type, const char* role) {
    if (component_count >= MAX_COMPONENTS) {
        Serial.printf("[ComponentManager] ERROR: Max components reached (%d)\n", MAX_COMPONENTS);
        return false;
    }
    
    // Vérifier que le GPIO est valide (0-48 pour ESP32-C3/S3 OU 200-247 pour MUX)
    bool is_mux_gpio = isMuxGpio(gpio);
    if (gpio >= 255 || (!is_mux_gpio && gpio > 48)) {
        Serial.printf("[ComponentManager] ERROR: Invalid GPIO %d (must be 0-48 or 200-247 for MUX)\n", gpio);
        return false;
    }
    
    /* Broche du bus AUDIO : REFUSER, plutot que de se bloquer dessus.
     *
     * setupGpio() y appelle isPinFloating(), qui sonde la broche en pull-up
     * puis pull-down. Sur le BCK de l'I2S, ca coupe l'horloge de bit : le DMA
     * ne se vide plus et une tache reste dans i2s.write() pour toujours. C'est
     * le calage observe (« phase_precedente = add-gpio:1 »). Une configuration
     * heritee ne doit pas pouvoir remettre la carte dans cet etat — d'ou le
     * refus ici, en plus du grisage cote app (/api/pins/caps declare ce bus). */
    {
        /* UNE BROCHE PRISE PAR SOI-MEME N'EST PAS PRISE.
         *
         * Le DAC declare le bus audio ; au rechargement suivant, ce meme bus
         * refusait de le recharger — les handlers ne sont pas vides entre deux
         * chargements, donc la declaration survivait et se retournait contre son
         * proprietaire. Constate : DAC present en memoire, absent de
         * /api/pins/actif, son coupe. On demande donc au handler du role s'il
         * s'agit de SES broches. */
        bool aSoi = false;
        if (role && *role) {
            ComplexHandler* h = ComplexHandlerRegistry::getHandler(role);
            aSoi = h && h->isGpioUsed(gpio);
        }
        const Occupations::Qui q = aSoi ? Occupations::Qui{nullptr, nullptr}
                                        : Occupations::qui(gpio);
        if (q.bus) {
            Serial.printf("[ComponentManager] GPIO %d refuse : occupe par le bus %s (%s)\n",
                          gpio, q.bus, q.role);
            return false;
        }
    }

    // Vérifier si le GPIO existe déjà
    if (findComponentByGpio(gpio) != 255) {
        Serial.printf("[ComponentManager] WARNING: GPIO %d already exists, skipping\n", gpio);
        return false;
    }
    
    // Validation centralisée via ValidationRegistry
    const ComponentDefinition* def = ComponentRegistry::findByType(type);
    if (def && def->id) {
        // Utiliser ValidationRegistry pour valider le GPIO selon le type de composant
        if (!ValidationRegistry::validate(def->id, gpio)) {
            Serial.printf("[ComponentManager] ERROR: Invalid GPIO %d for component %s\n", gpio, def->id);
            return false;
        }
    } else {
        // Fallback si pas de définition : validation basique
        if (!is_mux_gpio && gpio > 48) {
            Serial.printf("[ComponentManager] ERROR: Invalid GPIO %d\n", gpio);
            return false;
        }
    }
    
    // Ajouter le composant
    ComponentConfig& config = configs[component_count];
    ComponentState& state = states[component_count];
    
    // Initialiser la configuration et l'état avec les valeurs par défaut
    /* Marqueurs : le blocage constate se situe DANS ces trois appels
     * (phase_precedente = "nvs-addComponent"). Les separer nomme le coupable
     * — allocation, etat, ou configuration materielle de la broche. */
    marquer("add-config", gpio);
    ComponentInitializer::initializeConfig(config, gpio, type, midi_param, channel, msg_type);
    marquer("add-etat", gpio);
    ComponentInitializer::initializeState(state);
    marquer("add-gpio", gpio);
    ComponentInitializer::setupGpio(gpio, type, &config);
    marquer("add-fini", gpio);
    
    // Serial.printf("[ComponentManager] Added component: GPIO%d, type=%d, param=%d, channel=%d, msg_type=%d\n",
    //               gpio, (int)type, midi_param, channel, (int)msg_type);
    
    component_count++;
    return true;
}

bool ComponentManager::removeComponent(uint8_t gpio) {
    uint8_t index = findComponentByGpio(gpio);
    if (index == 255) return false;
    
    // Éteindre tous les messages MIDI actifs avant suppression
    const ComponentConfig& config = configs[index];
    ComponentState& state = states[index];
    
    if (midi_sender) {
        if (config.type == ComponentType::IMU) {
            ImuProcessor::silenceNoteSweepForGpio(config.gpio, config, midi_sender);
        }
        if (config.type == ComponentType::JOYSTICK) {
            JoystickProcessor::silenceNoteSweepForGpio(config.gpio, config, midi_sender);
        }
        if (config.type == ComponentType::JOYSTICK3) {
            Joystick3Processor::silenceNoteSweepForGpio(config.gpio, config, midi_sender);
        }
        // 1. NOTE_SWEEP : éteindre la note active
        if (config.msg_type == MidiMessageType::NOTE_SWEEP && state.last_note != 255) {
            midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
        }
        // 2. NOTE ou NOTE_VELOCITY : éteindre si actif
        else if (config.msg_type == MidiMessageType::NOTE || 
                 config.msg_type == MidiMessageType::NOTE_VELOCITY) {
            // Vérifier le mode du bouton
            String btnMode = "press_release"; // Défaut
            if (config.specificConfig.button) {
                btnMode = String(config.specificConfig.button->btnMode);
                if (btnMode.length() == 0) btnMode = "press_release";
            }
            
            uint8_t note = config.midi_param; // midi_param contient la note pour NOTE
            
            // Si mode toggle et état actif, envoyer Note Off
            if (btnMode == "toggle" && state.toggle_state) {
                midi_sender->sendNoteOff(config.midi_channel, note, 0);
            }
            // Si mode press_release et bouton pressé, envoyer Note Off
            else if (btnMode == "press_release" && state.prev_stable_state) {
                midi_sender->sendNoteOff(config.midi_channel, note, 0);
            }
        }
        // 3. CONTROL_CHANGE : remettre à zéro si actif
        else if (config.msg_type == MidiMessageType::CONTROL_CHANGE) {
            // Vérifier le mode du bouton (pour les boutons en CC)
            String btnMode = "press_release"; // Défaut
            if (config.specificConfig.button) {
                btnMode = String(config.specificConfig.button->btnMode);
                if (btnMode.length() == 0) btnMode = "press_release";
            }
            
            // Si mode toggle et état actif, envoyer CC=0
            if (btnMode == "toggle" && state.toggle_state) {
                midi_sender->sendControlChange(config.midi_channel, config.midi_param, 0);
            }
            // Si mode press_release et bouton pressé, envoyer CC=0
            else if (btnMode == "press_release" && state.prev_stable_state) {
                midi_sender->sendControlChange(config.midi_channel, config.midi_param, 0);
            }
            // Pour les potentiomètres en CC, si dernière valeur > 0, remettre à zéro
            // (moins critique mais peut être utile)
            else if (state.last_value > 0) {
                // Optionnel : envoyer CC=0 pour les potentiomètres
                // midi_sender->sendControlChange(config.midi_channel, config.midi_param, 0);
            }
        }
    }
    
    // Déplacer les éléments suivants
    for (uint8_t i = index; i < component_count - 1; i++) {
        configs[i] = configs[i + 1];
        states[i] = states[i + 1];
        filters[i] = filters[i + 1];
    }
    
    component_count--;
    return true;
}

void ComponentManager::clearAll() {
    // Éteindre tous les messages MIDI actifs avant de tout effacer
    for (uint8_t i = 0; i < component_count; i++) {
        marquer("clear-midi", i);
        const ComponentConfig& config = configs[i];
        ComponentState& state = states[i];
        
        if (midi_sender) {
            if (config.type == ComponentType::IMU) {
                ImuProcessor::silenceNoteSweepForGpio(config.gpio, config, midi_sender);
            }
            if (config.type == ComponentType::JOYSTICK) {
                JoystickProcessor::silenceNoteSweepForGpio(config.gpio, config, midi_sender);
            }
            if (config.type == ComponentType::JOYSTICK3) {
                Joystick3Processor::silenceNoteSweepForGpio(config.gpio, config, midi_sender);
            }
            // NOTE_SWEEP : éteindre la note active
            if (config.msg_type == MidiMessageType::NOTE_SWEEP && state.last_note != 255) {
                midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
            }
            // NOTE ou NOTE_VELOCITY : éteindre si actif
            else if (config.msg_type == MidiMessageType::NOTE || 
                     config.msg_type == MidiMessageType::NOTE_VELOCITY) {
                String btnMode = "press_release";
                if (config.specificConfig.button) {
                    btnMode = String(config.specificConfig.button->btnMode);
                    if (btnMode.length() == 0) btnMode = "press_release";
                }
                
                uint8_t note = config.midi_param;
                
                if (btnMode == "toggle" && state.toggle_state) {
                    midi_sender->sendNoteOff(config.midi_channel, note, 0);
                } else if (btnMode == "press_release" && state.prev_stable_state) {
                    midi_sender->sendNoteOff(config.midi_channel, note, 0);
                }
            }
            // CONTROL_CHANGE : remettre à zéro si actif
            else if (config.msg_type == MidiMessageType::CONTROL_CHANGE) {
                String btnMode = "press_release";
                if (config.specificConfig.button) {
                    btnMode = String(config.specificConfig.button->btnMode);
                    if (btnMode.length() == 0) btnMode = "press_release";
                }
                
                if (btnMode == "toggle" && state.toggle_state) {
                    midi_sender->sendControlChange(config.midi_channel, config.midi_param, 0);
                } else if (btnMode == "press_release" && state.prev_stable_state) {
                    midi_sender->sendControlChange(config.midi_channel, config.midi_param, 0);
                }
            }
        }
    }
    /* LIBERER les configurations specifiques.
     *
     * ComponentInitializer les alloue au « new » (ButtonConfig, VelostatConfig,
     * ImuConfig...), et RIEN ne les liberait : le destructeur de
     * ComponentConfig porte un TODO disant qu'on « assume que ComponentManager
     * gere la memoire » — il ne la gerait pas. Chaque rechargement fuyait donc
     * une configuration par composant.
     *
     * Tant que les rechargements etaient rares, la fuite passait inapercue.
     * Depuis qu'un enregistrement de broche recharge (ce qui est necessaire
     * pour qu'une modification prenne effet, MESURES.md §35), CHAQUE
     * modification fuit : ~90 octets pour deux composants, mesures. Au bout de
     * quelques dizaines d'editions le tas ne suffit plus, le rechargement
     * echoue APRES clearAll(), et la carte se retrouve sans aucun composant —
     * plus de MIDI du tout. C'est exactement le symptome signale.
     *
     * On libere selon le TYPE : l'union ne sait pas se detruire seule. */
    for (uint8_t i = 0; i < component_count; i++) {
        marquer("clear-free", i);
        ComponentConfig& c = configs[i];
        if (!c.specificConfig.specific) continue;
        switch (c.type) {
            case ComponentType::BUTTON:        delete c.specificConfig.button;       break;
            case ComponentType::LED:           delete c.specificConfig.led;          break;
            case ComponentType::POTENTIOMETER: delete c.specificConfig.potentiometer;break;
            case ComponentType::VELOSTAT:      delete c.specificConfig.velostat;     break;
            case ComponentType::JOYSTICK:      delete c.specificConfig.joystick;     break;
            case ComponentType::JOYSTICK3:     delete c.specificConfig.joystick3;    break;
            case ComponentType::IMU:           delete c.specificConfig.imu;          break;
            case ComponentType::MPR121:        delete c.specificConfig.mpr121;       break;
            case ComponentType::NOISE_SAMPLER: delete c.specificConfig.noiseSampler; break;
            default:                           operator delete(c.specificConfig.specific); break;
        }
        c.specificConfig.specific = nullptr;
    }

    /* Et le script, dont la configuration est proprietaire depuis qu'il est
     * dimensionne au contenu (§9.3). Meme regle que specificConfig : liberer,
     * puis remettre le pointeur sur la chaine vide — jamais nul, les huit
     * processeurs lisent mappingScript[0] sans se poser de question. */
    for (uint8_t i = 0; i < component_count; i++) {
        ComponentConfig& c = configs[i];
        /* Une reprise differee porte un POINTEUR vers ce texte : l'oublier
         * AVANT de le liberer, sinon le prochain battement rejouerait sur de la
         * memoire rendue. */
        MappingEngine::viderDifferes(c.mappingScript);
        if (c.scriptPossede) { free(c.scriptPossede); c.scriptPossede = nullptr; }
        c.mappingScript = "";
    }

    component_count = 0;
    // Réinitialiser les filtres
    for (uint8_t i = 0; i < MAX_COMPONENTS; i++) {
        filters[i].initialized = false;
    }
}

uint8_t ComponentManager::findComponentByGpio(uint8_t gpio) const {
    for (uint8_t i = 0; i < component_count; i++) {
        if (configs[i].gpio == gpio) return i;
    }
    return 255; // Non trouvé
}


void ComponentManager::saveConfigToNVS() {
    // TODO: Implémenter la sauvegarde si nécessaire
}

const ComponentConfig* ComponentManager::getConfig(uint8_t index) const {
    if (index >= component_count) return nullptr;
    return &configs[index];
}

ComponentConfig* ComponentManager::getConfigMutable(uint8_t index) {
    if (index >= component_count) return nullptr;
    return &configs[index];
}

const ComponentState* ComponentManager::getState(uint8_t index) const {
    if (index >= component_count) return nullptr;
    return &states[index];
}

void ComponentManager::printStats() {
    Serial.println("[ComponentManager] Memory usage:");
    Serial.printf("  Configs: %d bytes (%d components)\n", component_count * sizeof(ComponentConfig), component_count);
    Serial.printf("  States: %d bytes (%d components)\n", component_count * sizeof(ComponentState), component_count);
    Serial.printf("  Filters: %d bytes (%d components)\n", component_count * sizeof(AnalogFilter), component_count);
    Serial.printf("  Total: %d bytes\n", component_count * (sizeof(ComponentConfig) + sizeof(ComponentState) + sizeof(AnalogFilter)));
    
    // Afficher les composants chargés
    for (uint8_t i = 0; i < component_count; i++) {
        const ComponentConfig& config = configs[i];
        String typeStr = "Unknown";
        String msgTypeStr = "Note";
        
        // Utiliser ComponentDefinition pour obtenir le nom court et le type de message par défaut
        const ComponentDefinition* def = ComponentRegistry::findByType(config.type);
        if (def) {
            // Utiliser displayName ou créer un nom court depuis l'ID
            if (def->displayName) {
                // Créer un nom court (premiers 3-4 caractères)
                typeStr = String(def->displayName);
                if (typeStr.length() > 4) {
                    typeStr = typeStr.substring(0, 4);
                }
            } else if (def->id) {
                typeStr = String(def->id);
            }
            
            // Déterminer le type de message par défaut depuis le premier message MIDI
            if (def->midiMessageCount > 0 && def->midiMessages[0].id) {
                String firstMsgId = def->midiMessages[0].id;
                if (firstMsgId == "cc") {
                    msgTypeStr = "CC";
                } else if (firstMsgId == "note" || firstMsgId == "notevel" || firstMsgId == "notesweep") {
                    msgTypeStr = "Note";
                } else if (firstMsgId == "pc") {
                    msgTypeStr = "PC";
                } else {
                    msgTypeStr = "Note"; // Défaut
                }
            }
        } else {
            // Fallback : utiliser le switch case si définition non disponible
            switch (config.type) {
                case ComponentType::POTENTIOMETER: typeStr = "Pot"; msgTypeStr = "CC"; break;
                case ComponentType::BUTTON: typeStr = "Btn"; msgTypeStr = "Note"; break;
                case ComponentType::LED: typeStr = "LED"; msgTypeStr = "Note"; break;
                case ComponentType::BARGRAPH: typeStr = "Bar"; msgTypeStr = "CC"; break;
                case ComponentType::ACTUATOR: typeStr = "Act"; msgTypeStr = "Note"; break;
            }
        }
        
        Serial.printf("  [%d] %s GPIO%d → %s %d (ch%d)\n", 
            i, typeStr.c_str(), config.gpio, 
            msgTypeStr.c_str(),
            config.midi_param, config.midi_channel);
    }
}


void ComponentManager::handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    LedProcessor::handleMidiNoteOn(configs, component_count, channel, note, velocity);
}

void ComponentManager::handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    LedProcessor::handleMidiNoteOff(configs, component_count, channel, note, velocity);
}

void ComponentManager::handleMidiKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
    LedProcessor::handleMidiKeyPressure(configs, component_count, channel, note, pressure);
}

void ComponentManager::handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value) {
    LedProcessor::handleMidiControlChange(configs, component_count, channel, control, value);
}

// ============================================================================
// Gestion des multiplexeurs analogiques
// ============================================================================

bool ComponentManager::addMux(uint8_t mux_id, uint8_t sig, uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                              uint8_t en, uint16_t analog_min, uint16_t analog_max,
                              bool hysteresis_enabled, MuxOSCFormat osc_format, uint8_t filter_intensity,
                              uint8_t cc_base, uint8_t midi_channel, const char* osc_base) {
    if (mux_id >= MAX_MUXES) {
        Serial.printf("[ComponentManager] Mux ID %d invalide (max %d)\n", mux_id, MAX_MUXES - 1);
        return false;
    }
    
    // Valider les pins GPIO
    MuxValidator::ValidationResult pinResult = MuxValidator::validatePins(sig, s0, s1, s2, s3, en);
    if (!pinResult.valid) {
        Serial.printf("[ComponentManager] %s\n", pinResult.error_message.c_str());
        return false;
    }
    
    // Valider les seuils
    MuxValidator::ValidationResult thresholdResult = MuxValidator::validateThresholds(analog_min, analog_max);
    if (!thresholdResult.valid) {
        Serial.printf("[ComponentManager] %s\n", thresholdResult.error_message.c_str());
        return false;
    }
    
    // Supprimer les composants existants sur les pins du MUX
    MuxValidator::removeExistingComponents(*this, sig, s0, s1, s2, s3, en);
    
    // Normaliser les paramètres MIDI
    MuxValidator::normalizeMidiParams(cc_base, midi_channel);
    
    return mux_manager.addMux(mux_id, sig, s0, s1, s2, s3, en,
                              analog_min, analog_max, hysteresis_enabled,
                              osc_format, filter_intensity, cc_base, midi_channel, osc_base);
}

bool ComponentManager::removeMux(uint8_t mux_id) {
    if (mux_id >= MAX_MUXES) return false;
    return mux_manager.removeMux(mux_id);
}

const MuxConfig* ComponentManager::getMuxConfig(uint8_t mux_id) const {
    return mux_manager.getMuxConfig(mux_id);
}

void ComponentManager::updateMuxCache(uint8_t mux_id) {
    mux_manager.updateMuxCache(mux_id);
}

bool ComponentManager::readMuxAllChannels(uint8_t mux_id, uint16_t* values) {
    return mux_manager.readMuxAllChannels(mux_id, values);
}

uint16_t ComponentManager::readMuxChannel(uint8_t gpio) {
    return mux_manager.readMuxChannel(gpio);
}

bool ComponentManager::calibrateMux(uint8_t mux_id, uint8_t channel, bool is_min, bool all_channels) {
    return mux_manager.calibrateMux(mux_id, channel, is_min, all_channels, osc_queue);
}

bool ComponentManager::resetMuxThresholds(uint8_t mux_id, uint8_t channel, bool all_channels) {
    return mux_manager.resetMuxThresholds(mux_id, channel, all_channels, osc_queue);
}

void ComponentManager::loadMuxConfigFromNVS() {
    mux_manager.loadMuxConfigFromNVS();
}

void ComponentManager::midiTask(void* parameter) {
    ComponentManager* instance = static_cast<ComponentManager*>(parameter);
    instance->midiTaskLoop();
}

uint32_t g_margePileMidi = 0;   // cf. /api/audio/status

/* ── GIGUE D'ORDONNANCEMENT ────────────────────────────────────────────────
 * Le TOUR de cette tache a une periode fixe (10 ms, a la milliseconde) ; entre
 * deux tours elle traite le MIDI entrant des qu'il arrive (§170). Si une autre
 * tache plus prioritaire la preempte, l'intervalle reel s'allonge — et c'est
 * une note en retard. Elle etait a 4, sous le serveur web (10) : il la
 * preemptait par construction ; elle est a 19 depuis le §149. La mesure reste :
 * on juge l'ordonnancement sur un prejudice constate, pas sur la foi d'un
 * tableau de priorites.
 * On mesure l'ECART a la periode visee, pas l'intervalle : c'est le retard qui
 * s'entend. Un tour en retard est rattrape au suivant, donc un tour long est
 * suivi d'un tour court — les deux comptent comme de la gigue.            */
volatile uint32_t g_gigueMidiMaxUs   = 0;   // pire ecart a 10 ms, en us
volatile uint32_t g_gigueMidiTours   = 0;   // tours comptes dans la fenetre
volatile uint32_t g_gigueMidiRetards = 0;   // tours ou l'ecart depasse 5 ms
volatile uint32_t g_gigueMidiCumulUs = 0;   // somme des ecarts, pour la moyenne
/* WHEN, in ms since boot: the worst tick, the first and the last late one — a stall that
 * happens at boot looks the same as one that happens in the middle of a performance in the
 * counters, and only the time tells which (§205). */
volatile uint32_t g_gigueMidiMaxAtMs = 0, g_gigueMidiFirstLateMs = 0, g_gigueMidiLastLateMs = 0;
void nidmi_gigue_midi_reset(){
    g_gigueMidiMaxUs = g_gigueMidiTours = g_gigueMidiRetards = g_gigueMidiCumulUs = 0;
    g_gigueMidiMaxAtMs = g_gigueMidiFirstLateMs = g_gigueMidiLastLateMs = 0;
}

void ComponentManager::midiTaskLoop() {
    const TickType_t xFrequency = pdMS_TO_TICKS(10); // 10ms
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const uint32_t PERIODE_US = 10000;
    uint32_t precedent = 0;                 // us du tour precedent, 0 = amorcage

    for(;;) {
        if (_nvsWriteInProgress || !midi_sender) {
            _tempsReelEnPause = true;      // acquittement lu par pauseRealtimeTasks()
            vTaskDelay(pdMS_TO_TICKS(10));
            precedent = 0;   // une PAUSE n'est pas de la gigue : on repart a zero
            xLastWakeTime = xTaskGetTickCount();   // ... et on ne rattrape pas les tours manques
            continue;
        }
        _tempsReelEnPause = false;

        /* ── LE MIDI ENTRANT N'ATTEND PAS LE TOUR (MESURES §170) ───────────
         * Entre deux tours, la tache DORT SUR LA FILE D'ENTREE au lieu de
         * dormir tout court : un message MIDI arrive par l'USB, elle se
         * reveille et le traite aussitot — scripts, son, composants — puis se
         * rendort jusqu'au tour. Le tour garde sa periode de 10 ms, a la
         * milliseconde pres, comme avec vTaskDelayUntil (qui rattrapait de la
         * meme facon un tour en retard). */
        const TickType_t prochainTour = xLastWakeTime + xFrequency;
        const TickType_t maintenant = xTaskGetTickCount();
        if ((int32_t)(prochainTour - maintenant) > 0) {
            serverCore.usbMidi().traiterEntree(prochainTour - maintenant);
            continue;   // un message, ou l'heure du tour : on re-evalue
        }
        xLastWakeTime = prochainTour;
        serverCore.usbMidi().traiterEntree(0);   // ce qui vient d'arriver passe avant le tour

        /* L'ecart a la periode visee. `precedent == 0` = premier tour, ou reprise
         * apres pause : on ne mesure rien, on amorce. */
        const uint32_t maintenantUs = micros();
        if (precedent) {
            const uint32_t d = maintenantUs - precedent;           // intervalle reel
            const uint32_t ecart = (d > PERIODE_US) ? (d - PERIODE_US) : (PERIODE_US - d);
            if (ecart > g_gigueMidiMaxUs) { g_gigueMidiMaxUs = ecart; g_gigueMidiMaxAtMs = millis(); }
            if (ecart > 5000) {
                g_gigueMidiRetards++;
                if (!g_gigueMidiFirstLateMs) g_gigueMidiFirstLateMs = millis();
                g_gigueMidiLastLateMs = millis();
            }
            g_gigueMidiCumulUs += ecart;
            g_gigueMidiTours++;
        }
        precedent = maintenantUs;
        
        /* Le battement d'horloge du script map (metro, loadbang).
         * Ici plutot que dans la boucle principale : cette tache tourne a
         * periode FIXE (10 ms) et ne s'est jamais bloquee, alors que la boucle
         * l'a fait (§42). Une horloge qui derive ou s'arrete est pire que pas
         * d'horloge du tout. */
        /* MARGE DE PILE de cette tache, en octets. Publiee par /api/audio/status.
         * Les tableaux de sortie du moteur de script vivent sur CETTE pile ;
         * avant de les agrandir — une sortie OSC porte une adresse texte — il
         * faut savoir ce qui reste. Mesure, pas estimation. */
        g_margePileMidi = (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);

        /* ── LE MIDI ENTRANT PASSE ENTRE LES ETAPES DU TOUR (MESURES §173) ──
         * Un message arrive pendant le tour attendait la fin du tour ENTIER :
         * l'horloge des scripts, les differes, les multiplexeurs, toutes les
         * broches — jusqu'a ~3,7 ms sous charge. Il passe desormais entre deux
         * etapes, et avant chaque broche : il n'attend plus que l'etape en
         * cours. Les chronos des etapes n'en comptent pas le temps. */
        UsbMidiManager& usb = serverCore.usbMidi();
        uint32_t tEtape = micros();
        g_midiRouter.battreHorloge(millis());
        Chronos::horloge.noter(micros() - tEtape);
        usb.traiterEntree(0);
        tEtape = micros();
        /* Et la file des differes — del(), makenote()... — une seule fois pour
         * TOUS les scripts : broches comprises, qui n'ont pas d'horloge a elles. */
        MappingEngine::battreDifferes(millis(), &g_midiRouter);
        Chronos::differes.noter(micros() - tEtape);
        usb.traiterEntree(0);
        tEtape = micros();

        // Envoyer les mises à jour MIDI des multiplexeurs
        mux_manager.sendMidiUpdates(midi_sender);
        Chronos::mux.noter(micros() - tEtape);
        usb.traiterEntree(0);
        tEtape = micros();
        uint32_t tMidiEntreBroches = 0;   // soustrait du chrono des composants
        
        // Traiter les composants directs (potentiomètres, boutons, touch, etc.)
        // Round-robin: on ne traite qu'un sous-ensemble par cycle pour éviter de bloquer le CPU
        static uint8_t next_component_index = 0;
        const uint8_t MAX_COMPONENTS_PER_CYCLE = 4;
        
        uint8_t total = component_count;
        if (total > 0) {
            if (next_component_index >= total) {
                next_component_index = 0;
            }
            
            uint8_t processed = 0;
            uint8_t index = next_component_index;
            
            while (processed < MAX_COMPONENTS_PER_CYCLE && processed < total) {
                if (index >= total) {
                    index = 0;
                }
                /* Avant chaque broche, le MIDI arrive pendant la precedente. */
                if (processed) {
                    const uint32_t tm = micros();
                    usb.traiterEntree(0);
                    tMidiEntreBroches += micros() - tm;
                }
                
                // Vérifier que le composant est valide avant de le traiter
                const ComponentConfig& config = configs[index];
                /* Vérifier GPIO valide : 0-48 pour pins normales OU 200-247 pour MUX */
                bool is_mux_gpio = isMuxGpio(config.gpio);
                if (config.gpio < 255 && (is_mux_gpio || config.gpio <= 48)) {
                    // Utiliser le registre de processeurs (extensible pour des centaines de composants)
                    // Logique générique : si le GPIO a un ADC ou Touch, utiliser un filtre analogique
                    // (indépendamment de la définition du composant - basé sur les capacités matérielles)
                    const ComponentDefinition* def = ComponentRegistry::findByType(config.type);
                    AnalogFilter* filter_ptr = nullptr;
                    
                    // Les composants IMU (I2C) ont besoin d'un filtre même sans ADC
                    if (config.type == ComponentType::IMU) {
                        filter_ptr = &filters[index];
                    } else if (PinMapper::hasAdc(config.gpio)) {
                        // GPIO avec ADC : utiliser un filtre analogique
                        filter_ptr = &filters[index];
                    } else if (PinMapper::hasTouch(config.gpio)) {
                        // GPIO avec Touch : utiliser aussi un filtre analogique (touchRead() retourne une valeur analogique)
                        filter_ptr = &filters[index];
                    } else if (def && def->pinType == PinType::PIN_ANALOG) {
                        // Si la définition indique PIN_ANALOG mais le GPIO n'a ni ADC ni Touch, ignorer
                        filter_ptr = nullptr;
                    }
                    
                    /* LE DIAGNOSTIC INFORME, IL NE DECIDE PLUS.
                     *
                     * Ici, un capteur juge « en l'air » au setup etait SAUTE —
                     * son script compris — pour ne pas emettre le bruit d'une
                     * entree flottante. Deux raisons de ne plus le faire :
                     *
                     *  - le test se trompe. Les axes des joysticks en ont ete
                     *    EXCLUS pour cette raison (ComponentInitializer.cpp), et
                     *    il a ensuite museles un potentiometre parfaitement
                     *    cable : lecture 2205/4095, amplitude 1,4 % — une entree
                     *    flottante ne se tient pas ainsi. Il s'est meme
                     *    contredit d'un demarrage a l'autre sur la meme broche ;
                     *  - il decidait EN SILENCE. Une broche muette ressemblait a
                     *    un script qui ne marche pas, et rien ne permettait de
                     *    distinguer les deux sans un build special.
                     *
                     * Le verdict reste calcule et publie (`jugee_en_l_air` de
                     * /api/pins/actif et /api/pins/list) : il vaut comme
                     * AVERTISSEMENT a l'ecran. Le composant, lui, tourne. */

                    // Appeler le processeur enregistré pour ce type de composant
                    if (filter_ptr || !def || def->pinType != PinType::PIN_ANALOG) {
                        /* L'ACTIVITE DE LA BROCHE (§167), en mode MIDI : le
                         * processeur date une valeur utile (`last_telemetry_ts`) ;
                         * datee pendant CET appel, elle allume la LED de la
                         * broche dans l'app, et celle du bouton I/O. Pas de
                         * tableau de plus en RAM interne : on compare a l'heure
                         * d'avant l'appel. En mode SCRIPT, tous ne la datent pas
                         * (le potentiometre, non) : c'est executerCapteur qui
                         * note la broche (MappingEngine.cpp). */
                        const uint32_t avantProcesseur = millis();
                        const bool traite = ProcessorRegistry::process(config.type, configs[index], states[index], filter_ptr, midi_sender, osc_queue);
                        const uint32_t dateValeur = states[index].last_telemetry_ts;
                        if (dateValeur && (int32_t)(dateValeur - avantProcesseur) >= 0)
                            Activite::noterBroche(configs[index].gpio);
                        if (!traite) {
                            // Processeur non enregistré (ne devrait pas arriver si tous les processeurs sont chargés)
                            static unsigned long last_warning = 0;
                            if (millis() - last_warning > 5000) {  // Limiter les warnings
                                Serial.printf("[ComponentManager] WARNING: No processor registered for component type %d (GPIO:%d)\n", 
                                             static_cast<int>(config.type), configs[index].gpio);
                                last_warning = millis();
                            }
                        }
                    }

                    // --- Télémétrie WebSocket pour le monitoring SVG ---
                    // LED d’activité = flash bref quand une valeur pertinente change.
                    // Le front éteint via un decay côté navigateur.
                    if (!nidmi_ws_abonne(NIDMI_ABO_BROCHES)) {
                        // Aucun onglet ne suit les broches (MESURES §200) : ne rien émettre.
                        index++;
                        processed++;
                        continue;
                    }
                    const ComponentConfig& cfg = configs[index];
                    ComponentState& st = states[index];

                    auto shouldShowValue = [&](ComponentType t) -> bool {
                        switch (t) {
                            case ComponentType::MPR121:
                            case ComponentType::TOUCH:
                            case ComponentType::BUTTON:
                            case ComponentType::POTENTIOMETER:
                            case ComponentType::VELOSTAT:
                            case ComponentType::ULTRASONIC:
                            case ComponentType::JOYSTICK:
                            case ComponentType::JOYSTICK3:
                                return true;
                            default:
                                return false; // LED activity only
                        }
                    };

                    auto sendTelemetry = [&](const String& pinLabel, uint32_t raw, uint8_t midi, uint32_t ts, bool show_value) {
                        if (!telemetryQueue) return;
                        TelemetryWsMsg tm;
                        snprintf(tm.payload, sizeof(tm.payload),
                                 "PIN_TELEMETRY:%s:{\"raw\":%lu,\"midi\":%u,\"active\":1,\"ts\":%lu,\"show_value\":%s}",
                                 pinLabel.c_str(), (unsigned long)raw, (unsigned)midi,
                                 (unsigned long)ts, show_value ? "true" : "false");
                        if (xQueueSend(telemetryQueue, &tm, 0) != pdTRUE) {
                            telemetryDropCount++;
                        }
                    };

                    auto sendToPhysicalLabels2 = [&](const char* l1, const char* l2, uint32_t raw, uint8_t midi, uint32_t ts, bool show_value) {
                        uint8_t g1 = PinMapper::labelToGpio(l1);
                        uint8_t g2 = PinMapper::labelToGpio(l2);
                        if (g1 != 255) sendTelemetry(String(l1), raw, midi, ts, show_value);
                        if (g2 != 255) sendTelemetry(String(l2), raw, midi, ts, show_value);
                    };

                    auto sendMainIfUpdated = [&]() {
                        if (st.last_telemetry_ts == 0) return;
                        if (st.last_telemetry_ts == last_telemetry_sent_ts[index]) return;

                        uint32_t raw = st.last_raw_value_u32;
                        uint8_t midi = st.last_midi_value_u8;
                        uint32_t ts = st.last_telemetry_ts;
                        bool show_value = shouldShowValue(cfg.type);

                        if (cfg.type == ComponentType::MPR121) {
                            // MPR121 est sur le bus I2C: afficher sur SDA + SCL
                            sendToPhysicalLabels2("SDA", "SCL", raw, midi, ts, show_value);
                        } else if (cfg.type == ComponentType::IMU && cfg.specificConfig.imu) {
                            // IMU: activité LED seulement, bus dépend de bus_interface
                            bool use_spi = (cfg.specificConfig.imu->bus_interface == 1);
                            if (use_spi) {
                                // SPI: afficher sur MOSI/MISO/SCK
                                uint8_t mosi = PinMapper::labelToGpio("MOSI");
                                uint8_t miso = PinMapper::labelToGpio("MISO");
                                uint8_t sck  = PinMapper::labelToGpio("SCK");
                                if (mosi != 255) sendTelemetry("MOSI", raw, midi, ts, false);
                                if (miso != 255) sendTelemetry("MISO", raw, midi, ts, false);
                                if (sck  != 255) sendTelemetry("SCK", raw, midi, ts, false);
                            } else {
                                // I2C: afficher sur SDA + SCL
                                sendToPhysicalLabels2("SDA", "SCL", raw, midi, ts, false);
                            }
                        } else if (cfg.type == ComponentType::JOYSTICK || cfg.type == ComponentType::JOYSTICK3) {
                            // Joystick: afficher la valeur sur l’axe X (main gpio)
                            String pinLabel = PinMapper::gpioToLabel(cfg.gpio);
                            if (pinLabel.length() > 0) sendTelemetry(pinLabel, raw, midi, ts, true);
                        } else {
                            // Cas standard: valeur sur le rectangle correspondant à gpio principal
                            String pinLabel = PinMapper::gpioToLabel(cfg.gpio);
                            if (pinLabel.length() > 0) sendTelemetry(pinLabel, raw, midi, ts, show_value);
                        }

                        last_telemetry_sent_ts[index] = st.last_telemetry_ts;
                    };

                    auto sendAuxIfUpdated = [&]() {
                        if (st.aux_gpio == 255) return;
                        if (st.last_telemetry_ts_aux == 0) return;
                        if (st.last_telemetry_ts_aux == last_telemetry_sent_ts_aux[index]) return;

                        uint32_t raw = st.last_raw_value_aux_u32;
                        uint8_t midi = st.last_midi_value_aux_u8;
                        uint32_t ts = st.last_telemetry_ts_aux;

                        String pinLabel = PinMapper::gpioToLabel(st.aux_gpio);
                        if (pinLabel.length() > 0) sendTelemetry(pinLabel, raw, midi, ts, true);

                        last_telemetry_sent_ts_aux[index] = st.last_telemetry_ts_aux;
                    };

                    auto sendAux2IfUpdated = [&]() {
                        if (st.aux_gpio2 == 255) return;
                        if (st.last_telemetry_ts_aux2 == 0) return;
                        if (st.last_telemetry_ts_aux2 == last_telemetry_sent_ts_aux2[index]) return;

                        uint32_t raw = st.last_raw_value_aux2_u32;
                        uint8_t midi = st.last_midi_value_aux2_u8;
                        uint32_t ts = st.last_telemetry_ts_aux2;

                        String pinLabel = PinMapper::gpioToLabel(st.aux_gpio2);
                        if (pinLabel.length() > 0) sendTelemetry(pinLabel, raw, midi, ts, true);

                        last_telemetry_sent_ts_aux2[index] = st.last_telemetry_ts_aux2;
                    };

                    sendMainIfUpdated();
                    sendAuxIfUpdated();
                    sendAux2IfUpdated();
                }
                
                index++;
                processed++;
            }
            
            next_component_index = index;
        }
        Chronos::composants.noter(micros() - tEtape - tMidiEntreBroches);
        // La periode suivante s'attend EN TETE de boucle, sur la file d'entree.
    }
}
