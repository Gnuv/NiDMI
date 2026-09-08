#include "ComponentManager.h"
#include "../server/WebDebugConsole.h"
#include <Arduino.h> // For Serial.printf
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "../server/ServerCore.h"
#include "../osc/OSCQueue.h"
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
#include "../Globals.h"
#include "../components/motion/Lis3dhDef.h"

extern volatile bool g_pinMonitoringEnabled;

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

void ComponentManager::begin(MidiSender* sender) {
    midi_sender = sender;
    telemetryQueue = xQueueCreate(32, sizeof(TelemetryWsMsg));  // 8->32 : moins d'overflow sur capteurs actifs
    /* Charger d'abord les MUX */
    loadMuxConfigFromNVS();
    /* Puis charger les configs des pins */
    ConfigLoader::loadFromNVS(*this);
    
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
        4,                 // Priorité légèrement inférieure à MuxTask
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
        4,                 // Priorité légèrement inférieure à MuxTask
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
    
    // Les multiplexeurs sont maintenant lus par la tâche FreeRTOS sur Core 0
    // Seulement envoyer les batches OSC si la sortie OSC globale est activée (NVS osc_out_all)
    if (osc_output_all_enabled_) {
        mux_manager.sendOscBatches(osc_queue);
    }
    
    // Traiter OSC en priorité (avec queue FreeRTOS)
    osc_queue.update();
    
    // Le traitement des composants directs (non-MUX) est maintenant dans la tâche MIDI sur Core 0
    // On ne garde ici que le traitement réseau/OSC qui doit rester sur Core 1

    // Drainer la queue télémétrie (remplie par midiTaskLoop sur Core 0)
    // textAll() est appelé ici sur Core 1, où vit le serveur web — thread-safe.
    if (telemetryQueue) {
        TelemetryWsMsg tm;
        uint8_t drained = 0;
        while (drained < 32 && xQueueReceive(telemetryQueue, &tm, 0) == pdTRUE) {
            serverCore.websocket().textAll(tm.payload);
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




bool ComponentManager::addComponent(uint8_t gpio, ComponentType type, uint8_t midi_param, uint8_t channel, MidiMessageType msg_type) {
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

void ComponentManager::midiTaskLoop() {
    const TickType_t xFrequency = pdMS_TO_TICKS(10); // 10ms
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    for(;;) {
        if (_nvsWriteInProgress || !midi_sender) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Envoyer les mises à jour MIDI des multiplexeurs
        mux_manager.sendMidiUpdates(midi_sender);
        
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
                    
                    // Capteur mono-pin dont la pin a été détectée dans le vide au setup
                    // (voir ComponentInitializer::setupGpio) : on garde la config mais on
                    // n'émet rien, plutôt que d'envoyer le bruit d'une entrée flottante.
                    // Les joysticks ne lèvent jamais ce drapeau (exclus de la détection).
                    if (config.pin_disconnected) {
                        index++;
                        processed++;
                        continue;
                    }

                    // Appeler le processeur enregistré pour ce type de composant
                    if (filter_ptr || !def || def->pinType != PinType::PIN_ANALOG) {
                        if (!ProcessorRegistry::process(config.type, configs[index], states[index], filter_ptr, midi_sender, osc_queue)) {
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
                    if (!g_pinMonitoringEnabled) {
                        // Monitoring désactivé: ne rien émettre.
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
        
        // Attendre jusqu'à la prochaine période (10ms)
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
