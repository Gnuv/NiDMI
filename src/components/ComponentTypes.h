#pragma once

#include "../mapping/MappingEngine.h"

#include <Arduino.h>
#include "../midi/MidiMessageType.h"
#include "../utils/Hysteresis.h"

/**
 * @file ComponentTypes.h
 * @brief Définitions communes pour les composants
 * 
 * Ce fichier centralise les types et structures utilisés par tous les processeurs
 * et le ComponentManager. Cela évite les dépendances circulaires et facilite
 * l'ajout de nouveaux types de composants.
 */

// Types de composants supportés
enum class ComponentType : uint8_t {
    POTENTIOMETER = 0,
    BUTTON        = 1,
    LED           = 2,
    MUX           = 3,
    VELOSTAT      = 4,
    ULTRASONIC    = 5,
    TOUCH         = 6,
    ACTUATOR      = 7,  // Actuateurs de sortie (relais, buzzers, solénoïdes, etc.)
    BARGRAPH      = 8,  // Bargraph LED (10 segments) - réutilise LedProcessor pour l'instant
    JOYSTICK      = 9,  // Joystick 2 axes analogiques
    IMU           = 10, // IMU (accéléromètre, gyroscope, etc.)
    MPR121        = 11, // Touch capacitif 12 canaux (Grove, I2C)
    NOISE_SAMPLER = 12, // Source signal : bruit blanc externe échantillonné sur ADC (famille SIGNAL)
    JOYSTICK3     = 13, // Joystick 3 axes analogiques (X/Y/Z)
    DAC_I2S       = 14  // Sortie audio I2S (BCK/LRCK/DIN) : un peripherique qu'on DECLARE
    // Facilement extensible pour de nouveaux types
};

// Mode MIDI pour un composant (RTP config vs Mapping script)
enum class MidiMode : uint8_t {
    RTP = 0,    // Utilise la configuration RTP-MIDI classique
    SCRIPT = 1  // Utilise le script de mapping local
};

// Types de pins supportés par un composant
enum class PinType : uint8_t {
    PIN_ANALOG = 0,          // Pin analogique uniquement (ADC)
    PIN_DIGITAL = 1,         // Pin digitale uniquement
    PIN_ANALOG_OR_DIGITAL = 2, // Les deux sont acceptés
    PIN_PWM = 3,             // Pin avec capacité PWM
    PIN_I2C = 4,             // Bus I2C (SDA/SCL)
    PIN_SPI = 5              // Bus SPI (MOSI/MISO/SCK)
};

// Forward declarations des configurations spécifiques
// (définies dans leurs headers respectifs)
namespace Components {
    struct ButtonConfig;
    struct LedConfig;
    struct PotentiometerConfig;
    struct VelostatConfig;
    struct JoystickConfig;
    struct Joystick3Config;
    struct ImuConfig;
    struct Mpr121Config;
    struct NoiseSamplerConfig;
}

// Configuration optimisée d'un composant
struct ComponentConfig {
    uint8_t gpio;           // Pin GPIO
    ComponentType type;     // Type de composant
    uint8_t midi_param;    // CC/Note/Program number
    uint8_t midi_channel;  // Canal MIDI (1-16)
    MidiMessageType msg_type; // Type de message MIDI
    uint8_t flags;         // Flags (rtp_enabled, etc.)
    char osc_address[32];  // Adresse OSC par pin (ex: /ctl, /note, /led)
    uint8_t rtpNoteMin;    // Note min pour balayage (NOTE_SWEEP)
    uint8_t rtpNoteMax;   // Note max pour balayage (NOTE_SWEEP)
    uint8_t rtpNoteVelFix; // Vélocité fixe pour balayage (NOTE_SWEEP)
    uint16_t rtpNoteSweepAutoOffDelay; // Auto-off balayage (ms). 0 = timer désactivé ; défaut 1000.
    uint8_t midiCcRangeMin; // Plage MIDI min (0-127, défaut: 0) pour CC/autres messages
    uint8_t midiCcRangeMax; // Plage MIDI max (0-127, défaut: 127) pour CC/autres messages
    uint8_t midiCcOnOffMin; // Valeur CC pour état OFF (0-127, défaut: 0) - pour boutons
    uint8_t midiCcOnOffMax; // Valeur CC pour état ON (0-127, défaut: 127) - pour boutons
    uint8_t midiVelocity;   // [correctif NiDMI] Vélocité Note On (1-127, défaut: 100) - pour boutons
    /* Le script de mapping.
     *
     * C'etait `char mappingScript[128]` : une limite de 127 caracteres qui
     * n'etait pas un choix mais la taille du champ, et qui TRONQUAIT EN SILENCE
     * (MESURES.md §58, CONVERGENCE_NIDMI.md §9.3). C'est desormais un pointeur
     * TOUJOURS VALIDE — il vise la chaine vide quand il n'y a pas de script —
     * si bien qu'aucun des huit processeurs n'a eu a changer : ils lisent
     * `config.mappingScript[0]` comme avant.
     *
     * `scriptPossede` est le tampon dont cette configuration est PROPRIETAIRE,
     * dimensionne au contenu et libere par ComponentManager::clearAll(). Nul
     * quand le script vient d'ailleurs (une constante, un futur fichier). */
    const char* mappingScript;   // jamais nul
    char*       scriptPossede;   // tampon possede, ou nullptr
    char name[64];          // Nom personnalisé du composant (ex: "pot_volume", "btn_start")
    MidiMode midiMode;      // Mode MIDI: RTP config ou mapping script
    // Union pour les configurations spécifiques par type de composant
    // Permet d'économiser de la mémoire en ne stockant que la config nécessaire
    union {
        Components::ButtonConfig* button;
        Components::LedConfig* led;
        Components::PotentiometerConfig* potentiometer;
        Components::VelostatConfig* velostat;
        Components::JoystickConfig* joystick;
        Components::Joystick3Config* joystick3;
        Components::ImuConfig* imu;
        Components::Mpr121Config* mpr121;
        Components::NoiseSamplerConfig* noiseSampler;
        void* specific;  // Pointeur générique pour accès type-agnostique
    } specificConfig;
    
    // Champs génériques pour nouveaux composants (extensible sans modifier la structure)
    char customField1[16];  // Champ générique réutilisable (ex: encoderDirection, touchThreshold, etc.)
    char customField2[16];  // Champ générique réutilisable (ex: encoderMode, touchMode, etc.)
    uint8_t customInt1;     // Valeur numérique générique (ex: encoderSteps, touchSensitivity, etc.)
    uint8_t customInt2;     // Valeur numérique générique supplémentaire

    /**
     * Pin détectée "dans le vide" au setup (test pull-up/pull-down, voir
     * PinMapper::isPinFloating) : le composant reste configuré mais n'émet plus rien,
     * pour ne pas envoyer le bruit d'une entrée flottante.
     *
     * Réservé aux capteurs MONO-PIN (potentiomètre, velostat, noiseSampler). Les
     * joysticks en sont volontairement exclus : le test y donnait des faux positifs
     * qui muselaient des axes pourtant câblés (voir ComponentInitializer::setupGpio).
     */
    bool pin_disconnected;

    ComponentConfig() {
        specificConfig.specific = nullptr;
        customField1[0] = '\0';
        customField2[0] = '\0';
        customInt1 = 0;
        customInt2 = 0;
        mappingScript = "";
        scriptPossede = nullptr;
        name[0] = '\0';
        midiMode = MidiMode::RTP;  // Défaut: mode RTP classique
        pin_disconnected = false;
    }
    
    ~ComponentConfig() {
        // Libérer la mémoire allouée pour specificConfig si nécessaire
        // Note: Pour l'instant, on assume que ComponentManager gère la mémoire
        // TODO: Implémenter un système de gestion mémoire propre
    }
};

// État runtime d'un composant
struct ComponentState {
    uint16_t last_value;    // Dernière valeur lue
    uint32_t last_time;     // Dernière mise à jour
    uint8_t debounce_state; // État anti-rebond
    uint8_t last_note;      // Dernière note jouée (pour NOTE_SWEEP)

    // --- Télémétrie pour monitoring SVG (RAW vs MIDI) ---
    // RAW = valeur "brute" à afficher en mode RAW (touch 32 bits, MPR121 mask 0..4095, etc.)
    uint32_t last_raw_value_u32;
    // MIDI = valeur "fonctionnelle" (data1 OSC en mode MIDI, selon OscMidiHandler ou conventions directes)
    uint8_t last_midi_value_u8;
    // Timestamp de la dernière mise à jour pertinente de télémétrie (pour LED activity flash)
    uint32_t last_telemetry_ts;

    // --- Cas multi-axes (joystick) : seconde axe (Y) ---
    uint8_t aux_gpio;                 // GPIO de l'axe Y (ou 255 si non applicable)
    uint32_t last_raw_value_aux_u32;
    uint8_t last_midi_value_aux_u8;
    uint32_t last_telemetry_ts_aux;

    // --- Cas 3 axes (joystick3) : troisième axe (Z) ---
    uint8_t aux_gpio2;                // GPIO de l'axe Z (ou 255 si non applicable)
    uint32_t last_raw_value_aux2_u32;
    uint8_t last_midi_value_aux2_u8;
    uint32_t last_telemetry_ts_aux2;
    
    // Champs pour debouncing simple et fiable
    bool last_button_state; // État précédent du bouton (avant debounce)
    uint32_t last_change_time; // Temps du dernier changement
    uint32_t note_on_time; // Temps où la note a été jouée (pour auto-off)
    bool toggle_state;     // État pour mode toggle (true = note on, false = note off)

    /* Etat du script .nms de ce composant (toggle, counter, hysteresis, lp,
     * metro...). Il vit ici plutot que dans le moteur parce que chaque
     * composant a le sien : un tableau global les melangerait.
     *
     * UN ETAT PAR PIPELINE. Il n'y en avait qu'UN, sur l'argument que
     * « mappingScript fait 128 octets, il n'en porte guere plus » — argument
     * qui tombe avec la limite (§9.3). Tous les pipelines partageaient donc le
     * meme etat : deux toggle(), deux counter() ou deux metro() dans un meme
     * script se seraient marches dessus SANS RIEN DIRE. Lever la limite sans
     * lever ce partage aurait echange une panne visible contre une panne
     * silencieuse.
     *
     * Quatre : un script de broche en porte trois dans l'usage courant (note on,
     * note off, publication au registre). Au-dela, le moteur partage le dernier
     * emplacement — degradation connue et commentee, pas un plantage. */
    static constexpr uint8_t MAX_PIPELINES_BROCHE = 4;
    MappingEngine::Etat scriptEtats[MAX_PIPELINES_BROCHE];
    bool prev_stable_state; // État stable précédent (après debounce) pour détecter Falling/Rising
    
    // Champs pour joystick (réutilise customInt1/customInt2 pour stocker les dernières valeurs normalisées)
    // customInt1 = dernière valeur normalisée X (-127..127, stockée comme uint8_t avec offset 127)
    // customInt2 = dernière valeur normalisée Y (-127..127, stockée comme uint8_t avec offset 127)
    bool pulse_pending;    // Pour pulse: mémoriser qu'on a été pressé, attendre release
    
    // Hystérésis pour NOTE_SWEEP (zone morte de 2 bits = ±3 sur 0-127)
    Hysteresis<2> hysteresis;
    
    // Pour Velostat : dernière valeur aftertouch envoyée
    uint8_t last_aftertouch;
};
