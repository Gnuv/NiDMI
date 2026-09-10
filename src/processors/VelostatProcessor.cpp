#include "VelostatProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"
#include "../components/basic/VelostatDef.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../utils/PinMapper.h"
#include "../mapping/MappingEngine.h"

namespace {
// Mapping commun [seuil, max] -> [1, 127]. Utilisé deux fois : sur la valeur filtrée
// (aftertouch, et Note On quand le scan est désactivé) et sur le pic de frappe.
// `max` est la valeur ADC qui doit produire 127 : la descendre évite d'avoir à taper très fort
// pour atteindre la vélocité maximale, l'ADC ne montant jamais jusqu'à 4095 en pratique.
uint8_t mapVelocity(uint16_t value, uint16_t threshold, uint16_t max) {
    if (max <= threshold) max = 4095;          // Garde-fou : plage invalide
    if (value <= threshold) return 0;
    if (value >= max) return 127;
    long v = map(value, threshold, max, 1, 127);
    if (v < 1) v = 1;
    if (v > 127) v = 127;
    return (uint8_t)v;
}
} // namespace

void VelostatProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& filter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    // Vérifier que le GPIO est valide et a un ADC
    if (config.gpio >= 255 || config.gpio > 48) {
        return;
    }
    
    if (!PinMapper::hasAdc(config.gpio)) {
        return;
    }
    
    // Lecture analogique (0-4095 pour ESP32)
    uint16_t sensor_value = analogRead(config.gpio);
    
    // Mettre à jour alpha du filtre selon filter_intensity (1-10)
    uint8_t intensity = 5; // Valeur par défaut
    uint16_t velocity_threshold = 50;
    uint16_t velocity_max = 4095;
    uint8_t aftertouch_threshold = 4;
    uint8_t scan_time_ms = 0;
    uint8_t mask_time_ms = 0;

    if (config.specificConfig.velostat) {
        intensity = config.specificConfig.velostat->filter_intensity;
        if (intensity == 0) intensity = 5; // Valeur par défaut si non configuré
        velocity_threshold = config.specificConfig.velostat->velocityThreshold;
        velocity_max = config.specificConfig.velostat->velocityMax;
        aftertouch_threshold = config.specificConfig.velostat->aftertouchThreshold;
        scan_time_ms = config.specificConfig.velostat->scanTimeMs;
        mask_time_ms = config.specificConfig.velostat->maskTimeMs;
    }

    // Montage attendu : capteur entre 3V3 et la pin, tirage 10 kΩ entre la pin et GND.
    // La tension monte donc avec la pression, et tout l'aval raisonne sur
    // "plus haut = plus de pression". Un montage inverse n'est pas supporté : voir la note
    // de câblage du formulaire et la section impédance du guide d'implémentation.
    filter.setAlphaFromIntensity(intensity);

    // Filtrer la valeur brute
    uint16_t filtered_value = filter.process(sensor_value);
    
    // Mapper la valeur filtrée vers la vélocité (1-127)
    uint8_t velocity = mapVelocity(filtered_value, velocity_threshold, velocity_max);

    uint8_t note = config.midi_param;
    uint8_t channel = config.midi_channel;

    // Gestion Note On/Off
    bool note_is_on = (state.last_note != 255);

    // Anti-redéclenchement : après une Note On, on ignore tout nouveau franchissement pendant
    // maskTimeMs, le temps que le pad cesse de rebondir mécaniquement.
    // state.note_on_time porte le timestamp de la dernière Note On : champ déjà présent dans
    // ComponentState et inutilisé par le velostat (il ne sert qu'à l'auto-off NOTE_SWEEP du
    // potentiomètre), donc zéro octet de RAM supplémentaire sur les 64 composants possibles.
    bool masked = (mask_time_ms > 0 && state.note_on_time > 0 &&
                   (millis() - state.note_on_time) < (uint32_t)mask_time_ms);

    if (filtered_value > velocity_threshold && !note_is_on && !masked) {
        // --- Fenêtre de détection du pic de frappe ---
        // La vélocité au simple franchissement du seuil ne représente pas la force de la frappe.
        // On suit donc le pic pendant scanTimeMs avant d'émettre la note.
        // Ce scan se fait en boucle serrée ici, et non en accumulant des cycles de la tâche MIDI :
        // celle-ci tourne à 10 ms et traite 4 composants par cycle (round-robin), ce qui ne
        // laisserait qu'un échantillon au mieux sur une fenêtre de quelques millisecondes.
        if (scan_time_ms > 0) {
            uint32_t scan_start = micros();
            uint32_t scan_duration = (uint32_t)scan_time_ms * 1000UL;
            uint16_t peak = sensor_value;
            uint16_t last_sample = sensor_value;

            while ((micros() - scan_start) < scan_duration) {
                uint16_t s = (uint16_t)analogRead(config.gpio);
                last_sample = s;
                if (s > peak) peak = s;
                delayMicroseconds(100);
            }

            // Le filtre EMA n'a pas été alimenté pendant le scan : on le recale sur la dernière
            // lecture pour que l'aftertouch enchaîne sans phase de rattrapage.
            filter.filtered = (float)last_sample;

            velocity = mapVelocity(peak, velocity_threshold, velocity_max);
            filtered_value = peak;
        }

        // Note On
        if (config.midiMode != MidiMode::SCRIPT && midi_sender) {
            midi_sender->sendNoteOn(channel, note, velocity);
        }
        state.last_note = note;
        state.last_value = velocity;
        state.last_raw_value_u32 = filtered_value;
        state.last_midi_value_u8 = velocity;
        state.last_telemetry_ts = millis();
        state.last_aftertouch = velocity; // Initialiser last_aftertouch avec la vélocité
        state.last_time = millis();
        state.note_on_time = millis();    // Départ de la fenêtre d'anti-redéclenchement
        
        // Envoyer aussi en OSC si configuré
        MidiOutputCoordinator::sendOsc(osc_queue, config, velocity, filtered_value);
    } else if (filtered_value <= velocity_threshold && note_is_on) {
        // Note Off
        if (config.midiMode != MidiMode::SCRIPT && midi_sender) {
            midi_sender->sendNoteOff(channel, note, 0);
        }
        state.last_note = 255;
        state.last_value = 0;
        state.last_raw_value_u32 = filtered_value;
        state.last_midi_value_u8 = 0;
        state.last_telemetry_ts = millis();
        state.last_aftertouch = 0;
        
        // Envoyer aussi en OSC si configuré
        MidiOutputCoordinator::sendOsc(osc_queue, config, 0, filtered_value);
    } else if (note_is_on && velocity > 0) {
        // Note est active : envoyer Key Pressure (Polyphonic Aftertouch)
        // Filtrer la vélocité pour stabilité (utiliser le filtre analogique comme MovingAverage)
        // On utilise directement la vélocité mappée
        
        // Envoyer seulement si changement significatif
        int velocity_diff = (velocity > state.last_aftertouch) ? 
                           (velocity - state.last_aftertouch) : 
                           (state.last_aftertouch - velocity);
        if (velocity_diff > aftertouch_threshold) {
            if (config.midiMode != MidiMode::SCRIPT && midi_sender) {
                midi_sender->sendKeyPressure(channel, note, velocity);
            }
            state.last_aftertouch = velocity;
            state.last_raw_value_u32 = filtered_value;
            state.last_midi_value_u8 = velocity;
            state.last_telemetry_ts = millis();
            state.last_time = millis();
            
            // Envoyer aussi en OSC si configuré
            MidiOutputCoordinator::sendOsc(osc_queue, config, velocity, filtered_value);
        }
    }
    
    /* PLUS DE PUBLICATION D'OFFICE AU BUS. Le registre est un bus PARTAGE :
     * il ne doit contenir que ce qu'on y a mis expressement — « in() : s("x") ».
     * Y publier l'entree de chaque composant remplissait un espace commun de
     * valeurs que personne n'avait demande a partager. Le script lit desormais
     * ses entrees par in(n) / raw.in(n), qui ne passent pas par le registre. */
    // Script mode must run even without a component name.
    if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        /* Idem : la lecture pleine resolution pour raw.in() (§55). */
        MappingEngine::executerCapteur(config.mappingScript, (float)state.last_value,
                                      midi_sender, state.scriptEtats,
                                      ComponentState::MAX_PIPELINES_BROCHE,
                                      (float)filtered_value);
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
    VelostatProcessor::process(config, state, *filter, midi_sender, osc_queue);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::VELOSTAT,
    processWrapper
);
