#include "PotentiometerProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"  // Définitions communes
#include "../components/basic/PotentiometerDef.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../mapping/MappingEngine.h"
#include "../utils/AxisUtils.h"

void PotentiometerProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& filter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    // ===== TRAITEMENT POUR GPIO NORMALES (non-MUX) =====
    // GPIO normal : vérifier qu'il est valide et a un ADC
    if (config.gpio >= 255 || config.gpio > 48) {
        return;
    }
    
    if (!PinMapper::hasAdc(config.gpio)) {
        return;
    }
    
    // Lecture analogique directe
    uint16_t raw_value = analogRead(config.gpio);
    
    // Mettre à jour alpha du filtre selon filter_intensity (1-10)
    uint8_t intensity = 5; // Valeur par défaut
    uint16_t analog_min = 0;
    uint16_t analog_max = 4095;
    
    if (config.specificConfig.potentiometer) {
        intensity = config.specificConfig.potentiometer->filter_intensity;
        if (intensity == 0) intensity = 5; // Valeur par défaut si non configuré
        analog_min = config.specificConfig.potentiometer->potMin;
        analog_max = config.specificConfig.potentiometer->potMax;
        // Si les seuils ne sont pas configurés (0,0), utiliser les valeurs par défaut
        if (analog_min == 0 && analog_max == 0) {
            analog_min = 0;
            analog_max = 4095;
        }
    }
    
    filter.setAlphaFromIntensity(intensity);
    
    // FILTRAGE D'ABORD : filtrer la valeur brute avant normalisation
    // Cela permet au pitchbend d'appliquer le mapping potMin/potMax directement
    uint16_t filtered_raw_value;  // Valeur brute filtrée (0-4095)
    if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
        filtered_raw_value = filter.processMedianAndLowpass(raw_value);
    } else {
        filtered_raw_value = filter.process(raw_value);
    }
    
    // Appliquer le mapping min/max pour définir la plage analogique
    // où la plage MIDI (0-127) va s'étendre (pour Control Change, etc.)
    // Résultat : valeur normalisée dans 0-4095 selon potMin/potMax
    uint16_t mapped_value;  // Valeur normalisée selon potMin/potMax (0-4095) pour CC
    
    // Normaliser la valeur filtrée selon potMin/potMax (pour Control Change, OSC, etc.)
    // - valeurs < potMin → 0 (sera 0 MIDI après hystérésis)
    // - valeurs > potMax → 4095 (sera 127 MIDI après hystérésis)
    // - valeurs entre potMin et potMax → mappées linéairement vers 0-4095
    if (analog_max > analog_min) {
        if (filtered_raw_value < analog_min) {
            // En dessous du seuil min → 0 (normalisé)
            mapped_value = 0;
        } else if (filtered_raw_value > analog_max) {
            // Au dessus du seuil max → maximum (normalisé)
            mapped_value = 4095;
        } else {
            // Entre les seuils → mapper linéairement de [potMin, potMax] vers [0, 4095]
            mapped_value = (uint16_t)map(filtered_raw_value, analog_min, analog_max, 0, 4095);
        }
    } else {
        mapped_value = filtered_raw_value; // Pas de mapping si seuils invalides
    }
    
    // Valeur normalisée filtrée (pour compatibilité avec le code existant)
    uint16_t filtered_value = mapped_value;
    
    // ===== TRAITEMENT SPÉCIAL NOTE_SWEEP (GPIO normales) =====
    if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
        const uint16_t sweepOffMs = effectiveNoteSweepAutoOffMs(config.rtpNoteSweepAutoOffDelay);
        // 1. Auto-off : coupe la note si le délai est écoulé, mais on conserve
        //    state.last_note pour éviter un retrigger tant que la position pointe
        //    toujours sur la même note.
        if (sweepOffMs > 0 &&
            state.last_note != 255 &&
            state.note_on_time > 0) {
            uint32_t elapsed = millis() - state.note_on_time;
            if (elapsed >= sweepOffMs) {
                midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
                state.note_on_time = 0;
            }
        }
        
        // 2. Appliquer l'hystérésis directement sur filtered_value (0-4095)
        // L'hystérésis réduit automatiquement vers 0-127
        if (!state.hysteresis.update(filtered_value)) {
            return; // Valeur stable, rien à faire
        }
        
        // 3. Utiliser la valeur stabilisée par l'hystérésis (déjà 0-127)
        uint8_t stable_midi_value = state.hysteresis.getValue();
        
        // 4. Calculer la nouvelle note
        uint8_t noteMin = config.rtpNoteMin;
        uint8_t noteMax = config.rtpNoteMax;
        uint8_t newNote;
        
        if (stable_midi_value == 0) {
            newNote = 255; // Pas de note (potentiomètre à zéro)
        } else {
            newNote = map(stable_midi_value, 1, 127, noteMin, noteMax);
        }
        
        // 5. Si la note est identique à la précédente, ne rien faire (pas de retrigger)
        if (newNote == state.last_note) {
            return;
        }
        
        // 6. Note différente : couper l'ancienne uniquement si encore active
        if (state.last_note != 255 && state.note_on_time > 0) {
            midi_sender->sendNoteOff(config.midi_channel, state.last_note, 0);
        }
        
        // 7. Jouer la nouvelle note (sauf si 255)
        if (newNote != 255) {
            midi_sender->sendNoteOn(config.midi_channel, newNote, config.rtpNoteVelFix);
            state.note_on_time = millis();
        } else {
            state.note_on_time = 0;
        }
        
        // 8. Mettre à jour l'état
        state.last_note = newNote;
        state.last_value = stable_midi_value;
        state.last_time = millis();
        state.last_raw_value_u32 = filtered_value;
        state.last_midi_value_u8 = stable_midi_value;
        state.last_telemetry_ts = state.last_time;
        
        // 9. OSC si activé (même valeur que MIDI)
        MidiOutputCoordinator::sendOsc(osc_queue, config, stable_midi_value, filtered_value);
        
        return; // Traitement NOTE_SWEEP GPIO normale terminé
    }
    
    // ===== TRAITEMENT STANDARD pour GPIO normales (autres types) =====
    // Appliquer l'hystérésis directement sur filtered_value (0-4095)
    // L'hystérésis réduit automatiquement vers 0-127 (méthode Control-Surface)
    if (!state.hysteresis.update(filtered_value)) {
        return; // Valeur stable, rien à faire
    }
    
    uint8_t midi_value = state.hysteresis.getValue();  // Déjà 0-127
    
    // Appliquer la plage MIDI si configurée (pas plage complète 0-127)
        /* PLAGE MIDI : intention MUSICALE, donc elle n'a pas sa place AVANT le
         * script. La regle, ecrite une fois pour tous les composants :
         *
         *   ce qui releve de la LECTURE du capteur (anti-rebond, filtre,
         *   course utile, hysteresis) reste sur la carte, toujours ;
         *   ce qui releve de l'INTENTION musicale (plage, n° de note ou de CC,
         *   mode du bouton, balayage) appartient au script.
         *
         * C'etait deja le choix fait pour le bouton, dont btnMode est
         * deliberement ignore en mode script. Le potentiometre et le touch
         * etaient les deux seuls a appliquer midiCcRange avant le script : trois
         * regles differentes pour cinq composants, et rien qui le disait. */
    if (config.midiMode != MidiMode::SCRIPT &&
        (config.midiCcRangeMin != 0 || config.midiCcRangeMax != 127)) {
        // Mapper de 0-127 vers la plage configurée
        midi_value = map(midi_value, 0, 127, config.midiCcRangeMin, config.midiCcRangeMax);
    }
    
    // Comparer avec la dernière valeur ENVOYÉE (comme le MUX)
    if (midi_value == state.last_value) {
        return; // Pas de changement, ne pas envoyer
    }
    
    // Envoyer MIDI et OSC via le coordinateur (évite les redondances)
    // Pour le pitchbend : passer filtered_raw_value (brute filtrée) pour mapping direct potMin/potMax → 0-16383
    // Pour les autres : passer filtered_value (normalisée) pour hystérésis déjà appliquée
    uint16_t raw_value_for_handler = (config.msg_type == MidiMessageType::PITCH_BEND) 
                                    ? filtered_raw_value  // Pitchbend : valeur brute filtrée
                                    : filtered_value;     // Autres : valeur normalisée
    if (config.midiMode != MidiMode::SCRIPT) {
        MidiOutputCoordinator::sendMidiAndOsc(midi_sender, osc_queue, config, midi_value, raw_value_for_handler);
    }
    
    // Mettre à jour last_value UNE SEULE FOIS après l'envoi (comme le MUX)
    state.last_value = midi_value;
    state.last_time = millis();
    
    /* PLUS DE PUBLICATION D'OFFICE AU BUS. Le registre est un bus PARTAGE :
     * il ne doit contenir que ce qu'on y a mis expressement — « in() : s("x") ».
     * Y publier l'entree de chaque composant remplissait un espace commun de
     * valeurs que personne n'avait demande a partager. Le script lit desormais
     * ses entrees par in(n) / raw.in(n), qui ne passent pas par le registre. */
    // Script mode must run even without a component name.
    if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        /* `filtered_value` : la lecture apres filtre et course utile, AVANT la
         * quantification en 0..127 — c'est ce que rend raw.in() (§55). */
        /* NORMALISE 0..1. Une entree de composant ne porte AUCUNE echelle de
         * protocole : la mise a l'echelle MIDI — comme la mise a l'echelle DMX
         * ou OSC — appartient au script (« in() : *(127) : ctl.out(1,7) »).
         * Sans cela le pipeline arrivait deja quantifie en 0..127, c'est-a-dire
         * qu'un choix de protocole etait fige avant le premier verbe. La pleine
         * resolution reste lisible par raw.in(). */
        MappingEngine::executerCapteur(config.mappingScript,
                                      (float)filtered_value / 4095.0f,
                                      midi_sender, state.scriptEtats,
                                      ComponentState::MAX_PIPELINES_BROCHE,
                                      (float)filtered_value);
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
        return; // Potentiomètre nécessite un filtre
    }
    PotentiometerProcessor::process(config, state, *filter, midi_sender, osc_queue);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::POTENTIOMETER,
    processWrapper
);
