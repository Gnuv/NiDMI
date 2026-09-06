#include "TouchProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../utils/PinMapper.h"
#include "../config/SystemConfig.h"
#include "../mapping/MappingEngine.h"
#include "../utils/AxisUtils.h"

// Inclure sdkconfig.h pour vérifier CONFIG_SOC_TOUCH_SENSOR_SUPPORTED
#ifdef ESP32
#include <sdkconfig.h>
#endif

// Vérifier que c'est un ESP32-S3 ET que touch sensor est supporté
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S3)
    #ifdef CONFIG_SOC_TOUCH_SENSOR_SUPPORTED
        #define TOUCH_AVAILABLE 1
    #else
        #define TOUCH_AVAILABLE 0
        #warning "CONFIG_SOC_TOUCH_SENSOR_SUPPORTED n'est pas activé dans sdkconfig. Ajoutez CONFIG_SOC_TOUCH_SENSOR_SUPPORTED=y dans sdkconfig.defaults"
    #endif
#else
    #define TOUCH_AVAILABLE 0
#endif

// Macro pour activer/désactiver les logs de debug
// 1 = debug détaillé (spam), 0 = silencieux sauf WARN/ERROR
#define DEBUG_TOUCH 0
#define DEBUG_TOUCH_RAW 0
#if DEBUG_TOUCH_RAW
  #define TOUCH_RAW_LOG(...)  Serial.printf(__VA_ARGS__)
#else
  #define TOUCH_RAW_LOG(...)
#endif

#if DEBUG_TOUCH
    #define TOUCH_LOG(...) Serial.printf(__VA_ARGS__)
    #define TOUCH_LOG_ONCE(...) do { static bool _logged = false; if (!_logged) { Serial.printf(__VA_ARGS__); _logged = true; } } while(0)
    #define TOUCH_INFO(...) Serial.printf(__VA_ARGS__)
#else
    #define TOUCH_LOG(...)
    #define TOUCH_LOG_ONCE(...)
    #define TOUCH_INFO(...)
#endif

/* Warnings et erreurs toujours actifs */
#define TOUCH_WARN(...) Serial.printf(__VA_ARGS__)
#define TOUCH_ERROR(...) Serial.printf(__VA_ARGS__)

// ===== FONCTIONS UTILITAIRES STATIQUES =====

// Lecture tactile avec limitation de fréquence par GPIO (domaine brut 32 bits).
// Un cache par GPIO évite de spammer touchRead() trop souvent,
// ce qui peut bloquer la FSM touch et déclencher le task watchdog.
static uint32_t readTouchValue(uint8_t gpio) {
#if !TOUCH_AVAILABLE
    (void)gpio;
    return 0;
#else
    static uint32_t last_value[49]   = {0};
    static uint32_t last_read_ms[49] = {0};
    static uint32_t last_log_ms[49]  = {0};
    const uint32_t MIN_INTERVAL_MS = 5;

    uint32_t now = millis();
    if (last_read_ms[gpio] != 0 && (now - last_read_ms[gpio] < MIN_INTERVAL_MS)) {
        return last_value[gpio];
    }

    uint32_t raw = touchRead(gpio);
    last_value[gpio]   = raw;
    last_read_ms[gpio] = now;
    yield(); // Laisser le CPU aux autres tâches (évite task_wdt pendant baseline)

    if (now - last_log_ms[gpio] >= 500) {
        TOUCH_RAW_LOG("[TouchProcessor] GPIO%d: raw=%lu\n",
                      gpio, (unsigned long)raw);
        last_log_ms[gpio] = now;
    }

    return raw;
#endif
}

// État global pour la baseline et le lissage des GPIO tactiles
namespace {
    static uint32_t baseline_value[49]         = {0};
    static uint32_t baseline_sum[49]           = {0};
    static uint8_t  baseline_count[49]         = {0};
    static bool     baseline_set[49]           = {false};
    static uint32_t baseline_min[49]           = {UINT32_MAX};
    static uint32_t baseline_max[49]           = {0};
    static uint32_t baseline_start_time[49]    = {0};
    static uint32_t baseline_wait_last_log[49] = {0};
    static const uint32_t BASELINE_STABILIZATION_TIME_MS = 2000; // Délai pour stabilisation du signal
    static const uint8_t  BASELINE_SAMPLE_COUNT = 20;            // nb d'échantillons pour la baseline
    // Échantillons gardés pour calculer la MÉDIANE (robuste aux pics : un pic isolé ne
    // pollue pas la baseline, contrairement à la moyenne — cf. lectures touch bruitées sur S3).
    static uint32_t baseline_samples[49][BASELINE_SAMPLE_COUNT] = {{0}};

    // Lissage flottant pour chaque GPIO (même EMA que AnalogFilter, en pleine résolution)
    static float smoothed_touch_f[49]          = {0.0f};

    void resetBaselineInternal(uint8_t idx) {
        if (idx >= 49) return;
        baseline_value[idx]         = 0;
        baseline_sum[idx]           = 0;
        baseline_count[idx]         = 0;
        baseline_set[idx]           = false;
        baseline_min[idx]           = UINT32_MAX;
        baseline_max[idx]           = 0;
        baseline_start_time[idx]    = 0;
        baseline_wait_last_log[idx] = 0;
        smoothed_touch_f[idx]       = 0.0f;
    }
}

// Établir la baseline pour un GPIO.
// touch_value_in est la valeur déjà lue par l'appelant (évite un 2e appel à touchRead).
static bool establishBaseline(uint8_t gpio, uint32_t touch_value_in, uint32_t& baseline) {
    if (gpio > 48) {
        return false;
    }
    uint8_t idx = gpio;
    
    if (baseline_set[idx] && baseline_value[idx] > 0) {
        baseline = baseline_value[idx];
        TOUCH_LOG("[TouchProcessor] GPIO%d: Baseline déjà établie: %lu\n",
                  gpio, (unsigned long)baseline);
        return true;
    }
    
    // Démarrage du timer de stabilisation si nécessaire
    if (baseline_start_time[idx] == 0) {
        baseline_start_time[idx] = millis();
    }

    uint32_t elapsed = millis() - baseline_start_time[idx];
    if (elapsed < BASELINE_STABILIZATION_TIME_MS) {
        // On attend que le signal se stabilise avant de commencer à accumuler la baseline
        if (millis() - baseline_wait_last_log[idx] > 500) {
            TOUCH_INFO("[TouchProcessor] GPIO%d: Attente stabilisation pour baseline (%lums/%lums)\n",
                       gpio,
                       (unsigned long)elapsed,
                       (unsigned long)BASELINE_STABILIZATION_TIME_MS);
            baseline_wait_last_log[idx] = millis();
        }
        return false;
    }
    
    uint32_t touch_value = touch_value_in;
    if (baseline_count[idx] < BASELINE_SAMPLE_COUNT) {
        baseline_samples[idx][baseline_count[idx]] = touch_value;  // pour la médiane
    }
    baseline_sum[idx] += touch_value;
    baseline_count[idx]++;
    
    if (touch_value < baseline_min[idx]) baseline_min[idx] = touch_value;
    if (touch_value > baseline_max[idx]) baseline_max[idx] = touch_value;
    
    // Log progression de l'établissement de baseline (essentiel : toutes les 5 mesures)
    if (baseline_count[idx] <= 5 || (baseline_count[idx] % 5) == 0) {
        TOUCH_INFO("[TouchProcessor] GPIO%d: Baseline en cours... (%d/20) raw=%d, min=%d, max=%d\n",
                   gpio, baseline_count[idx], touch_value, baseline_min[idx], baseline_max[idx]);
    } else {
        TOUCH_LOG("[TouchProcessor] GPIO%d: Baseline en cours... (%d/20) raw=%d, min=%d, max=%d\n",
                  gpio, baseline_count[idx], touch_value, baseline_min[idx], baseline_max[idx]);
    }
    yield(); // Éviter task_wdt pendant l'établissement de la baseline (plusieurs pins)

    if (baseline_count[idx] >= BASELINE_SAMPLE_COUNT) {
        // MÉDIANE des échantillons (robuste aux pics) au lieu de la moyenne :
        // un pic isolé dans la fenêtre ne décale plus la baseline.
        uint32_t sorted[BASELINE_SAMPLE_COUNT];
        for (uint8_t i = 0; i < BASELINE_SAMPLE_COUNT; i++) sorted[i] = baseline_samples[idx][i];
        for (uint8_t i = 1; i < BASELINE_SAMPLE_COUNT; i++) {   // tri par insertion (n=20)
            uint32_t key = sorted[i];
            int j = (int)i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
            sorted[j + 1] = key;
        }
        uint32_t median = (sorted[BASELINE_SAMPLE_COUNT / 2 - 1] + sorted[BASELINE_SAMPLE_COUNT / 2]) / 2;
        uint32_t mean   = baseline_sum[idx] / baseline_count[idx];  // pour comparaison dans le log
        baseline_value[idx] = median;
        baseline_set[idx]   = true;
        baseline            = baseline_value[idx];
        TOUCH_INFO("[TouchProcessor] ✓ Baseline GPIO%d établie (médiane): %lu (moyenne=%lu, min=%lu, max=%lu, écart=%lu)\n",
                  gpio,
                  (unsigned long)baseline,
                  (unsigned long)mean,
                  (unsigned long)baseline_min[idx],
                  (unsigned long)baseline_max[idx],
                  (unsigned long)(baseline_max[idx] - baseline_min[idx]));
        
        if (baseline_min[idx] == baseline_max[idx]) {
            TOUCH_WARN("[TouchProcessor] ⚠️ GPIO%d: Toutes les lectures identiques (%lu)! touchRead() ne change peut-être pas?\n",
                      gpio, (unsigned long)baseline_min[idx]);
        }
        return true;
    }

    return false;
}

// ===== MÉTHODES PUBLIQUES DE CALIBRATION =====

void TouchProcessor::resetBaseline(uint8_t gpio) {
#if !TOUCH_AVAILABLE
    (void)gpio;
    return;
#else
    if (gpio > 48) return;
    resetBaselineInternal(gpio);
#endif
}

void TouchProcessor::resetAllBaselines() {
#if !TOUCH_AVAILABLE
    return;
#else
    for (uint8_t i = 0; i < 49; ++i) {
        resetBaselineInternal(i);
    }
#endif
}

// Parser customField2 Touch : "aft" ou "aft,onRaw,offRaw" (ex. "20000" ou "20000,2000,500")
static void parseTouchCustomField2(const char* s, uint32_t& aft, uint32_t& on_raw, uint32_t& off_raw) {
    aft = 20000;
    on_raw = 0;
    off_raw = 0;
    if (!s || s[0] == '\0') return;
    aft = (uint32_t)atoi(s);
    const char* p = strchr(s, ',');
    if (p) {
        on_raw = (uint32_t)atoi(p + 1);
        p = strchr(p + 1, ',');
        if (p) off_raw = (uint32_t)atoi(p + 1);
    }
}

// Calculer les seuils depuis la configuration (customField2 = "aft,onRaw,offRaw")
static void calculateThresholds(
    const ComponentConfig& config,
    uint32_t baseline,
    uint32_t& touch_threshold,
    uint32_t& velocity_threshold,
    uint32_t& aftertouch_range,
    uint32_t& note_on_threshold,
    uint32_t& note_off_threshold
) {
    uint32_t on_raw = 0, off_raw = 0;
    parseTouchCustomField2(config.customField2, aftertouch_range, on_raw, off_raw);
    if (aftertouch_range == 0) aftertouch_range = (baseline * 20) / 100;

    if (on_raw > 0) {
        note_on_threshold = baseline + on_raw;
    } else {
        note_on_threshold = (baseline * 102) / 100 + (baseline * 2) / 100;  // auto 102%+2%
    }
    if (off_raw > 0) {
        note_off_threshold = baseline + off_raw;
    } else {
        note_off_threshold = (baseline * 102) / 100;  // auto 102%
    }
    if (note_on_threshold < note_off_threshold) note_on_threshold = note_off_threshold;

    touch_threshold = note_off_threshold;
    velocity_threshold = note_off_threshold;

    TOUCH_LOG("[TouchProcessor] GPIO%d: on=%lu off=%lu aft=%lu (baseline=%lu)\n",
             config.gpio, (unsigned long)note_on_threshold, (unsigned long)note_off_threshold,
             (unsigned long)aftertouch_range, (unsigned long)baseline);
}

// Traitement NOTE_VELOCITY
static void processNoteVelocity(
    const ComponentConfig& config,
    ComponentState& state,
    uint32_t touch_value,
    uint32_t touch_smoothed,
    uint32_t baseline,
    uint32_t note_on_threshold,
    uint32_t note_off_threshold,
    uint32_t velocity_threshold,
    uint32_t aftertouch_range,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    bool note_is_on = (state.last_note != 255);
    bool is_touched = note_is_on
        ? (touch_value > note_off_threshold)   // Note déjà ON → rester ON tant qu'au-dessus du seuil bas
        : (touch_value > note_on_threshold);   // Note OFF → déclencher au-dessus du seuil haut

    // Capteur "valeur MONTE quand on touche" : plage [seuil .. seuil+bande] → vélocité 1..127
    uint32_t touch_min_value = note_on_threshold;  // juste au-dessus du seuil = touche légère
    uint32_t touch_max_value = velocity_threshold + (baseline * 20) / 100;  // bande 20% au-dessus = touche forte
    if (touch_max_value <= touch_min_value) {
        touch_max_value = touch_min_value + (baseline * 10) / 100;
    }

    // Mapper vers vélocité (1-127) : plus touch_value est haut, plus velocity est haute
    uint8_t velocity = 0;
    if (is_touched) {
        if (touch_value <= touch_min_value) {
            velocity = 1;
        } else if (touch_value >= touch_max_value) {
            velocity = 127;
        } else {
            velocity = map(touch_value, touch_min_value, touch_max_value, 1, 127);
            if (velocity < 1) velocity = 1;
            if (velocity > 127) velocity = 127;
        }
    }
    
    // Log continu de la valeur touch (avant traitement MIDI)
    static unsigned long last_touch_value_log = 0;
    if (millis() - last_touch_value_log > 100) {
        TOUCH_INFO("[TouchProcessor] GPIO%d: touch_value=%d (raw, avant traitement MIDI)\n", 
                  config.gpio, touch_value);
        last_touch_value_log = millis();
    }
        
        uint8_t note = config.midi_param;
        uint8_t channel = config.midi_channel;
        
        if (is_touched && !note_is_on) {
        // Note On
        TOUCH_INFO("[TouchProcessor] ✓✓✓ GPIO%d → Note On (note=%d, velocity=%d, raw=%d)\n",
                         config.gpio, note, velocity, touch_value);
            if (midi_sender) {
                midi_sender->sendNoteOn(channel, note, velocity);
            TOUCH_LOG("[TouchProcessor] → MIDI Note On envoyé (ch=%d, note=%d, vel=%d)\n", 
                     channel, note, velocity);
            } else {
            TOUCH_WARN("[TouchProcessor] ⚠️ MIDI Note On NON envoyé (midi_sender=NULL)\n");
            }
            state.last_note = note;
            state.last_value = velocity;
            state.last_raw_value_u32 = touch_value;     // RAW brute 32 bits
            state.last_midi_value_u8 = velocity;       // data1 OSC en mode MIDI
            state.last_telemetry_ts = millis();
        state.last_aftertouch = velocity;
            state.last_time = millis();
            MidiOutputCoordinator::sendOsc(osc_queue, config, velocity, touch_value);
        } else if (!is_touched && note_is_on) {
        // Note Off
        TOUCH_INFO("[TouchProcessor] ✗✗✗ GPIO%d → Note Off (note=%d, raw=%d)\n",
                         config.gpio, note, touch_value);
            if (midi_sender) {
                midi_sender->sendNoteOff(channel, note, 0);
                midi_sender->sendKeyPressure(channel, note, 0);
            TOUCH_LOG("[TouchProcessor] → MIDI Note Off envoyé (ch=%d, note=%d)\n", channel, note);
            } else {
            TOUCH_WARN("[TouchProcessor] ⚠️ MIDI Note Off NON envoyé (midi_sender=NULL)\n");
            }
            state.last_note = 255;
            state.last_value = 0;
            state.last_aftertouch = 0;
            state.last_raw_value_u32 = touch_value; // RAW brute 32 bits
            state.last_midi_value_u8 = 0;            // data1 OSC = 0 (note off)
            state.last_telemetry_ts = millis();
            state.last_time = state.last_telemetry_ts;
            MidiOutputCoordinator::sendOsc(osc_queue, config, 0, touch_value);
        } else if (note_is_on && is_touched && velocity > 0) {
        // Key Pressure : mapping [touch_threshold .. touch_threshold+aftertouch_range] → 0-127 (touch_smoothed)
        uint32_t at_min = velocity_threshold;
        uint32_t at_max = velocity_threshold + aftertouch_range;
        if (at_max <= at_min) at_max = at_min + 1;
        uint8_t at_velocity = 0;
        if (touch_smoothed <= at_min) {
            at_velocity = 0;
        } else if (touch_smoothed >= at_max) {
            at_velocity = 127;
        } else {
            at_velocity = map(touch_smoothed, at_min, at_max, 0, 127);
            if (at_velocity > 127) at_velocity = 127;
        }

        const uint32_t MIN_KEYPRESSURE_INTERVAL_MS = 20;
        uint32_t time_since_last = millis() - state.last_time;
        if (time_since_last >= MIN_KEYPRESSURE_INTERVAL_MS && at_velocity != state.last_aftertouch) {
            TOUCH_LOG("[TouchProcessor] GPIO%d: Key Pressure at_vel=%d (smoothed %lu in [%lu..%lu])\n",
                     config.gpio, at_velocity, (unsigned long)touch_smoothed, (unsigned long)at_min, (unsigned long)at_max);
            if (midi_sender) {
                midi_sender->sendKeyPressure(channel, note, at_velocity);
            }
            state.last_aftertouch = at_velocity;
            state.last_value = at_velocity;
            state.last_raw_value_u32 = touch_value;      // RAW brute 32 bits (utile pour le mode RAW)
            state.last_midi_value_u8 = at_velocity;    // data1 OSC en mode MIDI
            state.last_telemetry_ts = millis();
            state.last_time = millis();
            MidiOutputCoordinator::sendOsc(osc_queue, config, at_velocity, touch_smoothed);
        }
    }
}

// Traitement NOTE_SWEEP
// touch_value = signal lissé (EMA) pour limiter le bruit ; Schmitt sur le brut tactile
// évite les oscillations note on/off au voisinage du seuil.
static void processNoteSweep(
    const ComponentConfig& config,
    ComponentState& state,
    uint32_t touch_value,
    uint32_t touch_threshold,
    uint32_t baseline,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    uint8_t gi = config.gpio;
    if (gi > 48) {
        gi = 48;
    }
    static bool touch_sweep_latched[49];
    static uint32_t touch_sweep_last_transition[49];

    const uint16_t sweepOffMs = effectiveNoteSweepAutoOffMs(config.rtpNoteSweepAutoOffDelay);

    // Auto-off : coupe la note si le délai est écoulé, mais conserve state.last_note
    // pour éviter un retrigger si la position pointe toujours sur la même note.
    if (sweepOffMs > 0 &&
        state.last_note != 255 &&
        state.note_on_time > 0) {
        uint32_t elapsed = millis() - state.note_on_time;
        if (elapsed >= sweepOffMs) {
            if (midi_sender) {
                midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
            }
            state.note_on_time = 0;
            touch_sweep_last_transition[gi] = millis();
        }
    }

    // Schmitt : bande morte autour du seuil (évite rebonds quand la mesure oscille)
    uint32_t margin = baseline / 10;
    if (margin < 15) {
        margin = 15;
    }
    if (baseline > 80 && margin > baseline / 4) {
        margin = baseline / 4;
    }
    uint32_t thr_hi = touch_threshold + margin;
    uint32_t thr_lo = (touch_threshold > margin) ? touch_threshold - margin : 0;

    if (touch_value <= thr_lo) {
        touch_sweep_latched[gi] = false;
    } else if (touch_value >= thr_hi) {
        touch_sweep_latched[gi] = true;
    }

    uint16_t mapped_value;
    if (!touch_sweep_latched[gi]) {
        mapped_value = 0;
    } else {
        uint32_t touch_max = touch_threshold + (baseline * 20) / 100;
        if (touch_max <= thr_hi) {
            touch_max = thr_hi + 1;
        }
        if (touch_value >= touch_max) {
            mapped_value = 4095;
        } else {
            mapped_value = (uint16_t)map((long)touch_value, (long)thr_hi, (long)touch_max, 0, 4095);
            if (mapped_value > 4095) {
                mapped_value = 4095;
            }
        }
    }

    if (!state.hysteresis.update(mapped_value)) {
        return;
    }

    uint8_t stable_midi_value = state.hysteresis.getValue();
    uint8_t noteMin = config.rtpNoteMin;
    uint8_t noteMax = config.rtpNoteMax;
    uint8_t newNote = (stable_midi_value == 0) ? 255 : (uint8_t)map(stable_midi_value, 1, 127, noteMin, noteMax);

    if (newNote == state.last_note) {
        return;
    }

    constexpr uint32_t kMinNoteTransitionMs = 28;
    uint32_t now = millis();
    if (newNote != state.last_note &&
        (now - touch_sweep_last_transition[gi]) < kMinNoteTransitionMs) {
        return;
    }

    // Note différente : couper l'ancienne uniquement si encore active.
    if (state.last_note != 255 && state.note_on_time > 0) {
        if (midi_sender) {
            midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
        }
    }

    if (newNote != 255) {
        if (midi_sender) {
            midi_sender->sendNoteOn(config.midi_channel, newNote, config.rtpNoteVelFix);
        }
        state.note_on_time = millis();
    } else {
        state.note_on_time = 0;
    }

    touch_sweep_last_transition[gi] = now;
    state.last_note = newNote;
    state.last_value = stable_midi_value;
    state.last_raw_value_u32 = touch_value;
    state.last_midi_value_u8 = stable_midi_value;
    state.last_telemetry_ts = millis();
    state.last_time = millis();
    MidiOutputCoordinator::sendOsc(osc_queue, config, stable_midi_value, mapped_value);
}

// Traitement messages continus (CC, Pitch Bend, Aftertouch)
static void processContinuous(
    const ComponentConfig& config,
    ComponentState& state,
    uint32_t touch_value,
    uint32_t touch_threshold,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    // Mapper la valeur (domaine brut 32 bits -> 0-4095)
    uint16_t mapped_value = 0;
    // Sur ESP32-S3, la valeur tactile MONTE quand on touche :
    // on considère donc le contact actif quand on dépasse le seuil.
    bool is_touched_cc = (touch_value > touch_threshold);

    if (is_touched_cc) {
        // Mapper la plage [touch_threshold .. max_value] vers [0 .. 4095]
        // avec saturation au‑delà de max_value pour éviter les débordements.
        uint32_t max_value = touch_threshold * 2U;
        if (touch_value >= max_value) {
            mapped_value = 4095;
        } else {
            mapped_value = map(touch_value, touch_threshold, max_value, 0, 4095);
        }
    } else {
        mapped_value = 0;
    }
    
    // Log continu pour CC
    static unsigned long last_cc_log[49] = {0};
    if (is_touched_cc && millis() - last_cc_log[config.gpio] > 100) {
        TOUCH_INFO("[TouchProcessor] GPIO%d: TOUCH ACTIF (CC) → raw=%d, mapped=%d, threshold=%d\n",
                         config.gpio, touch_value, mapped_value, touch_threshold);
        last_cc_log[config.gpio] = millis();
    }
    
    // Hystérésis
    if (!state.hysteresis.update(mapped_value)) {
        TOUCH_LOG("[TouchProcessor] GPIO%d: Hystérésis bloque (mapped_value=%d stable)\n",
                         config.gpio, mapped_value);
        return;
    }
    
    uint8_t midi_value = state.hysteresis.getValue();
    
    // Plage MIDI configurée
    if (config.midiCcRangeMin != 0 || config.midiCcRangeMax != 127) {
        midi_value = map(midi_value, 0, 127, config.midiCcRangeMin, config.midiCcRangeMax);
    }
    
    if (midi_value == state.last_value) {
        TOUCH_LOG("[TouchProcessor] GPIO%d: Pas de changement (midi_value=%d == last_value=%d)\n",
                         config.gpio, midi_value, state.last_value);
        return;
    }
    
    // Throttling
    const uint32_t MIN_CONTINUOUS_INTERVAL_MS = 10;
    uint32_t time_since_last = millis() - state.last_time;
    if (time_since_last < MIN_CONTINUOUS_INTERVAL_MS) {
        TOUCH_LOG("[TouchProcessor] GPIO%d: Throttling bloque (temps=%dms < min=%dms)\n",
                         config.gpio, time_since_last, MIN_CONTINUOUS_INTERVAL_MS);
        return;
    }
    
    uint16_t raw_value_for_handler = (config.msg_type == MidiMessageType::PITCH_BEND) 
        ? mapped_value 
        : mapped_value;
    
    TOUCH_INFO("[TouchProcessor] →→→ GPIO%d: CC=%d, touch_value=%d, mapped=%d, midi_value=%d\n",
              config.gpio, config.midi_param, touch_value, mapped_value, midi_value);
    
    MidiOutputCoordinator::sendMidiAndOsc(midi_sender, osc_queue, config, midi_value, raw_value_for_handler);
    state.last_value = midi_value;
    state.last_time = millis();
    state.last_raw_value_u32 = touch_value;
    state.last_midi_value_u8 = midi_value;
    state.last_telemetry_ts = state.last_time;
}

// ===== FONCTION PRINCIPALE =====

void TouchProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& filter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    #if !TOUCH_AVAILABLE
        static unsigned long last_warning = 0;
        if (millis() - last_warning > 5000) {
            TOUCH_WARN("[TouchProcessor] WARNING: Touch non disponible sur ce MCU (ESP32-S3 requis avec CONFIG_SOC_TOUCH_SENSOR_SUPPORTED=y)\n");
            last_warning = millis();
        }
        return;
    #endif
    
    // Vérifier si touch est activé
    static bool touch_enabled_logged = false;
    bool touch_enabled = SystemConfig::isTouchEnabled();
    if (!touch_enabled_logged) {
        TOUCH_INFO("[TouchProcessor] ═══ VÉRIFICATION TOUCH ═══\n");
        TOUCH_INFO("[TouchProcessor] SystemConfig::isTouchEnabled() = %s\n", touch_enabled ? "true" : "false");
        TOUCH_INFO("[TouchProcessor] ÉTAT TOUCH: %s\n", touch_enabled ? "ACTIVÉ ✓✓✓" : "DÉSACTIVÉ ✗✗✗");
        touch_enabled_logged = true;
    }
    
    if (!touch_enabled) {
        static unsigned long last_warning = 0;
        if (millis() - last_warning > 10000) {
            TOUCH_WARN("[TouchProcessor] ⚠️⚠️⚠️ TOUCH DÉSACTIVÉ ⚠️⚠️⚠️\n");
            TOUCH_WARN("[TouchProcessor] Activez-le via l'interface web (onglet Connection -> Paramètres système)\n");
            last_warning = millis();
        }
        return;
    }
    
    // Validation GPIO
    if (config.gpio >= 255 || config.gpio > 48) {
        TOUCH_ERROR("[TouchProcessor] ERROR: GPIO invalide %d\n", config.gpio);
        return;
    }
    
    if (!PinMapper::hasTouch(config.gpio)) {
        static unsigned long last_warning = 0;
        if (millis() - last_warning > 5000) {
            TOUCH_WARN("[TouchProcessor] WARNING: GPIO%d n'a pas de capacité Touch\n", config.gpio);
            last_warning = millis();
        }
        return;
    }
    
    // Lecture tactile (domaine brut 32 bits)
    uint32_t touch_raw = readTouchValue(config.gpio);
    
    // Log essentiel de la valeur brute toutes les 200ms
    static unsigned long last_raw_info[49] = {0};
    if (millis() - last_raw_info[config.gpio] > 200) {
        TOUCH_INFO("[TouchProcessor] GPIO%d: raw=%lu (lecture brute)\n",
                   config.gpio, (unsigned long)touch_raw);
        last_raw_info[config.gpio] = millis();
    }
    
    if (touch_raw == 0) {
        static unsigned long last_error = 0;
        if (millis() - last_error > 5000) {
            TOUCH_ERROR("[TouchProcessor] ERROR: touchRead(GPIO%d) retourne 0 - vérifier la connexion\n", config.gpio);
            last_error = millis();
        }
        return;
    }
    
    // Configuration du filtre (intensité 1-10)
    uint8_t intensity = 5; // Défaut
    if (config.customField1[0] != '\0') {
        int parsed = atoi(config.customField1);
        if (parsed >= 1 && parsed <= 10) {
            intensity = (uint8_t)parsed;
        }
    }
    filter.setAlphaFromIntensity(intensity);
    
    // Lissage flottant (même EMA que AnalogFilter) en pleine résolution 32 bits
    uint8_t gpio_idx = config.gpio;
    float a = filter.alpha; // alpha déjà configuré par setAlphaFromIntensity
    if (smoothed_touch_f[gpio_idx] == 0.0f) {
        smoothed_touch_f[gpio_idx] = (float)touch_raw;
    } else {
        smoothed_touch_f[gpio_idx] = a * (float)touch_raw + (1.0f - a) * smoothed_touch_f[gpio_idx];
    }
    uint32_t touch_smoothed = (uint32_t)smoothed_touch_f[gpio_idx];
    
    TOUCH_LOG("[TouchProcessor] GPIO%d: raw=%lu, smoothed=%lu, alpha=%.3f\n",
             config.gpio, (unsigned long)touch_raw, (unsigned long)touch_smoothed, a);

    // Établir baseline (on passe touch_raw pour éviter une 2e lecture)
    uint32_t baseline;
    if (!establishBaseline(config.gpio, touch_raw, baseline)) {
        TOUCH_LOG("[TouchProcessor] GPIO%d: Baseline pas encore établie (en cours d'établissement...)\n",
                 config.gpio);
        return; // Baseline pas encore établie
    }
    
    // Calculer les seuils (customField2 = "aft,onRaw,offRaw")
    uint32_t touch_threshold;
    uint32_t velocity_threshold;
    uint32_t aftertouch_range;
    uint32_t note_on_threshold;
    uint32_t note_off_threshold;
    calculateThresholds(config, baseline, touch_threshold, velocity_threshold, aftertouch_range,
                        note_on_threshold, note_off_threshold);
    
    // Log périodique des valeurs et seuils
    static unsigned long last_debug = 0;
    if (millis() - last_debug > 1000) {
        TOUCH_LOG("[TouchProcessor] GPIO%d: raw=%lu, smoothed=%lu, baseline=%lu, "
                 "thresh=%lu, vel_thresh=%lu\n",
                 config.gpio,
                 (unsigned long)touch_raw,
                 (unsigned long)touch_smoothed,
                 (unsigned long)baseline,
                 (unsigned long)touch_threshold,
                 (unsigned long)velocity_threshold);
        last_debug = millis();
    }
    
    // Traitement selon le type de message
    TOUCH_LOG_ONCE("[TouchProcessor] GPIO%d: Type message MIDI = %d\n", config.gpio, (int)config.msg_type);
    
    if (config.msg_type == MidiMessageType::NOTE_VELOCITY) {
        processNoteVelocity(config, state, touch_raw, touch_smoothed, baseline,
                          note_on_threshold, note_off_threshold, velocity_threshold, aftertouch_range,
                          midi_sender, osc_queue);
    } else if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
        processNoteSweep(config, state, touch_smoothed, touch_threshold, baseline, midi_sender, osc_queue);
    } else {
        processContinuous(config, state, touch_raw, touch_threshold, midi_sender, osc_queue);
    }
    
    // Update FluxRegistry only when the component has a declared name.
    if (config.name && config.name[0] != '\0') {
        FluxRegistry::update(config.name, (float)state.last_value);
    }
    // Script mode must run even without a component name.
    if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        MappingEngine::executerCapteur(config.mappingScript, (float)state.last_value,
                                      midi_sender, &state.scriptEtat);
    }
}

// Wrapper pour normaliser la signature
static void processWrapper(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter* filter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    if (filter == nullptr) {
        return;
    }
    TouchProcessor::process(config, state, *filter, midi_sender, osc_queue);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::TOUCH,
    processWrapper
);
