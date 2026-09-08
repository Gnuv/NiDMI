#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "../utils/PinMapper.h"
#include "../midi/MidiSender.h"
#include "../midi/MidiMessageType.h"
#include "../osc/OSCManager.h"
#include "../osc/OSCQueue.h"
#include "../utils/Hysteresis.h"
#include "../utils/JSONParser.h"
#include "../utils/AnalogFilter.h"
#include "../components/ComponentTypes.h"  // Définitions communes (ComponentType, ComponentConfig, ComponentState)
#include "MuxManager.h"

/**
 * @brief Manager des composants avec architecture template optimisée
 * 
 * Implémente l'architecture de ARCHITECTURE_MIDI.md :
 * - Structures compactes (8 bytes config + 12 bytes state)
 * - Filtrage analogique adaptatif
 * - Anti-rebond intelligent
 * - Support multi-MCU via PinMapper
 * - Optimisation mémoire (75% de réduction)
 */
class ComponentManager {
private:
    /* MAX_COMPONENTS : 24, et c'est un choix de MEMOIRE.
     *
     * Chaque emplacement coute ~440 octets de RAM interne (config, etat,
     * filtre, trois horodatages de telemetrie). A 64, l'objet pesait 28 ko —
     * sur une carte qui vit avec une trentaine de kilo-octets de tas libre.
     * Servir l'interface embarquee (559 ko, une cinquantaine de requetes),
     * recharger une configuration et tenir l'audio dans ce qui restait la
     * mettait a genoux : des routes qui n'aboutissent plus, puis un blocage.
     *
     * 24 emplacements liberent ~17,6 ko. Ils permettent un multiplexeur 16
     * voies plus huit composants directs, ce qui couvre les boitiers vises.
     * Deux multiplexeurs demanderaient de remonter ce nombre — et de le payer
     * en tas. Le compromis se decide ici, en connaissance de cause. */
    static constexpr uint8_t MAX_COMPONENTS = 24;
    
    ComponentConfig configs[MAX_COMPONENTS];
    ComponentState states[MAX_COMPONENTS];
    uint8_t component_count;
    MidiSender* midi_sender;
    OSCManager osc_manager;
    OSCQueue osc_queue;
    
    // Multiplexeurs analogiques
    MuxManager mux_manager;
    
    AnalogFilter filters[MAX_COMPONENTS];

    // Dernière télémétrie SVG envoyée (per component index)
    uint32_t last_telemetry_sent_ts[MAX_COMPONENTS];
    uint32_t last_telemetry_sent_ts_aux[MAX_COMPONENTS];
    uint32_t last_telemetry_sent_ts_aux2[MAX_COMPONENTS];
    
    // FreeRTOS task pour traitement composants et MIDI sur Core 0
    TaskHandle_t midiTaskHandle;
    bool midiTaskStarted;
    static void midiTask(void* parameter);
    void midiTaskLoop();
    
    struct TelemetryWsMsg {
        char payload[192];
    };
    QueueHandle_t telemetryQueue;
    uint32_t telemetryDropCount;

    // Garde NVS : les tâches temps réel vérifient ce flag et yieldent
    volatile bool _nvsWriteInProgress = false;
    const char* volatile _phase = "repos";
    volatile int _phaseI = -1;

    /** Master switch OSC sortie (NVS osc_out_all), rechargé dans reloadConfigs() */
    bool osc_output_all_enabled_ = true;
    
public:
    bool isNvsWriteInProgress() const { return _nvsWriteInProgress; }

    /* OU en est un rechargement.
     *
     * Un rechargement qui se bloque laisse la carte SANS composants — plus
     * aucun MIDI — et la seule chose visible de l'exterieur etait « zero
     * composant », identique a « rien a charger ». Ces deux champs nomment
     * l'etape en cours ; /api/pins/actif les publie. Deux ecritures de pointeur
     * par etape, aucun cout mesurable. */
    const char* phaseRechargement() const { return _phase; }
    int         phaseIndice()       const { return _phaseI; }
    void        marquer(const char* p, int i = -1) { _phase = p; _phaseI = i; }
private:
    
public:
    ComponentManager();
    ~ComponentManager();
    
    void begin(MidiSender* sender);
    void update();
    void reloadConfigs();
    void syncOSCConfig();
    // Gestion des composants
    bool addComponent(uint8_t gpio, ComponentType type, uint8_t midi_param, uint8_t channel, MidiMessageType msg_type = MidiMessageType::NOTE);
    bool removeComponent(uint8_t gpio);
    void clearAll();
    
    // Réception MIDI pour piloter les LEDs
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value);
    void handleMidiKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure);
    
    // Getters
    uint8_t getComponentCount() const { return component_count; }
    const ComponentConfig* getConfig(uint8_t index) const;
    ComponentConfig* getConfigMutable(uint8_t index); // Pour ConfigLoader
    const ComponentState* getState(uint8_t index) const;
    
    // Gestion des multiplexeurs
    bool addMux(uint8_t mux_id, uint8_t sig, uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                uint8_t en = 255, uint16_t analog_min = 0, uint16_t analog_max = 4095,
                bool hysteresis_enabled = true, MuxOSCFormat osc_format = MuxOSCFormat::FLOAT,
                uint8_t filter_intensity = 5, uint8_t cc_base = 1, uint8_t midi_channel = 1,
                const char* osc_base = nullptr);
    bool removeMux(uint8_t mux_id);
    uint8_t getMuxCount() const { return mux_manager.getMuxCount(); }
    const MuxConfig* getMuxConfig(uint8_t mux_id) const;
    bool isMuxGpio(uint8_t gpio) const { return mux_manager.isMuxGpio(gpio); }
    uint16_t readMuxChannel(uint8_t gpio);
    
    // Lecture batch optimisée de tous les canaux d'un MUX
    bool readMuxAllChannels(uint8_t mux_id, uint16_t* values);
    void updateMuxCache(uint8_t mux_id); // Mettre à jour le cache MUX
    
    // Calibrage OSC des seuils min/max
    bool calibrateMux(uint8_t mux_id, uint8_t channel, bool is_min, bool all_channels);
    bool resetMuxThresholds(uint8_t mux_id, uint8_t channel, bool all_channels);
    
    // Protection NVS : suspendre/reprendre les tâches temps réel
    // Appeler avant/après toute écriture NVS pour éviter les stalls flash
    bool pauseRealtimeTasks();
    void resumeRealtimeTasks(bool restoreWdt);
    
    // Debug
    void printStats();
    
    
    // Utilitaires
    uint8_t findComponentByGpio(uint8_t gpio) const;
    void loadMuxConfigFromNVS();
    void saveConfigToNVS();
    
};
