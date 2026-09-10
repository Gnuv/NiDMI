#include "NoiseSamplerProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"  // Définitions communes
#include "../components/signal/NoiseSamplerDef.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../mapping/MappingEngine.h"
#include "../utils/AxisUtils.h"

void NoiseSamplerProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& filter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    // GPIO normal : vérifier qu'il est valide et a un ADC
    if (config.gpio >= 255 || config.gpio > 48) {
        return;
    }
    if (!PinMapper::hasAdc(config.gpio)) {
        return;
    }

    // ===== Récupération de la config spécifique =====
    bool sandh = true;              // mode sample-and-hold par défaut
    uint16_t rateMs = 250;
    uint16_t analog_min = 0;
    uint16_t analog_max = 4095;
    uint8_t intensity = 3;

    if (config.specificConfig.noiseSampler) {
        const Components::NoiseSamplerConfig* c = config.specificConfig.noiseSampler;
        sandh = (strcmp(c->sampleMode, "continuous") != 0);
        rateMs = c->rateMs;
        analog_min = c->inMin;
        analog_max = c->inMax;
        intensity = c->filter_intensity ? c->filter_intensity : 3;
        if (analog_min == 0 && analog_max == 0) {
            analog_min = 0;
            analog_max = 4095;
        }
    }

    // ===== Sample-and-Hold : ne latcher qu'une nouvelle valeur toutes les rateMs =====
    // Entre deux latches, la valeur est maintenue (on ne ré-émet rien).
    if (sandh) {
        if (rateMs < 1) rateMs = 1;
        uint32_t now = millis();
        if (state.last_change_time != 0 && (now - state.last_change_time) < rateMs) {
            return; // hold
        }
        state.last_change_time = now;
    }

    // Lecture analogique directe (bruit externe)
    uint16_t raw_value = analogRead(config.gpio);

    // En continu : lissage. En S&H : latch brut (chaque latch est volontaire).
    uint16_t filtered_raw_value;
    if (sandh) {
        filtered_raw_value = raw_value;
    } else {
        filter.setAlphaFromIntensity(intensity);
        if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
            filtered_raw_value = filter.processMedianAndLowpass(raw_value);
        } else {
            filtered_raw_value = filter.process(raw_value);
        }
    }

    // Mapping fenêtre d'entrée [inMin, inMax] -> [0, 4095]
    uint16_t filtered_value;
    if (analog_max > analog_min) {
        if (filtered_raw_value < analog_min) {
            filtered_value = 0;
        } else if (filtered_raw_value > analog_max) {
            filtered_value = 4095;
        } else {
            filtered_value = (uint16_t)map(filtered_raw_value, analog_min, analog_max, 0, 4095);
        }
    } else {
        filtered_value = filtered_raw_value;
    }

    // ===== NOTE_SWEEP : mélodie aléatoire (réutilise la logique du potentiomètre) =====
    if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
        const uint16_t sweepOffMs = effectiveNoteSweepAutoOffMs(config.rtpNoteSweepAutoOffDelay);
        if (sweepOffMs > 0 &&
            state.last_note != 255 &&
            state.note_on_time > 0) {
            uint32_t elapsed = millis() - state.note_on_time;
            if (elapsed >= sweepOffMs) {
                midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
                state.note_on_time = 0;
            }
        }

        // En continu, hystérésis pour stabiliser ; en S&H, échelle directe.
        uint8_t stable_midi_value;
        if (sandh) {
            stable_midi_value = (uint8_t)(filtered_value >> 5); // 0-4095 -> 0-127
            if (stable_midi_value > 127) stable_midi_value = 127;
        } else {
            if (!state.hysteresis.update(filtered_value)) {
                return;
            }
            stable_midi_value = state.hysteresis.getValue();
        }

        uint8_t noteMin = config.rtpNoteMin;
        uint8_t noteMax = config.rtpNoteMax;
        uint8_t newNote;
        if (stable_midi_value == 0) {
            newNote = 255;
        } else {
            newNote = map(stable_midi_value, 1, 127, noteMin, noteMax);
        }

        if (newNote == state.last_note) {
            return;
        }
        if (state.last_note != 255 && state.note_on_time > 0) {
            midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
        }
        if (newNote != 255) {
            midi_sender->sendNoteOn(config.midi_channel, newNote, config.rtpNoteVelFix);
            state.note_on_time = millis();
        } else {
            state.note_on_time = 0;
        }

        state.last_note = newNote;
        state.last_value = stable_midi_value;
        state.last_time = millis();
        state.last_raw_value_u32 = filtered_value;
        state.last_midi_value_u8 = stable_midi_value;
        state.last_telemetry_ts = state.last_time;

        MidiOutputCoordinator::sendOsc(osc_queue, config, stable_midi_value, filtered_value);

        if (config.name && config.name[0] != '\0') {
            FluxRegistry::update(config.name, (float)stable_midi_value);
        }
        if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        /* NORMALISE 0..1. Une entree de composant ne porte AUCUNE echelle de
         * protocole : la mise a l'echelle MIDI — comme la mise a l'echelle DMX
         * ou OSC — appartient au script (« in() : *(127) : ctl.out(1,7) »).
         * Sans cela le pipeline arrivait deja quantifie en 0..127, c'est-a-dire
         * qu'un choix de protocole etait fige avant le premier verbe. La pleine
         * resolution reste lisible par raw.in(). */
            MappingEngine::executerCapteur(config.mappingScript,
                                          (float)stable_midi_value / 127.0f,
                                          midi_sender, state.scriptEtats,
                                      ComponentState::MAX_PIPELINES_BROCHE,
                                          (float)stable_midi_value);
        }
        return;
    }

    // ===== TRAITEMENT STANDARD (CC, Note, etc.) =====
    uint8_t midi_value;
    if (sandh) {
        // Échelle directe : on veut un évènement frais à chaque latch.
        midi_value = (uint8_t)(filtered_value >> 5); // 0-4095 -> 0-127
        if (midi_value > 127) midi_value = 127;
    } else {
        if (!state.hysteresis.update(filtered_value)) {
            return; // Valeur stable, rien à faire
        }
        midi_value = state.hysteresis.getValue();
    }

    // Appliquer la plage MIDI si configurée
    if (config.midiCcRangeMin != 0 || config.midiCcRangeMax != 127) {
        midi_value = map(midi_value, 0, 127, config.midiCcRangeMin, config.midiCcRangeMax);
    }

    // En continu : dédup pour éviter le flood. En S&H : émission à chaque latch.
    if (!sandh && midi_value == state.last_value) {
        return;
    }

    if (config.midiMode != MidiMode::SCRIPT) {
        MidiOutputCoordinator::sendMidiAndOsc(midi_sender, osc_queue, config, midi_value, filtered_value);
    }

    state.last_value = midi_value;
    state.last_time = millis();
    state.last_raw_value_u32 = filtered_value;
    state.last_midi_value_u8 = midi_value;
    state.last_telemetry_ts = state.last_time;

    // Publication FluxRegistry : routable par d'autres composants via r("nom").
    if (config.name && config.name[0] != '\0') {
        FluxRegistry::update(config.name, (float)midi_value);
    }
    // Mode script : exécuter même sans nom de composant.
    if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        /* NORMALISE 0..1. Une entree de composant ne porte AUCUNE echelle de
         * protocole : la mise a l'echelle MIDI — comme la mise a l'echelle DMX
         * ou OSC — appartient au script (« in() : *(127) : ctl.out(1,7) »).
         * Sans cela le pipeline arrivait deja quantifie en 0..127, c'est-a-dire
         * qu'un choix de protocole etait fige avant le premier verbe. La pleine
         * resolution reste lisible par raw.in(). */
        MappingEngine::executerCapteur(config.mappingScript,
                                      (float)midi_value / 127.0f,
                                      midi_sender, state.scriptEtats,
                                      ComponentState::MAX_PIPELINES_BROCHE,
                                      (float)midi_value);
    }
}

// Wrapper pour normaliser la signature (filter par pointeur au lieu de référence)
static void processWrapper(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter* filter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    if (filter == nullptr) {
        return; // Le Noise Sampler nécessite un filtre analogique
    }
    NoiseSamplerProcessor::process(config, state, *filter, midi_sender, osc_queue);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::NOISE_SAMPLER,
    processWrapper
);
