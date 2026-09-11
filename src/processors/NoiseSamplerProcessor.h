#pragma once

#include <Arduino.h>
#include "../components/ComponentTypes.h"  // Définitions communes
#include "../midi/MidiSender.h"
#include "../midi/MidiMessageType.h"
#include "../osc/OSCQueue.h"
#include "../utils/PinMapper.h"
#include "../utils/AnalogFilter.h"

/**
 * @brief Processeur pour le Noise Sampler (famille SIGNAL)
 *
 * Échantillonne une source de bruit externe sur une entrée ADC.
 * Mode sample-and-hold (latch toutes les rateMs) ou continu (lissé),
 * conversion MIDI/OSC + script de mapping. (La publication automatique au
 * registre a ete retiree : publier se declare dans le script, s("nom").)
 */
class NoiseSamplerProcessor {
public:
    static void process(
        const ComponentConfig& config,
        ComponentState& state,
        AnalogFilter& filter,
        MidiSender* midi_sender,
        OSCQueue& osc_queue
    );
};
